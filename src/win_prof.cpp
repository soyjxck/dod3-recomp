/*
 * Windows host profiler for the port: the counterpart of the Mac's `sample`
 * (DOD3_STALL_SAMPLE) and the mach-based [stutter-cpu] report.
 *
 *   DOD3_PROF=<ms>        sample every host thread's stack every <ms> ms
 *                         (1-10; 2 is a good default) and print, every
 *                         DOD3_PROF_REPORT seconds (default 5), each busy
 *                         thread's CPU share and its hottest functions, plus
 *                         the hottest functions of the whole process.
 *   DOD3_STALL_MS=<n>     when no frame has been presented for n ms (default
 *                         300 when DOD3_STALL_SAMPLE is set), dump every
 *                         thread's stack once, at most DOD3_STALL_MAX times
 *                         (default 12) and 2 s apart. DOD3_STALL_SAMPLE=<dir>
 *                         writes the dumps to <dir>/stall_NN_fFRAME.txt instead
 *                         of stderr -- the macOS switch, same meaning.
 *
 * A sample is one SuspendThread + GetThreadContext + StackWalk64 per thread;
 * a thread whose CPU time has not moved since the last sample was asleep and
 * is not counted, so the shares are on-CPU time. Host functions are named
 * through dbghelp (the build links with /DEBUG and compiles the runtime with
 * /Z7); lifted PPU code has no symbols and is named through the lifter's
 * function table as ppu:<guest address>.
 */
#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <dbghelp.h>
#include <tlhelp32.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <string>
#include <vector>
#include <map>
#include <unordered_map>
#include <algorithm>
#include <direct.h>

#pragma comment(lib, "dbghelp.lib")

extern "C" uint32_t g_rsx_engine_frame;

/* func_entry in the generated ppu_recomp.h. */
struct prof_fentry { uint64_t addr; void* func; const char* name; };
extern "C" const struct prof_fentry function_table[];
extern "C" const uint64_t function_table_count;

namespace {

struct ThreadStat {
    std::string name;
    uint64_t samples = 0;            /* on-CPU samples this report */
    uint64_t waits = 0;              /* samples found in a wait */
    std::unordered_map<uint64_t, uint64_t> wait_site;   /* first non-ntdll frame above a wait */
    std::unordered_map<uint64_t, uint64_t> self;   /* leaf address -> count */
    std::unordered_map<uint64_t, uint64_t> incl;   /* DOD3_PROF_TREE: function -> samples with it on the stack */
    std::map<std::string, uint64_t> wait_chain;    /* DOD3_PROF_TREE: "wait <- caller <- ..." -> samples */
    /* DOD3_PROF_TREE threads keep their last samples with a time stamp, so a
     * slow frame can be explained after the fact (win_prof_slow_frame). */
    struct Sample { uint64_t t_us; int n; uint64_t fr[6]; };
    std::vector<Sample> ring; size_t ring_pos = 0;
};
static uint64_t prof_now_us(void)
{
    static LARGE_INTEGER f; LARGE_INTEGER c;
    if (!f.QuadPart) QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&c);
    return (uint64_t)(c.QuadPart / f.QuadPart) * 1000000ull + (uint64_t)(c.QuadPart % f.QuadPart) * 1000000ull / (uint64_t)f.QuadPart;
}

struct Sym { std::string name; uint64_t base; };

static std::unordered_map<uint64_t, Sym> s_syms;   /* address -> symbol (cached per 16-byte bucket) */
static std::vector<std::pair<uint64_t, const char*>> s_ppu;   /* host func -> lifted name, sorted */
static SRWLOCK s_dbg = SRWLOCK_INIT;


static void ppu_table_init()
{
    if (!s_ppu.empty() || !function_table_count) return;
    s_ppu.reserve((size_t)function_table_count);
    for (uint64_t i = 0; i < function_table_count; i++)
        if (function_table[i].func) s_ppu.push_back({ (uint64_t)function_table[i].func, function_table[i].name });
    std::sort(s_ppu.begin(), s_ppu.end());
}

