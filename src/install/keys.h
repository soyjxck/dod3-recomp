/*
 * The keys the setup makes the game's executable with (src/install/self.h). A
 * 1.01 build decrypts the update's EBOOT.BIN, an NPDRM SELF; a 1.00 build
 * the disc's, an application SELF. Both are key revision 0x1C:
 *   1.01   npdrm_erk (32 bytes), npdrm_riv (16): the NPDRM key set
 *          klic_free (16), klic_key (16): a free licence's klicensee, and
 *          the key that unwraps it
 *   1.00   app_erk (32), app_riv (16): the application key set
 * They are built in (src/install/keys_builtin.h). Should that file be emptied,
 * the player is asked instead: each key pasted as hex, or read from a key
 * file of `name=hex` lines, and kept in the install's keys.txt so the
 * executable can be made again without asking. A key is checked by its
 * length, then by decrypting the EBOOT with it, and the executable that
 * comes out by its hash.
 */
#pragma once
#include <cstdint>
#include <filesystem>
#include <map>
#include <string>
#include <vector>

#include "install/self.h"

namespace dod3setup {

struct KeyField {
    const char* id;       /* the name in a key file */
    const char* label;    /* for the player */
    size_t bytes;
};

/* The keys this build's executable needs. */
const std::vector<KeyField>& needed_keys();

struct Keys {
    std::map<std::string, std::vector<uint8_t>> v;
    bool complete() const;                  /* every needed key, at its length */
    SelfKeys self_keys() const;
};

/* `name=hex` lines (blank lines and # comments skipped; names not needed by
 * this build ignored). Fills `k` with what it finds; false with `err` on a
 * line that is not a key. */
bool keys_parse(const std::string& text, Keys* k, std::string* err);
bool keys_load(const std::filesystem::path& file, Keys* k, std::string* err);
bool keys_save(const std::filesystem::path& file, const Keys& k);

/* One pasted value: "" if it is fine, else what is wrong with it. */
std::string key_problem(const KeyField& f, const std::string& text, std::vector<uint8_t>* bytes);

/* <base>/keys.txt */
std::filesystem::path keys_file(const std::filesystem::path& base);

/* The keys built into the setup (src/install/keys_builtin.h); complete() when
 * the player need not be asked. */
const Keys& builtin_keys();

}  // namespace dod3setup
