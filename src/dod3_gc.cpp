/*
 * The title's garbage collector, wrapped natively (lifted with
 * --hook 0x000C1E50 --hook 0x00EE6538; README, "Lift").
 *
 *   func_000C1E50  UObject::CollectGarbage: entered nearly every frame,
 *                  does real work about every 15 s.
 *   func_00EE6538  the reachability pass under it: 18-20 ms of game thread
 *                  per heavy collection, a dropped frame at 60 fps.
 *
 * The reachability pass runs natively (src/dod3_gc_native.cpp, a statement
 * for statement translation of the lifted one; DOD3_GC_NATIVE below).
 * DOD3_GC_LOG=1 prints every collection that takes over 1 ms, with the
 * reachability pass's share; =2 adds the profiler's sampled stacks.
 */
#include "ppu_recomp.h"
#include <chrono>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stddef.h>
#include <vector>
#include <algorithm>
#ifdef _WIN32
#include <windows.h>
extern "C" void win_prof_slow_frame(uint64_t start_us, uint64_t end_us, double frame_ms);
#endif

extern "C" PPU_THREAD_LOCAL void (*g_trampoline_fn)(void*);
extern "C" uint8_t* vm_base;

/* Finish any tail call the lifted body left in the trampoline, so the time
 * measured is the whole function (the caller would drain it next anyway). */
static void drain(ppu_context* ctx)
{
    while (g_trampoline_fn) {
        void (*f)(void*) = g_trampoline_fn;
        g_trampoline_fn = 0;
        f((void*)ctx);
    }
}

static double now_ms(void)
{
    using namespace std::chrono;
    return duration<double, std::milli>(steady_clock::now().time_since_epoch()).count();
}

/* The clock of the profiler's sample ring (frame_clock_us in main.cpp). */
static uint64_t clock_us(void)
{
#ifdef _WIN32
    static LARGE_INTEGER f; LARGE_INTEGER c;
    if (!f.QuadPart) QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&c);
    return (uint64_t)(c.QuadPart / f.QuadPart) * 1000000ull +
           (uint64_t)(c.QuadPart % f.QuadPart) * 1000000ull / (uint64_t)f.QuadPart;
#else
    return 0;
#endif
}

static int gc_log(void)
{
    static int on = -1;
    if (on < 0) { const char* e = getenv("DOD3_GC_LOG"); on = e ? atoi(e) : 0; }
    return on;
}

static double s_reach_ms;       /* reachability time inside the current collection */
static unsigned s_reach_calls;

extern "C" void dod3_gc_reach_native(ppu_context* ctx);   /* src/dod3_gc_native.cpp */
extern "C" int  dod3_gc_par_threads(void);                /* src/dod3_gc_native.cpp: DOD3_GC_PAR */

/* DOD3_GC_NATIVE: 1 (default) the native pass, 0 the lifted one, "check"
 * both on the same heap (lifted first, then native over the same input)
 * with the results compared: the queued objects and every object's
 * unreachable bit. 28 collections over two runs matched exactly. */
static int gc_native(void)
{
    static int mode = -1;
    if (mode < 0) {
        const char* e = getenv("DOD3_GC_NATIVE");
        mode = !e ? 1 : !strcmp(e, "check") ? 2 : atoi(e) ? 1 : 0;
    }
    return mode;
}

static uint32_t be32(uint32_t a) { return __builtin_bswap32(*(const uint32_t*)(vm_base + a)); }
static uint64_t be64(uint32_t a) { return __builtin_bswap64(*(const uint64_t*)(vm_base + a)); }

struct ReachResult {
    std::vector<uint32_t> queued;
    std::vector<uint8_t> unreachable;
};
static void reach_snapshot(uint32_t list, ReachResult* r)
{
    const uint32_t n = be32(list + 4), data = be32(list + 0);
    r->queued.resize(n);
    for (uint32_t i = 0; i < n; i++) r->queued[i] = be32(data + i * 4);
    const uint32_t nobj = be32(0x01A0C2B8u), objs = be32(0x01A0C2B4u);
    r->unreachable.assign(nobj, 0);
    for (uint32_t i = 0; i < nobj; i++) {
        const uint32_t o = be32(objs + i * 4);
        r->unreachable[i] = o ? (uint8_t)((be64(o + 8) >> 33) & 1) : 2;
    }
}