/* The lifted function containing a host address, or NULL. */
static const char* ppu_name(uint64_t addr, uint64_t* base)
{
    if (s_ppu.empty()) return NULL;
    auto it = std::upper_bound(s_ppu.begin(), s_ppu.end(), std::make_pair(addr, (const char*)NULL),
                               [](const std::pair<uint64_t, const char*>& a, const std::pair<uint64_t, const char*>& b) { return a.first < b.first; });
    if (it == s_ppu.begin()) return NULL;
    --it;
    if (addr - it->first > 0x100000) return NULL;   /* a lifted function is never 1 MB */
    *base = it->first;
    return it->second;
}

static const Sym& symbolize(uint64_t addr)
{
    auto it = s_syms.find(addr);
    if (it != s_syms.end()) return it->second;
    Sym s; s.base = addr;
    uint64_t pb = 0;
    /* dbghelp first: the runtime and the SPU code have symbols, the lifted
     * PPU TUs do not, so a hit is right and a miss falls back to the lifted
     * function table. (The table first misnamed SPU functions as the last
     * PPU function before them in memory.) */
    char buf[sizeof(SYMBOL_INFO) + 256] = {0};
    SYMBOL_INFO* si = (SYMBOL_INFO*)buf;
    si->SizeOfStruct = sizeof(SYMBOL_INFO); si->MaxNameLen = 255;
    DWORD64 disp = 0;
    if (SymFromAddr(GetCurrentProcess(), addr, &disp, si) && si->Address <= addr && addr - si->Address < 0x40000) {
        s.name = si->Name; s.base = si->Address;
    } else if (const char* pn = ppu_name(addr, &pb)) {
        s.name = std::string("ppu:") + (pn ? pn : "?"); s.base = pb;
    } else {
        {
            HMODULE m = NULL; char mod[MAX_PATH] = "?";
            if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCSTR)addr, &m)) {
                GetModuleFileNameA(m, mod, sizeof mod);
                const char* b = strrchr(mod, '\\'); if (b) memmove(mod, b + 1, strlen(b));
            }
            char t[300]; snprintf(t, sizeof t, "%s+0x%llx", mod, (unsigned long long)(addr - (uint64_t)m));
            s.name = t; s.base = (uint64_t)m;
        }
    }
    return s_syms.emplace(addr, s).first->second;
}

/* Is this leaf an ntdll wait or delay: the thread is asleep, not running. */
static bool is_wait_leaf(const Sym& s)
{
    const char* n = s.name.c_str();
    if (strncmp(n, "Nt", 2) && strncmp(n, "Zw", 2)) return false;
    return strstr(n, "Wait") || strstr(n, "Delay") || strstr(n, "RemoveIoCompletion") ||
           strstr(n, "SignalAndWait") || strstr(n, "WorkerFactory") || strstr(n, "YieldExecution") ||
           strstr(n, "ReadFile") || strstr(n, "DeviceIoControl") || strstr(n, "ReplyWaitReceivePort");
}

static std::string thread_name(HANDLE h, DWORD tid)
{
    PWSTR d = NULL;
    std::string n;
    if (SUCCEEDED(GetThreadDescription(h, &d)) && d) {
        char buf[128]; WideCharToMultiByte(CP_UTF8, 0, d, -1, buf, sizeof buf, NULL, NULL);
        n = buf; LocalFree(d);
    }
    if (n.empty()) { char b[32]; snprintf(b, sizeof b, "tid %lu", (unsigned long)tid); n = b; }
    return n;
}

/* One thread's stack, suspended. Returns frames written.
 *
 * Unwound with RtlVirtualUnwind rather than StackWalk64: dbghelp takes
 * locks (its own and the loader's) that the suspended thread may hold, and
 * the first version of this hung the profiler on its first pass. The x64
 * unwinder needs only the image's function tables. */
static int safe_read64(uint64_t at, uint64_t* out)
{
    SIZE_T n = 0;
    return ReadProcessMemory(GetCurrentProcess(), (LPCVOID)at, out, 8, &n) && n == 8;
}
/* One frame up. A thread stopped in the middle of a prologue, or in code
 * whose unwind data does not describe where it is, hands the unwinder a
 * stack pointer that points nowhere; RtlVirtualUnwind then faults reading
 * it. That killed the profiler (and with it the report of the run) once the
 * walk went 64 frames deep. The walk just ends there. */
