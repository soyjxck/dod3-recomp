/* First-run setup, portable half: see setup_iso.h. */
#include "setup_iso.h"

#include <cstdio>
#include <cstring>
#include <fstream>
#include <memory>

namespace dod3setup {

const char* const kEbootElfSha256 = "82e0f955658428b226828c4045d0865185cabc3a57c91fb54fbe2186f44290bb";
const char* const kEbootBinSha256 = "d251717ade653a74290fd2d5028083c575b9c6f9a253ca0c230f65ea6f5197c8";
const char* const kTitleId = "BLUS31197";
const char* const kAppVer = "01.00";
const uint64_t kDiscBytes = 16ull << 30;

/* A path as UTF-8 in a std::string (u8string is std::u8string from C++20). */
static std::string utf8(const fs::path& p) { const auto s = p.u8string(); return std::string(s.begin(), s.end()); }
static std::string utf8_generic(const fs::path& p) { const auto s = p.generic_u8string(); return std::string(s.begin(), s.end()); }

/* ---- SHA-256 (FIPS 180-4) ------------------------------------------------- */
namespace {
struct Sha256 {
    uint32_t h[8] = { 0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19 };
    uint8_t buf[64]; size_t n = 0; uint64_t bits = 0;
    static uint32_t ror(uint32_t x, int r) { return (x >> r) | (x << (32 - r)); }
    void block(const uint8_t* p)
    {
        static const uint32_t k[64] = {
            0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
            0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
            0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
            0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
            0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
            0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
            0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
            0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2 };
        uint32_t w[64];
        for (int i = 0; i < 16; i++) w[i] = (uint32_t)p[4 * i] << 24 | p[4 * i + 1] << 16 | p[4 * i + 2] << 8 | p[4 * i + 3];
        for (int i = 16; i < 64; i++) {
            const uint32_t s0 = ror(w[i - 15], 7) ^ ror(w[i - 15], 18) ^ (w[i - 15] >> 3);
            const uint32_t s1 = ror(w[i - 2], 17) ^ ror(w[i - 2], 19) ^ (w[i - 2] >> 10);
            w[i] = w[i - 16] + s0 + w[i - 7] + s1;
        }
        uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4], f = h[5], g = h[6], hh = h[7];
        for (int i = 0; i < 64; i++) {
            const uint32_t t1 = hh + (ror(e, 6) ^ ror(e, 11) ^ ror(e, 25)) + ((e & f) ^ (~e & g)) + k[i] + w[i];
            const uint32_t t2 = (ror(a, 2) ^ ror(a, 13) ^ ror(a, 22)) + ((a & b) ^ (a & c) ^ (b & c));
            hh = g; g = f; f = e; e = d + t1; d = c; c = b; b = a; a = t1 + t2;
        }
        h[0] += a; h[1] += b; h[2] += c; h[3] += d; h[4] += e; h[5] += f; h[6] += g; h[7] += hh;
    }
    void add(const uint8_t* p, size_t len)
    {
        bits += (uint64_t)len * 8;
        while (len) {
            const size_t take = len < 64 - n ? len : 64 - n;
            memcpy(buf + n, p, take); n += take; p += take; len -= take;
            if (n == 64) { block(buf); n = 0; }
        }
    }
    std::string hex()
    {
        const uint64_t b = bits;
        const uint8_t one = 0x80, zero = 0;
        add(&one, 1);
        while (n != 56) add(&zero, 1);
        uint8_t len[8];
        for (int i = 0; i < 8; i++) len[i] = (uint8_t)(b >> (56 - 8 * i));
        add(len, 8);
        static const char* x = "0123456789abcdef";
        std::string s;
        for (uint32_t v : h) for (int i = 28; i >= 0; i -= 4) s += x[(v >> i) & 0xF];
        return s;
    }
};
}  // namespace

std::string sha256_file(const fs::path& p)
{
    std::ifstream f(p, std::ios::binary);
    if (!f) return "";
    Sha256 s;
    std::unique_ptr<char[]> buf(new char[1 << 20]);
    while (f) {
        f.read(buf.get(), 1 << 20);
        if (f.gcount() > 0) s.add((const uint8_t*)buf.get(), (size_t)f.gcount());
    }
    return s.hex();
}

