/* The installer's core: see install/install.h. */
#include "install/install.h"
#include "install/iso.h"
#include "install/pkg.h"
#include "install/crypto.h"
#include "cpu/eboot.h"
#include "util.h"

#include <cstdio>
#include <cstring>
#include <algorithm>
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <winioctl.h>
#endif

namespace dod3setup {

namespace {

/* The 1.01 update package: its content ID, and the SHA-1 its last 32 bytes
 * carry (= the checksum Sony's update server lists for it). */
const char kUpdateId[] = "UP0082-BLUS31197_00-DOD3PATCH0000000";
const char kUpdateSha1[] = "d71bc929d53703bf6899cb18d6041bcc7ecb5419";
const char kDlcPrefix[] = "UP0082-NPUB31251_00-";
/* SHA-256 of the update's EBOOT.BIN as an ELF (1.01); the disc's (1.00) is
 * install/iso.h's kEbootElfSha256. */
const char kElf101Sha256[] = "ed89e80f2336a948074c32151d2cadfb6965d174a15d9c067aeeff97c21e7e6c";

using dod3::utf8;

fs::path update_dir(const fs::path& base) { return base / "game/disc/game/BLES00000"; }
fs::path dlc_dir(const fs::path& base) { return base / "game/disc/game/NPUB31251"; }
fs::path staging(const fs::path& base) { return base / "game/setup.partial"; }

/* a file's first four bytes, big-endian, and the next two in `rev` */
uint32_t magic(const fs::path& p, uint16_t* rev = nullptr)
{
    FILE* f = dod3::open_file(p, "rb");
    uint8_t m[6] = {};
    if (f) {
        if (fread(m, 1, 6, f) != 6) m[0] = 0;
        fclose(f);
    }
    if (rev) *rev = (uint16_t)(m[4] << 8 | m[5]);
    return (uint32_t)m[0] << 24 | (uint32_t)m[1] << 16 | (uint32_t)m[2] << 8 | m[3];
}

/* a package's revision: 0x8000 is retail (PSN), 0 debug */
bool retail(const fs::path& p)
{
    uint16_t rev = 0;
    magic(p, &rev);
    return (rev & 0x8000) != 0;
}

/* Move everything under `from` into `to`, file by file (same volume: each a
 * rename), replacing what is there; then remove `from`. */
bool merge_move(const fs::path& from, const fs::path& to, std::string* err)
{
    std::error_code ec;
    fs::create_directories(to, ec);
    for (fs::directory_iterator it(from, ec), end; it != end && !ec; it.increment(ec)) {
        const fs::path t = to / it->path().filename();
        if (it->is_directory(ec)) {
            if (!merge_move(it->path(), t, err)) return false;
            continue;
        }
        if (fs::is_directory(t, ec)) fs::remove_all(t, ec);
        fs::rename(it->path(), t, ec);
        if (ec) {
            *err = "could not move " + utf8(it->path().filename()) + " into place: " + ec.message();
            return false;
        }
    }
    if (ec) {
        *err = "could not read " + utf8(from) + ": " + ec.message();
        return false;
    }
    fs::remove(from, ec);
    return true;
}

/* ---- links onto the disc (the game-data layout) -------------------------- */

/* Is `p` a link (a junction or a symlink), whatever it points at? */
bool is_link(const fs::path& p)
{
#ifdef _WIN32
    const DWORD a = GetFileAttributesW(p.c_str());
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_REPARSE_POINT);
#else
    std::error_code ec;
    return fs::is_symlink(fs::symlink_status(p, ec));
#endif
}

/* Remove the link itself, never what it points at. */
bool remove_link(const fs::path& p)
{
#ifdef _WIN32
    return RemoveDirectoryW(p.c_str()) != 0;
#else
    std::error_code ec;
    return fs::remove(p, ec);
#endif
}

/* A directory link at `link` onto `target`. On Windows a junction (unlike a
 * symlink it needs no privilege; it holds an absolute path, so a moved folder
 * is repaired at the next boot), elsewhere a relative symlink. */
bool make_link(const fs::path& target, const fs::path& link)
{
    std::error_code ec;
#ifdef _WIN32
    const std::wstring abs = fs::absolute(target, ec).wstring();
    if (abs.size() < 3 || abs[1] != L':') return false;   /* a junction needs a drive path */
    const std::wstring sub = L"\\??\\" + abs;
    if (!CreateDirectoryW(link.c_str(), nullptr)) return false;
    HANDLE h = CreateFileW(link.c_str(), GENERIC_WRITE, 0, nullptr, OPEN_EXISTING,
                           FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_BACKUP_SEMANTICS, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        RemoveDirectoryW(link.c_str());
        return false;
    }
    /* REPARSE_DATA_BUFFER, mount-point form (ntifs.h): tag, data length,
     * reserved; substitute name offset/length, print name offset/length
     * (bytes); the two names, each NUL-terminated */
    const size_t subb = sub.size() * 2, absb = abs.size() * 2;
    std::vector<uint8_t> buf(16 + subb + 2 + absb + 2);
    auto put16 = [&](size_t o, size_t v) {
        buf[o] = (uint8_t)v;
        buf[o + 1] = (uint8_t)(v >> 8);
    };
    const DWORD tag = IO_REPARSE_TAG_MOUNT_POINT;
    memcpy(&buf[0], &tag, 4);
    put16(4, buf.size() - 8);
    put16(8, 0);
    put16(10, subb);
    put16(12, subb + 2);
    put16(14, absb);
    memcpy(&buf[16], sub.c_str(), subb);
    memcpy(&buf[16 + subb + 2], abs.c_str(), absb);
    DWORD got = 0;
    const BOOL ok =
        DeviceIoControl(h, FSCTL_SET_REPARSE_POINT, buf.data(), (DWORD)buf.size(), nullptr, 0, &got, nullptr);
    CloseHandle(h);
    if (!ok) RemoveDirectoryW(link.c_str());
    return ok != 0;
#else
    fs::create_directory_symlink(fs::relative(target, link.parent_path(), ec), link, ec);
    return !ec;
#endif
}

size_t count_files(const fs::path& d)
{
    std::error_code ec;
    size_t n = 0;
    for (fs::recursive_directory_iterator it(d, ec), end; it != end && !ec; it.increment(ec))
        if (it->is_regular_file(ec)) n++;
    return n;
}

/* ---- replacing the disc ----------------------------------------------- */

/* What is the disc's in game/disc; everything else there (game/: the update,
 * the DLC, the game data and its links onto the disc; cache/) is kept. */
bool disc_entry(const fs::path& name)
{
    const std::string n = utf8(name);
    return n == "PS3_GAME" || n == "PS3_DISC.SFB" || n == "PS3_UPDATE";
}

/* A disc swap moves the old game/disc to staging/old, the new copy in, then
 * the old copy's own entries back. Finish one that was cut short, or put back
 * what a failed one moved. False if something could not be put back; it then
 * stays in staging/old, and nothing there is deleted. */
bool settle_old(const fs::path& base, std::string* err)
{
    std::error_code ec;
    const fs::path old = staging(base) / "old", final_dir = base / "game/disc";
    if (!fs::exists(old, ec)) return true;
    if (!fs::exists(final_dir, ec)) {
        fs::rename(old, final_dir, ec);
        if (ec) {
            *err = "could not put the previous copy back from " + utf8(old) + ": " + ec.message();
            return false;
        }
        return true;
    }
    std::vector<fs::path> keep;
    for (fs::directory_iterator it(old, ec), end; it != end && !ec; it.increment(ec))
        if (!disc_entry(it->path().filename())) keep.push_back(it->path().filename());
    for (const fs::path& n : keep) {
        if (fs::exists(final_dir / n, ec)) {
            *err = utf8(n) + " is both in the new copy and in " + utf8(old);
            return false;
        }
        fs::rename(old / n, final_dir / n, ec);
        if (ec) {
            *err = "could not move " + utf8(n) + " back from " + utf8(old) + ": " + ec.message();
            return false;
        }
    }
    fs::remove_all(old, ec);   /* only the old disc's own files are left */
    return true;
}

/* A DLC package that holds only licence files (.EDAT): NoPayStation's "FIX"
 * packages, which swap the packs' licences for ones a free RAP unlocks. The
 * port reads the DLC without licences, so they are not wanted. */
bool licence_only(const Pkg& pkg)
{
    for (const PkgEntry& e : pkg.entries) {
        if (e.dir || e.name.rfind("USRDIR/", 0) != 0) continue;
        const size_t n = e.name.size();
        if (n < 5 || strcmp(e.name.c_str() + n - 5, ".EDAT") != 0) return false;
    }
    return true;
}

}  // namespace