static bool unwind_step(CONTEXT* ctx)
{
    __try {
        DWORD64 base = 0;
        PRUNTIME_FUNCTION rf = RtlLookupFunctionEntry(ctx->Rip, &base, NULL);
        if (!rf) {
            /* A leaf function with no unwind data: the return address is
             * on top of the stack. */
            uint64_t ret = 0;
            if (!safe_read64(ctx->Rsp, &ret)) return false;
            ctx->Rip = ret; ctx->Rsp += 8;
        } else {
            PVOID handler = NULL; DWORD64 est = 0;
            RtlVirtualUnwind(UNW_FLAG_NHANDLER, base, ctx->Rip, rf, ctx, &handler, &est, NULL);
        }
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}
static int walk(HANDLE th, uint64_t* out, int cap)
{
    CONTEXT ctx; memset(&ctx, 0, sizeof ctx);
    ctx.ContextFlags = CONTEXT_CONTROL | CONTEXT_INTEGER;
    if (SuspendThread(th) == (DWORD)-1) return 0;
    int n = 0;
    if (GetThreadContext(th, &ctx)) {
        uint64_t last_sp = 0;
        while (n < cap && ctx.Rip) {
            out[n++] = ctx.Rip;
            /* The stack only unwinds upward; anything else is garbage. */
            if (ctx.Rsp < last_sp || (ctx.Rsp & 7)) break;
            last_sp = ctx.Rsp;
            if (!unwind_step(&ctx)) break;
        }
    }
    ResumeThread(th);
    return n;
}

struct Thr { DWORD tid; HANDLE h; };
static std::vector<Thr> s_threads; static ULONGLONG s_threads_at;

static void refresh_threads()
{
    const ULONGLONG now = GetTickCount64();
    if (!s_threads.empty() && now - s_threads_at < 1000) return;
    s_threads_at = now;
    std::vector<Thr> fresh;
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snap == INVALID_HANDLE_VALUE) return;
    THREADENTRY32 te; te.dwSize = sizeof te;
    const DWORD pid = GetCurrentProcessId(), self = GetCurrentThreadId();
    for (BOOL ok = Thread32First(snap, &te); ok; ok = Thread32Next(snap, &te)) {
        if (te.th32OwnerProcessID != pid || te.th32ThreadID == self) continue;
        HANDLE h = NULL;
        for (auto& t : s_threads) if (t.tid == te.th32ThreadID) { h = t.h; t.h = NULL; break; }
        if (!h) h = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE, te.th32ThreadID);
        if (h) fresh.push_back({ te.th32ThreadID, h });
    }
    CloseHandle(snap);
    for (auto& t : s_threads) if (t.h) CloseHandle(t.h);   /* gone */
    s_threads.swap(fresh);
}

static std::map<DWORD, ThreadStat> s_stats;
static const char* s_callers_of;                           /* DOD3_PROF_CALLERS */
/* DOD3_PROF_TREE=<part of a thread name>: for the threads it matches, walk
 * the whole stack (64 frames, not 16) and report the functions by inclusive
 * time -- what the thread is doing, from its main loop down, where the self
 * list only names the leaves. "tid " matches the threads without a name:
 * the game's main thread and the RSX walker. */
static const char* s_tree_of;
/* A comma-separated list of name parts: "tid,Rendering" is the game's main
 * thread, the RSX walker and the title's render thread. */
static bool tree_match(const char* name)
{
    const char* p = s_tree_of;
    while (p && *p) {
        const char* c = strchr(p, ',');
        const size_t n = c ? (size_t)(c - p) : strlen(p);
        if (n && strstr(name, std::string(p, n).c_str())) return true;
        p = c ? c + 1 : NULL;
    }
    return false;
}
static std::map<std::string, uint64_t> s_caller_chains;    /* "a <- b <- c" -> samples */
static std::unordered_map<uint64_t, uint64_t> s_self_all, s_incl_all;
static uint64_t s_total_samples;
static uint64_t s_passes;            /* sampling passes this report window */

