/*
 * The installer's platform layer: everything the shared wizard
 * (src/ui/wizard.cpp, Dear ImGui) needs from the platform, and nothing
 * else. One implementation per platform:
 *   src/ui/ui_win.cpp   Win32 window + Direct3D 12 (imgui_impl_win32/dx12),
 *                          IFileOpenDialog
 *   src/ui/ui_mac.mm    Cocoa window + Metal (imgui_impl_osx/metal),
 *                          NSOpenPanel
 * Dear ImGui is vendored in third_party/imgui (one pinned version for both).
 *
 * The wizard owns the ImGui context's contents (style, fonts, pages); the
 * platform owns the window, the device and the backends:
 *
 *   if (!ui_init("Drakengard 3 Recompiled", 1280, 720)) -> an alert
 *   while (ui_frame_begin()) { ...ImGui calls...; ui_frame_end(); }
 *   ui_shutdown();
 *
 * Keyboard and controller navigation are on (the backends feed gamepads:
 * XInput on Windows, GameController on the Mac).
 */
#pragma once
#include <filesystem>
#include <string>
#include <vector>

namespace dod3setup {

/* Create the window, the device and ImGui's context and backends; load the
 * font from `font_path` (a .ttf; empty = ImGui's default) at `font_px`
 * scaled by the display's DPI. False if any of it fails -- the caller then
 * shows an alert with the command-line way to install. */
bool ui_init(const char* title, int width, int height, const std::string& font_path, float font_px);

/* Pump the platform's events and start an ImGui frame (the backends'
 * NewFrame, then ImGui::NewFrame). False once the window has been closed:
 * the wizard treats that as Cancel / Quit. */
bool ui_frame_begin();

/* ImGui::Render, draw it, present (v-synced). */
void ui_frame_end();

/* Tear down the backends, the context, the device and the window. */
void ui_shutdown();

/* The native pickers, modal over the window. Empty when cancelled.
 * `filters`: pairs of {description, "*.pkg;*.iso;*.elf"} (Windows syntax;
 * the Mac takes the extensions after the "*."). */
std::vector<std::filesystem::path> ui_pick_files(const char* title,
                                                 const std::vector<std::pair<std::string, std::string>>& filters);
std::filesystem::path ui_pick_folder(const char* title);

/* A font that looks right on this platform for the wizard's text (Segoe UI
 * on Windows, the system's on the Mac); empty if none was found. */
std::string ui_default_font();

/* The whole installer (src/ui/wizard.cpp). `base` is where the game is
 * installed (beside the executable on Windows, Application Support on the
 * Mac). Returns 0 when the game is installed and ready to start, 1 if the
 * player quit, -1 if the UI could not start (show an alert instead). */
int run_installer(const std::filesystem::path& base);

}  // namespace dod3setup
