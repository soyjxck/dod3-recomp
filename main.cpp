/*
 * ps3recomp game project -- entry point template.
 *
 * This is the runner. It boots a lifted title through the toolkit's PPU
 * scaffold: allocate the flat guest VM, load the PPU ELF named on the command
 * line, register the lifted function table and the HLE NID handlers, start a
 * frame clock, and dispatch the entry OPD.
 *
 * The scaffold does the work; nothing below reimplements it. ppu_load_elf,
 * ppu_recomp_register, ppu_hle_init, ppu_sysprx_register, ppu_fs_register,
 * lv2_init_syscalls and ppu_run all come from the toolkit, compiled into this
 * project from PS3RECOMP_DIR (see CMakeLists.txt). What is left here is the
 * part a port owns: which backend to present through, how the frame clock is
 * paced, and whatever diagnostics the title turns out to need.
 *
 * runtime/ppu/tests/boot_main.cpp in the toolkit is the same boot in its
 * fully-instrumented form -- crash filter, hang watchdog, guest-PC sampling
 * profiler. Read it when a boot goes wrong; copy from it what the title
 * needs. It is deliberately not what a fresh project starts with.
 *
 * The one part that IS duplicated from it is the frame clock below, because a
 * port is expected to change its pacing and a shared one would be the wrong
 * shape for that. It was copied faithfully, comments and all. If a pacing bug
 * is fixed in one of the two, look at the other.
 *
 * This builds on Windows, macOS and Linux. CI builds and runs it against the
 * boot smoke title on the last two, and compile-checks it against clang-cl on
 * the first (tools/check_ppu_scaffold.py).
 */

/* The lifter's generated header, which declares ppu_context and the function
 * table. It comes first: everything below is written against that struct. */
#ifdef _WIN32
#include <intrin.h>
#include <immintrin.h>
#endif
#include <math.h>
#include "ppu_recomp.h"

/* The Win32 names -- Sleep, GetTickCount64, CreateThread, InterlockedIncrement,
 * VirtualAlloc, the scalar typedefs -- from one place. On Windows this is a
 * passthrough to <windows.h>; off Windows the toolkit's shim supplies the same
 * names over pthreads and mmap, so the call sites below are written once. */
#include "win32_compat.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#ifdef _WIN32
/* timeBeginPeriod: <windows.h> arrives with WIN32_LEAN_AND_MEAN set, which
 * excludes the multimedia timer API, so it is asked for by name -- and after
 * windows.h, since timeapi.h uses UINT. No shim exists off Windows because
 * there is nothing to shim: the 15.6 ms default timer granularity this works
 * around is a Windows problem. */
#include <timeapi.h>
#include <direct.h>
/* The defaults main() sets are written with POSIX setenv/mkdir; the CRT has
 * them as _putenv_s/_mkdir. getenv sees _putenv_s, so the runtime does too. */
static int setenv(const char* k, const char* v, int overwrite)
{
    if (!overwrite && getenv(k)) return 0;
    return _putenv_s(k, v) ? -1 : 0;
}
#define mkdir(path, mode) _mkdir(path)
/* src/win_prof.cpp: DOD3_PROF sampling profiler and DOD3_STALL_MS stack dumps. */
extern "C" void win_prof_start(void);
#endif

/* ---------------------------------------------------------------------------
 * What the scaffold gives this file
 * -----------------------------------------------------------------------*/
extern "C" {
uint32_t ppu_load_elf(const char* path);      /* ELF -> guest RAM, returns entry OPD */
void     ppu_recomp_register(void);           /* generated: lifted table -> address map */
void     ppu_hle_init(void);                  /* firmware import NID -> HLE handler */
void     ppu_sysprx_register(void);           /* boot-critical CRT (sys_initialize_tls, ...) */
void     ppu_fs_register(void);               /* cellFs over the real game directory */
void     lv2_init_syscalls(void);             /* the lv2 syscall table */
int      ppu_run(uint32_t entry_opd, uint32_t stack_top);

/* This project's own: load whatever real system PRX modules the title needs
 * into guest RAM and register their exports. stubs.cpp has the empty version
 * a game with no lifted PRX wants. */
void     ps3_load_prx_modules(void);

extern const char* ppu_vfs_root;              /* host dir the PS3 mount points map into */

/* The status the guest handed to sys_process_exit. A title that exits that way
 * never returns through ppu_run, so this is only read on the path where the
 * entry function unwound instead. */
extern int     g_sys_process_exit_called;
extern int32_t g_sys_process_exit_code;

/* Host-provided symbols the runtime and the HLE libraries link against. */
uint8_t* vm_base = nullptr;
extern uint32_t ppu_vm_size;                  /* ppu_loader.cpp: the OOB guard */

/* Guest-callback dispatch. g_ps3_guest_caller is the hook the HLE runtime
 * calls back into recompiled code through -- cellSysutil events and the GCM
 * vblank/flip handlers. ppu_guest_call does the OPD -> dispatch. */
typedef void (*ps3_guest_caller_fn)(uint32_t, uint64_t, uint64_t, uint64_t, uint64_t,
                                    uint64_t, uint64_t, uint64_t, uint64_t);
extern ps3_guest_caller_fn g_ps3_guest_caller;
uint64_t ppu_guest_call(uint32_t, uint64_t, uint64_t, uint64_t, uint64_t,
                        uint64_t, uint64_t, uint64_t, uint64_t);

/* The GCM half of the frame clock. */
void     cellGcmTickVBlank(void);
void     cellGcmTickFlip(void);
int      cellGcm_take_flip_pending(void);
void     cellGcm_rsx_process_fifo(void);      /* drain get -> put */
unsigned cellGcm_flip_request_count(void);
}

/* ---------------------------------------------------------------------------
 * RSX present backend
 * -----------------------------------------------------------------------*/
/* D3D12 on Windows, Metal on Apple, and the null backend's headless software
 * path anywhere else -- the same selection runtime/host/host_posix.c and the
 * toolkit's boot harness make. All three expose the same three entry points,
 * so the frame clock below is backend-agnostic. */
#if defined(_WIN32)
extern "C" int  rsx_d3d12_backend_init(uint32_t w, uint32_t h, const char* title);
extern "C" void rsx_d3d12_backend_present(void);
extern "C" int  rsx_d3d12_backend_pump_messages(void);
#  define rsx_backend_init    rsx_d3d12_backend_init
#  define rsx_backend_present rsx_d3d12_backend_present
#  define rsx_backend_pump    rsx_d3d12_backend_pump_messages
#  define RSX_BACKEND_NAME    "D3D12"
#elif defined(__APPLE__)
extern "C" int  rsx_metal_backend_init(uint32_t w, uint32_t h, const char* title);
extern "C" void rsx_metal_backend_present(void);
extern "C" int  rsx_metal_backend_pump_messages(void);
#  define rsx_backend_init    rsx_metal_backend_init
#  define rsx_backend_present rsx_metal_backend_present
#  define rsx_backend_pump    rsx_metal_backend_pump_messages
#  define RSX_BACKEND_NAME    "Metal"
#else
extern "C" int  rsx_null_backend_init(uint32_t w, uint32_t h, const char* title);
extern "C" void rsx_null_backend_present(void);
extern "C" int  rsx_null_backend_pump_messages(void);
#  define rsx_backend_init    rsx_null_backend_init
#  define rsx_backend_present rsx_null_backend_present
#  define rsx_backend_pump    rsx_null_backend_pump_messages
#  define RSX_BACKEND_NAME    "null (headless software)"
#endif

