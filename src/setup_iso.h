/*
 * First-run setup: the disc. Checking a disc a player supplies (a decrypted
 * ISO or a folder holding PS3_GAME: Drakengard 3, BLUS31197, version 01.00,
 * checked against the hash of its EBOOT.BIN), reading it, and copying it to
 * game/disc/. (No firmware is needed: the MP3 decoder MultiStream loads from
 * dev_flash is our own stand-in, main.cpp.)
 *
 * The whole install -- the disc, the 1.01 update, EBOOT.ELF and the DLC -- is
 * src/setup_install.h, with the installer's pages in src/setup_wizard.cpp.
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
extern const char* const kEbootElfSha256;      /* the disc's EBOOT.BIN decrypted (1.00) */
extern const char* const kEbootBinSha256;      /* the disc's PS3_GAME/USRDIR/EBOOT.BIN */
extern const char* const kTitleId;             /* BLUS31197 */
extern const char* const kAppVer;              /* 01.00 */
extern const uint64_t kDiscBytes;              /* about what game/disc takes */

std::string sha256_file(const fs::path& p);    /* "" if unreadable */

/* PARAM.SFO's TITLE_ID and APP_VER; false if it is not an SFO. */
bool param_sfo(const std::vector<uint8_t>& sfo, std::string* title_id, std::string* app_ver);

/* What is in place under `base` (the executable's directory). */
struct Installed { bool disc = false, elf = false; bool all() const { return disc && elf; } };
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

}  // namespace dod3setup
