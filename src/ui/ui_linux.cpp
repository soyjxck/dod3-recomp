/*
 * The installer's and the launcher's window on Linux (src/ui/ui.h): an SDL2
 * window drawn with SDL_Renderer, Dear ImGui's SDL2 and SDL_Renderer2
 * backends, and the desktop's own file pickers. The pages (src/ui/wizard.cpp,
 * src/ui/launcher.cpp) draw; this file only gives them frames.
 *
 *   - The window is fixed-size and centred, as on Windows and the Mac: the
 *     size asked for, made smaller (its shape kept) on a screen without room
 *     for it -- the Steam Deck's 1280x800 among them. The pages lay
 *     themselves out from the window's height (ui/look.h), so the style
 *     needs no scaling here. DOD3_UI_SCALE=<k> makes it k times the size.
 *   - Pads reach ImGui through SDL's GameController (the Deck's own
 *     controls, as Steam Input presents them, included).
 *   - The pickers are kdialog or zenity, whichever the desktop has (KDE
 *     first on KDE, the Deck's desktop), run as a child while the window
 *     keeps answering its events. With neither, Add Files says so in the log
 *     and the command line installs instead (dod3 --install ...).
 *
 * DOD3_SETUP_GRAB=<prefix>: while <prefix>.req exists, the next frame is
 * written to <prefix>.ppm and the request removed (as on the Mac).
 */
#include "ui/ui.h"

#include "imgui.h"
#include "backends/imgui_impl_sdl2.h"
#include "backends/imgui_impl_sdlrenderer2.h"

#include <SDL2/SDL.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <spawn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#include <algorithm>
#include <string>
#include <vector>

extern char** environ;

