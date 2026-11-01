/* The in-game layer and the trophy pop-up (ui/overlay.h). */
#ifndef NOMINMAX
#define NOMINMAX   /* windows.h (through the renderer's headers) would make max() a macro */
#endif
#include "ui/overlay.h"

#include "trophy_host.h"   /* ps3recomp: the unlocks, as the game makes them */

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <mutex>
#include <string>
#include <vector>

#ifdef _WIN32
#include "ui/look.h"
#include "backends/imgui_impl_dx12.h"
#include "rsx_draw_engine.h"   /* rsx_overlay_set */

#include <d3d12.h>
#include <algorithm>
#include <chrono>
#include <map>

namespace dod3setup {
bool png_rgba(const std::vector<uint8_t>& file, std::vector<uint8_t>* rgba, int* w, int* h);   /* ui/png.cpp */
}
#endif

namespace {

/* A pop-up waiting to be shown (the game's thread made it). */
struct Toast {
    std::string key;   /* "NPWR04682_00:5": its icon's texture */
    std::string name, detail;
    char grade = 'B';
    std::vector<uint8_t> rgba;   /* the icon */
    int w = 0, h = 0;
};

std::mutex g_mu;
std::deque<Toast> g_queue;

/* a 240x240 icon at half its size: the pop-up shows it at 64 design units */
void halve(Toast& t)
{
    if (t.w < 160 || t.h < 160) return;
    const int w = t.w / 2, h = t.h / 2;
    std::vector<uint8_t> o((size_t)w * h * 4);
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++)
            for (int c = 0; c < 4; c++) {
                const uint8_t* p = &t.rgba[((size_t)(2 * y) * t.w + 2 * x) * 4 + c];
                const size_t row = (size_t)t.w * 4;
                o[((size_t)y * w + x) * 4 + c] = (uint8_t)((p[0] + p[4] + p[row] + p[row + 4] + 2) / 4);
            }
    t.rgba.swap(o);
    t.w = w;
    t.h = h;
}

void on_unlock(const TrophyUnlockEvent* e, void* user)
{
    const bool popups = user != nullptr;
    fprintf(stderr, "[trophy] %s%s: %s -- %s\n", e->preview ? "(preview) " : "", trophy_grade_name(e->grade), e->name,
            e->detail);
    if (!popups) return;
    Toast t;
    t.key = std::string(e->np_comm_id) + ":" + std::to_string(e->id);
    t.name = e->name;
    t.detail = e->detail;
    t.grade = e->grade;
#ifdef _WIN32
    if (e->icon_png && e->icon_size) {
        const std::vector<uint8_t> file(e->icon_png, e->icon_png + e->icon_size);
        if (dod3setup::png_rgba(file, &t.rgba, &t.w, &t.h)) halve(t);
        else t.rgba.clear();
    }
#endif
    std::lock_guard<std::mutex> lk(g_mu);
    g_queue.push_back(std::move(t));
    while (g_queue.size() > 16) g_queue.pop_front();
}

#ifdef _WIN32
using namespace dod3setup::look;

/* ---- the layer: ImGui on the renderer's thread ---------------------------- */

constexpr int kSrvHeapSize = 160;   /* the font atlas and every icon, never freed */

ImGuiContext* g_ctx;
Fonts g_fonts;
ID3D12DescriptorHeap* g_heap;
UINT g_inc;
std::vector<int> g_free;
DXGI_FORMAT g_fmt = DXGI_FORMAT_UNKNOWN;
bool g_failed;
std::chrono::steady_clock::time_point g_last;

/* the showing pop-up */
bool g_showing;
Toast g_cur;
ImTextureData* g_cur_tex;
double g_cur_at;
double g_clock;
std::map<std::string, ImTextureData*> g_icons;

const float kIn = 0.35f, kHold = 5.6f, kOut = 0.40f;

