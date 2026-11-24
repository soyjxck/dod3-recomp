/* Files served from beside the game's own, through the runtime's path hook
 * (ps3_vfs_title_hook: every host path the title's file calls resolve to
 * passes through it). Two kinds, both installed by the setup
 * (src/install/install.h):
 *
 *  - A mod's files in game/mods, each at the path under the game tree
 *    (game/disc) it stands in for -- a translation's title screen in the
 *    game-data install, its movies on the disc, its DLC text. The player's
 *    own files stay as they are; the mod is in game/mods alone.
 *
 *  - The DLC chapters' Japanese voices (DLC_JPV/UNDUB, src/kernel/dlc.cpp).
 *    The title finds a voice-pack file by its name alone, and a few of the
 *    undub's names are in more than one chapter, or in the main game too:
 *    each chapter's dragon (ANIM_DLC_DR00_SF), One's BS070. Those are listed
 *    as /DLC_JPV/UNDUB/CHAPTER/<name> and resolved here to the copy of the
 *    chapter being played -- the one whose map was read last since the book
 *    or the title -- or, outside the chapters, to the main game's own
 *    package (a name the main game lacks: the first chapter that has it).
 *    (The undub changed each chapter's dragon for that chapter's
 *    voices, where the title itself reads one chapter's in all of them.)
 *    The undub's movies sit in UNDUB/MOVIES and are served where the title
 *    looks for the voice pack's (DLC_JPV/SQEX03GAME/MOVIES). Where a mod in
 *    game/mods has its own copy of a DLC chapter's package the undub has
 *    too -- a translation's text -- the mod's is read in Japanese as well:
 *    the undub's is English text retimed for the Japanese voices.
 *
 * Bridge 18 (src/ui/settings_menu.cpp) asks dod3_dlc_jpvoice(): with the
 * undub installed, the DLC chapters keep the Japanese voices instead of the
 * 1.01 update's switch to English there (tools/menu_patch.py). */
#include <atomic>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include "util.h"

extern "C" void (*ps3_vfs_title_hook)(char* path, size_t cap);   /* runtime/syscalls/sys_fs.c */
extern "C" int32_t ps3_sysutil_language(void);                   /* libs/system/cellSysutil.c */

namespace fs = std::filesystem;

