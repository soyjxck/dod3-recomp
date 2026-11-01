/*
 * The keyboard and mouse as the pad.
 *
 * The pad library asks this for port 0's state at every sweep
 * (cellPad_set_host_input), beside whatever real pad is there: keys and
 * mouse buttons held become the pad's buttons and the two sticks' directions
 * by a table of bindings, and the wheel a short press. The mouse's motion is
 * not here: it turns the camera directly (src/input/camera.cpp), with the
 * cursor hidden and kept in the window while the game has the focus.
 *
 * The bindings, each a list of keys (dod3.ini; "none" for no key):
 *
 *   KEY_CROSS      Space              KEY_CIRCLE    E, Backspace
 *   KEY_SQUARE     Mouse1             KEY_TRIANGLE  Mouse2
 *   KEY_L1         Q, Mouse3          KEY_R1        Shift
 *   KEY_L2         R                  KEY_R2        F
 *   KEY_L3         V, C               KEY_R3        V, X
 *   KEY_START      Esc, Enter         KEY_SELECT    Tab
 *   KEY_UP KEY_DOWN KEY_LEFT KEY_RIGHT              the arrows, and 1 3 4 2
 *   KEY_MOVE_UP _DOWN _LEFT _RIGHT                  W S A D (the left stick)
 *   KEY_LOOK_UP _DOWN _LEFT _RIGHT                  none none WheelUp WheelDown
 *                                                   (the right stick)
 *
 * Keys: A-Z, 0-9, F1-F12, Space, Enter, Tab, Esc, Backspace, Shift, Ctrl,
 * Alt, LShift RShift LCtrl RCtrl LAlt RAlt, Up Down Left Right, Insert
 * Delete Home End PageUp PageDown, CapsLock, Minus Equals LBracket RBracket
 * Semicolon Quote Comma Period Slash Backslash Grave, Num0-Num9, Mouse1
 * (left) Mouse2 (right) Mouse3 (middle) Mouse4 Mouse5, WheelUp WheelDown.
 *
 * V presses both sticks in (Intoner Mode); Escape is Start, so the window no
 * longer closes on it (Alt+F4 does; Cmd+Q on macOS). With a pad plugged in
 * as well, either can be used at any moment: the prompts follow the one
 * touched last.
 *
 * Where the keys' state comes from is the one part that differs by platform
 * (a table of names and key_held, below). Windows asks the system for a
 * virtual key. Linux has the Vulkan window's record of the key and mouse
 * button events SDL sent it (ps3recomp/libs/video/rsx_vulkan_engine.c), by
 * SDL scancode -- a place on the keyboard, as on the Mac -- so a key is only
 * held while the game is in front. macOS has the Metal window's own record of the key and mouse
 * button events it was sent (ps3recomp/libs/video/rsx_metal_backend.m), by
 * virtual keycode -- a place on the keyboard, whatever the layout prints on
 * it -- so a key is only ever held while the game is in front; Alt is the
 * Option key, and CapsLock cannot be bound (it reports its lock, not the
 * key).
 *
 *   DOD3_KEYBOARD=0   the toolkit's built-in keys instead (no mouse)
 *   DOD3_KBM_LOG=1    a line whenever what the keys hold changes
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <string>
#include <vector>
#include "input/camera.h"
#include "input/kbm.h"
#include "rsx_host_mouse.h"
#include "util.h"

#if defined(_WIN32) || defined(__APPLE__) || defined(PS3RECOMP_VULKAN_ENGINE_SDL)
#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <strings.h>
#define _stricmp strcasecmp
#define _strnicmp strncasecmp
#endif

extern "C" {
/* ps3recomp/libs/input/cellPad.h */
typedef struct CellPadHostInput {
    uint16_t buttons;
    uint8_t lx, ly, rx, ry;
    uint8_t l2, r2;
} CellPadHostInput;
typedef int (*cellPad_host_input_fn)(void* user, int focused, CellPadHostInput* out);
void cellPad_set_host_input(cellPad_host_input_fn fn, void* user);
#ifdef __APPLE__
/* ps3recomp/libs/video/rsx_metal_backend.h */
int rsx_metal_backend_key_down(unsigned keycode);
int rsx_metal_backend_mouse_button_down(unsigned button);
#elif !defined(_WIN32)
/* ps3recomp/libs/video/rsx_vulkan_engine.h */
int rsx_vulkan_engine_key_down(unsigned scancode);
int rsx_vulkan_engine_mouse_button_down(unsigned button);
#endif
}

