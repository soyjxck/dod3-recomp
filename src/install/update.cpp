/* Updates from the port's GitHub releases: see install/update.h. */
#include "install/update.h"
#include "install/crypto.h"    /* Sha256 */
#include "install/file.h"
#include "install/install.h"   /* free_bytes */
#include "install/zip.h"
#include "util.h"
#include "app_version.h"       /* generated: the port's version label */

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <utility>
#include <vector>

#ifdef _WIN32
#define NOMINMAX
#include <windows.h>
#include <shellapi.h>
#include <winhttp.h>
#pragma comment(lib, "winhttp.lib")
#endif

namespace dod3setup {

namespace {

const char kReleasesApi[] = "https://api.github.com/repos/soyjxck/dod3-recomp/releases?per_page=20";
const char kWorkDir[] = ".update";
const char kFromFile[] = "updated-from.txt";   /* in the work folder: the version an update came from */

/* ---- JSON, as much as GitHub's release list needs -------------------------------- */

struct Json {
    enum Kind { Null, Bool, Num, Str, Arr, Obj } kind = Null;
    bool b = false;
    double n = 0;
    std::string s;
    std::vector<Json> a;
    std::vector<std::pair<std::string, Json>> o;

    const Json* get(const char* key) const
    {
        for (const auto& kv : o)
            if (kv.first == key) return &kv.second;
        return nullptr;
    }
    std::string str(const char* key) const
    {
        const Json* j = get(key);
        return j && j->kind == Str ? j->s : std::string();
    }
};

struct JsonReader {
    const char* p;
    const char* end;   /* the text ends in a NUL (std::string), so strtod stops there too */
    int depth = 0;

