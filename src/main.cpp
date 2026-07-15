#include <atomic>
#include <windows.h>
#include <d3d11.h>
#include <dxgi.h>
#include <wrl/client.h>
#include <wincodec.h>
#include <cstdio>
#include <cstdarg>
#include <cstdint>
#include <string>
#include <vector>

// ============================================================================
// 共享内存协议：DLL 把最新一帧的原始像素(BGRA, top-down, 紧密排列)写入命名
// file mapping，script 进程可直接映射读取，无需 PNG 落盘/解码中转。
// 该结构体布局必须与 script 侧 (Dx11CaptureShared.h) 保持完全一致。
// ============================================================================
static const wchar_t* kDx11SharedName = L"OnmyojiDx11CaptureShared";
static const uint32_t kDx11SharedMagic = 0x31315844; // 'DX11'
static const uint32_t kDx11SharedVersion = 1;
// 支持的最大分辨率（用于预留共享内存大小），4K 足够覆盖桌面版窗口。
static const uint32_t kDx11SharedMaxW = 3840;
static const uint32_t kDx11SharedMaxH = 2160;

#pragma pack(push, 4)
struct Dx11CaptureShared {
    uint32_t magic;     // kDx11SharedMagic
    uint32_t version;   // kDx11SharedVersion
    uint32_t sequence;  // 每次成功写入自增（读端可用于判断是否有新帧）
    uint32_t status;    // 0 = 成功，非 0 = 无有效数据
    uint32_t width;
    uint32_t height;
    uint32_t channels;  // 固定为 4 (BGRA)
    uint32_t dataSize;  // = width * height * 4
    // 紧随其后是像素数据（BGRA, top-down, 每行 width*4 字节，无 padding）
};
#pragma pack(pop)

#include "MinHook.h"

using Microsoft::WRL::ComPtr;

typedef HRESULT(STDMETHODCALLTYPE* Present_t)(IDXGISwapChain*, UINT, UINT);
static Present_t g_originalPresent = nullptr;
static std::atomic<IDXGISwapChain*> g_swap{ nullptr };
static ID3D11Device* g_device = nullptr;
static ID3D11DeviceContext* g_context = nullptr;
static std::atomic<bool> g_initialized{ false };
static CRITICAL_SECTION g_logCs;
static std::wstring g_logPath;          // empty => 使用临时目录
static bool g_logInited = false;
static HANDLE g_hookThread = nullptr;
static std::atomic<bool> g_hookStopped{false};
static std::atomic<bool> g_cleanupInProgress{false};
static HMODULE g_hModule = nullptr;
static bool g_selfUnloading = false;

// 保护对 D3D11 immediate context / device 的访问。
// ID3D11DeviceContext 的 immediate context 不是线程安全的，游戏渲染线程会持续
// 在该 context 上渲染，因此绝不能在其它线程直接使用它。
static CRITICAL_SECTION g_deviceCs;
static bool g_deviceCsInited = false;

// 截图请求：CaptureFrame 在远程线程被调用，但真正的 D3D 操作必须放到渲染线程
// (hkPresent) 中执行，否则跨线程使用 immediate context 会损坏 GPU 命令流，
// 概率性导致显卡驱动崩溃/TDR 失败，从而出现全屏花屏、需要重启的严重问题。
static std::atomic<bool> g_captureRequested{ false };
static std::wstring g_capturePath;      // 受 g_deviceCs 保护
static std::atomic<bool> g_capturePersist{ true }; // 是否额外把截图持久化为 PNG
static std::atomic<int> g_captureResult{ 0 };
static HANDLE g_captureDoneEvent = nullptr; // 手动重置事件

// 跨进程截图请求事件：script 进程可通过 OpenEvent/SetEvent 直接触发截图，
// 无需每次都启动 remote_capture_call.exe 进程。DLL 在 hkPresent 中以
// 非阻塞方式检查此事件，触发后执行截图并写入共享内存。
static const wchar_t* kCaptureRequestEventName = L"OnmyojiDx11CaptureRequest";
static HANDLE g_crossProcessRequestEvent = nullptr; // auto-reset event