static void report(double secs, long interval_ms)
{
    std::vector<std::pair<uint64_t, DWORD>> busy;
    uint64_t cpu_total = 0;
    for (auto& kv : s_stats) { busy.push_back({ kv.second.samples, kv.first }); }
    std::sort(busy.rbegin(), busy.rend());
    /* A thread seen on-CPU in every pass is using a whole core: shares are
     * samples over passes. (Scaling by the nominal interval understated them:
     * a pass takes longer than the sleep between passes.) */
    const double per = s_passes ? secs / (double)s_passes : interval_ms / 1000.0;
    fprintf(stderr, "[prof] %.1f s window, %llu passes (%.1f ms apart, asked %ld), %llu on-CPU samples; threads by CPU:", secs,
            (unsigned long long)s_passes, s_passes ? secs * 1000.0 / (double)s_passes : 0.0, interval_ms,
            (unsigned long long)s_total_samples);
    int shown = 0;
    for (auto& b : busy) {
        if (!b.first) break;
        const ThreadStat& t = s_stats[b.second];
        const double pct = 100.0 * b.first * per / secs;
        if (shown++ < 12) fprintf(stderr, " %s %.0f%%,", t.name.c_str(), pct);
        cpu_total += b.first;
    }
    fprintf(stderr, " total %.1f cores\n", cpu_total * per / secs);
    for (auto& b : busy) {
        const ThreadStat& t = s_stats[b.second];
        const double pct = 100.0 * b.first * per / secs;
        /* Threads under 15% are skipped, except those DOD3_PROF_TREE names:
         * several lightly loaded threads can still be the chain a frame
         * waits on (PhysX's SPU tasks). */
        if (pct < 15.0 && !(pct >= 1.0 && s_tree_of && tree_match(t.name.c_str()))) continue;
        std::vector<std::pair<uint64_t, uint64_t>> top(t.self.begin(), t.self.end());
        std::sort(top.begin(), top.end(), [](auto& a, auto& c) { return a.second > c.second; });
        fprintf(stderr, "[prof]   %s (%.0f%%):", t.name.c_str(), pct);
        for (size_t i = 0; i < top.size() && i < 8; i++)
            fprintf(stderr, " %.1f%% %s;", 100.0 * top[i].second / b.first, symbolize(top[i].first).name.c_str());
        fputc('\n', stderr);
        if (!t.incl.empty()) {
            std::vector<std::pair<uint64_t, uint64_t>> in(t.incl.begin(), t.incl.end());
            std::sort(in.begin(), in.end(), [](auto& a, auto& c) { return a.second > c.second; });
            fprintf(stderr, "[prof]     inclusive:");
            for (size_t i = 0; i < in.size() && i < 40; i++)
                fprintf(stderr, " %.0f%% %s;", 100.0 * in[i].second / b.first, symbolize(in[i].first).name.c_str());
            fputc('\n', stderr);
        }
        if (t.waits) {
            std::vector<std::pair<uint64_t, uint64_t>> ws(t.wait_site.begin(), t.wait_site.end());
            std::sort(ws.begin(), ws.end(), [](auto& a, auto& c) { return a.second > c.second; });
            fprintf(stderr, "[prof]     waits (%.0f%% of its samples):", 100.0 * t.waits / (t.waits + b.first));
            for (size_t i = 0; i < ws.size() && i < 5; i++)
                fprintf(stderr, " %.0f%% %s;", 100.0 * ws[i].second / t.waits, symbolize(ws[i].first).name.c_str());
            fputc('\n', stderr);
            if (!t.wait_chain.empty()) {
                std::vector<std::pair<uint64_t, std::string>> wc;
                for (auto& kv : t.wait_chain) wc.push_back({ kv.second, kv.first });
                std::sort(wc.rbegin(), wc.rend());
                for (size_t i = 0; i < wc.size() && i < 8; i++)
                    fprintf(stderr, "[prof]       %.0f%% of its waits: %s\n", 100.0 * wc[i].first / t.waits, wc[i].second.c_str());
            }
        }
    }
    auto dump = [&](const char* what, std::unordered_map<uint64_t, uint64_t>& m, int n) {
        std::vector<std::pair<uint64_t, uint64_t>> top(m.begin(), m.end());
        std::sort(top.begin(), top.end(), [](auto& a, auto& c) { return a.second > c.second; });
        fprintf(stderr, "[prof]   %s:", what);
        for (size_t i = 0; i < top.size() && i < (size_t)n; i++)
            fprintf(stderr, " %.1f%% %s;", 100.0 * top[i].second / (s_total_samples ? s_total_samples : 1), symbolize(top[i].first).name.c_str());
        fputc('\n', stderr);
    };
    if (s_callers_of && !s_caller_chains.empty()) {
        std::vector<std::pair<uint64_t, std::string>> cc;
        for (auto& kv : s_caller_chains) cc.push_back({ kv.second, kv.first });
        std::sort(cc.rbegin(), cc.rend());
        fprintf(stderr, "[prof]   callers of %s:", s_callers_of);
        for (size_t i = 0; i < cc.size() && i < 6; i++) fprintf(stderr, " %llu x [%s];", (unsigned long long)cc[i].first, cc[i].second.c_str());
        fputc('\n', stderr);
        s_caller_chains.clear();
    }
    dump("hottest functions (self)", s_self_all, 14);
    dump("hottest functions (inclusive)", s_incl_all, 14);
    for (auto& kv : s_stats) { kv.second.samples = 0; kv.second.waits = 0; kv.second.self.clear(); kv.second.wait_site.clear(); kv.second.incl.clear(); kv.second.wait_chain.clear(); }
    s_self_all.clear(); s_incl_all.clear(); s_total_samples = 0; s_passes = 0;
}

