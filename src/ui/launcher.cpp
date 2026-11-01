/* The launcher: what the player sees at each start once the game is
 * installed (ui/ui.h, run_launcher). The game's own key art (PS3_GAME/PIC1.PNG
 * from the player's install; nothing of the game's ships with the port)
 * fills the window, the title and a menu on its left -- Play, Mods, DLC,
 * Settings, Quit -- and each section opens as a panel over the art:
 *
 *   Mods      the fan-made additions the setup knows (the Spanish translation,
 *             the DLC Undub) and any other installed: what is in, a switch to
 *             turn one off and on again (not the undub: it follows the game's
 *             Voice option), Remove, and Add Files / Add Folder for more
 *             (install/install.h does the work)
 *   DLC       the 20 packs, which are in, and Add DLC Packages
 *   Settings  dod3.ini: the in-game Settings pages' rows (src/ui/settings_menu.cpp,
 *             one list for both), a few that are only in the file (window size,
 *             mouse camera, camera speed, this launcher), the keyboard's keys
 *             and the cheats (src/patches/cheats.cpp, song_autoplay.cpp).
 *             A change is saved at once, the file's comments kept (ui/ini.h).
 *   Trophies  the disc's trophy list with what this install has earned
 *             (ui/trophies.h)
 *   Update    Windows: a newer release the launcher found on GitHub as it
 *             opened (install/update.h), offered by a line in the footer:
 *             its notes, and Install Update, which replaces the program's
 *             files and starts the new one
 *
 * The look -- Inter, the colours, the art, the widgets -- is the installer's
 * too (ui/look.h). Keyboard, mouse and controller all work (the backends feed
 * ImGui's navigation).
 *
 * Tests: DOD3_LAUNCHER_OPEN=mods|dlc|settings[:<n>]|trophies|update opens a panel at start
 * (n: the settings tab, 0 Display to 4 Cheats), DOD3_LAUNCHER_ADD=
 * '<file>|<file>' adds files as Add Files would, and DOD3_SETUP_GRAB=<prefix>
 * saves the window as <prefix>_0.ppm once it has settled (the platform's
 * grab); DOD3_LAUNCHER_AUTO=quit then closes it, =install presses Install (or
 * Remove) and closes it when that is done, =update installs the release the
 * check finds (install/update.h has its own test switches) and starts it, or
 * closes on a failure. DOD3_LAUNCHER_REMOVE=spanish|undub
 * starts removing one; DOD3_LAUNCHER_SWITCH=spanish:off|spanish:on switches
 * the translation at start; DOD3_LAUNCHER_KEYS='Down,Down,Space,...' presses
 * those keys, one every 12 frames (a key a player presses, as ImGui gets it
 * from the platform). */
#include "ui/ui.h"
#include "ui/ini.h"
#include "ui/look.h"           /* the type, the colours, the art, the widgets */
#include "ui/setup_shared.h"   /* the files added and the install, on their own threads */
#include "ui/trophies.h"       /* the Trophies panel */
#include "install/install.h"
#include "patches/sysset.h"    /* Dod3RowSpec, dod3_setting_rows */
#include "cpu/eboot.h"         /* what this EBOOT's build can patch */
#include "util.h"
#include "app_version.h"       /* generated: the port's version label */

#include "imgui_internal.h"    /* SetKeyOwner */

#ifdef _WIN32
#include "install/update.h"    /* the newest release, installed in place */
#endif

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iterator>
#include <map>
#include <memory>

const char* dod3_settings_path_now();   /* main.cpp: the dod3.ini the game will read */