namespace {

const int kChapters = 6;   /* ADDSCENARIO00000 (Zero's) to 00005 (Five's) */

std::string s_root;                         /* the game tree (PS3_VFS_ROOT): '/', upper case */
std::string s_mods;                         /* game/mods, as the host spells it */
std::unordered_set<std::string> s_tree;     /* paths under the game tree that game/mods has: "/GAME/..." */
std::string s_undub;                        /* DLC_JPV/UNDUB */
std::unordered_set<std::string> s_movies;   /* the undub's movies' names */
std::unordered_set<std::string> s_chapter[kChapters];   /* each chapter's own copies' names */
std::unordered_set<std::string> s_in_main_game;         /* of those names, the ones the main game has too */
std::unordered_map<std::string, std::string> s_mod_dlc;  /* a DLC package's name -> its path in game/mods */
std::atomic<int> s_playing{ -1 };           /* the DLC chapter whose map was read last, else -1 */
bool s_jpvoice = false;
bool s_spanish_menus = false;   /* the Spanish translation's menus (ES-DATA) installed and on */
std::atomic<int> s_logged{ 0 };

std::string upper_slash(const std::string& s)
{
    std::string u = s;
    for (char& c : u) c = c == '\\' ? '/' : (char)toupper((unsigned char)c);
    return u;
}

std::string chapter_dir(int i) { return "ADDSCENARIO0000" + std::to_string(i); }

/* `path` made `to`; `asked` is what the title asked for, for the log.
 * DOD3_MODS_LOG=1: every path served, not only the first 48. */
void serve(char* path, size_t cap, const std::string& to, std::string_view asked)
{
    static const bool all = getenv("DOD3_MODS_LOG") != nullptr;
    if (all || s_logged.fetch_add(1) < 48)
        fprintf(stderr, "[mods] %.*s -> %s\n", (int)asked.size(), asked.data(), to.c_str());
    snprintf(path, cap, "%s", to.c_str());
}

/* The DLC chapters' Japanese voices: true if `rel` was served from them. */
bool serve_undub(char* path, size_t cap, std::string_view rel, size_t root_len)
{
    /* the chapter being played: the one whose map was read last since the
     * book or the title. Some chapters stream main-game maps too (Two's,
     * Three's), which leave it as it is. */
    static const char kScenario[] = "/USRDIR/DLC/ADDSCENARIO0000";
    if (rel.ends_with("_MAP.XXX")) {
        const size_t d = rel.find(kScenario);
        const size_t at = d == std::string_view::npos ? d : d + sizeof kScenario - 1;
        if (at < rel.size() && rel[at] >= '0' && rel[at] < '0' + kChapters) s_playing = rel[at] - '0';
    } else if (rel.ends_with("/COOKEDPS3/BOOK.XXX") || rel.ends_with("/COOKEDPS3/TITLE.XXX")) {
        s_playing = -1;
    }
    /* a mod's own copy of a chapter's package (a translation's text) wins over the undub's */
    static const std::string_view kCooked = "/GAME/NPUB31251/USRDIR/DLC_JPV/UNDUB/COOKEDPS3/";
    if (!s_mod_dlc.empty() && rel.starts_with(kCooked)) {
        const auto m = s_mod_dlc.find(std::string(rel.substr(kCooked.size())));
        if (m != s_mod_dlc.end()) {
            serve(path, cap, s_mods + m->second, rel);
            return true;
        }
    }
    /* a name in more than one chapter, or one the main game has too */
    static const std::string_view kChapter = "/GAME/NPUB31251/USRDIR/DLC_JPV/UNDUB/CHAPTER/";
    if (rel.starts_with(kChapter)) {
        const std::string name(rel.substr(kChapter.size()));
        const int c = s_playing;
        if (c >= 0 && s_chapter[c].count(name)) {
            serve(path, cap, s_undub + "/" + chapter_dir(c) + "/" + name, rel);
        } else if (s_in_main_game.count(name)) {
            const std::string root(path, root_len);
            serve(path, cap, root + "/PS3_GAME/USRDIR/SQEX03GAME/COOKEDPS3/" + name, rel);
        } else {   /* no chapter yet, and not the main game's: the first chapter that has it */
            for (int i = 0; i < kChapters; i++)
                if (s_chapter[i].count(name)) {
                    serve(path, cap, s_undub + "/" + chapter_dir(i) + "/" + name, rel);
                    break;
                }
        }
        return true;
    }
    /* the undub's movies, where the title looks for the voice pack's */
    static const std::string_view kMovies = "/GAME/NPUB31251/USRDIR/DLC_JPV/SQEX03GAME/MOVIES/";
    if (rel.starts_with(kMovies) && s_movies.count(std::string(rel.substr(kMovies.size())))) {
        serve(path, cap, s_undub + "/MOVIES/" + std::string(rel.substr(kMovies.size())), rel);
        return true;
    }
    return false;
}

/* ps3_vfs_title_hook: every host path the title's file calls resolve to */
void hook(char* path, size_t cap)
{
    char up[1024];   /* the path upper-cased, with '/' */
    const size_t n = strlen(path), rn = s_root.size();
    if (n <= rn || n >= sizeof up) return;
    for (size_t i = 0; i < n; i++) {
        const char c = path[i];
        up[i] = c == '\\' ? '/' : (char)toupper((unsigned char)c);
    }
    if (memcmp(up, s_root.data(), rn) != 0 || up[rn] != '/') return;
    const std::string_view rel(up + rn, n - rn);   /* "/GAME/..." */
    if (s_jpvoice && serve_undub(path, cap, rel, rn)) return;
    if (!s_tree.empty() && s_tree.count(std::string(rel))) serve(path, cap, s_mods + std::string(path + rn), rel);
}

}  // namespace

/* With the DLC chapters' Japanese voices installed (and the voice pack they
 * join), bridge 18 keeps the chapters in Japanese. */
