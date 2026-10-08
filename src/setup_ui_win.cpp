/* The installer's window on Windows: Win32 + Direct3D 12 under Dear ImGui's
 * own backends, and the shell's file pickers. See setup_ui.h; this follows
 * imgui's examples/example_win32_directx12, with these differences:
 *   - closing the window does not post WM_QUIT (the game's own window and
 *     message loop come after the setup in the same process);
 *   - the window is fixed-size, centred, per-monitor DPI aware (the same
 *     awareness the game's window asks for), and carries the game's icon;
 *   - the wizard sets its style in unscaled units; ui_frame_begin scales it
 *     to the window's monitor, and again when the window moves to another;
 *   - DOD3_SETUP_GRAB=<prefix>: <prefix>.req asks for the next frame as
 *     <prefix>.ppm, the game's PS3RECOMP_FRAME_GRAB protocol, so the pages
 *     can be checked without anyone looking. */
#include "setup_ui.h"
#include "dod3_util.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <shobjidl.h>
#include <shlobj.h>
#include <d3d12.h>
#include <dxgi1_5.h>
#include <io.h>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include "imgui.h"
#include "backends/imgui_impl_win32.h"
#include "backends/imgui_impl_dx12.h"

#pragma comment(lib, "d3d12")
#pragma comment(lib, "dxgi")
#pragma comment(lib, "ole32")
#pragma comment(lib, "shell32")

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