const std::vector<DlcInfo>& known_dlc()
{
    static const std::vector<DlcInfo> k = {
        { "ADDJAPANESEVOICE", "Japanese Voice Pack" },
        { "ADDSCENARIO00000", "Zero's Prologue" },
        { "ADDSCENARIO00001", "One's Prologue" },
        { "ADDSCENARIO00002", "Two's Prologue" },
        { "ADDSCENARIO00003", "Three's Prologue" },
        { "ADDSCENARIO00004", "Four's Prologue" },
        { "ADDSCENARIO00005", "Five's Prologue" },
        { "ADDSONG000000001", "Drakengard BGM Remix Pack" },
        { "ADDSONG000000002", "NieR BGM Remix Pack" },
        { "ADDSONG000000003", "BGM Remix Pack" },
        { "ADDWEAPONCLOTHE0", "Zero's Garb (Variety Pack)" },
        { "ADDWEAPONCLOTHE1", "Caim's Garb" },
        { "ADDWEAPONCLOTHE2", "Furiae's Garb" },
        { "ADDWEAPONCLOTHE3", "Manah's Garb" },
        { "ADDWEAPONCLOTHE4", "Eris's Garb" },
        { "ADDWEAPONCLOTHE5", "Nier's Garb" },
        { "ADDWEAPONCLOTHE6", "Kain\xC3\xA9's Garb" },
        { "ADDWEAPONCLOTHE7", "Beautiful Child" },
        { "ADDWEAPONCLOTHE8", "Tokyo Tower" },
        { "ADDWEAPONCLOTHE9", "Experimental Weapon 7" },
    };
    return k;
}