static void reach_check(ppu_context* ctx)
{
    const uint32_t list = (uint32_t)ctx->gpr[3];
    const uint32_t num0 = be32(list + 4), cur0 = be32(list + 0xC);
    ppu_context saved = *ctx;
    func_00EE6538_lifted(ctx); drain(ctx);
    ReachResult a; reach_snapshot(list, &a);
    const ppu_context after_lifted = *ctx;
    /* Same input for the native pass: the list as it came in. */
    *(uint32_t*)(vm_base + list + 4) = __builtin_bswap32(num0);
    *(uint32_t*)(vm_base + list + 0xC) = __builtin_bswap32(cur0);
    *ctx = saved;
    dod3_gc_reach_native(ctx);
    ReachResult b; reach_snapshot(list, &b);
    /* DOD3_GC_PAR: the parallel phase 2 queues the same objects in another
     * order; compare them as sets. */
    if (dod3_gc_par_threads() > 0) {
        std::sort(a.queued.begin(), a.queued.end());
        std::sort(b.queued.begin(), b.queued.end());
    }
    size_t qdiff = 0, udiff = 0, first_q = (size_t)-1, first_u = (size_t)-1;
    const size_t nq = a.queued.size() < b.queued.size() ? a.queued.size() : b.queued.size();
    for (size_t i = 0; i < nq; i++) if (a.queued[i] != b.queued[i]) { if (!qdiff) first_q = i; qdiff++; }
    const size_t nu = a.unreachable.size() < b.unreachable.size() ? a.unreachable.size() : b.unreachable.size();
    for (size_t i = 0; i < nu; i++) if (a.unreachable[i] != b.unreachable[i]) { if (!udiff) first_u = i; udiff++; }
    int regs = 0;
    for (int i = 1; i < 32; i++) if (i != 3 && i >= 14 && after_lifted.gpr[i] != ctx->gpr[i]) regs++;
    fprintf(stderr, "[gc-check] queued %zu lifted / %zu native, %zu differ (first at %zd); unreachable bits differ for %zu of %zu (first at %zd); r1 %s, non-volatile regs differing %d\n",
            a.queued.size(), b.queued.size(), qdiff, (ptrdiff_t)first_q, udiff, nu, (ptrdiff_t)first_u,
            after_lifted.gpr[1] == ctx->gpr[1] ? "same" : "DIFFERENT", regs);
}

void func_00EE6538(ppu_context* ctx)
{
    const double t0 = now_ms();
    switch (gc_native()) {
    case 1:  dod3_gc_reach_native(ctx); break;
    case 2:  reach_check(ctx); break;
    default: func_00EE6538_lifted(ctx); drain(ctx); break;
    }
    s_reach_ms += now_ms() - t0;
    s_reach_calls++;
}

void func_000C1E50(ppu_context* ctx)
{
    s_reach_ms = 0; s_reach_calls = 0;
    const uint64_t u0 = clock_us();
    const double t0 = now_ms();
    func_000C1E50_lifted(ctx);
    drain(ctx);
    const double ms = now_ms() - t0;
    if (gc_log() && ms > 1.0) {
        static unsigned n = 0;
        /* GObjObjects: a TArray (data, num, max) at 0x01A0C2B4. */
        const uint32_t nobj = __builtin_bswap32(*(const uint32_t*)(vm_base + 0x01A0C2B8u));
        fprintf(stderr, "[gc] #%u collection %.2f ms, reachability %.2f ms (%u passes), %u objects, %.0f ns each\n",
                ++n, ms, s_reach_ms, s_reach_calls, nobj, nobj ? s_reach_ms * 1e6 / nobj : 0.0);
#ifdef _WIN32
        /* DOD3_GC_LOG=2 with DOD3_PROF / DOD3_PROF_TREE: the sampled stacks
         * of the collection, as [slow-frame] lines. */
        if (gc_log() >= 2) win_prof_slow_frame(u0, clock_us(), ms);
#endif
    }
}