/* ---- PARAM.SFO -------------------------------------------------------------- */
bool param_sfo(const std::vector<uint8_t>& d, std::string* title_id, std::string* app_ver)
{
    auto le32 = [&](size_t o) { return (uint32_t)d[o] | (uint32_t)d[o + 1] << 8 | (uint32_t)d[o + 2] << 16 | (uint32_t)d[o + 3] << 24; };
    auto le16 = [&](size_t o) { return (uint32_t)d[o] | (uint32_t)d[o + 1] << 8; };
    if (d.size() < 20 || memcmp(d.data(), "\0PSF", 4) != 0) return false;
    const uint32_t kt = le32(8), dt = le32(12), n = le32(16);
    if ((uint64_t)20 + (uint64_t)n * 16 > d.size()) return false;
    for (uint32_t i = 0; i < n; i++) {
        const size_t e = 20 + (size_t)i * 16;
        const uint32_t ko = le16(e), len = le32(e + 4), dof = le32(e + 12);
        if (kt + ko >= d.size() || (uint64_t)dt + dof + len > d.size()) continue;
        const char* key = (const char*)&d[kt + ko];
        std::string val((const char*)&d[dt + dof], len);
        val = val.c_str();                                   /* to the first NUL */
        if (!strncmp(key, "TITLE_ID", 9) && title_id) *title_id = val;
        if (!strncmp(key, "APP_VER", 8) && app_ver) *app_ver = val;
    }
    return true;
}

Installed check_installed(const fs::path& base)
{
    std::error_code ec;
    Installed r;
    r.disc = fs::exists(base / "game/disc/PS3_GAME/PARAM.SFO", ec) &&
             fs::exists(base / "game/disc/PS3_GAME/USRDIR/EBOOT.BIN", ec);
    r.elf = fs::file_size(base / "elf/EBOOT.ELF", ec) > 0 && !ec;
    return r;
}

/* ---- ISO 9660 ------------------------------------------------------------------
 * The primary volume descriptor's tree, or Joliet's when the image has one (its
 * names are not truncated). Multi-extent files (over 4 GB) are not expected on
 * this disc and are refused. */
namespace {
constexpr uint32_t kSector = 2048;

struct Iso {
    std::ifstream f;
    bool read(uint64_t off, void* dst, size_t n)
    {
        f.clear();
        f.seekg((std::streamoff)off);
        f.read((char*)dst, (std::streamsize)n);
        return (size_t)f.gcount() == n;
    }
};

uint32_t le32(const uint8_t* p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }

std::string iso_name(const uint8_t* p, size_t n, bool joliet)
{
    std::string s;
    if (joliet) {
        for (size_t i = 0; i + 1 < n; i += 2) {
            const uint32_t c = (uint32_t)p[i] << 8 | p[i + 1];
            if (c < 0x80) s += (char)c;
            else if (c < 0x800) { s += (char)(0xC0 | (c >> 6)); s += (char)(0x80 | (c & 0x3F)); }
            else { s += (char)(0xE0 | (c >> 12)); s += (char)(0x80 | ((c >> 6) & 0x3F)); s += (char)(0x80 | (c & 0x3F)); }
        }
    } else {
        s.assign((const char*)p, n);
    }
    const size_t semi = s.find(';');
    if (semi != std::string::npos) s.resize(semi);
    if (!s.empty() && s.back() == '.') s.pop_back();
    return s;
}

bool walk(Iso& iso, uint32_t lba, uint32_t len, const std::string& prefix, bool joliet,
          std::vector<DiscFile>* out, int depth, std::string* err)
{
    if (depth > 32) { *err = "the disc image's folders nest too deep (not an ISO 9660 image?)"; return false; }
    std::vector<uint8_t> dir(len);
    if (!iso.read((uint64_t)lba * kSector, dir.data(), len)) { *err = "the disc image is truncated"; return false; }
    for (uint32_t o = 0; o < len;) {
        const uint8_t rl = dir[o];
        if (rl == 0) { o = (o / kSector + 1) * kSector; continue; }      /* records do not span sectors */
        if (o + rl > len || rl < 34) break;
        const uint8_t* r = &dir[o];
        const uint32_t ext = le32(r + 2), size = le32(r + 10);
        const uint8_t flags = r[25], nl = r[32];
        o += rl;
        if (nl == 1 && (r[33] == 0 || r[33] == 1)) continue;            /* . and .. */
        if (flags & 0x80) { *err = "the disc image has a file split over several extents, which is not supported"; return false; }
        const std::string name = iso_name(r + 33, nl, joliet);
        const std::string path = prefix.empty() ? name : prefix + "/" + name;
        if (flags & 0x02) {
            if (!walk(iso, ext, size, path, joliet, out, depth + 1, err)) return false;
        } else {
            out->push_back({ path, (uint64_t)ext * kSector, size });
        }
    }
    return true;
}

bool list_iso(const fs::path& p, std::vector<DiscFile>* files, std::string* err)
{
    Iso iso;
    iso.f.open(p, std::ios::binary);
    if (!iso.f) { *err = "the disc image could not be opened"; return false; }
    uint8_t vd[kSector];
    const uint8_t* root = nullptr;
    bool joliet = false;
    uint8_t pvd_root[34] = {}, svd_root[34] = {};
    bool have_pvd = false, have_svd = false;
    for (uint32_t s = 16; s < 64; s++) {
        if (!iso.read((uint64_t)s * kSector, vd, kSector)) break;
        if (memcmp(vd + 1, "CD001", 5) != 0) break;
        if (vd[0] == 1 && !have_pvd) { memcpy(pvd_root, vd + 156, 34); have_pvd = true; }
        if (vd[0] == 2 && vd[88] == '%' && vd[89] == '/' && (vd[90] == '@' || vd[90] == 'C' || vd[90] == 'E')) {
            memcpy(svd_root, vd + 156, 34); have_svd = true;
        }
        if (vd[0] == 255) break;
    }
    if (!have_pvd) { *err = "this is not an ISO 9660 disc image"; return false; }
    if (have_svd) { root = svd_root; joliet = true; } else root = pvd_root;
    return walk(iso, le32(root + 2), le32(root + 10), "", joliet, files, 0, err);
}

bool list_folder(const fs::path& p, std::vector<DiscFile>* files, std::string* err)
{
    std::error_code ec;
    for (fs::recursive_directory_iterator it(p, ec), end; it != end && !ec; it.increment(ec)) {
        if (!it->is_regular_file(ec)) continue;
        const std::string rel = utf8_generic(fs::relative(it->path(), p, ec));
        files->push_back({ rel, 0, (uint64_t)it->file_size(ec) });
    }
    if (ec) { *err = "the folder could not be read: " + ec.message(); return false; }
    return true;
}

const DiscFile* find(const Disc& d, const std::string& path)
{
    for (const auto& f : d.files) {
        if (f.path.size() != path.size()) continue;
        bool same = true;                                   /* ISO names are upper case; folders may not be */
        for (size_t i = 0; i < path.size() && same; i++) same = toupper((unsigned char)f.path[i]) == toupper((unsigned char)path[i]);
        if (same) return &f;
    }
    return nullptr;
}
}  // namespace