// 共享内存句柄（在渲染线程首次截图时创建，受 g_deviceCs 保护）
static HANDLE g_sharedMapping = nullptr;
static void* g_sharedView = nullptr;


// 线程安全的日志系统初始化
static void InitLogSystem() {
    static volatile long s_initialized = 0;
    if (InterlockedCompareExchange(&s_initialized, 1, 0) == 0) {
        InitializeCriticalSection(&g_logCs);
        g_logInited = true;
    }
}

// 获取有效日志路径
static std::wstring GetEffectiveLogPath() {
    EnterCriticalSection(&g_logCs);
    std::wstring p = g_logPath; // 获取副本
    LeaveCriticalSection(&g_logCs);

    if (!p.empty()) return p;

    wchar_t buf[MAX_PATH];
    DWORD n = GetTempPathW(MAX_PATH, buf);
    if (n == 0 || n > MAX_PATH) return L"C:\\dx11_hook_log.txt";

    std::wstring s = buf;
    if (s.back() != L'\\' && s.back() != L'/') s.push_back(L'\\');
    s += L"dx11_hook_log.txt";
    return s;
}

// 日志写入函数
static void Log(const wchar_t* fmt, ...) {
    if (!g_logInited) InitLogSystem();

    EnterCriticalSection(&g_logCs);
    std::wstring path = GetEffectiveLogPath();

    FILE* f = nullptr;
    _wfopen_s(&f, path.c_str(), L"a+, ccs=UTF-8");
    if (!f) {
        LeaveCriticalSection(&g_logCs);
        return;
    }

    // 写入时间戳
    fwprintf(f, L"%llu: ", GetTickCount64());

    // 格式化并写入日志内容
    va_list ap;
    va_start(ap, fmt);
    vfwprintf(f, fmt, ap);
    va_end(ap);

    fwprintf(f, L"\n");
    fflush(f); // 确保数据刷新到文件
    fclose(f);

    LeaveCriticalSection(&g_logCs);
}

// 修改StopHookAndCleanup函数，在最后添加自卸载
extern "C" __declspec(dllexport) DWORD StopHookAndCleanup() {
    if (g_cleanupInProgress.exchange(true)) {
        Log(L"清理已在进行中，跳过重复调用");
        return 0;
    }

    Log(L"StopHookAndCleanup 被调用 - 开始清理过程");
    g_hookStopped.store(true);

    // 禁用所有MinHook钩子
    MH_STATUS mhStatus = MH_DisableHook(MH_ALL_HOOKS);
    if (mhStatus != MH_OK) {
        Log(L"MH_DisableHook 失败，状态: %d", mhStatus);
    }

    Sleep(100);

    // 唤醒任何正在等待截图完成的远程线程，避免其一直阻塞。
    g_captureRequested.store(false);
    if (g_captureDoneEvent) {
        g_captureResult.store(0);
        SetEvent(g_captureDoneEvent);
    }

    // 关闭跨进程截图请求事件
    if (g_crossProcessRequestEvent) {
        CloseHandle(g_crossProcessRequestEvent);
        g_crossProcessRequestEvent = nullptr;
        Log(L"跨进程截图请求事件已关闭");
    }

    // 安全清理D3D资源
    IDXGISwapChain* oldSwap = g_swap.exchange(nullptr);
    if (oldSwap) {
        Log(L"g_swap 重置为 null (原为 %p)", oldSwap);
    }

    g_initialized.store(false);

    // 不在此(远程)线程上对 immediate context 提交任何命令(如 ClearState/Flush)，
    // 因为那会与游戏渲染线程竞争同一个 context。这里只做引用计数释放
    // (Release 内部是线程安全的原子操作)，并用临界区确保不会与渲染线程中
    // 正在进行的截图操作重叠。
    if (g_deviceCsInited) EnterCriticalSection(&g_deviceCs);
    if (g_context) {
        g_context->Release();
        g_context = nullptr;
        Log(L"g_context 已释放");
    }
    if (g_device) {
        g_device->Release();
        g_device = nullptr;
        Log(L"g_device 已释放");
    }
    if (g_sharedView) {
        UnmapViewOfFile(g_sharedView);
        g_sharedView = nullptr;
    }
    if (g_sharedMapping) {
        CloseHandle(g_sharedMapping);
        g_sharedMapping = nullptr;
        Log(L"共享内存已释放");
    }
    if (g_deviceCsInited) LeaveCriticalSection(&g_deviceCs);

    // 3. 卸载MinHook
    Log(L"正在取消初始化 MinHook");
    mhStatus = MH_Uninitialize();
    if (mhStatus != MH_OK) {
        Log(L"MH_Uninitialize 失败，状态: %d", mhStatus);
    } else {
        Log(L"MinHook 取消初始化成功");
    }

    // 4. 清理日志系统
    Log(L"正在清理日志系统");
    Log(L"StopHookAndCleanup 完成成功");

    if (g_captureDoneEvent) {
        CloseHandle(g_captureDoneEvent);
        g_captureDoneEvent = nullptr;
    }

    if (g_deviceCsInited) {
        DeleteCriticalSection(&g_deviceCs);
        g_deviceCsInited = false;
    }

    if (g_logInited) {
        DeleteCriticalSection(&g_logCs);
        g_logInited = false;
        Log(L"临界区已删除");
    }

    // 5. 强制自卸载
    Log(L"准备强制卸载 DLL");
    Sleep(100);

    if (g_hModule) {
        g_selfUnloading = true; // 标记为自卸载
        Log(L"调用 FreeLibraryAndExitThread，hModule=%p", g_hModule);

        // 强制卸载
        FreeLibraryAndExitThread(g_hModule, 1);
    } else {
        Log(L"g_hModule 为 null，无法卸载");
    }

    return 1;
}

