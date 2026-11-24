/* What the launcher (src/ui/launcher.cpp) and the setup (src/ui/wizard.cpp)
 * share besides their look (ui/look.h): the files the player adds,
 * recognised on threads of their own; the install, run on one; the mods the
 * setup knows by name, as the player sees them; and a list read from the
 * environment, for the tests. */
#pragma once
#include "install/install.h"

#include <atomic>
#include <chrono>
#include <future>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace dod3setup {

/* run_launcher's and run_installer's results (ui/ui.h); kGoOn: neither yet. */
enum Result { kGoOn = -1, kPlay = 0, kQuit = 1, kGameFiles = 2 };

/* Files being recognised (identify), each on a thread of its own. */
struct FileChecks {
    struct Finished {
        Source src;         /* not set if the file was not recognised */
        std::string err;    /* why not */
        std::string name;   /* the file's name, for the player */
    };
    void add(const std::vector<fs::path>& paths);
    std::vector<Finished> take_finished();   /* those finished since the last call */
    size_t pending() const { return jobs.size(); }
    void wait();                             /* until every one has finished: at the end */

    struct Job {
        fs::path path;
        std::future<std::pair<Source, std::string>> fut;
    };
    std::vector<Job> jobs;
};

/* An install (install/install.h) on a thread of its own, and how far it is. */
struct InstallJob {
    std::atomic<bool> cancel{ false };      /* asks it to stop; it then fails, undone */
    std::atomic<uint64_t> done{ 0 }, total{ 1 };   /* bytes; total is never 0 */
    bool ok = false;                        /* once collected */
    std::string error;

    ~InstallJob() { stop(); }
    void start(const fs::path& base, const Plan& plan);
    bool collect();          /* true once, the frame it has finished (joined) */
    bool running() const { return thread.joinable(); }
    std::string what() const;   /* the step it is on; "" before the first */
    double seconds() const;     /* since it started */
    void stop();                /* cancelled and waited for: the window closed mid-install */

    std::thread thread;
    std::atomic<bool> finished{ false };
    mutable std::mutex what_mu;
    std::string step;
    std::chrono::steady_clock::time_point started;
};

/* The mods the setup knows by name, as the player sees them: each a group of
 * the parts install/install.h installs by id. */
struct ModPart {
    const char* id;
    const char* name;
    const char* needs;   /* what it is for, if not the base game */
};
struct ModGroup {
    const char* title;
    const char* by;
    const char* about;
    std::vector<ModPart> parts;
};
extern const ModGroup kSpanish, kUndub;

bool has_mod(const Status& have, std::string_view id);
std::vector<std::string> installed_parts(const ModGroup& g, const Status& have);   /* the ids */
bool known_mod(std::string_view id);   /* a part of kSpanish or kUndub */

/* Where a mod package came from, for the line under its name: its file and
 * its content ID, whichever were kept ("" if neither). */
std::string mod_source_line(const ModInfo& about);

/* The environment variable `var` split at `sep`, empty parts left out. */
std::vector<std::string> env_list(const char* var, char sep);

}  // namespace dod3setup
