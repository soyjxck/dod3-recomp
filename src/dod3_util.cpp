/* Small helpers shared by the port's sources: see dod3_util.h. */
#include "dod3_util.h"

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
