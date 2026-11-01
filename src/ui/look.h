/* The look the launcher and the installer share (src/ui/launcher.cpp,
 * src/ui/wizard.cpp): the type (Inter 4.1, built in from third_party/fonts/
 * inter), the colours, the game's own key art behind everything, and the
 * widgets drawn over it -- buttons, cards, lights, pills, a panel -- each one
 * reachable by the mouse, the keyboard and a controller.
 *
 * Every size is in design units, 1/720 of the window's height (px()), so a
 * page is the same at any size and on a Retina display. */
#pragma once
#ifndef IMGUI_DEFINE_MATH_OPERATORS
#define IMGUI_DEFINE_MATH_OPERATORS
#endif
#include "imgui.h"

#include <cstdint>
#include <filesystem>
#include <map>
#include <string>
#include <vector>

namespace dod3setup {
namespace look {

const ImU32 kAccent = IM_COL32(196, 30, 45, 255);
const ImU32 kAccentHi = IM_COL32(222, 50, 64, 255);
const ImU32 kText = IM_COL32(242, 240, 236, 255);
const ImU32 kDim = IM_COL32(170, 166, 172, 255);
const ImU32 kFaint = IM_COL32(118, 116, 124, 255);
const ImU32 kOk = IM_COL32(104, 200, 128, 255);
const ImU32 kWarn = IM_COL32(236, 176, 78, 255);
const ImU32 kErr = IM_COL32(236, 92, 96, 255);
const ImU32 kPanel = IM_COL32(13, 13, 17, 232);
const ImU32 kCard = IM_COL32(255, 255, 255, 9);
const ImU32 kLine = IM_COL32(255, 255, 255, 20);

const double kLongAgo = -10;   /* a time (ImGui's clock) for "never": an animation long finished */

/* A trophy grade's colour ('P' 'G' 'S' 'B'), as the trophy site has them too */
ImU32 grade_colour(char grade);
/* A trophy cup `size` tall, centred at c */
void trophy_cup(ImDrawList* dl, ImVec2 c, float size, ImU32 col);

ImU32 alpha(ImU32 c, float a);
ImU32 mix(ImU32 a, ImU32 b, float t);
float ease_out(float t);
std::string size_text(uint64_t bytes);   /* "1.4 GB", "31 MB" */

/* A button's look: quiet, the page's main one (in the accent), or one that removes */
enum class ButtonKind { Quiet, Main, Danger };

/* The mark at the start of a list's line */
enum Mark { kNoMark, kTickMark, kDotMark, kRingMark };

struct Fonts {
    ImFont *regular = nullptr, *medium = nullptr, *semibold = nullptr, *display = nullptr;
};
/* After ui_init: the four Inter faces, Regular the default. */
void load_fonts(Fonts& F);
/* ImGui's own style, for what the widgets below do not draw themselves. */
void style();

/* The key art, PS3_GAME/PIC1.PNG: its pixels (RGBA), the disc's own picture
 * cleaned of its logos (any thread); a texture of them (the UI's thread). */
bool art_pixels(const std::vector<uint8_t>& png, std::vector<uint8_t>* rgba, int* w, int* h);
ImTextureData* art_texture(const std::vector<uint8_t>& rgba, int w, int h);
/* The install's own copy, if it has one. Delete it after ui_shutdown. */
ImTextureData* load_art(const std::filesystem::path& base);

/* The widgets' state and the frame's measures; a page derives from it. */
struct Kit {
    Fonts F;
    float s = 1.0f, W = 0, H = 0, dt = 0;
    double now = 0;
    int frames = 0;
    int cooldown = 0;   /* frames during which activations are ignored */
    std::map<ImGuiID, float> anim;
    std::string toast;
    double toast_at = kLongAgo;
    float wrap_right = 0;   /* inside a card: its padding's edge (screen x) */
    float card_x0 = 0, card_x1 = 0;

    /* at the start of each frame: the measures, the type's scale */
    void begin_frame();