static void dump_all_stacks(FILE* out, const char* why)
{
    refresh_threads();
    fprintf(out, "%s\n", why);
    for (auto& t : s_threads) {
        uint64_t fr[48];
        const int n = walk(t.h, fr, 48);
        fprintf(out, "  thread %lu \"%s\":\n", (unsigned long)t.tid, thread_name(t.h, t.tid).c_str());
        for (int i = 0; i < n; i++) {
            const Sym& s = symbolize(fr[i]);
            fprintf(out, "    %2d  %s +0x%llx\n", i, s.name.c_str(), (unsigned long long)(fr[i] - s.base));
        }
    }
    fflush(out);
}

/* A slow-frame report asked for by another thread (RSX_HITCH_LOG's hook, on
 * the walker): printed here, between sampling passes, not on the asking
 * thread -- which could be suspended mid-allocation by this one while it
 * waited for the symbol lock, and the boot hung that way once. One at a
 * time; a request while one is pending is dropped. */
static volatile LONG s_sf_pending;
static uint64_t s_sf_start, s_sf_end; static double s_sf_ms;
extern "C" void win_prof_slow_frame(uint64_t start_us, uint64_t end_us, double frame_ms);
extern "C" void win_prof_slow_frame_async(uint64_t start_us, uint64_t end_us, double frame_ms)
{
    if (s_sf_pending) return;
    s_sf_start = start_us; s_sf_end = end_us; s_sf_ms = frame_ms;
    InterlockedExchange(&s_sf_pending, 1);
}