/* ---------------------------------------------------------------------------
 * Configuration
 * -----------------------------------------------------------------------*/
/* The flat VM must span every region the PS3 memory map uses, not just the
 * game image: sys_ppu_thread_create puts thread stacks at 0xD0000000, so a
 * smaller arena makes every spawned thread's stack access out of bounds. */
#define VM_SIZE       0x100010000ull   /* the full 32-bit guest space + a 64K guard */
#define STACK_TOP     0x0FF00000u      /* main-thread stack, below the 0x10000000 segment */
#define WINDOW_WIDTH  1280
#define WINDOW_HEIGHT 720

/* ---------------------------------------------------------------------------
 * Frame clock
 * -----------------------------------------------------------------------*/
/* On real hardware the RSX raises a vblank interrupt about 60 times a second
 * and that is what drives the game's frame loop. With no RSX it is synthesized
 * here: a host thread calls cellGcmTickVBlank/TickFlip, which invoke the
 * handlers the guest registered, and drains the GCM FIFO. Without this the
 * title initialises, registers its handlers and then waits forever.
 *
 * The ticks are driven off real elapsed time rather than off how long present()
 * took. A hidden or occluded window makes present block hard, and pacing the
 * ticks behind it paces the whole game behind it. */
extern "C" uint32_t ppu_hle_inject_base;   /* cellGcmSys.c: the GCM window's home */
static volatile LONG g_frames_presented = 0;

/* Frames handed to the backend at a guest FLIP boundary -- one per frame the
 * guest actually finished. The presents made before the guest's first flip, so
 * a fresh window is not left blank through a long boot, carry no guest frame
 * and are deliberately not counted. */
extern "C" unsigned ppu_boot_frames_presented(void)
{
    return (unsigned)g_frames_presented;
}

static void present_guest_frame(void)
{
    rsx_backend_present();
    /* This thread increments and guest threads read, so interlocked rather
     * than a volatile ++, which on arm64 is neither atomic nor a fence. */
    InterlockedIncrement(&g_frames_presented);
}

extern "C" unsigned cellGcm_user_queue_depth(void);

/* The FIFO walker sleeps between drains. A guest thread polling with usleep
 * is usually waiting on something only a drain writes -- the render thread's
 * GPU fence wait (func_008B0C70) re-reads a label every 200 us -- so every
 * guest usleep wakes the walker (sys_timer.c, g_lv2_usleep_hook). It cost the
 * render thread up to the whole 4 ms sleep, several times a frame.
 * DOD3_FIFO_KICK=0 goes back to the plain sleep. */
#include <atomic>
#include <condition_variable>
#include <mutex>
static std::mutex              s_kick_mu;
static std::condition_variable s_kick_cv;
static std::atomic<bool>       s_kicked{false};
extern "C" int (*g_lv2_usleep_hook)(uint32_t lr, uint64_t usec);
static void fifo_kick(void)
{
    if (!s_kicked.exchange(true, std::memory_order_acq_rel)) {
        std::lock_guard<std::mutex> lk(s_kick_mu);
        s_kick_cv.notify_one();
    }
}

/* The render thread's GPU fence wait (func_008B0C70) polls a label with
 * usleep(200), about 20 times a frame. A drain usually writes the label well
 * inside that, but the thread slept its whole 200 us (258 with timer slack)
 * every time -- some 5 ms of a frame on the busiest thread. That one poll
 * instead sleeps until the next drain completes, never longer than it asked.
 * DOD3_FAST_POLL_LR=<hex guest return address>, 0 for none. */
static std::atomic<uint64_t> s_drain_gen{0};
static std::atomic<int>      s_drain_waiters{0};
static std::mutex              s_drain_mu;
static std::condition_variable s_drain_cv;
static uint32_t s_fast_poll_lr = 0x008B0DD8u;
static void drain_done(void)
{
    s_drain_gen.fetch_add(1);
    if (s_drain_waiters.load()) {
        std::lock_guard<std::mutex> lk(s_drain_mu);
        s_drain_cv.notify_all();
    }
}
static std::atomic<uint64_t> s_fp_calls{0}, s_fp_early{0}, s_fp_us{0};
static int guest_usleep_hook(uint32_t lr, uint64_t usec)
{
    fifo_kick();
    if (!s_fast_poll_lr || lr != s_fast_poll_lr || usec > 100000) return 0;
    const uint64_t g = s_drain_gen.load();
    const auto t0 = std::chrono::steady_clock::now();
    s_drain_waiters.fetch_add(1);
    bool early;
    {
        std::unique_lock<std::mutex> lk(s_drain_mu);
        early = s_drain_cv.wait_for(lk, std::chrono::microseconds(usec),
                                    [g] { return s_drain_gen.load() != g; });
    }
    s_drain_waiters.fetch_sub(1);
    s_fp_calls++; if (early) s_fp_early++;
    s_fp_us += (uint64_t)std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now() - t0).count();
    return 1;
}
extern "C" int  ppu_waitprof_on(void);
extern "C" void ppu_waitprof_report(double window_s);

/* A monotonic microsecond clock: the vblank period is not a whole number of
 * milliseconds once it is raised above 60 Hz. */