bool update_required() { return DOD3_EBOOT == 101; }
const char* eboot_path() { return DOD3_EBOOT == 101 ? "elf/EBOOT_101.ELF" : "elf/EBOOT.ELF"; }

uint64_t Plan::bytes() const
{
    uint64_t n = disc.bytes + update.bytes + eboot.bytes;
    for (const Source& d : dlc) n += d.bytes;
    return n;
}

uint64_t free_bytes(const fs::path& base)
{
    std::error_code ec;
    fs::path p = fs::absolute(base, ec);
    while (!p.empty() && !fs::exists(p, ec) && p != p.parent_path()) p = p.parent_path();
    const fs::space_info s = fs::space(p, ec);
    return ec ? 0 : s.available;
}

Status installed(const fs::path& base)
{
    std::error_code ec;
    Status s;
    s.disc = disc_installed(base);
    s.update = fs::exists(update_dir(base) / "USRDIR/PATCH/SQEX03GAME/COOKEDPS3/COALESCED_INT.BIN", ec);
    s.eboot = fs::file_size(base / eboot_path(), ec) > 0 && !ec;
    s.eboot_bin = fs::exists(
        update_required() ? update_dir(base) / "USRDIR/EBOOT.BIN" : base / "game/disc/PS3_GAME/USRDIR/EBOOT.BIN", ec);
    for (const DlcInfo& d : known_dlc())
        if (fs::is_directory(dlc_dir(base) / "USRDIR/DLC" / d.id, ec)) s.dlc.insert(d.id);
    return s;
}