// 检查是否已停止
extern "C" __declspec(dllexport) BOOL IsHookStopped() {
    return g_hookStopped.load();
}

// 添加状态查询接口
extern "C" __declspec(dllexport) bool IsHookInitialized() {
    return g_initialized.load() && g_swap.load() != nullptr && g_device != nullptr && g_context != nullptr;
}

// 导出接口：设置日志路径（线程安全）
extern "C" __declspec(dllexport) void SetLogPath(const wchar_t* path) {
    InitLogSystem();
    EnterCriticalSection(&g_logCs);

    if (path && wcslen(path) > 0) {
        g_logPath = path;
    } else {
        g_logPath.clear();
    }

    // 获取 g_logPath 的副本，用于日志记录
    std::wstring logPathToLog = g_logPath;
    LeaveCriticalSection(&g_logCs);

    // 使用副本记录日志，避免竞争条件
    Log(L"日志路径设置为: %s", logPathToLog.empty() ? L"<TEMP>" : logPathToLog.c_str());
}

// 确保命名共享内存已创建（仅在渲染线程、持有 g_deviceCs 时调用）。
static bool EnsureSharedMemory() {
    if (g_sharedView) return true;

    const size_t total = sizeof(Dx11CaptureShared) +
        (size_t)kDx11SharedMaxW * kDx11SharedMaxH * 4;

    g_sharedMapping = CreateFileMappingW(
        INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE,
        (DWORD)(total >> 32), (DWORD)(total & 0xFFFFFFFF), kDx11SharedName);
    if (!g_sharedMapping) {
        Log(L"CreateFileMapping 失败: %lu", GetLastError());
        return false;
    }

    g_sharedView = MapViewOfFile(g_sharedMapping, FILE_MAP_WRITE, 0, 0, total);
    if (!g_sharedView) {
        Log(L"MapViewOfFile 失败: %lu", GetLastError());
        CloseHandle(g_sharedMapping);
        g_sharedMapping = nullptr;
        return false;
    }

    // 初始化头部：暂无有效数据
    Dx11CaptureShared* hdr = (Dx11CaptureShared*)g_sharedView;
    hdr->magic = kDx11SharedMagic;
    hdr->version = kDx11SharedVersion;
    hdr->sequence = 0;
    hdr->status = 1;
    hdr->width = hdr->height = hdr->channels = hdr->dataSize = 0;

    Log(L"共享内存已创建: %s (容量=%llu 字节)", kDx11SharedName, (unsigned long long)total);
    return true;
}