static uint64_t frame_clock_us(void)
{
#ifdef _WIN32
    static LARGE_INTEGER f; LARGE_INTEGER c;
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

/* DOD3_FPS=<n>: raise the title's 30 fps cap to n. It has two limiters: a
 * minimum frame time of (the float at 0x008EDC5C) / 30 s, 1.0 as shipped, and
 * its vblank handler, which flips on every second vblank. The first is set to
 * 0.125 (Whatcookie's RPCS3 "Unlock FPS" patch for BLUS31197 1.00) and the
 * vblank runs at 2n Hz. Its physics hold up to 120 fps; above that jumps get
 * lower and dragon lock-on fails. 0 when the cap stays. */
/* Pacing. With the cap left to the vblank count (the original scheme:
 * minimum frame time 0.125/30 s, vblank at 2n Hz, a flip every second
 * vblank) frame times are quantised to 1/(2n) s: at n = 60 a frame that
 * takes 17 ms instead of 16.6 waits for the next vblank and takes 25, so the
 * title flapped between 60 and 40 fps scene by scene. Now the cap is the
 * title's own minimum frame time, set to exactly 1/n s (the float is
 * seconds * 30), and the vblank runs at DOD3_VBLANK_MULT * n Hz (default 8:
 * 480 Hz at n = 60), so a late frame is late by at most ~2 ms.
 * DOD3_PACE=vblank restores the old scheme. */
static unsigned s_fps_target;
static unsigned s_vblank_mult = 2;
static void apply_fps_unlock(void)
{
    const char* e = getenv("DOD3_FPS");
    const int fps = e ? atoi(e) : 30;
    if (fps <= 30) return;
    const uint32_t addr = 0x008EDC5Cu;
    const uint32_t word = vm_read32(addr);
    if (word != 0x3F800000u) {           /* 1.0f: anything else is another build */
        fprintf(stderr, "[fps] DOD3_FPS=%d ignored: 0x%08X holds 0x%08X, not 1.0 -- not BLUS31197 1.00?\n",
                fps, addr, word);
        return;
    }
    const char* pace = getenv("DOD3_PACE");
    if (pace && !strcmp(pace, "vblank")) {
        vm_write32(addr, 0x3E000000u);   /* 0.125f: the vblank count is the cap */
        s_vblank_mult = 2;
    } else {
        const float min_frame = 30.0f / (float)fps;   /* seconds * 30 */
        uint32_t bits; memcpy(&bits, &min_frame, 4);
        vm_write32(addr, bits);
        s_vblank_mult = 8;
        if (const char* m = getenv("DOD3_VBLANK_MULT")) if (atoi(m) >= 2) s_vblank_mult = (unsigned)atoi(m);
    }
    s_fps_target = (unsigned)fps;
    fprintf(stderr, "[fps] frame cap raised to %d (%s, vblank %u Hz)\n", fps,
            s_vblank_mult == 2 && pace ? "capped by vblank count" : "capped by minimum frame time",
            s_vblank_mult * (unsigned)fps);
}

#ifndef _WIN32
#include <spawn.h>
#include <unistd.h>
extern char** environ;
extern "C" uint32_t g_rsx_engine_frame;
/* DOD3_STALL_SAMPLE=<dir>: a watchdog for hitches. When no frame has been
 * presented for 300 ms it runs `sample` on this process for a second, so
 * the stacks of every thread are taken while the stall is happening, and it
 * logs how long each stall lasted. At most 12 samples a run, 2 s apart. */
static void* stall_watch(void* arg)
{
    const char* dir = (const char*)arg;
    /* DOD3_STALL_MS / DOD3_STALL_MAX: the threshold (default 300) and how many
     * samples a run may take (default 12). */
    const uint64_t thr_us = (getenv("DOD3_STALL_MS") ? (uint64_t)atoi(getenv("DOD3_STALL_MS")) : 300u) * 1000u;
    const int max_n = getenv("DOD3_STALL_MAX") ? atoi(getenv("DOD3_STALL_MAX")) : 12;
    uint32_t last = g_rsx_engine_frame;
    uint64_t last_t = frame_clock_us(), last_sample = 0, stall_from = 0;
    int n = 0;
    for (;;) {
        usleep(20000);
        const uint32_t f = g_rsx_engine_frame;
        const uint64_t t = frame_clock_us();
        if (f != last) {
            if (stall_from)
                fprintf(stderr, "[stall] frame %u came after %.0f ms\n", f, (t - last_t) / 1000.0);
            last = f; last_t = t; stall_from = 0;
            continue;
        }
        if (!f || t - last_t < thr_us) continue;
        if (!stall_from) stall_from = t;
        if (n < max_n && t - last_sample > 2000000 && stall_from == t) {
            char path[512], pid[16];
            snprintf(path, sizeof path, "%s/stall_%02d_f%u.txt", dir, n, f);
            snprintf(pid, sizeof pid, "%d", (int)getpid());
            char* const argv[] = { (char*)"/usr/bin/sample", pid, (char*)"1", (char*)"-file", path, NULL };
            pid_t child;
            if (posix_spawn(&child, "/usr/bin/sample", NULL, NULL, argv, environ) == 0) {
                fprintf(stderr, "[stall] no present for %llu ms after frame %u -> %s\n",
                        (unsigned long long)(thr_us / 1000), f, path);
                n++; last_sample = t;
            }
        }
    }
    return NULL;
}
#endif

#ifdef __APPLE__
#include <mach/mach.h>
#include <map>
#include <string>
#include <vector>
#include <algorithm>
/* DOD3_STUTTER_MS=<n> also starts this: at every present it reads each host
 * thread's CPU time, and for a frame slower than n ms prints the threads that
 * used the CPU during it ([stutter-cpu]). The guest-side [stutter] report
 * sees syscalls; this sees the host -- which thread actually burned the
 * frame, the RSX walker (this process's main thread) included. */
static void* cpu_watch(void*)
{
    pthread_setname_np("stutter cpu watch");
    const uint64_t thr_us = (uint64_t)atoi(getenv("DOD3_STUTTER_MS")) * 1000u;
    struct Snap { std::string name; uint64_t cpu_us; };
    std::map<uint64_t, Snap> prev;
    uint32_t last = 0;
    uint64_t last_t = 0;
    for (;;) {
        usleep(1000);
        const uint32_t f = g_rsx_engine_frame;
        if (f == last) continue;
        const uint64_t t = frame_clock_us();
        std::map<uint64_t, Snap> cur;
        thread_act_array_t th; mach_msg_type_number_t n = 0;
        if (task_threads(mach_task_self(), &th, &n) == KERN_SUCCESS) {
            for (mach_msg_type_number_t i = 0; i < n; i++) {
                thread_identifier_info_data_t idi; mach_msg_type_number_t c1 = THREAD_IDENTIFIER_INFO_COUNT;
                thread_extended_info_data_t ext;  mach_msg_type_number_t c2 = THREAD_EXTENDED_INFO_COUNT;
                if (thread_info(th[i], THREAD_IDENTIFIER_INFO, (thread_info_t)&idi, &c1) == KERN_SUCCESS &&
                    thread_info(th[i], THREAD_EXTENDED_INFO, (thread_info_t)&ext, &c2) == KERN_SUCCESS)
                    cur[idi.thread_id] = { ext.pth_name[0] ? ext.pth_name : "?",
                                           (ext.pth_user_time + ext.pth_system_time) / 1000u };
                mach_port_deallocate(mach_task_self(), th[i]);
            }
            vm_deallocate(mach_task_self(), (vm_address_t)th, n * sizeof(thread_act_t));
        }
        if (last && t - last_t >= thr_us) {
            std::vector<std::pair<uint64_t, std::string>> used;
            for (auto& [id, sn] : cur) {
                auto it = prev.find(id);
                const uint64_t d = sn.cpu_us - (it != prev.end() ? it->second.cpu_us : 0);
                if (d >= 3000) used.push_back({ d, sn.name });
            }
            std::sort(used.rbegin(), used.rend());
            std::string line;
            char buf[96];
            for (size_t i = 0; i < used.size() && i < 10; i++) {
                snprintf(buf, sizeof buf, "%s%s %.0f", i ? ", " : "", used[i].second.c_str(), used[i].first / 1000.0);
                line += buf;
            }
            fprintf(stderr, "[stutter-cpu] frames %u-%u took %.0f ms; CPU ms by host thread: %s\n",
                    last, f, (t - last_t) / 1000.0, line.c_str());
        }
        prev.swap(cur);
        last = f; last_t = t;
    }
    return NULL;
}
#endif

/* DOD3_AB=<switch>[,<seconds>]: an A/B test inside one run. Two runs of the
 * same battle differ by 3 fps for no reason at all (the scene is not
 * deterministic), which hides any change worth less than that; so this flips
 * one run-time switch every <seconds> (default 4), counts the flips the title
 * requested in each window, and reports the paired difference between
 * neighbouring windows, where the scene is the same. Windows before
 * DOD3_AB_FROM seconds (default 70: boot and menus) are not counted.
 *   stores   the lifted code's inline store path on / every store a call
 *   icall    ps3_indirect_call's fast path on / the full path always
 *   none     nothing: the control, which has to report no difference
 * On Windows each report also gives the CPU time per frame of the busiest
 * threads in either state (thread cycle counters, so exact): a switch that
 * changes the frame rate without changing anyone's work per frame changed
 * how long somebody waits. */
extern "C" int  g_ppu_vm_slow_stores;
extern "C" void ppu_vm_slow_any_update(void);
static void ab_stores(int on) { g_ppu_vm_slow_stores = on ? 0 : 1; ppu_vm_slow_any_update(); }
extern "C" int g_ppu_icall_full;
static void ab_icall(int on) { g_ppu_icall_full = on ? 0 : 1; }
static void ab_none(int) {}
static const struct { const char* name; void (*set)(int on); } s_ab_switches[] = {
    { "stores", ab_stores },
    { "icall",  ab_icall },
    { "none",   ab_none },
};
#ifdef _WIN32
#include <tlhelp32.h>
#include <intrin.h>
#include <map>
#include <string>
#include <vector>
#include <algorithm>
struct AbThread { uint64_t last = 0; bool seen = false; double cyc[2] = { 0, 0 }; std::string name; };
static std::map<DWORD, AbThread> s_ab_threads;
static double s_ab_frames[2];
/* Charge each thread's cycles since the last call to `state`. */
static void ab_cpu_sample(int state, unsigned frames, bool count)
{
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snap == INVALID_HANDLE_VALUE) return;
    const DWORD pid = GetCurrentProcessId();
    THREADENTRY32 te; te.dwSize = sizeof te;
    for (BOOL ok = Thread32First(snap, &te); ok; ok = Thread32Next(snap, &te)) {
        if (te.th32OwnerProcessID != pid) continue;
        HANDLE h = OpenThread(THREAD_QUERY_LIMITED_INFORMATION, FALSE, te.th32ThreadID);
        if (!h) continue;
        ULONG64 c = 0;
        if (QueryThreadCycleTime(h, &c)) {
            AbThread& t = s_ab_threads[te.th32ThreadID];
            if (t.seen && count) t.cyc[state] += (double)(c - t.last);
            t.last = c; t.seen = true;
            if (t.name.empty()) {
                PWSTR w = NULL;
                if (SUCCEEDED(GetThreadDescription(h, &w)) && w) {
                    for (PWSTR q = w; *q; q++) t.name += (char)(*q < 128 ? *q : '?');
                    LocalFree(w);
                }
                if (t.name.empty()) t.name = "tid " + std::to_string(te.th32ThreadID);
            }
        }
        CloseHandle(h);
    }
    CloseHandle(snap);
    if (count) s_ab_frames[state] += frames;
}
static void ab_cpu_report(const char* name)
{
    static uint64_t tsc0 = 0; static LARGE_INTEGER q0, qf;
    if (!tsc0) { tsc0 = __rdtsc(); QueryPerformanceCounter(&q0); QueryPerformanceFrequency(&qf); return; }
    LARGE_INTEGER q; QueryPerformanceCounter(&q);
    const double hz = (double)(__rdtsc() - tsc0) * (double)qf.QuadPart / (double)(q.QuadPart - q0.QuadPart);
    if (s_ab_frames[0] < 1 || s_ab_frames[1] < 1 || hz < 1e8) return;
    std::vector<const AbThread*> v;
    for (auto& kv : s_ab_threads) v.push_back(&kv.second);
    std::sort(v.begin(), v.end(), [](const AbThread* a, const AbThread* b) {
        return a->cyc[0] + a->cyc[1] > b->cyc[0] + b->cyc[1]; });
    std::string line;
    char buf[160];
    for (size_t i = 0; i < v.size() && i < 7; i++) {
        snprintf(buf, sizeof buf, "%s%s %.2f / %.2f", i ? "; " : "", v[i]->name.c_str(),
                 v[i]->cyc[1] / hz * 1e3 / s_ab_frames[1], v[i]->cyc[0] / hz * 1e3 / s_ab_frames[0]);
        line += buf;
    }
    fprintf(stderr, "[ab] %s: CPU ms per frame, on / off: %s\n", name, line.c_str());
}
#else
static void ab_cpu_sample(int, unsigned, bool) {}
static void ab_cpu_report(const char*) {}
#endif
static void ab_step(uint64_t now_us)
{
    static int which = -2, state = 0, pairs = 0;
    static uint64_t win_us = 4000000, from_us = 70000000, t0 = 0, start = 0;
    static unsigned f0 = 0;
    static double fps_prev = 0, sum_d = 0, sum_d2 = 0, sum_on = 0, sum_off = 0;
    if (which == -2) {
        which = -1;
        if (const char* e = getenv("DOD3_AB")) {
            for (size_t i = 0; i < sizeof s_ab_switches / sizeof s_ab_switches[0]; i++) {
                const size_t n = strlen(s_ab_switches[i].name);
                if (!strncmp(e, s_ab_switches[i].name, n) && (e[n] == 0 || e[n] == ',')) {
                    which = (int)i;
                    if (e[n] == ',' && atof(e + n + 1) > 0) win_us = (uint64_t)(atof(e + n + 1) * 1e6);
                }
            }
            if (which < 0) fprintf(stderr, "[ab] DOD3_AB=%s: no such switch\n", e);
        }
        if (const char* e = getenv("DOD3_AB_FROM")) from_us = (uint64_t)atoi(e) * 1000000ull;
        if (which >= 0) { s_ab_switches[which].set(state); start = t0 = now_us; f0 = cellGcm_flip_request_count();
                          ab_cpu_report(""); }
    }
    if (which < 0 || now_us - t0 < win_us) return;
    const unsigned f = cellGcm_flip_request_count();
    const double fps = (double)(f - f0) * 1e6 / (double)(now_us - t0);
    ab_cpu_sample(state, f - f0, t0 - start >= from_us);
    /* A pair is an off window and the on window after it. */
    if (state == 1 && fps_prev > 0 && t0 - start >= from_us + win_us) {
        const double d = fps - fps_prev;
        pairs++; sum_d += d; sum_d2 += d * d; sum_on += fps; sum_off += fps_prev;
        const double mean = sum_d / pairs;
        const double var = pairs > 1 ? (sum_d2 - pairs * mean * mean) / (pairs - 1) : 0;
        fprintf(stderr, "[ab] %s: on %.1f fps, off %.1f fps, on-off %+.2f +/- %.2f over %d pairs\n",
                s_ab_switches[which].name, sum_on / pairs, sum_off / pairs, mean,
                pairs > 1 ? sqrt(var / pairs) : 0.0, pairs);
        if (pairs % 4 == 0) ab_cpu_report(s_ab_switches[which].name);
    }
    fps_prev = fps;
    state = !state;
    s_ab_switches[which].set(state);
    t0 = now_us; f0 = f;
}

