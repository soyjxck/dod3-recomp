/*
 * First-run setup, the portable half: what an installation needs, checking
 * the files a player supplies, and reading a disc image.
 *
 * The release does not carry any of the game. From the player's own copy it
 * needs:
 *   game/disc/                          the disc's files (PS3_GAME/...), copied
 *                                       from a decrypted ISO or a folder
 *   elf/EBOOT.ELF                       the disc's EBOOT.BIN, decrypted with
 *                                       RPCS3 (Utilities > Decrypt PS3 Binaries)
 *   fw/dev_flash/sys/external/flashMP3.pic
 *                                       from PS3 firmware 4.55 installed in
 *                                       RPCS3 (MultiStream loads it at run time)
 * all relative to the executable's directory. The build is tied to one
 * executable -- Drakengard 3, BLUS31197, version 01.00 -- so each file is
 * checked against the hash of the one it was built from.
 *
 * The platform halves (src/setup_win.cpp) ask for the files and show progress.
 */
#pragma once
#include <cstdint>
#include <functional>
#include <string>
#include <vector>
#include <filesystem>

namespace dod3setup {

namespace fs = std::filesystem;

/* SHA-256 of the build's inputs, as lowercase hex. */
extern const char* const kEbootElfSha256;      /* elf/EBOOT.ELF (decrypted) */
extern const char* const kEbootBinSha256;      /* the disc's PS3_GAME/USRDIR/EBOOT.BIN */
extern const char* const kFlashMp3Sha256;      /* firmware 4.55's flashMP3.pic */
extern const char* const kTitleId;             /* BLUS31197 */
extern const char* const kAppVer;              /* 01.00 */
extern const uint64_t kDiscBytes;              /* about what game/disc takes */

std::string sha256_file(const fs::path& p);    /* "" if unreadable */

/* PARAM.SFO's TITLE_ID and APP_VER; false if it is not an SFO. */
bool param_sfo(const std::vector<uint8_t>& sfo, std::string* title_id, std::string* app_ver);

/* What is in place under `base` (the executable's directory). */
struct Installed { bool disc = false, elf = false, mp3 = false; bool all() const { return disc && elf && mp3; } };
Installed check_installed(const fs::path& base);

/* A disc to install from: an ISO 9660 image or a folder holding PS3_GAME. */
struct DiscFile { std::string path; uint64_t offset = 0, size = 0; };   /* path uses '/', offset is the image byte offset */
struct Disc {
    bool is_iso = false;
    fs::path source;
    std::vector<DiscFile> files;
    uint64_t total = 0;
};

/* List the disc and check it is the right one: PARAM.SFO's title and version,
 * and EBOOT.BIN's hash (a disc image still under disc encryption, or another
 * release, fails here). On failure `err` says what to do. */
bool open_disc(const fs::path& source, Disc* disc, std::string* err);

/* Read one file of the disc whole (PARAM.SFO and the like). */
bool read_disc_file(const Disc& disc, const std::string& path, std::vector<uint8_t>* out);

/* Copy the disc's files into `dest` (created). progress(done, total) returns
 * false to cancel. */
bool copy_disc(const Disc& disc, const fs::path& dest, const std::function<bool(uint64_t, uint64_t)>& progress,
               std::string* err);

/* The whole install without a UI (dod3 --install <disc> <EBOOT.ELF>
 * <flashMP3.pic>): checks, then copies into `base`, reporting on stdout.
 * Returns 0 on success. */
int install_cli(const fs::path& base, const fs::path& disc, const fs::path& elf, const fs::path& mp3);

}  // namespace dod3setup
