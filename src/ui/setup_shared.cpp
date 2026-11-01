/* What the launcher and the setup share besides their look (ui/setup_shared.h). */
#include "ui/setup_shared.h"
#include "util.h"

#include <cstdlib>
#include <cstring>

namespace dod3setup {

void FileChecks::add(const std::vector<fs::path>& paths)
{
    for (const fs::path& p : paths)
        jobs.push_back({ p, std::async(std::launch::async, [p] {
                             Source s;
                             std::string err;
                             if (!identify(p, &s, &err)) s = Source();
                             return std::make_pair(s, err);
                         }) });
}

std::vector<FileChecks::Finished> FileChecks::take_finished()
{
    std::vector<Finished> out;
    for (size_t i = 0; i < jobs.size();) {
        if (jobs[i].fut.wait_for(std::chrono::seconds(0)) != std::future_status::ready) {
            i++;
            continue;
        }
        auto [src, err] = jobs[i].fut.get();
        const fs::path& p = jobs[i].path;
        out.push_back({ src, err, dod3::utf8(p.filename().empty() ? p : p.filename()) });
        jobs.erase(jobs.begin() + (long)i);
    }
    return out;
}

void FileChecks::wait()
{
    for (Job& j : jobs) j.fut.wait();
}

void InstallJob::start(const fs::path& base, const Plan& plan)
{
    cancel = false;
    finished = false;
    done = 0;
    total = 1;
    {
        std::lock_guard<std::mutex> g(what_mu);
        step.clear();
    }
    started = std::chrono::steady_clock::now();
    thread = std::thread([this, base, plan] {
        std::string err;
        const bool r = install(
            base, plan,
            [this](uint64_t d, uint64_t t, const std::string& w) {
                done = d;
                total = t ? t : 1;
                std::lock_guard<std::mutex> g(what_mu);
                step = w;
                return !cancel.load();
            },
            &err);
        ok = r;
        error = err;
        finished = true;
    });
}

bool InstallJob::collect()
{
    if (!thread.joinable() || !finished) return false;
    thread.join();
    return true;
}

std::string InstallJob::what() const
{
    std::lock_guard<std::mutex> g(what_mu);
    return step;
}

double InstallJob::seconds() const
{
    return std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
}

void InstallJob::stop()
{
    if (!thread.joinable()) return;
    cancel = true;
    thread.join();
}

const ModGroup kSpanish = { "Spanish Translation",
                            "Traducciones Vacuno \xC2\xB7 TraduSquare v1.1",
                            "The whole game in Spanish: text, menus, the title screen and subtitled movies. "
                            "Add the US release's downloads as they are: the two .pkg files and MOVIES.zip, "
                            "and the DLC and voice packages if you have those.",
                            { { "ES-GAME", "Story and mission text", nullptr },
                              { "ES-DATA", "Title screen and menus", nullptr },
                              { "ES-MOVIES", "Movies", nullptr },
                              { "ES-DLC", "DLC chapters", "with the DLC" },
                              { "ES-JPV", "Japanese voices' movies", "with the Japanese Voice Pack" } } };
const ModGroup kUndub = { "DLC Undub",
                          "Canzah and Slow \xC2\xB7 prototype 0.707",
                          "Japanese voices for the six DLC chapters, retimed to match. Follows the game's own "
                          "Voice setting (Change Outfits/Music > Voice). Add its folder, the one holding GAME "
                          "and ISO, with Add Folder.",
                          { { "UNDUB", "Japanese voices for the DLC chapters", nullptr } } };

bool has_mod(const Status& have, std::string_view id)
{
    for (const auto& m : have.mods)
        if (m.first == id) return true;
    return false;
}

std::vector<std::string> installed_parts(const ModGroup& g, const Status& have)
{
    std::vector<std::string> ids;
    for (const ModPart& p : g.parts)
        if (has_mod(have, p.id)) ids.push_back(p.id);
    return ids;
}

bool known_mod(std::string_view id)
{
    for (const ModGroup* g : { &kSpanish, &kUndub })
        for (const ModPart& p : g->parts)
            if (id == p.id) return true;
    return false;
}

std::vector<std::string> env_list(const char* var, char sep)
{
    std::vector<std::string> out;
    const char* v = getenv(var);
    for (const char* s = v; s && *s;) {
        const char* e = strchr(s, sep);
        const std::string one = e ? std::string(s, e) : std::string(s);
        if (!one.empty()) out.push_back(one);
        if (!e) break;
        s = e + 1;
    }
    return out;
}

}  // namespace dod3setup
