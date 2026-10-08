/* The settings-menu patch, applied to the player's own SQEX03GAME.XXX.
 *
 * tools/menu_patch.py rewrites some script functions of the title's script
 * package (`--show` lists them; src/dod3_settings_menu.cpp has what they do:
 * the Graphics and System pages, skip intro, the field of view, the pause
 * screen left clear behind the Graphics page) and records them in
 * src/dod3_menu_patch_data.h as edits of the original functions: ranges to
 * copy from each original body and the bytes that are new. At boot this file
 * checks the player's package against the hash the EBOOT itself carries for
 * it, rebuilds the patched package (the new bodies appended, their export
 * table entries pointed at them), and writes it (with its TOC line, on 1.00)
 * to game/patch or game/patch101 -- a tree PS3_VFS_OVERLAY lays over the
 * disc. That is done once; a stamp file names the patch version it was made
 * with.
 *
 * On 1.00 two copies of the package go into the overlay: the title reads its
 * packages from the game-data install (/dev_hdd0/game/BLES00000DATA/USRDIR/
 * FIOS-UNREALENGINE3/...), the TOC from the disc. On 1.01 the package is the
 * update's. The engine checks the loaded script package against a SHA-1
 * table in the EBOOT, so the patched one's hash is written into that table
 * (dod3_sha_override in main.cpp).
 *
 * DOD3_MENU_PATCH=0 leaves the menu as the title shipped it. */
#include <zlib.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <filesystem>
#include <string>
#include <vector>
#include "dod3_eboot.h"
#include "dod3_util.h"
#include "setup_crypto.h"
#if DOD3_EBOOT == 101
/* 1.01: the update's package (its PATCH folder), which its EBOOT's table
 * names; patch files have no TOC line. The overlay is game/patch101, so a
 * 1.00 build and a 1.01 build never see each other's patched package. */
#include "dod3_menu_patch_data_101.h"
#define MENU_OVERLAY "patch101"
#else
#include "dod3_menu_patch_data.h"
#define MENU_OVERLAY "patch"
#endif

bool dod3_sha_override(const char* name, const uint8_t sha[20]);   /* main.cpp */

namespace fs = std::filesystem;