    float px(float design) const { return design * s; }
    /* an item the mouse, the keyboard and a pad all reach: ImGui leaves an
     * invisible button out of its navigation unless asked */
    static bool hit(const char* id, const ImVec2& size);
    /* the last item under the mouse, or focused while the keyboard or a pad is in use */
    static bool hot_item();
    float& anim_of(ImGuiID id) { return anim[id]; }
    void approach(float& v, float target, float speed = 14.0f) const;

    ImVec2 text_size(ImFont* f, float size, const char* t, float wrap = 0.0f) const;
    /* letter-spaced, for the title (ASCII) */
    float tracked_width(ImFont* f, float size, const char* t, float track) const;
    void tracked(ImDrawList* dl, ImFont* f, float size, ImVec2 p, ImU32 col, const char* t, float track) const;
    /* ImGui text in a font and size, wrapped to the region (or the card) */
    void para(ImFont* f, float size, ImU32 col, const char* t);
    /* a path on one line: if it does not fit, its middle gives way to "..." */
    void path_line(ImFont* f, float size, ImU32 col, const char* prefix, const std::filesystem::path& p);
    void say(const std::string& t);

    /* a button `w` design units wide (0: as wide as its label needs) */
    bool button(const char* label, float w, ButtonKind kind = ButtonKind::Quiet, bool enabled = true);
    float button_width(const char* label, float w = 0) const;   /* in pixels */
    void dot(ImDrawList* dl, ImVec2 c, ImU32 col) const;    /* a filled light */
    void ring(ImDrawList* dl, ImVec2 c, ImU32 col) const;   /* an empty one */
    void tick(ImDrawList* dl, ImVec2 c, ImU32 col) const;
    /* a line of a list: a mark, a name, a note after it */
    void list_row(Mark m, ImU32 mark_col, const char* name, ImU32 name_col, const char* after = nullptr,
                  ImU32 after_col = kFaint, ImFont* font = nullptr, float size = 14);
    /* ImGui text in a font, a size and a colour, on one line */
    void label(ImFont* f, float size, ImU32 col, const char* t);
    /* a rounded label ending at `right_top` */
    void pill(ImDrawList* dl, ImVec2 right_top, const char* t, ImU32 col) const;
    /* an on/off switch at the cursor, showing `on`; true when flipped */
    bool toggle(const char* id, bool on, bool enabled = true);
    ImVec2 toggle_size() const { return ImVec2(px(44), px(24)); }
    /* a card around what is drawn between the two */
    void card_begin();
    void card_end();
    /* a bar, `f` of it filled, as wide as the region */
    void progress(float f);
    /* an install's progress: the step, the bar, "x of y" and `more` after it */
    void install_progress(const std::string& what, uint64_t done, uint64_t total, const std::string& more = "");
    /* "Checking 3 files...", while files are being recognised */
    void checking(size_t files);
    void toast_draw();

    /* Enter, or a pad's confirm button; Esc, or a pad's back button: pressed this frame */
    static bool confirm_pressed();
    static bool back_pressed();

    /* the window-sized ImGui window a page is drawn in */
    void begin_window(const char* id);

    /* the window's background: the art (faded in by `art_a`; a dark red
     * gradient without it), darkened on the left and at the foot for the text,
     * and by `shade` for a panel over it */
    void backdrop(ImTextureData* art, float art_a, float shade);
    /* DRAKENGARD 3 / RECOMPILED, at the top left */
    void title();
    /* the panel on the right, `e` (0..1) of the way in, with its heading;
     * the content's top-left and size come back. With `closable`, a close
     * button at its top right: true when pressed. */
    bool panel_frame(const char* heading, const char* sub, float e, bool closable, ImVec2* body, ImVec2* body_size);

    /* Tests (DOD3_SETUP_GRAB=<prefix>): at frame `req_at` the platform is asked
     * for the window (<prefix>.req -> <prefix>.ppm), at `save_at` it is kept as
     * <prefix>_<index>.ppm. */
    static void grab_step(const std::string& prefix, int frame, int req_at, int save_at, int index);
};

}  // namespace look
}  // namespace dod3setup