namespace dod3setup {

namespace {

using namespace look;

const char kSep[] = " \xC2\xB7 ";   /* " · ", between the parts of a line */

std::string join(const std::vector<std::string>& parts)
{
    std::string t;
    for (const std::string& p : parts) t += (t.empty() ? "" : kSep) + p;
    return t;
}

/* ---- the settings ------------------------------------------------------------ */

enum Tab { kDisplay, kGraphics, kControls, kSystem, kCheats, kTabs };
const char* const kTabNames[kTabs] = { "Display", "Graphics", "Controls", "System", "Cheats" };

/* The rows only the file has (the in-game pages have the rest). */
void extra_rows(std::vector<Dod3RowSpec>& rows)
{
    rows.push_back({ .key = "RSX_WINDOW",
                     .label = "Window Size",
                     .desc = "The window's size when the display mode is Windowed.",
                     .choices = { { "1280x720", "1280x720" },
                                  { "1600x900", "1600x900" },
                                  { "1920x1080", "1920x1080" },
                                  { "2560x1440", "2560x1440" },
                                  { "3840x2160", "3840x2160" } } });
    rows.push_back({ .key = "DOD3_MOUSE",
                     .label = "Mouse Camera",
                     .desc = "The mouse turns the camera while the game is in front.",
                     .choices = { { nullptr, "On" }, { "0", "Off" } } });
    rows.push_back({ .key = "DOD3_CAMERA_SPEED",
                     .label = "Camera Speed",
                     .desc = "Console keeps the stick's speed at any frame rate.",
                     .choices = { { nullptr, "Console" }, { "original", "Original" } } });
    rows.push_back({ .key = "DOD3_TROPHY_POPUPS",
                     .label = "Trophy Pop-ups",
                     .desc = "Show a trophy at the top right of the game as you earn it.",
                     .choices = { { nullptr, "On" }, { "0", "Off" } } });
    rows.push_back({ .key = "DOD3_LAUNCHER",
                     .label = "Launcher at Start",
#ifdef __linux__
                     .desc = "Open this at start. Off, DOD3_LAUNCHER=1 in the environment opens it anyway.",
#else
                     .desc = "Open this at start. Hold Shift (Option on a Mac) to open it anyway.",
#endif
                     .choices = { { nullptr, "On" }, { "0", "Off" } } });
    /* the Cheats tab (src/patches/cheats.cpp, src/patches/song_autoplay.cpp) */
#ifdef DOD3_FN_GAUGE_SET
    rows.push_back({ .key = "DOD3_CHEAT_HEALTH",
                     .label = "Infinite Health",
                     .desc = "Your health stays full, Zero's or the dragon's. Enemies are hurt as usual.",
                     .choices = { { nullptr, "Off" }, { "1", "On" } } });
    rows.push_back({ .key = "DOD3_CHEAT_INTONER",
                     .label = "Full Intoner Gauge",
                     .desc = "The Intoner gauge fills at the first blood and stays full, in Intoner mode too.",
                     .choices = { { nullptr, "Off" }, { "1", "On" } } });
    rows.push_back({ .key = "DOD3_CHEAT_ITEMS",
                     .label = "Infinite Items",
                     .desc = "Items are not used up.",
                     .choices = { { nullptr, "Off" }, { "1", "On" } } });
    rows.push_back({ .key = "DOD3_CHEAT_TIME",
                     .label = "Frozen Time Limits",
                     .desc = "A mission's time limit does not count down.",
                     .choices = { { nullptr, "Off" }, { "1", "On" } } });
#endif
#ifdef DOD3_FN_SONG_JUDGE
    rows.push_back({ .key = "DOD3_SONG_AUTOPLAY",
                     .label = "Final Song Auto-Play",
                     .desc = "Branch D's final song presses every note for you, on its time.",
                     .choices = { { nullptr, "Off" }, { "1", "On" } } });
#endif
#ifdef _WIN32
    rows.push_back({ .key = "DOD3_UPDATE_CHECK",
                     .label = "Check for Updates",
                     .desc = "Ask GitHub for a newer release when this opens; Update, at the bottom, installs it.",
                     .choices = { { nullptr, "On" }, { "0", "Off" } } });
#endif
}

/* Each row's tab, in the order it is shown there; a row not listed goes on
 * Graphics, after these. */
const struct {
    const char* key;
    Tab tab;
} kPlaces[] = {
    { "RSX_DISPLAY", kDisplay },    { "RSX_WINDOW", kDisplay },      { "RSX_SCALE", kDisplay },
    { "DOD3_ASPECT", kDisplay },    { "DOD3_FPS", kDisplay },        { "RSX_VSYNC", kDisplay },
    { "RSX_AA", kGraphics },        { "RSX_ANISO", kGraphics },      { "DOD3_FOV", kGraphics },
    { "DOD3_PROMPTS", kControls },  { "DOD3_MOUSE", kControls },     { "DOD3_MOUSE_SENS", kControls },
    { "DOD3_CAMERA_SPEED", kControls }, { "DOD3_SKIP_INTRO", kSystem }, { "DOD3_UNFOCUSED", kSystem },
    { "DOD3_TROPHY_POPUPS", kSystem }, { "RSX_BACKEND", kSystem }, { "DOD3_LAUNCHER", kSystem },
    { "DOD3_UPDATE_CHECK", kSystem },
    { "DOD3_CHEAT_HEALTH", kCheats }, { "DOD3_CHEAT_INTONER", kCheats }, { "DOD3_CHEAT_ITEMS", kCheats },
    { "DOD3_CHEAT_TIME", kCheats }, { "DOD3_SONG_AUTOPLAY", kCheats },
};

int place_of(const char* key)
{
    for (size_t i = 0; i < std::size(kPlaces); i++)
        if (!strcmp(kPlaces[i].key, key)) return (int)i;
    return (int)std::size(kPlaces);
}
Tab tab_of(const char* key)
{
    const int i = place_of(key);
    return i < (int)std::size(kPlaces) ? kPlaces[i].tab : kGraphics;
}

/* The keyboard's keys (src/input/kbm.cpp has the same names and defaults). */
struct Bind {
    const char* key;
    const char* label;
    const char* defaults;
};
const Bind kBinds[] = {
    { "KEY_MOVE_UP", "Move Forward", "W" },      { "KEY_MOVE_DOWN", "Move Back", "S" },
    { "KEY_MOVE_LEFT", "Move Left", "A" },       { "KEY_MOVE_RIGHT", "Move Right", "D" },
    { "KEY_CROSS", "Cross", "Space" },           { "KEY_CIRCLE", "Circle", "E Backspace" },
    { "KEY_SQUARE", "Square", "Mouse1" },        { "KEY_TRIANGLE", "Triangle", "Mouse2" },
    { "KEY_L1", "L1", "Q Mouse3" },              { "KEY_R1", "R1", "Shift" },
    { "KEY_L2", "L2", "R" },                     { "KEY_R2", "R2", "F" },
    { "KEY_L3", "L3", "V C" },                   { "KEY_R3", "R3", "V X" },
    { "KEY_START", "Start", "Esc Enter" },       { "KEY_SELECT", "Select", "Tab" },
    { "KEY_UP", "D-Pad Up", "Up 1" },            { "KEY_DOWN", "D-Pad Down", "Down 3" },
    { "KEY_LEFT", "D-Pad Left", "Left 4" },      { "KEY_RIGHT", "D-Pad Right", "Right 2" },
    { "KEY_LOOK_UP", "Camera Up", "none" },      { "KEY_LOOK_DOWN", "Camera Down", "none" },
    { "KEY_LOOK_LEFT", "Camera Left", "WheelUp" }, { "KEY_LOOK_RIGHT", "Camera Right", "WheelDown" },
};

/* An ImGui key as kbm.cpp names it; nullptr if it has no name there. */
const char* key_name(ImGuiKey k)
{
    static char one[8];
    if (k >= ImGuiKey_A && k <= ImGuiKey_Z) {
        one[0] = (char)('A' + (k - ImGuiKey_A));
        one[1] = 0;
        return one;
    }
    if (k >= ImGuiKey_0 && k <= ImGuiKey_9) {
        one[0] = (char)('0' + (k - ImGuiKey_0));
        one[1] = 0;
        return one;
    }
    if (k >= ImGuiKey_F1 && k <= ImGuiKey_F12) {
        snprintf(one, sizeof one, "F%d", 1 + (k - ImGuiKey_F1));
        return one;
    }
    if (k >= ImGuiKey_Keypad0 && k <= ImGuiKey_Keypad9) {
        snprintf(one, sizeof one, "Num%d", (int)(k - ImGuiKey_Keypad0));
        return one;
    }
    switch (k) {
    case ImGuiKey_Space: return "Space";
    case ImGuiKey_Enter:
    case ImGuiKey_KeypadEnter: return "Enter";
    case ImGuiKey_Tab: return "Tab";
    case ImGuiKey_Backspace: return "Backspace";
    case ImGuiKey_LeftShift:
    case ImGuiKey_RightShift: return "Shift";
    case ImGuiKey_LeftCtrl:
    case ImGuiKey_RightCtrl: return "Ctrl";
    case ImGuiKey_LeftAlt:
    case ImGuiKey_RightAlt: return "Alt";
    case ImGuiKey_UpArrow: return "Up";
    case ImGuiKey_DownArrow: return "Down";
    case ImGuiKey_LeftArrow: return "Left";
    case ImGuiKey_RightArrow: return "Right";
    case ImGuiKey_Insert: return "Insert";
    case ImGuiKey_Delete: return "Delete";
    case ImGuiKey_Home: return "Home";
    case ImGuiKey_End: return "End";
    case ImGuiKey_PageUp: return "PageUp";
    case ImGuiKey_PageDown: return "PageDown";
#ifndef __APPLE__
    case ImGuiKey_CapsLock: return "CapsLock";
#endif
    case ImGuiKey_Minus: return "Minus";
    case ImGuiKey_Equal: return "Equals";
    case ImGuiKey_LeftBracket: return "LBracket";
    case ImGuiKey_RightBracket: return "RBracket";
    case ImGuiKey_Semicolon: return "Semicolon";
    case ImGuiKey_Apostrophe: return "Quote";
    case ImGuiKey_Comma: return "Comma";
    case ImGuiKey_Period: return "Period";
    case ImGuiKey_Slash: return "Slash";
    case ImGuiKey_Backslash: return "Backslash";
    case ImGuiKey_GraveAccent: return "Grave";
    default: return nullptr;
    }
}

/* ---- the launcher -------------------------------------------------------------- */

enum class Panel { None, Mods, Dlc, Settings, Trophies, Update };

#ifdef _WIN32
/* A release's notes (GitHub Markdown) as the Update panel shows them: one
 * entry per paragraph, heading or list item; the emphasis and the links'
 * addresses left out. */
struct NoteLine {
    std::string text;
    bool heading = false, item = false;
};
std::vector<NoteLine> note_lines(const std::string& md)
{
    std::vector<NoteLine> out;
    for (size_t at = 0; at < md.size();) {
        size_t e = md.find('\n', at);
        if (e == std::string::npos) e = md.size();
        std::string l = md.substr(at, e - at);
        at = e + 1;
        if (!l.empty() && l.back() == '\r') l.pop_back();
        const size_t s = l.find_first_not_of(" \t");
        if (s == std::string::npos) continue;
        l.erase(0, s);
        NoteLine n;
        if (!l.compare(0, 2, "- ") || !l.compare(0, 2, "* ")) {
            n.item = true;
            l.erase(0, 2);
        } else if (l[0] == '#') {
            n.heading = true;
            l.erase(0, std::min(l.size(), l.find_first_not_of("# ")));
        } else if (l.size() > 4 && !l.compare(0, 2, "**") && l.find("**", 2) == l.size() - 2) {
            n.heading = true;   /* a paragraph that is all bold: the notes' section headings */
        }
        for (size_t i = 0; i < l.size(); i++) {
            if (l[i] == '[') {   /* [text](address): the text */
                const size_t mid = l.find("](", i), end = mid == std::string::npos ? mid : l.find(')', mid);
                if (end != std::string::npos) {
                    n.text += l.substr(i + 1, mid - i - 1);
                    i = end;
                    continue;
                }
            }
            if (l[i] == '*' && i + 1 < l.size() && l[i + 1] == '*') {   /* **bold** */
                i++;
                continue;
            }
            if (l[i] != '`') n.text += l[i];
        }
        out.push_back(n);
    }
    return out;
}
#endif

const float kPanelSlide = 0.25f;   /* seconds a panel takes to come in */
const char kOffNote[] = "Switched off: the game starts without it. Its files are kept, to switch it on again.";
const std::vector<std::pair<std::string, std::string>> kModFilter = {
    { "Drakengard 3 packages and archives (.pkg, .zip)", "*.pkg;*.zip" }, { "All files", "*.*" }
};

struct Launcher : Kit {
    fs::path base, ini;
    Status have;
    ImTextureData* art = nullptr;

    /* the panel open, and the keyboard's or a pad's way in and out of it */
    Panel panel = Panel::None;
    double panel_at = kLongAgo;
    bool focus_panel = false;          /* opened by them: its first item takes the focus */
    Panel refocus = Panel::None;       /* closed by them: its menu entry takes the focus */

    /* adding or removing: the files being recognised, then the plan, then the install */
    enum class Flow { Idle, Review, Working, Done, Failed };
    Flow flow = Flow::Idle;
    FileChecks checks;
    InstallJob job;
    Plan plan;
    std::vector<std::string> notes, removing;
    bool remove_legacy = false;   /* a mod an older setup installed: removing it takes the update */
    struct ModState {
        bool on = true, switchable = false;
    };
    std::map<std::string, ModState> mod_states;   /* each installed mod's */

    /* the settings */
    std::map<std::string, std::string> kv;   /* dod3.ini */
    TrophiesPanel trophies;
    std::vector<Dod3RowSpec> rows;
    int tab = kDisplay;
    int capture = -1, capture_frames = 0;   /* a key binding being set */

#ifdef _WIN32
    /* updates (install/update.h) */
    fs::path prog;                     /* the program's own folder: what an update replaces */
    UpdateCheck upd;
    std::unique_ptr<UpdateJob> ujob;   /* made again for each try */
    bool upd_found = false;
    std::vector<NoteLine> upd_notes;
#endif

