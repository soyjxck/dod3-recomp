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
#include "dod3_eboot.h"   /* the EBOOT version's addresses */
#include <chrono>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stddef.h>
#include <vector>
#include <algorithm>
#ifdef _WIN32
#include <windows.h>
extern "C" void win_prof_slow_frame_async(uint64_t start_us, uint64_t end_us, double frame_ms);
#endif

extern "C" PPU_THREAD_LOCAL void (*g_trampoline_fn)(void*);
extern "C" uint8_t* vm_base;
extern "C" uint32_t g_rsx_engine_frame;   /* rsx_draw_engine.c: the present count */

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
    const uint32_t nobj = be32(DOD3_A_GOBJOBJECTS + 4), objs = be32(DOD3_A_GOBJOBJECTS);
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
    DOD3_FN_GC_REACH_LIFTED(ctx); drain(ctx);
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

void DOD3_FN_GC_REACH(ppu_context* ctx)
{
    const double t0 = now_ms();
    switch (gc_native()) {
    case 1:  dod3_gc_reach_native(ctx); break;
    case 2:  reach_check(ctx); break;
    default: DOD3_FN_GC_REACH_LIFTED(ctx); drain(ctx); break;
    }
    s_reach_ms += now_ms() - t0;
    s_reach_calls++;
}

/* ---- the periodic collection, moved into the frame limiter's sleep ---------
 *
 * UWorld::Tick (func_0041FC84) purges pending-kill objects every
 * TimeBetweenPurgingPendingKillObjects (GEngine+0x4AC; ~10 s here) through
 * func_0041C270: CollectGarbage, then the levels' actor lists compacted, then
 * TimeSinceLastPendingKillPurge (world+0x1A0) zeroed. The frame limiter
 * (appUpdateTimeAndHandleMaxTickRate, func_008EDC6C) sleeps *before* the
 * tick, so the collection's 3-5 ms land on top of a frame that had already
 * waited out its budget: that frame reaches the screen late (16.7 + 4 ms, a
 * third vblank at 120 Hz) and the next one early -- a visible judder every
 * 10 s at 60 fps.
 *
 * DOD3_GC_DEFER=1 (default): that call's collection is skipped where it
 * stands, and the whole of func_0041C270 runs again at the start of the next
 * frame, inside the limiter's sleep (its usleep returns to 0x008EDF10), on
 * the game thread, with the guest registers saved around it; the sleep is
 * shortened by what it took. Nothing is ticking there (the top of
 * FEngineLoop::Tick), the world is checked to be the same (GWorld), and any
 * other collection in between (a level load) supersedes it. The first,
 * collection-less pass compacts the actor lists and zeroes the purge timer
 * as before, so the world tick does not ask again meanwhile. If no limiter
 * sleep comes (a frame over budget), the next periodic request collects at
 * once. */
static const uint32_t PERIODIC_GC_RET = DOD3_A_PERIODIC_GC_RET;   /* 1.00: func_0041C270's call at 0x0041C2C8 */
static const uint32_t LIMITER_SLEEP_RET = DOD3_A_LIMITER_SLEEP_RET; /* appSleep in the limiter */
static const uint32_t GWORLD = DOD3_A_GWORLD;           /* UGameEngine::Tick's world (func_009053D0) */
static uint32_t s_def_world;     /* a deferred purge's world, 0 if none */
static uint32_t s_def_frame;
static int s_in_deferred;
static int s_def_dropped;        /* the last one was not run: the next request collects at once */
static uint32_t s_limiter_frame = 0xFFFFFFFFu;   /* the frame of the last limiter sleep */

extern "C" void (*g_lv2_usleep_pre)(ppu_context* ctx, uint64_t* usec);

static int gc_defer(void)
{
    static int on = -1;
    if (on < 0) { const char* e = getenv("DOD3_GC_DEFER"); on = e ? atoi(e) : 1; }
    return on;
}

static void gc_usleep_pre(ppu_context* ctx, uint64_t* usec)
{
    if ((uint32_t)ctx->lr != LIMITER_SLEEP_RET) return;
    s_limiter_frame = g_rsx_engine_frame;
    if (!s_def_world) return;
    const uint32_t world = s_def_world;
    s_def_world = 0;
    if (be32(GWORLD) != world) {
        s_def_dropped = 1;
        if (gc_log()) fprintf(stderr, "[gc] deferred purge dropped: the world changed (%08X -> %08X)\n", world, be32(GWORLD));
        return;
    }
    const double t0 = now_ms();
    const ppu_context saved = *ctx;
    ctx->gpr[1] = (ctx->gpr[1] - 0x400) & ~0xFull;
    ctx->gpr[3] = world;
    ctx->lr = LIMITER_SLEEP_RET;
    s_in_deferred = 1;
    DOD3_FN_PERIODIC_GC(ctx);
    drain(ctx);
    s_in_deferred = 0;
    *ctx = saved;
    const double ms = now_ms() - t0;
    const uint64_t spent = (uint64_t)(ms * 1000.0);
    if (gc_log())
        fprintf(stderr, "[gc] deferred purge from frame %u ran at frame %u: %.2f ms of a %.2f ms limiter sleep\n",
                s_def_frame, g_rsx_engine_frame, ms, *usec / 1000.0);
    *usec = spent >= *usec ? 0 : *usec - spent;
}

void DOD3_FN_COLLECT_GARBAGE(ppu_context* ctx)
{
    const uint32_t caller = (uint32_t)ctx->lr;
    if (gc_defer() && !s_in_deferred) {
        g_lv2_usleep_pre = gc_usleep_pre;                /* from the first collection (boot) on */
        /* Only while the limiter is sleeping (a frame cap, frames within it):
         * uncapped, or over budget, there is no sleep to put it in. */
        const bool limiting = g_rsx_engine_frame - s_limiter_frame <= 2u;
        if (caller == PERIODIC_GC_RET && limiting && !s_def_world && !s_def_dropped) {
            s_def_world = (uint32_t)ctx->gpr[27];        /* func_0041C270's r27: the world */
            s_def_frame = g_rsx_engine_frame;
            return;
        }
        s_def_world = 0;                                 /* this one collects instead */
        s_def_dropped = 0;
    }
    s_reach_ms = 0; s_reach_calls = 0;
    const uint64_t u0 = clock_us();
    const double t0 = now_ms();
    DOD3_FN_COLLECT_GARBAGE_LIFTED(ctx);
    drain(ctx);
    const double ms = now_ms() - t0;
    if (gc_log() && ms > 1.0) {
        static unsigned n = 0;
        /* GObjObjects: a TArray (data, num, max) at 0x01A0C2B4. */
        const uint32_t nobj = __builtin_bswap32(*(const uint32_t*)(vm_base + DOD3_A_GOBJOBJECTS + 4));

        fprintf(stderr, "[gc] #%u collection %.2f ms, reachability %.2f ms (%u passes), %u objects, %.0f ns each, frame %u, from %08X\n",
                ++n, ms, s_reach_ms, s_reach_calls, nobj, nobj ? s_reach_ms * 1e6 / nobj : 0.0, g_rsx_engine_frame, caller);
#ifdef _WIN32
        /* DOD3_GC_LOG=2 with DOD3_PROF / DOD3_PROF_TREE: the sampled stacks
         * of the collection, as [slow-frame] lines. */
        if (gc_log() >= 2) win_prof_slow_frame_async(u0, clock_us(), ms);
#endif
    }
}