static DWORD WINAPI frame_clock(LPVOID)
{
#ifdef __APPLE__
    pthread_setname_np("rsx walker (main)");
#endif
    const char* title = getenv("PS3_TITLE");
    if (!title || !*title) title = "ps3recomp";

    int rsx_ok = (rsx_backend_init(WINDOW_WIDTH, WINDOW_HEIGHT, title) == 0);
    fprintf(stderr, "[rsx] %s backend init %s\n", RSX_BACKEND_NAME,
            rsx_ok ? "OK -- window open" : "FAILED");

    unsigned  last_flip = 0;
    /* The vblank period: 16 ms (62.5 Hz) as it has always been, 1/(2n) s for
     * DOD3_FPS=n, or DOD3_VBLANK_HZ=<hz> outright. The title flips on every
     * second vblank. DOD3_FIFO_SLEEP_MS=<n>: the walker's sleep between drains. */
    uint64_t vblank_us = 16000;
    if (s_fps_target) vblank_us = 1000000ull / ((uint64_t)s_vblank_mult * s_fps_target);
    if (const char* e = getenv("DOD3_VBLANK_HZ")) if (atoi(e) > 0) vblank_us = 1000000ull / (uint64_t)atoi(e);
    DWORD fifo_sleep_ms = 4;
    if (const char* e = getenv("DOD3_FIFO_SLEEP_MS")) fifo_sleep_ms = (DWORD)atoi(e);
    const bool kick_on = !(getenv("DOD3_FIFO_KICK") && getenv("DOD3_FIFO_KICK")[0] == '0');
    if (const char* e = getenv("DOD3_FAST_POLL_LR")) s_fast_poll_lr = (uint32_t)strtoul(e, 0, 16);
    if (kick_on) g_lv2_usleep_hook = guest_usleep_hook;
    uint64_t next_tick = frame_clock_us();
    uint64_t last_pump = 0, last_boot_present = 0;

    for (;;) {
        if (kick_on) {
            /* Until a guest poll kicks it, the next vblank, or the usual sleep
             * -- whichever comes first. */
            const uint64_t t = frame_clock_us();
            uint64_t wait_us = (uint64_t)fifo_sleep_ms * 1000ull;
            if ((long long)(next_tick - t) < (long long)wait_us)
                wait_us = (long long)(next_tick - t) > 0 ? next_tick - t : 0;
            std::unique_lock<std::mutex> lk(s_kick_mu);
            s_kick_cv.wait_for(lk, std::chrono::microseconds(wait_us),
                               [] { return s_kicked.load(std::memory_order_acquire); });
            s_kicked.store(false, std::memory_order_release);
        } else {
            Sleep(fifo_sleep_ms);
        }
        /* DOD3_GCM_WATCH=1 (temporary): every 5 s, the FIFO pointers, the sync
         * label, and the command words at `get` -- parked with work pending, or
         * drained dry? */
        { static int on = -1; if (on < 0) on = getenv("DOD3_GCM_WATCH") ? 1 : 0;
          static ULONGLONG last = 0; ULONGLONG t = GetTickCount64();
          if (on && t - last >= 5000) { last = t;
              uint32_t put = vm_read32(ppu_hle_inject_base + 0x2000u), get = vm_read32(ppu_hle_inject_base + 0x2004u), ref = vm_read32(ppu_hle_inject_base + 0x2008u);
              uint32_t ea = 0x40000000u + get;
              fprintf(stderr, "[gcm-watch] put=0x%08X get=0x%08X ref=0x%08X label=0x%08X cache64=0x%016llX words@get: %08X %08X %08X %08X flips=%u\n",
                      put, get, ref, vm_read32(ppu_hle_inject_base + 0x0FF0u), (unsigned long long)vm_read64(0x01A2A1D0u),
                      vm_read32(ea), vm_read32(ea + 4), vm_read32(ea + 8),
                      vm_read32(ea + 12), cellGcm_flip_request_count());
              fprintf(stderr, "[gcm-watch] user commands pending delivery: %u\n", cellGcm_user_queue_depth());
              fprintf(stderr, "[gcm-watch] malloc lwmutex 0x40400010: owner=%u waiter=%u attr=0x%X recur=%u\n",
                      vm_read32(0x40400010u), vm_read32(0x40400014u), vm_read32(0x40400018u), vm_read32(0x4040001Cu));
              uint32_t gctx = vm_read32(0x01AC3E38u);   /* CellGcmContextData* the title got */
              uint32_t cur = vm_read32(gctx + 8);
              fprintf(stderr, "[gcm-watch] ctx=0x%08X begin=0x%08X end=0x%08X current=0x%08X (io 0x%08X) cb=0x%08X words@current-16: %08X %08X %08X %08X\n",
                      gctx, vm_read32(gctx), vm_read32(gctx + 4), cur, cur - 0x40000000u, vm_read32(gctx + 12),
                      vm_read32(cur - 16), vm_read32(cur - 12), vm_read32(cur - 8), vm_read32(cur - 4));
              static int dumped = 0; static unsigned last_flips = 0; static int same = 0;
              unsigned fl = cellGcm_flip_request_count();
              same = (fl == last_flips) ? same + 1 : 0; last_flips = fl;
              if (!dumped && same >= 4) { dumped = 1;   /* no flip for 20 s: stalled */
                  /* The ShaderPatching job chain as the title left it (entry 0x01A2A880):
                   * did it write jobs the walker never ran? */
                  /* Which job descriptors (256 B each, from 0x01A2BA00) name a FIFO
                   * address -- the notify target each job clears on completion. */
                  for (uint32_t j = 0; j < 40; j++) {
                      uint32_t d = 0x01A2BA00u + j * 0x100u;
                      for (uint32_t o = 0; o < 0x100; o += 4) {
                          uint32_t v = vm_read32(d + o);
                          if (v >= 0x40000000u && v < 0x40300000u)
                              fprintf(stderr, "[jc-desc] job@%08X +0x%02X = %08X (io 0x%06X)\n", d, o, v, v - 0x40000000u);
                      }
                  }
                  for (uint32_t a = 0x01A2A880u; a < 0x01A2A880u + 48 * 8; a += 32)
                      fprintf(stderr, "[jc-dump] %08X: %016llX %016llX %016llX %016llX\n", a,
                              (unsigned long long)vm_read64(a), (unsigned long long)vm_read64(a + 8),
                              (unsigned long long)vm_read64(a + 16), (unsigned long long)vm_read64(a + 24));
                  for (uint32_t a = cur - 0x8000; a < cur; a += 16)
                      fprintf(stderr, "[gcm-tail] io %08X: %08X %08X %08X %08X\n", a - 0x40000000u,
                              vm_read32(a), vm_read32(a + 4), vm_read32(a + 8), vm_read32(a + 12)); } } }
        uint64_t now = frame_clock_us();
        /* PPU_WAITPROF=1: where the guest threads waited, every 5 s. */
        { static uint64_t wp_last = 0;
          if (!wp_last) wp_last = now;
          if (now - wp_last >= 5000000ull && ppu_waitprof_on()) {
              ppu_waitprof_report((double)(now - wp_last) / 1e6);
              const uint64_t c = s_fp_calls.exchange(0), e = s_fp_early.exchange(0), u = s_fp_us.exchange(0);
              fprintf(stderr, "[fast-poll] %llu fence polls, %llu woken by a drain, %.1f us avg; drains %llu\n",
                      (unsigned long long)c, (unsigned long long)e, c ? (double)u / (double)c : 0.0,
                      (unsigned long long)s_drain_gen.load());
              wp_last = now;
          } }

        ab_step(now);

        int fired = 0;
        while ((long long)(now - next_tick) >= 0 && fired < 240) {
            cellGcmTickVBlank();
            cellGcmTickFlip();
            /* Present a pending flip BEFORE draining any further. The flip
             * fires at a get==put frame boundary on the guest thread, so the
             * batch held right now is exactly the completed frame; presenting
             * after the drain races the guest's next-frame writes and shows a
             * mixed one. */
            if (rsx_ok && cellGcm_take_flip_pending()) {
                present_guest_frame();
                last_flip = cellGcm_flip_request_count();
            }
            /* Drain the FIFO every tick. This is what writes the RSX sync-fence
             * labels the game's per-frame logic blocks on, so it has to keep
             * advancing at 60 Hz even while present() throttles. */
            if (rsx_ok) { cellGcm_rsx_process_fifo(); drain_done(); }
            next_tick += vblank_us;
            fired++;
        }
        if (fired >= 240) next_tick = now;   /* fell too far behind -- resync */

        /* Drain at the outer cadence too, not only on the 16 ms tick: titles
         * fence every render pass on a label the drain writes, and one wave of
         * those at 16 ms apiece paces the guest into single-figure frame rates.
         * The real RSX writes them in microseconds. */
        if (rsx_ok) {
            /* DOD3_SLOW_STEP=1: report any frame-clock step over 300 ms. This
             * thread is the FIFO walker; a step that blocks it stalls the
             * title's command-buffer callback, which waits for the walker. */
            static int slow = -1; if (slow < 0) slow = getenv("DOD3_SLOW_STEP") ? 1 : 0;
            ULONGLONG t0 = GetTickCount64();
            if (cellGcm_take_flip_pending()) {
                present_guest_frame();
                last_flip = cellGcm_flip_request_count();
            }
            ULONGLONG t1 = GetTickCount64();
            cellGcm_rsx_process_fifo();
            drain_done();
            ULONGLONG t2 = GetTickCount64();
            if (slow && (t1 - t0 > 300 || t2 - t1 > 300))
                fprintf(stderr, "[slow-step] present %llu ms, fifo %llu ms\n", (unsigned long long)(t1 - t0), (unsigned long long)(t2 - t1));

            /* Window events need no more than a few hundred polls a second,
             * however often a kick wakes this loop. */
            const uint64_t pump_now = frame_clock_us();
            if (pump_now - last_pump >= 2000) {
                last_pump = pump_now;
                ULONGLONG t3 = GetTickCount64();
                int pumped = rsx_backend_pump();
                if (slow && GetTickCount64() - t3 > 300)
                    fprintf(stderr, "[slow-step] window pump %llu ms\n", (unsigned long long)(GetTickCount64() - t3));
                if (pumped != 0) {
                    /* Window closed: end the process. Stopping only the
                     * rendering left the guest running headless -- a
                     * dod3.exe nobody could see, holding the GPU and the
                     * audio device until Task Manager found it. The guest
                     * threads cannot be joined (they run lifted code with no
                     * exit path), so this is a hard exit after the logs are
                     * flushed. */
                    fprintf(stderr, "[rsx] window closed -- exiting\n");
                    fflush(stdout); fflush(stderr);
#ifdef _WIN32
                    TerminateProcess(GetCurrentProcess(), 0);
#else
                    _exit(0);
#endif
                    rsx_ok = 0;
                    continue;
                }
            }
            /* Present on a guest flip. A present on a fixed clock can catch the
             * drain mid-frame and flash a partial one. Before the first flip
             * present freely, so the window is not blank during boot. */
            unsigned fc = cellGcm_flip_request_count();
            if (fc != last_flip) {
                present_guest_frame();
                last_flip = fc;
            } else if (fc == 0 && pump_now - last_boot_present >= 16000) {
                last_boot_present = pump_now;
                rsx_backend_present();
            }
        }
    }
    return 0;
}

