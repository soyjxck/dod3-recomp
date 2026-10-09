/*
 * PS3 package reading for the setup (src/install/pkg.h). The cryptography is
 * src/install/crypto.h; the retail package key is ps3recomp's own (its
 * tools/pkg_extract.py), which CMakeLists.txt writes into a header in the
 * build tree.
 */
#include "install/pkg.h"
#include "install/crypto.h"
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

/* a FILE* closed when it goes, with 64-bit seeks */
struct File {
    FILE* f = nullptr;
    File(const fs::path& p, const char* mode) : f(dod3::open_file(p, mode)) {}
    ~File()
    {
        if (f) fclose(f);
    }
    bool seek(uint64_t o)
    {
#ifdef _WIN32
        return _fseeki64(f, (long long)o, SEEK_SET) == 0;
#else
        return fseeko(f, (off_t)o, SEEK_SET) == 0;
#endif
    }
};

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
    if (!f.f || fread(h, 1, sizeof h, f.f) != sizeof h || dod3_be32(h) != 0x7F504B47u) return "";
    return std::string((const char*)h + 0x30, strnlen((const char*)h + 0x30, 0x24));
}

std::string pkg_footer_sha1(const fs::path& p)
{
    std::error_code ec;
    const uint64_t size = fs::file_size(p, ec);
    File f(p, "rb");
    uint8_t t[20];
    if (ec || size < 64 || !f.f || !f.seek(size - 32) || fread(t, 1, 20, f.f) != 20) return "";
    char s[41];
    for (int i = 0; i < 20; i++) snprintf(s + 2 * i, 3, "%02x", t[i]);
    return s;
}

bool pkg_open(const fs::path& p, Pkg* pkg, std::string* err)
{
    if (!crypto_selftest()) {
        *err = "internal error: the cryptography self-test failed";
        return false;
    }
    std::error_code ec;
    pkg->path = p;
    pkg->file_size = fs::file_size(p, ec);
    File f(p, "rb");
    uint8_t h[0x80];
    if (ec || !f.f || fread(h, 1, sizeof h, f.f) != sizeof h || dod3_be32(h) != 0x7F504B47u) {
        *err = "not a PS3 package";
        return false;
    }
    if (!(h[4] & 0x80) || (h[6] << 8 | h[7]) != 1) {
        *err = "not a retail PS3 package";
        return false;
    }
    const uint32_t items = dod3_be32(h + 0x14);
    pkg->data_offset = dod3_be64(h + 0x20);
    pkg->data_size = dod3_be64(h + 0x28);
    pkg->content_id.assign((const char*)h + 0x30, strnlen((const char*)h + 0x30, 0x24));
    memcpy(pkg->riv, h + 0x70, 16);
    if (pkg->data_offset + pkg->data_size > pkg->file_size || items > 1000000) {
        *err = "the package is cut short";
        return false;
    }
    Aes aes;
    aes.set_key(k_ps3recomp_pkg_key, 128);
    std::vector<uint8_t> tab((size_t)items * 32);
    if (!f.seek(pkg->data_offset) || fread(tab.data(), 1, tab.size(), f.f) != tab.size()) {
        *err = "cannot read the package";
        return false;
    }
    aes_ctr(aes, pkg->riv, 0, tab.data(), tab.size());
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
            *err = "the package's file table is damaged";
            return false;
        }
        std::vector<uint8_t> nm(name_size);
        if (!f.seek(pkg->data_offset + name_off) || fread(nm.data(), 1, nm.size(), f.f) != nm.size()) {
            *err = "cannot read the package";
            return false;
        }
        aes_ctr(aes, pkg->riv, name_off, nm.data(), nm.size());
        ent.name.assign((const char*)nm.data(), strnlen((const char*)nm.data(), nm.size()));
        if (ent.name.empty() || ent.name.find("..") != std::string::npos || ent.name[0] == '/' || ent.name[0] == '\\') {
            *err = "the package names a file outside its folder";
            return false;
        }
        pkg->entries.push_back(ent);
    }
    return true;
}

bool pkg_extract(const Pkg& pkg, const fs::path& dest, const std::function<bool(uint64_t, uint64_t)>& progress,
                 std::string* err)
{
    Aes aes;
    aes.set_key(k_ps3recomp_pkg_key, 128);
    std::error_code ec;
    /* the folders, then the files to write, in data order */
    std::vector<const PkgEntry*> files;
    for (const PkgEntry& e : pkg.entries) {
        if (e.dir)
            fs::create_directories(dest / fs::path(e.name), ec);
        else
            files.push_back(&e);
    }
    std::sort(files.begin(), files.end(), [](const PkgEntry* a, const PkgEntry* b) { return a->offset < b->offset; });

    File in(pkg.path, "rb");
    if (!in.f) {
        *err = "cannot open " + utf8(pkg.path);
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
            *err = "cannot read " + utf8(pkg.path);
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
                const fs::path t = dest / fs::path(e->name);
                fs::create_directories(t.parent_path(), ec);
                out.reset(new File(t, "wb"));
                if (!out->f) {
                    *err = "cannot write " + utf8(t);
                    return false;
                }
                if (e->size == 0) {
                    out.reset();
                    cur = nullptr;
                    continue;
                }
            }
            const uint64_t a = std::max(lo, pkg.data_offset + cur->offset);
            const uint64_t b = std::min(hi, pkg.data_offset + cur->offset + cur->size);
            if (a < b) {
                work.assign(buf.begin() + (size_t)(a - lo), buf.begin() + (size_t)(b - lo));
                aes_ctr(aes, pkg.riv, a - pkg.data_offset, work.data(), work.size());
                if (fwrite(work.data(), 1, work.size(), out->f) != work.size()) {
                    *err = "disk full or not writable";
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
            *err = "cancelled";
            return false;
        }
    }
    if (cur) {
        *err = "the package is cut short";
        return false;
    }
    if (sha.hex() != pkg_footer_sha1(pkg.path)) {
        *err = "the package is damaged (its checksum does not match): " + utf8(pkg.path.filename());
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
        *err = "the package has no " + name;
        return false;
    }
    if (e->size > (1ull << 30)) {
        *err = name + " is too large to read whole";
        return false;
    }
    File f(pkg.path, "rb");
    out->resize((size_t)e->size);
    if (!f.f || !f.seek(pkg.data_offset + e->offset) || fread(out->data(), 1, out->size(), f.f) != out->size()) {
        *err = "cannot read " + name + " from " + utf8(pkg.path.filename());
        return false;
    }
    Aes aes;
    aes.set_key(k_ps3recomp_pkg_key, 128);
    aes_ctr(aes, pkg.riv, e->offset, out->data(), out->size());
    return true;
}

}  // namespace dod3setup