// 把后备缓冲区的原始 BGRA 像素写入共享内存，供 script 进程直接读取。
// 仅在渲染线程调用（复用 immediate context 是安全的）。
static bool SaveTextureToSharedMemory(ID3D11Texture2D* src) {
    if (!src || !g_device || !g_context) return false;

    D3D11_TEXTURE2D_DESC desc;
    src->GetDesc(&desc);
    if (desc.Width == 0 || desc.Height == 0) return false;
    if (desc.Width > kDx11SharedMaxW || desc.Height > kDx11SharedMaxH) {
        Log(L"共享内存: 分辨率 %ux%u 超过上限 %ux%u", desc.Width, desc.Height,
            kDx11SharedMaxW, kDx11SharedMaxH);
        return false;
    }

    if (!EnsureSharedMemory()) return false;

    D3D11_TEXTURE2D_DESC staging = desc;
    staging.Usage = D3D11_USAGE_STAGING;
    staging.BindFlags = 0;
    staging.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    staging.MiscFlags = 0;

    ComPtr<ID3D11Texture2D> copy;
    HRESULT hr = g_device->CreateTexture2D(&staging, nullptr, &copy);
    if (FAILED(hr)) {
        Log(L"共享内存: CreateTexture2D staging 失败: 0x%08x", hr);
        return false;
    }

    g_context->CopyResource(copy.Get(), src);

    D3D11_MAPPED_SUBRESOURCE mapped;
    hr = g_context->Map(copy.Get(), 0, D3D11_MAP_READ, 0, &mapped);
    if (FAILED(hr)) {
        Log(L"共享内存: Map 失败: 0x%08x", hr);
        return false;
    }

    Dx11CaptureShared* hdr = (Dx11CaptureShared*)g_sharedView;
    BYTE* dst = (BYTE*)g_sharedView + sizeof(Dx11CaptureShared);
    const UINT rowBytes = desc.Width * 4;

    // 逐行拷贝，去除 RowPitch 的行内 padding，得到紧密排列的 BGRA 数据。
    for (UINT y = 0; y < desc.Height; y++) {
        memcpy(dst + (size_t)y * rowBytes,
               (BYTE*)mapped.pData + (size_t)y * mapped.RowPitch, rowBytes);
    }

    g_context->Unmap(copy.Get(), 0);

    // 先写数据再更新头部，最后自增 sequence 作为“可用”标记。
    hdr->status = 0;
    hdr->width = desc.Width;
    hdr->height = desc.Height;
    hdr->channels = 4;
    hdr->dataSize = rowBytes * desc.Height;
    hdr->sequence += 1;

    Log(L"已写入共享内存 (宽x高=%u x %u, seq=%u)", desc.Width, desc.Height, hdr->sequence);
    return true;
}

