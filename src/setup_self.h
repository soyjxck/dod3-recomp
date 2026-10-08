/*
 * SELF -> ELF for the setup: the game's EBOOT.BIN decrypted (the keys:
 * src/setup_keys.h), so the player gives only the disc and the update
 * package.
 *
 * A retail SELF is the ELF's headers in the clear, then a metadata block and
 * the program segments encrypted:
 *   metadata info (0x40 bytes)   AES-256-CBC under the key set for the SELF's
 *                                key revision (ERK, RIV); an NPDRM SELF has a
 *                                first AES-128-CBC layer under its klicensee
 *                                (for a free licence the NP "klic free"
 *                                value, itself unwrapped with the NP klic key)
 *   metadata headers, keys       AES-128-CTR under the metadata info's key/IV
 *   each segment                 AES-128-CTR under its own key/IV, then zlib
 * The metadata info's padding decrypts to zeros only under the right keys,
 * which is how wrong keys are told from a damaged file.
 */
#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace dod3setup {

struct SelfInfo {
    uint16_t key_revision = 0;
    uint32_t self_type = 0;      /* 4 an application, 8 an NPDRM application */
    uint32_t npd_license = 0;    /* NPDRM: 1 network, 2 local, 3 free */
    std::string content_id;      /* NPDRM */
};

struct SelfKeys {
    uint8_t erk[32] = {}, riv[16] = {};          /* for the SELF's type and key revision */
    uint8_t klic_free[16] = {}, klic_key[16] = {};   /* NPDRM only */
};

/* Decrypt and rebuild the ELF. False with `err`; *wrong_keys is set when
 * the keys do not open the metadata (as opposed to a damaged file). */
bool self_decrypt(const std::vector<uint8_t>& self, const SelfKeys& keys, std::vector<uint8_t>* elf, bool* wrong_keys,
                  std::string* err);

}  // namespace dod3setup
