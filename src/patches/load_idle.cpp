/*
 * Loading in the frame limiter's sleep.
 *
 * A chapter's packages come in through Unreal's async loader, which
 * UObject::StaticTick runs once a frame for 5 ms at most -- and for less: it
 * gives the frame up whenever the read it needs next has not come back,
 * though here that takes a fraction of a millisecond. The rest of the frame
 * the game thread sleeps in the limiter. So a loading screen lasted a number
 * of frames, not an amount of work: 265 for the first chapter's, which is
 * 4.5 s at 60 fps, 2.9 s at a 120 cap and 9 s at the console's 30. (A larger
 * time limit in StaticTick changed nothing: it is the giving up that paces
 * it, and this build's loader has no "use the full time" argument.)
 *
 * While no scene is drawn -- a loading screen, a menu, a movie: a few dozen
 * draws a frame, where a scene is hundreds -- and packages are pending, the
 * limiter's sleep goes to the loader instead: ProcessAsyncLoading again, for
 * what is left of the sleep, as often as it gives up. It runs on the game
 * thread at the top of FEngineLoop::Tick, where nothing is ticking (as the
 * deferred purge of gc.cpp does), with the guest registers saved around it,
 * and the sleep is shortened by what it took: the frame keeps its time.
 * Without a frame cap, or in a frame over its budget, there is no sleep and
 * nothing changes.
 *
 * DOD3_LOAD_IDLE=0 turns it off; =2 does it with a scene on screen as well
 * (levels streaming in during play; not the default, being untried beyond
 * the first chapter). DOD3_LOAD_IDLE_LOG=1 says what each second of it did.
 *
 * DOD3_LOAD_LOG=1: a timeline of the loads, from a thread of its own that
 * looks every 2 ms: when packages became pending and when the last one was
 * done, the frames in between and the longest of them, and every frame over
 * 100 ms whether a package is pending or not (a blocking load: the map
 * itself, LoadMap on the game thread). With DOD3_PROF on macOS each load is a
 * report window of its own.
 */
#include "cpu/ppu.h"
#include "cpu/eboot.h"   /* the EBOOT version's addresses */
#include "util.h"
#include <stdio.h>
#include <stdlib.h>
#include <thread>
#include <chrono>
#ifdef __APPLE__
#include <pthread.h>
#include <sys/sysctl.h>
#include <sys/time.h>
#include <unistd.h>
#elif defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#elif defined(__linux__)
#include <pthread.h>
#endif

extern "C" uint32_t g_rsx_engine_frame;            /* rsx_draw_engine.c: the present count */
extern "C" volatile unsigned g_rsx_frame_draws;    /* and the draws of the last frame presented */
#ifdef __APPLE__
/* src/os/macos/prof_mac.cpp */
extern "C" void dod3_prof_mark(const char* why);
extern "C" void dod3_prof_slow_frame(uint64_t start_us, uint64_t end_us, double frame_ms);
#elif defined(_WIN32)
/* src/os/win32/prof.cpp */
extern "C" void win_prof_slow_frame_async(uint64_t start_us, uint64_t end_us, double frame_ms);
static void dod3_prof_mark(const char*) {}
static void dod3_prof_slow_frame(uint64_t start_us, uint64_t end_us, double frame_ms)
{
    win_prof_slow_frame_async(start_us, end_us, frame_ms);
}
#else
static void dod3_prof_mark(const char*) {}
static void dod3_prof_slow_frame(uint64_t, uint64_t, double) {}
#endif

extern "C" void dod3_archive_native_counts(uint64_t* native, uint64_t* lifted);   /* archive_native.cpp */

/* What the limiter's loader did during the current load (DOD3_LOAD_LOG). */
static volatile uint64_t s_limiter_us, s_limiter_calls;

