/*
 * First-run setup, Linux: the installer (src/ui/wizard.cpp, in an SDL2
 * window from src/ui/ui_linux.cpp) when something the game needs is
 * missing, or when asked for (--setup); once the game is installed, the
 * launcher (src/ui/launcher.cpp) at each start, unless dod3.ini turns it off.
 * The counterpart of os/win32/setup_win.cpp and os/macos/setup_mac.mm.
 *
 * The data folder is the executable's own, as on Windows -- the release is a
 * folder unpacked anywhere, the game installed into it -- unless that folder
 * cannot be written to (an install under /usr or /opt, a read-only image):
 * then $XDG_DATA_HOME/dod3-recomp (~/.local/share/dod3-recomp), the default
 * dod3.ini copied there from beside the executable on the first run.
 * Everything the game writes (saves, the shader cache, dod3.log) goes there.
 */
#include "install/install.h"
#include "ui/ui.h"

#include <SDL2/SDL.h>
#include <limits.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <string>
#include <system_error>
#include <thread>

namespace {

const char kTitle[] = "Drakengard 3 Recompiled";

std::string exe_dir()
{
    char buf[PATH_MAX];
    const ssize_t n = readlink("/proc/self/exe", buf, sizeof buf - 1);
    if (n <= 0) return ".";
    buf[n] = 0;
    std::string p(buf);
    const size_t sl = p.find_last_of('/');
    return sl == std::string::npos ? std::string(".") : p.substr(0, sl ? sl : 1);
}

/* No installer window: what to do instead -- the command line installs the
 * same way (main.cpp, --install). Shown as SDL's message box when it can be,
 * always on stderr. */
void no_window(const dod3setup::fs::path& base)
{
    char msg[1024];
    snprintf(msg, sizeof msg,
             "Setup could not open its window.\n\n"
             "The game can be installed from a terminal instead:\n\n"
             "%s/dod3 --install <disc .iso or folder> <update .pkg> [DLC .pkg ...]\n\n"
             "Details are in %s/dod3.log.",
             exe_dir().c_str(), base.string().c_str());
    fprintf(stderr, "%s\n", msg);
    SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, kTitle, msg, nullptr);
}

}  // namespace

/* The executable's folder (the release's), whatever the current directory. */
extern "C" const char* dod3_linux_exe_dir(void)
{
    static std::string dir;
    if (dir.empty()) dir = exe_dir();
    return dir.c_str();
}

/* The data folder (created): the executable's own if it can be written to,
 * else $XDG_DATA_HOME/dod3-recomp. NULL if neither can be made. */
extern "C" const char* dod3_linux_data_dir(void)
{
    static std::string dir;
    if (!dir.empty()) return dir.c_str();
    const std::string beside = exe_dir();
    if (access(beside.c_str(), W_OK) == 0) {
        dir = beside;
        return dir.c_str();
    }
    std::string base;
    if (const char* x = getenv("XDG_DATA_HOME"); x && *x == '/')
        base = x;
    else if (const char* h = getenv("HOME"); h && *h)
        base = std::string(h) + "/.local/share";
    else
        return NULL;
    const dod3setup::fs::path d = dod3setup::fs::path(base) / "dod3-recomp";
    std::error_code ec;
    dod3setup::fs::create_directories(d, ec);
    if (!dod3setup::fs::is_directory(d, ec)) return NULL;
    /* the default settings, once */
    if (!dod3setup::fs::exists(d / "dod3.ini", ec))
        dod3setup::fs::copy_file(dod3setup::fs::path(beside) / "dod3.ini", d / "dod3.ini", ec);
    dir = d.string();
    return dir.c_str();
}

/* Started from Steam or a desktop entry there is no terminal: the log goes to
 * dod3.log in the data folder (the current directory by now), the run before
 * kept as dod3.prev.log, as on Windows and the Mac. */
extern "C" void dod3_linux_log_to_file(void)
{
    if (isatty(STDERR_FILENO)) return;
    if (const char* e = getenv("DOD3_LOG_STDERR"); e && *e == '1') return;   /* Steam's own log, say */
    rename("dod3.log", "dod3.prev.log");
    /* A 1 MB buffer, written out by a thread of its own every 200 ms: a log
     * line is otherwise a write() by whichever thread logs it (see the Mac's). */
    static char buf[1 << 20];
    if (FILE* f = fopen("dod3.log", "w")) fclose(f);
    if (freopen("dod3.log", "a", stderr)) setvbuf(stderr, buf, _IOFBF, sizeof buf);
    if (!freopen("dod3.log", "a", stdout)) return;
    std::thread([] {
        pthread_setname_np(pthread_self(), "log flush");
        for (;;) {
            usleep(200 * 1000);
            fflush(stderr);
            fflush(stdout);
        }
    }).detach();
}

/* 0 to start the game: its files are in place under base (the installer
 * run first if not, or always with force), and the launcher said Play,
 * unless dod3.ini turns it off; nonzero if the player quit. There is no key
 * to hold here (no window to read it from yet): --setup opens the installer,
 * and DOD3_LAUNCHER=1 in the environment the launcher. */
extern "C" int dod3_setup_linux(const char* base_dir, int force)
{
    using namespace dod3setup;
    const fs::path base(base_dir);
    if (!installed(base).ready() || force) {
        const int r = run_installer(base);
        if (r >= 0) return r;
        no_window(base);
        return 1;
    }
    return launcher_wanted(false) ? run_launcher_loop(base) : 0;
}