bool read_disc_file(const Disc& disc, const std::string& path, std::vector<uint8_t>* out)
{
    const DiscFile* f = find(disc, path);
    if (!f || f->size > (256u << 20)) return false;
    out->resize((size_t)f->size);
    if (disc.is_iso) {
        std::ifstream in(disc.source, std::ios::binary);
        in.seekg((std::streamoff)f->offset);
        in.read((char*)out->data(), (std::streamsize)f->size);
        return (uint64_t)in.gcount() == f->size;
    }
    std::ifstream in(disc.source / fs::u8path(f->path), std::ios::binary);
    in.read((char*)out->data(), (std::streamsize)f->size);
    return (uint64_t)in.gcount() == f->size;
}

bool open_disc(const fs::path& source, Disc* disc, std::string* err)
{
    std::error_code ec;
    disc->source = source;
    disc->files.clear();
    disc->is_iso = fs::is_regular_file(source, ec);
    if (!disc->is_iso) {
        /* The folder that holds PS3_GAME; accept PS3_GAME itself too. */
        if (!fs::exists(source / "PS3_GAME", ec) && fs::exists(source / "PARAM.SFO", ec) && fs::exists(source / "USRDIR", ec))
            disc->source = source.parent_path();
    }
    if (!(disc->is_iso ? list_iso(disc->source, &disc->files, err) : list_folder(disc->source, &disc->files, err)))
        return false;
    disc->total = 0;
    for (const auto& f : disc->files) disc->total += f.size;

    std::vector<uint8_t> sfo;
    std::string tid, ver;
    if (!read_disc_file(*disc, "PS3_GAME/PARAM.SFO", &sfo) || !param_sfo(sfo, &tid, &ver)) {
        *err = disc->is_iso ? "no PS3_GAME/PARAM.SFO in this disc image: it is not a PS3 game disc, or it is still disc-encrypted"
                            : "no PS3_GAME/PARAM.SFO in this folder: choose the folder that contains PS3_GAME";
        return false;
    }
    if (tid != kTitleId) {
        *err = "this is " + tid + ", not Drakengard 3 (US) " + kTitleId + ". Only the US disc is supported.";
        return false;
    }
    if (ver != kAppVer) {
        *err = "this disc is version " + ver + "; only version " + kAppVer + " is supported.";
        return false;
    }
    std::vector<uint8_t> eboot;
    if (!read_disc_file(*disc, "PS3_GAME/USRDIR/EBOOT.BIN", &eboot)) { *err = "the disc has no PS3_GAME/USRDIR/EBOOT.BIN"; return false; }
    Sha256 s; s.add(eboot.data(), eboot.size());
    if (s.hex() != kEbootBinSha256) {
        *err = disc->is_iso ? "this disc image's EBOOT.BIN does not match the supported release. If the image came "
                              "straight from a disc drive it is still disc-encrypted: make a decrypted one "
                              "(for example with PS3 Disc Dumper)."
                            : "this folder's EBOOT.BIN does not match the supported release (BLUS31197 v01.00, no update).";
        return false;
    }
    return true;
}