namespace dod3setup {

namespace {

constexpr int kFrames = 2;          /* frames in flight = back buffers */
constexpr int kSrvHeapSize = 16;    /* the font atlas, and room to grow */
const wchar_t kClass[] = L"Dod3Setup";

struct FrameContext {
    ID3D12CommandAllocator* alloc = nullptr;
    UINT64 fence = 0;
};

/* the SRV heap's free list (the example's ExampleDescriptorHeapAllocator) */
struct SrvAlloc {
    ID3D12DescriptorHeap* heap = nullptr;
    D3D12_CPU_DESCRIPTOR_HANDLE cpu0 = {};
    D3D12_GPU_DESCRIPTOR_HANDLE gpu0 = {};
    UINT inc = 0;
    std::vector<int> free_list;
    void create(ID3D12Device* dev, ID3D12DescriptorHeap* h)
    {
        heap = h;
        cpu0 = h->GetCPUDescriptorHandleForHeapStart();
        gpu0 = h->GetGPUDescriptorHandleForHeapStart();
        inc = dev->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        for (int n = kSrvHeapSize; n > 0; n--) free_list.push_back(n - 1);
    }
    void alloc(D3D12_CPU_DESCRIPTOR_HANDLE* c, D3D12_GPU_DESCRIPTOR_HANDLE* g)
    {
        IM_ASSERT(!free_list.empty());
        const int i = free_list.back();
        free_list.pop_back();
        c->ptr = cpu0.ptr + (SIZE_T)i * inc;
        g->ptr = gpu0.ptr + (UINT64)i * inc;
    }
    void release(D3D12_CPU_DESCRIPTOR_HANDLE c, D3D12_GPU_DESCRIPTOR_HANDLE)
    { free_list.push_back((int)((c.ptr - cpu0.ptr) / inc)); }
};

HWND g_hwnd = nullptr;
bool g_closed = false;
bool g_com = false;
float g_dpi = 1.0f;          /* the window's monitor */
float g_dpi_applied = 1.0f;  /* what the style is scaled to */
FrameContext g_frame[kFrames];
UINT g_frame_index = 0;
ID3D12Device* g_dev = nullptr;
ID3D12DescriptorHeap* g_rtv_heap = nullptr;
ID3D12DescriptorHeap* g_srv_heap = nullptr;
SrvAlloc g_srv;
ID3D12CommandQueue* g_queue = nullptr;
ID3D12GraphicsCommandList* g_list = nullptr;
ID3D12Fence* g_fence = nullptr;
HANDLE g_fence_event = nullptr;
UINT64 g_fence_value = 0;
IDXGISwapChain3* g_swap = nullptr;
HANDLE g_swap_wait = nullptr;
bool g_occluded = false;
ID3D12Resource* g_rt[kFrames] = {};
D3D12_CPU_DESCRIPTOR_HANDLE g_rtv[kFrames] = {};
UINT g_width = 0, g_height = 0;

std::wstring widen(const std::string& s)
{
    if (s.empty()) return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
    std::wstring w((size_t)n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), &w[0], n);
    return w;
}

void wait_gpu()
{
    if (!g_queue || !g_fence) return;
    g_queue->Signal(g_fence, ++g_fence_value);
    g_fence->SetEventOnCompletion(g_fence_value, g_fence_event);
    WaitForSingleObject(g_fence_event, INFINITE);
}

void create_targets()
{
    for (UINT i = 0; i < kFrames; i++) {
        ID3D12Resource* b = nullptr;
        g_swap->GetBuffer(i, IID_PPV_ARGS(&b));
        g_dev->CreateRenderTargetView(b, nullptr, g_rtv[i]);
        g_rt[i] = b;
    }
}

void release_targets()
{
    wait_gpu();
    for (auto& r : g_rt)
        if (r) {
            r->Release();
            r = nullptr;
        }
}

bool create_device()
{
    if (D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&g_dev)) != S_OK) return false;
    {
        D3D12_DESCRIPTOR_HEAP_DESC d = {};
        d.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
        d.NumDescriptors = kFrames;
        d.NodeMask = 1;
        if (g_dev->CreateDescriptorHeap(&d, IID_PPV_ARGS(&g_rtv_heap)) != S_OK) return false;
        const SIZE_T inc = g_dev->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
        D3D12_CPU_DESCRIPTOR_HANDLE h = g_rtv_heap->GetCPUDescriptorHandleForHeapStart();
        for (UINT i = 0; i < kFrames; i++) {
            g_rtv[i] = h;
            h.ptr += inc;
        }
    }
    {
        D3D12_DESCRIPTOR_HEAP_DESC d = {};
        d.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
        d.NumDescriptors = kSrvHeapSize;
        d.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
        if (g_dev->CreateDescriptorHeap(&d, IID_PPV_ARGS(&g_srv_heap)) != S_OK) return false;
        g_srv.create(g_dev, g_srv_heap);
    }
    {
        D3D12_COMMAND_QUEUE_DESC d = {};
        d.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
        d.NodeMask = 1;
        if (g_dev->CreateCommandQueue(&d, IID_PPV_ARGS(&g_queue)) != S_OK) return false;
    }
    for (auto& f : g_frame)
        if (g_dev->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&f.alloc)) != S_OK) return false;
    if (g_dev->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, g_frame[0].alloc, nullptr, IID_PPV_ARGS(&g_list)) !=
            S_OK ||
        g_list->Close() != S_OK)
        return false;
    if (g_dev->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&g_fence)) != S_OK) return false;
    g_fence_event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!g_fence_event) return false;

    DXGI_SWAP_CHAIN_DESC1 sd = {};
    sd.BufferCount = kFrames;
    sd.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.Flags = DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.SampleDesc.Count = 1;
    sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    sd.Scaling = DXGI_SCALING_STRETCH;
    IDXGIFactory5* factory = nullptr;
    IDXGISwapChain1* sc1 = nullptr;
    if (CreateDXGIFactory1(IID_PPV_ARGS(&factory)) != S_OK) return false;
    const bool ok = factory->CreateSwapChainForHwnd(g_queue, g_hwnd, &sd, nullptr, nullptr, &sc1) == S_OK &&
                    sc1->QueryInterface(IID_PPV_ARGS(&g_swap)) == S_OK;
    if (ok) factory->MakeWindowAssociation(g_hwnd, DXGI_MWA_NO_ALT_ENTER);
    if (sc1) sc1->Release();
    factory->Release();
    if (!ok) return false;
    g_swap->SetMaximumFrameLatency(kFrames);
    g_swap_wait = g_swap->GetFrameLatencyWaitableObject();
    create_targets();
    return true;
}