    /* tests (the header) */
    struct {
        std::string grab;
        bool quit = false, install = false, update = false;
        std::vector<ImGuiKey> keys;   /* DOD3_LAUNCHER_KEYS */
        size_t next_key = 0;
    } tests;

    bool working() const
    {
#ifdef _WIN32
        if (ujob && ujob->running()) return true;
#endif
        return flow == Flow::Working;
    }
    float panel_in() const { return panel == Panel::None ? 0.0f : ease_out((float)(now - panel_at) / kPanelSlide); }

    void open_panel(Panel p)
    {
        panel = p;
        panel_at = now;
        focus_panel = ImGui::GetIO().NavVisible;
    }

    void take_focus()
    {
        if (!focus_panel) return;
        ImGui::SetKeyboardFocusHere();
        focus_panel = false;
    }

    /* ---- adding and removing ---- */

    void add(const std::vector<fs::path>& paths)
    {
        if (paths.empty()) return;
        notes.clear();
        checks.add(paths);
        flow = Flow::Review;
    }

    void poll_checks()
    {
        for (const FileChecks::Finished& f : checks.take_finished()) {
            if (f.src.kind == SourceKind::Disc || f.src.kind == SourceKind::Eboot)
                notes.push_back(f.name + ": this is the game itself. To install it again, use Game Files.");
            else if (!f.src.set())
                notes.push_back(f.name + ": " + f.err);
            else
                plan_add(plan, f.src);
        }
    }

    bool plan_empty() const
    {
        return plan.dlc.empty() && plan.mods.empty() && !plan.update.set() && plan.remove_ids.empty() &&
               !plan.remove_mods;
    }
    bool remove_blocked() const { return remove_legacy && !plan.update.set(); }

    void start_remove(const std::vector<std::string>& ids, const std::string& name)
    {
        plan = Plan();
        notes.clear();
        removing = { name };
        remove_legacy = false;
        for (const std::string& id : ids) {
            if (mod_removable(base, id))
                plan.remove_ids.push_back(id);
            else
                remove_legacy = true;
        }
        if (remove_legacy) plan.remove_mods = true;   /* with the update, as the setup did */
        flow = Flow::Review;
    }

    void start_install()
    {
        job.start(base, plan);
        flow = Flow::Working;
    }

    void end_flow()
    {
        flow = Flow::Idle;
        plan = Plan();
        notes.clear();
        removing.clear();
        remove_legacy = false;
    }

    /* what is installed, read again */
    void refresh()
    {
        have = installed(base);
        mod_states.clear();
        for (const auto& m : have.mods) mod_states[m.first] = { mod_enabled(base, m.first), mod_switchable(base, m.first) };
    }
    /* are all of these on; can all of them be switched */
    bool all_on(const std::vector<std::string>& ids) const
    {
        for (const std::string& id : ids)
            if (const auto it = mod_states.find(id); it != mod_states.end() && !it->second.on) return false;
        return true;
    }
    bool all_switchable(const std::vector<std::string>& ids) const
    {
        for (const std::string& id : ids)
            if (const auto it = mod_states.find(id); it == mod_states.end() || !it->second.switchable) return false;
        return !ids.empty();
    }

    /* a mod's installed parts switched off or on: their files moved (no install) */
    void switch_mod(const std::vector<std::string>& ids, const std::string& name, bool on)
    {
        std::string err;
        bool moved = true;
        for (const std::string& id : ids) moved = moved && set_mod_enabled(base, id, on, &err);
        refresh();
        say(moved ? name + (on ? " on" : " off") : err);
    }

    /* ---- dod3.ini ---- */

    /* a key set in memory, or with nullptr back to its default */
    void put(const std::string& key, const char* v)
    {
        if (v)
            kv[key] = v;
        else
            kv.erase(key);
    }
    /* the keys written to the file; the toast says `said`, or that it failed */
    void save(const std::vector<std::pair<std::string, const char*>>& keys, const std::string& said = "Saved")
    {
        say(dod3::ini_write(ini, keys) ? said : "Could not save dod3.ini");
    }

    /* ---- the title's side: the menu, the footer ---- */

    std::string mods_summary() const
    {
        std::vector<std::string> parts;
        const std::vector<std::string> es = installed_parts(kSpanish, have);
        if (!es.empty()) parts.push_back(all_on(es) ? "Spanish translation" : "Spanish translation (off)");
        if (has_mod(have, "UNDUB")) parts.push_back("DLC Undub");
        size_t other = 0;
        for (const auto& m : have.mods) other += !known_mod(m.first);
        if (other) parts.push_back(std::to_string(other) + (other == 1 ? " other" : " others"));
        return parts.empty() ? "None installed" : join(parts);
    }

    std::string settings_summary() const
    {
        std::vector<std::string> parts;
        for (const char* key : { "RSX_SCALE", "DOD3_FPS", "RSX_DISPLAY" })
            for (const Dod3RowSpec& r : rows)
                if (!strcmp(r.key, key)) {
                    std::string v = value_text(r);
                    const size_t paren = v.find(" (");
                    if (paren != std::string::npos) v.resize(paren);
                    if (!strcmp(key, "DOD3_FPS") && !v.empty() && v[0] >= '0' && v[0] <= '9') v += " fps";
                    parts.push_back(v);
                }
        return join(parts);
    }

    /* the accent bar of a menu entry, grown in from the middle by `a` */
    void entry_bar(ImDrawList* dl, float x, float cy, float h, float a)
    {
        const float bh = h * 0.8f * a;
        if (a > 0.01f) dl->AddRectFilled(ImVec2(x - px(14), cy - bh / 2), ImVec2(x - px(11), cy + bh / 2), kAccent);
    }

    /* Play, the menu's big first entry, with its mark */
    bool play_item(float y, bool enabled)
    {
        ImDrawList* dl = ImGui::GetWindowDrawList();
        const float x = px(72), h = px(64);
        ImGui::SetCursorScreenPos(ImVec2(x - px(20), y));
        ImGui::BeginDisabled(!enabled);
        const bool pressed = hit("##play", ImVec2(px(420), h)) && cooldown == 0;
        const bool hot = enabled && hot_item();
        ImGui::EndDisabled();
        float& a = anim_of(ImGui::GetItemID());
        approach(a, hot ? 1.0f : 0.0f, 16.0f);
        const ImVec2 ls = text_size(F.display, 34, "Play");
        const float tx = x + px(12) * a, ty = y + (h - ls.y) / 2, cy = ty + ls.y / 2;
        entry_bar(dl, x, cy, ls.y, a);
        const float r = px(9);
        const ImVec2 c(tx + r * 0.6f, cy);
        dl->AddTriangleFilled(ImVec2(c.x - r * 0.55f, c.y - r), ImVec2(c.x - r * 0.55f, c.y + r), ImVec2(c.x + r, c.y),
                              mix(kAccent, kAccentHi, a));
        dl->AddText(F.display, px(34), ImVec2(tx + px(28), ty),
                    enabled ? mix(kText, IM_COL32(255, 255, 255, 255), a) : alpha(kText, 0.3f), "Play");
        return pressed && enabled;
    }

    /* one of the other entries, with a line under it (`sub`); true when chosen */
    bool menu_item(const char* id, const char* label, const char* sub, float y, bool active, bool enabled)
    {
        ImDrawList* dl = ImGui::GetWindowDrawList();
        const float x = px(72), h = px(52);
        ImGui::SetCursorScreenPos(ImVec2(x - px(20), y));
        ImGui::BeginDisabled(!enabled);
        const bool pressed = hit(id, ImVec2(px(420), h)) && cooldown == 0;
        const bool hot = enabled && hot_item();
        ImGui::EndDisabled();
        float& a = anim_of(ImGui::GetItemID());
        approach(a, hot || active ? 1.0f : 0.0f, 16.0f);
        const ImVec2 ls = text_size(F.medium, 22, label);
        const float tx = x + px(12) * a, ty = y + (sub ? px(4) : (h - ls.y) / 2);
        entry_bar(dl, x, ty + ls.y / 2, ls.y, a);
        dl->AddText(F.medium, px(22), ImVec2(tx, ty),
                    enabled ? mix(alpha(kText, 0.72f), kText, a) : alpha(kText, 0.3f), label);
        if (sub && *sub)
            dl->AddText(F.regular, px(13), ImVec2(tx, ty + ls.y + px(1)), alpha(kDim, 0.65f + 0.35f * a), sub);
        return pressed && enabled;
    }

