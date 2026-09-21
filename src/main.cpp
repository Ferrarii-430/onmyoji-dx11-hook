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

// ============================================================================
// 坐标点击（后台输入注入）共享内存协议。
// DLL 已注入游戏进程内部，可直接拿到 swap chain 的输出窗口(HWND)，向其
// PostMessage(WM_MOUSEMOVE/WM_LBUTTONDOWN/WM_LBUTTONUP)。相比外部进程的
// SendInput/PostMessage，本方式不依赖前台焦点、坐标为客户区坐标、且与 Unity
// 输入主线程同进程投递，适合 Unity 游戏后台运行时的坐标点击。
// 触发方式：
//   1) 快速路径：script 进程把 (x,y) 写入 OnmyojiDx11ClickShared 共享内存，
//      再 SetEvent(OnmyojiDx11ClickRequest)。DLL 点击工作线程被唤醒执行。
//   2) 回退路径：injector -click 经 CreateRemoteThread 调用导出 InjectClick，
//      内部同样写共享内存 + SetEvent，由同一工作线程执行，保证时序一致。
// 该结构体布局必须与 script 侧 (Dx11CaptureShared.h::Dx11ClickCommand) 一致。
// ============================================================================
static const wchar_t* kClickSharedName       = L"OnmyojiDx11ClickShared";
static const wchar_t* kClickRequestEventName = L"OnmyojiDx11ClickRequest";
static const uint32_t kClickMagic   = 0x314B4C43; // 'CLK1'
static const uint32_t kClickVersion = 1;