namespace dod3setup {
namespace {

struct Ui {
    SDL_Window* window = nullptr;
    SDL_Renderer* renderer = nullptr;
    bool live = false, closed = false;
};
Ui s_ui;

/* DOD3_SETUP_GRAB: the frame's pixels as a PPM, read back before the present. */
void grab_if_asked()
{
    static const char* prefix = getenv("DOD3_SETUP_GRAB");
    if (!prefix || !*prefix) return;
    const std::string req = std::string(prefix) + ".req", out = std::string(prefix) + ".ppm";
    struct stat st;
    if (stat(req.c_str(), &st) != 0) return;
    unlink(req.c_str());
    int w = 0, h = 0;
    if (SDL_GetRendererOutputSize(s_ui.renderer, &w, &h) != 0 || w <= 0 || h <= 0) return;
    std::vector<uint8_t> px((size_t)w * h * 3);
    if (SDL_RenderReadPixels(s_ui.renderer, nullptr, SDL_PIXELFORMAT_RGB24, px.data(), w * 3) != 0) return;
    FILE* f = fopen((out + ".tmp").c_str(), "wb");
    if (!f) return;
    fprintf(f, "P6\n%d %d\n255\n", w, h);
    fwrite(px.data(), 1, px.size(), f);
    fclose(f);
    rename((out + ".tmp").c_str(), out.c_str());
}

/* After a picker: ImGui saw the button go down on the window, not up. */
void after_picker()
{
    ImGuiIO& io = ImGui::GetIO();
    for (int b = 0; b < ImGuiMouseButton_COUNT; b++) io.AddMouseButtonEvent(b, false);
    io.ClearInputKeys();
    if (s_ui.window) SDL_RaiseWindow(s_ui.window);
}

bool on_path(const char* exe)
{
    const char* path = getenv("PATH");
    if (!path || !*path) path = "/usr/local/bin:/usr/bin:/bin";
    std::string p(path);
    size_t i = 0;
    while (i <= p.size()) {
        size_t j = p.find(':', i);
        if (j == std::string::npos) j = p.size();
        const std::string dir = p.substr(i, j - i);
        i = j + 1;
        if (dir.empty()) continue;
        if (access((dir + "/" + exe).c_str(), X_OK) == 0) return true;
    }
    return false;
}

/* The picker the desktop has: kdialog on KDE, else zenity, else kdialog;
 * empty with neither. */
std::string picker()
{
    const char* de = getenv("XDG_CURRENT_DESKTOP");
    const bool kde = de && strstr(de, "KDE");
    const bool has_k = on_path("kdialog"), has_z = on_path("zenity");
    if (kde && has_k) return "kdialog";
    if (has_z) return "zenity";
    if (has_k) return "kdialog";
    return {};
}

/* Run `args` (args[0] on PATH) and return what it wrote to stdout, the
 * window's events answered meanwhile (the desktop would call a window that
 * stops answering frozen). False if it could not start or was cancelled. */
bool run_capture(const std::vector<std::string>& args, std::string* out)
{
    int fds[2];
    if (pipe(fds) != 0) return false;
    posix_spawn_file_actions_t fa;
    posix_spawn_file_actions_init(&fa);
    posix_spawn_file_actions_adddup2(&fa, fds[1], STDOUT_FILENO);
    posix_spawn_file_actions_addclose(&fa, fds[0]);
    posix_spawn_file_actions_addclose(&fa, fds[1]);
    std::vector<char*> argv;
    for (const std::string& a : args) argv.push_back(const_cast<char*>(a.c_str()));
    argv.push_back(nullptr);
    pid_t pid = 0;
    const int rc = posix_spawnp(&pid, argv[0], &fa, nullptr, argv.data(), environ);
    posix_spawn_file_actions_destroy(&fa);
    close(fds[1]);
    if (rc != 0) {
        close(fds[0]);
        fprintf(stderr, "[setup] could not run %s: %s\n", argv[0], strerror(rc));
        return false;
    }
    out->clear();
    char buf[4096];
    for (;;) {
        struct pollfd p = { fds[0], POLLIN, 0 };
        const int r = poll(&p, 1, 30);
        if (r > 0) {
            const ssize_t n = read(fds[0], buf, sizeof buf);
            if (n > 0) {
                out->append(buf, (size_t)n);
                continue;
            }
            if (n == 0 || (n < 0 && errno != EINTR)) break;
        }
        /* keep the window answering; what it is sent meanwhile is not the pages' */
        SDL_PumpEvents();
        SDL_FlushEvents(SDL_FIRSTEVENT, SDL_LASTEVENT);
    }
    close(fds[0]);
    int status = 0;
    while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {}
    return WIFEXITED(status) && WEXITSTATUS(status) == 0;
}

std::vector<std::string> lines(const std::string& s)
{
    std::vector<std::string> v;
    size_t i = 0;
    while (i < s.size()) {
        size_t j = s.find('\n', i);
        if (j == std::string::npos) j = s.size();
        std::string l = s.substr(i, j - i);
        while (!l.empty() && (l.back() == '\r' || l.back() == ' ')) l.pop_back();
        if (!l.empty()) v.push_back(l);
        i = j + 1;
    }
    return v;
}

/* {description, "*.pkg;*.iso"} -> the patterns, space-separated */
std::string patterns(const std::string& win)
{
    std::string out;
    size_t i = 0;
    while (i <= win.size()) {
        size_t j = win.find(';', i);
        if (j == std::string::npos) j = win.size();
        std::string e = win.substr(i, j - i);
        i = j + 1;
        while (!e.empty() && e.front() == ' ') e.erase(0, 1);
        if (e.empty()) continue;
        if (e == "*.*") e = "*";
        if (!out.empty()) out += ' ';
        out += e;
        /* the desktop's pickers match case as written: the upper case too */
        std::string up = e;
        for (char& c : up) c = (char)toupper((unsigned char)c);
        if (up != e) out += ' ' + up;
    }
    return out;
}

std::string start_dir()
{
    const char* home = getenv("HOME");
    return home && *home ? std::string(home) : std::string("/");
}

}  // namespace

bool ui_init(const char* title, int width, int height, const std::string& font_path, float font_px)
{
    SDL_SetHint(SDL_HINT_VIDEO_ALLOW_SCREENSAVER, "1");   /* a setup page is no reason to keep the screen on */
    if (SDL_InitSubSystem(SDL_INIT_VIDEO | SDL_INIT_GAMECONTROLLER | SDL_INIT_EVENTS) != 0) {
        fprintf(stderr, "[setup] SDL: %s\n", SDL_GetError());
        return false;
    }
    /* the size asked for, made smaller (its shape kept) where the screen has
     * no room for it: at most 94% of the usable area's width and 90% of its
     * height, as on Windows and the Mac */
    float k = 1.0f;
    if (const char* e = getenv("DOD3_UI_SCALE"))
        if (atof(e) >= 0.5 && atof(e) <= 4.0) k = (float)atof(e);
    SDL_Rect usable;
    if (SDL_GetDisplayUsableBounds(0, &usable) == 0 && usable.w > 0 && usable.h > 0) {
        const float room_w = usable.w * 0.94f, room_h = usable.h * 0.90f;
        if (width * k > room_w || height * k > room_h) k = std::min(room_w / width, room_h / height);
    }
    s_ui.window = SDL_CreateWindow(title ? title : "", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
                                   (int)(width * k), (int)(height * k), SDL_WINDOW_ALLOW_HIGHDPI);
    if (!s_ui.window) {
        fprintf(stderr, "[setup] SDL_CreateWindow: %s\n", SDL_GetError());
        SDL_QuitSubSystem(SDL_INIT_VIDEO | SDL_INIT_GAMECONTROLLER | SDL_INIT_EVENTS);
        return false;
    }
    s_ui.renderer = SDL_CreateRenderer(s_ui.window, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
    if (!s_ui.renderer) s_ui.renderer = SDL_CreateRenderer(s_ui.window, -1, SDL_RENDERER_SOFTWARE);
    if (!s_ui.renderer) {
        fprintf(stderr, "[setup] SDL_CreateRenderer: %s\n", SDL_GetError());
        SDL_DestroyWindow(s_ui.window);
        s_ui = Ui{};
        return false;
    }
    {
        SDL_RendererInfo ri;
        if (SDL_GetRendererInfo(s_ui.renderer, &ri) == 0)
            fprintf(stderr, "[setup] window %dx%d, renderer %s\n", (int)(width * k), (int)(height * k), ri.name);
    }

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;   /* no imgui.ini in the data folder */
    io.LogFilename = nullptr;
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard | ImGuiConfigFlags_NavEnableGamepad;
    if (!ImGui_ImplSDL2_InitForSDLRenderer(s_ui.window, s_ui.renderer) || !ImGui_ImplSDLRenderer2_Init(s_ui.renderer)) {
        ImGui::DestroyContext();
        SDL_DestroyRenderer(s_ui.renderer);
        SDL_DestroyWindow(s_ui.window);
        s_ui = Ui{};
        return false;
    }
    if (!font_path.empty() && !io.Fonts->AddFontFromFileTTF(font_path.c_str(), font_px))
        fprintf(stderr, "[setup] could not load the font %s\n", font_path.c_str());
    if (io.Fonts->Fonts.empty()) io.Fonts->AddFontDefault();
    ImGui::GetStyle().FontSizeBase = font_px;
    s_ui.live = true;
    s_ui.closed = false;
    return true;
}

bool ui_frame_begin()
{
    for (;;) {
        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            ImGui_ImplSDL2_ProcessEvent(&e);
            if (e.type == SDL_QUIT) s_ui.closed = true;
            if (e.type == SDL_WINDOWEVENT && e.window.event == SDL_WINDOWEVENT_CLOSE &&
                e.window.windowID == SDL_GetWindowID(s_ui.window))
                s_ui.closed = true;
        }
        if (!s_ui.live || s_ui.closed) return false;
        /* minimised: wait rather than spin */
        if (SDL_GetWindowFlags(s_ui.window) & SDL_WINDOW_MINIMIZED) {
            SDL_WaitEventTimeout(nullptr, 50);
            continue;
        }
        break;
    }
    ImGui_ImplSDLRenderer2_NewFrame();
    ImGui_ImplSDL2_NewFrame();
    ImGui::NewFrame();
    return true;
}

void ui_frame_end()
{
    ImGui::Render();
    ImGuiIO& io = ImGui::GetIO();
    SDL_RenderSetScale(s_ui.renderer, io.DisplayFramebufferScale.x, io.DisplayFramebufferScale.y);
    SDL_SetRenderDrawColor(s_ui.renderer, 0, 0, 0, 255);
    SDL_RenderClear(s_ui.renderer);
    ImGui_ImplSDLRenderer2_RenderDrawData(ImGui::GetDrawData(), s_ui.renderer);
    grab_if_asked();
    SDL_RenderPresent(s_ui.renderer);   /* v-synced (PRESENTVSYNC): the frame pacing */
}

void ui_shutdown()
{
    if (!s_ui.live) return;
    ImGui_ImplSDLRenderer2_Shutdown();
    ImGui_ImplSDL2_Shutdown();
    ImGui::DestroyContext();
    SDL_DestroyRenderer(s_ui.renderer);
    SDL_DestroyWindow(s_ui.window);
    SDL_PumpEvents();   /* the window goes now, not when the game's window is made */
    SDL_QuitSubSystem(SDL_INIT_VIDEO | SDL_INIT_GAMECONTROLLER | SDL_INIT_EVENTS);
    s_ui = Ui{};
}

std::vector<std::filesystem::path> ui_pick_files(const char* title,
                                                 const std::vector<std::pair<std::string, std::string>>& filters)
{
    std::vector<std::filesystem::path> out;
    const std::string tool = picker();
    if (tool.empty()) {
        fprintf(stderr, "[setup] no file picker (kdialog or zenity) on this system; install from a terminal: "
                        "dod3 --install <files...>\n");
        return out;
    }
    std::vector<std::string> args;
    if (tool == "kdialog") {
        /* kdialog --getopenfilename <dir> "<patterns>|<name>\n..." */
        std::string f;
        for (const auto& [name, pats] : filters) {
            if (!f.empty()) f += '\n';
            f += patterns(pats) + "|" + name;
        }
        args = { "kdialog", "--title", title ? title : "", "--getopenfilename", start_dir(), f,
                 "--multiple", "--separate-output" };
    } else {
        args = { "zenity", "--file-selection", "--multiple", "--separator=\n", std::string("--title=") + (title ? title : "") };
        for (const auto& [name, pats] : filters) args.push_back("--file-filter=" + name + " | " + patterns(pats));
    }
    std::string text;
    if (run_capture(args, &text))
        for (const std::string& l : lines(text)) out.emplace_back(l);
    if (s_ui.live) after_picker();
    return out;
}

std::filesystem::path ui_pick_folder(const char* title)
{
    std::filesystem::path out;
    const std::string tool = picker();
    if (tool.empty()) {
        fprintf(stderr, "[setup] no folder picker (kdialog or zenity) on this system\n");
        return out;
    }
    std::vector<std::string> args;
    if (tool == "kdialog")
        args = { "kdialog", "--title", title ? title : "", "--getexistingdirectory", start_dir() };
    else
        args = { "zenity", "--file-selection", "--directory", std::string("--title=") + (title ? title : "") };
    std::string text;
    if (run_capture(args, &text)) {
        const std::vector<std::string> v = lines(text);
        if (!v.empty()) out = v.front();
    }
    if (s_ui.live) after_picker();
    return out;
}

}  // namespace dod3setup
