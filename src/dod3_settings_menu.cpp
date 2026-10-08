/* The Graphics Settings and System Settings pages in the title's own
 * Settings menu.
 *
 * tools/menu_patch.py rewrites a few script functions in SQEX03GAME.XXX (the
 * copy served through PS3_VFS_OVERLAY): the Settings root gets two more
 * entries, "Graphics Settings" and "System Settings", above Restore
 * Defaults. Both open the page the title shipped unused
 * (Sqex03GameHUDOptionDisplay, with its layout
 * HUD_Pause.menu.select_option_display), rewritten as a list of rows whose
 * count, text and values come from here; the root says which page it opens.
 * The script reaches this file through two natives:
 *
 *   Sqex03DataMessage.GetString(i)   i in [MAGIC, MAGIC + 1000): text from here
 *     MAGIC+0 / +1         Graphics Settings: the root entry's label / description
 *     MAGIC+2 / +3         System Settings: the same
 *     MAGIC+4 / +5         Advanced Graphics (rows from src/dod3_sysset.cpp)
 *     MAGIC+100+row        a row's label (of the open page)
 *     MAGIC+200+row        its value (the one being edited)
 *     MAGIC+300+row        its description
 *   Sqex03GameOption.UpdateDisplayParam(cmd, a, b) -> int
 *     the settings bridge; its only script caller was the unused page
 *     0 begin   1 change(row, dir)   2 is-default(row)   3 reset
 *     4 apply   5 changed?   6 skip the intro? (the title's version-check
 *     page then hands straight to "Press START": no company, middleware
 *     or UE3 logos, no opening movie; DOD3_SKIP_INTRO=0 keeps them)
 *     7 open(root entry): 3 Graphics, 4 System, 5 Advanced Graphics,
 *     6 Restore Defaults (the reset and apply that follow cover every page)
 *     8 the page's rows
 *     9 the camera's FOV   10 is the Graphics page on screen? (in play,
 *     the pause screen's dimming and frost are then left out)
 *
 * Both natives' exec thunks are replaced in the function registry the
 * script VM calls them through. A thunk evaluates its own arguments from the
 * caller's FFrame (Object at +0x14, Code at +0x18) by running the native
 * for each opcode out of GNatives -- step() below does the same.
 *
 * The values are the dod3.ini keys (RSX_SCALE, RSX_DISPLAY, DOD3_FPS,
 * RSX_VSYNC, RSX_AA, RSX_ANISO, DOD3_FOV; RSX_BACKEND, DOD3_SKIP_INTRO,
 * DOD3_UNFOCUSED); Apply writes the open page's to dod3.ini and applies what
 * can change while running: the frame rate, texture filtering, the field
 * of view, the background behaviour and (Direct3D 12 / Vulkan) the
 * resolution, display mode, v-sync and anti-aliasing. The renderer and the
 * intro wait for the next start -- a row whose saved value differs from the
 * running one says so.
 *
 * The field of view: Sqex03GameCamera.UpdateViewTarget passes every view's
 * final FOV here (bridge 9, with whether the gameplay camera made it) and
 * uses the one returned. DOD3_FOV=+N widens the gameplay camera as a zoom
 * factor -- the tangent of the half-angle times tan((65+N)/2) / tan(65/2) --
 * so its default 65 degrees (horizontal) becomes 65+N and the game's own
 * zooms keep their proportion. Cutscene cameras are left alone. */
#include "dod3_ppu.h"
#include "dod3_eboot.h"       /* the EBOOT version's addresses */
#include "dod3_sysset.h"      /* the Advanced Graphics page's rows */
#include "dod3_util.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <chrono>
#include <string>
#include <vector>

extern "C" void ps3_indirect_call(ppu_context* ctx);
extern "C" void ppu_register_function(uint64_t addr, void (*fn)(ppu_context*));
const char* dod3_settings_path();   /* main.cpp: the dod3.ini read at boot, or the one to create */
void dod3_fps_reload();             /* main.cpp: DOD3_FPS again */
void dod3_unfocused_reload();       /* main.cpp: DOD3_UNFOCUSED again */
extern "C" volatile int g_rsx_display_reload;   /* rsx_draw_engine.c: the renderer re-reads its display settings */
#if defined(_WIN32) || defined(__APPLE__)
extern "C" int g_rsx_aniso;         /* the live anisotropy level: rsx_d3d12_engine.c, rsx_metal_backend.m */
#endif
extern "C" int g_rsx_aa;            /* rsx_draw_engine.c: 1 = FXAA at present */