static bool SaveTextureToPngWithManualSwap(ID3D11Texture2D* src, const std::wstring& path) {
    if (!src || !g_device || !g_context)
        return false;

    HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    bool shouldUninit = SUCCEEDED(hr) || hr == S_FALSE;

    D3D11_TEXTURE2D_DESC desc;
    src->GetDesc(&desc);

    // 创建 staging 纹理
    D3D11_TEXTURE2D_DESC staging = desc;
    staging.Usage = D3D11_USAGE_STAGING;
    staging.BindFlags = 0;
    staging.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    staging.MiscFlags = 0;

    ComPtr<ID3D11Texture2D> copy;
    hr = g_device->CreateTexture2D(&staging, nullptr, &copy);
    if (FAILED(hr)) {
        Log(L"CreateTexture2D staging 失败: 0x%08x", hr);
        if (shouldUninit) CoUninitialize();
        return false;
    }

    // 关键修复：在渲染间隙进行复制，避免干扰正常渲染
    g_context->CopyResource(copy.Get(), src);

    D3D11_MAPPED_SUBRESOURCE mapped;
    hr = g_context->Map(copy.Get(), 0, D3D11_MAP_READ, 0, &mapped);
    if (FAILED(hr)) {
        Log(L"Map 失败: 0x%08x", hr);
        if (shouldUninit) CoUninitialize();
        return false;
    }

    // 使用栈上数组替代动态分配，提高性能
    const size_t bufferSize = mapped.RowPitch * desc.Height;
    std::vector<BYTE> tempBuffer(bufferSize);

    // 手动交换R和B通道
    for (UINT y = 0; y < desc.Height; y++) {
        BYTE* srcRow = (BYTE*)mapped.pData + y * mapped.RowPitch;
        BYTE* dstRow = tempBuffer.data() + y * mapped.RowPitch;

        for (UINT x = 0; x < desc.Width; x++) {
            BYTE* srcPixel = srcRow + x * 4; // BGRA
            BYTE* dstPixel = dstRow + x * 4; // RGBA

            // 交换R和B通道
            dstPixel[0] = srcPixel[2]; // R
            dstPixel[1] = srcPixel[1]; // G
            dstPixel[2] = srcPixel[0]; // B
            dstPixel[3] = srcPixel[3]; // A
        }
    }

    // 初始化 WIC
    ComPtr<IWICImagingFactory> factory;
    hr = CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory));
    if (FAILED(hr)) {
        Log(L"CoCreateInstance WIC 失败: 0x%08x", hr);
        g_context->Unmap(copy.Get(), 0);
        if (shouldUninit) CoUninitialize();
        return false;
    }

    ComPtr<IWICStream> stream;
    hr = factory->CreateStream(&stream);
    if (FAILED(hr)) {
        Log(L"WIC CreateStream 失败: 0x%08x", hr);
        g_context->Unmap(copy.Get(), 0);
        if (shouldUninit) CoUninitialize();
        return false;
    }

    hr = stream->InitializeFromFilename(path.c_str(), GENERIC_WRITE);
    if (FAILED(hr)) {
        Log(L"Stream InitializeFromFilename 失败: 0x%08x", hr);
        g_context->Unmap(copy.Get(), 0);
        if (shouldUninit) CoUninitialize();
        return false;
    }

    ComPtr<IWICBitmapEncoder> encoder;
    hr = factory->CreateEncoder(GUID_ContainerFormatPng, nullptr, &encoder);
    if (FAILED(hr)) {
        Log(L"CreateEncoder 失败: 0x%08x", hr);
        g_context->Unmap(copy.Get(), 0);
        if (shouldUninit) CoUninitialize();
        return false;
    }

    hr = encoder->Initialize(stream.Get(), WICBitmapEncoderNoCache);
    if (FAILED(hr)) {
        Log(L"Encoder Initialize 失败: 0x%08x", hr);
        g_context->Unmap(copy.Get(), 0);
        if (shouldUninit) CoUninitialize();
        return false;
    }

    ComPtr<IWICBitmapFrameEncode> frame;
    ComPtr<IPropertyBag2> props;
    hr = encoder->CreateNewFrame(&frame, &props);
    if (FAILED(hr)) {
        Log(L"CreateNewFrame 失败: 0x%08x", hr);
        g_context->Unmap(copy.Get(), 0);
        if (shouldUninit) CoUninitialize();
        return false;
    }

    hr = frame->Initialize(props.Get());
    if (FAILED(hr)) {
        Log(L"Frame Initialize 失败: 0x%08x", hr);
        g_context->Unmap(copy.Get(), 0);
        if (shouldUninit) CoUninitialize();
        return false;
    }

    hr = frame->SetSize(desc.Width, desc.Height);
    if (FAILED(hr)) {
        Log(L"SetSize 失败: 0x%08x", hr);
        g_context->Unmap(copy.Get(), 0);
        if (shouldUninit) CoUninitialize();
        return false;
    }

    // 使用RGBA格式
    WICPixelFormatGUID pf = GUID_WICPixelFormat32bppRGBA;
    hr = frame->SetPixelFormat(&pf);
    if (FAILED(hr)) {
        Log(L"SetPixelFormat 失败: 0x%08x", hr);
        g_context->Unmap(copy.Get(), 0);
        if (shouldUninit) CoUninitialize();
        return false;
    }

    // 写入交换后的数据
    hr = frame->WritePixels(desc.Height, mapped.RowPitch, mapped.RowPitch * desc.Height, tempBuffer.data());
    if (FAILED(hr)) {
        Log(L"WritePixels 失败: 0x%08x", hr);
        g_context->Unmap(copy.Get(), 0);
        if (shouldUninit) CoUninitialize();
        return false;
    }

    frame->Commit();
    encoder->Commit();
    g_context->Unmap(copy.Get(), 0);

    if (shouldUninit) CoUninitialize();
    Log(L"后备缓冲区格式 = %u, 手动交换 BGR->RGB", desc.Format);
    Log(L"已保存 PNG: %s (宽x高=%u x %u)", path.c_str(), desc.Width, desc.Height);
    return true;
}