/* ---------------------------------------------------------------------------
 * VFS root
 * -----------------------------------------------------------------------*/
/* Derive the directory holding PS3_GAME from the EBOOT path
 * <root>/PS3_GAME/USRDIR/EBOOT.elf -> <root>. $PS3_VFS_ROOT overrides. */
static char s_vfs_root[1024];

static void derive_vfs_root(const char* eboot)
{
    const char* env = getenv("PS3_VFS_ROOT");
    if (env && *env) { ppu_vfs_root = env; return; }

    strncpy(s_vfs_root, eboot, sizeof s_vfs_root - 1);
    for (char* p = s_vfs_root; *p; p++) if (*p == '\\') *p = '/';
    /* strip EBOOT.elf, USRDIR and PS3_GAME */
    for (int i = 0; i < 3; i++) { char* s = strrchr(s_vfs_root, '/'); if (s) *s = 0; }
    if (!s_vfs_root[0]) strcpy(s_vfs_root, ".");
    ppu_vfs_root = s_vfs_root;
}

/* ---------------------------------------------------------------------------
 * Guest VM
 * -----------------------------------------------------------------------*/
#ifdef _WIN32
/* Demand-paging for the flat VM: reserve the whole 4 GB guest space, which
 * costs no commit, and commit each 64 KB page on first access. Every 32-bit
 * guest offset is then valid, so a garbage guest pointer reads zero instead of
 * killing the process. Faults outside the arena fall through untouched. */
