/*
 * First-run setup, macOS: asks for the player's files with the standard
 * panels, checks them (src/setup_iso.cpp) and copies them into the data
 * folder, with a progress window. The macOS counterpart of setup_win.cpp.
 *
 * The data folder is ~/Library/Application Support/Drakengard 3 Recompiled,
 * not the app's own folder: a signed .app must not be written to, and a
 * downloaded app can run from a read-only translocated copy. Everything the
 * game writes (saves, the shader cache, dod3.log) goes there too, and the
 * player's dod3.ini lives there (copied from the app's default on first run).
 */
#include "setup_iso.h"

#import <AppKit/AppKit.h>
#include <atomic>
#include <stdio.h>
#include <string>
#include <thread>
#include <unistd.h>

using namespace dod3setup;

namespace {

NSString* const kTitle = @"Drakengard 3 Recompiled";

NSString* ns(const std::string& s) { return [NSString stringWithUTF8String:s.c_str()]; }

/* An alert with up to three buttons; returns 0, 1 or 2 for the button
 * pressed (the first is the default). */
int ask(NSString* text, NSAlertStyle style, NSArray<NSString*>* buttons)
{
    NSAlert* a = [[NSAlert alloc] init];
    a.messageText = kTitle;
    a.informativeText = text;
    a.alertStyle = style;
    for (NSString* b in buttons) [a addButtonWithTitle:b];
    [NSApp activateIgnoringOtherApps:YES];
    const NSModalResponse r = [a runModal];
    return (int)(r - NSAlertFirstButtonReturn);
}

/* One file or folder from the open panel; empty if cancelled. */
fs::path pick(NSString* message, bool folder)
{
    NSOpenPanel* p = [NSOpenPanel openPanel];
    p.message = message;
    p.canChooseFiles = !folder;
    p.canChooseDirectories = folder;
    p.allowsMultipleSelection = NO;
    p.treatsFilePackagesAsDirectories = YES;
    [NSApp activateIgnoringOtherApps:YES];
    if ([p runModal] != NSModalResponseOK || !p.URL.fileSystemRepresentation) return fs::path();
    return fs::path(p.URL.fileSystemRepresentation);
}

}  // namespace

@interface Dod3CopyProgress : NSObject
@property(atomic) BOOL cancelled;
- (void)cancel:(id)sender;
@end
@implementation Dod3CopyProgress
- (void)cancel:(id)sender { (void)sender; self.cancelled = YES; }
@end