// 改进的CaptureFrame函数 - 整合了两个版本的功能
extern "C" __declspec(dllexport) DWORD CaptureFrame(const wchar_t* savePath) {
    if (!savePath) {
        Log(L"CaptureFrame 被调用，路径为 null");
        return 0;
    }

    Log(L"CaptureFrame 被调用，路径=%s", savePath);

    // 等待初始化完成（减少等待时间）
    const int maxWaitMs = 1000; // 从2000ms减少到1000ms
    const int waitInterval = 10;
    int waited = 0;

    while (!IsHookInitialized() && waited < maxWaitMs / waitInterval) {
        Sleep(waitInterval);
        waited++;
    }

    if (!IsHookInitialized()) {
        Log(L"钩子未在 %d 毫秒后初始化", waited * waitInterval);
        return 0;
    }

    if (!g_captureDoneEvent) {
        Log(L"截图事件未创建，无法请求截图");
        return 0;
    }

    // 持久化开关：script 侧若不需要 PNG 落盘，会把路径参数传为空或哨兵
    // "__NO_FILE__"。此时只写共享内存，不写文件。
    std::wstring incoming = savePath;
    bool persist = !(incoming.empty() || incoming == L"__NO_FILE__");

    // 关键修复：不在本(远程)线程直接使用 D3D immediate context。
    // 将请求投递给渲染线程 (hkPresent) 执行，然后在此等待结果。
    // 这样所有对 immediate context 的访问都发生在同一个(渲染)线程上，
    // 避免与游戏渲染竞争导致 GPU 命令流损坏、驱动崩溃与全屏花屏。
    EnterCriticalSection(&g_deviceCs);
    g_capturePath = persist ? incoming : std::wstring();
    LeaveCriticalSection(&g_deviceCs);
    g_capturePersist.store(persist);

    ResetEvent(g_captureDoneEvent);
    g_captureResult.store(0);
    g_captureRequested.store(true);

    // 等待渲染线程完成截图。Present 通常每秒调用数十次，2 秒超时足够宽松；
    // 若游戏长时间不出帧则超时返回失败，绝不阻塞卸载。
    DWORD wr = WaitForSingleObject(g_captureDoneEvent, 2000);
    if (wr != WAIT_OBJECT_0) {
        g_captureRequested.store(false); // 撤销未被处理的请求
        Log(L"CaptureFrame 等待渲染线程超时 (wr=%lu)", wr);
        return 0;
    }

    int r = g_captureResult.load();
    Log(L"CaptureFrame 结果=%d", r);
    return r;
}

