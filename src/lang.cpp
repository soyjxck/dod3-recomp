/* The launcher's and the setup's text in the player's language (lang.h). */
#include "lang.h"
#include "ui/ini.h"
#include "util.h"
#include "launcher_langs.h"   /* generated: the catalogues in lang/, kLangFiles */

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <memory>
#include <mutex>
#include <unordered_map>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#elif defined(__APPLE__)
#include <CoreFoundation/CoreFoundation.h>
#endif

namespace dod3 {

namespace {

namespace fs = std::filesystem;

/* ---- a language's plural rule ------------------------------------------------- */

/* gettext's Plural-Forms expression (C's integer operators over n), worked
 * out as it is read. Both sides of a ?: are worked out, so a division by
 * zero is 0 rather than a fault. `bad` is set by anything it cannot read. */
struct PluralExpr {
    const char* p;
    uint64_t n;
    bool bad = false;
    int depth = 0;

    void ws()
    {
        while (*p == ' ' || *p == '\t') p++;
    }
    bool eat(const char* tok)
    {
        ws();
        const size_t l = strlen(tok);
        if (strncmp(p, tok, l)) return false;
        p += l;
        return true;
    }
    uint64_t primary()
    {
        ws();
        if (*p == '(') {
            p++;
            const uint64_t v = ternary();
            if (!eat(")")) bad = true;
            return v;
        }
        if (*p == 'n') {
            p++;
            return n;
        }
        if (*p == '!' && p[1] != '=') {
            p++;
            return !primary();
        }
        if (*p < '0' || *p > '9') {
            bad = true;
            return 0;
        }
        uint64_t v = 0;
        while (*p >= '0' && *p <= '9') v = v * 10 + (uint64_t)(*p++ - '0');
        return v;
    }
    uint64_t mul()
    {
        uint64_t v = primary();
        for (;;) {
            if (eat("*"))
                v *= primary();
            else if (eat("/")) {
                const uint64_t d = primary();
                v = d ? v / d : 0;
            } else if (eat("%")) {
                const uint64_t d = primary();
                v = d ? v % d : 0;
            } else
                return v;
        }
    }
    uint64_t add()
    {
        uint64_t v = mul();
        for (;;) {
            if (eat("+"))
                v += mul();
            else if (eat("-"))
                v -= mul();
            else
                return v;
        }
    }
    uint64_t rel()
    {
        uint64_t v = add();
        for (;;) {
            if (eat("<="))
                v = v <= add();
            else if (eat(">="))
                v = v >= add();
            else if (eat("<"))
                v = v < add();
            else if (eat(">"))
                v = v > add();
            else
                return v;
        }
    }
    uint64_t eq()
    {
        uint64_t v = rel();
        for (;;) {
            if (eat("=="))
                v = v == rel();
            else if (eat("!="))
                v = v != rel();
            else
                return v;
        }
    }
    uint64_t land()
    {
        uint64_t v = eq();
        while (eat("&&")) {
            const uint64_t r = eq();
            v = v && r;
        }
        return v;
    }
    uint64_t lor()
    {
        uint64_t v = land();
        while (eat("||")) {
            const uint64_t r = land();
            v = v || r;
        }
        return v;
    }
    uint64_t ternary()
    {
        if (++depth > 64) {   /* nothing a real rule comes near */
            bad = true;
            return 0;
        }
        const uint64_t c = lor();
        uint64_t v = c;
        if (eat("?")) {
            const uint64_t a = ternary();
            if (!eat(":")) bad = true;
            const uint64_t b = ternary();
            v = c ? a : b;
        }
        depth--;
        return v;
    }
};

/* ---- a catalogue --------------------------------------------------------------- */

struct SvHash {
    using is_transparent = void;
    size_t operator()(std::string_view s) const { return std::hash<std::string_view>{}(s); }
};

struct Catalog {
    std::string code, name;
    struct Entry {
        std::string context;
        std::vector<std::string> forms;   /* one, or the plural forms in the rule's order */
    };
    std::unordered_map<std::string, std::vector<Entry>, SvHash, std::equal_to<>> map;   /* by the English */
    std::string plural = "n != 1";
    unsigned nplurals = 2;

