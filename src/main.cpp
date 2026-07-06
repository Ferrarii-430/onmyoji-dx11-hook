#include <atomic>
#include <windows.h>
#include <d3d11.h>
#include <dxgi.h>
#include <wrl/client.h>
#include <wincodec.h>
#include <cstdio>
#include <cstdarg>
#include <string>
#include <vector>

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

    // 安全清理D3D资源
    IDXGISwapChain* oldSwap = g_swap.exchange(nullptr);
    if (oldSwap) {
        Log(L"g_swap 重置为 null (原为 %p)", oldSwap);
    }

    g_initialized.store(false);

    if (g_context) {
        try {
            g_context->ClearState();
            g_context->Flush();
            Log(L"上下文状态已清除并刷新");
        } catch (...) {
            Log(L"在上下文 ClearState/Flush 中发生异常");
        }
        g_context->Release();
        g_context = nullptr;
        Log(L"g_context 已释放");
    }

    if (g_device) {
        g_device->Release();
        g_device = nullptr;
        Log(L"g_device 已释放");
    }

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

    // 单次尝试，避免重试造成的性能问题
    IDXGISwapChain* sc = g_swap.load();
    if (!sc) {
        Log(L"无可用交换链 (g_swap==null)");
        return 0;
    }

    ComPtr<ID3D11Texture2D> back;
    HRESULT hr = sc->GetBuffer(0, __uuidof(ID3D11Texture2D), (void**)&back);

    if (SUCCEEDED(hr) && back) {
        bool ok = SaveTextureToPngWithManualSwap(back.Get(), savePath);
        Log(L"CaptureFrame 结果=%d", ok ? 1 : 0);
        return ok ? 1 : 0;
    } else {
        Log(L"GetBuffer 失败: 0x%08x", hr);
        return 0;
    }
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

    InitializeCriticalSection(&g_logCs);
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