// Present hook implementation
HRESULT STDMETHODCALLTYPE hkPresent(IDXGISwapChain* pSwapChain, UINT SyncInterval, UINT Flags) {
    // 如果已经停止，直接调用原始函数
    if (g_hookStopped.load()) {
        return g_originalPresent(pSwapChain, SyncInterval, Flags);
    }

    // 关键修复：只在必要时更新交换链指针
    static IDXGISwapChain* lastKnownSwapChain = nullptr;
    IDXGISwapChain* currentSwap = g_swap.load();

    if (currentSwap != pSwapChain) {
        // 使用compare_exchange确保原子更新
        if (g_swap.compare_exchange_strong(currentSwap, pSwapChain)) {
            if (lastKnownSwapChain != pSwapChain) {
                Log(L"交换链更新: %p -> %p", lastKnownSwapChain, pSwapChain);
                lastKnownSwapChain = pSwapChain;
            }
        }
    }

    // 只在第一次调用或设备丢失时初始化
    if (!g_initialized.load()) {
        // 使用原子操作确保只初始化一次
        static std::atomic<bool> initializing{false};
        if (!initializing.exchange(true)) {
            Log(L"正在初始化钩子...");

            HRESULT hr = pSwapChain->GetDevice(__uuidof(ID3D11Device), (void**)&g_device);
            if (SUCCEEDED(hr) && g_device) {
                g_device->GetImmediateContext(&g_context);
                if (g_context) {
                    g_initialized.store(true);
                    Log(L"钩子初始化成功 - 设备=%p, 上下文=%p", g_device, g_context);
                } else {
                    Log(L"GetImmediateContext 失败");
                    g_device->Release();
                    g_device = nullptr;
                }
            } else {
                Log(L"在 hkPresent 中 GetDevice 失败: 0x%08x", hr);
            }
            initializing.store(false);
        }
    }

    // 检查跨进程截图请求事件（非阻塞）：script 进程通过 SetEvent 发出截图请求，
    // 此处仅写入共享内存（不做 PNG 落盘），适用于高频截图场景。
    if (g_initialized.load() && g_crossProcessRequestEvent &&
        WaitForSingleObject(g_crossProcessRequestEvent, 0) == WAIT_OBJECT_0) {
        // 事件已触发（auto-reset 会自动重置），执行截图写入共享内存
        if (pSwapChain == g_swap.load()) {
            EnterCriticalSection(&g_deviceCs);
            if (g_device && g_context) {
                ComPtr<ID3D11Texture2D> back;
                HRESULT hr = pSwapChain->GetBuffer(0, __uuidof(ID3D11Texture2D), (void**)&back);
                if (SUCCEEDED(hr) && back) {
                    SaveTextureToSharedMemory(back.Get());
                }
            }
            LeaveCriticalSection(&g_deviceCs);
        }
    }

    // 在渲染线程上处理待执行的截图请求（来自 CaptureFrame 远程调用）。
    // 此时后备缓冲区已渲染完成、尚未 Present，且所有 D3D 操作都在拥有 context
    // 的渲染线程上进行，从根本上避免了跨线程使用 immediate context 造成的
    // GPU 命令流损坏(花屏)。
    if (g_initialized.load() && g_captureRequested.load()) {
        std::wstring path;
        EnterCriticalSection(&g_deviceCs);
        path = g_capturePath;
        LeaveCriticalSection(&g_deviceCs);
        bool persist = g_capturePersist.load();

        int result = 0;
        if (pSwapChain == g_swap.load()) {
            EnterCriticalSection(&g_deviceCs);
            if (g_device && g_context) {
                ComPtr<ID3D11Texture2D> back;
                HRESULT hr = pSwapChain->GetBuffer(0, __uuidof(ID3D11Texture2D), (void**)&back);
                if (SUCCEEDED(hr) && back) {
                    // 主路径：把原始像素写入共享内存供 script 直接读取。
                    bool sharedOk = SaveTextureToSharedMemory(back.Get());
                    // 可选：按开关额外持久化为 PNG。
                    if (persist && !path.empty()) {
                        if (!SaveTextureToPngWithManualSwap(back.Get(), path)) {
                            Log(L"PNG 持久化失败: %s", path.c_str());
                        }
                    }
                    result = sharedOk ? 1 : 0;
                } else {
                    Log(L"GetBuffer 失败: 0x%08x", hr);
                }
            }
            LeaveCriticalSection(&g_deviceCs);
        }

        g_captureResult.store(result);
        g_captureRequested.store(false);
        if (g_captureDoneEvent) SetEvent(g_captureDoneEvent);
    }

    // 调用原始函数
    return g_originalPresent(pSwapChain, SyncInterval, Flags);
}