    int menu()
    {
        /* while files are being recognised, leaving would wait for them with the window still */
        const bool busy = working() || checks.pending() > 0;
        float y = H * 0.36f;
        int result = kGoOn;
        if (play_item(y, !busy)) result = kPlay;
        /* Enter, or a pad's confirm button, plays straight away when nothing
         * shows the focus yet (ImGui's first press only shows it) */
        if (!ImGui::GetIO().NavVisible && panel == Panel::None && !busy && capture < 0 && cooldown == 0 &&
            confirm_pressed())
            result = kPlay;
        y += px(84);
        auto open = [&](Panel p) {
            if (panel == p) {   /* its own entry again: closed, unless an install is running in it */
                if (!working()) panel = Panel::None;
                return;
            }
            open_panel(p);
        };
        auto back_here = [&](Panel p) {
            if (refocus != p) return;
            ImGui::SetKeyboardFocusHere();
            refocus = Panel::None;
        };
        back_here(Panel::Mods);
        if (menu_item("##mods", "Mods", mods_summary().c_str(), y, panel == Panel::Mods, true)) open(Panel::Mods);
        y += px(62);
        char dlc[64];
        snprintf(dlc, sizeof dlc, "%zu of %zu installed", have.dlc.size(), known_dlc().size());
        back_here(Panel::Dlc);
        if (menu_item("##dlc", "DLC", dlc, y, panel == Panel::Dlc, true)) open(Panel::Dlc);
        y += px(62);
        back_here(Panel::Settings);
        if (menu_item("##settings", "Settings", settings_summary().c_str(), y, panel == Panel::Settings, true))
            open(Panel::Settings);
        y += px(62);
        back_here(Panel::Trophies);
        if (menu_item("##trophies", "Trophies", trophies.summary().c_str(), y, panel == Panel::Trophies, true))
            open(Panel::Trophies);
        y += px(62);
        if (menu_item("##quit", "Quit", nullptr, y, false, !working())) result = kQuit;
        return result;
    }

    int footer()
    {
        ImDrawList* dl = ImGui::GetWindowDrawList();
        const float y = H - px(52);
        char v[64];
        snprintf(v, sizeof v, "Version %s", DOD3_VERSION_STRING);
        dl->AddText(F.regular, px(13), ImVec2(px(72), y), alpha(kDim, 0.8f), v);
        float x = px(72) + text_size(F.regular, 13, v).x + px(26);
#ifdef _WIN32
        /* a newer release: its Update panel */
        if (upd_found) {
            const std::string t = "Update to " + upd.release.label;
            const ImVec2 us = text_size(F.semibold, 13, t.c_str());
            ImGui::SetCursorScreenPos(ImVec2(x - px(6), y - px(4)));
            const bool pressed = hit("##update", ImVec2(us.x + px(26), us.y + px(8)));
            const bool hot = hot_item();
            dl->AddCircleFilled(ImVec2(x + px(3), y + us.y / 2), px(3.5f), kAccentHi);
            dl->AddText(F.semibold, px(13), ImVec2(x + px(14), y), hot ? kText : kAccentHi, t.c_str());
            if (hot) dl->AddLine(ImVec2(x + px(14), y + us.y + px(1)), ImVec2(x + px(14) + us.x, y + us.y + px(1)), kAccent, px(1.5f));
            if (pressed && cooldown == 0) open_panel(Panel::Update);
            x += us.x + px(14) + px(26);
        }
#endif
        /* Game Files: the installer, for the disc or the update */
        const char* gf = "Game Files";
        const ImVec2 gs = text_size(F.semibold, 13, gf);
        const ImVec2 gp(x, y);
        ImGui::SetCursorScreenPos(ImVec2(gp.x - px(6), gp.y - px(4)));
        const bool busy = working() || checks.pending() > 0;
        ImGui::BeginDisabled(busy);
        const bool pressed = hit("##gamefiles", ImVec2(gs.x + px(12), gs.y + px(8)));
        const bool hot = hot_item();
        ImGui::EndDisabled();
        dl->AddText(F.semibold, px(13), gp, busy ? alpha(kDim, 0.4f) : hot ? kText : alpha(kDim, 0.9f), gf);
        if (hot) dl->AddLine(ImVec2(gp.x, gp.y + gs.y + px(1)), ImVec2(gp.x + gs.x, gp.y + gs.y + px(1)), kAccent, px(1.5f));
        return pressed && cooldown == 0 && !busy ? kGameFiles : kGoOn;
    }

    /* ---- the Mods and DLC panels ---- */

    /* the switch at a card's top right, for the mod's installed parts `ids`;
     * false if they cannot be switched (the undub, or installed without a list) */
    bool card_switch(const std::vector<std::string>& ids, const std::string& name, float right, float y)
    {
        if (!all_switchable(ids)) return false;
        const bool on = all_on(ids);
        const ImVec2 back = ImGui::GetCursorScreenPos(), ts = toggle_size();
        ImGui::SetCursorScreenPos(ImVec2(right - ts.x, y));
        ImGui::PushID(name.c_str());
        if (toggle("##on", on)) switch_mod(ids, name, !on);
        ImGui::PopID();
        const char* t = on ? "On" : "Off";
        const ImVec2 ls = text_size(F.semibold, 13, t);
        ImGui::GetWindowDrawList()->AddText(F.semibold, px(13), ImVec2(right - ts.x - px(10) - ls.x, y + (ts.y - ls.y) / 2),
                                            on ? kText : kDim, t);
        ImGui::SetCursorScreenPos(back);
        return true;
    }

    void mod_card(const ModGroup& g)
    {
        const std::vector<std::string> ids = installed_parts(g, have);
        const int in = (int)ids.size();
        const bool on = all_on(ids);
        card_begin();
        ImDrawList* dl = ImGui::GetWindowDrawList();
        const float right = card_x1 - px(18);
        label(F.semibold, 19, kText, g.title);
        const ImVec2 tp = ImGui::GetItemRectMin();
        if (!(in > 0 && card_switch(ids, g.title, right, tp.y - px(1)))) {
            if (in == (int)g.parts.size())
                pill(dl, ImVec2(right, tp.y + px(2)), "Installed", kOk);
            else if (in > 0)
                pill(dl, ImVec2(right, tp.y + px(2)), "Partly installed", kWarn);
            else
                pill(dl, ImVec2(right, tp.y + px(2)), "Not installed", kFaint);
        }
        para(F.medium, 13, kDim, g.by);
        ImGui::Dummy(ImVec2(0, px(4)));
        para(F.regular, 14, alpha(kText, 0.82f), g.about);
        if (in > 0 && !on) {
            ImGui::Dummy(ImVec2(0, px(4)));
            para(F.medium, 13, kWarn, kOffNote);
        }
        if (g.parts.size() > 1 || in < (int)g.parts.size()) {
            ImGui::Dummy(ImVec2(0, px(4)));
            for (const ModPart& p : g.parts) {
                const bool part_in = has_mod(have, p.id);
                list_row(part_in ? kTickMark : kRingMark, part_in && on ? kOk : kFaint, p.name,
                         part_in && on ? kText : kDim, p.needs);
            }
        }
        if (&g == &kUndub && !have.dlc.count("ADDJAPANESEVOICE")) {
            ImGui::Dummy(ImVec2(0, px(4)));
            para(F.medium, 13, kWarn, "Needs the Japanese Voice Pack: add its package on the DLC page first.");
        }
        if (in > 0) {
            ImGui::Dummy(ImVec2(0, px(8)));
            ImGui::PushID(g.title);
            if (button("Remove", 0, ButtonKind::Danger)) start_remove(ids, g.title);
            ImGui::PopID();
        }
        card_end();
    }

    /* a mod the setup does not know by name: a homebrew package */
    void other_mod_card(const std::string& id, const std::string& name)
    {
        card_begin();
        label(F.semibold, 17, kText, name.c_str());
        card_switch({ id }, name, card_x1 - px(18), ImGui::GetItemRectMin().y - px(2));
        para(F.regular, 13, kDim,
             all_on({ id }) ? "Installed from a homebrew package laid out as the console installs it." : kOffNote);
        ImGui::Dummy(ImVec2(0, px(6)));
        ImGui::PushID(id.c_str());
        if (button("Remove", 0, ButtonKind::Danger)) start_remove({ id }, name);
        ImGui::PopID();
        card_end();
    }

    /* the foot of a panel: the Add buttons and a line of text */
    void panel_foot(ImVec2 body, ImVec2 size, const char* note, Panel p)
    {
        const float y = body.y + size.y - px(44);
        ImDrawList* dl = ImGui::GetWindowDrawList();
        dl->AddLine(ImVec2(body.x, y - px(16)), ImVec2(body.x + size.x, y - px(16)), kLine);
        ImGui::SetCursorScreenPos(ImVec2(body.x, y));
        take_focus();
        add_buttons(p);
        const float bx = ImGui::GetItemRectMax().x + px(18);
        dl->AddText(F.regular, px(13), ImVec2(bx, y + px(4)), kDim, note, nullptr, body.x + size.x - bx);
    }