static LONG WINAPI vm_commit_veh(EXCEPTION_POINTERS* ep)
{
    if (ep->ExceptionRecord->ExceptionCode == EXCEPTION_ACCESS_VIOLATION) {
        ULONG_PTR fault = ep->ExceptionRecord->ExceptionInformation[1];
        uintptr_t base  = (uintptr_t)vm_base;
        if (vm_base && fault >= base && fault < base + VM_SIZE) {
            void* page = (void*)(fault & ~(uintptr_t)0xFFFF);
            if (VirtualAlloc(page, 0x10000, MEM_COMMIT, PAGE_READWRITE))
                return EXCEPTION_CONTINUE_EXECUTION;
        }
    }
    return EXCEPTION_CONTINUE_SEARCH;
}
#endif

static bool alloc_guest_vm(void)
{
#ifdef _WIN32
    AddVectoredExceptionHandler(1, vm_commit_veh);
    vm_base = (uint8_t*)VirtualAlloc(NULL, VM_SIZE, MEM_RESERVE, PAGE_READWRITE);
    ppu_vm_size = 0;              /* the whole space is backed; no OOB guard needed */
#else
    /* No vectored exception handlers off Windows, so the arena is committed up
     * front and lazily backed by the OS: only pages the title touches cost
     * anything. It stops below the 0xE0000000 mark, so ppu_vm_size arms the
     * loader's bounds check for what is left. */
    vm_base = (uint8_t*)calloc(1, 0xE0000000u);
    ppu_vm_size = 0xE0000000u;
#endif
    return vm_base != nullptr;
}

