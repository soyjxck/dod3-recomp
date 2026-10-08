/* SELF -> ELF: see setup_self.h. Written from the SELF format; every offset
 * in the file is checked before it is used. */
#include "setup_self.h"
#include "setup_crypto.h"

#include <zlib.h>

#include <algorithm>
#include <cstring>

namespace dod3setup {

namespace {

constexpr uint32_t kSelfMagic = 0x53434500;   /* "SCE\0" */
constexpr uint32_t kNpdMagic = 0x4E504400;    /* "NPD\0" */

/* big-endian reads that check the bounds */
struct Reader {
    const std::vector<uint8_t>& d;
    bool ok = true;
    bool has(uint64_t off, uint64_t n) { if (off > d.size() || n > d.size() - off) ok = false; return ok; }
    uint16_t u16(uint64_t o) { return has(o, 2) ? (uint16_t)(d[o] << 8 | d[o + 1]) : 0; }
    uint32_t u32(uint64_t o) { return has(o, 4) ? (uint32_t)d[o] << 24 | (uint32_t)d[o + 1] << 16 | (uint32_t)d[o + 2] << 8 | d[o + 3] : 0; }
    uint64_t u64(uint64_t o) { return has(o, 8) ? (uint64_t)u32(o) << 32 | u32(o + 4) : 0; }
};

uint16_t rd16(const uint8_t* p) { return (uint16_t)(p[0] << 8 | p[1]); }
uint32_t rd32(const uint8_t* p) { return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }
uint64_t rd64(const uint8_t* p) { return (uint64_t)rd32(p) << 32 | rd32(p + 4); }

struct Header {
    uint16_t key_revision;
    uint32_t meta_off;      /* metadata info, from the end of the 0x20-byte SCE header */
    uint64_t header_len;
    uint64_t app_info, elf, phdr, shdr, ctrl, ctrl_size;
};

bool header(const std::vector<uint8_t>& self, Header* h, std::string* err)
{
    Reader r{self};
    if (r.u32(0) != kSelfMagic || r.u16(0x0A) != 1) { *err = "not a SELF"; return false; }
    h->key_revision = r.u16(0x08);
    h->meta_off = r.u32(0x0C);
    h->header_len = r.u64(0x10);
    h->app_info = r.u64(0x28);
    h->elf = r.u64(0x30);
    h->phdr = r.u64(0x38);
    h->shdr = r.u64(0x40);
    h->ctrl = r.u64(0x58);
    h->ctrl_size = r.u64(0x60);
    if (!r.ok || h->header_len > self.size() || !r.has(h->app_info, 0x20) || !r.has(h->elf, 0x40) ||
        !r.has(0x20 + (uint64_t)h->meta_off, 0x40) || !r.has(h->ctrl, h->ctrl_size)) {
        *err = "the SELF's header is damaged";
        return false;
    }
    return true;
}

}  // namespace

bool self_info(const std::vector<uint8_t>& self, SelfInfo* info, std::string* err)
{
    Header h;
    if (!header(self, &h, err)) return false;
    Reader r{self};
    *info = SelfInfo();
    info->key_revision = h.key_revision;
    info->self_type = r.u32(h.app_info + 0x0C);
    for (uint64_t o = h.ctrl; o + 16 <= h.ctrl + h.ctrl_size;) {
        const uint32_t type = r.u32(o), size = r.u32(o + 4);
        if (!r.ok || size < 16) break;
        if (type == 3 && r.has(o + 16, 0x40) && r.u32(o + 16) == kNpdMagic) {
            info->npd_license = r.u32(o + 16 + 8);
            const char* cid = (const char*)&self[(size_t)(o + 16 + 16)];
            info->content_id.assign(cid, strnlen(cid, 0x30));
        }
        o += size;
    }
    return true;
}

bool self_decrypt(const std::vector<uint8_t>& self, const SelfKeys& keys, std::vector<uint8_t>* elf,
                  bool* wrong_keys, std::string* err)
{
    *wrong_keys = false;
    if (!crypto_selftest()) { *err = "internal error: the cryptography self-test failed"; return false; }
    Header h;
    SelfInfo info;
    if (!header(self, &h, err) || !self_info(self, &info, err)) return false;
    if (info.self_type != 4 && info.self_type != 8) { *err = "not an application SELF (type " + std::to_string(info.self_type) + ")"; return false; }
    if (info.self_type == 8 && info.npd_license != 3) {
        *err = "this NPDRM SELF has a per-console licence (type " + std::to_string(info.npd_license) + "), which the setup cannot open";
        return false;
    }

    /* the metadata info */
    const uint64_t mi = 0x20 + (uint64_t)h.meta_off;
    uint8_t meta[0x40];
    memcpy(meta, &self[(size_t)mi], sizeof meta);
    Aes aes;
    if (info.self_type == 8) {
        uint8_t klic[16], iv[16] = {};
        aes.set_key(keys.klic_key, 128);
        aes.decrypt(keys.klic_free, klic);
        aes.set_key(klic, 128);
        aes_cbc_decrypt(aes, iv, meta, sizeof meta);
    }
    {
        uint8_t iv[16];
        memcpy(iv, keys.riv, 16);
        aes.set_key(keys.erk, 256);
        aes_cbc_decrypt(aes, iv, meta, sizeof meta);
    }
    for (int i = 0; i < 16; i++)
        if (meta[0x10 + i] || meta[0x30 + i]) { *wrong_keys = true; *err = "the keys do not open this EBOOT.BIN"; return false; }

    /* the metadata headers, the section headers and the keys */
    const uint64_t mh = mi + 0x40;
    if (h.header_len <= mh) { *err = "the SELF's metadata is damaged"; return false; }
    std::vector<uint8_t> md(self.begin() + (long)mh, self.begin() + (long)h.header_len);
    aes.set_key(meta, 128);
    aes_ctr(aes, meta + 0x20, 0, md.data(), md.size());
    if (md.size() < 0x20) { *err = "the SELF's metadata is damaged"; return false; }
    const uint32_t nsec = rd32(&md[0x0C]), nkeys = rd32(&md[0x10]);
    if (nsec > 256 || nkeys > 1024 || 0x20 + (uint64_t)nsec * 0x30 + (uint64_t)nkeys * 16 > md.size()) {
        *err = "the SELF's metadata is damaged";
        return false;
    }
    const uint8_t* sec = &md[0x20];
    const uint8_t* dkeys = sec + nsec * 0x30;

    /* the ELF's own headers (in the clear) */
    Reader r{self};
    const uint8_t* eh = &self[(size_t)h.elf];
    if (memcmp(eh, "\x7F" "ELF", 4) != 0 || eh[4] != 2 || eh[5] != 2) { *err = "the SELF does not hold a 64-bit big-endian ELF"; return false; }
    const uint64_t e_phoff = rd64(eh + 0x20), e_shoff = rd64(eh + 0x28);
    const uint16_t e_phentsize = rd16(eh + 0x36), e_phnum = rd16(eh + 0x38), e_shentsize = rd16(eh + 0x3A), e_shnum = rd16(eh + 0x3C);
    if (e_phentsize != 0x38 || e_phnum == 0 || e_phnum > 64 || !r.has(h.phdr, (uint64_t)e_phnum * 0x38) ||
        (e_shnum && (e_shentsize != 0x40 || e_shnum > 4096 || !r.has(h.shdr, (uint64_t)e_shnum * 0x40)))) {
        *err = "the SELF's ELF headers are damaged";
        return false;
    }
    const uint8_t* ph = &self[(size_t)h.phdr];
    uint64_t end = std::max<uint64_t>(0x40, e_phoff + (uint64_t)e_phnum * 0x38);
    if (e_shnum) end = std::max(end, e_shoff + (uint64_t)e_shnum * 0x40);
    for (int i = 0; i < e_phnum; i++) end = std::max(end, rd64(ph + i * 0x38 + 0x08) + rd64(ph + i * 0x38 + 0x20));
    if (end > (1ull << 30)) { *err = "the SELF's ELF headers are damaged"; return false; }
    elf->assign((size_t)end, 0);
    memcpy(elf->data(), eh, 0x40);
    memcpy(elf->data() + e_phoff, ph, (size_t)e_phnum * 0x38);
    if (e_shnum) memcpy(elf->data() + e_shoff, &self[(size_t)h.shdr], (size_t)e_shnum * 0x40);

    /* the segments */
    for (uint32_t i = 0; i < nsec; i++) {
        const uint8_t* s = sec + i * 0x30;
        const uint64_t off = rd64(s), size = rd64(s + 8);
        const uint32_t type = rd32(s + 0x10), prog = rd32(s + 0x14), encrypted = rd32(s + 0x20);
        const uint32_t key_idx = rd32(s + 0x24), iv_idx = rd32(s + 0x28), compressed = rd32(s + 0x2C);
        if (type != 2) continue;   /* 2: a program segment */
        if (prog >= e_phnum || !r.has(off, size)) { *err = "the SELF's segment table is damaged"; return false; }
        const uint64_t p_offset = rd64(ph + prog * 0x38 + 0x08), p_filesz = rd64(ph + prog * 0x38 + 0x20);
        std::vector<uint8_t> data(self.begin() + (long)off, self.begin() + (long)(off + size));
        if (encrypted == 3) {
            if (key_idx >= nkeys || iv_idx >= nkeys) { *err = "the SELF's segment table is damaged"; return false; }
            aes.set_key(dkeys + key_idx * 16, 128);
            aes_ctr(aes, dkeys + iv_idx * 16, 0, data.data(), data.size());
        }
        if (compressed == 2) {
            uLongf got = (uLongf)p_filesz;
            if (uncompress(elf->data() + p_offset, &got, data.data(), (uLong)data.size()) != Z_OK || got != p_filesz) {
                *err = "a segment of the SELF does not decompress (damaged, or the wrong keys)";
                return false;
            }
        } else {
            memcpy(elf->data() + p_offset, data.data(), (size_t)std::min<uint64_t>(data.size(), p_filesz));
        }
    }
    return true;
}

}  // namespace dod3setup