bool dod3_dlc_jpvoice() { return s_jpvoice; }

/* The language of the Settings pages (src/ui/settings_menu.cpp): "fr" with
 * the game in French (DOD3_LANGUAGE; a translation mod's text is English-named
 * and not read then); "es" with the Spanish translation's menus installed and
 * not switched off in the launcher; else "en". DOD3_MENU_LANGUAGE=en|es|fr
 * says which instead (tests). */
const char* dod3_menu_language()
{
    if (const char* l = getenv("DOD3_MENU_LANGUAGE")) return !strcmp(l, "es") ? "es" : !strcmp(l, "fr") ? "fr" : "en";
    if (ps3_sysutil_language() == 2 /* CELL_SYSUTIL_LANG_FRENCH */) return "fr";
    return s_spanish_menus ? "es" : "en";
}

/* At boot: what game/mods and DLC_JPV/UNDUB hold, and the hook if either has
 * anything. */
void dod3_mods_prepare()
{
    const char* r = getenv("PS3_VFS_ROOT");
    if (!r || !*r) return;
    std::error_code ec;
    const fs::path root = fs::u8path(r);
    s_root = upper_slash(r);
    while (!s_root.empty() && s_root.back() == '/') s_root.pop_back();

    const fs::path mods = root.parent_path() / "mods";
    s_mods = dod3::utf8(mods);
    /* game/mods/installed.txt has a line a mod, "<id>\t..." (src/install/install.cpp);
     * a mod switched off has game/modfiles/<id>.off */
    if (FILE* f = dod3::open_file(mods / "installed.txt", "rb")) {
        char line[512];
        while (fgets(line, sizeof line, f))
            if (!strncmp(line, "ES-DATA\t", 8)) s_spanish_menus = !fs::exists(root.parent_path() / "modfiles/ES-DATA.off", ec);
        fclose(f);
    }
    for (fs::recursive_directory_iterator it(mods, ec), end; !ec && it != end; it.increment(ec)) {
        if (!it->is_regular_file(ec)) continue;
        const std::string rel = dod3::utf8(fs::relative(it->path(), mods, ec));
        if (rel == "installed.txt") continue;
        const std::string key = "/" + upper_slash(rel);
        s_tree.insert(key);
        if (key.starts_with("/GAME/NPUB31251/USRDIR/DLC/"))
            s_mod_dlc[upper_slash(dod3::utf8(it->path().filename()))] = "/" + rel;
    }

    const fs::path jpv = root / "game/NPUB31251/USRDIR/DLC_JPV", undub = jpv / "UNDUB";
    if (fs::exists(undub / "UNDUB.TXT", ec) && fs::is_directory(jpv / "SQEX03GAME", ec)) {
        s_undub = dod3::utf8(undub);
        for (fs::directory_iterator it(undub / "MOVIES", ec), end; !ec && it != end; it.increment(ec))
            if (it->is_regular_file(ec)) s_movies.insert(upper_slash(dod3::utf8(it->path().filename())));
        const fs::path cooked = root / "PS3_GAME/USRDIR/SQEX03GAME/COOKEDPS3";
        size_t by_chapter = 0;
        for (int i = 0; i < kChapters; i++) {
            for (fs::directory_iterator it(undub / chapter_dir(i), ec), end; !ec && it != end;
                 it.increment(ec)) {
                if (!it->is_regular_file(ec)) continue;
                const std::string name = upper_slash(dod3::utf8(it->path().filename()));
                s_chapter[i].insert(name);
                if (fs::exists(cooked / it->path().filename(), ec)) s_in_main_game.insert(name);
            }
            by_chapter += s_chapter[i].size();
        }
        s_jpvoice = true;
        fprintf(stderr, "[mods] the DLC chapters' Japanese voices: %zu movies, %zu packages served by chapter\n",
                s_movies.size(), by_chapter);
    }
    if (!s_tree.empty()) fprintf(stderr, "[mods] %zu files from %s\n", s_tree.size(), s_mods.c_str());
    if (s_jpvoice || !s_tree.empty()) ps3_vfs_title_hook = hook;
}
