/*
 * macOS host profiler for the port: the counterpart of src/os/win32/prof.cpp,
 * with the same switches and report.
 *
 *   DOD3_PROF=<ms>        sample every host thread's stack every <ms> ms
 *                         (1-50; 2 is a good default) and print, every
 *                         DOD3_PROF_REPORT seconds (default 5), each busy
 *                         thread's CPU share and its hottest functions, plus
 *                         the hottest functions of the whole process.
 *   DOD3_PROF_TREE=<part of a thread name>[,...]   for the threads it
 *                         matches, the whole stack (64 frames): inclusive
 *                         time and the call chains behind the waits.
 *   DOD3_PROF_CALLERS=<function>   who calls a function.
 *
 * The DOD3_PROF_TREE threads also keep their last ten seconds of samples, so
 * a slow frame can be explained after it happened: dod3_prof_slow_frame
 * (DOD3_LOAD_LOG's frames over 100 ms) prints, per such thread, where the
 * samples inside that frame were ([slow-frame] lines).
 *
 * `sample` (DOD3_STALL_SAMPLE) takes every thread's stack at once and gets a
 * few dozen samples a second out of this process's 60-odd threads; this takes
 * them one thread at a time from inside, a few microseconds each. A sample is
 * thread_suspend + thread_get_state + a walk of the frame-pointer chain
 * (arm64 code on macOS always keeps one) while the thread is stopped, and
 * nothing that takes a lock or allocates until it runs again: a thread
 * stopped inside malloc would otherwise stop the sampler with it. A thread
 * the kernel reports as not running is waiting, charged to the first frame in
 * this executable above the system libraries, so the shares are on-CPU time.
 * Functions are named from this executable's own symbol table (lifted PPU
 * code as ppu:func_<guest address>), the system libraries through dladdr.
 */
#ifdef __APPLE__
#include <mach/mach.h>
#include <mach/mach_vm.h>
#include <mach/thread_status.h>
#include <mach-o/dyld.h>
#include <mach-o/loader.h>
#include <mach-o/nlist.h>
#include <dlfcn.h>
#include <cxxabi.h>
#include <pthread.h>
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <string>
#include <vector>
#include <map>
#include <unordered_map>
#include <algorithm>
#include "util.h"

extern "C" uint32_t g_rsx_engine_frame;

