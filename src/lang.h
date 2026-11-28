/*
 * The launcher's and the setup's text in the player's language.
 *
 * Every sentence the two windows show is written in the source in English
 * and looked up by that wording: tr("Play") is "Play", or what the language
 * in use has for it. A language is a gettext catalogue, lang/<code>.po in the
 * repository (compiled into the program: gen/launcher_langs.h) or in a lang
 * folder beside the executable (which wins: a translator tries a file there
 * without building anything). lang/dod3.pot is the list of every string,
 * written by tools/lang.py from the calls below; docs/TRANSLATING.md is the
 * translators' page.
 *
 *   tr("Quit")                           a sentence as it is
 *   trc("heading", "Install")            one that reads differently in two
 *                                        places: the context tells them apart
 *   trf("Needs {1}; {2} free.", a, b)    one with parts filled in: {1}, {2}
 *                                        ... are the arguments, in any order
 *                                        the language likes
 *   trnf(n, "{1} file", "{1} files", n)  one that depends on a count: the
 *                                        language's own plural forms
 *   TR_NOOP("Display")                   one that is only stored here (a
 *                                        table); tr() where it is shown
 *
 * The language: DOD3_UI_LANGUAGE in the environment, else in dod3.ini (the
 * launcher's Settings > System row), else the system's. "en" is the
 * source's own; a code with no catalogue is English too. tr() can be called
 * from any thread; a language once loaded stays in memory, so what tr()
 * returned stays valid when the language changes.
 *
 * Tests: tools/lang.py pseudo writes a made-up translation of every string
 * (in brackets, accented, a third longer, Cyrillic at its end); as
 * lang/qps.po beside the executable with DOD3_UI_LANGUAGE=qps, a window
 * shows what was not marked (plain English) and what does not fit, and
 * DOD3_UI_LANGUAGE_LOG=1 lists on stderr each string the language in use has
 * no line for. DOD3_UI_LANGUAGE_SYSTEM=ru-RU:en-US stands in for the
 * system's languages.
 */
#pragma once
#include <cstdint>
#include <filesystem>
#include <initializer_list>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

#define TR_NOOP(s) s

namespace dod3 {

const char* tr(const char* en);
const char* trc(const char* context, const char* en);
/* The form for `n` things; English has the two given. */
const char* trn(uint64_t n, const char* one, const char* other);

/* `text` with {1}, {2}, ... replaced by the arguments ({{ is a brace). */
std::string fill(std::string_view text, std::initializer_list<std::string> args);

inline std::string lang_arg(const std::string& s) { return s; }
inline std::string lang_arg(const char* s) { return s ? s : ""; }
inline std::string lang_arg(std::string_view s) { return std::string(s); }
template <class T>
    requires std::is_arithmetic_v<T>
std::string lang_arg(T v)
{
    return std::to_string(v);
}

template <class... A> std::string trf(const char* en, const A&... a) { return fill(tr(en), { lang_arg(a)... }); }
template <class... A> std::string trcf(const char* context, const char* en, const A&... a)
{
    return fill(trc(context, en), { lang_arg(a)... });
}
template <class... A> std::string trnf(uint64_t n, const char* one, const char* other, const A&... a)
{
    return fill(trn(n, one, other), { lang_arg(a)... });
}

struct Language {
    std::string code;   /* "ru", "pt-BR": the file's name */
    std::string name;   /* its own name, as its catalogue gives it; the code if it has none */
};

/* The translations there are, by code: those compiled in and those in the
 * lang folder. English is not in the list. */
std::vector<Language> languages();

/* Use `code`: "" or "auto" the system's language, "en" English, else that
 * translation, or its language's ("ru-RU" finds "ru"). Returns the code now
 * in use ("en" when there is no translation for it). */
std::string set_language(const std::string& code);

/* The code in use. */
std::string language();

/* Once, before the first window: where the lang folder is looked for
 * (`program_dir`, the executable's folder) and the language to use, from the
 * environment's DOD3_UI_LANGUAGE, else the one in `ini`, else the system's. */
void lang_init(const std::filesystem::path& program_dir, const std::filesystem::path& ini);

}  // namespace dod3