void srv_alloc(ImGui_ImplDX12_InitInfo*, D3D12_CPU_DESCRIPTOR_HANDLE* c, D3D12_GPU_DESCRIPTOR_HANDLE* g)
{
    IM_ASSERT(!g_free.empty());
    const int i = g_free.back();
    g_free.pop_back();
    c->ptr = g_heap->GetCPUDescriptorHandleForHeapStart().ptr + (SIZE_T)i * g_inc;
    g->ptr = g_heap->GetGPUDescriptorHandleForHeapStart().ptr + (UINT64)i * g_inc;
}
void srv_free(ImGui_ImplDX12_InitInfo*, D3D12_CPU_DESCRIPTOR_HANDLE c, D3D12_GPU_DESCRIPTOR_HANDLE)
{
    g_free.push_back((int)((c.ptr - g_heap->GetCPUDescriptorHandleForHeapStart().ptr) / g_inc));
}

/* The context and the backend, the first time (and again if the window's
 * format changes). */
bool layer_ready(const rsx_overlay_frame* f)
{
    if (g_failed) return false;
    ID3D12Device* dev = (ID3D12Device*)f->device;
    if (!g_ctx) {
        IMGUI_CHECKVERSION();
        g_ctx = ImGui::CreateContext();
        ImGui::SetCurrentContext(g_ctx);
        ImGuiIO& io = ImGui::GetIO();
        io.IniFilename = nullptr;
        io.LogFilename = nullptr;
        io.ConfigFlags |= ImGuiConfigFlags_NoMouse | ImGuiConfigFlags_NoKeyboard;   /* shows, takes nothing */
        style();
        load_fonts(g_fonts);
        D3D12_DESCRIPTOR_HEAP_DESC hd = {};
        hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
        hd.NumDescriptors = kSrvHeapSize;
        hd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
        if (FAILED(dev->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&g_heap)))) {
            fprintf(stderr, "[overlay] no descriptor heap: the in-game layer is off\n");
            g_failed = true;
            return false;
        }
        g_inc = dev->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        for (int n = kSrvHeapSize; n > 0; n--) g_free.push_back(n - 1);
        g_last = std::chrono::steady_clock::now();
    }
    ImGui::SetCurrentContext(g_ctx);
    if (g_fmt != (DXGI_FORMAT)f->format) {
        if (g_fmt != DXGI_FORMAT_UNKNOWN) ImGui_ImplDX12_Shutdown();
        ImGui_ImplDX12_InitInfo ii;
        ii.Device = dev;
        ii.CommandQueue = (ID3D12CommandQueue*)f->queue;
        ii.NumFramesInFlight = (int)std::max<u32>(f->frames_in_flight, 2) + 1;
        ii.RTVFormat = (DXGI_FORMAT)f->format;
        ii.DSVFormat = DXGI_FORMAT_UNKNOWN;
        ii.SrvDescriptorHeap = g_heap;
        ii.SrvDescriptorAllocFn = srv_alloc;
        ii.SrvDescriptorFreeFn = srv_free;
        if (!ImGui_ImplDX12_Init(&ii)) {
            fprintf(stderr, "[overlay] the Direct3D 12 backend did not start: the in-game layer is off\n");
            g_failed = true;
            return false;
        }
        g_fmt = (DXGI_FORMAT)f->format;
        fprintf(stderr, "[overlay] in-game layer on (Direct3D 12, format %u)\n", f->format);
    }
    return true;
}

/* `t` cut to at most `lines` lines of `wrap` pixels, an ellipsis at the end if it was cut */
std::string fit(ImFont* font, float size, const std::string& t, float wrap, int lines)
{
    const char* s = t.c_str();
    const char* end = s + t.size();
    const char* p = s;
    for (int l = 0; l < lines && p < end; l++) {
        const char* e = font->CalcWordWrapPosition(size, p, end, wrap);
        if (e == p) e = p + 1;
        if (l == lines - 1 && e < end) {   /* the last line, with more after it */
            std::string last(p, e);
            while (!last.empty() && font->CalcTextSizeA(size, FLT_MAX, 0, (last + "\xE2\x80\xA6").c_str()).x > wrap)
                last.pop_back();
            while (!last.empty() && last.back() == ' ') last.pop_back();
            return std::string(s, p) + last + "\xE2\x80\xA6";
        }
        p = e;
        while (p < end && (*p == ' ' || *p == '\n')) p++;
    }
    return t;
}