namespace {

/* ---- symbols ---------------------------------------------------------- */

struct Sym {
    std::string name;
    uint64_t base;
    bool system;   /* outside this executable */
};

static uint64_t s_text_lo, s_text_hi;                           /* this executable's __TEXT */
static std::vector<std::pair<uint64_t, const char*>> s_symtab;  /* address -> raw name, sorted */
static std::unordered_map<uint64_t, Sym> s_syms;                /* address -> symbol (cache) */

/* This executable's nlist symbols, local ones included: the build is not
 * stripped, and the lifted functions and the runtime's static functions are
 * all there. The table lives in __LINKEDIT, which stays mapped. */
static void symtab_init()
{
    Dl_info self;
    if (!dladdr((const void*)&symtab_init, &self)) return;
    const mach_header_64* mh = (const mach_header_64*)self.dli_fbase;
    intptr_t slide = 0;
    for (uint32_t i = 0; i < _dyld_image_count(); i++)
        if ((const void*)_dyld_get_image_header(i) == (const void*)mh) slide = _dyld_get_image_vmaddr_slide(i);
    const segment_command_64* linkedit = NULL;
    const symtab_command* st = NULL;
    const uint8_t* p = (const uint8_t*)(mh + 1);
    for (uint32_t i = 0; i < mh->ncmds; i++) {
        const load_command* lc = (const load_command*)p;
        if (lc->cmd == LC_SEGMENT_64) {
            const segment_command_64* sg = (const segment_command_64*)lc;
            if (!strcmp(sg->segname, "__LINKEDIT")) linkedit = sg;
            if (!strcmp(sg->segname, "__TEXT")) {
                s_text_lo = sg->vmaddr + slide;
                s_text_hi = s_text_lo + sg->vmsize;
            }
        } else if (lc->cmd == LC_SYMTAB)
            st = (const symtab_command*)lc;
        p += lc->cmdsize;
    }
    if (!linkedit || !st) return;
    const uint8_t* le = (const uint8_t*)(linkedit->vmaddr + slide - linkedit->fileoff);
    const nlist_64* nl = (const nlist_64*)(le + st->symoff);
    const char* strs = (const char*)(le + st->stroff);
    s_symtab.reserve(st->nsyms);
    for (uint32_t i = 0; i < st->nsyms; i++) {
        if (nl[i].n_type & N_STAB) continue;
        if ((nl[i].n_type & N_TYPE) != N_SECT) continue;
        const uint64_t a = nl[i].n_value + slide;
        if (a < s_text_lo || a >= s_text_hi) continue;
        s_symtab.push_back({ a, strs + nl[i].n_un.n_strx });
    }
    std::sort(s_symtab.begin(), s_symtab.end());
}

/* "__Z13func_01000158P11ppu_context" -> "ppu:func_01000158"; C++ names
 * without their argument list. */
static std::string pretty(const char* raw)
{
    const char* m = raw[0] == '_' ? raw + 1 : raw;
    int status = 0;
    char* d = (m[0] == '_' && m[1] == 'Z') ? abi::__cxa_demangle(m, NULL, NULL, &status) : NULL;
    std::string s = d && !status ? d : m;
    free(d);
    std::string suffix;
    const size_t cold = s.find(" (.cold");
    if (cold != std::string::npos) suffix = " (cold)";
    const size_t paren = s.find('(');
    if (paren != std::string::npos && paren > 0) s.resize(paren);
    if (!s.compare(0, 5, "func_")) s = "ppu:" + s;
    return s + suffix;
}

static const Sym& symbolize(uint64_t addr)
{
    auto it = s_syms.find(addr);
    if (it != s_syms.end()) return it->second;
    Sym s;
    s.base = addr;
    s.system = !(addr >= s_text_lo && addr < s_text_hi);
    if (!s.system && !s_symtab.empty()) {
        auto u = std::upper_bound(s_symtab.begin(), s_symtab.end(), std::make_pair(addr, (const char*)NULL),
                                  [](const std::pair<uint64_t, const char*>& a,
                                     const std::pair<uint64_t, const char*>& b) { return a.first < b.first; });
        if (u != s_symtab.begin()) {
            --u;
            s.name = pretty(u->second);
            s.base = u->first;
        }
    }
    if (s.name.empty()) {
        Dl_info di;
        if (dladdr((const void*)addr, &di) && di.dli_sname) {
            s.name = pretty(di.dli_sname);
            s.base = (uint64_t)di.dli_saddr;
        } else {
            char t[64];
            snprintf(t, sizeof t, "0x%llx", (unsigned long long)addr);
            s.name = t;
        }
    }
    return s_syms.emplace(addr, s).first->second;
}

/* ---- threads ---------------------------------------------------------- */

struct Thr {
    thread_act_t port;
    uint64_t id;
    uint64_t stack_lo, stack_hi;   /* a pthread's stack; 0 when not known */
};
static std::vector<Thr> s_threads;
static uint64_t s_threads_at;
static thread_act_t s_self_port;

static void refresh_threads()
{
    const uint64_t now = dod3_now_us();
    if (!s_threads.empty() && now - s_threads_at < 1000000) return;
    s_threads_at = now;
    for (auto& t : s_threads) mach_port_deallocate(mach_task_self(), t.port);
    s_threads.clear();
    thread_act_array_t th;
    mach_msg_type_number_t n = 0;
    if (task_threads(mach_task_self(), &th, &n) != KERN_SUCCESS) return;
    for (mach_msg_type_number_t i = 0; i < n; i++) {
        if (th[i] == s_self_port) {
            mach_port_deallocate(mach_task_self(), th[i]);
            continue;
        }
        Thr t = { th[i], 0, 0, 0 };
        thread_identifier_info_data_t idi;
        mach_msg_type_number_t c = THREAD_IDENTIFIER_INFO_COUNT;
        if (thread_info(th[i], THREAD_IDENTIFIER_INFO, (thread_info_t)&idi, &c) == KERN_SUCCESS) t.id = idi.thread_id;
        /* Here, with nothing suspended: the pthread lookup takes a lock. */
        if (pthread_t pt = pthread_from_mach_thread_np(th[i])) {
            const uint64_t hi = (uint64_t)pthread_get_stackaddr_np(pt);
            t.stack_hi = hi;
            t.stack_lo = hi - pthread_get_stacksize_np(pt);
        }
        s_threads.push_back(t);
    }
    vm_deallocate(mach_task_self(), (vm_address_t)th, n * sizeof(thread_act_t));
}

static std::string thread_name(const Thr& t)
{
    thread_extended_info_data_t ext;
    mach_msg_type_number_t c = THREAD_EXTENDED_INFO_COUNT;
    if (thread_info(t.port, THREAD_EXTENDED_INFO, (thread_info_t)&ext, &c) == KERN_SUCCESS && ext.pth_name[0])
        return ext.pth_name;
    char b[32];
    snprintf(b, sizeof b, "tid %llu", (unsigned long long)t.id);
    return b;
}

/* Reads one word of a stopped thread's stack: directly inside the stack the
 * pthread was given, through the kernel elsewhere (a fiber's stack, or a frame
 * pointer that points nowhere: the read fails instead of faulting). */
static bool read64(const Thr& t, uint64_t at, uint64_t* out)
{
    if (at & 7) return false;
    if (t.stack_hi && at >= t.stack_lo && at + 8 <= t.stack_hi) {
        *out = *(const uint64_t*)at;
        return true;
    }
    mach_vm_size_t got = 0;
    return mach_vm_read_overwrite(mach_task_self(), at, 8, (mach_vm_address_t)out, &got) == KERN_SUCCESS && got == 8;
}

/* One thread's stack: frames written to out (pc first), the link register in
 * *lr (the caller of a leaf that keeps no frame of its own). */
static int walk(const Thr& t, uint64_t* out, int cap, uint64_t* lr)
{
    if (thread_suspend(t.port) != KERN_SUCCESS) return 0;
    int n = 0;
    arm_thread_state64_t st;
    mach_msg_type_number_t c = ARM_THREAD_STATE64_COUNT;
    if (thread_get_state(t.port, ARM_THREAD_STATE64, (thread_state_t)&st, &c) == KERN_SUCCESS) {
        out[n++] = arm_thread_state64_get_pc(st);
        *lr = arm_thread_state64_get_lr(st);
        uint64_t fp = arm_thread_state64_get_fp(st);
        while (n < cap && fp) {
            uint64_t next = 0, ret = 0;
            if (!read64(t, fp, &next) || !read64(t, fp + 8, &ret) || !ret) break;
            out[n++] = ret;
            if (next <= fp) break;   /* the chain only goes up the stack */
            fp = next;
        }
    }
    thread_resume(t.port);
    return n;
}

/* ---- statistics --------------------------------------------------------- */

struct ThreadStat {
    std::string name;
    uint64_t samples = 0;   /* on-CPU samples this report */
    uint64_t waits = 0;     /* samples found waiting */
    std::unordered_map<uint64_t, uint64_t> wait_site;
    std::unordered_map<uint64_t, uint64_t> self;
    std::unordered_map<uint64_t, uint64_t> incl;
    std::map<std::string, uint64_t> wait_chain;
    /* DOD3_PROF_TREE threads: the last samples, time-stamped (slow_frame). */
    struct Sample {
        uint64_t t_us;
        bool wait;
        int n;
        uint64_t fr[32];
    };
    std::vector<Sample> ring;
    size_t ring_pos = 0;
};
static std::map<uint64_t, ThreadStat> s_stats;
static const char* s_callers_of;
static const char* s_tree_of;
static std::map<std::string, uint64_t> s_caller_chains;
static std::unordered_map<uint64_t, uint64_t> s_self_all, s_incl_all;
static uint64_t s_total_samples, s_passes;

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

static void report(double secs, long interval_ms, const char* why)
{
    std::vector<std::pair<uint64_t, uint64_t>> busy;
    for (auto& kv : s_stats) busy.push_back({ kv.second.samples, kv.first });
    std::sort(busy.rbegin(), busy.rend());
    const double per = s_passes ? secs / (double)s_passes : interval_ms / 1000.0;
    fprintf(stderr, "[prof] %.1f s window%s%s, %llu passes (%.1f ms apart, asked %ld), %llu on-CPU samples; threads by CPU:",
            secs, why ? " to " : "", why ? why : "", (unsigned long long)s_passes,
            s_passes ? secs * 1000.0 / (double)s_passes : 0.0, interval_ms, (unsigned long long)s_total_samples);
    uint64_t cpu_total = 0;
    int shown = 0;
    for (auto& b : busy) {
        if (!b.first) break;
        if (shown++ < 12) fprintf(stderr, " %s %.0f%%,", s_stats[b.second].name.c_str(), 100.0 * b.first * per / secs);
        cpu_total += b.first;
    }
    fprintf(stderr, " total %.1f cores\n", cpu_total * per / secs);
    for (auto& b : busy) {
        const ThreadStat& t = s_stats[b.second];
        const double pct = 100.0 * b.first * per / secs;
        const bool tree = s_tree_of && tree_match(t.name.c_str());
        if (pct < 15.0 && !(tree && (b.first || t.waits))) continue;
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
                fprintf(stderr, " %.0f%% %s;", 100.0 * in[i].second / (b.first ? b.first : 1),
                        symbolize(in[i].first).name.c_str());
            fputc('\n', stderr);
        }
        if (t.waits) {
            std::vector<std::pair<uint64_t, uint64_t>> ws(t.wait_site.begin(), t.wait_site.end());
            std::sort(ws.begin(), ws.end(), [](auto& a, auto& c) { return a.second > c.second; });
            fprintf(stderr, "[prof]     waits (%.0f%% of its samples):", 100.0 * t.waits / (t.waits + b.first));
            for (size_t i = 0; i < ws.size() && i < 5; i++)
                fprintf(stderr, " %.0f%% %s;", 100.0 * ws[i].second / t.waits, symbolize(ws[i].first).name.c_str());
            fputc('\n', stderr);
            std::vector<std::pair<uint64_t, std::string>> wc;
            for (auto& kv : t.wait_chain) wc.push_back({ kv.second, kv.first });
            std::sort(wc.rbegin(), wc.rend());
            for (size_t i = 0; i < wc.size() && i < 8; i++)
                fprintf(stderr, "[prof]       %.0f%% of its waits: %s\n", 100.0 * wc[i].first / t.waits,
                        wc[i].second.c_str());
        }
    }
    auto dump = [&](const char* what, std::unordered_map<uint64_t, uint64_t>& m, int n) {
        std::vector<std::pair<uint64_t, uint64_t>> top(m.begin(), m.end());
        std::sort(top.begin(), top.end(), [](auto& a, auto& c) { return a.second > c.second; });
        fprintf(stderr, "[prof]   %s:", what);
        for (size_t i = 0; i < top.size() && i < (size_t)n; i++)
            fprintf(stderr, " %.1f%% %s;", 100.0 * top[i].second / (s_total_samples ? s_total_samples : 1),
                    symbolize(top[i].first).name.c_str());
        fputc('\n', stderr);
    };
    if (s_callers_of && !s_caller_chains.empty()) {
        std::vector<std::pair<uint64_t, std::string>> cc;
        for (auto& kv : s_caller_chains) cc.push_back({ kv.second, kv.first });
        std::sort(cc.rbegin(), cc.rend());
        fprintf(stderr, "[prof]   callers of %s:", s_callers_of);
        for (size_t i = 0; i < cc.size() && i < 6; i++)
            fprintf(stderr, " %llu x [%s];", (unsigned long long)cc[i].first, cc[i].second.c_str());
        fputc('\n', stderr);
        s_caller_chains.clear();
    }
    dump("hottest functions (self)", s_self_all, 14);
    dump("hottest functions (inclusive)", s_incl_all, 14);
    for (auto& kv : s_stats) {
        kv.second.samples = kv.second.waits = 0;
        kv.second.self.clear();
        kv.second.wait_site.clear();
        kv.second.incl.clear();
        kv.second.wait_chain.clear();
    }
    s_self_all.clear();
    s_incl_all.clear();
    s_total_samples = s_passes = 0;
}