    void ws()
    {
        while (p < end && (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r')) p++;
    }
    bool lit(const char* t)
    {
        const size_t n = strlen(t);
        if ((size_t)(end - p) < n || memcmp(p, t, n) != 0) return false;
        p += n;
        return true;
    }
    bool hex4(uint32_t* v)
    {
        if (end - p < 4) return false;
        *v = 0;
        for (int i = 0; i < 4; i++) {
            const char c = *p++;
            const int d = c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
            if (d < 0) return false;
            *v = *v << 4 | (uint32_t)d;
        }
        return true;
    }
    static void put_utf8(std::string& s, uint32_t c)
    {
        if (c < 0x80) {
            s += (char)c;
        } else if (c < 0x800) {
            s += (char)(0xC0 | c >> 6);
            s += (char)(0x80 | (c & 0x3F));
        } else if (c < 0x10000) {
            s += (char)(0xE0 | c >> 12);
            s += (char)(0x80 | (c >> 6 & 0x3F));
            s += (char)(0x80 | (c & 0x3F));
        } else {
            s += (char)(0xF0 | c >> 18);
            s += (char)(0x80 | (c >> 12 & 0x3F));
            s += (char)(0x80 | (c >> 6 & 0x3F));
            s += (char)(0x80 | (c & 0x3F));
        }
    }
    bool string(std::string* out)
    {
        if (p >= end || *p != '"') return false;
        p++;
        while (p < end && *p != '"') {
            const char c = *p++;
            if ((unsigned char)c < 0x20) return false;
            if (c != '\\') {
                *out += c;
                continue;
            }
            if (p >= end) return false;
            switch (*p++) {
            case '"': *out += '"'; break;
            case '\\': *out += '\\'; break;
            case '/': *out += '/'; break;
            case 'b': *out += '\b'; break;
            case 'f': *out += '\f'; break;
            case 'n': *out += '\n'; break;
            case 'r': *out += '\r'; break;
            case 't': *out += '\t'; break;
            case 'u': {
                uint32_t u, lo;
                if (!hex4(&u)) return false;
                if (u >= 0xD800 && u < 0xDC00) {   /* a surrogate pair */
                    if (!lit("\\u") || !hex4(&lo) || lo < 0xDC00 || lo >= 0xE000) return false;
                    u = 0x10000 + ((u - 0xD800) << 10) + (lo - 0xDC00);
                }
                put_utf8(*out, u);
                break;
            }
            default: return false;
            }
        }
        if (p >= end) return false;
        p++;
        return true;
    }
    bool value(Json* v)
    {
        if (++depth > 64) return false;
        ws();
        if (p >= end) return false;
        bool ok = true;
        if (*p == '{' || *p == '[') {
            const bool obj = *p == '{';
            const char close = obj ? '}' : ']';
            v->kind = obj ? Json::Obj : Json::Arr;
            p++;
            ws();
            if (p < end && *p == close) {
                p++;
            } else {
                for (;;) {
                    Json* item;
                    if (obj) {
                        std::string k;
                        ws();
                        if (!string(&k)) return false;
                        ws();
                        if (p >= end || *p != ':') return false;
                        p++;
                        v->o.emplace_back(std::move(k), Json());
                        item = &v->o.back().second;
                    } else {
                        v->a.emplace_back();
                        item = &v->a.back();
                    }
                    if (!value(item)) return false;
                    ws();
                    if (p < end && *p == ',') {
                        p++;
                        continue;
                    }
                    if (p < end && *p == close) {
                        p++;
                        break;
                    }
                    return false;
                }
            }
        } else if (*p == '"') {
            v->kind = Json::Str;
            ok = string(&v->s);
        } else if (lit("true")) {
            v->kind = Json::Bool;
            v->b = true;
        } else if (lit("false")) {
            v->kind = Json::Bool;
        } else if (lit("null")) {
            v->kind = Json::Null;
        } else {
            char* e = nullptr;
            v->n = strtod(p, &e);
            if (e == p || e > end) return false;
            v->kind = Json::Num;
            p = e;
        }
        depth--;
        return ok;
    }
};

bool json_parse(const std::string& text, Json* out)
{
    JsonReader r{ text.data(), text.data() + text.size() };
    if (!r.value(out)) return false;
    r.ws();
    return r.p == r.end;
}

/* ---- versions ----------------------------------------------------------------- */

struct Version {
    std::vector<unsigned> nums;
    int rank = 3;        /* alpha 0, beta 1, rc 2, a final release 3 */
    unsigned extra = 0;  /* "beta.2": 2 */
};

Version parse_version(std::string v)
{
    Version r;
    size_t i = 0;
    while (i < v.size() && (v[i] == 'v' || v[i] == 'V' || v[i] == ' ')) i++;
    while (i < v.size() && isdigit((unsigned char)v[i])) {
        unsigned n = 0;
        while (i < v.size() && isdigit((unsigned char)v[i])) n = n * 10 + (unsigned)(v[i++] - '0');
        r.nums.push_back(n);
        if (i < v.size() && v[i] == '.' && i + 1 < v.size() && isdigit((unsigned char)v[i + 1]))
            i++;
        else
            break;
    }
    std::string rest;
    for (; i < v.size(); i++) rest += (char)tolower((unsigned char)v[i]);
    if (rest.find("alpha") != std::string::npos || rest.find("preview") != std::string::npos)
        r.rank = 0;
    else if (rest.find("beta") != std::string::npos)
        r.rank = 1;
    else if (rest.find("rc") != std::string::npos)
        r.rank = 2;
    size_t k = rest.size();
    while (k > 0 && isdigit((unsigned char)rest[k - 1])) k--;
    if (k < rest.size()) r.extra = (unsigned)strtoul(rest.c_str() + k, nullptr, 10);
    return r;
}

/* "v0.4.2-alpha" -> "0.4.2 alpha" */
std::string label_of(const std::string& tag)
{
    std::string l = tag;
    if (!l.empty() && (l[0] == 'v' || l[0] == 'V')) l.erase(0, 1);
    std::replace(l.begin(), l.end(), '-', ' ');
    return l;
}

bool ends_with(const std::string& s, const char* t)
{
    const size_t n = strlen(t);
    return s.size() >= n && s.compare(s.size() - n, n, t) == 0;
}

bool is_hex64(const std::string& s)
{
    if (s.size() != 64) return false;
    for (char c : s)
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) return false;
    return true;
}

/* A path the release may name inside the program folder: relative, '/' separated, no "." or ".." part, no
 * drive, no control characters. */
bool safe_path(const std::string& p)
{
    if (p.empty() || p[0] == '/' || p.find('\\') != std::string::npos || p.find(':') != std::string::npos) return false;
    size_t at = 0;
    for (;;) {
        const size_t e = p.find('/', at);
        const std::string part = p.substr(at, e == std::string::npos ? std::string::npos : e - at);
        if (part.empty() || part == "." || part == "..") return false;
        for (char c : part)
            if ((unsigned char)c < 0x20) return false;
        if (e == std::string::npos) return true;
        at = e + 1;
    }
}

/* Paths an update never writes: the installed game, the saves, and the player's settings when there are some. */
bool kept(const std::string& rel, const fs::path& dir)
{
    std::string l = rel;
    for (char& c : l) c = (char)tolower((unsigned char)c);
    if (!l.compare(0, 5, "game/") || !l.compare(0, 9, "gamedata/") || !l.compare(0, 8, ".update/")) return true;
    std::error_code ec;
    return l == "dod3.ini" && fs::exists(dir / "dod3.ini", ec);
}

/* ---- HTTPS ---------------------------------------------------------------------- */

#ifdef _WIN32
std::wstring wide(const std::string& s)
{
    std::wstring w(s.size(), L'\0');
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), w.data(), (int)w.size());
    w.resize(n > 0 ? (size_t)n : 0);
    return w;
}

struct Handle {
    HINTERNET h = nullptr;
    ~Handle()
    {
        if (h) WinHttpCloseHandle(h);
    }
};

/* GET `url` (https only; GitHub's redirects to its download host followed); the body to `sink` as it
 * arrives, its length to *total when the answer gives one. `api`: GitHub's API headers. */
bool http_get(const std::string& url, bool api, const std::function<bool(const uint8_t*, size_t)>& sink,
              std::atomic<uint64_t>* got, std::atomic<uint64_t>* total, const std::atomic<bool>* cancel,
              std::string* err)
{
    const std::wstring w = wide(url);
    URL_COMPONENTS parts = {};
    parts.dwStructSize = sizeof parts;
    parts.dwHostNameLength = parts.dwUrlPathLength = parts.dwExtraInfoLength = (DWORD)-1;
    if (!WinHttpCrackUrl(w.c_str(), 0, 0, &parts) || parts.nScheme != INTERNET_SCHEME_HTTPS) {
        *err = "not an https address: " + url;
        return false;
    }
    const std::wstring host(parts.lpszHostName, parts.dwHostNameLength);
    const std::wstring object = std::wstring(parts.lpszUrlPath, parts.dwUrlPathLength) +
                                std::wstring(parts.lpszExtraInfo, parts.dwExtraInfoLength);
    std::string agent = std::string("dod3-recomp/") + DOD3_VERSION_STRING;
    std::replace(agent.begin(), agent.end(), ' ', '-');
    Handle s, c, q;
    s.h = WinHttpOpen(wide(agent).c_str(), WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME,
                      WINHTTP_NO_PROXY_BYPASS, 0);
    if (s.h) WinHttpSetTimeouts(s.h, 15000, 15000, 15000, 30000);
    if (s.h) c.h = WinHttpConnect(s.h, host.c_str(), parts.nPort, 0);
    if (c.h)
        q.h = WinHttpOpenRequest(c.h, L"GET", object.c_str(), nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
                                 WINHTTP_FLAG_SECURE);
    const wchar_t* headers = api ? L"Accept: application/vnd.github+json\r\nX-GitHub-Api-Version: 2022-11-28\r\n" : nullptr;
    if (!q.h || !WinHttpSendRequest(q.h, headers ? headers : WINHTTP_NO_ADDITIONAL_HEADERS, headers ? (DWORD)-1L : 0,
                                    WINHTTP_NO_REQUEST_DATA, 0, 0, 0) ||
        !WinHttpReceiveResponse(q.h, nullptr)) {
        *err = "GitHub could not be reached (error " + std::to_string(GetLastError()) + ")";
        return false;
    }
    DWORD code = 0, len = sizeof code;
    WinHttpQueryHeaders(q.h, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX, &code,
                        &len, WINHTTP_NO_HEADER_INDEX);
    if (code != 200) {
        *err = "GitHub answered " + std::to_string(code);
        return false;
    }
    DWORD clen = 0;
    len = sizeof clen;
    if (total && WinHttpQueryHeaders(q.h, WINHTTP_QUERY_CONTENT_LENGTH | WINHTTP_QUERY_FLAG_NUMBER,
                                     WINHTTP_HEADER_NAME_BY_INDEX, &clen, &len, WINHTTP_NO_HEADER_INDEX) && clen)
        *total = clen;
    std::vector<uint8_t> buf(1 << 16);
    for (;;) {
        if (cancel && *cancel) {
            *err = "Cancelled";
            return false;
        }
        DWORD avail = 0, read = 0;
        if (!WinHttpQueryDataAvailable(q.h, &avail)) {
            *err = "the download stopped (error " + std::to_string(GetLastError()) + ")";
            return false;
        }
        if (!avail) return true;
        if (!WinHttpReadData(q.h, buf.data(), std::min<DWORD>(avail, (DWORD)buf.size()), &read)) {
            *err = "the download stopped (error " + std::to_string(GetLastError()) + ")";
            return false;
        }
        if (!sink(buf.data(), read)) {
            *err = "could not write the download";
            return false;
        }
        if (got) *got += read;
    }
}

bool move_file(const fs::path& from, const fs::path& to, bool replace)
{
    return MoveFileExW(from.c_str(), to.c_str(), replace ? MOVEFILE_REPLACE_EXISTING : 0) != 0;
}
#endif

}  // namespace