bool gamedata_layout(const fs::path& root)
{
    std::error_code ec;
    const fs::path disc_game = root / "PS3_GAME/USRDIR/SQEX03GAME";
    if (!fs::is_directory(disc_game, ec)) return false;
    const fs::path data = root / "game/BLES00000DATA";
    const fs::path target = data / "USRDIR/FIOS-UNREALENGINE3/SQEX03GAME";
    static const char* const kFolders[] = { "COOKEDSOUND", "COOKEDPS3" };
    bool ok = true;
    fs::create_directories(target, ec);
    if (!fs::exists(data / "ICON0.PNG", ec)) fs::copy_file(root / "PS3_GAME/ICON0.PNG", data / "ICON0.PNG", ec);
    for (const char* f : kFolders) {
        const fs::path link = target / f, src = disc_game / f;
        if (is_link(link)) {   /* before is_directory: that follows the link */
            if (fs::is_directory(link, ec)) continue;
            fprintf(stderr, "[gamedata] %s points nowhere (was the folder moved?): linking it again\n", f);
            remove_link(link);
        } else if (fs::is_directory(link, ec)) {
            /* the title's own copy: whole, or cut short (the title would then
             * skip its install and miss the files) */
            if (count_files(link) >= count_files(src)) continue;
            fprintf(stderr, "[gamedata] %s is a copy that was cut short: replacing it with a link\n", f);
            const fs::path cut = target / (std::string(f) + ".partial");
            fs::rename(link, cut, ec);
            if (ec) {
                ok = false;
                break;
            }
            fs::remove_all(cut, ec);   /* a real folder, no links in it */
        }
        if (!make_link(src, link)) {
            ok = false;
            break;
        }
        fprintf(stderr, "[gamedata] %s: linked to the disc's (no copy needed)\n", f);
    }
    if (!ok) {
        /* no links here (exFAT, a network drive): leave no tree, so the title
         * installs the game data itself (about 8 minutes, once) */
        fprintf(stderr,
                "[gamedata] could not link the game data to the disc; the game will copy it on its first start\n");
        for (const char* f : kFolders)
            if (is_link(target / f)) remove_link(target / f);
        bool links = false;
        for (const char* f : kFolders) links |= is_link(target / f);
        if (!links) fs::remove_all(data, ec);
    }
    return ok;
}

bool identify(const fs::path& p, Source* s, std::string* err)
{
    std::error_code ec;
    *s = Source();
    if (fs::is_directory(p, ec)) {
        Disc disc;
        if (!open_disc(p, &disc, err)) return false;
        s->kind = SourceKind::Disc;
        s->path = disc.source;
        s->name = "Drakengard 3 disc (BLUS31197)";
        s->bytes = disc.total;
        return true;
    }
    if (!fs::is_regular_file(p, ec)) {
        *err = "not a file or a folder";
        return false;
    }
    s->path = p;
    const uint32_t m = magic(p);
    if (m == 0x7F504B47u) {   /* a PS3 package */
        const std::string cid = pkg_content_id(p);
        if (!retail(p)) {
            /* NoPayStation's "FIX" packages are debug packages */
            *err = cid.rfind(kDlcPrefix, 0) == 0
                       ? "this is a licence fix package, not the DLC itself. It is not needed: add the DLC packages"
                       : "a debug package, not one from PSN";
            return false;
        }
        if (cid == kUpdateId) {
            if (pkg_footer_sha1(p) != kUpdateSha1) {
                *err = "this update package is not version 1.01, or it is damaged";
                return false;
            }
            if (!update_required()) {
                *err = "this build plays version 1.00, which takes no update";
                return false;
            }
            Pkg pkg;
            if (!pkg_open(p, &pkg, err)) return false;
            s->kind = SourceKind::Update;
            s->id = cid;
            s->name = "Update 1.01";
            s->bytes = pkg.payload();
            return true;
        }
        if (cid.rfind(kDlcPrefix, 0) == 0) {
            const std::string id = cid.substr(sizeof kDlcPrefix - 1);
            for (const DlcInfo& d : known_dlc()) {
                if (id != d.id) continue;
                Pkg pkg;
                if (!pkg_open(p, &pkg, err)) return false;
                if (licence_only(pkg)) {
                    *err = "this is a licence fix package, not the DLC itself. It is not needed: add the " +
                           std::string(d.name) + " package";
                    return false;
                }
                s->kind = SourceKind::Dlc;
                s->id = id;
                s->name = d.name;
                s->bytes = pkg.payload();
                return true;
            }
            *err = "this Drakengard 3 package (" + id + ") is not one of the DLC packs";
            return false;
        }
        *err = cid.empty() ? "an unreadable PS3 package" : "a package for another game (" + cid + ")";
        return false;
    }
    if (m == 0x7F454C46u) {   /* an ELF */
        const std::string sha = sha256_file(p);
        const bool v101 = sha == kElf101Sha256, v100 = sha == kEbootElfSha256;
        if (update_required() ? v101 : v100) {
            s->kind = SourceKind::Eboot;
            s->name = std::string("EBOOT.ELF (") + DOD3_EBOOT_NAME + ")";
            s->bytes = fs::file_size(p, ec);
            return true;
        }
        if (v100)
            *err =
                "this is the disc's executable (1.00); this build plays 1.01, and makes its executable from the update";
        else if (v101)
            *err = "this is the update's executable (1.01); this build plays 1.00";
        else
            *err = "this EBOOT.ELF is not Drakengard 3's (BLUS31197) " DOD3_EBOOT_NAME;
        return false;
    }
    if (m == 0x53434500u) {   /* "SCE\0": a SELF */
        *err = update_required()
                   ? "EBOOT.BIN is not needed: the setup takes the game's executable from the update package"
                   : "EBOOT.BIN is not needed: the setup takes the game's executable from the disc";
        return false;
    }
    Disc disc;   /* a disc image */
    if (open_disc(p, &disc, err)) {
        s->kind = SourceKind::Disc;
        s->name = "Drakengard 3 disc image (BLUS31197)";
        s->bytes = disc.total;
        return true;
    }
    return false;
}

