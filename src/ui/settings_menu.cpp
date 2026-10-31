/* The Graphics Settings, System Settings and Advanced Graphics pages in the
 * title's own Settings menu.
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
 *     MAGIC+4 / +5         Advanced Graphics (rows from src/patches/sysset.cpp)
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
 *     11 to 14: the camera's stick and the view's rotation, for
 *     src/input/camera.cpp
 *     15 to 17: the view's aspect ratio, its FOV and the HUD (DOD3_ASPECT)
 *     18 entering a DLC mission (1.01): keep the Japanese voices?
 *   (tools/menu_patch.py has the same numbers, as CMD_*; Cmd below)
 *
 * Both natives' exec thunks are replaced in the function registry the
 * script VM calls them through. A thunk evaluates its own arguments from the
 * caller's FFrame (Object at +0x14, Code at +0x18) by running the native
 * for each opcode out of GNatives -- src/cpu/uscript.h does the same.
 *
 * The values are dod3.ini keys (build_rows lists them, and the launcher shows
 * the same list); Apply writes the open page's to dod3.ini and applies the
 * live rows at once -- the display rows too, on Direct3D 12, Vulkan and
 * Metal (LIVE_DISPLAY). The others wait for the next start, and a row whose
 * saved value differs from the running one says so.
 *
 * With the Spanish fan translation's menus installed and on, the pages speak
 * Spanish too, in its words ("Ajustes ..."): kSpanish, by the English text.
 *
 * The field of view: Sqex03GameCamera.UpdateViewTarget passes every view's
 * final FOV here (bridge 9, with whether the gameplay camera made it) and
 * uses the one returned. DOD3_FOV=+N widens the gameplay camera as a zoom
 * factor -- the tangent of the half-angle times tan((65+N)/2) / tan(65/2) --
 * so its default 65 degrees (horizontal) becomes 65+N and the game's own
 * zooms keep their proportion. Cutscene cameras are left alone. */
#include "cpu/ppu.h"
#include "cpu/eboot.h"       /* the EBOOT version's addresses */
#include "cpu/uscript.h"     /* the script VM, for the two thunks */
#include "input/camera.h"    /* bridge 11 to 14: the camera's stick and the mouse */
#include "patches/prompts.h"     /* DOD3_PROMPTS: the button prompts' glyphs */
#include "patches/sysset.h"      /* the Advanced Graphics page's rows */
#include "ui/ini.h"             /* dod3.ini, edited in place */
#include "util.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <chrono>
#include <iterator>
#include <string>
#include <vector>

extern "C" void ppu_register_function(uint64_t addr, void (*fn)(ppu_context*));
const char* dod3_settings_path();   /* main.cpp: the dod3.ini read at boot, or the one to create */
extern "C" void rsx_present_set_screen_aspect(float aspect);    /* rsx_draw_engine.h */
extern "C" float rsx_present_window_aspect(void);
void dod3_hud_aspect(int phase, uint32_t canvas, bool on);       /* src/patches/hud_aspect.cpp */
bool dod3_dlc_jpvoice();                                          /* src/kernel/mods.cpp */
bool dod3_menus_spanish();                                        /* src/kernel/mods.cpp */
void dod3_fps_reload();             /* main.cpp: DOD3_FPS again */
void dod3_unfocused_reload();       /* main.cpp: DOD3_UNFOCUSED again */
extern "C" volatile int g_rsx_display_reload;   /* rsx_draw_engine.c: the renderer re-reads its display settings */
#if defined(_WIN32) || defined(__APPLE__) || defined(PS3RECOMP_VULKAN_ENGINE_SDL)
extern "C" int g_rsx_aniso;         /* the live anisotropy level: rsx_d3d12_engine.c, rsx_metal_backend.m,
                                       rsx_vulkan_engine.c (Linux) */
#endif
extern "C" int g_rsx_aa;            /* rsx_draw_engine.c: 1 = FXAA at present */

