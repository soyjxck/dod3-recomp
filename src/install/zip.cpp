/* .zip reading for the setup: see install/zip.h. */
#include "install/zip.h"
#include "install/file.h"
#include "lang.h"
#include "util.h"

#include <zlib.h>
#include <cstdio>
#include <cstring>
#include <algorithm>
#include <memory>

namespace dod3setup {

namespace fs = std::filesystem;

namespace {

uint16_t le16(const uint8_t* p) { return (uint16_t)(p[0] | p[1] << 8); }
uint32_t le32(const uint8_t* p) { return (uint32_t)(p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24); }

/* The records of a .zip (PKWARE's APPNOTE): their signatures and fixed sizes. */
const uint32_t kEndSig = 0x06054b50u;       /* the end of the central directory */
const uint32_t kCentralSig = 0x02014b50u;   /* an entry in the central directory */
const uint32_t kLocalSig = 0x04034b50u;     /* an entry's own header, before its data */
const size_t kEndSize = 22, kCentralSize = 46, kLocalSize = 30;

}  // namespace

bool zip_list(const fs::path& zip, std::vector<ZipEntry>* entries, std::string* err)
{
    entries->clear();
    std::error_code ec;
    const uint64_t size = fs::file_size(zip, ec);
    File in(zip, "rb");
    if (ec || !in.f || size < kEndSize) {
        *err = dod3::tr("cannot read the archive");
        return false;
    }
    /* the end of central directory record: in the last 64 KB (its comment's most) and its own size */
    const uint64_t tail = std::min<uint64_t>(size, 65536 + kEndSize);
    std::vector<uint8_t> t((size_t)tail);
    if (!in.seek(size - tail) || !in.read(t.data(), t.size())) {
        *err = dod3::tr("cannot read the archive");
        return false;
    }
    size_t eocd = SIZE_MAX;
    for (size_t i = t.size() - kEndSize + 1; i-- > 0;)
        if (le32(&t[i]) == kEndSig) {
            eocd = i;
            break;
        }
    if (eocd == SIZE_MAX) {
        *err = dod3::tr("not a .zip archive");
        return false;
    }
    const uint32_t count = le16(&t[eocd + 10]), cd_size = le32(&t[eocd + 12]), cd_off = le32(&t[eocd + 16]);
    if (cd_off == 0xFFFFFFFFu || count == 0xFFFF) {
        *err = dod3::tr("a ZIP64 archive, which the setup does not read: unpack it and add what is inside");
        return false;
    }
    if ((uint64_t)cd_off + cd_size > size) {
        *err = dod3::tr("the archive is cut short");
        return false;
    }
    std::vector<uint8_t> cd(cd_size);
    if (!in.seek(cd_off) || !in.read(cd.data(), cd.size())) {
        *err = dod3::tr("cannot read the archive");
        return false;
    }
    for (size_t p = 0, i = 0; i < count; i++) {
        if (p + kCentralSize > cd.size() || le32(&cd[p]) != kCentralSig) {
            *err = dod3::tr("the archive's directory is damaged");
            return false;
        }
        ZipEntry e;
        const uint16_t flags = le16(&cd[p + 8]);
        e.method = le16(&cd[p + 10]);
        e.crc = le32(&cd[p + 16]);
        e.csize = le32(&cd[p + 20]);
        e.usize = le32(&cd[p + 24]);
        const uint16_t n = le16(&cd[p + 28]), x = le16(&cd[p + 30]), c = le16(&cd[p + 32]);
        e.header = le32(&cd[p + 42]);
        if (p + kCentralSize + n > cd.size()) {
            *err = dod3::tr("the archive's directory is damaged");
            return false;
        }
        e.name.assign((const char*)&cd[p + kCentralSize], n);
        e.dir = !e.name.empty() && e.name.back() == '/';
        if (flags & 1) {
            *err = dod3::tr("an encrypted archive");
            return false;
        }
        if (!e.dir && e.method != 0 && e.method != 8) {
            *err = dod3::trf("the archive uses a compression the setup does not read ({1})", e.method);
            return false;
        }
        if (e.name.find("..") != std::string::npos || e.name[0] == '/' || e.name[0] == '\\') {
            *err = dod3::tr("the archive names a file outside its folder");
            return false;
        }
        entries->push_back(e);
        p += kCentralSize + (size_t)n + x + c;
    }
    return true;
}

bool zip_extract(const fs::path& zip, const ZipEntry& e, const fs::path& dest,
                 const std::function<bool(uint64_t)>& progress, std::string* err)
{
    File in(zip, "rb");
    uint8_t lh[kLocalSize];
    if (!in.f || !in.seek(e.header) || !in.read(lh, sizeof lh) || le32(lh) != kLocalSig) {
        *err = dod3::trf("cannot read {1} from the archive", e.name);
        return false;
    }
    if (!in.seek(e.header + kLocalSize + le16(&lh[26]) + le16(&lh[28]))) {   /* past its name and extra field */
        *err = dod3::trf("cannot read {1} from the archive", e.name);
        return false;
    }
    std::error_code ec;
    fs::create_directories(dest.parent_path(), ec);
    File out(dest, "wb");
    if (!out.f) {
        *err = dod3::trf("cannot write {1}", dod3::utf8(dest));
        return false;
    }
    std::vector<uint8_t> ibuf(1u << 20), obuf(1u << 20);
    uLong crc = crc32(0, nullptr, 0);
    uint64_t left = e.csize, written = 0, done = 0;
    z_stream z{};
    if (e.method == 8 && inflateInit2(&z, -MAX_WBITS) != Z_OK) {
        *err = dod3::tr("internal error: zlib");
        return false;
    }
    std::unique_ptr<z_stream, void (*)(z_stream*)> zguard(e.method == 8 ? &z : nullptr,
                                                          [](z_stream* s) { inflateEnd(s); });
    bool ended = e.method != 8;
    while (left > 0) {
        const size_t n = (size_t)std::min<uint64_t>(ibuf.size(), left);
        if (!in.read(ibuf.data(), n)) {
            *err = dod3::tr("the archive is cut short");
            return false;
        }
        left -= n;
        done += n;
        if (e.method == 0) {
            crc = crc32(crc, ibuf.data(), (uInt)n);
            if (fwrite(ibuf.data(), 1, n, out.f) != n) {
                *err = dod3::tr("disk full or not writable");
                return false;
            }
            written += n;
        } else {
            z.next_in = ibuf.data();
            z.avail_in = (uInt)n;
            /* until the input is used up and nothing more is pending: a full
             * output buffer may leave output behind (zlib's zpipe loop) */
            do {
                z.next_out = obuf.data();
                z.avail_out = (uInt)obuf.size();
                const int r = inflate(&z, Z_NO_FLUSH);
                if (r == Z_BUF_ERROR) break;   /* nothing to do until more input comes */
                if (r != Z_OK && r != Z_STREAM_END) {
                    *err = dod3::trf("{1} in the archive is damaged", e.name);
                    return false;
                }
                const size_t got = obuf.size() - z.avail_out;
                crc = crc32(crc, obuf.data(), (uInt)got);
                if (fwrite(obuf.data(), 1, got, out.f) != got) {
                    *err = dod3::tr("disk full or not writable");
                    return false;
                }
                written += got;
                ended = r == Z_STREAM_END;
            } while (!ended && (z.avail_in > 0 || z.avail_out == 0));
        }
        if (progress && !progress(done)) {
            *err = dod3::tr("cancelled");
            return false;
        }
    }
    if (!ended || written != e.usize || crc != e.crc) {
        *err = dod3::trf("{1} in the archive is damaged (its checksum does not match)", e.name);
        return false;
    }
    return true;
}

}  // namespace dod3setup
