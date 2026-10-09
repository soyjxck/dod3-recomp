/* The keys for the game's executable: see install/keys.h. */
#include "install/keys.h"
#include "install/keys_builtin.h"
#include "install/crypto.h"
#include "cpu/eboot.h"

#include <cstring>
#include <fstream>
#include <sstream>

namespace dod3setup {

namespace fs = std::filesystem;

const std::vector<KeyField>& needed_keys()
{
#if DOD3_EBOOT == 101
    static const std::vector<KeyField> k = {
        { "npdrm_erk", "NPDRM ERK (key revision 0x1C)", 32 },
        { "npdrm_riv", "NPDRM RIV (key revision 0x1C)", 16 },
        { "klic_free", "NP klic free", 16 },
        { "klic_key", "NP klic key", 16 },
    };
#else
    static const std::vector<KeyField> k = {
        { "app_erk", "APP ERK (key revision 0x1C)", 32 },
        { "app_riv", "APP RIV (key revision 0x1C)", 16 },
    };
#endif
    return k;
}

bool Keys::complete() const
{
    for (const KeyField& f : needed_keys()) {
        auto it = v.find(f.id);
        if (it == v.end() || it->second.size() != f.bytes) return false;
    }
    return true;
}

SelfKeys Keys::self_keys() const
{
    SelfKeys s;
    auto get = [&](const char* id, uint8_t* out, size_t n) {
        auto it = v.find(id);
        if (it != v.end() && it->second.size() == n) memcpy(out, it->second.data(), n);
    };
#if DOD3_EBOOT == 101
    get("npdrm_erk", s.erk, 32);
    get("npdrm_riv", s.riv, 16);
    get("klic_free", s.klic_free, 16);
    get("klic_key", s.klic_key, 16);
#else
    get("app_erk", s.erk, 32);
    get("app_riv", s.riv, 16);
#endif
    return s;
}

std::string key_problem(const KeyField& f, const std::string& text, std::vector<uint8_t>* bytes)
{
    std::vector<uint8_t> b;
    if (!from_hex(text, &b)) return "not hex";
    if (b.size() != f.bytes) {
        return std::to_string(b.size()) + " bytes; this key is " + std::to_string(f.bytes) + " (" +
               std::to_string(f.bytes * 2) + " hex digits)";
    }
    if (bytes) *bytes = b;
    return "";
}

bool keys_parse(const std::string& text, Keys* k, std::string* err)
{
    std::istringstream in(text);
    std::string line;
    int n = 0;
    while (std::getline(in, line)) {
        n++;
        const size_t hash = line.find('#');
        if (hash != std::string::npos) line.resize(hash);
        const size_t eq = line.find('=');
        auto trim = [](std::string s) {
            const size_t a = s.find_first_not_of(" \t\r\n"), b = s.find_last_not_of(" \t\r\n");
            return a == std::string::npos ? std::string() : s.substr(a, b - a + 1);
        };
        if (trim(line).empty()) continue;
        if (eq == std::string::npos) {
            *err = "line " + std::to_string(n) + " is not name=hex";
            return false;
        }
        const std::string name = trim(line.substr(0, eq)), value = trim(line.substr(eq + 1));
        for (const KeyField& f : needed_keys()) {
            if (name != f.id) continue;
            std::vector<uint8_t> b;
            const std::string p = key_problem(f, value, &b);
            if (!p.empty()) {
                *err = name + ": " + p;
                return false;
            }
            k->v[f.id] = b;
        }
    }
    return true;
}

bool keys_load(const fs::path& file, Keys* k, std::string* err)
{
    std::ifstream f(file, std::ios::binary);
    if (!f) {
        *err = "cannot read the key file";
        return false;
    }
    std::stringstream ss;
    ss << f.rdbuf();
    return keys_parse(ss.str(), k, err);
}

bool keys_save(const fs::path& file, const Keys& k)
{
    std::ofstream f(file, std::ios::binary | std::ios::trunc);
    if (!f) return false;
    f << "# The keys this install's executable was made with (the setup asks for them once).\n";
    for (const KeyField& kf : needed_keys()) {
        auto it = k.v.find(kf.id);
        if (it != k.v.end()) f << kf.id << "=" << to_hex(it->second.data(), it->second.size()) << "\n";
    }
    return (bool)f;
}

fs::path keys_file(const fs::path& base) { return base / "keys.txt"; }

const Keys& builtin_keys()
{
    static const Keys k = [] {
        Keys r;
        for (const auto& b : k_builtin_keys) {
            if (!*b.hex) continue;
            for (const KeyField& f : needed_keys()) {
                std::vector<uint8_t> v;
                if (f.id == std::string(b.id) && key_problem(f, b.hex, &v).empty()) r.v[f.id] = v;
            }
        }
        return r;
    }();
    return k;
}

}  // namespace dod3setup