namespace {

/* the EBOOT's (src/cpu/eboot.h) */
const uint32_t THUNK_GETSTRING = DOD3_A_EXEC_GETSTRING;  /* USqex03DataMessage::execGetString */
const uint32_t THUNK_BRIDGE = DOD3_A_EXEC_BRIDGE;     /* USqex03GameOption::execUpdateDisplayParam */
const uint32_t FRAME_CODE = uscript::kFrameCode;
using uscript::eval_int;

const int MAGIC = 900000;

/* The bridge's commands (the header): tools/menu_patch.py's CMD_*, in order. */
enum Cmd {
    CMD_BEGIN,
    CMD_CHANGE,
    CMD_ISDEF,
    CMD_RESET,
    CMD_APPLY,
    CMD_CHANGED,
    CMD_SKIPINTRO,
    CMD_OPEN,
    CMD_ROWS,
    CMD_FOV,
    CMD_GRAPHICS_SHOWN,
    CMD_CAM_DT,
    CMD_CAM_ACTIVE,
    CMD_CAM_AXIS,
    CMD_CAM_VIEW,
    CMD_ASPECT,
    CMD_ASPECT_FOV,
    CMD_HUD,
    CMD_DLC_JPVOICE,
};

/* The script passes floats as their bits in an int. */
float as_float(int bits)
{
    float f;
    memcpy(&f, &bits, 4);
    return f;
}
int as_bits(float f)
{
    int b;
    memcpy(&b, &f, 4);
    return b;
}

const double kRadian = 3.14159265358979 / 180.0;   /* a degree, in radians */

uint32_t app_realloc(ppu_context* ctx, uint32_t p, uint32_t size)
{
    ctx->gpr[3] = p;
    ctx->gpr[4] = size;
    ctx->gpr[5] = 8;
    DOD3_FN_APP_REALLOC(ctx);
    dod3_drain(ctx);
    return (uint32_t)ctx->gpr[3];
}

/* UTF-8 to UTF-16 (the Basic Multilingual Plane: all the text here). */
std::u16string utf16(const std::string& s)
{
    std::u16string out;
    for (size_t i = 0; i < s.size();) {
        const unsigned char c = (unsigned char)s[i];
        const int n = c < 0x80 ? 1 : (c >> 5) == 0x6 ? 2 : (c >> 4) == 0xE ? 3 : 1;
        uint32_t u = n == 1 ? c : n == 2 ? (c & 0x1F) : (c & 0x0F);
        for (int k = 1; k < n && i + k < s.size(); k++) u = u << 6 | ((unsigned char)s[i + k] & 0x3F);
        out += (char16_t)u;
        i += n;
    }
    return out;
}

/* *fs = text, as the thunk assigns: Data reallocated to the exact size,
 * Num = Max = characters + the terminator, UTF-16. */
void set_fstring(ppu_context* ctx, uint32_t fs, const std::string& text)
{
    const std::u16string t = utf16(text);
    const uint32_t n = (uint32_t)t.size() + 1;
    const uint32_t data = app_realloc(ctx, vm_read32(fs), n * 2);
    for (uint32_t i = 0; i + 1 < n; i++) vm_write16(data + 2 * i, t[i]);
    vm_write16(data + 2 * (n - 1), 0);
    vm_write32(fs, data);
    vm_write32(fs + 4, n);
    vm_write32(fs + 8, n);
}

/* ---- the pages in Spanish ---------------------------------------------------- */

/* The text in the Spanish translation's words: its menu is "Ajustes", with
 * "Ajustes de mando", "Ajustes visuales" and descriptions as "Ajustes
 * relacionados con ...". Values left out (1280x720, MSAA 4x, Xbox, ...) are
 * the same in both. */
const struct {
    const char* en;
    const char* es;
} kSpanish[] = {
    /* the Settings root's entries */
    { "Graphics Settings", "Ajustes gráficos" },
    { "Adjust settings related to graphics and the display.", "Ajustes relacionados con los gráficos y la pantalla." },
    { "System Settings", "Ajustes del sistema" },
    { "Adjust the start-up, focus, prompt, mouse and renderer options.",
      "Ajustes del inicio, el segundo plano, los botones y el ratón." },
    { "Advanced Graphics", "Gráficos avanzados" },
    { "Adjust the engine's shadows, motion blur and post-processing.",
      "Ajustes de sombras, desenfoque de movimiento y posprocesado." },
    /* what rows add */
    { " Applies after a restart.", " Se aplica al reiniciar." },
    { " (next start)", " (al reiniciar)" },
    { "Custom", "Personalizado" },
    /* values many rows have */
    { "On", "Sí" },
    { "Off", "No" },
    { "Original", "Original" },
    { "Default", "Predeterminado" },
    { "Auto", "Auto" },
    /* Graphics Settings */
    { "Resolution", "Resolución" },
    { "The resolution the game is drawn at.", "La resolución a la que se dibuja el juego." },
    { "Display Mode", "Modo de pantalla" },
    { "Window, borderless window or fullscreen.", "Ventana, ventana sin bordes o pantalla completa." },
    { "Windowed", "Ventana" },
    { "Borderless", "Sin bordes" },
    { "Fullscreen", "Pantalla completa" },
    { "Frame Rate", "Fotogramas por segundo" },
    { "The frame rate limit. 60 is recommended.", "El límite de fotogramas por segundo. Se recomienda 60." },
    { "30 (Original)", "30 (original)" },
    { "Adaptive", "Adaptativo" },
    { "Unlimited", "Sin límite" },
    { "V-Sync", "Sincronización vertical" },
    { "Wait for the display's refresh.", "Espera al refresco de la pantalla." },
    { "Anti-Aliasing", "Suavizado de bordes" },
    { "Smooths jagged edges. FXAA softens the picture slightly.", "Suaviza los bordes dentados. FXAA suaviza algo la imagen." },
    { "Texture Filtering", "Filtrado de texturas" },
    { "Sharper ground and walls when seen at an angle.", "Suelos y paredes más nítidos vistos en ángulo." },
    { "Trilinear", "Trilineal" },
    { "2x Anisotropic", "Anisotrópico 2x" },
    { "4x Anisotropic", "Anisotrópico 4x" },
    { "8x Anisotropic", "Anisotrópico 8x" },
    { "16x Anisotropic", "Anisotrópico 16x" },
    { "Field of View", "Campo de visión" },
    { "Widens the gameplay camera. Cutscenes keep their framing.", "Amplía la cámara de juego. Las cinemáticas no cambian." },
    /* System Settings */
    { "Skip Intro", "Saltar introducción" },
    { "Skip the logos and the opening movie.", "Salta los logotipos y el vídeo de apertura." },
    { "When Unfocused", "En segundo plano" },
    { "What the game does while its window is in the background.", "Qué hace el juego con su ventana en segundo plano." },
    { "Keep Running", "Seguir" },
    { "Mute", "Silenciar" },
    { "Pause", "Pausar" },
    { "Button Prompts", "Iconos de botones" },
    { "The buttons the game shows. Auto follows the pad you hold.", "Los botones que muestra el juego. Auto sigue a tu mando." },
    { "Keyboard", "Teclado" },
    { "Mouse Sensitivity", "Sensibilidad del ratón" },
    { "How far the mouse turns the camera.", "Cuánto gira la cámara con el ratón." },
    { "Renderer", "Renderizador" },
    { "The graphics API the game is drawn with.", "La API gráfica con la que se dibuja el juego." },
    /* Advanced Graphics (src/patches/sysset.cpp) */
    { "Aspect Ratio", "Relación de aspecto" },
    { "Your screen's shape. Auto follows the window.", "La forma de tu pantalla. Auto sigue a la ventana." },
    { "Shadow Quality", "Calidad de las sombras" },
    { "Sharper, more defined shadows.", "Sombras más nítidas y definidas." },
    { "High", "Alta" },
    { "Ultra", "Ultra" },
    { "Motion Blur", "Desenfoque de movimiento" },
    { "Blur on fast movement of the camera and characters.", "Desenfoque al mover rápido la cámara y los personajes." },
    { "Post-Processing", "Posprocesado" },
    { "Depth of field, bloom and the game's colour grading.", "Profundidad de campo, resplandor y etalonaje del juego." },
    { "Depth of Field", "Profundidad de campo" },
    { "The blur of what is near and far. Off keeps the bloom.", "Desenfoque de lo cercano y lo lejano. Sin él queda el resplandor." },
    { "Blood on Screen", "Sangre en pantalla" },
    { "Blood splashes and the low-health border over the screen.", "Salpicaduras y borde de salud baja sobre la pantalla." },
    { "Border Only", "Solo el borde" },
    { "Draw Distance", "Distancia de dibujado" },
    { "How far away props, plants and other scenery are drawn.", "Hasta dónde se dibujan objetos, plantas y escenario." },
    { "Tree Detail", "Detalle de los árboles" },
    { "How far trees show and stay detailed. From the next area.", "Distancia de detalle de los árboles. Desde la próxima zona." },
};

bool s_spanish;   /* the Spanish translation's menus are on (init_rows) */

/* The text as the pages show it: in Spanish with the translation's menus, else as it is. */
const char* tr(const char* en)
{
    if (!s_spanish || !en) return en;
    for (const auto& t : kSpanish)
        if (!strcmp(t.en, en)) return t.es;
    return en;
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
    int page;                 /* PAGE_GRAPHICS, PAGE_SYSTEM, PAGE_ADVANCED */
    void (*apply_fn)(const char* value);   /* a live row's own apply (src/patches/sysset.cpp) */
};

std::vector<Row> s_rows;            /* every page's */
const int NPAGES = 3;
std::vector<int> s_page_rows[NPAGES];   /* each page's, in order */
int s_page;                         /* the page open; -1: every page (Restore Defaults) */
bool s_init;
float s_fov_k = 1.0f;               /* DOD3_FOV as a zoom factor on tan(FOV/2) */
float s_aspect;                     /* DOD3_ASPECT: the screen's width over its height; 0: the title's 16:9 */
bool s_aspect_auto;                 /* DOD3_ASPECT=auto: the window's */
float s_aspect_from;                /* the ratio the view had (bridge 15); 0: not constrained, left alone */
float s_aspect_now;                 /* the one it has (0: as it was) */

void set_fov(const char* v)
{
    const double add = v ? atof(v) : 0.0;
    s_fov_k = (float)(tan((65.0 + add) * kRadian / 2) / tan(65.0 * kRadian / 2));
}

/* A screen shape the view can take: 5:4 to 8:1, and not 16:9 (the title's). */
float aspect_ok(float a)
{
    a = a < 1.25f ? 1.25f : a > 8.0f ? 8.0f : a;
    return fabsf(a / (16.0f / 9.0f) - 1.0f) < 0.01f ? 0.0f : a;
}

/* DOD3_ASPECT: W:H or a ratio, or auto (the window's); 21:9 is the 2560x1080
 * and 5120x2160 screens' 64:27. Unset: the title's 16:9. The present is told
 * the screen's shape: the frame is shown in that shape, in the middle of the
 * window, and what is 16:9 in it (the title, movies) in the middle of that. */
void set_aspect(const char* v)
{
    float w = 0.0f, h = 0.0f;
    s_aspect = 0.0f;
    s_aspect_auto = v && !strcmp(v, "auto");
    if (v && *v && !s_aspect_auto) {
        if (sscanf(v, "%f:%f", &w, &h) == 2 && h > 0.0f)
            s_aspect = (w == 21.0f && h == 9.0f) ? 64.0f / 27.0f : w / h;
        else
            s_aspect = (float)atof(v);
        s_aspect = s_aspect > 0.0f ? aspect_ok(s_aspect) : 0.0f;
    }
    rsx_present_set_screen_aspect(s_aspect_auto ? 0.0f : s_aspect > 0.0f ? s_aspect : 16.0f / 9.0f);
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
#if defined(_WIN32) || defined(__APPLE__) || defined(PS3RECOMP_VULKAN_ENGINE_SDL)
#define LIVE_DISPLAY true
#else
#define LIVE_DISPLAY false
#endif

/* Every page's rows, in order: what each is, not what it is set to. The
 * launcher (src/ui/launcher.cpp) shows the same list. */
void build_rows(std::vector<Dod3RowSpec>& rows)
{
    rows.push_back({ "RSX_SCALE",
                     "Resolution",
                     "The resolution the game is drawn at.",
                     { { "1", "1280x720" },
                       { "1.5", "1920x1080" },
                       { "2", "2560x1440" },
                       { "3", "3840x2160" },
                       { "4", "5120x2880" } },
                     0,
                     LIVE_DISPLAY,
                     nullptr,
                     PAGE_GRAPHICS });
    rows.push_back({ "RSX_DISPLAY",
                     "Display Mode",
                     "Window, borderless window or fullscreen.",
                     { { "windowed", "Windowed" }, { "borderless", "Borderless" }, { "fullscreen", "Fullscreen" } },
                     0,
                     LIVE_DISPLAY,
                     nullptr,
                     PAGE_GRAPHICS });
    rows.push_back({ "DOD3_FPS",
                     "Frame Rate",
                     "The frame rate limit. 60 is recommended.",
                     { { NULL, "30 (Original)" },
                       { "60", "60" },
                       { "120", "120" },
                       { "adaptive", "Adaptive" },
                       { "uncapped", "Unlimited" } },
                     0,
                     true,
                     nullptr,
                     PAGE_GRAPHICS });
    rows.push_back({ "RSX_VSYNC",
                     "V-Sync",
                     "Wait for the display's refresh.",
                     { { "1", "On" }, { "0", "Off" } },
                     0,
                     LIVE_DISPLAY,
                     nullptr,
                     PAGE_GRAPHICS });
    rows.push_back({ "RSX_AA",
                     "Anti-Aliasing",
                     "Smooths jagged edges. FXAA softens the picture slightly.",
                     { { NULL, "Off" },
                       { "fxaa", "FXAA" },
                       { "msaa2", "MSAA 2x" },
                       { "msaa4", "MSAA 4x" },
                       { "msaa8", "MSAA 8x" } },
                     0,
                     LIVE_DISPLAY,
                     nullptr,
                     PAGE_GRAPHICS });
    rows.push_back({ "RSX_ANISO",
                     "Texture Filtering",
                     "Sharper ground and walls when seen at an angle.",
                     { { "1", "Trilinear" },
                       { "2", "2x Anisotropic" },
                       { "4", "4x Anisotropic" },
                       { "8", "8x Anisotropic" },
                       { "16", "16x Anisotropic" } },
                     4,
                     true,
                     nullptr,
                     PAGE_GRAPHICS });
    rows.push_back({ "DOD3_FOV",
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
                     true,
                     [](const char* v) { set_fov(v); },
                     PAGE_GRAPHICS });
    /* Advanced Graphics: the aspect ratio, then the engine's own switches
     * (below). The Graphics page has the layout's seven rows. */
    rows.push_back({ "DOD3_ASPECT",
                     "Aspect Ratio",
                     "Your screen's shape. Auto follows the window.",
                     { { NULL, "16:9" }, { "16:10", "16:10" }, { "21:9", "21:9" }, { "32:9", "32:9" }, { "auto", "Auto" } },
                     0,
                     true,
                     [](const char* v) { set_aspect(v); },
                     PAGE_ADVANCED });
    /* System Settings */
    rows.push_back({ "DOD3_SKIP_INTRO",
                     "Skip Intro",
                     "Skip the logos and the opening movie.",
                     { { "1", "On" }, { "0", "Off" } },
                     0,
                     false,
                     nullptr,
                     PAGE_SYSTEM });
    rows.push_back({ "DOD3_UNFOCUSED",
                     "When Unfocused",
                     "What the game does while its window is in the background.",
                     { { NULL, "Keep Running" }, { "mute", "Mute" }, { "pause", "Pause" } },
                     0,
                     true,
                     nullptr,
                     PAGE_SYSTEM });
    rows.push_back({ "DOD3_PROMPTS",
                     "Button Prompts",
                     "The buttons the game shows. Auto follows the pad you hold.",
                     { { NULL, "Auto" },
                       { "ps3", "PlayStation 3" },
                       { "ps4", "PlayStation 4" },
                       { "ps5", "PlayStation 5" },
                       { "xbox", "Xbox" },
                       { "switch", "Nintendo Switch" },
                       { "keyboard", "Keyboard" } },
                     0,
                     true,
                     [](const char*) { dod3_prompts_poll(true); },
                     PAGE_SYSTEM });
    rows.push_back({ "DOD3_MOUSE_SENS",
                     "Mouse Sensitivity",
                     "How far the mouse turns the camera.",
                     { { "0.25", "1" },
                       { "0.4", "2" },
                       { "0.55", "3" },
                       { "0.75", "4" },
                       { NULL, "5" },
                       { "1.3", "6" },
                       { "1.7", "7" },
                       { "2.2", "8" },
                       { "3", "9" },
                       { "4", "10" } },
                     4,
                     true,
                     [](const char*) { dod3_camera_reload(); },
                     PAGE_SYSTEM });
    rows.push_back({ "RSX_BACKEND",
                     "Renderer",
                     "The graphics API the game is drawn with.",
#ifdef _WIN32
                     { { NULL, "Direct3D 12" }, { "vulkan", "Vulkan" } },
#elif defined(__APPLE__)
                     { { NULL, "Metal" } },
#else
                     { { NULL, "Vulkan" } },
#endif
                     0,
                     false,
                     nullptr,
                     PAGE_SYSTEM });
    /* Advanced Graphics: the engine's own switches (src/patches/sysset.cpp) */
    dod3_sysset_rows(rows);
}

void init_rows()
{
    if (s_init) return;
    s_init = true;
    set_fov(getenv("DOD3_FOV"));   /* (the aspect ratio is set at boot: dod3_settings_menu_install) */
    s_spanish = dod3_menus_spanish();
    std::vector<Dod3RowSpec> specs;
    build_rows(specs);
    for (const Dod3RowSpec& a : specs) {
        Row r{ a.key, a.label, a.desc, {}, a.def, a.live };
        for (const auto& c : a.choices) r.choices.push_back({ c.first, c.second });
        r.page = a.page;
        r.apply_fn = a.apply;
        read_current(r, r.running);
        r.saved = r.pending = r.running;
        s_page_rows[r.page].push_back((int)s_rows.size());
        s_rows.push_back(r);
    }
}

std::string value_text(const Row& r, int idx)
{
    std::string t = idx >= 0 ? tr(r.choices[idx].text) : std::string(tr("Custom")) + " (" + r.other + ")";
    if (!r.live && idx != r.running) t += tr(" (next start)");
    return t;
}

/* a row's description, and that it waits for the next start if it does */
std::string desc_text(const Row& r)
{
    return std::string(tr(r.desc)) + (r.live ? "" : tr(" Applies after a restart."));
}

bool menu_text(int i, std::string& out)
{
    if (i < MAGIC || i >= MAGIC + 1000) return false;
    init_rows();
    const int k = i - MAGIC;
    const Row* r = row_at(k % 100);
    static const char* const kRoot[] = {   /* the Settings root's entries: label, description */
        "Graphics Settings", "Adjust settings related to graphics and the display.",
        "System Settings",   "Adjust the start-up, focus, prompt, mouse and renderer options.",
        "Advanced Graphics", "Adjust the engine's shadows, motion blur and post-processing.",
    };
    if (k >= 0 && k < (int)std::size(kRoot))
        out = tr(kRoot[k]);
    else if (k >= 100 && k < 200)
        out = r ? tr(r->label) : "";
    else if (k >= 200 && k < 300)
        out = r ? value_text(*r, r->pending) : "";
    else if (k >= 300 && k < 400)
        out = r ? desc_text(*r) : "";
    else
        out = "";
    return true;
}

/* dod3.ini, edited in place (src/ui/ini.cpp) */
void write_ini(const std::vector<std::pair<std::string, const char*>>& kv)
{
    dod3::ini_write(std::filesystem::path(dod3_settings_path()), kv);   /* the narrow path main.cpp opened */
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
#if defined(_WIN32) || defined(__APPLE__) || defined(PS3RECOMP_VULKAN_ENGINE_SDL)
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
    if (cmd < CMD_CAM_DT || cmd > CMD_CAM_VIEW) init_rows();   /* the camera's come every frame and need no rows */
    switch (cmd) {
    case CMD_BEGIN: /* edit the saved values */ each([](Row& r) { r.pending = r.saved; }); return 0;
    case CMD_CHANGE: { /* row a, a step in direction b */
        Row* r = row_at(a);
        if (!r) return 0;
        const int m = (int)r->choices.size();
        int i = r->pending < 0 ? (b > 0 ? 0 : m - 1) : r->pending + (b > 0 ? 1 : -1);
        if (i < 0) i = m - 1;
        if (i >= m) i = 0;
        r->pending = i;
        return 1;
    }
    case CMD_ISDEF: { /* is row a at its default? (asked for every row every frame the page is drawn) */
        s_page_drawn = std::chrono::steady_clock::now();
        const Row* r = row_at(a);
        return (r && r->pending == r->def) ? 1 : 0;
    }
    case CMD_RESET: each([](Row& r) { r.pending = r.def; }); return 0;
    case CMD_APPLY: apply(); return 0;
    case CMD_CHANGED: {
        int changed = 0;
        each([&](Row& r) {
            if (r.pending != r.saved) changed = 1;
        });
        return changed;
    }
    case CMD_SKIPINTRO: { /* skip the boot logos and the opening movie? DOD3_SKIP_INTRO, on unless 0 */
        const char* e = getenv("DOD3_SKIP_INTRO");
        return (e && e[0] == '0') ? 0 : 1;
    }
    case CMD_OPEN:   /* the root opens entry a: 3 + page, then Restore Defaults (every page) */
        if (a >= 3 && a < 3 + NPAGES)
            s_page = a - 3;
        else if (a == 3 + NPAGES)
            s_page = -1;
        return 0;
    case CMD_ROWS: /* the open page's rows */ return (int)s_page_rows[s_page > 0 ? s_page : 0].size();
    case CMD_FOV: { /* the camera's field of view (float bits); b: the gameplay camera's */
        float fov = as_float(a);
        if (!b || s_fov_k == 1.0f || !(fov > 0.0f && fov < 170.0f)) return a;
        fov = (float)(2.0 * atan(tan(fov * kRadian / 2) * s_fov_k) / kRadian);
        return as_bits(fov > 150.0f ? 150.0f : fov);
    }
    case CMD_GRAPHICS_SHOWN:   /* is a graphics page on screen? The pause screen's dimming and
                                * frost are left out while it is, so changes can be seen. */
        return ((s_page == PAGE_GRAPHICS || s_page == PAGE_ADVANCED) &&
                std::chrono::steady_clock::now() - s_page_drawn < std::chrono::milliseconds(250))
                   ? 1
                   : 0;
    /* the camera's stick on its way to the pawn (src/input/camera.cpp) */
    case CMD_CAM_DT: /* the frame's fDelta (float bits); b: the pawn is the dragon */
        dod3_camera_frame(as_float(a), b);
        return 0;
    case CMD_CAM_ACTIVE: /* is the mouse turning the camera? */ return dod3_camera_active();
    case CMD_CAM_AXIS: /* the stick's value on axis a (float bits) -> the one to use */
        return as_bits(dod3_camera_axis(a, as_float(b)));
    /* (CMD_CAM_VIEW takes a Rotator: hook_bridge) */
    /* the view's aspect ratio: Sqex03GameCamera.DoUpdateCamera, before it
     * fills the camera cache. A constrained view -- in play; the title's is
     * not -- is letterboxed (or pillarboxed) by the engine to its ratio, with
     * the projection made for it: it gets DOD3_ASPECT. A wider view's FOV
     * (horizontal) is widened by the ratio's change so that the view keeps
     * its height; a narrower one keeps its FOV and is taller. The HUD is
     * fitted into the view, and the present told its shape, by CMD_HUD. */
    case CMD_ASPECT: { /* the camera's ConstrainedAspectRatio (float bits); b: is the view constrained to it */
        const float ar = as_float(a);
        s_aspect_from = (b && ar > 0.5f && ar < 8.0f) ? ar : 0.0f;
        if (getenv("DOD3_ASPECT_LOG")) {
            static float last_ar = -1.0f;
            static int last_b = -1;
            if (ar != last_ar || b != last_b) fprintf(stderr, "[aspect] camera %.4f constrained %d\n", ar, b);
            last_ar = ar;
            last_b = b;
        }
        float want = s_aspect;
        if (s_aspect_auto) {
            const float wa = rsx_present_window_aspect();
            want = wa > 0.0f ? aspect_ok(wa) : 0.0f;
        }
        s_aspect_now = s_aspect_from > 0.0f ? want : 0.0f;
        return s_aspect_now > 0.0f ? as_bits(s_aspect_now) : a;
    }
    case CMD_ASPECT_FOV: { /* the view's FOV (float bits) */
        float fov = as_float(a);
        if (s_aspect_now <= s_aspect_from || !(fov > 0.0f && fov < 170.0f)) return a;
        fov = (float)(2.0 * atan(tan(fov * kRadian / 2) * s_aspect_now / s_aspect_from) / kRadian);
        return as_bits(fov > 170.0f ? 170.0f : fov);
    }
    case CMD_HUD: /* the HUD's drawing: a phase, and for the begin its UCanvas */
        dod3_hud_aspect(a, (uint32_t)b, s_aspect_now > 0.0f);
        return 0;
    case CMD_DLC_JPVOICE: /* entering a DLC mission (1.01): keep the Japanese voices? */
        return dod3_dlc_jpvoice() ? 1 : 0;
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
        vm_write8(s_scratch, uscript::kIntConst);
        vm_write32(s_scratch + 1, (uint32_t)idx);
        vm_write8(s_scratch + 5, uscript::kEndParms);
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
    if (uscript::at_end(stack)) {   /* no arguments: the title's call */
        DOD3_FN_EXEC_BRIDGE(ctx);
        dod3_drain(ctx);
        ctx->gpr[2] = r2;
        ctx->lr = lr;
        return;
    }
    int args[3] = { 0, 0, 0 }, rot_yaw = 0;
    for (int k = 0; k < 3 && !uscript::at_end(stack); k++) {
        if (k == 1 && args[0] == CMD_CAM_VIEW) {   /* a Rotator -- Pitch, Yaw, Roll: the first two */
            const uint64_t sp = ctx->gpr[1];
            const uint32_t nsp = ((uint32_t)sp - 0xA0u) & ~0xFu;
            vm_write64(nsp, sp);
            ctx->gpr[1] = nsp;
            uscript::step(ctx, stack, nsp + 0x80);
            ctx->gpr[1] = sp;
            args[1] = (int)vm_read32(nsp + 0x80);
            rot_yaw = (int)vm_read32(nsp + 0x84);
            continue;
        }
        args[k] = eval_int(ctx, stack);
    }
    while (!uscript::at_end(stack))   /* extra arguments: evaluate, drop */
        eval_int(ctx, stack);
    vm_write32(stack + FRAME_CODE, vm_read32(stack + FRAME_CODE) + 1);
    int v = 0;
    if (args[0] == CMD_CAM_VIEW)
        dod3_camera_view(args[1], rot_yaw, args[2]);   /* the view's rotation; is it the gameplay camera's */
    else
        v = bridge(args[0], args[1], args[2]);
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
    set_aspect(getenv("DOD3_ASPECT"));   /* the present's screen shape, before the first movie */
}

void dod3_setting_rows(std::vector<Dod3RowSpec>& rows) { build_rows(rows); }
