/* Installed DLC.
 *
 * Drakengard 3's DLC (UP0082-NPUB31251, installed under
 * /dev_hdd0/game/NPUB31251/USRDIR as the console installs it) is plain UE3
 * content the title reads as it is. Of the EDATs that come with it the title
 * reads little:
 *   - each pack's engine INI is empty (libs/filesystem/edat.c serves an
 *     empty EDAT as an empty file);
 *   - the 18-byte marker is never read;
 *   - the Japanese voice pack's two file lists, DLC_JPV/JPV_PKG_FILES.TXT
 *     and JPV_NON_PKG_FILES.TXT(.EDAT), are what the voice switch reads to
 *     redirect the game's sound, animation and texture packages to
 *     DLC_JPV. The title looks for the plain .TXT before the .EDAT.
 *
 * So the plain lists are written here, from the files the pack installed:
 * one line per file, "/DLC_JPV/" + its path under DLC_JPV + CRLF, sorted --
 * the .XXX packages in JPV_PKG_FILES.TXT, the rest (two TFCs) in
 * JPV_NON_PKG_FILES.TXT; movies are found by name. That is the lists' own
 * format: the EDATs' headers give their sizes, 453593 and 126 bytes, and
 * these come to exactly that. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <algorithm>
#include <filesystem>
#include <string>
#include <vector>
#include "util.h"
#include "install/install.h"

namespace fs = std::filesystem;

namespace {

std::string upper_ext(const fs::path& p)
{
    std::string e = dod3::utf8(p.extension());
    for (char& c : e) c = (char)toupper((unsigned char)c);
    return e;
}

bool write_list(const fs::path& out, const std::vector<std::string>& lines)
{
    std::string text;
    for (const std::string& l : lines) text += l + "\r\n";
    FILE* f = dod3::open_file(out, "wb");
    if (!f) return false;
    const bool ok = fwrite(text.data(), 1, text.size(), f) == text.size();
    fclose(f);
    return ok;
}

}  // namespace

/* The Japanese voice pack's file lists, if it is installed under the game
 * tree `game_root` (game/disc) and they are missing (or `force`). Also the
 * installer's, right after it installs the pack. */
bool dod3setup::dlc_write_jpv_lists(const fs::path& game_root, bool force)
{
    const fs::path jpv = game_root / "game" / "NPUB31251" / "USRDIR" / "DLC_JPV";
    std::error_code ec;
    if (!fs::is_directory(jpv / "SQEX03GAME", ec)) return false;
    const fs::path pkg_list = jpv / "JPV_PKG_FILES.TXT", non_list = jpv / "JPV_NON_PKG_FILES.TXT";
    if (!force && fs::exists(pkg_list, ec) && fs::exists(non_list, ec)) return true;
    std::vector<std::string> pkgs, others;
    for (fs::recursive_directory_iterator it(jpv / "SQEX03GAME", ec), end; !ec && it != end; it.increment(ec)) {
        if (!it->is_regular_file(ec)) continue;
        const std::string ext = upper_ext(it->path());
        if (ext == ".BIK" || ext == ".EDAT" || ext == ".TXT") continue;
        const auto rel = fs::relative(it->path(), jpv, ec).generic_u8string();
        (ext == ".XXX" ? pkgs : others).push_back("/DLC_JPV/" + std::string(rel.begin(), rel.end()));
    }
    std::sort(pkgs.begin(), pkgs.end());
    std::sort(others.begin(), others.end());
    if (pkgs.empty()) return false;
    if (write_list(pkg_list, pkgs) && write_list(non_list, others)) {
        fprintf(stderr, "[dlc] Japanese voice pack: wrote its file lists (%zu packages, %zu other files)\n",
                pkgs.size(), others.size());
        return true;
    }
    fprintf(stderr, "[dlc] Japanese voice pack: could not write its file lists in %s\n", dod3::utf8(jpv).c_str());
    return false;
}

/* At boot: the lists, if the pack is installed and they are missing (a pack
 * copied in by hand). */
void dod3_dlc_prepare()
{
    const char* r = getenv("PS3_VFS_ROOT");
    if (r && *r) dod3setup::dlc_write_jpv_lists(fs::path(r), false);
}