bool read_eboot_bin(const fs::path& base, const Plan& plan, std::vector<uint8_t>* self, std::string* err)
{
    std::error_code ec;
    fs::path file;
    if (update_required()) {
        if (plan.update.set()) {
            Pkg pkg;
            return pkg_open(plan.update.path, &pkg, err) && pkg_read_file(pkg, "USRDIR/EBOOT.BIN", self, err);
        }
        file = update_dir(base) / "USRDIR/EBOOT.BIN";
    } else {
        if (plan.disc.set()) {
            Disc disc;
            if (!open_disc(plan.disc.path, &disc, err)) return false;
            if (read_disc_file(disc, "PS3_GAME/USRDIR/EBOOT.BIN", self)) return true;
            *err = "cannot read the disc's EBOOT.BIN";
            return false;
        }
        file = base / "game/disc/PS3_GAME/USRDIR/EBOOT.BIN";
    }
    const uint64_t n = fs::file_size(file, ec);
    FILE* f = ec ? nullptr : dod3::open_file(file, "rb");
    if (!f || n > (256u << 20)) {
        if (f) fclose(f);
        *err = update_required() ? "the update is not installed" : "the disc is not installed";
        return false;
    }
    self->resize((size_t)n);
    const bool ok = fread(self->data(), 1, self->size(), f) == self->size();
    fclose(f);
    if (!ok) *err = "cannot read " + utf8(file);
    return ok;
}

bool make_elf(const std::vector<uint8_t>& self, const Keys& keys, std::vector<uint8_t>* elf, bool* wrong_keys,
              std::string* err)
{
    *wrong_keys = false;
    if (!keys.complete()) {
        *err = "the keys for the game's executable are missing";
        return false;
    }
    if (!self_decrypt(self, keys.self_keys(), elf, wrong_keys, err)) return false;
    const std::string sha = sha256_hex(elf->data(), elf->size());
    if (sha != (update_required() ? kElf101Sha256 : kEbootElfSha256)) {
        *err = "the executable made from EBOOT.BIN is not the expected one (SHA-256 " + sha + ")";
        return false;
    }
    return true;
}