void draw_toast(ImDrawList* dl, float W, float H, float in)
{
    const float s = H / 720.0f;
    auto px = [s](float v) { return v * s; };
    const ImU32 gc = grade_colour(g_cur.grade);
    const float w = px(392), pad = px(14), icon = px(64);
    const float tx_w = w - pad * 3 - icon;
    const char* kinds[] = { "PLATINUM TROPHY", "GOLD TROPHY", "SILVER TROPHY", "BRONZE TROPHY" };
    const char* kind = kinds[g_cur.grade == 'P' ? 0 : g_cur.grade == 'G' ? 1 : g_cur.grade == 'S' ? 2 : 3];
    const std::string name = fit(g_fonts.semibold, px(17), g_cur.name, tx_w, 2);
    const std::string detail = fit(g_fonts.regular, px(13), g_cur.detail, tx_w, 2);
    const ImVec2 ns = g_fonts.semibold->CalcTextSizeA(px(17), FLT_MAX, tx_w, name.c_str());
    const ImVec2 ds = detail.empty() ? ImVec2(0, 0) : g_fonts.regular->CalcTextSizeA(px(13), FLT_MAX, tx_w, detail.c_str());
    const float text_h = px(15) + px(4) + ns.y + (detail.empty() ? 0 : px(3) + ds.y);
    const float h = std::max(icon, text_h) + pad * 2;

    /* slid in from beyond the right edge */
    const float margin = px(26);
    const float x0 = W - margin - w + (1.0f - in) * (w + margin + px(10));
    const float y0 = margin + px(8);
    const float a = std::clamp(in * 1.4f, 0.0f, 1.0f);
    const ImVec2 p0(x0, y0), p1(x0 + w, y0 + h);
    /* a soft shadow, the panel, its edge, the grade's bar */
    for (int i = 3; i >= 1; i--)
        dl->AddRectFilled(p0 + ImVec2(0, px(3)) - ImVec2(px(2) * i, px(2) * i), p1 + ImVec2(0, px(3)) + ImVec2(px(2) * i, px(2) * i),
                          IM_COL32(0, 0, 0, (int)(22 * a)), px(14) + px(2) * i);
    dl->AddRectFilled(p0, p1, alpha(IM_COL32(16, 16, 21, 255), 0.94f * a), px(14));
    dl->AddRect(p0, p1, alpha(IM_COL32(255, 255, 255, 30), a), px(14), 0, std::max(1.0f, px(1)));
    dl->AddRectFilled(ImVec2(x0, y0 + px(14)), ImVec2(x0 + px(3), y0 + h - px(14)), alpha(gc, a), px(2));

    /* the icon */
    const ImVec2 i0(x0 + pad + px(2), y0 + (h - icon) / 2), i1 = i0 + ImVec2(icon, icon);
    if (g_cur_tex)
        dl->AddImageRounded(g_cur_tex->GetTexRef(), i0, i1, ImVec2(0, 0), ImVec2(1, 1), alpha(IM_COL32_WHITE, a), px(9));
    else {
        dl->AddRectFilled(i0, i1, alpha(IM_COL32(255, 255, 255, 14), a), px(9));
        trophy_cup(dl, (i0 + i1) * 0.5f, icon * 0.55f, alpha(gc, a));
    }
    dl->AddRect(i0, i1, alpha(IM_COL32(255, 255, 255, 24), a), px(9));

    /* the grade's line, the name, what it was for */
    float tx = i1.x + pad, ty = y0 + (h - text_h) / 2;
    trophy_cup(dl, ImVec2(tx + px(6), ty + px(7.5f)), px(13), alpha(gc, a));
    float kx = tx + px(17);
    for (const char* c = kind; *c; c++) {   /* letter-spaced */
        const char ch[2] = { *c, 0 };
        dl->AddText(g_fonts.semibold, px(11.5f), ImVec2(kx, ty + px(1.5f)), alpha(gc, a), ch);
        kx += g_fonts.semibold->CalcTextSizeA(px(11.5f), FLT_MAX, 0, ch).x + px(1.1f);
    }
    ty += px(15) + px(4);
    dl->AddText(g_fonts.semibold, px(17), ImVec2(tx, ty), alpha(kText, a), name.c_str(), nullptr, tx_w);
    ty += ns.y + px(3);
    if (!detail.empty()) dl->AddText(g_fonts.regular, px(13), ImVec2(tx, ty), alpha(kDim, a), detail.c_str(), nullptr, tx_w);
}