void dod3_load_in_limiter(ppu_context* ctx, uint64_t* usec)
{
#if defined(DOD3_FN_PROCESS_ASYNC_LOADING) && defined(DOD3_A_GOBJ_ASYNC_PACKAGES)
    static int mode = -1, log = 0;
    if (mode < 0) {
        const char* e = getenv("DOD3_LOAD_IDLE");
        mode = e && *e ? atoi(e) : 1;
        log = getenv("DOD3_LOAD_IDLE_LOG") ? 1 : 0;
    }
    if (mode <= 0) return;
    /* Three frames in a row without a scene start it; one frame of a scene
     * ends it. */
    static uint32_t last_frame = 0xFFFFFFFFu, quiet;
    const uint32_t frame = g_rsx_engine_frame;
    if (frame != last_frame) {
        last_frame = frame;
        quiet = g_rsx_frame_draws < 300 ? quiet + 1 : 0;
    }
    if (mode == 1 && quiet < 3) return;
    if (!vm_read32(DOD3_A_GOBJ_ASYNC_PACKAGES + 4)) return;
    /* A millisecond stays a sleep: the limiter's own margin, and the loader
     * ends a step after its limit, not at it. */
    if (*usec < 2000) return;
    const uint64_t t0 = dod3_now_us();
    const uint64_t deadline = t0 + (*usec > 50000 ? 50000 : *usec) - 1000;

    const ppu_context saved = *ctx;
    unsigned calls = 0;
    for (uint64_t now = t0; now + 300 < deadline && vm_read32(DOD3_A_GOBJ_ASYNC_PACKAGES + 4);) {
        ctx->gpr[1] = (saved.gpr[1] - 0x400) & ~0xFull;
        ctx->gpr[3] = 1;                         /* bUseTimeLimit */
        ctx->gpr[4] = 0;
        ctx->gpr[5] = 0;                         /* ExcludeType: NAME_None */
        ctx->fpr[1] = (double)(float)((double)(deadline - now) / 1e6);
        DOD3_FN_PROCESS_ASYNC_LOADING(ctx);
        dod3_drain(ctx);
        *ctx = saved;
        calls++;
        /* It gave up for a read: a moment for that to come back. */
        const uint64_t until = dod3_now_us() + 100;
        do { std::this_thread::yield(); now = dod3_now_us(); } while (now < until);
    }
    const uint64_t spent = dod3_now_us() - t0;
    *usec = spent >= *usec ? 0 : *usec - spent;
    s_limiter_us += spent;
    s_limiter_calls += calls;

    if (log) {
        static uint64_t since, sum;
        static unsigned n, ncalls;
        n++; sum += spent; ncalls += calls;
        if (!since) since = t0;
        if (t0 - since >= 1000000) {
            fprintf(stderr, "[load] in the limiter's sleep: %u frames a second, %.1f ms and %.1f calls a frame\n",
                    n, (double)sum / 1000.0 / n, (double)ncalls / n);
            since = t0; sum = 0; n = ncalls = 0;
        }
    }
#else
    (void)ctx; (void)usec;
#endif
}

/* Seconds since the process started (elsewhere than macOS and Windows: since
 * the first call). */
static double since_process_start()
{
#ifdef _WIN32
    FILETIME created, exited, kernel, user, now;
    if (!GetProcessTimes(GetCurrentProcess(), &created, &exited, &kernel, &user)) return 0.0;
    GetSystemTimeAsFileTime(&now);
    const uint64_t c = (uint64_t)created.dwHighDateTime << 32 | created.dwLowDateTime;
    const uint64_t n = (uint64_t)now.dwHighDateTime << 32 | now.dwLowDateTime;
    return n > c ? (n - c) / 1e7 : 0.0;   /* 100 ns units */
#elif defined(__APPLE__)
    static double start = -1.0;
    if (start < 0.0) {
        struct kinfo_proc kp;
        size_t len = sizeof kp;
        int mib[4] = { CTL_KERN, KERN_PROC, KERN_PROC_PID, getpid() };
        if (sysctl(mib, 4, &kp, &len, NULL, 0) == 0)
            start = kp.kp_proc.p_starttime.tv_sec + kp.kp_proc.p_starttime.tv_usec / 1e6;
        else
            start = 0.0;
    }
    struct timeval now;
    gettimeofday(&now, NULL);
    return start > 0.0 ? now.tv_sec + now.tv_usec / 1e6 - start : 0.0;
#else
    static const uint64_t t0 = dod3_now_us();
    return (dod3_now_us() - t0) / 1e6;
#endif
}

