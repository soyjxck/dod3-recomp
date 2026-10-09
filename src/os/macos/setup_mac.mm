/*
 * First-run setup, macOS: the installer (src/ui/wizard.cpp, in a Metal
 * window from src/ui/ui_mac.mm) when something the game needs is missing,
 * or when asked for (Option held at launch). The macOS counterpart of
 * os/win32/setup_win.cpp.
 *
 * The data folder is ~/Library/Application Support/Drakengard 3 Recompiled,
 * not the app's own folder: a signed .app must not be written to, and a
 * downloaded app can run from a read-only translocated copy. Everything the
 * game writes (saves, the shader cache, dod3.log) goes there too, and the
 * player's dod3.ini lives there (copied from the app's default on first run).
 */
#include "install/install.h"
#include "ui/ui.h"

#import <AppKit/AppKit.h>
#include <stdio.h>
#include <string>
#include <thread>
#include <unistd.h>

namespace {

NSString* const kTitle = @"Drakengard 3 Recompiled";

/* No installer window: what to do instead -- the command line installs the
 * same way (main.cpp, --install). */
void no_window(const dod3setup::fs::path& base)
{
    NSString* exe = NSBundle.mainBundle.executablePath ?: @"dod3";
    NSAlert* a = [[NSAlert alloc] init];
    a.messageText = kTitle;
    a.informativeText =
        [NSString stringWithFormat:@"Setup could not open its window.\n\n"
                                   @"The game can be installed from Terminal instead:\n\n"
                                   @"\"%@\" --install <disc .iso or folder> <update .pkg> [DLC .pkg ...]\n\n"
                                   @"Details are in %s/dod3.log.",
                                   exe, base.string().c_str()];
    a.alertStyle = NSAlertStyleCritical;
    [a addButtonWithTitle:@"Quit"];
    [NSApp activateIgnoringOtherApps:YES];
    [a runModal];
}

}  // namespace

/* The data folder (created): ~/Library/Application Support/Drakengard 3
 * Recompiled. NULL if it cannot be made. */
extern "C" const char* dod3_mac_data_dir(void)
{
    static std::string dir;
    if (dir.empty()) {
        @autoreleasepool {
            NSURL* sup = [NSFileManager.defaultManager URLForDirectory:NSApplicationSupportDirectory
                                                              inDomain:NSUserDomainMask
                                                     appropriateForURL:nil
                                                                create:YES
                                                                 error:nil];
            if (!sup) return NULL;
            NSURL* d = [sup URLByAppendingPathComponent:kTitle isDirectory:YES];
            if (![NSFileManager.defaultManager createDirectoryAtURL:d
                                        withIntermediateDirectories:YES
                                                         attributes:nil
                                                              error:nil])
                return NULL;
            dir = d.fileSystemRepresentation;
        }
    }
    return dir.c_str();
}

/* Started from Finder there is no terminal: the log goes to dod3.log in the
 * data folder (the current directory by now); the run before is kept as
 * dod3.prev.log, as on Windows. */
extern "C" void dod3_mac_log_to_file(void)
{
    if (isatty(STDERR_FILENO)) return;
    rename("dod3.log", "dod3.prev.log");
    /* A 1 MB buffer, written out by a thread of its own every 200 ms: a log
     * line is otherwise a write() by whichever thread logs it, and the render
     * walker was caught in one for hundreds of milliseconds (a hitch). A crash
     * can lose the last 200 ms of the log; the Windows build does the same. */
    static char buf[1 << 20];
    if (freopen("dod3.log", "w", stderr)) setvbuf(stderr, buf, _IOFBF, sizeof buf);
    freopen("dod3.log", "a", stdout);
    std::thread([] {
        pthread_setname_np("log flush");
        for (;;) { usleep(200 * 1000); fflush(stderr); fflush(stdout); }
    }).detach();
}

/* 0 when the game's files are in place under base (running the installer
 * first if not, or always with force); nonzero if the player quit. The
 * player's dod3.ini is seeded from the app's default (Contents/Resources). */
extern "C" int dod3_setup_mac(const char* base_dir, int force)
{
    using namespace dod3setup;
    const fs::path base(base_dir);
    @autoreleasepool {
        std::error_code ec;
        if (!fs::exists(base / "dod3.ini", ec)) {
            NSString* def = [NSBundle.mainBundle pathForResource:@"dod3" ofType:@"ini"];
            if (def) fs::copy_file(fs::path(def.fileSystemRepresentation), base / "dod3.ini", ec);
        }
        /* The shader cache the package ships (tools/package_mac.sh): the
         * translated shaders and the pipeline list of a playthrough, so the
         * first launch warms up instead of compiling in play. Files the
         * player already has are left alone; the compiled archive is theirs. */
        if (NSString* res = NSBundle.mainBundle.resourcePath) {
            const fs::path shipped = fs::path(res.fileSystemRepresentation) / "cache";
            if (fs::is_directory(shipped, ec)) {
                fs::create_directories(base / "cache/msl", ec);
                for (const auto& f : fs::directory_iterator(shipped / "msl", ec))
                    if (!fs::exists(base / "cache/msl" / f.path().filename(), ec))
                        fs::copy_file(f.path(), base / "cache/msl" / f.path().filename(), ec);
                if (!fs::exists(base / "cache/pipelines.list", ec))
                    fs::copy_file(shipped / "pipelines.list", base / "cache/pipelines.list", ec);
            }
        }
        if (installed(base).ready() && !force) return 0;
        const int r = run_installer(base);
        if (r >= 0) return r;
        no_window(base);
        return 1;
    }
}

/* Option held at launch: run setup again (the --setup of a double-click). */
extern "C" int dod3_mac_option_held(void) { return ([NSEvent modifierFlags] & NSEventModifierFlagOption) != 0; }