    const Entry* find(const char* context, const char* en) const
    {
        const auto it = map.find(std::string_view(en));
        if (it == map.end()) return nullptr;
        const std::string_view want = context ? context : "";
        for (const Entry& e : it->second)
            if (e.context == want) return &e;
        return nullptr;
    }
    unsigned form_of(uint64_t n) const
    {
        PluralExpr e{ plural.c_str(), n };
        const uint64_t v = e.ternary();
        e.ws();
        if (e.bad || *e.p) return n != 1;
        return (unsigned)std::min<uint64_t>(v, nplurals ? nplurals - 1 : 0);
    }
};

/* a .po file's quoted string at `s` ("..." with C's escapes), added to *out */
void po_string(std::string_view s, std::string* out)
{
    const size_t a = s.find('"');
    if (a == std::string_view::npos) return;
    for (size_t i = a + 1; i < s.size() && s[i] != '"'; i++) {
        if (s[i] != '\\' || i + 1 >= s.size()) {
            *out += s[i];
            continue;
        }
        const char c = s[++i];
        *out += c == 'n' ? '\n' : c == 't' ? '\t' : c == 'r' ? '\r' : c;
    }
}

/* the header's "Plural-Forms: nplurals=3; plural=(...);" */
void po_header(const std::string& h, Catalog* c)
{
    const size_t at = h.find("Plural-Forms:");
    if (at == std::string::npos) return;
    const size_t end = h.find('\n', at);
    const std::string line = h.substr(at, end == std::string::npos ? end : end - at);
    const size_t np = line.find("nplurals="), pl = line.find("plural=");
    if (np == std::string::npos || pl == std::string::npos) return;
    const int count = atoi(line.c_str() + np + 9);
    std::string expr = line.substr(pl + 7);
    if (const size_t semi = expr.find(';'); semi != std::string::npos) expr.resize(semi);
    PluralExpr probe{ expr.c_str(), 1 };
    probe.ternary();
    probe.ws();
    if (count < 1 || count > 8 || probe.bad || *probe.p) return;   /* not a rule: English's stays */
    c->nplurals = (unsigned)count;
    c->plural = expr;
}

/* A gettext catalogue's text read into *c: msgctxt, msgid, msgid_plural,
 * msgstr and msgstr[n], with their continuation lines; an entry marked
 * fuzzy, or without a translation, is left out (English shows). */
void po_parse(std::string_view text, Catalog* c)
{
    if (text.substr(0, 3) == "\xEF\xBB\xBF") text.remove_prefix(3);
    struct {
        std::string context, id, id_plural;
        std::vector<std::string> str;
        bool fuzzy = false, has_id = false, has_str = false;
    } e;
    std::string* field = nullptr;
    auto flush = [&] {
        if (e.has_id && e.has_str) {
            if (e.id.empty()) {
                if (!e.str.empty()) po_header(e.str[0], c);
            } else if (!e.fuzzy && !e.str.empty() && !e.str[0].empty()) {
                auto& list = c->map[e.id];
                bool known = false;
                for (const Catalog::Entry& x : list) known |= x.context == e.context;
                if (!known) list.push_back({ e.context, e.str });
            }
        }
        e = {};
        field = nullptr;
    };
    for (size_t at = 0; at <= text.size();) {
        size_t end = text.find('\n', at);
        if (end == std::string_view::npos) end = text.size();
        std::string_view l = text.substr(at, end - at);
        at = end + 1;
        while (!l.empty() && (l.back() == '\r' || l.back() == ' ' || l.back() == '\t')) l.remove_suffix(1);
        while (!l.empty() && (l.front() == ' ' || l.front() == '\t')) l.remove_prefix(1);
        if (l.empty()) {
            flush();
            continue;
        }
        if (l[0] == '#') {
            if (l.substr(0, 2) == "#~") continue;   /* an entry no longer in the source */
            if (e.has_str) flush();                 /* the next entry's comments */
            if (l.substr(0, 2) == "#," && l.find("fuzzy") != std::string_view::npos) e.fuzzy = true;
            continue;
        }
        if (l[0] == '"') {
            if (field) po_string(l, field);
            continue;
        }
        auto key = [&](std::string_view k) { return l.substr(0, k.size()) == k; };
        if (key("msgctxt")) {
            if (e.has_id) flush();
            field = &e.context;
        } else if (key("msgid_plural")) {
            field = &e.id_plural;
        } else if (key("msgid")) {
            if (e.has_id) flush();
            e.has_id = true;
            field = &e.id;
        } else if (key("msgstr")) {
            size_t i = 0;
            if (l.size() > 7 && l[6] == '[') i = (size_t)atoi(std::string(l.substr(7, 2)).c_str());
            if (i > 7) i = 7;
            if (e.str.size() <= i) e.str.resize(i + 1);
            e.has_str = true;
            field = &e.str[i];
        } else {
            field = nullptr;
            continue;
        }
        po_string(l, field);
    }
    flush();
    if (const Catalog::Entry* own = c->find("language", "English"); own && !own->forms[0].empty())
        c->name = own->forms[0];
    else
        c->name = c->code;
}

/* ---- the languages there are ---------------------------------------------------- */

std::mutex s_mu;                                        /* the three below */
fs::path s_dir;                                         /* the executable's folder */
std::map<std::string, std::unique_ptr<Catalog>> s_loaded;   /* by code, each read once; never freed */
std::atomic<const Catalog*> s_cur{ nullptr };           /* in use; none: English */
std::string s_code = "en";

std::string lower(std::string s)
{
    for (char& ch : s) ch = ch >= 'A' && ch <= 'Z' ? (char)(ch + 32) : ch == '_' ? '-' : ch;
    return s;
}

/* "ru_RU.UTF-8", "ru-RU" or "ru" as "ru-ru" / "ru" */
std::string tag_of(std::string s)
{
    if (const size_t cut = s.find_first_of(".@"); cut != std::string::npos) s.resize(cut);
    return lower(s);
}
std::string primary_of(const std::string& tag) { return tag.substr(0, tag.find('-')); }

bool read_file(const fs::path& p, std::string* out)
{
    FILE* f = open_file(p, "rb");
    if (!f) return false;
    char buf[65536];
    size_t n;
    while ((n = fread(buf, 1, sizeof buf, f)) > 0 && out->size() < (8u << 20)) out->append(buf, n);
    fclose(f);
    return true;
}

/* the lang folders: beside the executable, then the current directory's (as dod3.ini is found) */
std::vector<fs::path> lang_dirs()
{
    std::vector<fs::path> dirs;
    if (!s_dir.empty()) dirs.push_back(s_dir / "lang");
    dirs.push_back(fs::path("lang"));
    return dirs;
}

/* every catalogue's code as its file is named ("pt-BR"), the folders' and the built-in ones */
std::vector<std::string> codes_locked()
{
    std::vector<std::string> codes;
    auto add = [&](const std::string& c) {
        for (const std::string& have : codes)
            if (lower(have) == lower(c)) return;
        codes.push_back(c);
    };
    std::error_code ec;
    for (const fs::path& d : lang_dirs())
        for (fs::directory_iterator it(d, ec), end; !ec && it != end; it.increment(ec))
            if (lower(utf8(it->path().extension())) == ".po") add(utf8(it->path().stem()));
    for (const auto& f : kLangFiles)
        if (f.code) add(f.code);
    std::sort(codes.begin(), codes.end());
    return codes;
}

/* the catalogue with exactly this code, read the first time; nullptr if there is none */
const Catalog* load_locked(const std::string& code)
{
    const std::string k = lower(code);
    if (const auto it = s_loaded.find(k); it != s_loaded.end()) return it->second.get();
    std::string text;
    bool found = false;
    std::error_code ec;
    for (const fs::path& d : lang_dirs()) {
        for (fs::directory_iterator it(d, ec), end; !found && !ec && it != end; it.increment(ec))
            if (lower(utf8(it->path().extension())) == ".po" && lower(utf8(it->path().stem())) == k)
                found = read_file(it->path(), &text);
        if (found) break;
    }
    for (const auto& f : kLangFiles)
        if (!found && f.code && lower(f.code) == k) {
            text.assign((const char*)f.data, f.size);
            found = true;
        }
    if (!found) return nullptr;
    auto c = std::make_unique<Catalog>();
    c->code = code;
    po_parse(text, c.get());
    return (s_loaded[k] = std::move(c)).get();
}

/* the catalogue for a tag: its own, else its language's ("ru-ru": "ru") */
const Catalog* find_locked(const std::string& tag)
{
    const std::vector<std::string> codes = codes_locked();
    for (const std::string& want : { tag, primary_of(tag) })
        for (const std::string& c : codes)
            if (lower(c) == want) return load_locked(c);
    return nullptr;
}

/* the system's languages, the one preferred first */
std::vector<std::string> system_languages()
{
    std::vector<std::string> out;
    if (const char* t = getenv("DOD3_UI_LANGUAGE_SYSTEM")) { /* tests: as if the system's were these ("ru-RU:en-US") */
        for (const char* s = t; *s;) {
            const char* e = strchr(s, ':');
            out.push_back(e ? std::string(s, e) : std::string(s));
            if (!e) break;
            s = e + 1;
        }
        return out;
    }
#ifdef _WIN32
    ULONG count = 0, len = 0;
    if (GetUserPreferredUILanguages(MUI_LANGUAGE_NAME, &count, nullptr, &len) && len) {
        std::vector<wchar_t> buf(len);
        if (GetUserPreferredUILanguages(MUI_LANGUAGE_NAME, &count, buf.data(), &len))
            for (const wchar_t* p = buf.data(); *p; p += wcslen(p) + 1) {
                std::string s;
                for (const wchar_t* q = p; *q; q++) s += (char)(*q < 128 ? *q : '?');
                out.push_back(s);
            }
    }
#elif defined(__APPLE__)
    if (CFArrayRef list = CFLocaleCopyPreferredLanguages()) {
        for (CFIndex i = 0; i < CFArrayGetCount(list); i++) {
            char s[64];
            if (CFStringGetCString((CFStringRef)CFArrayGetValueAtIndex(list, i), s, sizeof s, kCFStringEncodingUTF8))
                out.push_back(s);
        }
        CFRelease(list);
    }
#else
    if (const char* l = getenv("LANGUAGE"); l && *l) {   /* a list, colons between */
        for (const char* s = l; *s;) {
            const char* e = strchr(s, ':');
            out.push_back(e ? std::string(s, e) : std::string(s));
            if (!e) break;
            s = e + 1;
        }
    }
    for (const char* var : { "LC_ALL", "LC_MESSAGES", "LANG" })
        if (const char* v = getenv(var); v && *v) {
            out.push_back(v);
            break;
        }
#endif
    return out;
}

/* ---- what a language lacks (tests) ------------------------------------------------- */

std::atomic<bool> s_log{ false };   /* DOD3_UI_LANGUAGE_LOG=1 */

/* A string the language in use has no line for, said once on stderr: with a
 * catalogue that has every string of lang/dod3.pot (tools/lang.py pseudo),
 * what is listed is text the source did not mark, or a name from a file. */
void missing(const char* en)
{
    static std::mutex mu;
    static std::unordered_map<std::string, bool> said;
    std::lock_guard<std::mutex> g(mu);
    if (said.emplace(en, true).second) fprintf(stderr, "[lang] no translation: %s\n", en);
}

}  // namespace

/* ---- the text ---------------------------------------------------------------------- */

const char* trc(const char* context, const char* en)
{
    if (!en || !*en) return en;
    const Catalog* c = s_cur.load(std::memory_order_acquire);
    if (!c) return en;
    const Catalog::Entry* e = c->find(context, en);
    if (!e && s_log.load(std::memory_order_relaxed)) missing(en);
    return e ? e->forms[0].c_str() : en;
}

const char* tr(const char* en) { return trc(nullptr, en); }

const char* trn(uint64_t n, const char* one, const char* other)
{
    const char* en = n == 1 ? one : other;
    const Catalog* c = s_cur.load(std::memory_order_acquire);
    if (!c) return en;
    const Catalog::Entry* e = c->find(nullptr, one);
    if (!e && s_log.load(std::memory_order_relaxed)) missing(en);
    if (!e) return en;
    const unsigned f = c->form_of(n);
    return f < e->forms.size() && !e->forms[f].empty() ? e->forms[f].c_str() : en;
}

std::string fill(std::string_view text, std::initializer_list<std::string> args)
{
    std::string out;
    out.reserve(text.size() + 32);
    for (size_t i = 0; i < text.size(); i++) {
        if (text[i] != '{') {
            out += text[i];
            continue;
        }
        if (i + 1 < text.size() && text[i + 1] == '{') {
            out += '{';
            i++;
            continue;
        }
        size_t j = i + 1, n = 0;
        while (j < text.size() && text[j] >= '0' && text[j] <= '9' && j - i < 4) n = n * 10 + (size_t)(text[j++] - '0');
        if (j > i + 1 && j < text.size() && text[j] == '}' && n >= 1 && n <= args.size()) {
            out += *(args.begin() + (n - 1));
            i = j;
        } else {
            out += text[i];
        }
    }
    return out;
}

/* ---- the language ------------------------------------------------------------------ */

std::vector<Language> languages()
{
    std::lock_guard<std::mutex> g(s_mu);
    std::vector<Language> out;
    for (const std::string& code : codes_locked())
        if (const Catalog* c = load_locked(code)) out.push_back({ code, c->name });
    return out;
}

std::string set_language(const std::string& code)
{
    std::lock_guard<std::mutex> g(s_mu);
    const std::string tag = tag_of(code);
    const Catalog* c = nullptr;
    if (tag.empty() || tag == "auto" || tag == "system") {
        for (const std::string& s : system_languages()) {
            const std::string t = tag_of(s);
            if (primary_of(t) == "en" || (c = find_locked(t))) break;   /* English before any other: English */
        }
    } else if (primary_of(tag) != "en") {
        c = find_locked(tag);
    }
    s_cur.store(c, std::memory_order_release);
    s_code = c ? c->code : "en";
    return s_code;
}

std::string language()
{
    std::lock_guard<std::mutex> g(s_mu);
    return s_code;
}

void lang_init(const fs::path& program_dir, const fs::path& ini)
{
    {
        std::lock_guard<std::mutex> g(s_mu);
        s_dir = program_dir;
    }
    if (const char* l = getenv("DOD3_UI_LANGUAGE_LOG")) s_log = *l == '1';
    std::string want;
    if (const char* e = getenv("DOD3_UI_LANGUAGE")) {
        want = e;
    } else {
        const auto kv = ini_read(ini);
        if (const auto it = kv.find("DOD3_UI_LANGUAGE"); it != kv.end()) want = it->second;
    }
    set_language(want);
}

}  // namespace dod3
