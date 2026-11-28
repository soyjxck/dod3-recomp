/*
 * PS3 package reading for the setup (src/install/pkg.h). The cryptography is
 * src/install/crypto.h; the retail package key is ps3recomp's own (its
 * tools/pkg_extract.py), which CMakeLists.txt writes into a header in the
 * build tree.
 */
#include "install/pkg.h"
#include "install/crypto.h"
#include "install/file.h"
#include "lang.h"
#include "ps3recomp_pkg_key.h"
#include "util.h"

#include <stdio.h>
#include <string.h>
#include <algorithm>
#include <memory>

namespace dod3setup {
namespace fs = std::filesystem;

namespace {

using dod3::utf8;

/* Decrypt (or encrypt: the same) bytes [off, off + n) of the data area: the
 * retail key's AES-CTR, or a debug package's SHA-1 keystream (pkg.h). */
void decrypt_range(const Pkg& pkg, const Aes& aes, uint64_t off, uint8_t* buf, size_t n)
{
    if (!pkg.debug) {
        aes_ctr(aes, pkg.riv, off, buf, n);
        return;
    }
    uint8_t in[64] = {}, h[20];
    memcpy(in, pkg.qa, 8);
    memcpy(in + 8, pkg.qa, 8);
    memcpy(in + 16, pkg.qa + 8, 8);
    memcpy(in + 24, pkg.qa + 8, 8);
    for (size_t i = 0; i < n;) {
        const uint64_t blk = (off + i) / 16, at = (off + i) % 16;
        for (int b = 0; b < 8; b++) in[56 + b] = (uint8_t)(blk >> (56 - 8 * b));
        Sha1 s;
        s.update(in, sizeof in);
        s.digest(h);
        const size_t take = std::min<size_t>(16 - (size_t)at, n - i);
        for (size_t j = 0; j < take; j++) buf[i + j] ^= h[at + j];
        i += take;
    }
}

}  // namespace

uint64_t Pkg::payload() const
{
    uint64_t n = 0;
    for (const PkgEntry& e : entries)
        if (!e.dir) n += e.size;
    return n;
}

std::string pkg_content_id(const fs::path& p)
{
    File f(p, "rb");
    uint8_t h[0x60];
    if (!f.f || !f.read(h, sizeof h) || dod3_be32(h) != kPkgMagic) return "";
    return std::string((const char*)h + 0x30, strnlen((const char*)h + 0x30, 0x24));
}

bool pkg_is_debug(const fs::path& p)
{
    File f(p, "rb");
    uint8_t h[8];
    return f.f && f.read(h, sizeof h) && dod3_be32(h) == kPkgMagic && !(h[4] & 0x80);
}

std::string pkg_footer_sha1(const fs::path& p)
{
    std::error_code ec;
    const uint64_t size = fs::file_size(p, ec);
    File f(p, "rb");
    uint8_t t[20];
    if (ec || size < 64 || !f.f || !f.seek(size - 32) || !f.read(t, sizeof t)) return "";
    return to_hex(t, sizeof t);
}

bool pkg_open(const fs::path& p, Pkg* pkg, std::string* err)
{
    if (!crypto_selftest()) {
        *err = dod3::tr("internal error: the cryptography self-test failed");
        return false;
    }
    std::error_code ec;
    pkg->path = p;
    pkg->file_size = fs::file_size(p, ec);
    File f(p, "rb");
    uint8_t h[0x80];
    if (ec || !f.f || !f.read(h, sizeof h) || dod3_be32(h) != kPkgMagic || dod3_be16(h + 6) != 1) {
        *err = dod3::tr("not a PS3 package");   /* the type at 6: 1 is the PS3's */
        return false;
    }
    pkg->debug = !(h[4] & 0x80);   /* the revision's top bit: retail */
    memcpy(pkg->qa, h + 0x60, 16);
    const uint32_t items = dod3_be32(h + 0x14);
    pkg->data_offset = dod3_be64(h + 0x20);
    pkg->data_size = dod3_be64(h + 0x28);
    pkg->content_id.assign((const char*)h + 0x30, strnlen((const char*)h + 0x30, 0x24));
    memcpy(pkg->riv, h + 0x70, 16);
    if (pkg->data_offset + pkg->data_size > pkg->file_size || items > 1000000) {
        *err = dod3::tr("the package is cut short");
        return false;
    }
    Aes aes;
    aes.set_key(k_ps3recomp_pkg_key, 128);
    std::vector<uint8_t> tab((size_t)items * 32);
    if (!f.seek(pkg->data_offset) || fread(tab.data(), 1, tab.size(), f.f) != tab.size()) {
        *err = dod3::tr("cannot read the package");
        return false;
    }
    decrypt_range(*pkg, aes, 0, tab.data(), tab.size());
    pkg->entries.clear();
    for (uint32_t i = 0; i < items; i++) {
        const uint8_t* e = tab.data() + (size_t)i * 32;
        const uint32_t name_off = dod3_be32(e), name_size = dod3_be32(e + 4);
        PkgEntry ent;
        ent.offset = dod3_be64(e + 8);
        ent.size = dod3_be64(e + 16);
        const uint8_t type = e[27];
        ent.dir = type == 4 || type == 0x12;
        if (name_size > 1024 || name_off + (uint64_t)name_size > pkg->data_size ||
            ent.offset + ent.size > pkg->data_size) {
            *err = dod3::tr("the package's file table is damaged");
            return false;
        }
        std::vector<uint8_t> nm(name_size);
        if (!f.seek(pkg->data_offset + name_off) || fread(nm.data(), 1, nm.size(), f.f) != nm.size()) {
            *err = dod3::tr("cannot read the package");
            return false;
        }
        decrypt_range(*pkg, aes, name_off, nm.data(), nm.size());
        ent.name.assign((const char*)nm.data(), strnlen((const char*)nm.data(), nm.size()));
        /* a colon would be a drive or a stream on Windows; no package of the console's has one, or a control character */
        bool odd = false;
        for (unsigned char c : ent.name) odd |= c < 0x20 || c == ':';
        if (ent.name.empty() || ent.name.find("..") != std::string::npos || ent.name[0] == '/' || ent.name[0] == '\\' ||
            odd) {
            *err = dod3::tr("the package names a file outside its folder");
            return false;
        }
        pkg->entries.push_back(ent);
    }
    return true;
}

bool pkg_extract(const Pkg& pkg, const fs::path& dest, const std::function<bool(uint64_t, uint64_t)>& progress,
                 std::string* err, const std::function<std::string(const std::string&)>& layout)
{
    Aes aes;
    aes.set_key(k_ps3recomp_pkg_key, 128);
    std::error_code ec;
    auto where = [&](const PkgEntry& e) { return layout ? layout(e.name) : e.name; };
    /* the folders, then the files to write, in data order */
    std::vector<const PkgEntry*> files;
    for (const PkgEntry& e : pkg.entries) {
        if (!e.dir)
            files.push_back(&e);
        else if (!where(e).empty())
            fs::create_directories(dest / fs::path(where(e)), ec);
    }
    std::sort(files.begin(), files.end(), [](const PkgEntry* a, const PkgEntry* b) { return a->offset < b->offset; });

    File in(pkg.path, "rb");
    if (!in.f) {
        *err = dod3::trf("cannot open {1}", utf8(pkg.path));
        return false;
    }
    Sha1 sha;
    const uint64_t hashed_end = pkg.file_size - 32;
    const size_t kChunk = 8u << 20;
    std::vector<uint8_t> buf(kChunk), work;
    size_t next = 0;                 /* index into files */
    std::unique_ptr<File> out;       /* the file being written */
    const PkgEntry* cur = nullptr;
    for (uint64_t pos = 0; pos < pkg.file_size;) {
        const size_t n = (size_t)std::min<uint64_t>(kChunk, pkg.file_size - pos);
        if (fread(buf.data(), 1, n, in.f) != n) {
            *err = dod3::trf("cannot read {1}", utf8(pkg.path));
            return false;
        }
        if (pos < hashed_end) sha.update(buf.data(), (size_t)std::min<uint64_t>(n, hashed_end - pos));
        /* the files this chunk overlaps */
        const uint64_t lo = pos, hi = pos + n;
        while (next < files.size() || cur) {
            if (!cur) {
                const PkgEntry* e = files[next];
                if (pkg.data_offset + e->offset >= hi) break;
                cur = e;
                next++;
                const std::string to = where(*e);
                if (!to.empty()) {   /* else left out: read past it */
                    const fs::path t = dest / fs::path(to);
                    fs::create_directories(t.parent_path(), ec);
                    out.reset(new File(t, "wb"));
                    if (!out->f) {
                        *err = dod3::trf("cannot write {1}", utf8(t));
                        return false;
                    }
                }
                if (e->size == 0) {
                    out.reset();
                    cur = nullptr;
                    continue;
                }
            }
            const uint64_t a = std::max(lo, pkg.data_offset + cur->offset);
            const uint64_t b = std::min(hi, pkg.data_offset + cur->offset + cur->size);
            if (a < b && out) {
                work.assign(buf.begin() + (size_t)(a - lo), buf.begin() + (size_t)(b - lo));
                decrypt_range(pkg, aes, a - pkg.data_offset, work.data(), work.size());
                if (fwrite(work.data(), 1, work.size(), out->f) != work.size()) {
                    *err = dod3::tr("disk full or not writable");
                    return false;
                }
            }
            if (b == pkg.data_offset + cur->offset + cur->size) {
                out.reset();
                cur = nullptr;
                continue;
            }
            break;   /* the current file goes on in the next chunk */
        }
        pos = hi;
        if (progress && !progress(pos, pkg.file_size)) {
            *err = dod3::tr("cancelled");
            return false;
        }
    }
    if (cur) {
        *err = dod3::tr("the package is cut short");
        return false;
    }
    const std::string want = pkg.expect_sha1.empty() ? pkg_footer_sha1(pkg.path) : pkg.expect_sha1;
    if (want != std::string(40, '0') && sha.hex() != want) {
        *err = dod3::trf("the package is damaged (its checksum does not match): {1}", utf8(pkg.path.filename()));
        return false;
    }
    return true;
}

bool pkg_read_file(const Pkg& pkg, const std::string& name, std::vector<uint8_t>* out, std::string* err)
{
    const PkgEntry* e = nullptr;
    for (const PkgEntry& x : pkg.entries)
        if (!x.dir && x.name == name) e = &x;
    if (!e) {
        *err = dod3::trf("the package has no {1}", name);
        return false;
    }
    if (e->size > (1ull << 30)) {
        *err = dod3::trf("{1} is too large to read whole", name);
        return false;
    }
    File f(pkg.path, "rb");
    out->resize((size_t)e->size);
    if (!f.f || !f.seek(pkg.data_offset + e->offset) || fread(out->data(), 1, out->size(), f.f) != out->size()) {
        *err = dod3::trf("cannot read {1} from {2}", name, utf8(pkg.path.filename()));
        return false;
    }
    Aes aes;
    aes.set_key(k_ps3recomp_pkg_key, 128);
    decrypt_range(pkg, aes, e->offset, out->data(), out->size());
    return true;
}

}  // namespace dod3setup