void release_device()
{
    if (g_queue && g_fence) wait_gpu();
    for (auto& r : g_rt)
        if (r) {
            r->Release();
            r = nullptr;
        }
    if (g_swap) {
        g_swap->Release();
        g_swap = nullptr;
    }
    if (g_swap_wait) {
        CloseHandle(g_swap_wait);
        g_swap_wait = nullptr;
    }
    for (auto& f : g_frame)
        if (f.alloc) {
            f.alloc->Release();
            f.alloc = nullptr;
            f.fence = 0;
        }
    if (g_list) {
        g_list->Release();
        g_list = nullptr;
    }
    if (g_queue) {
        g_queue->Release();
        g_queue = nullptr;
    }
    if (g_rtv_heap) {
        g_rtv_heap->Release();
        g_rtv_heap = nullptr;
    }
    if (g_srv_heap) {
        g_srv_heap->Release();
        g_srv_heap = nullptr;
    }
    g_srv = SrvAlloc();
    if (g_fence) {
        g_fence->Release();
        g_fence = nullptr;
    }
    if (g_fence_event) {
        CloseHandle(g_fence_event);
        g_fence_event = nullptr;
    }
    if (g_dev) {
        g_dev->Release();
        g_dev = nullptr;
    }
}

FrameContext* next_frame()
{
    FrameContext* f = &g_frame[g_frame_index % kFrames];
    if (g_fence->GetCompletedValue() < f->fence) {
        g_fence->SetEventOnCompletion(f->fence, g_fence_event);
        HANDLE h[] = { g_swap_wait, g_fence_event };
        WaitForMultipleObjects(2, h, TRUE, INFINITE);
    } else {
        WaitForSingleObject(g_swap_wait, INFINITE);
    }
    return f;
}

/* DOD3_SETUP_GRAB: copy the back buffer out and write it as a PPM. */
const char* grab_prefix()
{
    static const char* p = (const char*)1;
    if (p == (const char*)1) {
        p = getenv("DOD3_SETUP_GRAB");
        if (p && !*p) p = nullptr;
    }
    return p;
}

void grab(ID3D12Resource* rt)
{
    const D3D12_RESOURCE_DESC rd = rt->GetDesc();
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp = {};
    UINT64 bytes = 0;
    g_dev->GetCopyableFootprints(&rd, 0, 1, 0, &fp, nullptr, nullptr, &bytes);
    D3D12_HEAP_PROPERTIES hp = {};
    hp.Type = D3D12_HEAP_TYPE_READBACK;
    D3D12_RESOURCE_DESC bd = {};
    bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    bd.Width = bytes;
    bd.Height = 1;
    bd.DepthOrArraySize = 1;
    bd.MipLevels = 1;
    bd.SampleDesc.Count = 1;
    bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    ID3D12Resource* rb = nullptr;
    if (g_dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                       IID_PPV_ARGS(&rb)) != S_OK)
        return;
    ID3D12CommandAllocator* a = nullptr;
    ID3D12GraphicsCommandList* l = nullptr;
    g_dev->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&a));
    g_dev->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, a, nullptr, IID_PPV_ARGS(&l));
    D3D12_RESOURCE_BARRIER b = {};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition.pResource = rt;
    b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    b.Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
    b.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
    l->ResourceBarrier(1, &b);
    D3D12_TEXTURE_COPY_LOCATION dst = {}, src = {};
    dst.pResource = rb;
    dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    dst.PlacedFootprint = fp;
    src.pResource = rt;
    src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    src.SubresourceIndex = 0;
    l->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
    std::swap(b.Transition.StateBefore, b.Transition.StateAfter);
    l->ResourceBarrier(1, &b);
    l->Close();
    g_queue->ExecuteCommandLists(1, (ID3D12CommandList* const*)&l);
    wait_gpu();
    const std::string pre = grab_prefix(), tmp = pre + ".ppm.tmp", out = pre + ".ppm", req = pre + ".req";
    uint8_t* p = nullptr;
    if (rb->Map(0, nullptr, (void**)&p) == S_OK) {
        FILE* f = fopen(tmp.c_str(), "wb");
        if (f) {
            const UINT w = (UINT)rd.Width, h = rd.Height;
            fprintf(f, "P6\n%u %u\n255\n", w, h);
            std::vector<uint8_t> row((size_t)w * 3);
            for (UINT y = 0; y < h; y++) {
                const uint8_t* s = p + fp.Offset + (size_t)y * fp.Footprint.RowPitch;
                for (UINT x = 0; x < w; x++) {
                    row[x * 3] = s[x * 4];
                    row[x * 3 + 1] = s[x * 4 + 1];
                    row[x * 3 + 2] = s[x * 4 + 2];
                }
                fwrite(row.data(), 1, row.size(), f);
            }
            fclose(f);
            remove(out.c_str());
            rename(tmp.c_str(), out.c_str());
            remove(req.c_str());
        }
        D3D12_RANGE none = { 0, 0 };
        rb->Unmap(0, &none);
    }
    l->Release();
    a->Release();
    rb->Release();
}