/* ---- the release list ------------------------------------------------------------- */

bool version_newer(const std::string& a, const std::string& b)
{
    const Version x = parse_version(a), y = parse_version(b);
    for (size_t i = 0; i < std::max(x.nums.size(), y.nums.size()); i++) {
        const unsigned m = i < x.nums.size() ? x.nums[i] : 0, n = i < y.nums.size() ? y.nums[i] : 0;
        if (m != n) return m > n;
    }
    if (x.rank != y.rank) return x.rank > y.rank;
    return x.extra > y.extra;
}

bool pick_release(const std::string& json, const std::string& current, const char* asset_end, Release* out,
                  std::string* err)
{
    Json root;
    if (!json_parse(json, &root) || root.kind != Json::Arr) {
        if (err) *err = "GitHub's list of releases could not be read";
        return false;
    }
    bool any = false;
    for (const Json& r : root.a) {
        if (r.kind != Json::Obj) continue;
        const Json* draft = r.get("draft");
        if (draft && draft->kind == Json::Bool && draft->b) continue;
        const std::string tag = r.str("tag_name");
        if (tag.empty() || !version_newer(tag, current) || (any && !version_newer(tag, out->tag))) continue;
        const Json* assets = r.get("assets");
        if (!assets || assets->kind != Json::Arr) continue;
        for (const Json& a : assets->a) {
            const std::string name = a.str("name"), url = a.str("browser_download_url");
            std::string digest = a.str("digest");
            for (char& c : digest) c = (char)tolower((unsigned char)c);
            /* GitHub's own download link, a plain file name, and a SHA-256 to check it against */
            if (!ends_with(name, asset_end) || !safe_path(name) || name.find('/') != std::string::npos ||
                url.compare(0, 19, "https://github.com/") != 0 || digest.compare(0, 7, "sha256:") != 0 ||
                !is_hex64(digest.substr(7)))
                continue;
            const Json* size = a.get("size");
            Release rel;
            rel.tag = tag;
            rel.label = label_of(tag);
            rel.page = r.str("html_url");
            rel.notes = r.str("body");
            rel.date = r.str("published_at").substr(0, 10);
            rel.url = url;
            rel.name = name;
            rel.size = size && size->kind == Json::Num && size->n > 0 ? (uint64_t)size->n : 0;
            rel.sha256 = digest.substr(7);
            *out = rel;
            any = true;
            break;
        }
    }
    return any;
}