/* The samples each DOD3_PROF_TREE thread took between start_us and end_us:
 * where the time inside one slow frame went. The functions by inclusive share,
 * then the commonest leaves with their callers; a wait is named by the first
 * frame of this executable above it. */
static void slow_frame(uint64_t start_us, uint64_t end_us, double frame_ms)
{
    for (auto& kv : s_stats) {
        ThreadStat& st = kv.second;
        if (st.ring.empty()) continue;
        std::unordered_map<uint64_t, int> incl;
        std::map<std::string, int> leaves;
        int total = 0, waits = 0;
        for (const auto& smp : st.ring) {
            if (!smp.n || smp.t_us < start_us || smp.t_us > end_us) continue;
            total++;
            int first = 0;
            if (smp.wait) {
                waits++;
                while (first < smp.n && symbolize(smp.fr[first]).system) first++;
            }
            uint64_t seen[32];
            int ns = 0;
            for (int i = first; i < smp.n; i++) {
                const uint64_t b = symbolize(smp.fr[i]).base;
                if (std::find(seen, seen + ns, b) != seen + ns) continue;
                seen[ns++] = b;
                incl[b]++;
            }
            std::string key = smp.wait ? "wait in " : "";
            for (int i = first; i < smp.n && i < first + 4; i++) {
                if (i > first) key += " <- ";
                key += symbolize(smp.fr[i]).name;
            }
            leaves[key]++;
        }
        if (!total) continue;
        std::vector<std::pair<int, uint64_t>> in;
        for (auto& i : incl) in.push_back({ i.second, i.first });
        std::sort(in.rbegin(), in.rend());
        fprintf(stderr, "[slow-frame] %.0f ms: %s, %d samples (%d waiting); inclusive:", frame_ms, st.name.c_str(),
                total, waits);
        for (size_t i = 0; i < in.size() && i < 24; i++)
            fprintf(stderr, " %d%% %s;", 100 * in[i].first / total, symbolize(in[i].second).name.c_str());
        fputc('\n', stderr);
        std::vector<std::pair<int, std::string>> top;
        for (auto& l : leaves) top.push_back({ l.second, l.first });
        std::sort(top.rbegin(), top.rend());
        fprintf(stderr, "[slow-frame]   leaves:");
        for (size_t i = 0; i < top.size() && i < 8; i++)
            fprintf(stderr, " %d%% %s;", 100 * top[i].first / total, top[i].second.c_str());
        fputc('\n', stderr);
    }
}
static volatile int s_sf_pending;
static uint64_t s_sf_start, s_sf_end;
static double s_sf_ms;