LRESULT WINAPI wnd_proc(HWND h, UINT msg, WPARAM wp, LPARAM lp)
{
    if (ImGui_ImplWin32_WndProcHandler(h, msg, wp, lp)) return 1;
    switch (msg) {
    case WM_SIZE:
        if (g_dev && wp != SIZE_MINIMIZED) {
            release_targets();
            DXGI_SWAP_CHAIN_DESC1 d = {};
            g_swap->GetDesc1(&d);
            g_swap->ResizeBuffers(0, LOWORD(lp), HIWORD(lp), d.Format, d.Flags);
            create_targets();
            g_width = LOWORD(lp);
            g_height = HIWORD(lp);
        }
        return 0;
    case WM_DPICHANGED: {
        g_dpi = HIWORD(wp) / 96.0f;
        const RECT* r = (const RECT*)lp;
        SetWindowPos(h, nullptr, r->left, r->top, r->right - r->left, r->bottom - r->top,
                     SWP_NOZORDER | SWP_NOACTIVATE);
        return 0;
    }
    case WM_SYSCOMMAND:
        if ((wp & 0xfff0) == SC_KEYMENU) return 0;   /* no Alt menu */
        break;
    case WM_CLOSE:
        g_closed = true;   /* the wizard decides; the window goes in ui_shutdown */
        return 0;
    }
    return DefWindowProcW(h, msg, wp, lp);
}

}  // namespace

