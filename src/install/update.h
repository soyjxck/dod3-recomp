/*
 * Updates from the port's GitHub releases, for the launcher (src/ui/launcher.cpp).
 * Windows only for now: the Mac's app bundle is replaced another way.
 *
 * The check: when the launcher opens (unless DOD3_UPDATE_CHECK = 0), it reads
 * the repository's list of releases -- not GitHub's "latest release", which
 * leaves pre-releases out, and every release of the port so far is one -- and
 * takes the newest whose version is above this build's and which has a
 * Windows zip with a SHA-256 digest.
 *
 * The install, once the player asks for it, in <program folder>/.update:
 *   1. the zip is downloaded and checked against its size and the digest
 *      GitHub lists for it;
 *   2. it is unpacked (install/zip.h checks every entry's CRC-32);
 *   3. each file is swapped in: the old one becomes <name>.old -- Windows lets a
 *      running program and its loaded DLLs be renamed -- and the new one takes
 *      its name. If one cannot be, every file swapped so far is put back, so the
 *      folder is as it was.
 * Never touched: game/, gamedata/, and dod3.ini when there is one (the
 * player's settings; the release's is the defaults). Then the launcher starts
 * the new dod3.exe and closes. Its first start removes the .old files and the
 * work folder (update_cleanup) and says it was updated.
 *
 * The approach follows King Kong Recompiled's updater (MIT): the release's
 * hash checked before anything is installed, files staged and then swapped
 * with every old one put back on a failure, the game and the player's data
 * never written.
 *
 * Tests: DOD3_UPDATE_AS=<version> checks as that version (0.4.0 is offered
 * the newest release); DOD3_UPDATE_API=<file> reads the release list from a
 * file instead of GitHub, and DOD3_UPDATE_ZIP=<zip> installs that file in
 * place of the download (checked against the list's digest all the same).
 */
#pragma once
#include <atomic>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <thread>

namespace dod3setup {

namespace fs = std::filesystem;

/* A release the check found. */
struct Release {
    std::string tag;      /* "v0.4.2-alpha" */
    std::string label;    /* "0.4.2 alpha", as the port names its versions */
    std::string page;     /* the release's page on GitHub */
    std::string notes;    /* its notes, as written there (Markdown) */
    std::string date;     /* "2026-11-02" */
    std::string url;      /* the Windows zip */
    std::string name;     /* the zip's file name */
    uint64_t size = 0;    /* its size */
    std::string sha256;   /* its digest, 64 lowercase hex digits */
};

/* True when version `a` ("0.4.2-alpha", "v0.4.2 alpha", "0.5.0") is above
 * `b`: the numbers first, then a final release above a beta above an alpha. */
bool version_newer(const std::string& a, const std::string& b);

/* The newest release in GitHub's release list `json` above version `current`
 * with an asset ending in `asset_end` that carries a SHA-256 digest. False
 * when there is none (`err` says why when the list itself was not readable). */
bool pick_release(const std::string& json, const std::string& current, const char* asset_end, Release* out,
                  std::string* err);

/* This build's version as the check compares it (DOD3_UPDATE_AS, else the
 * label from CMakeLists.txt). */
std::string update_current_version();

/* The check, on a thread of its own that nothing waits for: a slow answer
 * never holds up Play or Quit (the thread then ends by itself). */
struct UpdateCheck {
    bool found = false;   /* once collected */
    Release release;
    std::string error;

    void start();
    bool collect();   /* true once, the frame it has finished */

    struct State {
        std::atomic<bool> finished{ false };
        bool found = false;
        Release release;
        std::string error;
    };
    std::shared_ptr<State> state;
};

/* The download and install, on a thread of its own. */
struct UpdateJob {
    enum Step { kDownloading, kChecking, kUnpacking, kInstalling, kDone };
    std::atomic<int> step{ kDownloading };
    std::atomic<uint64_t> done{ 0 }, total{ 1 };   /* of the step: bytes, or files */
    std::atomic<bool> cancel{ false };             /* before the files are swapped in */
    bool ok = false;                               /* once collected */
    bool changed = false;                          /* a failure left the folder changed (never: put back) */
    std::string error;

    ~UpdateJob();
    void start(const fs::path& dir, const Release& r);
    bool collect();
    bool running() const { return thread.joinable(); }
    std::string what() const;   /* the step, for the progress line */

    std::thread thread;
    std::atomic<bool> finished{ false };
};

/* At start: the .old files and the work folder an update left removed (on a
 * thread, retried while the program that made them is still closing). True
 * when the last start was an update's, and *from is the version it came from. */
bool update_cleanup(const fs::path& dir, std::string* from);

/* The folder this program runs from: what an update replaces files in. */
fs::path update_program_dir();

/* Starts dir/dod3.exe as this program was started (its command line, its
 * working folder). False if it could not be. */
bool update_restart(const fs::path& dir);

/* The release page in the player's browser. */
void open_in_browser(const std::string& url);

}  // namespace dod3setup