/* A report cut early, asked for by another thread (dod3_prof_mark). */
static volatile int s_mark_pending;
static const char* volatile s_mark_why;

static void* prof_thread(void* arg)
{
    const long interval = (long)(intptr_t)arg;
    pthread_setname_np("dod3 profiler");
    pthread_set_qos_class_self_np(QOS_CLASS_USER_INTERACTIVE, 0);
    s_self_port = mach_thread_self();
    long report_s = 5;
    if (const char* e = getenv("DOD3_PROF_REPORT")) report_s = atol(e) > 0 ? atol(e) : 5;
    /* Not before the first frame: the window and the device are being made. */
    while (g_rsx_engine_frame == 0) usleep(50000);
    uint64_t t0 = dod3_now_us();
    for (;;) {
        usleep((useconds_t)interval * 1000u);
        refresh_threads();
        s_passes++;
        for (auto& t : s_threads) {
            ThreadStat& st = s_stats[t.id];
            if (st.name.empty() || st.name.compare(0, 4, "tid ") == 0) st.name = thread_name(t);
            /* Asleep in the kernel or running: asked before it is stopped. */
            thread_basic_info_data_t bi;
            mach_msg_type_number_t c = THREAD_BASIC_INFO_COUNT;
            const bool waiting = thread_info(t.port, THREAD_BASIC_INFO, (thread_info_t)&bi, &c) == KERN_SUCCESS &&
                                 bi.run_state != TH_STATE_RUNNING;
            const bool deep = s_tree_of && tree_match(st.name.c_str());
            uint64_t raw[64], lr = 0;
            int n = walk(t, raw, deep ? 64 : 16, &lr);
            if (!n) continue;
            /* The link register is the caller of a leaf that made no frame:
             * kept when it names a function other than the leaf's and other
             * than the first caller the chain has. */
            uint64_t fr[65];
            int m = 0;
            fr[m++] = raw[0];
            if (lr && symbolize(lr).base != symbolize(raw[0]).base && (n < 2 || symbolize(lr).base != symbolize(raw[1]).base))
                fr[m++] = lr;
            for (int i = 1; i < n; i++) fr[m++] = raw[i];
            n = m;
            if (deep) {
                if (st.ring.empty()) st.ring.resize(4096);
                ThreadStat::Sample& smp = st.ring[st.ring_pos++ % st.ring.size()];
                smp.t_us = dod3_now_us();
                smp.wait = waiting;
                smp.n = n < 32 ? n : 32;
                memcpy(smp.fr, fr, sizeof(uint64_t) * (size_t)smp.n);
            }
            const Sym& ls = symbolize(fr[0]);
            if (waiting) {
                st.waits++;
                for (int i = 1; i < n; i++) {
                    const Sym& w = symbolize(fr[i]);
                    if (w.system) continue;
                    st.wait_site[w.base]++;
                    if (deep) {
                        std::string chain;
                        for (int k = i; k < n && k < i + 8; k++) {
                            if (k > i) chain += " <- ";
                            chain += symbolize(fr[k]).name;
                        }
                        st.wait_chain[chain]++;
                    }
                    break;
                }
                continue;
            }
            if (s_callers_of && ls.name == s_callers_of) {
                std::string chain;
                for (int i = 1; i < n && i <= 3; i++) {
                    if (i > 1) chain += " <- ";
                    chain += symbolize(fr[i]).name;
                }
                s_caller_chains[chain]++;
            }
            st.samples++;
            s_total_samples++;
            st.self[ls.base]++;
            s_self_all[ls.base]++;
            uint64_t seen[65];
            int ns = 0;
            for (int i = 0; i < n; i++) {
                const uint64_t b = symbolize(fr[i]).base;
                if (std::find(seen, seen + ns, b) != seen + ns) continue;
                seen[ns++] = b;
                s_incl_all[b]++;
                if (deep) st.incl[b]++;
            }
        }
        const uint64_t now = dod3_now_us();
        const double secs = (double)(now - t0) / 1e6;
        if (__atomic_load_n(&s_sf_pending, __ATOMIC_ACQUIRE)) {
            slow_frame(s_sf_start, s_sf_end, s_sf_ms);
            __atomic_store_n(&s_sf_pending, 0, __ATOMIC_RELEASE);
        }
        if (s_mark_pending) {
            report(secs, interval, s_mark_why);
            s_mark_pending = 0;
            t0 = now;
        } else if (secs >= report_s) {
            report(secs, interval, NULL);
            t0 = now;
        }
    }
    return NULL;
}