    /* Add Files / Add Folder on the Mods panel; Add DLC Packages on the DLC one */
    void add_buttons(Panel p)
    {
        const bool mods = p == Panel::Mods;
        if (button(mods ? "Add Files..." : "Add DLC Packages...", 0, ButtonKind::Main))
            add(ui_pick_files(mods ? "Add mods" : "Add DLC packages", kModFilter));
        if (mods) {
            ImGui::SameLine(0, px(10));
            if (button("Add Folder...", 0)) {
                const fs::path f = ui_pick_folder("A mod's folder (the DLC Undub's holds GAME and ISO)");
                if (!f.empty()) add({ f });
            }
        }
    }

    void mods_panel(ImVec2 body, ImVec2 size)
    {
        panel_foot(body, size, "Packages (.pkg), a movie archive (.zip) or a mod's folder, as downloaded. Your own "
                               "game files are never changed.", Panel::Mods);
        ImGui::SetCursorScreenPos(body);
        ImGui::BeginChild("##mods", ImVec2(size.x, size.y - px(64)), ImGuiChildFlags_NavFlattened,
                          ImGuiWindowFlags_NoBackground);
        mod_card(kSpanish);
        mod_card(kUndub);
        for (const auto& m : have.mods)
            if (!known_mod(m.first)) other_mod_card(m.first, m.second);
        ImGui::EndChild();
    }

    void dlc_panel(ImVec2 body, ImVec2 size)
    {
        panel_foot(body, size, "The PSN packages you own (UP0082-NPUB31251_00-...). No licence files are needed.",
                   Panel::Dlc);
        ImGui::SetCursorScreenPos(body);
        ImGui::BeginChild("##dlc", ImVec2(size.x, size.y - px(64)), ImGuiChildFlags_NavFlattened,
                          ImGuiWindowFlags_NoBackground);
        struct Group {
            const char* title;
            const char* prefix;
        };
        const Group groups[] = { { "Story", "ADDSCENARIO" },
                                 { "Voices", "ADDJAPANESEVOICE" },
                                 { "Music", "ADDSONG" },
                                 { "Outfits and Weapons", "ADDWEAPONCLOTHE" } };
        ImDrawList* dl = ImGui::GetWindowDrawList();
        const float cw = (ImGui::GetContentRegionAvail().x - px(12)) / 2;
        for (const Group& g : groups) {
            label(F.semibold, 13, kDim, g.title);
            ImGui::Dummy(ImVec2(0, px(2)));
            int col = 0;
            for (const DlcInfo& d : known_dlc()) {
                if (strncmp(d.id, g.prefix, strlen(g.prefix))) continue;
                const bool in = have.dlc.count(d.id) != 0;
                if (col) ImGui::SameLine(0, px(12));
                const ImVec2 p = ImGui::GetCursorScreenPos(), sz(cw, px(44));
                ImGui::Dummy(sz);
                dl->AddRectFilled(p, p + sz, in ? IM_COL32(255, 255, 255, 12) : IM_COL32(255, 255, 255, 5), px(10));
                if (in)
                    tick(dl, ImVec2(p.x + px(18), p.y + sz.y / 2), kOk);
                else
                    ring(dl, ImVec2(p.x + px(18), p.y + sz.y / 2), kFaint);
                const ImVec2 ts = text_size(F.medium, 14, d.name);
                dl->AddText(F.medium, px(14), ImVec2(p.x + px(36), p.y + (sz.y - ts.y) / 2), in ? kText : kDim, d.name);
                col ^= 1;
            }
            ImGui::Dummy(ImVec2(0, px(10)));
        }
        para(F.regular, 13, kFaint,
             "The Japanese Voice Pack brings Japanese voices to the main story (Change Outfits/Music > Voice), and "
             "with the DLC Undub to the DLC chapters too.");
        ImGui::EndChild();
    }

    /* ---- adding or removing, in place of the panel's own page ---- */

    void review_page()
    {
        label(F.semibold, 19, kText, !removing.empty() ? "Remove" : "Ready to Install");
        ImGui::Dummy(ImVec2(0, px(6)));
        auto row = [&](const std::string& name, uint64_t bytes, ImU32 col) {
            list_row(kDotMark, col, name.c_str(), kText, bytes ? size_text(bytes).c_str() : nullptr, kFaint, F.medium, 15);
            ImGui::Dummy(ImVec2(0, px(2)));
        };
        for (const std::string& r : removing) row(r, 0, kErr);
        if (plan.update.set()) row("Update 1.01", plan.update.bytes, kOk);
        for (const Source& d : plan.dlc) row(d.name, d.bytes, kOk);
        for (const Source& m : plan.mods) row(m.name, m.bytes, kOk);
        checking(checks.pending());
        ImGui::Dummy(ImVec2(0, px(6)));
        for (const std::string& n : notes) para(F.regular, 14, kErr, n.c_str());
        if (needs_voice_pack(plan, have))
            para(F.medium, 14, kWarn,
                 "The DLC Undub needs the Japanese Voice Pack: add its package too (or install it first).");
        if (remove_blocked())
            para(F.medium, 14, kWarn,
                 "An older version of the setup installed this mod, without a list of its files. Add the 1.01 "
                 "update package (Add Files) to remove it: the update puts back the files the mod replaced, "
                 "and all the mods are removed.");
        if (remove_legacy && plan.update.set())
            para(F.regular, 14, kDim, "All the installed mods are removed, and the update installed again.");
        const uint64_t need = plan.bytes(), space = free_bytes(base);
        if (need) para(F.regular, 13, kFaint, ("Needs " + size_text(need) + "; " + size_text(space) + " free.").c_str());
        const bool full = space && need > space;
        if (full) para(F.medium, 14, kErr, "Not enough space on the drive.");
        ImGui::Dummy(ImVec2(0, px(14)));
        const bool can = !checks.pending() && !plan_empty() && !needs_voice_pack(plan, have) && !remove_blocked() && !full;
        take_focus();
        if (button(!removing.empty() ? "Remove" : "Install", 140, !removing.empty() ? ButtonKind::Danger : ButtonKind::Main, can) ||
            (tests.install && can && frames > 60))
            start_install();
        ImGui::SameLine(0, px(10));
        if (button("Add Files...", 0, ButtonKind::Quiet, !checks.pending())) add(ui_pick_files("Add files", kModFilter));
        ImGui::SameLine(0, px(10));
        if (button("Cancel", 0, ButtonKind::Quiet, !checks.pending())) end_flow();
    }

    void working_page()
    {
        install_progress(job.what(), job.done, job.total);
        ImGui::Dummy(ImVec2(0, px(14)));
        if (button(job.cancel ? "Cancelling..." : "Cancel", 0, ButtonKind::Quiet, !job.cancel)) job.cancel = true;
    }

    void result_page()
    {
        const bool good = flow == Flow::Done;
        label(F.semibold, 19, kText,
              good ? (!removing.empty() ? "Removed" : "Installed") : job.cancel ? "Cancelled" : "Failed");
        ImGui::Dummy(ImVec2(0, px(6)));
        if (!good && !job.cancel) para(F.regular, 14, kErr, job.error.c_str());
        if (!good)
            para(F.regular, 14, kDim, "The part that was being installed has been taken out again; the parts before it "
                                      "stay.");
        ImGui::Dummy(ImVec2(0, px(14)));
        if (button("Done", 120, ButtonKind::Main)) end_flow();
    }

    void flow_page(ImVec2 body, ImVec2 size)
    {
        ImGui::SetCursorScreenPos(body);
        ImGui::BeginChild("##flow", size, ImGuiChildFlags_NavFlattened, ImGuiWindowFlags_NoBackground);
        if (flow == Flow::Review)
            review_page();
        else if (flow == Flow::Working)
            working_page();
        else
            result_page();
        ImGui::EndChild();
    }

#ifdef _WIN32
    /* ---- the Update panel ---- */

    void start_update()
    {
        ujob = std::make_unique<UpdateJob>();
        ujob->start(prog, upd.release);
    }

    /* the check's answer, and the install's end: a finished install starts
     * the new dod3.exe and closes this one (*result) */
    void poll_update(int* result)
    {
        if (upd.collect() && upd.found) {
            upd_found = true;
            upd_notes = note_lines(upd.release.notes);
            say(upd.release.label + " is out: Update, at the bottom");
            if (tests.update) {
                open_panel(Panel::Update);
                start_update();
            }
        }
        if (ujob && ujob->collect()) {
            if (ujob->ok && tests.update) {   /* the new one closes by itself, without a check, its window grabbed apart */
                _putenv_s("DOD3_LAUNCHER_AUTO", "quit");
                _putenv_s("DOD3_UPDATE_CHECK", "0");
                if (!tests.grab.empty()) _putenv_s("DOD3_SETUP_GRAB", (tests.grab + "_after").c_str());
            }
            if (ujob->ok && update_restart(prog))
                *result = kQuit;
            else if (tests.update) {
                fprintf(stderr, "[launcher] update %s\n", ujob->ok ? "installed, not restarted" : ujob->error.c_str());
                *result = kQuit;
            }
        }
    }