/* DOD3_LOAD_LOG: a step of the start-up, with the time since the process
 * started. Asked each time: the first steps come before dod3.ini is read. */
extern "C" void dod3_boot_mark(const char* what)
{
    if (getenv("DOD3_LOAD_LOG"))
        fprintf(stderr, "[load] %.3f s after the process started: %s\n", since_process_start(), what);
}

#if defined(DOD3_A_GOBJ_ASYNC_PACKAGES)
static void load_log_thread()
{
#ifdef __APPLE__
    pthread_setname_np("dod3 load log");
#elif defined(__linux__)
    pthread_setname_np(pthread_self(), "dod3 load log");
#endif
    const uint64_t t0 = dod3_now_us();
    dod3_boot_mark("t=0 of the [load] lines");
    uint64_t since = 0, frame_at = t0, longest = 0, lim0 = 0;
    uint32_t last_frame = g_rsx_engine_frame, f0 = 0, most = 0;
    for (;;) {
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
        const uint64_t now = dod3_now_us();
        const uint32_t frame = g_rsx_engine_frame;
        if (frame != last_frame) {
            if (!last_frame) dod3_boot_mark("the first frame");
            const uint64_t gap = now - frame_at;
            if (gap > 100000 && last_frame) {
                fprintf(stderr, "[load] t=%.2f frame %u came %.0f ms after the one before%s\n", (now - t0) / 1e6,
                        frame, gap / 1000.0, since ? " (packages pending)" : "");
                dod3_prof_slow_frame(frame_at, now, gap / 1000.0);
            }
            if (since && gap > longest) longest = gap;
            last_frame = frame;
            frame_at = now;
        }
        const uint32_t n = vm_read32(DOD3_A_GOBJ_ASYNC_PACKAGES + 4);
        if (n && !since) {
            since = now;
            f0 = frame;
            most = n;
            longest = 0;
            lim0 = s_limiter_us;
            s_limiter_calls = 0;
            uint64_t an, al;
            dod3_archive_native_counts(&an, &al);
            fprintf(stderr, "[load] t=%.2f frame %u: %u packages pending (draws last frame: %u)\n", (now - t0) / 1e6,
                    frame, n, g_rsx_frame_draws);
            dod3_prof_mark("a load's start");
        } else if (n > most)
            most = n;
        if (!n && since) {
            const double secs = (now - since) / 1e6;
            uint64_t an, al;
            dod3_archive_native_counts(&an, &al);
            fprintf(stderr,
                    "[load] t=%.2f frame %u: done, %.2f s and %u frames (%.1f fps; longest %.0f ms), at most %u packages "
                    "at once; the limiter's loader %.0f ms in %llu calls; archive reads %llu native, %llu lifted\n",
                    (now - t0) / 1e6, frame, secs, frame - f0, secs > 0 ? (frame - f0) / secs : 0.0, longest / 1000.0,
                    most, (s_limiter_us - lim0) / 1000.0, (unsigned long long)s_limiter_calls,
                    (unsigned long long)an, (unsigned long long)al);
            dod3_prof_mark("a load's end");
            since = 0;
        }
    }
}
#endif

extern "C" void dod3_load_log_start(void)
{
#if defined(DOD3_A_GOBJ_ASYNC_PACKAGES)
    if (!getenv("DOD3_LOAD_LOG")) return;
    std::thread(load_log_thread).detach();
#endif
}
