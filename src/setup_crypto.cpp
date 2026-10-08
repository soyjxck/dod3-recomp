/* The setup's cryptography: see setup_crypto.h. */
#include "setup_crypto.h"
#include "dod3_util.h"

#include <algorithm>
#include <cstdio>
#include <cstring>

namespace dod3setup {

namespace {

uint32_t ror32(uint32_t v, int c) { return c ? (v >> c) | (v << (32 - c)) : v; }
uint32_t rol32(uint32_t v, int c) { return (v << c) | (v >> (32 - c)); }

/* ---- AES tables, built from the field arithmetic (FIPS-197 5.1) -------- */

struct Tables {
    uint8_t sbox[256], isbox[256];
    uint32_t te[4][256], td[4][256];
    static uint8_t xtime(uint8_t x) { return (uint8_t)((x << 1) ^ ((x & 0x80) ? 0x1B : 0)); }
    static uint8_t mul(uint8_t a, uint8_t b)
    {
        uint8_t r = 0;
        for (; b; b >>= 1, a = xtime(a))
            if (b & 1) r ^= a;
        return r;
    }
    Tables()
    {
        uint8_t inv[256] = {};
        for (int x = 1; x < 256; x++)
            for (int y = 1; y < 256; y++)
                if (mul((uint8_t)x, (uint8_t)y) == 1) {
                    inv[x] = (uint8_t)y;
                    break;
                }
        for (int x = 0; x < 256; x++) {   /* the affine map */
            const uint8_t b = inv[x];
            uint8_t s = b;
            for (int i = 1; i <= 4; i++) s ^= (uint8_t)((b << i) | (b >> (8 - i)));
            sbox[x] = s ^ 0x63;
        }
        for (int x = 0; x < 256; x++) isbox[sbox[x]] = (uint8_t)x;
        for (int x = 0; x < 256; x++) {
            const uint8_t s = sbox[x], i = isbox[x];
            const uint32_t e = (uint32_t)mul(s, 2) << 24 | (uint32_t)s << 16 | (uint32_t)s << 8 | mul(s, 3);
            const uint32_t d =
                (uint32_t)mul(i, 14) << 24 | (uint32_t)mul(i, 9) << 16 | (uint32_t)mul(i, 13) << 8 | mul(i, 11);
            for (int k = 0; k < 4; k++) {
                te[k][x] = ror32(e, 8 * k);
                td[k][x] = ror32(d, 8 * k);
            }
        }
    }
};

const Tables& tab()
{
    static const Tables t;   /* built once, thread-safe */
    return t;
}

}  // namespace

/* ---- AES -------------------------------------------------------------- */

bool Aes::set_key(const uint8_t* key, int bits)
{
    const Tables& T = tab();
    const int nk = bits / 32;
    if (nk != 4 && nk != 8) return false;
    nr_ = nk + 6;
    const int words = 4 * (nr_ + 1);
    auto sub = [&](uint32_t w) {
        return (uint32_t)T.sbox[w >> 24] << 24 | (uint32_t)T.sbox[(w >> 16) & 0xFF] << 16 |
               (uint32_t)T.sbox[(w >> 8) & 0xFF] << 8 | T.sbox[w & 0xFF];
    };
    for (int i = 0; i < nk; i++) ek_[i] = dod3_be32(key + 4 * i);
    uint8_t rcon = 1;
    for (int i = nk; i < words; i++) {
        uint32_t t = ek_[i - 1];
        if (i % nk == 0) {
            t = sub(rol32(t, 8)) ^ (uint32_t)rcon << 24;
            rcon = Tables::xtime(rcon);
        } else if (nk > 6 && i % nk == 4) {
            t = sub(t);
        }
        ek_[i] = ek_[i - nk] ^ t;
    }
    /* the equivalent inverse cipher's schedule: reversed, InvMixColumns on
     * the inner rounds (td of sbox cancels td's own inverse S-box) */
    for (int r = 0; r <= nr_; r++)
        for (int c = 0; c < 4; c++) {
            uint32_t w = ek_[(nr_ - r) * 4 + c];
            if (r > 0 && r < nr_)
                w = T.td[0][T.sbox[w >> 24]] ^ T.td[1][T.sbox[(w >> 16) & 0xFF]] ^ T.td[2][T.sbox[(w >> 8) & 0xFF]] ^
                    T.td[3][T.sbox[w & 0xFF]];
            dk_[r * 4 + c] = w;
        }
    return true;
}

void Aes::encrypt(const uint8_t in[16], uint8_t out[16]) const
{
    const Tables& T = tab();
    uint32_t s[4], t[4];
    for (int i = 0; i < 4; i++) s[i] = dod3_be32(in + 4 * i) ^ ek_[i];
    const uint32_t* k = ek_ + 4;
    for (int r = 1; r < nr_; r++, k += 4) {
        for (int i = 0; i < 4; i++)
            t[i] = T.te[0][s[i] >> 24] ^ T.te[1][(s[(i + 1) & 3] >> 16) & 0xFF] ^
                   T.te[2][(s[(i + 2) & 3] >> 8) & 0xFF] ^ T.te[3][s[(i + 3) & 3] & 0xFF] ^ k[i];
        memcpy(s, t, sizeof s);
    }
    for (int i = 0; i < 4; i++) {
        const uint32_t w = (uint32_t)T.sbox[s[i] >> 24] << 24 | (uint32_t)T.sbox[(s[(i + 1) & 3] >> 16) & 0xFF] << 16 |
                           (uint32_t)T.sbox[(s[(i + 2) & 3] >> 8) & 0xFF] << 8 | T.sbox[s[(i + 3) & 3] & 0xFF];
        dod3_put_be32(out + 4 * i, w ^ k[i]);
    }
}

void Aes::decrypt(const uint8_t in[16], uint8_t out[16]) const
{
    const Tables& T = tab();
    uint32_t s[4], t[4];
    for (int i = 0; i < 4; i++) s[i] = dod3_be32(in + 4 * i) ^ dk_[i];
    const uint32_t* k = dk_ + 4;
    for (int r = 1; r < nr_; r++, k += 4) {
        for (int i = 0; i < 4; i++)
            t[i] = T.td[0][s[i] >> 24] ^ T.td[1][(s[(i + 3) & 3] >> 16) & 0xFF] ^
                   T.td[2][(s[(i + 2) & 3] >> 8) & 0xFF] ^ T.td[3][s[(i + 1) & 3] & 0xFF] ^ k[i];
        memcpy(s, t, sizeof s);
    }
    for (int i = 0; i < 4; i++) {
        const uint32_t w = (uint32_t)T.isbox[s[i] >> 24] << 24 |
                           (uint32_t)T.isbox[(s[(i + 3) & 3] >> 16) & 0xFF] << 16 |
                           (uint32_t)T.isbox[(s[(i + 2) & 3] >> 8) & 0xFF] << 8 | T.isbox[s[(i + 1) & 3] & 0xFF];
        dod3_put_be32(out + 4 * i, w ^ k[i]);
    }
}

void aes_ctr(const Aes& aes, const uint8_t iv[16], uint64_t off, uint8_t* buf, size_t n)
{
    uint64_t block = off / 16;
    size_t skip = (size_t)(off % 16), done = 0;
    uint8_t ctr[16], ks[16];
    while (done < n) {
        /* iv + block, as a 128-bit big-endian number */
        memcpy(ctr, iv, 16);
        uint64_t add = block;
        for (int i = 15; i >= 0 && add; i--) {
            const uint64_t v = (uint64_t)ctr[i] + (add & 0xFF);
            ctr[i] = (uint8_t)v;
            add = (add >> 8) + (v >> 8);
        }
        aes.encrypt(ctr, ks);
        const size_t take = std::min<size_t>(16 - skip, n - done);
        for (size_t i = 0; i < take; i++) buf[done + i] ^= ks[skip + i];
        done += take;
        skip = 0;
        block++;
    }
}

void aes_cbc_decrypt(const Aes& aes, uint8_t iv[16], uint8_t* buf, size_t n)
{
    uint8_t c[16], p[16];
    for (size_t o = 0; o + 16 <= n; o += 16) {
        memcpy(c, buf + o, 16);
        aes.decrypt(c, p);
        for (int i = 0; i < 16; i++) buf[o + i] = p[i] ^ iv[i];
        memcpy(iv, c, 16);
    }
}

/* ---- SHA-1 (FIPS 180-4 6.1) ------------------------------------------ */

void Sha1::block(const uint8_t* p)
{
    uint32_t w[80];
    for (int i = 0; i < 16; i++) w[i] = dod3_be32(p + 4 * i);
    for (int i = 16; i < 80; i++) w[i] = rol32(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
    uint32_t a = h_[0], b = h_[1], c = h_[2], d = h_[3], e = h_[4];
    for (int i = 0; i < 80; i++) {
        uint32_t f, k;
        if (i < 20) {
            f = (b & c) | (~b & d);
            k = 0x5A827999;
        } else if (i < 40) {
            f = b ^ c ^ d;
            k = 0x6ED9EBA1;
        } else if (i < 60) {
            f = (b & c) | (b & d) | (c & d);
            k = 0x8F1BBCDC;
        } else {
            f = b ^ c ^ d;
            k = 0xCA62C1D6;
        }
        const uint32_t t = rol32(a, 5) + f + e + k + w[i];
        e = d;
        d = c;
        c = rol32(b, 30);
        b = a;
        a = t;
    }
    h_[0] += a;
    h_[1] += b;
    h_[2] += c;
    h_[3] += d;
    h_[4] += e;
}

void Sha1::update(const void* data, size_t m)
{
    const uint8_t* p = (const uint8_t*)data;
    len_ += m;
    while (m) {
        const size_t take = std::min(m, 64 - n_);
        memcpy(buf_ + n_, p, take);
        n_ += take;
        p += take;
        m -= take;
        if (n_ == 64) {
            block(buf_);
            n_ = 0;
        }
    }
}

void Sha1::digest(uint8_t out[20])
{
    const uint64_t bits = len_ * 8;
    const uint8_t one = 0x80, zero = 0;
    update(&one, 1);
    while (n_ != 56) update(&zero, 1);
    uint8_t l[8];
    for (int i = 0; i < 8; i++) l[i] = (uint8_t)(bits >> (56 - 8 * i));
    update(l, 8);
    for (int i = 0; i < 5; i++) dod3_put_be32(out + 4 * i, h_[i]);
}

std::string Sha1::hex()
{
    uint8_t d[20];
    digest(d);
    return to_hex(d, 20);
}

/* ---- SHA-256 (FIPS 180-4 6.2) ---------------------------------------- */

void Sha256::block(const uint8_t* p)
{
    static const uint32_t k[64] = { 0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4,
                                    0xab1c5ed5, 0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe,
                                    0x9bdc06a7, 0xc19bf174, 0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f,
                                    0x4a7484aa, 0x5cb0a9dc, 0x76f988da, 0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7,
                                    0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967, 0x27b70a85, 0x2e1b2138, 0x4d2c6dfc,
                                    0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85, 0xa2bfe8a1, 0xa81a664b,
                                    0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070, 0x19a4c116,
                                    0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
                                    0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7,
                                    0xc67178f2 };
    uint32_t w[64];
    for (int i = 0; i < 16; i++) w[i] = dod3_be32(p + 4 * i);
    for (int i = 16; i < 64; i++) {
        const uint32_t s0 = ror32(w[i - 15], 7) ^ ror32(w[i - 15], 18) ^ (w[i - 15] >> 3);
        const uint32_t s1 = ror32(w[i - 2], 17) ^ ror32(w[i - 2], 19) ^ (w[i - 2] >> 10);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    uint32_t a = h_[0], b = h_[1], c = h_[2], d = h_[3], e = h_[4], f = h_[5], g = h_[6], h = h_[7];
    for (int i = 0; i < 64; i++) {
        const uint32_t t1 = h + (ror32(e, 6) ^ ror32(e, 11) ^ ror32(e, 25)) + ((e & f) ^ (~e & g)) + k[i] + w[i];
        const uint32_t t2 = (ror32(a, 2) ^ ror32(a, 13) ^ ror32(a, 22)) + ((a & b) ^ (a & c) ^ (b & c));
        h = g;
        g = f;
        f = e;
        e = d + t1;
        d = c;
        c = b;
        b = a;
        a = t1 + t2;
    }
    h_[0] += a;
    h_[1] += b;
    h_[2] += c;
    h_[3] += d;
    h_[4] += e;
    h_[5] += f;
    h_[6] += g;
    h_[7] += h;
}

void Sha256::update(const void* data, size_t m)
{
    const uint8_t* p = (const uint8_t*)data;
    len_ += m;
    while (m) {
        const size_t take = std::min(m, 64 - n_);
        memcpy(buf_ + n_, p, take);
        n_ += take;
        p += take;
        m -= take;
        if (n_ == 64) {
            block(buf_);
            n_ = 0;
        }
    }
}

void Sha256::digest(uint8_t out[32])
{
    const uint64_t bits = len_ * 8;
    const uint8_t one = 0x80, zero = 0;
    update(&one, 1);
    while (n_ != 56) update(&zero, 1);
    uint8_t l[8];
    for (int i = 0; i < 8; i++) l[i] = (uint8_t)(bits >> (56 - 8 * i));
    update(l, 8);
    for (int i = 0; i < 8; i++) dod3_put_be32(out + 4 * i, h_[i]);
}

std::string Sha256::hex()
{
    uint8_t d[32];
    digest(d);
    return to_hex(d, 32);
}

std::string sha256_hex(const void* p, size_t n)
{
    Sha256 s;
    s.update(p, n);
    return s.hex();
}

std::string to_hex(const uint8_t* p, size_t n)
{
    static const char* x = "0123456789abcdef";
    std::string s;
    s.reserve(n * 2);
    for (size_t i = 0; i < n; i++) {
        s += x[p[i] >> 4];
        s += x[p[i] & 15];
    }
    return s;
}

bool from_hex(const std::string& s, std::vector<uint8_t>* out)
{
    out->clear();
    int hi = -1;
    for (size_t i = 0; i < s.size(); i++) {
        const char c = s[i];
        if (c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == ':' || c == '-' || c == ',') continue;
        if (c == '0' && i + 1 < s.size() && (s[i + 1] == 'x' || s[i + 1] == 'X')) {
            i++;
            continue;
        }   /* x is never a digit */
        int v;
        if (c >= '0' && c <= '9')
            v = c - '0';
        else if (c >= 'a' && c <= 'f')
            v = c - 'a' + 10;
        else if (c >= 'A' && c <= 'F')
            v = c - 'A' + 10;
        else
            return false;
        if (hi < 0)
            hi = v;
        else {
            out->push_back((uint8_t)(hi << 4 | v));
            hi = -1;
        }
    }
    return hi < 0;
}

bool crypto_selftest()
{
    static const bool ok = [] {
        uint8_t key[32], pt[16], ct[16], back[16];
        for (int i = 0; i < 32; i++) key[i] = (uint8_t)i;
        for (int i = 0; i < 16; i++) pt[i] = (uint8_t)(i * 0x11);
        static const char* want128 = "69c4e0d86a7b0430d8cdb78070b4c55a";   /* FIPS-197 C.1 */
        static const char* want256 = "8ea2b7ca516745bfeafc49904b496089";   /* FIPS-197 C.3 */
        Aes a;
        a.set_key(key, 128);
        a.encrypt(pt, ct);
        a.decrypt(ct, back);
        if (to_hex(ct, 16) != want128 || memcmp(back, pt, 16) != 0) return false;
        a.set_key(key, 256);
        a.encrypt(pt, ct);
        a.decrypt(ct, back);
        if (to_hex(ct, 16) != want256 || memcmp(back, pt, 16) != 0) return false;
        Sha1 s1;
        s1.update("abc", 3);
        if (s1.hex() != "a9993e364706816aba3e25717850c26c9cd0d89d") return false;
        return sha256_hex("abc", 3) == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad";
    }();
    return ok;
}

}  // namespace dod3setup