/* ---------------------------------------------------------------------------
 * Main
 * -----------------------------------------------------------------------*/
static void harness_guest_caller(uint32_t opd, uint64_t a0, uint64_t a1,
                                 uint64_t a2, uint64_t a3, uint64_t a4,
                                 uint64_t a5, uint64_t a6, uint64_t a7)
{
    ppu_guest_call(opd, a0, a1, a2, a3, a4, a5, a6, a7);
}

#ifdef __APPLE__
#include <pthread.h>
#include <sys/stat.h>

static uint32_t s_boot_entry;

/* The guest's half of the Apple split in main(): run the entry OPD, then end
 * the process the way the non-Apple path's return from main() would. */
static void* guest_main(void*)
{
    pthread_setname_np("main");
    int rc = ppu_run(s_boot_entry, STACK_TOP);
    printf("\n[boot] ppu_run returned %d (entry function unwound)\n", rc);
    fflush(stdout);
    exit(g_sys_process_exit_called ? (int)g_sys_process_exit_code : 0);
}
#endif

#if defined(DOD3_X86_V3) && defined(_WIN32)
/* The lifted code is built for x86-64-v3 (CMake DOD3_X86_LEVEL): main says so
 * at start, where the alternative is an illegal-instruction crash somewhere
 * in the title's start-up. */
__attribute__((target("xsave"))) static bool x86_v3_cpu(void)
{
    int r1[4], r7[4];
    __cpuidex(r1, 1, 0); __cpuidex(r7, 7, 0);
    const bool os_avx = (r1[2] & (1 << 27)) && ((_xgetbv(0) & 6) == 6);
    return os_avx && (r1[2] & (1 << 12)) /* FMA */ && (r1[2] & (1 << 22)) /* MOVBE */ &&
           (r7[1] & (1 << 5)) /* AVX2 */ && (r7[1] & (1 << 3)) && (r7[1] & (1 << 8)) /* BMI1, BMI2 */;
}
#endif