static DWORD WINAPI prof_thread(LPVOID arg)
{
    const long interval = (long)(intptr_t)arg;
    SetThreadDescription(GetCurrentThread(), L"dod3 profiler");
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);
    long report_s = 5;
    if (const char* e = getenv("DOD3_PROF_REPORT")) report_s = atol(e) > 0 ? atol(e) : 5;
    /* Not before the first frame is presented: suspending threads while the
     * D3D12 device and window are being created deadlocked the boot twice
     * (a thread held the loader lock while suspended, and the walk here
     * needs it), hanging at GCM init with only the 30 us polls in the log. */
    while (g_rsx_engine_frame == 0) Sleep(50);
    LARGE_INTEGER qf, t0; QueryPerformanceFrequency(&qf); QueryPerformanceCounter(&t0);
    for (;;) {
        Sleep((DWORD)interval);
        if (s_sf_pending) { win_prof_slow_frame(s_sf_start, s_sf_end, s_sf_ms); InterlockedExchange(&s_sf_pending, 0); }
        refresh_threads();
        AcquireSRWLockExclusive(&s_dbg);
        s_passes++;
        const uint64_t pass_us = prof_now_us();
        for (auto& t : s_threads) {
            ThreadStat& st = s_stats[t.tid];
            if (st.name.empty() || st.name.compare(0, 4, "tid ") == 0) st.name = thread_name(t.h, t.tid);
            uint64_t fr[64];
            const bool deep = s_tree_of && tree_match(st.name.c_str());
            const int n = walk(t.h, fr, deep ? 64 : 16);
            if (!n) continue;
            if (deep) {
                if (st.ring.empty()) st.ring.resize(4096);
                ThreadStat::Sample& smp = st.ring[st.ring_pos++ % st.ring.size()];
                smp.t_us = pass_us; smp.n = n < 6 ? n : 6;
                for (int i = 0; i < smp.n; i++) smp.fr[i] = fr[i];
            }
            /* Leaf attributed to its function start, so one function is one key. */
            const Sym& ls = symbolize(fr[0]);
            if (is_wait_leaf(ls)) {
                st.waits++;
                /* The first frame outside ntdll/kernelbase names the wait. */
                for (int i = 1; i < n; i++) {
                    const Sym& w = symbolize(fr[i]);
                    const char* nm = w.name.c_str();
                    if (!strncmp(nm, "Nt", 2) || !strncmp(nm, "Zw", 2) || !strncmp(nm, "Rtl", 3) ||
                        strstr(nm, "ntdll") || strstr(nm, "KERNELBASE") || strstr(nm, "kernel32") ||
                        !strcmp(nm, "WaitForSingleObjectEx") || !strcmp(nm, "WaitForSingleObject") ||
                        !strcmp(nm, "SleepEx") || !strcmp(nm, "Sleep") || !strcmp(nm, "SleepConditionVariableSRW") ||
                        !strcmp(nm, "SleepConditionVariableCS") || !strcmp(nm, "WaitForMultipleObjectsEx"))
                        continue;
                    st.wait_site[w.base]++;
                    if (deep) {
                        /* Who is waiting: the wait and the seven frames above it. */
                        std::string chain;
                        for (int k = i; k < n && k < i + 8; k++) { if (k > i) chain += " <- "; chain += symbolize(fr[k]).name; }
                        st.wait_chain[chain]++;
                    }
                    break;
                }
                continue;
            }
            const uint64_t leaf = ls.base;
            if (s_callers_of && ls.name == s_callers_of) {
                std::string chain;
                for (int i = 1; i < n && i <= 3; i++) { if (i > 1) chain += " <- "; chain += symbolize(fr[i]).name; }
                s_caller_chains[chain]++;
            }
            st.samples++; s_total_samples++;
            st.self[leaf]++; s_self_all[leaf]++;
            std::vector<uint64_t> seen;
            for (int i = 0; i < n; i++) {
                const uint64_t b = symbolize(fr[i]).base;
                if (std::find(seen.begin(), seen.end(), b) != seen.end()) continue;
                seen.push_back(b); s_incl_all[b]++;
                if (deep) st.incl[b]++;
            }
        }
        ReleaseSRWLockExclusive(&s_dbg);
        LARGE_INTEGER now; QueryPerformanceCounter(&now);
        const double secs = (double)(now.QuadPart - t0.QuadPart) / qf.QuadPart;
        if (secs >= report_s) {
            AcquireSRWLockExclusive(&s_dbg);
            report(secs, interval);
            ReleaseSRWLockExclusive(&s_dbg);
            t0 = now;
        }
    }
}