std::string update_current_version()
{
    const char* as = getenv("DOD3_UPDATE_AS");
    return as && *as ? as : DOD3_VERSION_STRING;
}

/* ---- the check ---------------------------------------------------------------------- */

void UpdateCheck::start()
{
    state = std::make_shared<State>();
    std::thread([st = state] {
        std::string json, err;
        bool read = false;
        if (const char* f = getenv("DOD3_UPDATE_API"); f && *f) {
            File in(fs::u8path(f), "rb");
            if (in.f) {
                char b[4096];
                for (size_t n; (n = fread(b, 1, sizeof b, in.f)) > 0;) json.append(b, n);
                read = true;
            } else {
                err = std::string("cannot read ") + f;
            }
        } else {
#ifdef _WIN32
            read = http_get(kReleasesApi, true,
                            [&](const uint8_t* p, size_t n) {
                                json.append((const char*)p, n);
                                return json.size() < (8u << 20);
                            },
                            nullptr, nullptr, nullptr, &err);
#else
            err = "not on this platform";
#endif
        }
        if (read) st->found = pick_release(json, update_current_version(), "-win64.zip", &st->release, &err);
        if (st->found)
            fprintf(stderr, "[update] %s is out (this is %s)\n", st->release.label.c_str(),
                    update_current_version().c_str());
        else
            fprintf(stderr, "[update] no newer release%s%s\n", err.empty() ? "" : ": ", err.c_str());
        st->error = err;
        st->finished = true;
    }).detach();
}