namespace {

/* the EBOOT's (src/dod3_eboot.h) */
const uint32_t GNATIVES = DOD3_A_GNATIVES;        /* 8 bytes an opcode: function, this-adjust */
const uint32_t THUNK_GETSTRING = DOD3_A_EXEC_GETSTRING;  /* USqex03DataMessage::execGetString */
const uint32_t THUNK_BRIDGE = DOD3_A_EXEC_BRIDGE;     /* USqex03GameOption::execUpdateDisplayParam */
const uint32_t FRAME_OBJECT = 0x14, FRAME_CODE = 0x18;

const int MAGIC = 900000;

/* One FFrame::Step: the native for the opcode at Code, with the result
 * written to `result` (guest). */
void step(ppu_context* ctx, uint32_t stack, uint32_t result)
{
    const uint32_t obj = vm_read32(stack + FRAME_OBJECT);
    const uint32_t code = vm_read32(stack + FRAME_CODE);
    const uint32_t op = vm_read8(code);
    vm_write32(stack + FRAME_CODE, code + 1);
    uint32_t fn = vm_read32(GNATIVES + op * 8);
    const uint32_t self = obj + vm_read32(GNATIVES + op * 8 + 4);
    if (fn & 1) fn = vm_read32(vm_read32(self) + fn - 1);   /* virtual: the vtable slot */
    ctx->gpr[2] = vm_read32(fn + 4);
    ctx->gpr[3] = self;
    ctx->gpr[4] = stack;
    ctx->gpr[5] = result;
    ctx->ctr = vm_read32(fn);
    ps3_indirect_call(ctx);
    dod3_drain(ctx);
}

/* Evaluate one int argument, in a frame of our own below the caller's. */
int32_t eval_int(ppu_context* ctx, uint32_t stack)
{
    const uint64_t sp = ctx->gpr[1];
    const uint32_t nsp = ((uint32_t)sp - 0x90u) & ~0xFu;
    vm_write64(nsp, sp);
    vm_write32(nsp + 0x80, 0);
    ctx->gpr[1] = nsp;
    step(ctx, stack, nsp + 0x80);
    ctx->gpr[1] = sp;
    return (int32_t)vm_read32(nsp + 0x80);
}

uint32_t app_realloc(ppu_context* ctx, uint32_t p, uint32_t size)
{
    ctx->gpr[3] = p;
    ctx->gpr[4] = size;
    ctx->gpr[5] = 8;
    DOD3_FN_APP_REALLOC(ctx);
    dod3_drain(ctx);
    return (uint32_t)ctx->gpr[3];
}

/* *fs = text, as the thunk assigns: Data reallocated to the exact size,
 * Num = Max = characters + the terminator, UTF-16. */
void set_fstring(ppu_context* ctx, uint32_t fs, const std::string& text)
{
    const uint32_t n = (uint32_t)text.size() + 1;
    const uint32_t data = app_realloc(ctx, vm_read32(fs), n * 2);
    for (uint32_t i = 0; i + 1 < n; i++) vm_write16(data + 2 * i, (uint8_t)text[i]);
    vm_write16(data + 2 * (n - 1), 0);
    vm_write32(fs, data);
    vm_write32(fs + 4, n);
    vm_write32(fs + 8, n);
}

/* ---- the settings ---------------------------------------------------------- */

struct Choice {
    const char* value;
    const char* text;
};
struct Row {
    const char* key;          /* dod3.ini / environment name */
    const char* label;
    const char* desc;
    std::vector<Choice> choices;   /* value NULL: the key unset */
    int def;                  /* index of the default */
    bool live;                /* applied at once (else on the next start) */
    int running, saved, pending;   /* choice indices; -1 = a value not in the list */
    std::string other;        /* that value */
    int page;                 /* 0 Graphics, 1 System, 2 Advanced Graphics */
    void (*apply_fn)(const char* value);   /* a live row's own apply (src/dod3_sysset.cpp) */
};

std::vector<Row> s_rows;            /* every page's */
const int NPAGES = 3;
std::vector<int> s_page_rows[NPAGES];   /* each page's, in order */
int s_page;                         /* the page open; -1: every page (Restore Defaults) */
bool s_init;
float s_fov_k = 1.0f;               /* DOD3_FOV as a zoom factor on tan(FOV/2) */

void set_fov(const char* v)
{
    const double rad = 3.14159265358979 / 180.0, add = v ? atof(v) : 0.0;
    s_fov_k = (float)(tan((65.0 + add) * rad / 2) / tan(65.0 * rad / 2));
}

/* The rows a command covers: the open page's, or every page's. */
template <class F> void each(F f)
{
    for (Row& r : s_rows)
        if (s_page < 0 || r.page == s_page) f(r);
}

Row* row_at(int k)
{
    const std::vector<int>& v = s_page_rows[s_page > 0 ? s_page : 0];
    return k >= 0 && k < (int)v.size() ? &s_rows[v[k]] : nullptr;
}

int find_choice(const Row& r, const char* v)
{
    for (size_t i = 0; i < r.choices.size(); i++) {
        const char* c = r.choices[i].value;
        if (!v || !*v) {
            if (!c) return (int)i;
            continue;
        }
        if (c && !strcmp(c, v)) return (int)i;
        if (c && r.key == std::string("RSX_SCALE") && atof(c) == atof(v)) return (int)i;
    }
    return -1;
}

void read_current(Row& r, int& idx)
{
    const char* v = getenv(r.key);
    idx = find_choice(r, v);
    if (idx < 0) {
        if (!v || !*v)
            idx = r.def;
        else
            r.other = v;
    }
}

/* The renderer applies RSX_DISPLAY / RSX_VSYNC / RSX_SCALE changes at its
 * next window pump (g_rsx_display_reload); the Direct3D 12, Vulkan and
 * Metal engines do. */
#if defined(_WIN32) || defined(__APPLE__)
#define LIVE_DISPLAY true
#else
#define LIVE_DISPLAY false
#endif

void init_rows()
{
    if (s_init) return;
    s_init = true;
    const char* restart = " Applies after a restart.";
    s_rows.push_back({ "RSX_SCALE",
                       "Resolution",
                       "The resolution the game is drawn at.",
                       { { "1", "1280x720" },
                         { "1.5", "1920x1080" },
                         { "2", "2560x1440" },
                         { "3", "3840x2160" },
                         { "4", "5120x2880" } },
                       0,
                       LIVE_DISPLAY });
    s_rows.push_back({ "RSX_DISPLAY",
                       "Display Mode",
                       "Window, borderless window or fullscreen.",
                       { { "windowed", "Windowed" }, { "borderless", "Borderless" }, { "fullscreen", "Fullscreen" } },
                       0,
                       LIVE_DISPLAY });
    s_rows.push_back({ "DOD3_FPS",
                       "Frame Rate",
                       "The frame rate limit. 60 is recommended.",
                       { { NULL, "30 (Original)" }, { "60", "60" }, { "120", "120" }, { "uncapped", "Unlimited" } },
                       0,
                       true });
    s_rows.push_back({ "RSX_VSYNC",
                       "V-Sync",
                       "Wait for the display's refresh.",
                       { { "1", "On" }, { "0", "Off" } },
                       0,
                       LIVE_DISPLAY });
    s_rows.push_back({ "RSX_AA",
                       "Anti-Aliasing",
                       "Smooths jagged edges. FXAA softens the picture slightly.",
                       { { NULL, "Off" },
                         { "fxaa", "FXAA" },
                         { "msaa2", "MSAA 2x" },
                         { "msaa4", "MSAA 4x" },
                         { "msaa8", "MSAA 8x" } },
                       0,
                       LIVE_DISPLAY });
    s_rows.push_back({ "RSX_ANISO",
                       "Texture Filtering",
                       "Sharper ground and walls when seen at an angle.",
                       { { "1", "Trilinear" },
                         { "2", "2x Anisotropic" },
                         { "4", "4x Anisotropic" },
                         { "8", "8x Anisotropic" },
                         { "16", "16x Anisotropic" } },
                       4,
                       true });
    s_rows.push_back({ "DOD3_FOV",
                       "Field of View",
                       "Widens the gameplay camera. Cutscenes keep their framing.",
                       { { NULL, "Default" },
                         { "5", "+5" },
                         { "10", "+10" },
                         { "15", "+15" },
                         { "20", "+20" },
                         { "25", "+25" },
                         { "30", "+30" } },
                       0,
                       true });
    set_fov(getenv("DOD3_FOV"));
    /* System Settings */
    s_rows.push_back({ "DOD3_SKIP_INTRO",
                       "Skip Intro",
                       "Skip the logos and the opening movie.",
                       { { "1", "On" }, { "0", "Off" } },
                       0,
                       false });
    s_rows.back().page = 1;
    s_rows.push_back({ "DOD3_UNFOCUSED",
                       "When Unfocused",
                       "What the game does while its window is in the background.",
                       { { NULL, "Keep Running" }, { "mute", "Mute" }, { "pause", "Pause" } },
                       0,
                       true });
    s_rows.back().page = 1;
#ifdef _WIN32
    s_rows.push_back({ "RSX_BACKEND",
                       "Renderer",
                       "The graphics API the game is drawn with.",
                       { { NULL, "Direct3D 12" }, { "vulkan", "Vulkan" } },
                       0,
                       false });
#else
    s_rows.push_back(
        { "RSX_BACKEND", "Renderer", "The graphics API the game is drawn with.", { { NULL, "Metal" } }, 0, false });
#endif
    s_rows.back().page = 1;
    /* Advanced Graphics: the engine's own switches (src/dod3_sysset.cpp) */
    std::vector<Dod3RowSpec> adv;
    dod3_sysset_rows(adv);
    for (const Dod3RowSpec& a : adv) {
        Row r{ a.key, a.label, a.desc, {}, a.def, a.live };
        for (const auto& c : a.choices) r.choices.push_back({ c.first, c.second });
        r.page = 2;
        r.apply_fn = a.apply;
        s_rows.push_back(r);
    }
    for (size_t i = 0; i < s_rows.size(); i++) {
        Row& r = s_rows[i];
        if (!r.live) r.desc = strdup((std::string(r.desc) + restart).c_str());
        read_current(r, r.running);
        r.saved = r.pending = r.running;
        s_page_rows[r.page].push_back((int)i);
    }
}

std::string value_text(const Row& r, int idx)
{
    std::string t = idx >= 0 ? r.choices[idx].text : ("Custom (" + r.other + ")");
    if (!r.live && idx != r.running) t += " (next start)";
    return t;
}

bool menu_text(int i, std::string& out)
{
    if (i < MAGIC || i >= MAGIC + 1000) return false;
    init_rows();
    const int k = i - MAGIC;
    const Row* r = row_at(k % 100);
    if (k == 0)
        out = "Graphics Settings";
    else if (k == 1)
        out = "Adjust settings related to graphics and the display.";
    else if (k == 2)
        out = "System Settings";
    else if (k == 3)
        out = "Adjust the start-up, the background behaviour and the renderer.";
    else if (k == 4)
        out = "Advanced Graphics";
    else if (k == 5)
        out = "Adjust the engine's shadows, motion blur and post-processing.";
    else if (k >= 100 && k < 200)
        out = r ? r->label : "";
    else if (k >= 200 && k < 300)
        out = r ? value_text(*r, r->pending) : "";
    else if (k >= 300 && k < 400)
        out = r ? r->desc : "";
    else
        out = "";
    return true;
}

/* dod3.ini: KEY = VALUE on the key's line (uncommenting "#KEY = ..."), or
 * appended; a NULL value comments the line out. */
void write_ini(const std::vector<std::pair<std::string, const char*>>& kv)
{
    const char* path = dod3_settings_path();
    std::vector<std::string> lines;
    bool crlf = false;    /* written back with the line ends it had */
    if (FILE* f = fopen(path, "rb")) {
        char buf[1024];
        while (fgets(buf, sizeof buf, f)) {
            std::string l(buf);
            if (l.size() >= 2 && l[l.size() - 2] == '\r') crlf = true;
            while (!l.empty() && (l.back() == '\n' || l.back() == '\r')) l.pop_back();
            lines.push_back(l);
        }
        fclose(f);
    }
    for (const auto& p : kv) {
        const std::string& key = p.first;
        int hit = -1, commented = -1;
        for (size_t i = 0; i < lines.size(); i++) {
            const char* s = lines[i].c_str();
            while (*s == ' ' || *s == '\t') s++;
            const bool c = *s == '#';
            if (c) {
                s++;
                while (*s == ' ' || *s == '\t') s++;
            }
            if (strncmp(s, key.c_str(), key.size())) continue;
            const char* e = s + key.size();
            while (*e == ' ' || *e == '\t') e++;
            if (*e != '=') continue;
            if (!c) {
                hit = (int)i;
                break;
            }
            if (commented < 0) commented = (int)i;
        }
        const std::string line = p.second ? key + " = " + p.second : "#" + key + " = ";
        if (hit >= 0) {
            if (p.second)
                lines[hit] = line;
            else
                lines[hit] = "#" + lines[hit];
        } else if (p.second) {
            if (commented >= 0)
                lines[commented] = line;
            else
                lines.push_back(line);
        }
    }
    if (FILE* f = fopen(path, "wb")) {
        for (const std::string& l : lines) fprintf(f, "%s%s", l.c_str(), crlf ? "\r\n" : "\n");
        fclose(f);
        fprintf(stderr, "[settings] saved %s\n", path);
    } else {
        fprintf(stderr, "[settings] could not write %s\n", path);
    }
}

void apply()
{
    std::vector<std::pair<std::string, const char*>> kv;
    bool display = false, fps = false, unfocused = false;
    each([&](Row& r) {
        if (r.pending == r.saved) return;
        const char* v = r.pending >= 0 ? r.choices[r.pending].value : r.other.c_str();
        kv.push_back({ r.key, v });
        r.saved = r.pending;
        if (r.live) {
            if (v)
                dod3_setenv(r.key, v, 1);
            else
                dod3_unsetenv(r.key);
            r.running = r.pending;
#if defined(_WIN32) || defined(__APPLE__)
            if (!strcmp(r.key, "RSX_ANISO")) g_rsx_aniso = v ? atoi(v) : 16;
#endif
            if (!strcmp(r.key, "RSX_AA")) {
                g_rsx_aa = !v                    ? 0
                           : !strcmp(v, "fxaa")  ? 1
                           : !strcmp(v, "msaa2") ? 2
                           : !strcmp(v, "msaa4") ? 4
                           : !strcmp(v, "msaa8") ? 8
                                                 : 0;
                display = true;   /* the renderer makes or drops its MSAA targets at the reload */
            }
            if (!strcmp(r.key, "RSX_DISPLAY") || !strcmp(r.key, "RSX_VSYNC") || !strcmp(r.key, "RSX_SCALE"))
                display = true;
            if (!strcmp(r.key, "DOD3_FPS")) fps = true;
            if (!strcmp(r.key, "DOD3_UNFOCUSED")) unfocused = true;
            if (!strcmp(r.key, "DOD3_FOV")) set_fov(v);
            if (r.apply_fn) r.apply_fn(v);
        }
    });
    if (display) g_rsx_display_reload = 1;
    if (fps) dod3_fps_reload();
    if (unfocused) dod3_unfocused_reload();
    if (!kv.empty()) write_ini(kv);
}

std::chrono::steady_clock::time_point s_page_drawn;   /* the page's last is-default query */

int bridge(int cmd, int a, int b)
{
    init_rows();
    switch (cmd) {
    case 0: /* begin: edit the saved values */ each([](Row& r) { r.pending = r.saved; }); return 0;
    case 1: { /* change row a by b */
        Row* r = row_at(a);
        if (!r) return 0;
        const int m = (int)r->choices.size();
        int i = r->pending < 0 ? (b > 0 ? 0 : m - 1) : r->pending + (b > 0 ? 1 : -1);
        if (i < 0) i = m - 1;
        if (i >= m) i = 0;
        r->pending = i;
        return 1;
    }
    case 2: { /* is row a at its default? (asked for every row every frame the page is drawn) */
        s_page_drawn = std::chrono::steady_clock::now();
        const Row* r = row_at(a);
        return (r && r->pending == r->def) ? 1 : 0;
    }
    case 3: /* reset */ each([](Row& r) { r.pending = r.def; }); return 0;
    case 4: apply(); return 0;
    case 5: {
        int changed = 0;
        each([&](Row& r) {
            if (r.pending != r.saved) changed = 1;
        });
        return changed;
    }
    case 6: { /* skip the boot logos and the opening movie? DOD3_SKIP_INTRO, on unless 0 */
        const char* e = getenv("DOD3_SKIP_INTRO");
        return (e && e[0] == '0') ? 0 : 1;
    }
    case 7:   /* the root opens entry a: 3 + page, then Restore Defaults (every page) */
        if (a >= 3 && a < 3 + NPAGES)
            s_page = a - 3;
        else if (a == 3 + NPAGES)
            s_page = -1;
        return 0;
    case 10:  /* is the Graphics page on screen? The pause screen's dimming and
               * frost are left out while it is, so changes can be seen. */
        return ((s_page == 0 || s_page == 2) &&     /* Graphics, Advanced Graphics */
                std::chrono::steady_clock::now() - s_page_drawn < std::chrono::milliseconds(250))
                   ? 1
                   : 0;
    case 8: /* the open page's rows */ return (int)s_page_rows[s_page > 0 ? s_page : 0].size();
    case 9: { /* the camera's field of view (float bits); b: the gameplay camera's */
        float fov;
        memcpy(&fov, &a, 4);
        if (!b || s_fov_k == 1.0f || !(fov > 0.0f && fov < 170.0f)) return a;
        const double rad = 3.14159265358979 / 180.0;
        fov = (float)(2.0 * atan(tan(fov * rad / 2) * s_fov_k) / rad);
        if (fov > 150.0f) fov = 150.0f;
        int r;
        memcpy(&r, &fov, 4);
        return r;
    }
    }
    return 0;
}

/* ---- the two thunks ---------------------------------------------------------- */

uint32_t s_scratch;   /* guest: "IntConst <i> EndFunctionParms" for the original GetString */

void hook_getstring(ppu_context* ctx)
{
    const uint64_t lr = ctx->lr, r2 = ctx->gpr[2];
    const uint32_t self = (uint32_t)ctx->gpr[3], stack = (uint32_t)ctx->gpr[4], result = (uint32_t)ctx->gpr[5];
    const int32_t idx = eval_int(ctx, stack);
    const uint32_t after = vm_read32(stack + FRAME_CODE);   /* at the 0x16 */
    std::string text;
    if (menu_text(idx, text)) {
        vm_write32(stack + FRAME_CODE, after + 1);
        set_fstring(ctx, result, text);
    } else {
        /* The title's own: the original thunk, fed the value already
         * evaluated (an argument is evaluated once, as it would be). */
        if (!s_scratch) s_scratch = app_realloc(ctx, 0, 16);
        vm_write8(s_scratch, 0x1D);
        vm_write32(s_scratch + 1, (uint32_t)idx);
        vm_write8(s_scratch + 5, 0x16);
        vm_write32(stack + FRAME_CODE, s_scratch);
        ctx->gpr[2] = r2;
        ctx->gpr[3] = self;
        ctx->gpr[4] = stack;
        ctx->gpr[5] = result;
        ctx->lr = lr;
        DOD3_FN_EXEC_GETSTRING(ctx);
        dod3_drain(ctx);
        vm_write32(stack + FRAME_CODE, after + 1);
    }
    ctx->gpr[2] = r2;
    ctx->lr = lr;
}

void hook_bridge(ppu_context* ctx)
{
    const uint64_t lr = ctx->lr, r2 = ctx->gpr[2];
    const uint32_t stack = (uint32_t)ctx->gpr[4], result = (uint32_t)ctx->gpr[5];
    if (vm_read8(vm_read32(stack + FRAME_CODE)) == 0x16) {   /* no arguments: the title's call */
        DOD3_FN_EXEC_BRIDGE(ctx);
        dod3_drain(ctx);
        ctx->gpr[2] = r2;
        ctx->lr = lr;
        return;
    }
    int args[3] = { 0, 0, 0 };
    for (int k = 0; k < 3 && vm_read8(vm_read32(stack + FRAME_CODE)) != 0x16; k++) args[k] = eval_int(ctx, stack);
    while (vm_read8(vm_read32(stack + FRAME_CODE)) != 0x16)   /* extra arguments: evaluate, drop */
        eval_int(ctx, stack);
    vm_write32(stack + FRAME_CODE, vm_read32(stack + FRAME_CODE) + 1);
    const int v = bridge(args[0], args[1], args[2]);
    if (result) vm_write32(result, (uint32_t)v);
    ctx->gpr[2] = r2;
    ctx->lr = lr;
}

}  // namespace

/* Called once at boot, after the lifted function table is registered. */
void dod3_settings_menu_install()
{
    ppu_register_function(THUNK_GETSTRING, hook_getstring);
    ppu_register_function(THUNK_BRIDGE, hook_bridge);
}
