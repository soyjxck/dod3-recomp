/*
 * A cheap cycle counter for the timing hooks (src/dod3_gc_native.cpp,
 * src/dod3_hot.cpp): the TSC on x86-64, the steady clock in ns elsewhere
 * (Apple silicon) -- the reports call both "cycles"; compare within a
 * platform only.
 */
#pragma once
#include <stdint.h>
#if defined(_M_X64) || defined(__x86_64__)
#if defined(_MSC_VER)
#include <intrin.h>
#else
#include <x86intrin.h>
#endif
static inline uint64_t dod3_cycles(void) { return __rdtsc(); }
#else
#include <chrono>
static inline uint64_t dod3_cycles(void)
{
    return (uint64_t)std::chrono::duration_cast<std::chrono::nanoseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}
#endif