#pragma pack(push, 4)
struct Dx11ClickCommand {
    uint32_t magic;     // kClickMagic
    uint32_t version;   // kClickVersion
    uint32_t sequence;  // 写端每次请求自增
    uint32_t doneSeq;   // DLL 执行投递后置为本次 sequence
    int32_t  x;         // 截图像素坐标（与后备缓冲区一致）
    int32_t  y;
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

// === 安全卸载支撑 ===
// 正在 hkPresent 内执行的线程数（覆盖从进入钩子到调用原始 Present 返回的
// 全程）。StopHookAndCleanup 禁用钩子后等待其归零，确保渲染线程已离开
// DLL 代码（含 MinHook trampoline）再执行卸载。
static std::atomic<int> g_inHookCount{ 0 };
// 正在导出 API（CaptureFrame/InjectClick/SetLogPath）内执行的外部线程数。
// 卸载前同样等待归零，避免远程调用线程仍在执行 DLL 代码时 unmap。
static std::atomic<int> g_apiCallCount{ 0 };
// 日志停机标志：置位后 Log() 直接返回。修复原实现“DeleteCriticalSection
// 之后仍调用 Log（含 DLL_PROCESS_DETACH 中的调用）”的未定义行为。
static std::atomic<bool> g_logShutdown{ false };

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

// 跨进程“新帧就绪”事件：SaveTextureToSharedMemory 写完一帧后 SetEvent，
// script 进程 WaitForSingleObject 等待此事件即可拿到新帧，无需以 2ms 间隔
// 轮询共享内存序号（每次轮询读 4 字节却要付一次完整的系统调用代价）。
// auto-reset：script 是唯一等待方，每请求一次消费一次信号；
// 读端拿到信号后仍按 sequence != prevSeq 二次确认，吃掉旧信号也不会误判。
static const wchar_t* kCaptureReadyEventName = L"OnmyojiDx11CaptureFrameReady";
static HANDLE g_crossProcessReadyEvent = nullptr; // auto-reset event

// 共享内存句柄（在渲染线程首次截图时创建，受 g_deviceCs 保护）
static HANDLE g_sharedMapping = nullptr;
static void* g_sharedView = nullptr;

// === 坐标点击状态 ===
// 输出窗口及其后备缓冲区尺寸，在 hkPresent 中由 swap chain desc 更新。
// 点击工作线程读取 g_outputHwnd 用于 PostMessage。
static std::atomic<HWND> g_outputHwnd{ nullptr };
static std::atomic<uint32_t> g_backBufferW{ 0 };
static std::atomic<uint32_t> g_backBufferH{ 0 };

static HANDLE g_clickRequestEvent = nullptr;   // auto-reset，跨进程
static HANDLE g_clickSharedMapping = nullptr;
static void*  g_clickSharedView = nullptr;
static HANDLE g_clickThread = nullptr;
static std::atomic<bool> g_clickThreadStop{ false };
static CRITICAL_SECTION g_clickCs;             // 保护 g_clickSharedView 读写
static bool g_clickCsInited = false;


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
    if (g_logShutdown.load()) return; // 日志已停机：不再进入（可能已删除的）临界区
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

// 等待原子计数归零（1ms 轮询，带超时）。超时返回 false：调用方应放弃
// 卸载而不是冒险 unmap——保持已禁用状态、模块常驻不释放，宁可泄漏不崩溃。
static bool WaitForDrain(std::atomic<int>& count, DWORD timeoutMs) {
    const ULONGLONG deadline = GetTickCount64() + timeoutMs;
    while (count.load() != 0) {
        if (GetTickCount64() >= deadline) return false;
        Sleep(1);
    }
    return true;
}

// 导出 API 在途计数守卫：StopHookAndCleanup 删除临界区/卸载模块前等待
// g_apiCallCount 归零，避免远程调用线程仍在执行 DLL 代码时 unmap。
struct ApiCallGuard {
    ApiCallGuard() { g_apiCallCount.fetch_add(1); }
    ~ApiCallGuard() { g_apiCallCount.fetch_sub(1); }
};

// 修改StopHookAndCleanup函数，在最后添加自卸载
extern "C" __declspec(dllexport) DWORD StopHookAndCleanup() {
    if (g_cleanupInProgress.exchange(true)) {
        Log(L"清理已在进行中，跳过重复调用");
        return 0;
    }

    Log(L"StopHookAndCleanup 被调用 - 开始清理过程");

    // 1) 先置停止标志：hkPresent 此后透传原始 Present，InitThread 若尚未
    //    安装钩子则直接跳过。必须发生在任何等待之前，与 hkPresent 中
    //    “先计数、再检查停止标志”配对（两者均为顺序一致原子操作），保证
    //    任何交错下在途线程都能被等待方观测到。
    g_hookStopped.store(true);

    // 2) 等待初始化线程退出：避免 HookPresent 中的 MinHook 安装与下面的
    //    Disable/Uninitialize 并发操作 MinHook 内部状态。
    if (g_hookThread) {
        if (WaitForSingleObject(g_hookThread, 3000) != WAIT_OBJECT_0) {
            Log(L"等待初始化线程退出超时，放弃清理以避免崩溃");
            return 0;
        }
        CloseHandle(g_hookThread);
        g_hookThread = nullptr;
    }

    // 3) 禁用所有 MinHook 钩子：此后新的 Present 调用直接进入原始函数，
    //    不再经过 hkPresent。
    MH_STATUS mhStatus = MH_DisableHook(MH_ALL_HOOKS);
    if (mhStatus != MH_OK) {
        Log(L"MH_DisableHook 失败，状态: %d", mhStatus);
    }

    // 4) 排空正在 hkPresent 内执行的渲染线程。
    //    原实现为固定 Sleep(100)：低帧率、或一次截图（含 PNG 编码耗时
    //    数百毫秒）恰好在途时，100ms 不足以让渲染线程离开 DLL 代码，
    //    随后 MH_Uninitialize 释放 trampoline、unmap 模块导致闪退。
    //    现改为等待计数归零；超时则放弃卸载（保持已禁用状态，模块常驻）。
    if (!WaitForDrain(g_inHookCount, 5000)) {
        Log(L"等待渲染线程退出 hkPresent 超时 (inHook=%d)，放弃卸载",
            g_inHookCount.load());
        return 0;
    }

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

    // 关闭跨进程“新帧就绪”事件
    if (g_crossProcessReadyEvent) {
        CloseHandle(g_crossProcessReadyEvent);
        g_crossProcessReadyEvent = nullptr;
        Log(L"跨进程新帧就绪事件已关闭");
    }

    // === 点击功能清理 ===
    // 置停止标志并唤醒工作线程；必须等它退出后才能删除其使用的临界区。
    g_clickThreadStop.store(true);
    if (g_clickRequestEvent) SetEvent(g_clickRequestEvent); // 唤醒工作线程使其退出
    if (g_clickThread) {
        if (WaitForSingleObject(g_clickThread, 2000) != WAIT_OBJECT_0) {
            Log(L"点击工作线程未在超时内退出，放弃清理以避免崩溃");
            return 0;
        }
        CloseHandle(g_clickThread);
        g_clickThread = nullptr;
        Log(L"点击工作线程已结束");
    }

    // === 排空在途的导出 API 调用（CaptureFrame/InjectClick 等远程线程）===
    // 这些线程已被上文唤醒，或通过各自的 g_hookStopped 检查快速退出。
    // 必须等它们全部离开后才能删除它们使用过的临界区并卸载模块。
    if (!WaitForDrain(g_apiCallCount, 5000)) {
        Log(L"等待在途远程 API 调用退出超时 (apiCalls=%d)，放弃卸载",
            g_apiCallCount.load());
        return 0;
    }

    // 此后不再有任何其它线程执行 DLL 代码（渲染线程、初始化线程、点击
    // 工作线程、远程调用线程均已离开），以下资源清理可以安全进行。
    if (g_clickRequestEvent) {
        CloseHandle(g_clickRequestEvent);
        g_clickRequestEvent = nullptr;
    }
    if (g_clickSharedView) {
        UnmapViewOfFile(g_clickSharedView);
        g_clickSharedView = nullptr;
    }
    if (g_clickSharedMapping) {
        CloseHandle(g_clickSharedMapping);
        g_clickSharedMapping = nullptr;
        Log(L"点击共享内存已释放");
    }
    if (g_clickCsInited) {
        DeleteCriticalSection(&g_clickCs);
        g_clickCsInited = false;
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

    // 4. 清理日志系统（最后一批日志，随后停机）
    Log(L"StopHookAndCleanup 完成成功");

    if (g_captureDoneEvent) {
        CloseHandle(g_captureDoneEvent);
        g_captureDoneEvent = nullptr;
    }

    if (g_deviceCsInited) {
        DeleteCriticalSection(&g_deviceCs);
        g_deviceCsInited = false;
    }

    Log(L"准备卸载 DLL (hModule=%p)", g_hModule);

    // 5. 日志停机：置位后 Log() 直接返回——包括 unmap 触发的
    //    DLL_PROCESS_DETACH 中的调用。原实现在 DeleteCriticalSection 之后
    //    仍调用 Log（此处及 DLL 分离时），是退出闪退的根源之一。
    g_logShutdown.store(true);
    if (g_logInited) {
        DeleteCriticalSection(&g_logCs);
        g_logInited = false;
    }

    // 6. 收尾余量：计数归零的线程可能停在“fetch_sub 之后、函数返回之前”
    //    的几条指令上（恰被调度器抢占）。短暂等待覆盖该窗口再卸载。
    Sleep(50);

    // 7. 自卸载：FreeLibraryAndExitThread 专为“线程可能仍执行本 DLL 代码”
    //    的场景设计——递减引用计数、（若降为 0 则 unmap）并直接从 kernel32
    //    退出线程，不再返回本 DLL 代码。
    if (g_hModule) {
        g_selfUnloading = true; // 标记为自卸载
        FreeLibraryAndExitThread(g_hModule, 1);
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
    ApiCallGuard apiGuard;
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

    // 通知读端新帧就绪：script 进程等待此事件，替代 2ms 轮询序号
    if (g_crossProcessReadyEvent) {
        SetEvent(g_crossProcessReadyEvent);
    }

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
    ApiCallGuard apiGuard;
    if (!savePath) {
        Log(L"CaptureFrame 被调用，路径为 null");
        return 0;
    }

    Log(L"CaptureFrame 被调用，路径=%s", savePath);

    // 等待初始化完成（减少等待时间）；钩子停止时立即放弃，便于
    // StopHookAndCleanup 快速排空在途调用。
    const int maxWaitMs = 1000; // 从2000ms减少到1000ms
    const int waitInterval = 10;
    int waited = 0;

    while (!IsHookInitialized() && !g_hookStopped.load() && waited < maxWaitMs / waitInterval) {
        Sleep(waitInterval);
        waited++;
    }

    if (g_hookStopped.load() || !IsHookInitialized()) {
        Log(L"钩子未就绪或已停止 (waited=%d ms, stopped=%d)",
            waited * waitInterval, (int)g_hookStopped.load());
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
    // 先登记“正在钩子内执行”，再检查停止标志（两者均为顺序一致原子操作）：
    // 与 StopHookAndCleanup 中“先置停止标志、禁用钩子后等计数归零”配对，
    // 保证任何交错下停止线程都能观测到本线程，不会在渲染线程仍在 DLL
    // 代码内执行时卸载模块。
    g_inHookCount.fetch_add(1);

    // 如果已经停止，直接透传原始函数，尽快离开 DLL 代码
    if (g_hookStopped.load()) {
        HRESULT hr = g_originalPresent(pSwapChain, SyncInterval, Flags);
        g_inHookCount.fetch_sub(1);
        return hr;
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
                    // 记录输出窗口与后备缓冲区尺寸，供点击工作线程使用
                    DXGI_SWAP_CHAIN_DESC scd = {};
                    if (SUCCEEDED(pSwapChain->GetDesc(&scd))) {
                        g_outputHwnd.store(scd.OutputWindow);
                        g_backBufferW.store(scd.BufferDesc.Width);
                        g_backBufferH.store(scd.BufferDesc.Height);
                        Log(L"输出窗口=%p 后备缓冲区=%ux%u", scd.OutputWindow, scd.BufferDesc.Width, scd.BufferDesc.Height);
                    }
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

    // 调用原始函数，返回前登记退出：递减后本函数仅剩返回指令，
    // StopHookAndCleanup 的收尾余量（Sleep(50)）覆盖该窗口
    HRESULT hr = g_originalPresent(pSwapChain, SyncInterval, Flags);
    g_inHookCount.fetch_sub(1);
    return hr;
}

// 向输出窗口投递一次左键点击（客户区坐标）。
// 在游戏进程内部 PostMessage，不依赖前台焦点，适合 Unity 后台运行。
static void PostClickToWindow(HWND hwnd, int x, int y) {
    if (!hwnd) return;
    LPARAM lp = MAKELONG(x, y);
    PostMessageW(hwnd, WM_MOUSEMOVE, 0, lp);
    Sleep(30);
    PostMessageW(hwnd, WM_LBUTTONDOWN, MK_LBUTTON, lp);
    Sleep(40);
    PostMessageW(hwnd, WM_LBUTTONUP, 0, lp);
    Log(L"PostClickToWindow hwnd=%p (%d,%d)", hwnd, x, y);
}

// 点击工作线程
static DWORD WINAPI ClickWorkerThread(LPVOID) {
    Log(L"点击工作线程已启动");
    for (;;) {
        if (g_clickThreadStop.load()) break;
        DWORD wr = WaitForSingleObject(g_clickRequestEvent, 200);
        if (wr == WAIT_TIMEOUT) continue;
        if (g_clickThreadStop.load()) break;
        if (wr != WAIT_OBJECT_0) { Sleep(10); continue; }

        int x = 0, y = 0;
        uint32_t seq = 0;
        bool valid = false;
        EnterCriticalSection(&g_clickCs);
        if (g_clickSharedView) {
            Dx11ClickCommand* cmd = (Dx11ClickCommand*)g_clickSharedView;
            if (cmd->magic == kClickMagic && cmd->version == kClickVersion) {
                x = cmd->x;
                y = cmd->y;
                seq = cmd->sequence;
                valid = true;
            }
        }
        LeaveCriticalSection(&g_clickCs);

        if (!valid) {
            Log(L"点击请求但共享内存无效，跳过");
            continue;
        }

        HWND hwnd = g_outputHwnd.load();
        if (hwnd) {
            PostClickToWindow(hwnd, x, y);
        } else {
            Log(L"点击请求但输出窗口尚未就绪，跳过 (seq=%u)", seq);
        }

        // 标记完成，供 InjectClick / script 端轮询确认
        EnterCriticalSection(&g_clickCs);
        if (g_clickSharedView) {
            ((Dx11ClickCommand*)g_clickSharedView)->doneSeq = seq;
        }
        LeaveCriticalSection(&g_clickCs);
    }
    Log(L"点击工作线程退出");
    return 0;
}

// 导出接口：在指定坐标执行一次左键点击
extern "C" __declspec(dllexport) DWORD InjectClick(LPARAM packedXY) {
    ApiCallGuard apiGuard;
    const int x = (int)(LONG)(uint32_t)(packedXY & 0xFFFFFFFFu);
    const int y = (int)(LONG)(uint32_t)((packedXY >> 32) & 0xFFFFFFFFu);
    Log(L"InjectClick 被调用 x=%d y=%d", x, y);

    // 等待钩子初始化与输出窗口就绪
    for (int i = 0; i < 100; ++i) {
        if (g_hookStopped.load()) { Log(L"InjectClick: 钩子已停止"); return 0; }
        if (IsHookInitialized() && g_outputHwnd.load()) break;
        Sleep(10);
    }
    if (!IsHookInitialized() || !g_outputHwnd.load()) {
        Log(L"InjectClick: 钩子/输出窗口未就绪");
        return 0;
    }
    if (!g_clickCsInited || !g_clickSharedView || !g_clickRequestEvent) {
        Log(L"InjectClick: 点击共享内存/事件未就绪");
        return 0;
    }

    uint32_t mySeq = 0;
    EnterCriticalSection(&g_clickCs);
    {
        Dx11ClickCommand* cmd = (Dx11ClickCommand*)g_clickSharedView;
        cmd->magic = kClickMagic;
        cmd->version = kClickVersion;
        cmd->x = x;
        cmd->y = y;
        cmd->sequence += 1;
        mySeq = cmd->sequence;
        cmd->doneSeq = 0;
    }
    LeaveCriticalSection(&g_clickCs);

    SetEvent(g_clickRequestEvent);

    // 等待工作线程完成投递（最多约 1 秒）
    for (int i = 0; i < 100; ++i) {
        if (g_hookStopped.load()) break;
        Sleep(10);
        EnterCriticalSection(&g_clickCs);
        const uint32_t ds = g_clickSharedView
            ? ((Dx11ClickCommand*)g_clickSharedView)->doneSeq : 0;
        LeaveCriticalSection(&g_clickCs);
        if (ds == mySeq) {
            Log(L"InjectClick 完成 seq=%u", mySeq);
            return 1;
        }
    }
    Log(L"InjectClick 超时未确认 seq=%u", mySeq);
    return 0;
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
    // g_hookThread 已在 DllMain 中保存为 CreateThread 返回的真实句柄，
    // 供 StopHookAndCleanup 等待本线程退出（原实现误存 GetCurrentThread()
    // 伪句柄，无法用于跨线程等待/关闭）。
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
        // 创建跨进程“新帧就绪”事件（auto-reset），script 进程等待此事件读取新帧，
        // 替代轮询共享内存序号。旧版 script 进程不打开它，行为不受影响。
        if (!g_crossProcessReadyEvent) {
            g_crossProcessReadyEvent = CreateEventW(nullptr, FALSE, FALSE, kCaptureReadyEventName);
            if (g_crossProcessReadyEvent) {
                Log(L"跨进程新帧就绪事件已创建: %s", kCaptureReadyEventName);
            }
        }
        g_captureRequested.store(false);

        // === 初始化坐标点击子系统 ===
        if (!g_clickCsInited) {
            InitializeCriticalSection(&g_clickCs);
            g_clickCsInited = true;
        }
        if (!g_clickSharedMapping) {
            g_clickSharedMapping = CreateFileMappingW(
                INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE,
                0, sizeof(Dx11ClickCommand), kClickSharedName);
            if (g_clickSharedMapping) {
                g_clickSharedView = MapViewOfFile(g_clickSharedMapping,
                    FILE_MAP_READ | FILE_MAP_WRITE, 0, 0, sizeof(Dx11ClickCommand));
                if (g_clickSharedView) {
                    Dx11ClickCommand* cmd = (Dx11ClickCommand*)g_clickSharedView;
                    cmd->magic = kClickMagic;
                    cmd->version = kClickVersion;
                    cmd->sequence = 0;
                    cmd->doneSeq = 0;
                    cmd->x = cmd->y = 0;
                    Log(L"点击共享内存已创建: %s", kClickSharedName);
                }
            }
        }
        if (!g_clickRequestEvent) {
            g_clickRequestEvent = CreateEventW(nullptr, FALSE, FALSE, kClickRequestEventName);
            if (g_clickRequestEvent) {
                Log(L"点击请求事件已创建: %s", kClickRequestEventName);
            }
        }
        if (!g_clickThread) {
            g_clickThreadStop.store(false);
            g_clickThread = CreateThread(nullptr, 0, ClickWorkerThread, nullptr, 0, nullptr);
        }

        // 保存真实句柄：StopHookAndCleanup 需等待本线程退出，避免
        // HookPresent 安装钩子与停止清理并发操作 MinHook 内部状态。
        if (!g_hookThread) {
            g_hookThread = CreateThread(nullptr, 0, InitThread, nullptr, 0, nullptr);
        }
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