bool ui_init(const char* title, int width, int height, const std::string& font_path, float font_px)
{
    /* the awareness the game's window asks for (rsx_d3d12_engine.c) */
    if (HMODULE u32 = GetModuleHandleW(L"user32.dll")) {
        typedef BOOL(WINAPI * SetCtx)(DPI_AWARENESS_CONTEXT);
        if (auto set = (SetCtx)(void*)GetProcAddress(u32, "SetProcessDpiAwarenessContext"))
            set(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    }
    const HRESULT co = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    g_com = co == S_OK || co == S_FALSE;

    POINT origin = { 0, 0 };
    HMONITOR mon = MonitorFromPoint(origin, MONITOR_DEFAULTTOPRIMARY);
    g_dpi = ImGui_ImplWin32_GetDpiScaleForMonitor(mon);
    MONITORINFO mi = { sizeof mi };
    GetMonitorInfoW(mon, &mi);

    WNDCLASSEXW wc = { sizeof wc };
    wc.style = CS_CLASSDC;
    wc.lpfnWndProc = wnd_proc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.hIcon = LoadIconW(wc.hInstance, MAKEINTRESOURCEW(101));   /* app.rc */
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.lpszClassName = kClass;
    RegisterClassExW(&wc);
    const DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX;
    RECT r = { 0, 0, (LONG)(width * g_dpi), (LONG)(height * g_dpi) };
    AdjustWindowRectEx(&r, style, FALSE, 0);
    const int ww = r.right - r.left, wh = r.bottom - r.top;
    const RECT& wa = mi.rcWork;
    g_hwnd = CreateWindowExW(0, kClass, widen(title).c_str(), style, wa.left + (wa.right - wa.left - ww) / 2,
                             wa.top + (wa.bottom - wa.top - wh) / 2, ww, wh, nullptr, nullptr, wc.hInstance, nullptr);
    if (!g_hwnd) {
        ui_shutdown();
        return false;
    }
    g_closed = false;
    if (!create_device()) {
        fprintf(stderr, "[setup] Direct3D 12 is not available\n");
        ui_shutdown();
        return false;
    }
    RECT cr;
    GetClientRect(g_hwnd, &cr);
    g_width = (UINT)cr.right;
    g_height = (UINT)cr.bottom;
    ShowWindow(g_hwnd, SW_SHOWDEFAULT);
    UpdateWindow(g_hwnd);
    SetForegroundWindow(g_hwnd);

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;   /* no imgui.ini beside the game */
    io.LogFilename = nullptr;
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard | ImGuiConfigFlags_NavEnableGamepad;
    g_dpi_applied = 1.0f;   /* ui_frame_begin scales the wizard's style */
    ImGui_ImplWin32_Init(g_hwnd);
    ImGui_ImplDX12_InitInfo ii;
    ii.Device = g_dev;
    ii.CommandQueue = g_queue;
    ii.NumFramesInFlight = kFrames;
    ii.RTVFormat = DXGI_FORMAT_R8G8B8A8_UNORM;
    ii.DSVFormat = DXGI_FORMAT_UNKNOWN;
    ii.SrvDescriptorHeap = g_srv_heap;
    ii.SrvDescriptorAllocFn = [](ImGui_ImplDX12_InitInfo*, D3D12_CPU_DESCRIPTOR_HANDLE* c,
                                 D3D12_GPU_DESCRIPTOR_HANDLE* g) { g_srv.alloc(c, g); };
    ii.SrvDescriptorFreeFn = [](ImGui_ImplDX12_InitInfo*, D3D12_CPU_DESCRIPTOR_HANDLE c,
                                D3D12_GPU_DESCRIPTOR_HANDLE g) { g_srv.release(c, g); };
    if (!ImGui_ImplDX12_Init(&ii)) {
        ui_shutdown();
        return false;
    }
    if (!font_path.empty() && !io.Fonts->AddFontFromFileTTF(font_path.c_str(), font_px))
        fprintf(stderr, "[setup] could not load the font %s\n", font_path.c_str());
    if (io.Fonts->Fonts.empty()) io.Fonts->AddFontDefault();
    ImGui::GetStyle().FontSizeBase = font_px;
    return true;
}

bool ui_frame_begin()
{
    for (;;) {
        MSG m;
        while (PeekMessageW(&m, nullptr, 0, 0, PM_REMOVE)) {
            if (m.message == WM_QUIT) {   /* not ours: leave it for the game's loop */
                PostQuitMessage((int)m.wParam);
                g_closed = true;
                break;
            }
            TranslateMessage(&m);
            DispatchMessageW(&m);
        }
        if (g_closed) return false;
        /* minimised or covered: wait rather than spin */
        if ((g_occluded && g_swap->Present(0, DXGI_PRESENT_TEST) == DXGI_STATUS_OCCLUDED) || IsIconic(g_hwnd)) {
            MsgWaitForMultipleObjects(0, nullptr, FALSE, 50, QS_ALLINPUT);
            continue;
        }
        g_occluded = false;
        break;
    }
    if (g_dpi != g_dpi_applied) {
        ImGuiStyle& st = ImGui::GetStyle();
        st.ScaleAllSizes(g_dpi / g_dpi_applied);
        st.FontScaleDpi = g_dpi;
        g_dpi_applied = g_dpi;
    }
    ImGui_ImplDX12_NewFrame();
    ImGui_ImplWin32_NewFrame();
    ImGui::NewFrame();
    return true;
}

void ui_frame_end()
{
    ImGui::Render();
    FrameContext* f = next_frame();
    const UINT bi = g_swap->GetCurrentBackBufferIndex();
    f->alloc->Reset();
    D3D12_RESOURCE_BARRIER b = {};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition.pResource = g_rt[bi];
    b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    b.Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
    b.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
    g_list->Reset(f->alloc, nullptr);
    g_list->ResourceBarrier(1, &b);
    const ImVec4 bg = ImGui::GetStyle().Colors[ImGuiCol_WindowBg];
    const float clear[4] = { bg.x, bg.y, bg.z, 1.0f };
    g_list->ClearRenderTargetView(g_rtv[bi], clear, 0, nullptr);
    g_list->OMSetRenderTargets(1, &g_rtv[bi], FALSE, nullptr);
    g_list->SetDescriptorHeaps(1, &g_srv_heap);
    ImGui_ImplDX12_RenderDrawData(ImGui::GetDrawData(), g_list);
    std::swap(b.Transition.StateBefore, b.Transition.StateAfter);
    g_list->ResourceBarrier(1, &b);
    g_list->Close();
    g_queue->ExecuteCommandLists(1, (ID3D12CommandList* const*)&g_list);
    g_queue->Signal(g_fence, ++g_fence_value);
    f->fence = g_fence_value;
    if (const char* pre = grab_prefix()) {
        const std::string req = std::string(pre) + ".req";
        if (_access(req.c_str(), 0) == 0) grab(g_rt[bi]);
    }
    g_occluded = g_swap->Present(1, 0) == DXGI_STATUS_OCCLUDED;
    g_frame_index++;
}

void ui_shutdown()
{
    if (ImGui::GetCurrentContext()) {
        wait_gpu();
        if (ImGui::GetIO().BackendRendererUserData) ImGui_ImplDX12_Shutdown();
        if (ImGui::GetIO().BackendPlatformUserData) ImGui_ImplWin32_Shutdown();
        ImGui::DestroyContext();
    }
    release_device();
    if (g_hwnd) {
        DestroyWindow(g_hwnd);
        g_hwnd = nullptr;
    }
    UnregisterClassW(kClass, GetModuleHandleW(nullptr));
    /* WM_DESTROY's leftovers, so the game's loop starts clean */
    MSG m;
    while (PeekMessageW(&m, nullptr, 0, 0, PM_REMOVE)) {
        if (m.message == WM_QUIT) {
            PostQuitMessage((int)m.wParam);
            break;
        }
        DispatchMessageW(&m);
    }
    if (g_com) {
        CoUninitialize();
        g_com = false;
    }
}

std::vector<std::filesystem::path> ui_pick_files(const char* title,
                                                 const std::vector<std::pair<std::string, std::string>>& filters)
{
    std::vector<std::filesystem::path> out;
    IFileOpenDialog* d = nullptr;
    if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&d)))) return out;
    DWORD opt = 0;
    d->GetOptions(&opt);
    d->SetOptions(opt | FOS_ALLOWMULTISELECT | FOS_FORCEFILESYSTEM | FOS_FILEMUSTEXIST | FOS_PATHMUSTEXIST);
    d->SetTitle(widen(title).c_str());
    std::vector<std::wstring> names, specs;
    for (const auto& f : filters) {
        names.push_back(widen(f.first));
        specs.push_back(widen(f.second));
    }
    std::vector<COMDLG_FILTERSPEC> fs;
    for (size_t i = 0; i < names.size(); i++) fs.push_back({ names[i].c_str(), specs[i].c_str() });
    if (!fs.empty()) d->SetFileTypes((UINT)fs.size(), fs.data());
    if (SUCCEEDED(d->Show(g_hwnd))) {
        IShellItemArray* items = nullptr;
        if (SUCCEEDED(d->GetResults(&items))) {
            DWORD n = 0;
            items->GetCount(&n);
            for (DWORD i = 0; i < n; i++) {
                IShellItem* it = nullptr;
                PWSTR p = nullptr;
                if (SUCCEEDED(items->GetItemAt(i, &it)) && SUCCEEDED(it->GetDisplayName(SIGDN_FILESYSPATH, &p))) {
                    out.emplace_back(p);
                    CoTaskMemFree(p);
                }
                if (it) it->Release();
            }
            items->Release();
        }
    }
    d->Release();
    if (ImGui::GetCurrentContext()) {
        ImGui::GetIO().ClearInputKeys();
        ImGui::GetIO().ClearInputMouse();
    }
    return out;
}

