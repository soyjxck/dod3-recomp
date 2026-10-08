/*
 * The setup's cryptography, self-contained so it links no library on either
 * platform: AES (128- and 256-bit keys, both directions, table driven; CTR
 * and CBC), SHA-1 and SHA-256. Written from FIPS-197 and FIPS 180-4, and
 * checked against their test vectors before first use (crypto_selftest).
 *
 * Keys: the retail package key comes from ps3recomp (CMakeLists.txt); the
 * keys that open the game's executable come from the player
 * (src/setup_keys.h).
 */
#pragma once
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace dod3setup {

class Aes {
public:
    bool set_key(const uint8_t* key, int bits);   /* 128 or 256 */
    void encrypt(const uint8_t in[16], uint8_t out[16]) const;
    void decrypt(const uint8_t in[16], uint8_t out[16]) const;
private:
    uint32_t ek_[60] = {}, dk_[60] = {};
    int nr_ = 0;
};

/* CTR: XOR into buf the keystream for bytes [off, off + n) of a stream whose
 * counter starts at `iv` (a 128-bit big-endian number, +1 per block). */
void aes_ctr(const Aes& aes, const uint8_t iv[16], uint64_t off, uint8_t* buf, size_t n);

/* CBC decryption in place; n a multiple of 16. `iv` is left as the last
 * ciphertext block, so a long stream can be decrypted in pieces. */
void aes_cbc_decrypt(const Aes& aes, uint8_t iv[16], uint8_t* buf, size_t n);

struct Sha1 {
    void update(const void* p, size_t n);
    void digest(uint8_t out[20]);
    std::string hex();
private:
    void block(const uint8_t* p);
    uint32_t h_[5] = {0x67452301, 0xEFCDAB89, 0x98BADCFE, 0x10325476, 0xC3D2E1F0};
    uint8_t buf_[64] = {};
    size_t n_ = 0;
    uint64_t len_ = 0;
};

struct Sha256 {
    void update(const void* p, size_t n);
    void digest(uint8_t out[32]);
    std::string hex();
private:
    void block(const uint8_t* p);
    uint32_t h_[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
    uint8_t buf_[64] = {};
    size_t n_ = 0;
    uint64_t len_ = 0;
};

std::string sha256_hex(const void* p, size_t n);
std::string to_hex(const uint8_t* p, size_t n);

/* Hex as a player pastes it: spaces, tabs, colons, dashes and a 0x prefix
 * are skipped. False if anything else is in it or the digits are odd. */
bool from_hex(const std::string& s, std::vector<uint8_t>* out);

/* FIPS-197 C.1 and C.3 both ways, SHA-1 and SHA-256 of "abc". Cached. */
bool crypto_selftest();

}  // namespace dod3setup