void overlay_frame(const rsx_overlay_frame* f, void*)
{
    if (f->api != RSX_OVERLAY_D3D12 || !f->width || !f->height) return;
    /* nothing showing and nothing waiting: no frame of ours at all */
    if (!g_showing) {
        std::lock_guard<std::mutex> lk(g_mu);
        if (g_queue.empty()) return;
    }
    if (!layer_ready(f)) return;
    ImGuiIO& io = ImGui::GetIO();
    const auto now = std::chrono::steady_clock::now();
    float dt = std::chrono::duration<float>(now - g_last).count();
    g_last = now;
    dt = std::clamp(dt, 1.0f / 1000, 0.25f);   /* a long load does not skip the slide */
    g_clock += dt;
    io.DeltaTime = dt;
    io.DisplaySize = ImVec2((float)f->width, (float)f->height);

    if (!g_showing) {
        std::lock_guard<std::mutex> lk(g_mu);
        g_cur = std::move(g_queue.front());
        g_queue.pop_front();
        g_showing = true;
        g_cur_at = g_clock;
        g_cur_tex = nullptr;
    }
    ImGui_ImplDX12_NewFrame();
    ImGui::NewFrame();
    if (!g_cur.rgba.empty() && !g_cur_tex) {
        auto it = g_icons.find(g_cur.key);
        if (it == g_icons.end() && g_icons.size() < (size_t)kSrvHeapSize - 8)
            it = g_icons.emplace(g_cur.key, art_texture(g_cur.rgba, g_cur.w, g_cur.h)).first;
        if (it != g_icons.end()) g_cur_tex = it->second;
    }
    const float t = (float)(g_clock - g_cur_at);
    float in = 1.0f;
    if (t < kIn) in = ease_out(t / kIn);
    else if (t > kIn + kHold) in = 1.0f - ease_out((t - kIn - kHold) / kOut);
    draw_toast(ImGui::GetForegroundDrawList(), io.DisplaySize.x, io.DisplaySize.y, std::clamp(in, 0.0f, 1.0f));
    if (t >= kIn + kHold + kOut) g_showing = false;   /* the next one, if any, from the next frame */

    ImGui::Render();
    ID3D12GraphicsCommandList* list = (ID3D12GraphicsCommandList*)f->list;
    list->SetDescriptorHeaps(1, &g_heap);
    ImGui_ImplDX12_RenderDrawData(ImGui::GetDrawData(), list);
}
#endif

}  // namespace

void dod3_overlay_install()
{
    const char* e = getenv("DOD3_TROPHY_POPUPS");
    const bool popups = !(e && e[0] == '0');
    trophy_host_set_listener(on_unlock, popups ? (void*)1 : nullptr);
#ifdef _WIN32
    if (popups) rsx_overlay_set(overlay_frame, nullptr);
#endif
}