bool UpdateCheck::collect()
{
    if (!state || !state->finished) return false;
    found = state->found;
    release = state->release;
    error = state->error;
    state.reset();
    return true;
}

/* ---- the install ---------------------------------------------------------------------- */

UpdateJob::~UpdateJob()
{
    cancel = true;
    if (thread.joinable()) thread.join();
}

std::string UpdateJob::what() const
{
    switch (step.load()) {
    case kDownloading: return "Downloading";
    case kChecking: return "Checking the download";
    case kUnpacking: return "Unpacking";
    case kInstalling: return "Installing";
    default: return "Done";
    }
}

bool UpdateJob::collect()
{
    if (!thread.joinable() || !finished) return false;
    thread.join();
    return true;
}

void UpdateJob::start(const fs::path& dir, const Release& r)
{
    thread = std::thread([this, dir, r] {
        const auto fail = [&](const std::string& why) {
            std::error_code ec;
            fs::remove_all(dir / kWorkDir, ec);   /* nothing of a failed try is kept: the download, the unpacked files */
            error = why;
            fprintf(stderr, "[update] failed: %s\n", why.c_str());
            finished = true;
        };
#ifndef _WIN32
        (void)dir;
        (void)r;
        fail("Updates are installed this way on Windows only.");
#else
        std::error_code ec;
        const fs::path work = dir / kWorkDir, files = work / "files";
        fs::remove_all(files, ec);   /* a cancelled or failed attempt's */
        fs::create_directories(files, ec);
        if (ec) return fail("Could not make the folder " + dod3::utf8(files) + ".");
        const uint64_t space = free_bytes(dir);
        if (space && r.size && space < r.size * 4)
            return fail("Not enough space on the drive: the update needs about " + std::to_string(r.size * 4 >> 20) +
                        " MB while it is installed.");

        /* 1. the download, hashed as it is written */
        step = kDownloading;
        done = 0;
        total = r.size ? r.size : 1;
        const fs::path zip = work / r.name;
        Sha256 sha;
        uint64_t got = 0;
        {
            File out(zip, "wb");
            if (!out.f) return fail("Could not write " + dod3::utf8(zip) + ".");
            const auto sink = [&](const uint8_t* p, size_t n) {
                sha.update(p, n);
                got += n;
                return fwrite(p, 1, n, out.f) == n;
            };
            std::string err;
            bool whole;
            if (const char* test = getenv("DOD3_UPDATE_ZIP"); test && *test) {   /* tests: a local file */
                File in(fs::u8path(test), "rb");
                whole = in.f != nullptr;
                std::vector<uint8_t> b(1 << 16);
                for (size_t n; whole && (n = fread(b.data(), 1, b.size(), in.f)) > 0;) {
                    whole = sink(b.data(), n) && !cancel;
                    done += n;
                }
                if (!whole) err = cancel ? "Cancelled" : std::string("cannot read ") + test;
            } else {
                whole = http_get(r.url, false, sink, &done, &total, &cancel, &err);
            }
            if (!whole) return fail(err == "Cancelled" ? err : "The download failed: " + err + ".");
        }

        /* 2. its size and hash, as GitHub lists them */
        step = kChecking;
        if (r.size && got != r.size)
            return fail("The download is not the size GitHub lists for it. Nothing was installed.");
        if (sha.hex() != r.sha256)
            return fail("The download does not match the SHA-256 GitHub lists for it. Nothing was installed.");

        /* 3. unpacked, every entry checked against its CRC-32 */
        step = kUnpacking;
        std::vector<ZipEntry> entries;
        std::string err;
        if (!zip_list(zip, &entries, &err)) return fail("The download could not be unpacked: " + err + ".");
        /* the release's files sit in one folder named after it: its name is left off */
        std::string top;
        for (const ZipEntry& e : entries) {
            std::string n = e.name;
            std::replace(n.begin(), n.end(), '\\', '/');
            const size_t sl = n.find('/');
            const std::string first = sl == std::string::npos ? "" : n.substr(0, sl + 1);
            if (&e == &entries[0])
                top = first;
            else if (first != top)
                top.clear();
        }
        struct Item {
            const ZipEntry* e;
            std::string rel;
        };
        std::vector<Item> items;
        uint64_t packed = 0;
        bool has_exe = false;
        for (const ZipEntry& e : entries) {
            /* names as Windows PowerShell's Compress-Archive writes them: '\' separated, so a folder's entry
             * ends in '\' (install/zip.h knows folders by a '/') */
            std::string n = e.name;
            std::replace(n.begin(), n.end(), '\\', '/');
            if (e.dir || n.empty() || n.back() == '/') continue;
            if (n.compare(0, top.size(), top) != 0) continue;
            const std::string rel = n.substr(top.size());
            if (!safe_path(rel)) return fail("The download names a file outside its folder (" + n + "). Nothing was installed.");
            if (kept(rel, dir)) continue;
            has_exe |= rel == "dod3.exe";
            items.push_back({ &e, rel });
            packed += e.csize;
        }
        if (!has_exe) return fail("The download has no dod3.exe. Nothing was installed.");
        done = 0;
        total = packed ? packed : 1;
        uint64_t before = 0;
        for (const Item& it : items) {
            if (!zip_extract(zip, *it.e, files / fs::u8path(it.rel),
                             [&](uint64_t n) {
                                 done = before + n;
                                 return !cancel;
                             },
                             &err))
                return fail(cancel ? "Cancelled" : "The download could not be unpacked (" + it.rel + "): " + err + ".");
            before += it.e->csize;
        }
        fs::remove(zip, ec);

        /* 4. swapped in: the old file aside as <name>.old, the new one in its place; any failure puts back
         * every file swapped so far */
        step = kInstalling;
        done = 0;
        total = items.size();
        struct Swap {
            fs::path target;
            bool had;
        };
        std::vector<Swap> swapped;
        std::string failed;
        for (const Item& it : items) {
            const fs::path target = dir / fs::u8path(it.rel), old = fs::path(target.native() + L".old");
            fs::create_directories(target.parent_path(), ec);
            const bool had = fs::exists(target, ec);
            if (had && !move_file(target, old, true)) {
                failed = it.rel;
                break;
            }
            if (!move_file(files / fs::u8path(it.rel), target, false)) {
                if (had) move_file(old, target, true);
                failed = it.rel;
                break;
            }
            swapped.push_back({ target, had });
            done++;
        }
        if (!failed.empty()) {
            bool back = true;
            for (auto s = swapped.rbegin(); s != swapped.rend(); ++s) {
                const fs::path old = fs::path(s->target.native() + L".old");
                if (!DeleteFileW(s->target.c_str()) && s->had) back = false;
                if (s->had && !move_file(old, s->target, true)) back = false;
            }
            changed = !back;
            return fail(failed + " could not be replaced (is another program using it?). " +
                        (back ? "Every file was put back as it was." : "Some files could not be put back: unzip the "
                                                                        "release over this folder to finish."));
        }

        /* 5. what is left goes at the new version's first start (update_cleanup): the running program's .old
         * cannot be deleted while it runs */
        for (const Swap& s : swapped)
            if (s.had) fs::remove(fs::path(s.target.native() + L".old"), ec);
        fs::remove_all(files, ec);
        if (File f(work / kFromFile, "wb"); f.f) fputs(update_current_version().c_str(), f.f);
        fprintf(stderr, "[update] %zu files of %s installed\n", items.size(), r.label.c_str());
        step = kDone;
        ok = true;
        finished = true;
#endif
    });
}