namespace {

using dod3::utf8;

std::string sha1_hex(const std::vector<uint8_t>& d)
{
    dod3setup::Sha1 s;
    s.update(d.data(), d.size());
    return s.hex();
}

uint32_t le32(const uint8_t* p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }

bool read_file(const fs::path& p, std::vector<uint8_t>& out)
{
    FILE* f = dod3::open_file(p, "rb");
    if (!f) return false;
    fseek(f, 0, SEEK_END);
    const long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    out.resize(n > 0 ? (size_t)n : 0);
    const bool ok = n >= 0 && fread(out.data(), 1, out.size(), f) == out.size();
    fclose(f);
    return ok;
}

bool write_file(const fs::path& p, const void* d, size_t n)
{
    std::error_code ec;
    fs::create_directories(p.parent_path(), ec);
    fs::path tmp = p;
    tmp += ".partial";
    FILE* f = dod3::open_file(tmp, "wb");
    if (!f) return false;
    const bool ok = fwrite(d, 1, n, f) == n;
    if (fclose(f) != 0 || !ok) return false;
    fs::rename(tmp, p, ec);
    return !ec;
}

const uint32_t PACKAGE_TAG = 0x9E2A83C1u;

/* A fully compressed package: tag, block size, total compressed and
 * uncompressed sizes, a (compressed, uncompressed) pair a block, the blocks. */
bool inflate_full(const std::vector<uint8_t>& f, std::vector<uint8_t>& u, uint32_t& bs)
{
    if (f.size() < 16 || dod3_be32(&f[0]) != PACKAGE_TAG) return false;
    bs = dod3_be32(&f[4]);
    const uint32_t usz = dod3_be32(&f[12]);
    if (!bs) return false;
    const size_t nb = (usz + bs - 1) / bs;
    if (f.size() < 16 + 8 * nb) return false;
    u.resize(usz);
    size_t src = 16 + 8 * nb, dst = 0;
    for (size_t i = 0; i < nb; i++) {
        const uint32_t c = dod3_be32(&f[16 + 8 * i]), b = dod3_be32(&f[20 + 8 * i]);
        if (src + c > f.size() || dst + b > u.size()) return false;
        uLongf out = b;
        if (uncompress(&u[dst], &out, &f[src], c) != Z_OK || out != b) return false;
        src += c; dst += b;
    }
    return dst == usz;
}

std::vector<uint8_t> deflate_full(const std::vector<uint8_t>& u, uint32_t bs)
{
    const size_t nb = (u.size() + bs - 1) / bs;
    std::vector<uint8_t> out(16 + 8 * nb);
    dod3_put_be32(&out[0], PACKAGE_TAG);
    dod3_put_be32(&out[4], bs);
    dod3_put_be32(&out[12], (uint32_t)u.size());
    uint32_t total = 0;
    std::vector<uint8_t> tmp(compressBound(bs));
    for (size_t i = 0; i < nb; i++) {
        const size_t off = i * bs, b = u.size() - off < bs ? u.size() - off : bs;
        uLongf c = (uLongf)tmp.size();
        compress2(tmp.data(), &c, &u[off], (uLong)b, Z_BEST_SPEED);
        dod3_put_be32(&out[16 + 8 * i], (uint32_t)c);
        dod3_put_be32(&out[20 + 8 * i], (uint32_t)b);
        out.insert(out.end(), tmp.begin(), tmp.begin() + c);
        total += (uint32_t)c;
    }
    dod3_put_be32(&out[8], total);
    return out;
}

/* The byte offset of export `idx`'s (1-based) table entry. Summary (v860):
 * tag, version, header size, folder name (FString), package flags, name
 * count/offset, export count/offset, ... */
size_t export_entry(const std::vector<uint8_t>& u, uint32_t idx)
{
    size_t p = 12;
    const int32_t fl = (int32_t)dod3_be32(&u[p]); p += 4;
    p += fl >= 0 ? (size_t)fl : (size_t)(-fl) * 2;
    p += 4 + 8;                             /* package flags, names */
    const uint32_t n = dod3_be32(&u[p]), off = dod3_be32(&u[p + 4]);
    if (idx == 0 || idx > n) return 0;
    p = off;
    for (uint32_t i = 1; i < idx; i++) {
        /* class, super, outer, name (2), archetype, object flags (8), size,
         * offset, export flags, net-object counts (n + array), guid, package flags */
        const uint32_t nn = dod3_be32(&u[p + 44]);
        p += 48 + 4 * (size_t)nn + 16 + 4;
        if (p + 48 > u.size()) return 0;
    }
    return p;
}

bool apply_patch(std::vector<uint8_t>& u)
{
    const uint8_t* s = k_menu_patch_ops;
    const uint8_t* end = s + sizeof k_menu_patch_ops;
    for (unsigned k = 0; k < k_menu_patch_count; k++) {
        if (s + 16 > end) return false;
        const uint32_t idx = le32(s), oldsz = le32(s + 4), newsz = le32(s + 8), nops = le32(s + 12);
        s += 16;
        const size_t e = export_entry(u, idx);
        if (!e) return false;
        const uint32_t size = dod3_be32(&u[e + 32]), off = dod3_be32(&u[e + 36]);
        if (size != oldsz || (size_t)off + size > u.size()) return false;
        std::vector<uint8_t> body;
        body.reserve(newsz);
        for (uint32_t i = 0; i < nops; i++) {
            if (s + 5 > end) return false;
            if (*s == 0) {
                const uint32_t a = le32(s + 1), n = le32(s + 5);
                if (s + 9 > end || (size_t)a + n > oldsz) return false;
                body.insert(body.end(), u.begin() + off + a, u.begin() + off + a + n);
                s += 9;
            } else {
                const uint32_t n = le32(s + 1);
                if (s + 5 + n > end) return false;
                body.insert(body.end(), s + 5, s + 5 + n);
                s += 5 + n;
            }
        }
        if (body.size() != newsz) return false;
        const uint32_t at = (uint32_t)u.size();
        u.insert(u.end(), body.begin(), body.end());
        dod3_put_be32(&u[e + 32], newsz);
        dod3_put_be32(&u[e + 36], at);
    }
    return true;
}

/* PS3TOC.TXT: "<size> <uncompressed size> <path> 0" a line. */
bool toc_rewrite(const std::vector<uint8_t>& in, std::string& out, uint32_t csize, uint32_t usize)
{
    std::string t(in.begin(), in.end());
    int hits = 0;
    size_t p = 0;
    while (p < t.size()) {
        size_t e = t.find('\n', p);
        if (e == std::string::npos) e = t.size();
        std::string line = t.substr(p, e - p);
        const bool cr = !line.empty() && line.back() == '\r';
        if (cr) line.pop_back();
        std::string low = line;
        for (char& c : low) c = (char)tolower((unsigned char)c);
        const char* pkg = "\\sqex03game.xxx 0";
        const char* side = "\\sqex03game.xxx.uncompressed_size 0";
        const size_t sp1 = line.find(' '), sp2 = sp1 == std::string::npos ? sp1 : line.find(' ', sp1 + 1);
        std::string repl;
        if (sp2 != std::string::npos && low.size() > strlen(pkg) && !low.compare(low.size() - strlen(pkg), strlen(pkg), pkg))
            repl = std::to_string(csize) + " " + std::to_string(usize) + line.substr(sp2);
        else if (sp2 != std::string::npos && low.size() > strlen(side) && !low.compare(low.size() - strlen(side), strlen(side), side))
            repl = std::to_string(std::to_string(usize).size() + 2) + line.substr(sp1);
        if (!repl.empty()) {
            t.replace(p, line.size(), repl);
            e = p + repl.size() + (cr ? 1 : 0);
            hits++;
        }
        p = e + 1;
    }
    out = t;
    return hits == 2;
}

/* The player's script package at `pkg`, checked to be the one the patch was
 * made from, patched, and compressed again (`z`; `usize` uncompressed). */
bool patched_package(const fs::path& pkg, std::vector<uint8_t>* z, uint32_t* usize)
{
    std::vector<uint8_t> f, u;
    uint32_t bs = 0;
    if (!read_file(pkg, f)) {
        fprintf(stderr, "[menu] the script package is missing: %s\n", utf8(pkg).c_str());
        return false;
    }
    if (!inflate_full(f, u, bs)) { fprintf(stderr, "[menu] SQEX03GAME.XXX: not a compressed package\n"); return false; }
    f.clear();
    if (sha1_hex(u) != k_menu_patch_orig_sha1) {
        fprintf(stderr, "[menu] SQEX03GAME.XXX is not the BLUS31197 %s one; the settings menu stays as shipped\n", k_menu_patch_eboot);
        return false;
    }
    if (!apply_patch(u) || u.size() != k_menu_patch_size || sha1_hex(u) != k_menu_patch_sha1) {
        fprintf(stderr, "[menu] the patch did not apply\n");
        return false;
    }
    *z = deflate_full(u, bs);
    *usize = (uint32_t)u.size();
    return true;
}

#if DOD3_EBOOT == 101
const fs::path k_cooked101 = fs::path("game") / "BLES00000" / "USRDIR" / "PATCH" / "SQEX03GAME" / "COOKEDPS3";

bool generate(const fs::path& root, const fs::path& ov)
{
    std::vector<uint8_t> z;
    uint32_t usize = 0;
    if (!patched_package(root / k_cooked101 / "SQEX03GAME.XXX", &z, &usize)) return false;
    const std::string side = std::to_string(usize) + "\r\n";
    return write_file(ov / k_cooked101 / "SQEX03GAME.XXX", z.data(), z.size()) &&
           write_file(ov / k_cooked101 / "SQEX03GAME.XXX.UNCOMPRESSED_SIZE", side.data(), side.size());
}

bool generated(const fs::path& ov)
{
    std::error_code ec;
    return fs::exists(ov / k_cooked101 / "SQEX03GAME.XXX", ec);
}
#else
bool generate(const fs::path& root, const fs::path& ov)
{
    const fs::path game = root / "PS3_GAME" / "USRDIR" / "SQEX03GAME";
    std::vector<uint8_t> toc, z;
    uint32_t usize = 0;
    if (!read_file(game / "PS3TOC.TXT", toc)) {
        fprintf(stderr, "[menu] PS3TOC.TXT is missing under %s\n", utf8(game).c_str());
        return false;
    }
    if (!patched_package(game / "COOKEDPS3" / "SQEX03GAME.XXX", &z, &usize)) return false;
    std::string toc2;
    if (!toc_rewrite(toc, toc2, (uint32_t)z.size(), usize)) {
        fprintf(stderr, "[menu] PS3TOC.TXT has no SQEX03GAME.XXX line\n");
        return false;
    }
    const std::string side = std::to_string(usize) + "\r\n";
    const fs::path cooked[2] = {
        ov / "PS3_GAME" / "USRDIR" / "SQEX03GAME" / "COOKEDPS3",
        ov / "game" / "BLES00000DATA" / "USRDIR" / "FIOS-UNREALENGINE3" / "SQEX03GAME" / "COOKEDPS3",
    };
    for (const fs::path& c : cooked)
        if (!write_file(c / "SQEX03GAME.XXX", z.data(), z.size()) ||
            !write_file(c / "SQEX03GAME.XXX.UNCOMPRESSED_SIZE", side.data(), side.size()))
            return false;
    return write_file(ov / "PS3_GAME" / "USRDIR" / "SQEX03GAME" / "PS3TOC.TXT", toc2.data(), toc2.size());
}

bool generated(const fs::path& ov)
{
    std::error_code ec;
    return fs::exists(ov / "PS3_GAME" / "USRDIR" / "SQEX03GAME" / "COOKEDPS3" / "SQEX03GAME.XXX", ec) &&
           fs::exists(ov / "game" / "BLES00000DATA" / "USRDIR" / "FIOS-UNREALENGINE3" / "SQEX03GAME" / "COOKEDPS3" / "SQEX03GAME.XXX", ec);
}
#endif

}  // namespace