static DWORD WINAPI stall_thread(LPVOID arg)
{
    const char* dir = (const char*)arg;
    SetThreadDescription(GetCurrentThread(), L"dod3 stall watch");
    long thr_ms = 300, max_n = 12;
    if (const char* e = getenv("DOD3_STALL_MS")) thr_ms = atol(e) > 0 ? atol(e) : 300;
    if (const char* e = getenv("DOD3_STALL_MAX")) max_n = atol(e);
    /* DOD3_STALL_FROM=<s>: arm only after that many seconds, so a run's slow
     * boot does not spend every dump before the part under study. */
    if (const char* e = getenv("DOD3_STALL_FROM")) { long s = atol(e); if (s > 0) Sleep((DWORD)s * 1000u); }
    uint32_t last = g_rsx_engine_frame;
    ULONGLONG last_t = 0, last_sample = 0, stall_from = 0;
    { LARGE_INTEGER qf, qc; QueryPerformanceFrequency(&qf); QueryPerformanceCounter(&qc); last_t = (ULONGLONG)(qc.QuadPart * 1000 / qf.QuadPart); }
    long n = 0;
    for (;;) {
        Sleep(thr_ms < 100 ? 5 : 20);
        const uint32_t f = g_rsx_engine_frame;
        LARGE_INTEGER qf, qc; QueryPerformanceFrequency(&qf); QueryPerformanceCounter(&qc);
        const ULONGLONG t = (ULONGLONG)(qc.QuadPart * 1000 / qf.QuadPart);   /* ms, fine-grained */
        if (f != last) {
            if (stall_from) fprintf(stderr, "[stall] frame %u came after %llu ms\n", f, (unsigned long long)(t - last_t));
            last = f; last_t = t; stall_from = 0;
            continue;
        }
        if (!f || t - last_t < (ULONGLONG)thr_ms) continue;
        if (!stall_from) stall_from = t;
        if (n < max_n && t - last_sample > 2000 && stall_from == t) {
            char why[160];
            snprintf(why, sizeof why, "[stall] no present for %ld ms after frame %u -- every thread's stack:", thr_ms, f);
            FILE* out = stderr;
            char path[512] = "";
            if (dir && *dir) {
                snprintf(path, sizeof path, "%s/stall_%02ld_f%u.txt", dir, n, f);
                FILE* fp = fopen(path, "w");
                if (fp) out = fp;
            }
            AcquireSRWLockExclusive(&s_dbg);
            dump_all_stacks(out, why);
            ReleaseSRWLockExclusive(&s_dbg);
            if (out != stderr) { fclose(out); fprintf(stderr, "%s -> %s\n", why, path); }
            n++; last_sample = t;
        }
    }
}

} // namespace

static LONG WINAPI crash_filter(EXCEPTION_POINTERS* ep)
{
    static volatile LONG once = 0;
    if (InterlockedCompareExchange(&once, 1, 0) != 0) { Sleep(5000); return EXCEPTION_EXECUTE_HANDLER; }
    const EXCEPTION_RECORD* er = ep->ExceptionRecord;
    fprintf(stderr, "\n[crash] exception 0x%08lX at %p on thread %lu \"%s\"", (unsigned long)er->ExceptionCode,
            er->ExceptionAddress, (unsigned long)GetCurrentThreadId(),
            thread_name(GetCurrentThread(), GetCurrentThreadId()).c_str());
    if (er->ExceptionCode == EXCEPTION_ACCESS_VIOLATION && er->NumberParameters >= 2)
        fprintf(stderr, " -- %s of %p", er->ExceptionInformation[0] == 0 ? "read" : er->ExceptionInformation[0] == 1 ? "write" : "execute",
                (void*)er->ExceptionInformation[1]);
    fputc('\n', stderr);
    /* Unwind from the faulting context: the same walk as the sampler, on a
     * copy of the context rather than a suspended thread. */
    CONTEXT ctx = *ep->ContextRecord;
    AcquireSRWLockExclusive(&s_dbg);
    for (int n = 0; n < 48 && ctx.Rip; n++) {
        const Sym& sy = symbolize(ctx.Rip);
        fprintf(stderr, "[crash]   %2d  %s +0x%llx\n", n, sy.name.c_str(), (unsigned long long)(ctx.Rip - sy.base));
        DWORD64 base = 0;
        PRUNTIME_FUNCTION rf = RtlLookupFunctionEntry(ctx.Rip, &base, NULL);
        if (!rf) { uint64_t ret = 0; if (!safe_read64(ctx.Rsp, &ret)) break; ctx.Rip = ret; ctx.Rsp += 8; }
        else { PVOID h = NULL; DWORD64 est = 0; RtlVirtualUnwind(UNW_FLAG_NHANDLER, base, ctx.Rip, rf, &ctx, &h, &est, NULL); }
    }
    ReleaseSRWLockExclusive(&s_dbg);
    fflush(stderr);
    return EXCEPTION_EXECUTE_HANDLER;   /* and die */
}