bool install(const fs::path& base, const Plan& plan,
             const std::function<bool(uint64_t, uint64_t, const std::string&)>& progress, std::string* err)
{
    std::error_code ec;
    /* the progress total: the bytes read */
    uint64_t total = 0, done = 0;
    if (plan.disc.set()) total += plan.disc.bytes;
    if (plan.eboot.set()) total += plan.eboot.bytes;
    if (plan.update.set()) total += fs::file_size(plan.update.path, ec);
    for (const Source& d : plan.dlc) total += fs::file_size(d.path, ec);
    const fs::path stage = staging(base);
    /* staging is deleted only once nothing of the previous copy is in it */
    auto clean = [&](std::string* why) {
        if (!settle_old(base, why)) return false;
        fs::remove_all(stage, ec);
        return true;
    };
    auto fail = [&](const std::string& e) {
        *err = e;
        std::string why;
        if (!clean(&why)) *err += ". Also, " + why;
        return false;
    };

    fs::create_directories(base / "game", ec);
    if (!clean(err)) return false;   /* a run that was cut short */

    /* The executable, made first (in memory) so wrong keys stop the install
     * before anything is copied; written once the update is in place. */
    std::vector<uint8_t> elf;
    const bool make = !plan.eboot.set() && !installed(base).eboot;
    if (make) {
        if (!progress(0, total, "Making the game's executable")) return fail("cancelled");
        std::vector<uint8_t> self;
        bool wrong = false;
        if (!read_eboot_bin(base, plan, &self, err) || !make_elf(self, plan.keys, &elf, &wrong, err)) return fail(*err);
    }

    if (plan.disc.set() &&
        !(fs::exists(base / "game/disc", ec) && fs::equivalent(plan.disc.path, base / "game/disc", ec))) {
        Disc disc;
        if (!open_disc(plan.disc.path, &disc, err)) return fail(*err);
        const fs::path part = stage / "disc", old = stage / "old", final_dir = base / "game/disc";
        if (!copy_disc(
                disc, part, [&](uint64_t d, uint64_t) { return progress(done + d, total, "Copying the game"); }, err))
            return fail(*err);
        /* The swap: the old copy aside, the new one in, then what the old one
         * held besides the disc back (settle_old). Nothing that may hold the
         * game data's links onto the disc is deleted on the way. */
        if (fs::exists(final_dir, ec)) {
            fs::rename(final_dir, old, ec);
            if (ec) return fail("could not replace the game's files: " + ec.message());
        }
        fs::rename(part, final_dir, ec);
        if (ec)
            return fail("could not move the game into place: " + ec.message()); /* settle_old puts the old copy back */
        if (!settle_old(base, err)) return false;
    }
    if (plan.disc.set()) {
        done += plan.disc.bytes;
        gamedata_layout(base / "game/disc");
    }

    if (plan.eboot.set()) {
        if (!progress(done, total, "Copying EBOOT.ELF")) return fail("cancelled");
        const fs::path part = stage / "EBOOT.ELF", to = base / eboot_path();
        fs::create_directories(stage, ec);
        fs::create_directories(to.parent_path(), ec);
        fs::copy_file(plan.eboot.path, part, fs::copy_options::overwrite_existing, ec);
        if (!ec) fs::rename(part, to, ec);
        if (ec) return fail("could not copy EBOOT.ELF: " + ec.message());
        done += plan.eboot.bytes;
    }

    if (plan.update.set()) {
        Pkg pkg;
        if (!pkg_open(plan.update.path, &pkg, err)) return fail(*err);
        const fs::path part = stage / "update";
        const uint64_t start = done;
        if (!pkg_extract(
                pkg, part, [&](uint64_t d, uint64_t) { return progress(start + d, total, "Installing update 1.01"); },
                err))
            return fail(*err);
        /* the whole package, as the console installs it (EBOOT.BIN too, so
         * the executable can be made again); the old PATCH folder goes whole,
         * so a file 1.01 dropped does not linger */
        fs::remove_all(update_dir(base) / "USRDIR/PATCH", ec);
        if (!merge_move(part, update_dir(base), err)) return fail(*err);
        done = start + pkg.file_size;
    }

    if (make) {
        if (!progress(done, total, "Writing the game's executable")) return fail("cancelled");
        const fs::path part = stage / "EBOOT.ELF", to = base / eboot_path();
        fs::create_directories(stage, ec);
        fs::create_directories(to.parent_path(), ec);
        FILE* f = dod3::open_file(part, "wb");
        const bool wrote = f && fwrite(elf.data(), 1, elf.size(), f) == elf.size();
        if (f) fclose(f);
        if (wrote) fs::rename(part, to, ec);
        if (!wrote || ec) return fail("could not write " + utf8(to));
        if (!builtin_keys().complete()) keys_save(keys_file(base), plan.keys); /* the player's, for the next time */
    }

    for (const Source& d : plan.dlc) {
        Pkg pkg;
        if (!pkg_open(d.path, &pkg, err)) return fail(*err);
        const fs::path part = stage / "dlc";
        const uint64_t start = done;
        const std::string what = "Installing " + d.name;
        if (!pkg_extract(pkg, part, [&](uint64_t x, uint64_t) { return progress(start + x, total, what); }, err))
            return fail(*err);
        /* each pack is its own folder under USRDIR/DLC; the voice pack also
         * fills USRDIR/DLC_JPV. The shared PARAM.SFO and icons are the same
         * in every pack. */
        if (!merge_move(part, dlc_dir(base), err)) return fail(*err);
        if (d.id == "ADDJAPANESEVOICE" && !dlc_write_jpv_lists(base / "game/disc", true))
            return fail("the Japanese Voice Pack installed, but its file lists could not be written");
        done = start + pkg.file_size;
    }
    fs::remove_all(stage, ec);
    progress(total, total, "Done");
    return true;
}