/* At boot, after the EBOOT is in guest memory and before the title runs:
 * make (once) and lay over the patched package. False leaves the title's own. */
bool dod3_menu_patch_prepare()
{
    const char* off = getenv("DOD3_MENU_PATCH");
    if (off && !strcmp(off, "0")) return false;
    if (getenv("PS3_VFS_OVERLAY")) return false;   /* someone else's overlay (tools/menu_patch.py tests) */
    const char* r = getenv("PS3_VFS_ROOT");
    if (!r || !*r) return false;
    const fs::path root(r);
    const fs::path ov = root.parent_path().empty() ? fs::path(MENU_OVERLAY) : root.parent_path() / MENU_OVERLAY;
    const fs::path stamp = ov / "menu_patch.stamp";
    const std::string want = std::string(k_menu_patch_version) + " " + k_menu_patch_sha1;
    std::vector<uint8_t> have;
    std::error_code ec;
    const bool fresh = read_file(stamp, have) && std::string(have.begin(), have.end()) == want && generated(ov);
    if (!fresh) {
        fprintf(stderr, "[menu] adding Graphics Settings to the title's Settings menu (%s)...\n", utf8(ov).c_str());
        fs::remove(stamp, ec);
        if (!generate(root, ov) || !write_file(stamp, want.data(), want.size())) return false;
    }
    std::vector<uint8_t> sha;
    if (!dod3setup::from_hex(k_menu_patch_sha1, &sha) || sha.size() != 20 || !dod3_sha_override("sqex03game.xxx", sha.data()))
        return false;
    const std::string ovs = utf8(ov);
    dod3_setenv("PS3_VFS_OVERLAY", ovs.c_str(), 1);
    fprintf(stderr, "[menu] Graphics Settings in the Settings menu (overlay %s)\n", ovs.c_str());
    return true;
}