/* The samples each DOD3_PROF_TREE thread took between start_us and end_us
 * (the clock of frame_clock_us in main.cpp): what it was doing during one
 * slow frame. On-CPU leaves are named with their callers; a wait is named by
 * the first frame outside the system DLLs. */
extern "C" void win_prof_slow_frame(uint64_t start_us, uint64_t end_us, double frame_ms)
{
    if (!s_tree_of) return;
    AcquireSRWLockExclusive(&s_dbg);
    for (auto& kv : s_stats) {
        ThreadStat& st = kv.second;
        if (st.ring.empty()) continue;
        std::map<std::string, int> hist; int total = 0, waits = 0;
        for (const auto& smp : st.ring) {
            if (!smp.n || smp.t_us < start_us || smp.t_us > end_us) continue;
            total++;
            const Sym& ls = symbolize(smp.fr[0]);
            std::string key;
            if (is_wait_leaf(ls)) {
                waits++;
                key = "wait";
                for (int i = 1; i < smp.n; i++) {
                    const char* nm = symbolize(smp.fr[i]).name.c_str();
                    if (!strncmp(nm, "Nt", 2) || !strncmp(nm, "Zw", 2) || !strncmp(nm, "Rtl", 3) || strstr(nm, "ntdll") ||
                        strstr(nm, "KERNELBASE") || !strncmp(nm, "Sleep", 5) || !strncmp(nm, "WaitFor", 7)) continue;
                    key += " in "; key += nm; break;
                }
            } else {
                key = ls.name;
                for (int i = 1; i < smp.n && i < 4; i++) { key += " <- "; key += symbolize(smp.fr[i]).name; }
            }
            hist[key]++;
        }
        if (!total) continue;
        std::vector<std::pair<int, std::string>> top;
        for (auto& h : hist) top.push_back({ h.second, h.first });
        std::sort(top.rbegin(), top.rend());
        fprintf(stderr, "[slow-frame] %.1f ms: %s, %d samples (%d waiting):", frame_ms, st.name.c_str(), total, waits);
        for (size_t i = 0; i < top.size() && i < 5; i++)
            fprintf(stderr, " %d%% %s;", 100 * top[i].first / total, top[i].second.c_str());
        fputc('\n', stderr);
    }
    ReleaseSRWLockExclusive(&s_dbg);
}

extern "C" void win_prof_start(void)
{
    const char* prof = getenv("DOD3_PROF");
    const char* stall_dir = getenv("DOD3_STALL_SAMPLE");
    const char* stall_ms = getenv("DOD3_STALL_MS");
    SymSetOptions(SYMOPT_DEFERRED_LOADS | SYMOPT_UNDNAME | SYMOPT_NO_PROMPTS);
    if (!SymInitialize(GetCurrentProcess(), NULL, TRUE))
        fprintf(stderr, "[prof] SymInitialize failed (%lu): host functions will be unnamed\n", GetLastError());
    ppu_table_init();
    if (!getenv("DOD3_NO_CRASH_HANDLER")) {
        SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
        SetUnhandledExceptionFilter(crash_filter);
    }
    if (!prof && !stall_dir && !stall_ms) return;
    s_callers_of = getenv("DOD3_PROF_CALLERS");
    s_tree_of = getenv("DOD3_PROF_TREE");
    if (prof) {
        long ms = atol(prof); if (ms < 1) ms = 2; if (ms > 50) ms = 50;
        CreateThread(NULL, 1u << 20, prof_thread, (LPVOID)(intptr_t)ms, 0, NULL);
        fprintf(stderr, "[prof] sampling every %ld ms\n", ms);
    }
    if (stall_dir || stall_ms) {
        if (stall_dir && *stall_dir) _mkdir(stall_dir);
        CreateThread(NULL, 1u << 20, stall_thread, (LPVOID)stall_dir, 0, NULL);
    }
}
#endif