    void update_panel(ImVec2 body, ImVec2 size)
    {
        const Release& r = upd.release;
        if (ujob && (ujob->running() || ujob->ok)) {
            ImGui::SetCursorScreenPos(body);
            ImGui::BeginChild("##updating", size, ImGuiChildFlags_NavFlattened, ImGuiWindowFlags_NoBackground);
            if (ujob->ok) {   /* installed, but the new one could not be started */
                label(F.semibold, 19, kText, "Installed");
                ImGui::Dummy(ImVec2(0, px(6)));
                para(F.regular, 14, kDim, ("Close the launcher and start dod3.exe again to play " + r.label + ".").c_str());
            } else {
                const int step = ujob->step;
                const uint64_t done = ujob->done, total = ujob->total;
                label(F.semibold, 19, kText, (ujob->what() + "...").c_str());
                ImGui::Dummy(ImVec2(0, px(14)));
                progress(total ? (float)((double)done / (double)total) : 0.0f);
                ImGui::Dummy(ImVec2(0, px(8)));
                para(F.regular, 13, kDim,
                     step == UpdateJob::kInstalling ? (std::to_string(done) + " of " + std::to_string(total) + " files").c_str()
                     : step == UpdateJob::kChecking ? "Its SHA-256 against the one GitHub lists"
                                                     : (size_text(done) + " of " + size_text(total)).c_str());
                ImGui::Dummy(ImVec2(0, px(14)));
                if (step < UpdateJob::kInstalling) {
                    take_focus();
                    if (button(ujob->cancel ? "Cancelling..." : "Cancel", 0, ButtonKind::Quiet, !ujob->cancel))
                        ujob->cancel = true;
                } else {
                    para(F.regular, 13, kFaint, "The launcher starts again when it is done.");
                }
            }
            ImGui::EndChild();
            return;
        }
        /* the offer, with a failed try's reason */
        const float y = body.y + size.y - px(44);
        ImDrawList* dl = ImGui::GetWindowDrawList();
        dl->AddLine(ImVec2(body.x, y - px(16)), ImVec2(body.x + size.x, y - px(16)), kLine);
        ImGui::SetCursorScreenPos(ImVec2(body.x, y));
        take_focus();
        if (button("Install Update", 0, ButtonKind::Main)) start_update();
        ImGui::SameLine(0, px(10));
        if (button("Release Page", 0)) open_in_browser(r.page);
        const float bx = ImGui::GetItemRectMax().x + px(18);
        dl->AddText(F.regular, px(13), ImVec2(bx, y + px(4)), kDim,
                    "Your game, saves and settings stay as they are. The launcher starts again when it is done.",
                    nullptr, body.x + size.x - bx);
        ImGui::SetCursorScreenPos(body);
        ImGui::BeginChild("##update", ImVec2(size.x, size.y - px(64)), ImGuiChildFlags_NavFlattened,
                          ImGuiWindowFlags_NoBackground);
        label(F.semibold, 19, kText, ("Drakengard 3 Recompiled " + r.label).c_str());
        std::vector<std::string> about;
        if (!r.date.empty()) about.push_back("Released " + r.date);
        if (r.size) about.push_back(size_text(r.size) + " download");
        about.push_back("you have " + std::string(DOD3_VERSION_STRING));
        para(F.regular, 13, kDim, join(about).c_str());
        if (ujob && !ujob->error.empty()) {
            ImGui::Dummy(ImVec2(0, px(6)));
            para(F.medium, 14, ujob->cancel ? kDim : kErr, ujob->error.c_str());
            if (ujob->changed)
                para(F.regular, 13, kWarn, "Unzip the release from its page into this folder to finish.");
        }
        ImGui::Dummy(ImVec2(0, px(10)));
        for (const NoteLine& n : upd_notes) {
            if (n.heading) {
                ImGui::Dummy(ImVec2(0, px(6)));
                label(F.semibold, 15, kText, n.text.c_str());
            } else {
                para(F.regular, 13, n.item ? alpha(kText, 0.82f) : kDim, (n.item ? "\xE2\x80\xA2  " + n.text : n.text).c_str());
            }
            ImGui::Dummy(ImVec2(0, px(3)));
        }
        ImGui::EndChild();
    }
#endif

    /* ---- the Settings panel ---- */

    int current(const Dod3RowSpec& r, std::string* other = nullptr) const
    {
        const auto it = kv.find(r.key);
        const char* v = it == kv.end() ? nullptr : it->second.c_str();
        for (size_t i = 0; i < r.choices.size(); i++) {
            const char* c = r.choices[i].first;
            if (!v || !*v) {
                if (!c) return (int)i;
                continue;
            }
            if (c && !strcmp(c, v)) return (int)i;
            if (c && !strcmp(r.key, "RSX_SCALE") && atof(c) == atof(v)) return (int)i;
        }
        if (!v || !*v) return r.def;
        if (other) *other = v;
        return -1;
    }
    std::string value_text(const Dod3RowSpec& r) const
    {
        std::string other;
        const int i = current(r, &other);
        return i >= 0 ? r.choices[i].second : other;
    }

    void set_value(const Dod3RowSpec& r, int i)
    {
        put(r.key, r.choices[i].first);
        save({ { r.key, r.choices[i].first } });
    }

    /* Left or Right for the focused row `id`: -1, 1, or 0 for neither. The row
     * owns the keys meanwhile, so ImGui's navigation (which reads only keys
     * nobody owns) does not move the focus away. */
    int step_dir(ImGuiID id)
    {
        static const ImGuiKey less[] = { ImGuiKey_LeftArrow, ImGuiKey_GamepadDpadLeft, ImGuiKey_GamepadLStickLeft };
        static const ImGuiKey more[] = { ImGuiKey_RightArrow, ImGuiKey_GamepadDpadRight, ImGuiKey_GamepadLStickRight };
        for (ImGuiKey k : less) ImGui::SetKeyOwner(k, id);
        for (ImGuiKey k : more) ImGui::SetKeyOwner(k, id);
        for (ImGuiKey k : less)
            if (ImGui::IsKeyPressed(k, ImGuiInputFlags_Repeat, id)) return -1;
        for (ImGuiKey k : more)
            if (ImGui::IsKeyPressed(k, ImGuiInputFlags_Repeat, id)) return 1;
        return 0;
    }

    void setting_row(const Dod3RowSpec& r)
    {
        ImGui::PushID(r.key);
        ImDrawList* dl = ImGui::GetWindowDrawList();
        const float w = ImGui::GetContentRegionAvail().x;
        const float sw = px(250);   /* the value's box, at the right */
        const ImVec2 p = ImGui::GetCursorScreenPos();
        const float sx = p.x + w - sw - px(10), dw = sx - p.x - px(34);
        const ImVec2 ds = text_size(F.regular, 13, r.desc, dw);
        const float h = std::max(px(62), px(33) + ds.y + px(13));
        hit("##row", ImVec2(w, h));
        const bool hot = hot_item();
        float& a = anim_of(ImGui::GetItemID());
        approach(a, hot ? 1.0f : 0.0f);
        int dir = 0;
        if (ImGui::IsItemClicked() && cooldown == 0) {   /* the left arrow steps back, anywhere else on */
            const float mx = ImGui::GetIO().MousePos.x;
            dir = mx >= sx && mx < sx + px(40) ? -1 : 1;
        }
        if (!dir && ImGui::IsItemFocused() && cooldown == 0) {
            dir = step_dir(ImGui::GetItemID());
            if (!dir && ImGui::IsItemActivated() && !ImGui::IsMouseClicked(0)) dir = 1;   /* Enter, Space, a pad's confirm */
        }
        dl->AddRectFilled(p, p + ImVec2(w, h), IM_COL32(255, 255, 255, (int)(10 * a)), px(10));
        dl->AddText(F.semibold, px(15), ImVec2(p.x + px(14), p.y + px(11)), kText, r.label);
        dl->AddText(F.regular, px(13), ImVec2(p.x + px(14), p.y + px(33)), kDim, r.desc, nullptr, dw);
        /* the value between two arrows */
        std::string other;
        const int cur = current(r, &other);
        const std::string vt = cur >= 0 ? r.choices[cur].second : "Custom (" + other + ")";
        const float cy = p.y + h / 2;
        const ImVec2 b0(sx, cy - px(17)), b1(sx + sw, cy + px(17));
        dl->AddRectFilled(b0, b1, IM_COL32(255, 255, 255, (int)(12 + 10 * a)), px(9));
        const ImVec2 vs = text_size(F.medium, 15, vt.c_str());
        dl->AddText(F.medium, px(15), ImVec2(sx + (sw - vs.x) / 2, cy - vs.y / 2), kText, vt.c_str());
        auto chevron = [&](float x, int d) {   /* pointing the way d steps */
            const float r2 = px(4.5f);
            const ImVec2 pts[3] = { ImVec2(x - r2 * 0.5f * d, cy - r2), ImVec2(x + r2 * 0.5f * d, cy),
                                    ImVec2(x - r2 * 0.5f * d, cy + r2) };
            dl->AddPolyline(pts, 3, alpha(kText, 0.45f + 0.5f * a), 0, px(1.8f));
        };
        chevron(sx + px(20), -1);
        chevron(sx + sw - px(20), 1);
        /* the dots: where the value is among the choices */
        if (r.choices.size() > 1 && r.choices.size() <= 10) {
            const float gap = px(7), n = (float)r.choices.size();
            const float x0 = sx + sw / 2 - (n - 1) * gap / 2;
            for (size_t i = 0; i < r.choices.size(); i++)
                dl->AddCircleFilled(ImVec2(x0 + gap * (float)i, b1.y + px(6)), px(1.6f),
                                    (int)i == cur ? kAccentHi : IM_COL32(255, 255, 255, 40));
        }
        if (dir && r.choices.size() > 1) {
            const int n = (int)r.choices.size();
            set_value(r, cur < 0 ? (dir > 0 ? 0 : n - 1) : (cur + dir + n) % n);
        }
        ImGui::PopID();
    }