int main(int argc, char** argv)
{
#if defined(DOD3_X86_V3) && defined(_WIN32)
    if (!x86_v3_cpu()) {
        fprintf(stderr, "This build needs a CPU with AVX2, FMA and BMI2 (x86-64-v3). "
                        "Rebuild with -DDOD3_X86_LEVEL= for older processors.\n");
        return 1;
    }
#endif

    if (argc < 2) {
        printf("usage: %s <PPU ELF>\n", argv[0]);
        return 2;
    }

#ifdef _WIN32
#pragma comment(lib, "winmm.lib")
    /* 1 ms timer resolution. The default granularity is about 15.6 ms, which
     * inflates every shorter wait the title makes and throttles the whole
     * thing. POSIX timers are already fine-grained. */
    timeBeginPeriod(1);
    setvbuf(stdout, NULL, _IONBF, 0);   /* unbuffered: do not lose prints on a kill */
#endif

    printf("=== ps3recomp game runner ===\n");

    /* Drakengard 3's RHI appends commands behind a JUMP-to-self park and patches
     * the park shortly after. The toolkit's FIFO resyncs skip past such a park
     * and drop the back-end label releases the render thread waits on, so keep
     * the FIFO waiting instead. An explicit GCM_FIFO_NO_RESYNC=0 overrides. */
    setenv("GCM_FIFO_NO_RESYNC", "1", 0);
    /* The memory-manager SPU tasks send on SPU port 1, the port lv2 would hand
     * a dynamic attach; the toolkit hands out 0x10 upward unless told not to. */
    setenv("SPURS_DYNPORT_LOW", "1", 0);
    /* Loading the title peaks around 514 MB, just past the default overflow
     * window's end at 0x80000000; give it room. */
    setenv("SYS_MEM_OVERFLOW_END", "88000000", 0);
    /* The ShaderPatching job chain must run inside RunJobChain: the render
     * thread refills its 21 job descriptors every frame, and a job that runs
     * late reads the refilled one and never clears the FIFO park it was for. */
    setenv("SPURS_JC_SYNC", "1", 0);
    /* The HLE posts a job-chain completion event to every lv2 queue attached
     * to SPURS. This title's only attached queue belongs to CellMemoryManager,
     * which reads each one as a request and allocates: 400 MB in five seconds. */
    setenv("SPURS_JC_DONE_EVENTS", "0", 0);
    /* The toolkit's per-event log is on whenever stderr is redirected: two
     * lines and two flushes per SPURS job, a line per event-queue wait. At
     * this title's ~2700 jobs/s that is a measurable share of the render
     * thread, and the threads contend for the FILE lock. Quiet unless asked
     * (PS3_VERBOSE=1); the port's own milestone lines are not gated by it. */
    setenv("PS3_VERBOSE", "0", 0);
    /* Firmware files the title loads at run time. MultiStream fetches its MP3
     * decoder from /dev_flash/sys/external/flashMP3.pic when the first MP3
     * stream starts; without it the audio SPU task dies and the game hangs on
     * the next sound (opening movie, new game). tools/extract_dev_flash.py
     * unpacks fw/dev_flash from the PS3UPDAT.PUP on the game disc. */
    setenv("PS3_DEV_FLASH", "fw/dev_flash", 0);
    /* The PhysX taskset (five memory-manager/physics tasks sharing request
     * blocks). Run concurrently on host threads they race: a task reads a
     * request record before it is filled, dispatches type 0 to a null
     * handler and dies mid-chapter. One at a time removes that. */
    setenv("SPURS_TASKSET_SERIAL", "01AA7700", 0);
    /* The runtime treats SPU image 22 as You Don't Know Jack's cri media task
     * (context from the CreateTask globals, an EXIT that returns to the task).
     * Here image 22 is a PhysX task; it never returned and the serialised
     * taskset stalled behind it a few seconds into the first level. */
    setenv("SPU_CRI_IMAGE", "-1", 0);
    /* Drakengard 3's libgcm keeps NV0039 (memory-to-memory copy) on
     * subchannel 1: cellGcmSetTransferReportData is how its occlusion
     * queries come back. The runtime's default routes subchannel 1 to the 3D
     * engine (Twisted Metal binds NV4097 there). */
    setenv("GCM_SUBCH1_2D", "1", 0);
    /* The runtime's GCM window (labels, reports, the put/get/ref control
     * block and the IO offset tables) defaults to 0x20000000, and Drakengard 3
     * maps 10 MB of its own memory there (cellGcmMapMainMemory(0x20000000,
     * 0xA00000)): its occlusion-query results are copied to the start of it,
     * straight over the labels and the control block -- put and get turned to
     * 0xFFFF and misaligned values, the FIFO walker lost its place, and the
     * picture went black. 0x8F000000 is past the sys_memory overflow window
     * this port sets (..0x88000000) and below RSX local memory. */
    ppu_hle_inject_base = 0x8F000000u;
    /* The point-light shaft mask (fragment program fp-struct
     * 1a9b74dc1afc2a84) writes max(distance / radius, behind-the-light)^4
     * unclamped, and the shaft composite multiplies the scene by about
     * 0.3 + 1.05 * mask^2: the village interior's smoky doorway and windows
     * blew out to white. Clamped to [0, 1] the room matches the original. */
    setenv("RSX_FP_SAT_ALPHA", "1a9b74dc1afc2a84", 0);
    /* Translated shaders survive between runs (see rsx_metal_backend.m). */
    setenv("PS3RECOMP_MSL_CACHE", "cache/msl", 0);
    /* ...and compiled DXBC on Windows (rsx_d3d12_engine.c). */
    setenv("PS3RECOMP_DXBC_CACHE", "cache/dxbc", 0);
    mkdir("cache", 0755);
    /* The sound driver (CDevSd's MultiStream threads and the MultiStream SPU
     * task, image 1 = spu_0000) at user-interactive QoS: at default QoS a
     * busy moment wrote its blocks late, 88 gaps (3.9 s of sound) in a
     * 4-minute battle, against 2 boosted (AUDIO_GAPS=1 counts them). */
    setenv("PPU_QOS_INTERACTIVE", "CDevSd", 0);
    setenv("SPU_QOS_INTERACTIVE", "1", 0);

    if (!alloc_guest_vm()) {
        fprintf(stderr, "ERROR: could not allocate the guest address space\n");
        return 1;
    }

    uint32_t entry = ppu_load_elf(argv[1]);
    if (!entry) {
        fprintf(stderr, "ERROR: could not load %s\n", argv[1]);
        return 1;
    }
    apply_fps_unlock();
#ifdef _WIN32
    /* DOD3_PROF / DOD3_STALL_MS / DOD3_STALL_SAMPLE: src/win_prof.cpp. */
    win_prof_start();
#else
    if (const char* d = getenv("DOD3_STALL_SAMPLE")) {
        mkdir(d, 0755);
        pthread_t th;
        if (pthread_create(&th, NULL, stall_watch, (void*)d) == 0) pthread_detach(th);
    }
#endif
#ifdef __APPLE__
    if (getenv("DOD3_STUTTER_MS")) {
        pthread_t th;
        if (pthread_create(&th, NULL, cpu_watch, NULL) == 0) pthread_detach(th);
    }
#endif

    derive_vfs_root(argv[1]);
    printf("[boot] VFS root: %s\n", ppu_vfs_root);

    ppu_recomp_register();   /* lifted function table -> address map */
    ps3_load_prx_modules();  /* this game's lifted system PRX, if it has any */
    ppu_hle_init();          /* firmware import NID -> HLE handlers */
    ppu_sysprx_register();   /* boot-critical CRT */
    ppu_fs_register();       /* cellFs over the game directory */
    lv2_init_syscalls();     /* the lv2 syscall table */

    /* Install the guest-callback hook, then start the frame clock. It no-ops
     * until the title registers its vblank and flip handlers during init. */
    g_ps3_guest_caller = harness_guest_caller;

    printf("\n[boot] dispatching entry OPD 0x%08X (stack top 0x%08X)\n\n",
           entry, STACK_TOP);

#ifdef __APPLE__
    /* AppKit only creates windows on the main thread, and the Metal backend's
     * init dispatch_syncs onto the main queue when called from anywhere else.
     * With the guest occupying the main thread nothing drains that queue and
     * the frame clock deadlocks before it ever walks the FIFO -- the guest then
     * spins on GCM get forever. So on Apple the roles swap: the guest runs on
     * a thread with the same 256 MB host stack guest PPU threads get, and the
     * frame clock owns the main thread. A guest exit ends the process from
     * its own thread. */
    s_boot_entry = entry;
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, 256u * 1024 * 1024);
    pthread_t guest;
    if (pthread_create(&guest, &attr, guest_main, NULL) != 0) {
        fprintf(stderr, "ERROR: could not start the guest thread\n");
        return 1;
    }
    pthread_attr_destroy(&attr);
    frame_clock(NULL);
    return 0;
#else
    CreateThread(NULL, 4u * 1024 * 1024, frame_clock, NULL, 0, NULL);

    int rc = ppu_run(entry, STACK_TOP);
    printf("\n[boot] ppu_run returned %d (entry function unwound)\n", rc);

    /* A title that called sys_process_exit never reaches this line: that path
     * ends in the host exit(). Getting here means the entry function returned
     * instead, so hand back the status the guest published if it published
     * one. Returning a hardcoded 0 would report success for a run that never
     * got anywhere. */
    return g_sys_process_exit_called ? (int)g_sys_process_exit_code : 0;
#endif
}
