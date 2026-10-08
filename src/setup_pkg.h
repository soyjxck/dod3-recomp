/*
 * PS3 package (.pkg) reading for the setup: the 1.01 update and the DLC.
 *
 * A retail package is a header, a file table and the files, the table and
 * files under AES-128-CTR with the retail package key (ps3recomp's, from its
 * tools/pkg_extract.py; it opens the package wrapper and nothing else --
 * licensed content inside stays as it is). Its last 32 bytes are the SHA-1 of
 * everything before them, which is also the checksum Sony's update server
 * lists, so a package checks itself: extract() hashes the file as it reads
 * it and fails on a mismatch.
 */
#pragma once
#include <cstdint>
#include <functional>
#include <string>
#include <vector>
#include <filesystem>

namespace dod3setup {

struct PkgEntry {
    std::string name;        /* e.g. "USRDIR/PATCH/SQEX03GAME/COOKEDPS3/CORE.XXX" */
    uint64_t offset = 0;     /* in the data area */
    uint64_t size = 0;
    bool dir = false;
};

struct Pkg {
    std::filesystem::path path;
    std::string content_id;  /* "UP0082-BLUS31197_00-DOD3PATCH0000000" */
    uint64_t file_size = 0;
    uint64_t data_offset = 0, data_size = 0;
    uint8_t riv[16] = {};
    std::vector<PkgEntry> entries;
    uint64_t payload() const;        /* bytes of the files */
};

/* Read the header and the file table. False (with `err`) if it is not a
 * retail PS3 package. */
bool pkg_open(const std::filesystem::path& p, Pkg* pkg, std::string* err);

/* Just the content ID, from the plain-text header (cheap). */
std::string pkg_content_id(const std::filesystem::path& p);

/* Extract the entries `want` accepts (all if null) under `dest`, as
 * map(name) (dest / name if null). Reads the package once, start to end,
 * checking its SHA-1 footer. progress(bytes read, total) returns false to
 * cancel. Every file and directory created is appended to `created`. */
bool pkg_extract(const Pkg& pkg, const std::filesystem::path& dest,
                 const std::function<bool(const PkgEntry&)>& want,
                 const std::function<std::filesystem::path(const PkgEntry&)>& map,
                 const std::function<bool(uint64_t, uint64_t)>& progress,
                 std::vector<std::filesystem::path>* created, std::string* err);

/* One file of the package, decrypted into memory (EBOOT.BIN). Not checked
 * against the footer: what is read from it is checked by its own hash. */
bool pkg_read_file(const Pkg& pkg, const std::string& name, std::vector<uint8_t>* out, std::string* err);

/* The footer's SHA-1 (hex): what the update server lists. */
std::string pkg_footer_sha1(const std::filesystem::path& p);

}  // namespace dod3setup