int install_files_cli(const fs::path& base, const std::vector<fs::path>& files, const fs::path& keys_path)
{
    Plan plan;
    {
        std::error_code ec;
        const fs::path kp = keys_path.empty() ? keys_file(base) : keys_path;
        std::string err;
        if (keys_path.empty() && builtin_keys().complete()) {
            plan.keys = builtin_keys();
        } else if ((!keys_path.empty() || fs::exists(kp, ec)) && !keys_load(kp, &plan.keys, &err)) {
            printf("%s: %s\n", utf8(kp).c_str(), err.c_str());
            return 1;
        }
    }
    for (const fs::path& f : files) {
        Source s;
        std::string err;
        if (!identify(f, &s, &err)) {
            printf("%s: %s\n", utf8(f).c_str(), err.c_str());
            return 1;
        }
        printf("%s: %s\n", utf8(f).c_str(), s.name.c_str());
        switch (s.kind) {
        case SourceKind::Disc: plan.disc = s; break;
        case SourceKind::Update: plan.update = s; break;
        case SourceKind::Eboot: plan.eboot = s; break;
        case SourceKind::Dlc:
            plan.dlc.erase(
                std::remove_if(plan.dlc.begin(), plan.dlc.end(), [&](const Source& o) { return o.id == s.id; }),
                plan.dlc.end());
            plan.dlc.push_back(s);
            break;
        default: break;
        }
    }
    const Status have = installed(base);
    std::string missing;
    if (!plan.disc.set() && !have.disc) missing += " the game disc (an .iso or its folder),";
    if (update_required() && !plan.update.set() && !have.update) missing += " the 1.01 update package,";
    if (!plan.eboot.set() && !have.eboot && !plan.keys.complete()) {
        missing += " the keys for the game's executable (--keys <file> with";
        for (const KeyField& k : needed_keys()) missing += std::string(" ") + k.id;
        missing += "),";
    }
    if (!missing.empty()) {
        missing.pop_back();
        printf("still needed:%s\n", missing.c_str());
        return 1;
    }
    const uint64_t need = plan.bytes(), space = free_bytes(base);
    printf("installing %.2f GB into %s (%.1f GB free)\n", need / 1073741824.0, utf8(base).c_str(),
           space / 1073741824.0);
    if (space && need > space) {
        printf("not enough space\n");
        return 1;
    }
    int last = -1;
    std::string step, err;
    const bool ok = install(
        base, plan,
        [&](uint64_t d, uint64_t t, const std::string& what) {
            const int pct = t ? (int)(d * 100 / t) : 100;
            if (pct != last || what != step) {
                if (what != step && !step.empty()) printf("\n");
                last = pct;
                step = what;
                printf("\r%-40s %3d%%", what.c_str(), pct);
                fflush(stdout);
            }
            return true;
        },
        &err);
    printf("\n");
    if (!ok) {
        printf("install failed: %s\n", err.c_str());
        return 1;
    }
    printf("installed\n");
    return 0;
}

}  // namespace dod3setup