static bool s_on;

} // namespace

/* Ends the current report window at the next pass, labelled: what was being
 * sampled up to here ("the loading screen"). A no-op without DOD3_PROF. */
extern "C" void dod3_prof_mark(const char* why)
{
    if (!s_on) return;
    s_mark_why = why;
    __atomic_store_n(&s_mark_pending, 1, __ATOMIC_RELEASE);
}

/* Explains a slow frame from the samples (DOD3_PROF_TREE threads only), on the
 * profiler's thread at its next pass: symbolising here, on the asking thread,
 * could wait on a lock a thread the sampler has stopped holds. One at a time;
 * a request while one is pending is dropped. */
extern "C" void dod3_prof_slow_frame(uint64_t start_us, uint64_t end_us, double frame_ms)
{
    if (!s_on || !s_tree_of || __atomic_load_n(&s_sf_pending, __ATOMIC_ACQUIRE)) return;
    s_sf_start = start_us;
    s_sf_end = end_us;
    s_sf_ms = frame_ms;
    __atomic_store_n(&s_sf_pending, 1, __ATOMIC_RELEASE);
}

extern "C" void mac_prof_start(void)
{
    const char* prof = getenv("DOD3_PROF");
    if (!prof) return;
    long ms = atol(prof);
    if (ms < 1) ms = 2;
    if (ms > 50) ms = 50;
    s_callers_of = getenv("DOD3_PROF_CALLERS");
    s_tree_of = getenv("DOD3_PROF_TREE");
    symtab_init();
    s_on = true;
    pthread_t th;
    pthread_attr_t at;
    pthread_attr_init(&at);
    pthread_attr_setstacksize(&at, 1u << 20);
    if (pthread_create(&th, &at, prof_thread, (void*)(intptr_t)ms) == 0) pthread_detach(th);
    pthread_attr_destroy(&at);
    fprintf(stderr, "[prof] sampling every %ld ms (%zu symbols)\n", ms, s_symtab.size());
}
#endif
