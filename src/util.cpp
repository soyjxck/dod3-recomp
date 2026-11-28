/* Small helpers shared by the port's sources: see util.h. */
#include "util.h"

#include <stdlib.h>
#include <time.h>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#endif

extern "C" uint64_t dod3_now_us(void)
{
#ifdef _WIN32
    static LARGE_INTEGER f;
    LARGE_INTEGER c;
    if (!f.QuadPart) QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&c);
    return (uint64_t)(c.QuadPart / f.QuadPart) * 1000000ull +
           (uint64_t)(c.QuadPart % f.QuadPart) * 1000000ull / (uint64_t)f.QuadPart;
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000ull + (uint64_t)ts.tv_nsec / 1000u;
#endif
}

extern "C" int dod3_setenv(const char* name, const char* value, int overwrite)
{
#ifdef _WIN32
    /* the CRT's _putenv_s: getenv, and so the runtime, sees it */
    if (!overwrite && getenv(name)) return 0;
    return _putenv_s(name, value) ? -1 : 0;
#else
    return setenv(name, value, overwrite);
#endif
}

extern "C" int dod3_unsetenv(const char* name)
{
#ifdef _WIN32
    return _putenv_s(name, "") ? -1 : 0;   /* an empty value removes it */
#else
    return unsetenv(name);
#endif
}

FILE* dod3::open_file(const std::filesystem::path& p, const char* mode)
{
#ifdef _WIN32
    wchar_t m[8] = {};
    for (int i = 0; i < 7 && mode[i]; i++) m[i] = (wchar_t)mode[i];
    return _wfopen(p.c_str(), m);
#else
    return fopen(p.c_str(), mode);
#endif
}

std::string dod3::error_text(const std::error_code& ec)
{
#ifdef _WIN32
    /* a system error: Windows' own text for it, in the display language, as UTF-8 */
    if (ec.category() == std::system_category()) {
        wchar_t* w = nullptr;
        DWORD n =
            FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
                           nullptr, (DWORD)ec.value(), 0, (LPWSTR)&w, 0, nullptr);
        while (n && (w[n - 1] == L'\r' || w[n - 1] == L'\n' || w[n - 1] == L' ')) n--;
        std::string s;
        if (n) {
            s.resize((size_t)WideCharToMultiByte(CP_UTF8, 0, w, (int)n, nullptr, 0, nullptr, nullptr));
            WideCharToMultiByte(CP_UTF8, 0, w, (int)n, s.data(), (int)s.size(), nullptr, nullptr);
        }
        if (w) LocalFree(w);
        if (!s.empty()) return s;
    }
#endif
    return ec.message();
}