std::filesystem::path ui_pick_folder(const char* title)
{
    std::filesystem::path out;
    IFileOpenDialog* d = nullptr;
    if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&d)))) return out;
    DWORD opt = 0;
    d->GetOptions(&opt);
    d->SetOptions(opt | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST);
    d->SetTitle(widen(title).c_str());
    if (SUCCEEDED(d->Show(g_hwnd))) {
        IShellItem* it = nullptr;
        PWSTR p = nullptr;
        if (SUCCEEDED(d->GetResult(&it)) && SUCCEEDED(it->GetDisplayName(SIGDN_FILESYSPATH, &p))) {
            out = p;
            CoTaskMemFree(p);
        }
        if (it) it->Release();
    }
    d->Release();
    if (ImGui::GetCurrentContext()) {
        ImGui::GetIO().ClearInputKeys();
        ImGui::GetIO().ClearInputMouse();
    }
    return out;
}

std::string ui_default_font()
{
    PWSTR dir = nullptr;
    std::string out;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Fonts, 0, nullptr, &dir))) {
        const std::filesystem::path p = std::filesystem::path(dir) / L"segoeui.ttf";
        std::error_code ec;
        if (std::filesystem::exists(p, ec)) out = dod3::utf8(p);
    }
    CoTaskMemFree(dir);
    return out;
}

}  // namespace dod3setup