    std::string bind_value(const Bind& b) const
    {
        const auto it = kv.find(b.key);
        return it == kv.end() ? b.defaults : it->second;
    }

    void bind_row(int i)
    {
        const float kResetW = 84, kGap = 12;   /* the Reset button at the row's end, and the gap before it */
        const Bind& b = kBinds[i];
        ImGui::PushID(b.key);
        ImDrawList* dl = ImGui::GetWindowDrawList();
        const float w = ImGui::GetContentRegionAvail().x - px(kResetW + kGap), h = px(44);
        const ImVec2 p = ImGui::GetCursorScreenPos();
        const bool pressed = hit("##bind", ImVec2(w, h));
        const bool hot = hot_item();
        float& a = anim_of(ImGui::GetItemID());
        approach(a, hot || capture == i ? 1.0f : 0.0f);
        if (pressed && cooldown == 0 && capture < 0) {   /* the next key pressed is the binding (poll_capture) */
            capture = i;
            capture_frames = 0;
            ImGui::GetIO().ConfigFlags &= ~(ImGuiConfigFlags_NavEnableKeyboard | ImGuiConfigFlags_NavEnableGamepad);
        }
        dl->AddRectFilled(p, p + ImVec2(w, h), IM_COL32(255, 255, 255, (int)(9 * a)), px(9));
        if (capture == i) dl->AddRect(p, p + ImVec2(w, h), alpha(kAccentHi, 0.9f), px(9), 0, px(1.5f));
        const ImVec2 ls = text_size(F.medium, 14, b.label);
        dl->AddText(F.medium, px(14), ImVec2(p.x + px(14), p.y + (h - ls.y) / 2), kText, b.label);
        /* the keys, as caps */
        float x = p.x + px(190);
        if (capture == i) {
            const char* t = "Press a key or a mouse button  \xC2\xB7  Esc cancels";
            const ImVec2 ts = text_size(F.medium, 13, t);
            dl->AddText(F.medium, px(13), ImVec2(x, p.y + (h - ts.y) / 2), kAccentHi, t);
        } else {
            const std::string v = bind_value(b);
            bool any = false;
            for (size_t at = 0; at < v.size();) {
                size_t e = v.find(' ', at);
                if (e == std::string::npos) e = v.size();
                const std::string k = v.substr(at, e - at);
                at = e + 1;
                if (k.empty() || k == "none") continue;
                any = true;
                const ImVec2 ts = text_size(F.semibold, 13, k.c_str());
                const ImVec2 c0(x, p.y + h / 2 - px(13)), c1(x + ts.x + px(20), p.y + h / 2 + px(13));
                dl->AddRectFilled(c0, c1, IM_COL32(255, 255, 255, 22), px(6));
                dl->AddRect(c0, c1, IM_COL32(255, 255, 255, 30), px(6));
                dl->AddText(F.semibold, px(13), ImVec2(x + px(10), p.y + (h - ts.y) / 2), kText, k.c_str());
                x = c1.x + px(8);
            }
            if (!any) dl->AddText(F.regular, px(13), ImVec2(x, p.y + (h - ls.y) / 2), kFaint, "Not set");
        }
        /* Reset, when it is not the default */
        ImGui::SameLine(0, px(kGap));
        const bool custom = kv.count(b.key) != 0;
        const ImVec2 rp = ImGui::GetCursorScreenPos();
        ImGui::BeginDisabled(!custom);
        const bool reset = hit("##reset", ImVec2(px(kResetW), h));
        const bool rh = hot_item();
        ImGui::EndDisabled();
        if (custom) {
            const ImVec2 ts = text_size(F.semibold, 13, "Reset");
            dl->AddText(F.semibold, px(13), ImVec2(rp.x + (px(kResetW) - ts.x) / 2, rp.y + (h - ts.y) / 2),
                        rh ? kText : kDim, "Reset");
        }
        if (reset && custom && cooldown == 0) {
            put(b.key, nullptr);
            save({ { b.key, nullptr } });
        }
        ImGui::PopID();
    }

    /* the key binding being set: the first key or button pressed */
    void poll_capture()
    {
        if (capture < 0) return;
        if (++capture_frames < 3) return;   /* not the click or key that started it */
        ImGuiIO& io = ImGui::GetIO();
        const char* name = nullptr;
        const bool stop = back_pressed();
        for (int k = ImGuiKey_NamedKey_BEGIN; k < ImGuiKey_NamedKey_END && !name && !stop; k++)
            if (ImGui::IsKeyPressed((ImGuiKey)k, false)) name = key_name((ImGuiKey)k);
        static const char* const mouse[] = { "Mouse1", "Mouse2", "Mouse3", "Mouse4", "Mouse5" };
        for (int m = 0; m < 5 && !name && !stop; m++)
            if (ImGui::IsMouseClicked(m)) name = mouse[m];
        if (!name && !stop && io.MouseWheel > 0) name = "WheelUp";
        if (!name && !stop && io.MouseWheel < 0) name = "WheelDown";
        if (name) {
            const Bind& b = kBinds[capture];
            put(b.key, name);
            save({ { b.key, name } }, std::string(b.label) + ": " + name);
        }
        if (name || stop) {
            capture = -1;
            cooldown = 6;   /* the key that ended it does not also press what is under it */
            io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard | ImGuiConfigFlags_NavEnableGamepad;
        }
    }

    void settings_panel(ImVec2 body, ImVec2 size)
    {
        ImDrawList* dl = ImGui::GetWindowDrawList();
        /* the tabs: L1 / R1 move between them too */
        if (capture < 0) {
            if (ImGui::IsKeyPressed(ImGuiKey_GamepadL1)) tab = (tab + kTabs - 1) % kTabs;
            if (ImGui::IsKeyPressed(ImGuiKey_GamepadR1)) tab = (tab + 1) % kTabs;
        }
        float x = body.x;
        for (int t = 0; t < kTabs; t++) {
            const ImVec2 ts = text_size(F.semibold, 14, kTabNames[t]);
            const ImVec2 p(x, body.y), sz(ts.x + px(32), px(36));
            ImGui::SetCursorScreenPos(p);
            ImGui::PushID(t);
            if (hit("##tab", sz) && cooldown == 0 && capture < 0) tab = t;
            const bool hot = hot_item();
            ImGui::PopID();
            if (t == tab)
                dl->AddRectFilled(p, p + sz, IM_COL32(255, 255, 255, 30), px(18));
            else if (hot)
                dl->AddRectFilled(p, p + sz, IM_COL32(255, 255, 255, 12), px(18));
            dl->AddText(F.semibold, px(14), ImVec2(p.x + px(16), p.y + (sz.y - ts.y) / 2), t == tab ? kText : kDim,
                        kTabNames[t]);
            x += sz.x + px(6);
        }
        const float top = px(52);
        ImGui::SetCursorScreenPos(ImVec2(body.x, body.y + top));
        ImGui::BeginChild("##settings", ImVec2(size.x, size.y - top), ImGuiChildFlags_NavFlattened,
                          ImGuiWindowFlags_NoBackground);
        std::vector<const Dod3RowSpec*> list;
        for (const Dod3RowSpec& r : rows)
            if (tab_of(r.key) == tab && r.choices.size() > 1) list.push_back(&r);
        std::stable_sort(list.begin(), list.end(),
                         [](const Dod3RowSpec* a, const Dod3RowSpec* b) { return place_of(a->key) < place_of(b->key); });
        take_focus();
        for (const Dod3RowSpec* r : list) setting_row(*r);
        if (tab == kControls) {
            ImGui::Dummy(ImVec2(0, px(14)));
            label(F.semibold, 13, kDim, "KEYBOARD");
            para(F.regular, 13, kFaint,
                 "Click a row, then press the key or mouse button for it. The game's Controller Settings page says "
                 "what each button does.");
            ImGui::Dummy(ImVec2(0, px(6)));
            for (int i = 0; i < (int)std::size(kBinds); i++) bind_row(i);
        }
        ImGui::Dummy(ImVec2(0, px(14)));
        if (button("Restore Defaults", 0, ButtonKind::Quiet, capture < 0)) {
            std::vector<std::pair<std::string, const char*>> reset;
            for (const Dod3RowSpec* r : list) {
                put(r->key, r->choices[r->def].first);
                reset.push_back({ r->key, r->choices[r->def].first });
            }
            if (tab == kControls)
                for (const Bind& b : kBinds) {
                    put(b.key, nullptr);
                    reset.push_back({ b.key, nullptr });
                }
            save(reset, "Defaults restored");
        }
        ImGui::Dummy(ImVec2(0, px(8)));
        ImGui::EndChild();
    }