namespace {

const int kWheelUp = -1, kWheelDown = -2;   /* not keys: notches, each a short press */

#ifdef __APPLE__
/* A key is its macOS virtual keycode (kVK_* in HIToolbox/Events.h). Above
 * those: the names that mean either of two keys, the mouse's buttons, and
 * the one key that cannot be read. */
const int kEitherShift = 0x100, kEitherCtrl = 0x101, kEitherAlt = 0x102, kEitherEnter = 0x103;
const int kMouseButton = 0x200;   /* + 0 left, 1 right, 2 middle, 3, 4 */
const int kNoSuchKey = 0x300;

const struct {
    const char* name;
    int vk;
} kKeys[] = {
    { "Space", 0x31 },         { "Enter", kEitherEnter },   { "Tab", 0x30 },
    { "Esc", 0x35 },           { "Backspace", 0x33 },       { "Shift", kEitherShift },
    { "Ctrl", kEitherCtrl },   { "Alt", kEitherAlt },       { "LShift", 0x38 },
    { "RShift", 0x3C },        { "LCtrl", 0x3B },           { "RCtrl", 0x3E },
    { "LAlt", 0x3A },          { "RAlt", 0x3D },            { "Up", 0x7E },
    { "Down", 0x7D },          { "Left", 0x7B },            { "Right", 0x7C },
    { "Insert", 0x72 },        { "Delete", 0x75 },          { "Home", 0x73 },
    { "End", 0x77 },           { "PageUp", 0x74 },          { "PageDown", 0x79 },
    { "CapsLock", kNoSuchKey }, { "Minus", 0x1B },          { "Equals", 0x18 },
    { "LBracket", 0x21 },      { "RBracket", 0x1E },        { "Semicolon", 0x29 },
    { "Quote", 0x27 },         { "Comma", 0x2B },           { "Period", 0x2F },
    { "Slash", 0x2C },         { "Backslash", 0x2A },       { "Grave", 0x32 },
    { "Mouse1", kMouseButton + 0 }, { "Mouse2", kMouseButton + 1 }, { "Mouse3", kMouseButton + 2 },
    { "Mouse4", kMouseButton + 3 }, { "Mouse5", kMouseButton + 4 }, { "WheelUp", kWheelUp },
    { "WheelDown", kWheelDown },
};

/* 'A'-'Z' and '0'-'9': the ANSI layout's places. */
int letter_key(char c)
{
    static const uint8_t az[26] = { 0x00, 0x0B, 0x08, 0x02, 0x0E, 0x03, 0x05, 0x04, 0x22, 0x26, 0x28, 0x25, 0x2E,
                                    0x2D, 0x1F, 0x23, 0x0C, 0x0F, 0x01, 0x11, 0x20, 0x09, 0x0D, 0x07, 0x10, 0x06 };
    static const uint8_t digit[10] = { 0x1D, 0x12, 0x13, 0x14, 0x15, 0x17, 0x16, 0x1A, 0x1C, 0x19 };
    return c >= 'A' ? az[c - 'A'] : digit[c - '0'];
}
int function_key(int n)   /* 1-12 */
{
    static const uint8_t f[12] = { 0x7A, 0x78, 0x63, 0x76, 0x60, 0x61, 0x62, 0x64, 0x65, 0x6D, 0x67, 0x6F };
    return f[n - 1];
}
int numpad_key(int n)   /* 0-9 */
{
    static const uint8_t num[10] = { 0x52, 0x53, 0x54, 0x55, 0x56, 0x57, 0x58, 0x59, 0x5B, 0x5C };
    return num[n];
}
bool key_held(int k)
{
    const auto key = [](unsigned kc) { return rsx_metal_backend_key_down(kc) != 0; };
    switch (k) {
    case kEitherShift: return key(0x38) || key(0x3C);
    case kEitherCtrl: return key(0x3B) || key(0x3E);
    case kEitherAlt: return key(0x3A) || key(0x3D);
    case kEitherEnter: return key(0x24) || key(0x4C);   /* Return, and the keypad's Enter */
    case kNoSuchKey: return false;
    }
    if (k >= kMouseButton) return rsx_metal_backend_mouse_button_down((unsigned)(k - kMouseButton)) != 0;
    return key((unsigned)k);
}
#elif !defined(_WIN32)
/* Linux: a key is its SDL scancode (SDL_scancode.h: the USB HID usage).
 * Above those, as on the Mac: the names that mean either of two keys and
 * the mouse's buttons. */
const int kEitherShift = 0x200, kEitherCtrl = 0x201, kEitherAlt = 0x202, kEitherEnter = 0x203;
const int kMouseButton = 0x300;   /* + 0 left, 1 right, 2 middle, 3, 4 */
const int kNoSuchKey = 0x400;     /* every key here can be read */

const struct {
    const char* name;
    int vk;
} kKeys[] = {
    { "Space", 44 },          { "Enter", kEitherEnter },  { "Tab", 43 },
    { "Esc", 41 },            { "Backspace", 42 },        { "Shift", kEitherShift },
    { "Ctrl", kEitherCtrl },  { "Alt", kEitherAlt },      { "LShift", 225 },
    { "RShift", 229 },        { "LCtrl", 224 },           { "RCtrl", 228 },
    { "LAlt", 226 },          { "RAlt", 230 },            { "Up", 82 },
    { "Down", 81 },           { "Left", 80 },             { "Right", 79 },
    { "Insert", 73 },         { "Delete", 76 },           { "Home", 74 },
    { "End", 77 },            { "PageUp", 75 },           { "PageDown", 78 },
    { "CapsLock", 57 },       { "Minus", 45 },            { "Equals", 46 },
    { "LBracket", 47 },       { "RBracket", 48 },         { "Semicolon", 51 },
    { "Quote", 52 },          { "Comma", 54 },            { "Period", 55 },
    { "Slash", 56 },          { "Backslash", 49 },        { "Grave", 53 },
    { "Mouse1", kMouseButton + 0 }, { "Mouse2", kMouseButton + 1 }, { "Mouse3", kMouseButton + 2 },
    { "Mouse4", kMouseButton + 3 }, { "Mouse5", kMouseButton + 4 }, { "WheelUp", kWheelUp },
    { "WheelDown", kWheelDown },
};

/* SDL_SCANCODE_A = 4 .. Z = 29; 1 = 30 .. 9 = 38, 0 = 39 */
int letter_key(char c) { return c >= 'A' ? 4 + (c - 'A') : c == '0' ? 39 : 30 + (c - '1'); }
int function_key(int n) { return 58 + n - 1; }                 /* F1 = 58 .. F12 = 69 */
int numpad_key(int n) { return n == 0 ? 98 : 89 + (n - 1); }    /* KP_1 = 89 .. KP_9 = 97, KP_0 = 98 */
bool key_held(int k)
{
    const auto key = [](unsigned sc) { return rsx_vulkan_engine_key_down(sc) != 0; };
    switch (k) {
    case kEitherShift: return key(225) || key(229);
    case kEitherCtrl: return key(224) || key(228);
    case kEitherAlt: return key(226) || key(230);
    case kEitherEnter: return key(40) || key(88);   /* Return, and the keypad's Enter */
    case kNoSuchKey: return false;
    }
    if (k >= kMouseButton) return rsx_vulkan_engine_mouse_button_down((unsigned)(k - kMouseButton)) != 0;
    return key((unsigned)k);
}
#else
/* A key is its Windows virtual-key code. */
const int kNoSuchKey = -3;   /* every key here can be read */
int letter_key(char c) { return c; }   /* 'A'-'Z' and '0'-'9' are their own codes */
int function_key(int n) { return VK_F1 + n - 1; }
int numpad_key(int n) { return VK_NUMPAD0 + n; }
bool key_held(int vk) { return (GetAsyncKeyState(vk) & 0x8000) != 0; }

const struct {
    const char* name;
    int vk;
} kKeys[] = {
    { "Space", VK_SPACE },       { "Enter", VK_RETURN },    { "Tab", VK_TAB },
    { "Esc", VK_ESCAPE },        { "Backspace", VK_BACK },  { "Shift", VK_SHIFT },
    { "Ctrl", VK_CONTROL },      { "Alt", VK_MENU },        { "LShift", VK_LSHIFT },
    { "RShift", VK_RSHIFT },     { "LCtrl", VK_LCONTROL },  { "RCtrl", VK_RCONTROL },
    { "LAlt", VK_LMENU },        { "RAlt", VK_RMENU },      { "Up", VK_UP },
    { "Down", VK_DOWN },         { "Left", VK_LEFT },       { "Right", VK_RIGHT },
    { "Insert", VK_INSERT },     { "Delete", VK_DELETE },   { "Home", VK_HOME },
    { "End", VK_END },           { "PageUp", VK_PRIOR },    { "PageDown", VK_NEXT },
    { "CapsLock", VK_CAPITAL },  { "Minus", VK_OEM_MINUS }, { "Equals", VK_OEM_PLUS },
    { "LBracket", VK_OEM_4 },    { "RBracket", VK_OEM_6 },  { "Semicolon", VK_OEM_1 },
    { "Quote", VK_OEM_7 },       { "Comma", VK_OEM_COMMA }, { "Period", VK_OEM_PERIOD },
    { "Slash", VK_OEM_2 },       { "Backslash", VK_OEM_5 }, { "Grave", VK_OEM_3 },
    { "Mouse1", VK_LBUTTON },    { "Mouse2", VK_RBUTTON },  { "Mouse3", VK_MBUTTON },
    { "Mouse4", VK_XBUTTON1 },   { "Mouse5", VK_XBUTTON2 }, { "WheelUp", kWheelUp },
    { "WheelDown", kWheelDown },
};
#endif

const struct {
    const char* key;        /* dod3.ini */
    const char* defaults;
    uint16_t button;        /* CELL_PAD_CTRL_*, or 0 for a stick's direction */
} kBind[kBinds] = {
    { "KEY_CROSS", "Space", 1 << 14 },
    { "KEY_CIRCLE", "E Backspace", 1 << 13 },
    { "KEY_SQUARE", "Mouse1", 1 << 15 },
    { "KEY_TRIANGLE", "Mouse2", 1 << 12 },
    { "KEY_L1", "Q Mouse3", 1 << 10 },
    { "KEY_R1", "Shift", 1 << 11 },
    { "KEY_L2", "R", 1 << 8 },
    { "KEY_R2", "F", 1 << 9 },
    { "KEY_L3", "V C", 1 << 1 },
    { "KEY_R3", "V X", 1 << 2 },
    { "KEY_START", "Esc Enter", 1 << 3 },
    { "KEY_SELECT", "Tab", 1 << 0 },
    { "KEY_UP", "Up 1", 1 << 4 },
    { "KEY_DOWN", "Down 3", 1 << 6 },
    { "KEY_LEFT", "Left 4", 1 << 7 },
    { "KEY_RIGHT", "Right 2", 1 << 5 },
    { "KEY_MOVE_UP", "W", 0 },
    { "KEY_MOVE_DOWN", "S", 0 },
    { "KEY_MOVE_LEFT", "A", 0 },
    { "KEY_MOVE_RIGHT", "D", 0 },
    { "KEY_LOOK_UP", "none", 0 },
    { "KEY_LOOK_DOWN", "none", 0 },
    { "KEY_LOOK_LEFT", "WheelUp", 0 },
    { "KEY_LOOK_RIGHT", "WheelDown", 0 },
};

struct Key {
    std::string name;   /* as the prompts name it */
    int vk;
};
std::vector<Key> s_keys[kBinds];
bool s_installed, s_mouse_look = true, s_log;
uint64_t s_wheel_until[2];   /* a notch's press, up and down, lasts until then */

bool lookup(const std::string& word, Key& out)
{
    if (word.size() == 1) {
        const char c = word[0];
        if (c >= 'a' && c <= 'z') {
            out = { std::string(1, (char)(c - 32)), letter_key((char)(c - 32)) };
            return true;
        }
        if ((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9')) {
            out = { word, letter_key(c) };
            return true;
        }
    }
    if ((word[0] == 'F' || word[0] == 'f') && word.size() <= 3) {
        const int n = atoi(word.c_str() + 1);
        if (n >= 1 && n <= 12) {
            out = { "F" + std::to_string(n), function_key(n) };
            return true;
        }
    }
    if (word.size() == 4 && !_strnicmp(word.c_str(), "Num", 3) && word[3] >= '0' && word[3] <= '9') {
        out = { std::string("Num") + word[3], numpad_key(word[3] - '0') };
        return true;
    }
    for (const auto& k : kKeys)
        if (!_stricmp(word.c_str(), k.name)) {
            out = { k.name, k.vk };
            return true;
        }
    return false;
}

void parse(int b, const char* list)
{
    s_keys[b].clear();
    std::string word;
    for (const char* p = list;; p++) {
        if (*p && *p != ',' && *p != ' ' && *p != '\t') {
            word += *p;
            continue;
        }
        if (!word.empty()) {
            Key k;
            if (!_stricmp(word.c_str(), "none")) {
                /* no key */
            } else if (lookup(word, k) && k.vk == kNoSuchKey) {
                fprintf(stderr, "[keyboard] %s: %s cannot be bound on this system\n", kBind[b].key, k.name.c_str());
            } else if (lookup(word, k)) {
                s_keys[b].push_back(k);
            } else {
                fprintf(stderr, "[keyboard] %s: no key called \"%s\"\n", kBind[b].key, word.c_str());
            }
            word.clear();
        }
        if (!*p) break;
    }
}

bool down(int b, uint64_t now)
{
    for (const Key& k : s_keys[b]) {
        if (k.vk == kWheelUp) {
            if (now < s_wheel_until[0]) return true;
        } else if (k.vk == kWheelDown) {
            if (now < s_wheel_until[1]) return true;
        } else if (key_held(k.vk))
            return true;
    }
    return false;
}

/* A stick from its four directions: a full push, the diagonals on the circle. */
void stick(bool up, bool dn, bool lf, bool rt, uint8_t& x, uint8_t& y)
{
    const int h = (rt ? 1 : 0) - (lf ? 1 : 0), v = (dn ? 1 : 0) - (up ? 1 : 0);
    const int reach = (h && v) ? 90 : 127;
    x = (uint8_t)(128 + h * reach);
    y = (uint8_t)(128 + v * reach);
}

int host_input(void*, int focused, CellPadHostInput* out)
{
    const uint64_t now = dod3_now_us();
    const int wheel = rsx_host_mouse_take_wheel();
    if (!focused) return 0;   /* keys held while another window is in front are not ours */
    if (wheel > 0) s_wheel_until[0] = now + 70000;
    if (wheel < 0) s_wheel_until[1] = now + 70000;

    bool on[kBinds];
    bool any = false;
    for (int b = 0; b < kBinds; b++) {
        on[b] = down(b, now);
        any = any || on[b];
        if (on[b]) out->buttons |= kBind[b].button;
    }
    stick(on[kBindMoveUp], on[kBindMoveDown], on[kBindMoveLeft], on[kBindMoveRight], out->lx, out->ly);
    stick(on[kBindLookUp], on[kBindLookDown], on[kBindLookLeft], on[kBindLookRight], out->rx, out->ry);
    if (on[kBindL2]) out->l2 = 255;
    if (on[kBindR2]) out->r2 = 255;
    if (s_log) {
        static CellPadHostInput last;
        if (memcmp(&last, out, sizeof last)) {
            last = *out;
            fprintf(stderr, "[keyboard] buttons %04X left %u,%u right %u,%u\n", out->buttons, out->lx, out->ly, out->rx,
                    out->ry);
        }
    }
    return any || now - dod3_camera_mouse_time() < 150000;
}

}  // namespace

#ifdef __linux__
/* A Steam Deck (its DMI board: Valve's Jupiter, or Galileo for the OLED). */
static bool on_steam_deck()
{
    auto read = [](const char* path) {
        std::string v;
        if (FILE* f = fopen(path, "r")) {
            char b[64] = {};
            if (fgets(b, sizeof b, f)) v = b;
            fclose(f);
        }
        while (!v.empty() && (v.back() == '\n' || v.back() == ' ')) v.pop_back();
        return v;
    };
    const std::string vendor = read("/sys/class/dmi/id/board_vendor"), product = read("/sys/class/dmi/id/product_name");
    return vendor == "Valve" && (product == "Jupiter" || product == "Galileo");
}
#endif

void dod3_kbm_install()
{
    if (const char* e = getenv("DOD3_KEYBOARD"))
        if (!strcmp(e, "0")) return;
#ifdef __linux__
    /* A Steam Deck, started outside Steam (Desktop Mode, from a terminal or
     * the file manager): Steam's Desktop Configuration turns the Deck's own
     * buttons into keys and its trackpad into the mouse, so every press came
     * twice -- once from the pad, once as Esc or Enter, which is Start -- and
     * the pause menu kept opening. The pad alone, then, unless DOD3_KEYBOARD=1
     * asks for the keys (a real keyboard plugged in). Started from Steam (a
     * non-Steam shortcut too), Steam gives the game a pad layout instead and
     * the keys are the player's. */
    if (!getenv("DOD3_KEYBOARD") && !getenv("SteamGameId") && !getenv("SteamAppId") && on_steam_deck()) {
        fprintf(stderr, "[kbm] Steam Deck outside Steam: keyboard and mouse off, the pad alone "
                        "(DOD3_KEYBOARD=1 for a keyboard of your own)\n");
        return;
    }
#endif
    for (int b = 0; b < kBinds; b++) {
        const char* e = getenv(kBind[b].key);
        parse(b, e && *e ? e : kBind[b].defaults);
    }
    if (const char* e = getenv("DOD3_MOUSE")) s_mouse_look = strcmp(e, "0") != 0;
    s_log = getenv("DOD3_KBM_LOG") != nullptr;
    s_installed = true;
    cellPad_set_host_input(host_input, nullptr);
    rsx_host_window_escape_closes(0);
    rsx_host_mouse_capture(s_mouse_look ? 1 : 0);
}

bool dod3_kbm_present() { return s_installed; }
bool dod3_kbm_mouse_look() { return s_installed && s_mouse_look; }
const char* dod3_kbm_key(Dod3Bind b) { return s_installed && !s_keys[b].empty() ? s_keys[b][0].name.c_str() : nullptr; }

#else /* no keyboard and mouse of the port's own on this platform yet */

void dod3_kbm_install() {}
bool dod3_kbm_present() { return false; }
bool dod3_kbm_mouse_look() { return false; }
const char* dod3_kbm_key(Dod3Bind) { return nullptr; }

#endif
