/*
 * Small helpers shared by the port's sources, C and C++:
 *   dod3_be16/32/64, dod3_put_be32   big-endian access to a byte buffer
 *   dod3_now_us                      a monotonic microsecond clock (the frame
 *                                    clock's, and the profiler's)
 *   dod3_setenv, dod3_unsetenv       POSIX setenv/unsetenv, also on Windows
 * and, from C++:
 *   dod3::utf8(path)                 a path as UTF-8
 *   dod3::open_file(path, mode)      fopen that takes any path on Windows too
 *                                    (path::string() throws there for names
 *                                    outside the ANSI code page)
 */
#pragma once
#include <stdint.h>
#include <stdio.h>

static inline uint16_t dod3_be16(const uint8_t* p) { return (uint16_t)(p[0] << 8 | p[1]); }
static inline uint32_t dod3_be32(const uint8_t* p)
{
    return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
}
static inline uint64_t dod3_be64(const uint8_t* p) { return (uint64_t)dod3_be32(p) << 32 | dod3_be32(p + 4); }
static inline void dod3_put_be32(uint8_t* p, uint32_t v)
{
    p[0] = (uint8_t)(v >> 24);
    p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);
    p[3] = (uint8_t)v;
}

#ifdef __cplusplus
extern "C" {
#endif
uint64_t dod3_now_us(void);
int dod3_setenv(const char* name, const char* value, int overwrite);
int dod3_unsetenv(const char* name);
#ifdef __cplusplus
}

#include <filesystem>
#include <string>

namespace dod3 {

inline std::string utf8(const std::filesystem::path& p)
{
    const auto s = p.u8string();
    return std::string(s.begin(), s.end());
}

FILE* open_file(const std::filesystem::path& p, const char* mode);

}  // namespace dod3
#endif