static bool HookPresent() {
    DXGI_SWAP_CHAIN_DESC sd = {};
    sd.BufferCount = 1;
    sd.BufferDesc.Width = 16;
    sd.BufferDesc.Height = 16;
    sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.OutputWindow = GetForegroundWindow();
    sd.SampleDesc.Count = 1;
    sd.Windowed = TRUE;

    ID3D11Device* device = nullptr;
    ID3D11DeviceContext* context = nullptr;
    IDXGISwapChain* swap = nullptr;
    HRESULT hr = D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0,
        nullptr, 0, D3D11_SDK_VERSION, &sd, &swap, &device, nullptr, &context);
    if (FAILED(hr) || !swap) {
        Log(L"CreateDeviceAndSwapChain 失败: 0x%08x", hr);
        if (device) device->Release();
        if (context) context->Release();
        return false;
    }

    void** vtbl = *(void***)swap;
    void* pPresent = vtbl[8]; // Present

    if (MH_Initialize() != MH_OK) {
        Log(L"MinHook MH_Initialize 失败");
        swap->Release(); device->Release(); context->Release();
        return false;
    }

    // Create hook using void* casts to satisfy MinHook prototype
    if (MH_CreateHook(pPresent, reinterpret_cast<void*>(hkPresent), reinterpret_cast<void**>(&g_originalPresent)) != MH_OK) {
        Log(L"MH_CreateHook 失败");
        MH_Uninitialize();
        swap->Release(); device->Release(); context->Release();
        return false;
    }

    if (MH_EnableHook(pPresent) != MH_OK) {
        Log(L"MH_EnableHook 失败");
        MH_RemoveHook(pPresent);
        MH_Uninitialize();
        swap->Release(); device->Release(); context->Release();
        return false;
    }

    Log(L"Present 钩子已安装");
    swap->Release(); device->Release(); context->Release();
    return true;
}

DWORD WINAPI InitThread(LPVOID) {
    g_hookThread = GetCurrentThread(); // 保存当前线程句柄

    // g_logCs 已在 DllMain 中经 InitLogSystem 初始化，切勿重复 InitializeCriticalSection。
    Log(L"DLL 已附加，启动钩子线程");

    // 检查是否已经被要求停止
    if (g_hookStopped.load()) {
        Log(L"钩子已标记为停止，跳过初始化");
        return 0;
    }

    HookPresent();
    return 0;
}

// 修改DllMain保存模块句柄
BOOL WINAPI DllMain(HMODULE hModule, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        g_hModule = hModule;
        DisableThreadLibraryCalls(hModule);

        g_hookStopped.store(false);
        g_cleanupInProgress.store(false);
        g_selfUnloading = false;

        InitLogSystem();

        // 初始化保护 D3D context 的临界区与截图完成事件（手动重置）。
        if (!g_deviceCsInited) {
            InitializeCriticalSection(&g_deviceCs);
            g_deviceCsInited = true;
        }
        if (!g_captureDoneEvent) {
            g_captureDoneEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        }
        // 创建跨进程截图请求事件（auto-reset），允许 script 进程直接 SetEvent
        // 而无需启动 remote_capture_call.exe 进程。
        if (!g_crossProcessRequestEvent) {
            g_crossProcessRequestEvent = CreateEventW(nullptr, FALSE, FALSE, kCaptureRequestEventName);
            if (g_crossProcessRequestEvent) {
                Log(L"跨进程截图请求事件已创建: %s", kCaptureRequestEventName);
            }
        }
        g_captureRequested.store(false);

        CreateThread(nullptr, 0, InitThread, nullptr, 0, nullptr);
    } else if (reason == DLL_PROCESS_DETACH) {
        Log(L"DLL 正在分离 - selfUnloading=%d", g_selfUnloading);
        if (!g_selfUnloading && !g_hookStopped.load()) {
            Log(L"在 DLL 分离时紧急清理");
            MH_DisableHook(MH_ALL_HOOKS);
        }
        Log(L"DLL 分离成功");
    }
    return TRUE;
}