bool copy_disc(const Disc& disc, const fs::path& dest, const std::function<bool(uint64_t, uint64_t)>& progress,
               std::string* err)
{
    std::error_code ec;
    fs::create_directories(dest, ec);
    std::ifstream iso;
    if (disc.is_iso) { iso.open(disc.source, std::ios::binary); if (!iso) { *err = "the disc image could not be opened"; return false; } }
    std::unique_ptr<char[]> buf(new char[4 << 20]);
    uint64_t done = 0;
    for (const auto& f : disc.files) {
        const fs::path to = dest / fs::u8path(f.path);
        fs::create_directories(to.parent_path(), ec);
        std::ofstream out(to, std::ios::binary | std::ios::trunc);
        if (!out) { *err = "could not write " + utf8(to); return false; }
        std::ifstream in;
        if (disc.is_iso) iso.seekg((std::streamoff)f.offset);
        else { in.open(disc.source / fs::u8path(f.path), std::ios::binary); if (!in) { *err = "could not read " + f.path; return false; } }
        std::istream& src = disc.is_iso ? (std::istream&)iso : (std::istream&)in;
        for (uint64_t left = f.size; left;) {
            const size_t n = (size_t)(left < (4u << 20) ? left : (4u << 20));
            src.read(buf.get(), (std::streamsize)n);
            if ((size_t)src.gcount() != n) { *err = "a read failed in " + f.path + " (the disc image is truncated?)"; return false; }
            out.write(buf.get(), (std::streamsize)n);
            if (!out) { *err = "a write failed for " + utf8(to) + " (disk full?)"; return false; }
            left -= n; done += n;
            if (!progress(done, disc.total)) { *err = "cancelled"; return false; }
        }
    }
    return true;
}

int install_cli(const fs::path& base, const fs::path& disc_path, const fs::path& elf)
{
    Disc disc;
    std::string err;
    if (!open_disc(disc_path, &disc, &err)) { printf("disc: %s\n", err.c_str()); return 1; }
    printf("disc: OK, %zu files, %.1f GB\n", disc.files.size(), (double)disc.total / (1u << 30));
    if (sha256_file(elf) != kEbootElfSha256) {
        printf("EBOOT.ELF: does not match BLUS31197 v01.00 (decrypt the disc's EBOOT.BIN with RPCS3)\n");
        return 1;
    }
    std::error_code ec;
    const fs::path part = base / "game/disc.partial", final_dir = base / "game/disc";
    fs::remove_all(part, ec);
    int last = -1;
    const bool ok = copy_disc(disc, part, [&](uint64_t done, uint64_t total) {
        const int pct = total ? (int)(done * 100 / total) : 100;
        if (pct != last) { last = pct; printf("\rcopying the disc: %3d%%", pct); fflush(stdout); }
        return true;
    }, &err);
    printf("\n");
    if (!ok) { fs::remove_all(part, ec); printf("copy failed: %s\n", err.c_str()); return 1; }
    fs::remove_all(final_dir, ec);
    fs::rename(part, final_dir, ec);
    if (ec) { printf("could not finish: %s\n", ec.message().c_str()); return 1; }
    fs::create_directories(base / "elf", ec);
    fs::copy_file(elf, base / "elf/EBOOT.ELF", fs::copy_options::overwrite_existing, ec);
    if (ec) { printf("could not copy %s: %s\n", utf8(elf).c_str(), ec.message().c_str()); return 1; }
    printf("installed into %s\n", utf8(base).c_str());
    return 0;
}

}  // namespace dod3setup