/* ---- after an update -------------------------------------------------------------- */

bool update_cleanup(const fs::path& dir, std::string* from)
{
    std::error_code ec;
    const fs::path work = dir / kWorkDir;
    if (!fs::is_directory(work, ec)) return false;
    bool updated = false;
    if (File f(work / kFromFile, "rb"); f.f) {
        char b[64] = {};
        const size_t n = fread(b, 1, sizeof b - 1, f.f);
        *from = std::string(b, n);
        updated = true;
    }
    fs::remove(work / kFromFile, ec);   /* said once */
    /* <name>.old beside each file of the program's own folders (not the game's, not the saves): the files
     * the update set aside. Retried for a few seconds: the program that installed it may still be closing.
     * The work folder goes once none is left, so a later start tries again. */
    std::thread([dir, work] {
        bool left = true;
        for (int tries = 0; tries < 40 && left; tries++) {
            if (tries) std::this_thread::sleep_for(std::chrono::milliseconds(250));
            left = false;
            std::error_code walk, ec;
            for (fs::recursive_directory_iterator it(dir, fs::directory_options::skip_permission_denied, walk), end;
                 !walk && it != end; it.increment(walk)) {
                const fs::path p = it->path();
                if (it.depth() == 0 && it->is_directory(ec)) {
                    std::string n = dod3::utf8(p.filename());
                    for (char& c : n) c = (char)tolower((unsigned char)c);
                    if (n == "game" || n == "gamedata" || n == kWorkDir) it.disable_recursion_pending();
                    continue;
                }
                if (p.extension() != ".old" || !it->is_regular_file(ec)) continue;
                if (!fs::exists(fs::path(p).replace_extension(), ec)) continue;   /* only the copies an update set aside */
                if (!fs::remove(p, ec)) left = true;
            }
        }
        std::error_code ec;
        if (!left) fs::remove_all(work, ec);
    }).detach();
    return updated;
}

fs::path update_program_dir()
{
#ifdef _WIN32
    wchar_t exe[MAX_PATH] = L"";
    GetModuleFileNameW(NULL, exe, MAX_PATH);
    return fs::path(exe).parent_path();
#else
    return fs::path();
#endif
}

bool update_restart(const fs::path& dir)
{
#ifdef _WIN32
    const fs::path exe = dir / "dod3.exe";
    std::wstring cmd = L"\"" + exe.native() + L"\"";
    STARTUPINFOW si = {};
    si.cb = sizeof si;
    PROCESS_INFORMATION pi = {};
    if (!CreateProcessW(exe.c_str(), cmd.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr, &si, &pi)) return false;
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return true;
#else
    (void)dir;
    return false;
#endif
}

void open_in_browser(const std::string& url)
{
    if (url.compare(0, 8, "https://") != 0) return;
#ifdef _WIN32
    ShellExecuteW(nullptr, L"open", wide(url).c_str(), nullptr, nullptr, SW_SHOWNORMAL);
#endif
}

}  // namespace dod3setup