    /* ---- the frame ---- */

    int frame()
    {
        begin_frame();
        test_keys();
        poll_checks();
        poll_capture();
        if (job.collect()) {
            refresh();
            flow = job.ok ? Flow::Done : Flow::Failed;
            if (job.ok) say(!removing.empty() ? "Removed" : "Installed");
        }

        int restart = kGoOn;
#ifdef _WIN32
        poll_update(&restart);
#endif

        backdrop(art, 1.0f, panel_in());
        begin_window("##launcher");
        title();
        int result = menu();
        if (const int f = footer(); f != kGoOn) result = f;
        if (restart != kGoOn) result = restart;

        if (panel != Panel::None) {
            ImVec2 body, size;
            const char* heading = panel == Panel::Mods       ? "Mods"
                                  : panel == Panel::Dlc      ? "DLC"
                                  : panel == Panel::Trophies ? "Trophies"
                                  : panel == Panel::Update   ? "Update"
                                                             : "Settings";
            const char* sub = panel == Panel::Mods     ? "Fan-made additions, installed beside your own game files."
                              : panel == Panel::Dlc    ? "The downloadable content you own."
                              : panel == Panel::Trophies ? "Earned as you play; kept with your saves."
                              : panel == Panel::Update ? "A newer release of the port, from its GitHub page."
                                                       : "Saved to dod3.ini as you change them; used at the next start.";
            const bool close = panel_frame(heading, sub, panel_in(), true, &body, &size);
            if (panel == Panel::Settings)
                settings_panel(body, size);
            else if (panel == Panel::Trophies) {
                trophies.draw(*this, body, size, focus_panel);
                focus_panel = false;
            }
#ifdef _WIN32
            else if (panel == Panel::Update)
                update_panel(body, size);
#endif
            else if (flow != Flow::Idle)
                flow_page(body, size);
            else if (panel == Panel::Mods)
                mods_panel(body, size);
            else
                dlc_panel(body, size);
            const bool back = back_pressed() && capture < 0 && cooldown == 0 && !trophies.typing();
            if ((close || back) && !working()) {
                if (ImGui::GetIO().NavVisible) refocus = panel;
                panel = Panel::None;
                focus_panel = false;
            }
        }
        ImGui::End();
        toast_draw();
        return test_step(result);
    }

    /* ---- tests (the header) ---- */

    void read_test_env()
    {
        if (const char* g = getenv("DOD3_SETUP_GRAB")) tests.grab = g;
        if (const char* a = getenv("DOD3_LAUNCHER_AUTO")) {
            tests.quit = !strcmp(a, "quit");
            tests.install = !strcmp(a, "install");
            tests.update = !strcmp(a, "update");
        }
        if (const char* o = getenv("DOD3_LAUNCHER_OPEN")) {
            panel = !strncmp(o, "mods", 4)       ? Panel::Mods
                    : !strncmp(o, "dlc", 3)      ? Panel::Dlc
                    : !strncmp(o, "settings", 8) ? Panel::Settings
                    : !strncmp(o, "trophies", 8) ? Panel::Trophies
                    : !strncmp(o, "update", 6)   ? Panel::Update
                                                 : Panel::None;
            if (const char* c = strchr(o, ':')) tab = std::clamp(atoi(c + 1), 0, kTabs - 1);
        }
        std::vector<fs::path> paths;
        for (const std::string& f : env_list("DOD3_LAUNCHER_ADD", '|')) paths.push_back(fs::u8path(f));
        add(paths);
        static const struct {
            const char* name;
            ImGuiKey key;
        } names[] = { { "Up", ImGuiKey_UpArrow },       { "Down", ImGuiKey_DownArrow }, { "Left", ImGuiKey_LeftArrow },
                      { "Right", ImGuiKey_RightArrow }, { "Space", ImGuiKey_Space },    { "Enter", ImGuiKey_Enter },
                      { "Esc", ImGuiKey_Escape },       { "Tab", ImGuiKey_Tab },        { "Q", ImGuiKey_Q },
                      { "L1", ImGuiKey_GamepadL1 },     { "R1", ImGuiKey_GamepadR1 } };
        for (const std::string& k : env_list("DOD3_LAUNCHER_KEYS", ','))
            for (const auto& n : names)
                if (k == n.name) tests.keys.push_back(n.key);
        if (const char* sw = getenv("DOD3_LAUNCHER_SWITCH")) {   /* spanish:on | spanish:off */
            switch_mod(installed_parts(kSpanish, have), kSpanish.title, !strcmp(sw, "spanish:on"));
            fprintf(stderr, "[launcher] %s\n", toast.c_str());
        }
        if (const char* r = getenv("DOD3_LAUNCHER_REMOVE")) {   /* spanish | undub */
            panel = Panel::Mods;
            const ModGroup& g = !strcmp(r, "undub") ? kUndub : kSpanish;
            start_remove(installed_parts(g, have), g.title);
        }
    }

    /* the keys to press: each down at a frame that is a multiple of 12, up the frame after */
    void test_keys()
    {
        if (tests.next_key >= tests.keys.size() || frames <= 40) return;
        if (frames % 12 == 0)
            ImGui::GetIO().AddKeyEvent(tests.keys[tests.next_key], true);
        else if (frames % 12 == 1)
            ImGui::GetIO().AddKeyEvent(tests.keys[tests.next_key++], false);
    }

    /* the grab once the window has settled; a test's end */
    int test_step(int result)
    {
        grab_step(tests.grab, frames, 50, 80, 0);
        if (tests.quit && frames >= 90 && !working() && !checks.pending()) result = kQuit;
        if (tests.install && (flow == Flow::Done || flow == Flow::Failed)) {
            if (flow == Flow::Done)
                fprintf(stderr, "[launcher] done\n");
            else
                fprintf(stderr, "[launcher] failed: %s\n", job.error.c_str());
            result = kQuit;
        }
        return result;
    }
};

}  // namespace

bool launcher_wanted(bool key_held)
{
    if (key_held) return true;
    if (const char* e = getenv("DOD3_LAUNCHER")) return strcmp(e, "0") != 0;
    const auto kv = dod3::ini_read(fs::path(dod3_settings_path_now()));
    const auto it = kv.find("DOD3_LAUNCHER");
    return it == kv.end() || it->second != "0";
}

int run_launcher(const fs::path& base)
{
    if (!ui_init("Drakengard 3 Recompiled", 1280, 720, "", 17.0f)) return -1;
    look::style();
    Launcher L;
    look::load_fonts(L.F);
    L.base = base;
    L.ini = fs::path(dod3_settings_path_now());
    L.art = look::load_art(base);
    L.refresh();
    L.trophies.start(base);
    L.kv = dod3::ini_read(L.ini);
    dod3_setting_rows(L.rows);
    extra_rows(L.rows);
    L.read_test_env();
#ifdef _WIN32
    {
        L.prog = update_program_dir();
        std::string from;
        if (update_cleanup(L.prog, &from)) L.say("Updated from " + from + " to " + DOD3_VERSION_STRING);
        const char* e = getenv("DOD3_UPDATE_CHECK");
        const auto it = L.kv.find("DOD3_UPDATE_CHECK");
        if (e ? strcmp(e, "0") != 0 : it == L.kv.end() || it->second != "0") L.upd.start();
    }
#endif
    int result = kQuit;
    while (ui_frame_begin()) {
        const int r = L.frame();
        ui_frame_end();
        if (r != kGoOn) {
            result = r;
            break;
        }
    }
    L.job.stop();   /* the window was closed mid-install: the install stops first */
    L.checks.wait();
    ui_shutdown();  /* the backend lets go of the picture's texture */
    delete L.art;
    return result;
}

int run_launcher_loop(const fs::path& base)
{
    for (;;) {
        const int r = run_launcher(base);
        if (r != kGameFiles) return r < 0 ? kPlay : r;   /* no window: the game starts as before */
        /* Game Files: the installer -- its Play starts the game; closing it comes back here */
        const int i = run_installer(base);
        if (i == kPlay) return kPlay;
        if (!installed(base).ready()) return kQuit;
    }
}

}  // namespace dod3setup