namespace {

/* copy_disc on a worker thread, with a small window: what is happening, a
 * bar, Cancel. */
bool copy_with_progress(const Disc& disc, const fs::path& dest, std::string* err)
{
    NSPanel* w = [[NSPanel alloc] initWithContentRect:NSMakeRect(0, 0, 460, 130)
                                            styleMask:NSWindowStyleMaskTitled
                                              backing:NSBackingStoreBuffered
                                                defer:NO];
    w.title = kTitle;
    NSTextField* label = [NSTextField labelWithString:@"Copying the game's files…"];
    label.frame = NSMakeRect(20, 88, 420, 20);
    NSTextField* sub = [NSTextField labelWithString:ns("to " + dest.parent_path().string() + "/disc")];
    sub.frame = NSMakeRect(20, 66, 420, 18);
    sub.font = [NSFont systemFontOfSize:11];
    sub.textColor = NSColor.secondaryLabelColor;
    sub.lineBreakMode = NSLineBreakByTruncatingMiddle;
    NSProgressIndicator* bar = [[NSProgressIndicator alloc] initWithFrame:NSMakeRect(20, 42, 420, 20)];
    bar.indeterminate = NO;
    bar.minValue = 0;
    bar.maxValue = 1;
    Dod3CopyProgress* state = [[Dod3CopyProgress alloc] init];
    NSButton* cancel = [NSButton buttonWithTitle:@"Cancel" target:state action:@selector(cancel:)];
    cancel.frame = NSMakeRect(350, 8, 90, 30);
    for (NSView* v in @[ label, sub, bar, cancel ]) [w.contentView addSubview:v];
    [w center];
    [w makeKeyAndOrderFront:nil];

    std::atomic<uint64_t> done{0}, total{1};
    std::atomic<bool> finished{false};
    bool ok = false;
    std::string werr;
    std::thread worker([&] {
        ok = copy_disc(disc, dest, [&](uint64_t d, uint64_t t) {
            done = d;
            total = t ? t : 1;
            return !state.cancelled;
        }, &werr);
        finished = true;
    });
    NSModalSession s = [NSApp beginModalSessionForWindow:w];
    while (!finished) {
        [NSApp runModalSession:s];
        bar.doubleValue = (double)done / (double)total;
        usleep(50 * 1000);
    }
    [NSApp endModalSession:s];
    worker.join();
    [w orderOut:nil];
    if (!ok) *err = werr;
    return ok;
}

/* Step 1: the disc -- a decrypted ISO or a folder holding PS3_GAME, checked,
 * then copied to base/game/disc. */
bool setup_disc(const fs::path& base)
{
    for (;;) {
        const int kind = ask(@"Step 1 of 2: the game disc.\n\nIs your copy of the disc a decrypted .iso file, or the "
                             @"disc dumped to a folder (one that contains PS3_GAME)?",
                             NSAlertStyleInformational, @[ @"Choose .iso File…", @"Choose Folder…", @"Cancel" ]);
        if (kind == 2) return false;
        const fs::path src = kind == 0 ? pick(@"Choose the decrypted Drakengard 3 disc image (.iso)", false)
                                       : pick(@"Choose the folder that contains PS3_GAME", true);
        if (src.empty()) continue;
        Disc disc;
        std::string err;
        if (!open_disc(src, &disc, &err)) {
            if (ask(ns("That disc can't be used:\n\n" + err), NSAlertStyleWarning, @[ @"Try Again", @"Cancel" ]) == 1)
                return false;
            continue;
        }
        std::error_code ec;
        fs::create_directories(base / "game", ec);
        const fs::space_info sp = fs::space(base / "game", ec);
        if (!ec && sp.available < disc.total + (256ull << 20)) {
            ask(ns("Not enough free space: the game's files need " + std::to_string((disc.total >> 30) + 1) +
                   " GB on the disk that holds\n" + base.string()),
                NSAlertStyleCritical, @[ @"OK" ]);
            return false;
        }
        /* Copy to game/disc.partial, then rename: a cancelled or failed copy
         * never looks installed. */
        const fs::path part = base / "game/disc.partial", final_dir = base / "game/disc";
        fs::remove_all(part, ec);
        if (!copy_with_progress(disc, part, &err)) {
            fs::remove_all(part, ec);
            if (err == "cancelled") return false;
            if (ask(ns("Copying the disc failed:\n\n" + err), NSAlertStyleWarning, @[ @"Try Again", @"Cancel" ]) == 1)
                return false;
            continue;
        }
        fs::remove_all(final_dir, ec);
        fs::rename(part, final_dir, ec);
        if (ec) {
            ask(ns("Could not finish the copy:\n" + ec.message()), NSAlertStyleCritical, @[ @"OK" ]);
            return false;
        }
        return true;
    }
}

/* Step 2: the decrypted EBOOT.ELF, checked by its hash. */
bool setup_elf(const fs::path& base)
{
    for (;;) {
        if (ask(@"Step 2 of 2: EBOOT.ELF.\n\nChoose the EBOOT.ELF that RPCS3's Utilities › Decrypt PS3 Binaries made "
                @"from the disc's PS3_GAME/USRDIR/EBOOT.BIN.",
                NSAlertStyleInformational, @[ @"Choose EBOOT.ELF…", @"Cancel" ]) == 1)
            return false;
        const fs::path src = pick(@"Choose the decrypted EBOOT.ELF", false);
        if (src.empty()) continue;
        if (sha256_file(src) != kEbootElfSha256) {
            if (ask(@"That file doesn't match the supported release (Drakengard 3, BLUS31197, version 01.00). It must "
                    @"be the disc's PS3_GAME/USRDIR/EBOOT.BIN decrypted with RPCS3 (Utilities › Decrypt PS3 "
                    @"Binaries), with no game update applied.",
                    NSAlertStyleWarning, @[ @"Try Again", @"Cancel" ]) == 1)
                return false;
            continue;
        }
        std::error_code ec;
        fs::create_directories(base / "elf", ec);
        fs::copy_file(src, base / "elf/EBOOT.ELF", fs::copy_options::overwrite_existing, ec);
        if (ec) {
            ask(ns("Could not copy " + src.string() + ":\n" + ec.message()), NSAlertStyleCritical, @[ @"OK" ]);
            return false;
        }
        return true;
    }
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
            if (![NSFileManager.defaultManager createDirectoryAtURL:d withIntermediateDirectories:YES
                                                         attributes:nil error:nil])
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
    if (freopen("dod3.log", "w", stderr)) setvbuf(stderr, NULL, _IOLBF, 0);
    freopen("dod3.log", "a", stdout);
}

/* 0 when the game's files are in place under base (asking for them first if
 * not, or always with force); nonzero if the player cancelled. The player's
 * dod3.ini is seeded from the app's default (Contents/Resources). */
extern "C" int dod3_setup_mac(const char* base_dir, int force)
{
    const fs::path base(base_dir);
    @autoreleasepool {
        std::error_code ec;
        if (!fs::exists(base / "dod3.ini", ec)) {
            NSString* def = [NSBundle.mainBundle pathForResource:@"dod3" ofType:@"ini"];
            if (def) fs::copy_file(fs::path(def.fileSystemRepresentation), base / "dod3.ini", ec);
        }
        const Installed have = check_installed(base);
        if (have.disc && have.elf && !force) return 0;

        [NSApplication sharedApplication];
        [NSApp setActivationPolicy:NSApplicationActivationPolicyRegular];
        if (ask(ns("Drakengard 3 Recompiled needs two things from your own copy of the game:\n\n"
                   "1. The game disc (Drakengard 3, US, BLUS31197): a decrypted .iso, or the disc dumped to a "
                   "folder.\n"
                   "2. EBOOT.ELF: the disc's PS3_GAME/USRDIR/EBOOT.BIN decrypted with RPCS3 (Utilities › Decrypt "
                   "PS3 Binaries).\n\n"
                   "They are copied into\n" + base.string() + "\n(about 16 GB)."),
                NSAlertStyleInformational, @[ @"Continue", @"Cancel" ]) == 1)
            return 1;
        bool ok = true;
        if (ok && (force || !have.disc)) ok = setup_disc(base);
        if (ok && (force || !have.elf)) ok = setup_elf(base);
        if (!ok) return 1;
        ask(@"Setup is complete. The game will start now.\n\nTo run setup again, hold the Option key while opening "
            @"the app.",
            NSAlertStyleInformational, @[ @"Start" ]);
    }
    return 0;
}

/* Option held at launch: run setup again (the --setup of a double-click). */
extern "C" int dod3_mac_option_held(void)
{
    return ([NSEvent modifierFlags] & NSEventModifierFlagOption) != 0;
}
