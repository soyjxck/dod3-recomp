/*
 * Native reachability pass for the title's UE3 garbage collector: a
 * hand translation of the lifted func_00EE6538 (FArchiveRealtimeGC::
 * PerformReachabilityAnalysis + ProcessObjectArray), statement for
 * statement, with the PPC condition-register and carry bookkeeping gone and
 * the guest calls (virtuals, GMalloc, the helpers it calls) made exactly
 * where the lifted code makes them. src/dod3_gc.cpp chooses it
 * (DOD3_GC_NATIVE) and can run both and compare (DOD3_GC_NATIVE=check).
 *
 * Guest layout, from the code:
 *   0x01A0C2B4  GObjObjects (data, num); 0x01A0C33C its first GC index
 *   0x019C816C  a counter of objects visited
 *   0x019907CC / 0x019907D0  the permanent-object range, never collected
 *   0x0197FFA0  GMalloc (vtable +0xC Realloc, +0x10 Free); 0 until
 *               func_008EBDD0 creates it
 *   0x00EE6528 / 0x00EE6530  64-bit flag masks (the KeepFlags OR, the
 *               unreachable mark)
 *   object +0x00 vtable (+0x30 a "keep" test, +0xFC AddReferencedObjects)
 *          +0x08 64-bit ObjectFlags (bit 61 pending kill, bit 33
 *                unreachable, bit 14 the native-reference path, bit 12)
 *          +0x34 Outer, Class at +0x34 of the current object in phase 2
 *   class  +0x150 reference token stream; token = ReturnCount:8 Type:4
 *          Offset:20
 *   ObjectsToSerialize (arg r3): +0 data, +4 num, +8 max, +0xC current
 */
#define PPU_INLINE_VM 1
#include "ppu_recomp.h"
#include "dod3_eboot.h"   /* the EBOOT version's addresses */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "dod3_cycles.h"

/* The pass is bound by memory latency: 136k object headers spread over the
 * heap, each read (and its flags written) once. The guest's own code issued
 * dcbt for the object ahead; this does the same on the host -- the header of
 * the object `ahead` slots further on, so it is in cache by the time it is
 * read. Hints only: no effect on what the pass does. DOD3_GC_PREFETCH=0 off. */
static int gc_prefetch_on(void)
{
    static int on = -1;
    if (on < 0) { const char* e = getenv("DOD3_GC_PREFETCH"); on = !(e && e[0] == '0'); }
    return on;
}
static inline void gc_prefetch_obj(uint32_t array_data, uint32_t index, uint32_t limit)
{
    if (index >= limit) return;
    extern uint8_t* vm_base;
    const uint32_t o = vm_read32(array_data + index * 4u);
    if (o) __builtin_prefetch((const void*)(vm_base + o), 0, 3);
}
#include <map>
#include <atomic>
#include <condition_variable>
#include <mutex>
#include <thread>
#include <vector>
extern "C" uint8_t* vm_base;

/* DOD3_GC_STATS=1: per collection, the cycles in the two per-object virtual
 * calls and the commonest targets of each. */
static int s_stats = -1;
static uint64_t s_cyc[2], s_calls[2];
static std::map<uint32_t, uint32_t> s_targets[2];
static void stats_report(uint64_t total)
{
    fprintf(stderr, "[gc-stats] %.1f Mcycles in all; keep-test (vtable+0x30) %llu calls %.1f Mcycles; AddReferencedObjects (vtable+0xFC) %llu calls %.1f Mcycles\n",
            total / 1e6, (unsigned long long)s_calls[0], s_cyc[0] / 1e6, (unsigned long long)s_calls[1], s_cyc[1] / 1e6);
    for (int k = 0; k < 2; k++) {
        std::multimap<uint32_t, uint32_t, std::greater<uint32_t>> by;
        for (auto& t : s_targets[k]) by.insert({ t.second, t.first });
        int n = 0;
        fprintf(stderr, "[gc-stats]   %s targets:", k ? "0xFC" : "0x30");
        for (auto& b : by) { if (n++ == 6) break; fprintf(stderr, " %08X x%u", b.second, b.first); }
        fprintf(stderr, " (%zu distinct)\n", s_targets[k].size());
        s_targets[k].clear(); s_cyc[k] = 0; s_calls[k] = 0;
    }
}

extern "C" PPU_THREAD_LOCAL void (*g_trampoline_fn)(void*);
extern "C" void ps3_indirect_call(ppu_context* ctx);

namespace {

const uint32_t GOBJ          = DOD3_A_GOBJOBJECTS;
const uint32_t GOBJ_FIRST    = DOD3_A_GOBJ_FIRST_GC_INDEX;
const uint32_t VISIT_COUNTER = DOD3_A_GC_VISIT_COUNTER;
const uint32_t PERM_START    = DOD3_A_PERM_OBJ_START;
const uint32_t PERM_END      = DOD3_A_PERM_OBJ_END;
const uint32_t GMALLOC       = DOD3_A_GMALLOC;
const uint32_t MASKS         = DOD3_GC_PC(0x00EE6528u);
const uint64_t BIT_PENDING_KILL = 1ull << 61;
const uint64_t BIT_UNREACHABLE  = 1ull << 33;

inline void drain(ppu_context* ctx)
{
    while (g_trampoline_fn) {
        void (*f)(void*) = g_trampoline_fn;
        g_trampoline_fn = 0;
        f((void*)ctx);
    }
}

/* A guest virtual: the OPD at vtable+slot, its first word the code. */
inline uint32_t vcall(ppu_context* ctx, uint32_t obj_vtable_holder, uint32_t slot, uint32_t ret_lr)
{
    const uint32_t vt = vm_read32(obj_vtable_holder);
    const uint32_t code = vm_read32(vm_read32(vt + slot));
    /* The two targets nearly every object resolves to, done here instead of
     * through the guest dispatcher (DOD3_GC_STATS: 91% of the keep tests,
     * 97% of the AddReferencedObjects calls):
     *   0x00EF8010  UObject's keep test: r3 = pending-kill (flags bit 61),
     *               r4 = that bit isolated
     *   0x00018EF0  UObject::AddReferencedObjects: its first instruction
     *               is blr, an empty function */
    if (code == DOD3_GC_PC(0x00EF8010u)) {
        const uint64_t pk = vm_read64((uint32_t)ctx->gpr[3] + 8) & (1ull << 61);
        ctx->gpr[4] = pk;
        ctx->gpr[3] = pk ? 1 : 0;
        return (uint32_t)ctx->gpr[3];
    }
    if (code == 0x00018EF0u) return (uint32_t)ctx->gpr[3];
    ctx->ctr = code;
    ctx->lr = ret_lr;
    const int k = slot == 0x30 ? 0 : slot == 0xFC ? 1 : -1;
    const uint64_t c0 = (s_stats > 0 && k >= 0) ? dod3_cycles() : 0;
    ps3_indirect_call(ctx);
    drain(ctx);
    if (c0) { s_cyc[k] += dod3_cycles() - c0; s_calls[k]++; s_targets[k][code]++; }
    return (uint32_t)ctx->gpr[3];
}

inline uint32_t gmalloc(ppu_context* ctx, uint32_t ret_lr_init)
{
    uint32_t m = vm_read32(GMALLOC);
    if (m == 0) {
        ctx->lr = ret_lr_init;
        DOD3_FN_GMALLOC_CREATE(ctx); drain(ctx);
        m = vm_read32(GMALLOC);
    }
    return m;
}

/* TArray growth (the slack rule inlined at every AddItem). */
inline int32_t grow_max(int32_t n, int32_t max)
{
    if (!(n > 0)) return 0;
    if (max == 0 && !(n > 4)) return 4;
    if (!(n > 6) && max == 0) return 6;
    const int32_t three = (int32_t)(3u * (uint32_t)n);
    int32_t g = n + three / 8 + 16;
    if (n > g) g = 0x7FFFFFFF;
    return g;
}

struct Pass {
    ppu_context* ctx;
    uint32_t list;          /* ObjectsToSerialize */
    uint32_t slot70;        /* the guest stack slot the lifted code keeps the object in */
    uint32_t perm_start, perm_end;

    /* ObjectsToSerialize.AddItem(value): num+1, grow through GMalloc,
     * store. `value_from` is re-read after any realloc (as the lifted
     * code re-reads its source). */
    void add_item(uint32_t value_addr, int from_slot70, uint32_t ret_init, uint32_t ret_realloc)
    {
        const int32_t old = (int32_t)vm_read32(list + 4);
        int32_t max = (int32_t)vm_read32(list + 8);
        const int32_t n = old + 1;
        vm_write32(list + 4, (uint32_t)n);
        uint32_t data;
        if (n > max) {
            max = grow_max(n, max);
            data = vm_read32(list + 0);
            vm_write32(list + 8, (uint32_t)max);
            if ((data | (uint32_t)max) != 0) {
                const uint32_t m = gmalloc(ctx, ret_init);
                ctx->gpr[3] = m; ctx->gpr[4] = data; ctx->gpr[5] = (uint32_t)max * 4u; ctx->gpr[6] = 8;
                data = vcall(ctx, m, 0xC, ret_realloc);
                vm_write32(list + 0, data);
            }
        } else {
            data = vm_read32(list + 0);
        }
        const uint32_t dst = data + (uint32_t)old * 4u;
        if (dst != 0) vm_write32(dst, vm_read32(from_slot70 ? slot70 : value_addr));
    }

    /* Object at *ref: null a pending-kill reference (when allowed), clear
     * the unreachable mark of a newly reached object and queue it.
     * Returns 0 if *ref was null to begin with (the delegate token cares). */
    int handle(uint32_t ref, int null_pending, uint32_t ret_init, uint32_t ret_realloc)
    {
        const uint32_t obj = vm_read32(ref);
        if (obj == 0) return 0;
        if (!(obj < perm_start) && obj < perm_end) return 1;
        uint64_t flags = vm_read64(obj + 8);
        if (null_pending && (flags & BIT_PENDING_KILL)) { vm_write32(ref, 0); return 1; }
        if (!(flags & BIT_UNREACHABLE)) return 1;
        flags &= ~BIT_UNREACHABLE;
        vm_write64(obj + 8, flags);
        add_item(ref, 0, ret_init, ret_realloc);
        return 1;
    }
};

}  // namespace

/* ---- phase 2 on several threads (DOD3_GC_PAR=<threads>) -------------------
 *
 * Phase 2 is a mark: from the objects phase 1 queued, follow every object
 * reference the class token streams describe, clear the unreachable bit of
 * each object reached for the first time and queue it. It is bound by memory
 * latency (an object's references are cache misses), so it spreads well over
 * threads. Everything in it is plain memory work except two things, which
 * stay on the game thread:
 *   - AddReferencedObjects (vtable +0xFC) is guest code for ~2% of objects
 *     (the rest resolve to the empty UObject one). Those objects' calls are
 *     deferred; when the workers run dry, the game thread makes them, one by
 *     one, as the serial pass does, and whatever they queue (the guest list
 *     grows) goes back to the workers. Workers are idle meanwhile, so guest
 *     code never runs alongside them.
 *   - ObjectsToSerialize itself (a guest TArray, grown through GMalloc): the
 *     workers keep the objects they reach in host lists, and the game thread
 *     appends them all at the end.
 * The bit is cleared with an atomic AND on its byte, so an object reached by
 * two threads at once is queued once. The result -- which objects are
 * reachable, which references to pending-kill objects are nulled -- is the
 * serial pass's; the order of the guest list is not (DOD3_GC_NATIVE=check
 * compares sets when this is on). */
namespace {

inline uint32_t g32(uint32_t a) { return __builtin_bswap32(*(const uint32_t*)(vm_base + a)); }
inline uint64_t g64(uint32_t a) { return __builtin_bswap64(*(const uint64_t*)(vm_base + a)); }
inline void p32(uint32_t a, uint32_t v) { *(uint32_t*)(vm_base + a) = __builtin_bswap32(v); }
inline void p64(uint32_t a, uint64_t v) { *(uint64_t*)(vm_base + a) = __builtin_bswap64(v); }

const uint32_t AROBJ_EMPTY = 0x00018EF0u;   /* UObject::AddReferencedObjects: blr */

struct ParShared {
    std::mutex mu;
    std::condition_variable cv_work, cv_idle;
    std::vector<uint32_t> queue;        /* objects to traverse */
    std::vector<uint32_t> deferred;     /* objects whose AddReferencedObjects is guest code */
    std::vector<uint32_t> reached;      /* every object a worker queued (for the guest list) */
    uint32_t perm_start = 0, perm_end = 0;
    int busy = 0;                       /* workers holding work */
    int generation = 0;                 /* a round */
    /* Phase 1 rounds (mode 1): chunks of the object array, claimed by index. */
    int mode = 0;
    uint32_t p1_data = 0, p1_first = 0, p1_num = 0, p1_nchunks = 0, p1_outer = 0;
    uint64_t p1_keep = 0;
    std::atomic<uint32_t> p1_next{0};
    struct P1Event { uint32_t kind, v; };   /* 0: the whole body for index v; 1: the class check for object v */
    struct P1Chunk { std::vector<uint32_t> queued; std::vector<P1Event> ev; uint32_t visits = 0; };
    std::vector<P1Chunk> p1;
    int nthreads = 0;
    bool quit = false;
    std::atomic<int> bad_token{0};
};
ParShared* s_par;

const uint32_t P1_CHUNK = 2048;
const uint32_t UOBJ_KEEP_TEST = DOD3_GC_PC(0x00EF8010u);  /* UObject's keep test: r3 = pending kill */

/* Phase 1 for one chunk of the object array: what needs no guest code, done
 * here; the rest recorded, in index order, for the game thread.
 *   - RF_RootSet (0x4000) objects: queued (func_00ECDD7C is TArray::AddItem).
 *   - The UObject keep test (91% of objects) inlined, as vcall does: pending
 *     kill -> flags |= or_mask (bit 61 itself), then mark (bit 33) or queue.
 *   - Any other keep test is guest code: event 0, the serial body later.
 *   - Objects whose class is outer_match (UClass) get event 1: the game
 *     thread re-checks token-stream-assembled (0x1000) when it gets there,
 *     after every earlier guest call, as the serial pass would see it. */
void p1_chunk(uint32_t c)
{
    ParShared& S = *s_par;
    ParShared::P1Chunk& r = S.p1[c];
    const uint32_t i0 = S.p1_first + c * P1_CHUNK;
    uint32_t i1 = i0 + P1_CHUNK; if (i1 > S.p1_num) i1 = S.p1_num;
    for (uint32_t i = i0; i < i1; i++) {
        if (i + 16u < i1) { const uint32_t a = g32(S.p1_data + (i + 16u) * 4u); if (a) __builtin_prefetch(vm_base + a, 1, 3); }
        const uint32_t obj = g32(S.p1_data + i * 4u);
        if (obj == 0) continue;
        r.visits++;
        uint64_t flags = g64(obj + 8);
        if (flags & 0x4000u) {
            r.queued.push_back(obj);
        } else {
            if (g32(g32(g32(obj) + 0x30)) != UOBJ_KEEP_TEST) { r.ev.push_back({0, i}); continue; }
            if (((flags & S.p1_keep) == 0 && S.p1_keep != ~0ull) || (flags & BIT_PENDING_KILL))
                p64(obj + 8, flags | BIT_UNREACHABLE);
            else
                r.queued.push_back(obj);
        }
        if (g32(obj + 0x34) == S.p1_outer) r.ev.push_back({1, obj});
    }
}

/* Phase 1 chunks on the workers and this thread; returns with all done. */
void par_phase1(uint32_t data, uint32_t first, uint32_t num, uint64_t keep, uint32_t outer)
{
    ParShared& S = *s_par;
    std::unique_lock<std::mutex> lk(S.mu);
    S.p1_data = data; S.p1_first = first; S.p1_num = num; S.p1_outer = outer; S.p1_keep = keep;
    S.p1_nchunks = num > first ? (num - first + P1_CHUNK - 1) / P1_CHUNK : 0;
    S.p1.clear(); S.p1.resize(S.p1_nchunks);
    S.p1_next = 0;
    S.mode = 1;
    S.generation++;
    S.cv_work.notify_all();
    lk.unlock();
    for (uint32_t c; (c = S.p1_next.fetch_add(1)) < S.p1_nchunks; ) p1_chunk(c);
    lk.lock();
    S.cv_idle.wait(lk, [&] { return S.busy == 0; });
    S.mode = 0;
}

struct Worker {
    std::vector<uint32_t> local, reached, deferred;
    uint32_t st[256][4];                /* the traversal stack: data, stride, count, return index */

    /* *ref: null it if it names a pending-kill object (when allowed), else
     * mark and queue a newly reached object. 0 if *ref was null. */
    int handle(uint32_t ref, int null_pending)
    {
        const uint32_t obj = g32(ref);
        if (obj == 0) return 0;
        if (!(obj < s_par->perm_start) && obj < s_par->perm_end) return 1;
        const uint64_t flags = g64(obj + 8);
        if (null_pending && (flags & BIT_PENDING_KILL)) { p32(ref, 0); return 1; }
        if (!(flags & BIT_UNREACHABLE)) return 1;
        /* Bit 33 of the big-endian flags: byte +11, mask 0x02. */
        const uint8_t old = __atomic_fetch_and(vm_base + obj + 11, (uint8_t)~0x02u, __ATOMIC_ACQ_REL);
        if (old & 0x02u) { local.push_back(obj); reached.push_back(obj); }
        return 1;
    }

    void traverse(uint32_t cur)
    {
        /* AddReferencedObjects: the empty one needs nothing; any other is
         * guest code, made later on the game thread. */
        {
            const uint32_t vt = g32(cur);
            const uint32_t code = g32(g32(vt + 0xFC));
            if (code != AROBJ_EMPTY) deferred.push_back(cur);
        }
        uint32_t data = cur;
        const uint32_t cls = g32(data + 0x34);
        uint32_t ti = 0;
        int32_t ret = 0;
        int sp = 0;
        st[0][0] = data; st[0][1] = 0; st[0][2] = 0xFFFFFFFFu; st[0][3] = 0xFFFFFFFFu;
        for (;;) {
            if (ret > 0) {
                for (int32_t k = 0; ; ) {
                    const int e = sp;
                    const int32_t cnt = (int32_t)st[e][2] - 1;
                    st[e][2] = (uint32_t)cnt;
                    if (cnt > 0) {
                        data = st[sp][0] + st[sp][1];
                        ti = st[sp][3];
                        st[sp][0] = data;
                        break;
                    }
                    k++;
                    sp--;
                    if (sp < 0) { s_par->bad_token = 2; return; }
                    data = st[sp][0];
                    if (!(k < ret)) break;
                }
            }
            const uint32_t tokens = g32(cls + 0x150);
            const uint32_t cur_i = ti;
            const uint32_t tok = g32(tokens + cur_i * 4u);
            ti = cur_i + 1;
            const uint32_t type = (tok >> 20) & 0xFu;
            const uint32_t off = tok & 0xFFFFFu;
            const int32_t rc = (int32_t)(tok >> 24);
            const int entry = sp;
            if ((type == 4 || type == 5 || type == 8) && sp >= 255) { s_par->bad_token = 3; return; }
            switch (type) {
            case 1: ret = rc; handle(data + off, 1); break;
            case 2: ret = rc; handle(data + off, 0); break;
            case 3: {
                const uint32_t arr = data + off;
                ret = rc;
                for (int32_t j = 0; j < (int32_t)g32(arr + 4); j++)
                    handle(g32(arr + 0) + (uint32_t)j * 4u, 1);
                break;
            }
            case 4: {
                const uint32_t arr = data + off;
                const uint32_t i1 = cur_i + 1, i2 = cur_i + 2;
                ti = cur_i + 3;
                sp++;
                data = g32(arr + 0);
                st[entry + 1][0] = data;
                st[entry + 1][1] = g32(g32(cls + 0x150) + i1 * 4u);
                const uint32_t count = g32(arr + 4);
                st[entry + 1][2] = count;
                const uint32_t skip = g32(g32(cls + 0x150) + i2 * 4u);
                st[entry + 1][3] = ti;
                const uint32_t si = (skip & 0xFF000000u) | (((skip & 0xFFFFFFu) + i2) & 0xFFFFFFu);
                if (count != 0) { ret = 0; break; }
                ti = si & 0xFFFFFFu;
                const uint32_t prev = g32(g32(cls + 0x150) + (ti - 1u) * 4u);
                ret = (int32_t)(prev >> 24) - (int32_t)(si >> 24);
                break;
            }
            case 5: {
                const uint32_t i1 = cur_i + 1, i2 = cur_i + 2;
                ti = cur_i + 3;
                st[entry + 1][0] = data;
                sp++;
                ret = 0;
                st[entry + 1][1] = g32(g32(cls + 0x150) + i1 * 4u);
                st[entry + 1][3] = ti;
                st[entry + 1][2] = g32(g32(cls + 0x150) + i2 * 4u);
                break;
            }
            case 6:
                return;
            case 7: {
                const uint32_t ref = data + off;
                ret = rc;
                if (!handle(ref, 1)) break;
                if (g32(ref) == 0) { p32(ref + 4, 0); p32(ref + 8, 0); }
                break;
            }
            case 8: {
                const uint32_t skip = g32(tokens + ti * 4u);
                uint32_t g4 = g32(cur + 0x14);           /* the current object's, as list+0xC holds it */
                const uint32_t si = (skip & 0xFF000000u) | (((skip & 0xFFFFFFu) + ti) & 0xFFFFFFu);
                ti = cur_i + 2;
                if (g4 != 0) g4 = g32(g4 + 0x1C);
                if (g4 == 0) { ti = si & 0xFFFFFFu; ret = 0; break; }
                data = g4;
                sp++;
                ret = 0;
                st[entry + 1][0] = data; st[entry + 1][1] = 0; st[entry + 1][2] = 0; st[entry + 1][3] = 0xFFFFFFFFu;
                break;
            }
            default:
                s_par->bad_token = 1;               /* the serial pass calls appErrorf here */
                return;
            }
        }
    }

    void run(void)
    {
        ParShared& S = *s_par;
        int seen_gen = 0;
        std::unique_lock<std::mutex> lk(S.mu);
        for (;;) {
            S.cv_work.wait(lk, [&] { return S.quit || (S.generation != seen_gen) || (!S.queue.empty() && seen_gen); });
            if (S.quit) return;
            seen_gen = S.generation;
            if (S.mode == 1) {
                S.busy++;
                lk.unlock();
                for (uint32_t c; (c = S.p1_next.fetch_add(1)) < S.p1_nchunks; ) p1_chunk(c);
                lk.lock();
                if (--S.busy == 0) S.cv_idle.notify_all();
                continue;
            }
            while (!S.queue.empty()) {
                /* A batch: enough to amortise the lock, few enough to share. */
                const size_t take = S.queue.size() > 64 ? 32 : 1;
                for (size_t i = 0; i < take; i++) { local.push_back(S.queue.back()); S.queue.pop_back(); }
                S.busy++;
                lk.unlock();
                while (!local.empty()) {
                    const uint32_t o = local.back(); local.pop_back();
                    traverse(o);
                    /* Share when there is plenty and someone may be idle. */
                    if (local.size() > 256) {
                        std::lock_guard<std::mutex> g(S.mu);
                        S.queue.insert(S.queue.end(), local.end() - 128, local.end());
                        local.resize(local.size() - 128);
                        S.cv_work.notify_all();
                    }
                }
                lk.lock();
                S.busy--;
                S.reached.insert(S.reached.end(), reached.begin(), reached.end()); reached.clear();
                S.deferred.insert(S.deferred.end(), deferred.begin(), deferred.end()); deferred.clear();
                if (S.queue.empty() && S.busy == 0) S.cv_idle.notify_all();
            }
        }
    }
};

void par_start(int n)
{
    if (s_par) return;
    s_par = new ParShared;
    s_par->nthreads = n;
    for (int i = 0; i < n; i++)
        std::thread([] { Worker* w = new Worker; w->run(); }).detach();
}

/* Traverse `seed` and everything it reaches; returns when the workers are
 * idle and the queue is empty. */
void par_round(const std::vector<uint32_t>& seed)
{
    ParShared& S = *s_par;
    std::unique_lock<std::mutex> lk(S.mu);
    S.queue.insert(S.queue.end(), seed.begin(), seed.end());
    S.generation++;
    S.cv_work.notify_all();
    S.cv_idle.wait(lk, [&] { return S.queue.empty() && S.busy == 0; });
}

/* DOD3_GC_PAR=<n>: phase 2 on n worker threads (default 8; 0 = the serial
 * pass). Drakengard 3, ~138k objects: the collection 11.3 -> 5.9 ms with 6,
 * 5.3 with 12 (phase 2 22 -> 6 Mcycles); phase 1, still serial, is ~2 ms. */
int gc_par_threads(void)
{
    static int n = -1;
    if (n < 0) { const char* e = getenv("DOD3_GC_PAR"); n = e ? atoi(e) : 8; if (n < 0) n = 0; if (n > 32) n = 32; }
    return n;
}

/* ObjectsToSerialize.Append(v): one growth for all of them. */
void list_append(ppu_context* ctx, uint32_t list, const std::vector<uint32_t>& v, uint32_t ret_init, uint32_t ret_realloc)
{
    if (v.empty()) return;
    const uint32_t k = (uint32_t)v.size();
    const int32_t old = (int32_t)vm_read32(list + 4);
    int32_t max = (int32_t)vm_read32(list + 8);
    const int32_t n = old + (int32_t)k;
    vm_write32(list + 4, (uint32_t)n);
    uint32_t data = vm_read32(list + 0);
    if (n > max) {
        max = grow_max(n, max);
        vm_write32(list + 8, (uint32_t)max);
        const uint32_t m = gmalloc(ctx, ret_init);
        ctx->gpr[3] = m; ctx->gpr[4] = data; ctx->gpr[5] = (uint32_t)max * 4u; ctx->gpr[6] = 8;
        data = vcall(ctx, m, 0xC, ret_realloc);
        vm_write32(list + 0, data);
    }
    if (data) for (uint32_t i = 0; i < k; i++) p32(data + ((uint32_t)old + i) * 4u, v[i]);
}

/* Phase 2 on the workers; the guest calls and the guest list on this thread.
 * Returns the list's final length (the serial pass's r3). */
uint32_t phase2_parallel(ppu_context* ctx, Pass& p, uint32_t list)
{
    par_start(gc_par_threads());
    ParShared& S = *s_par;
    S.reached.clear(); S.deferred.clear();
    std::vector<uint32_t> seed;
    uint32_t done = 0;                                  /* guest-list entries already handed out */
    uint32_t last = 0;
    for (;;) {
        S.perm_start = vm_read32(PERM_START);
        S.perm_end = vm_read32(PERM_END);
        const uint32_t num = vm_read32(list + 4), data = vm_read32(list + 0);
        seed.clear();
        for (uint32_t i = done; i < num; i++) seed.push_back(vm_read32(data + i * 4u));
        done = num;
        if (seed.empty() && S.deferred.empty()) break;
        if (!seed.empty()) { last = seed.back(); par_round(seed); }
        /* The guest AddReferencedObjects calls, in the order the workers
         * met their objects; what they queue lands in the guest list. */
        std::vector<uint32_t> calls; calls.swap(S.deferred);
        for (uint32_t cur : calls) {
            vm_write32(list + 0xC, cur);
            ctx->gpr[3] = cur; ctx->gpr[4] = list;
            vcall(ctx, cur, 0xFC, DOD3_GC_PC(0x00EE6940));
        }
    }
    /* The workers' objects into the guest list (they are queued in it in
     * the serial pass), one growth for all of them. */
    if (!S.reached.empty()) {
        list_append(ctx, list, S.reached, DOD3_GC_PC(0x00EE6B78), DOD3_GC_PC(0x00EE6B98));
        last = S.reached.back();
    }
    (void)p;
    if (S.bad_token.load()) {
        fprintf(stderr, "[gc-par] a token stream did not parse (%d): the collection's marks may be short\n", S.bad_token.load());
        S.bad_token = 0;
    }
    if (last) vm_write32(list + 0xC, last);
    return vm_read32(list + 4);
}

}  // namespace

extern "C" int dod3_gc_par_threads(void) { return gc_par_threads(); }

namespace {

/* loc_00EE6830: a UClass whose token stream is not assembled yet gets it. */
inline void phase1_class(ppu_context* ctx, uint32_t o, uint32_t outer_match)
{
    const uint32_t outer = vm_read32(o + 0x34);
    if (outer != outer_match) return;
    if (vm_read64(o + 8) & 0x1000u) return;
    uint32_t arg = 0;
    if (o != 0 && (vm_read32(outer + 0xC0) & 0x20u)) arg = o;
    ctx->gpr[3] = arg;
    ctx->lr = DOD3_GC_PC(0x00EE6878); DOD3_FN_GC_PREPARE(ctx); drain(ctx);
}

/* Phase 1 for the object at index i, as the lifted loop body; the visit
 * counter only when `visit` (the workers count the ones they hand back). */
void phase1_one(ppu_context* ctx, Pass& p, uint32_t list, uint32_t i, uint64_t or_mask, uint64_t mark_bit,
                uint64_t keep, uint32_t outer_match, int visit)
{
    const uint32_t obj = vm_read32(vm_read32(GOBJ) + i * 4u);
    vm_write32(p.slot70, obj);
    if (obj == 0) return;
    if (visit) vm_write32(VISIT_COUNTER, vm_read32(VISIT_COUNTER) + 1);
    if (vm_read64(obj + 8) & 0x4000u) {
        ctx->gpr[3] = list; ctx->gpr[4] = p.slot70;
        ctx->lr = DOD3_GC_PC(0x00EE6674); DOD3_FN_TARRAY_ADDITEM(ctx); drain(ctx);
    } else {
        ctx->gpr[3] = obj;
        const uint32_t keep_it = vcall(ctx, obj, 0x30, DOD3_GC_PC(0x00EE6690));
        if (keep_it != 0) {
            const uint32_t o = vm_read32(p.slot70);
            vm_write64(o + 8, vm_read64(o + 8) | or_mask);
        }
        const uint32_t o = vm_read32(p.slot70);
        const uint64_t f = vm_read64(o + 8);
        if (((f & keep) == 0 && keep != ~0ull) || (f & BIT_PENDING_KILL)) {
            vm_write64(o + 8, f | mark_bit);         /* unreachable until reached */
        } else {
            p.add_item(0, 1, DOD3_GC_PC(0x00EE67DC), DOD3_GC_PC(0x00EE6800));
        }
    }
    phase1_class(ctx, vm_read32(p.slot70), outer_match);
}

}  // namespace

/* The native func_00EE6538 (r3 = ObjectsToSerialize, r4 = KeepFlags). */
extern "C" void dod3_gc_reach_native(ppu_context* ctx)
{
    if (s_stats < 0) { const char* e = getenv("DOD3_GC_STATS"); s_stats = (e && *e != '0') ? 1 : 0; }
    const uint64_t t_all = s_stats > 0 ? dod3_cycles() : 0;
    const uint32_t list = (uint32_t)ctx->gpr[3];
    const uint64_t keep = ctx->gpr[4];
    const uint64_t sp0 = ctx->gpr[1];
    const uint64_t lr0 = ctx->lr;                        /* restored on return, as the epilogue */
    ctx->gpr[1] = sp0 - 0x110;
    vm_write64(ctx->gpr[1], sp0);                        /* back chain, as the prologue */
    Pass p;
    p.ctx = ctx; p.list = list; p.slot70 = (uint32_t)ctx->gpr[1] + 0x70;

    const uint64_t or_mask  = vm_read64(MASKS + 0);       /* r25 */
    const uint64_t mark_bit = vm_read64(MASKS + 8);       /* r23 */

    /* Reserve the list for the objects past the first GC index. */
    {
        const uint32_t num = vm_read32(GOBJ + 4), first = vm_read32(GOBJ_FIRST);
        vm_write32(list + 0xC, 0);
        vm_write32(VISIT_COUNTER, 0);
        ctx->gpr[3] = list;
        ctx->gpr[4] = (uint64_t)(int64_t)(int32_t)(num - first + 2);
        ctx->lr = DOD3_GC_PC(0x00EE65D4); DOD3_FN_GC_REACH_A(ctx); drain(ctx);
        ctx->lr = DOD3_GC_PC(0x00EE65D8); DOD3_FN_GC_REACH_B(ctx); drain(ctx);
    }
    const uint32_t outer_match = (uint32_t)ctx->gpr[3];  /* r31: what func_00EE6408 returned */

    /* Phase 1: every object past the first GC index. With DOD3_GC_PAR the
     * workers (and this thread) take what needs no guest code (p1_chunk);
     * the rest follows here in index order, then anything appended to the
     * object array meanwhile, as the serial loop would reach it. */
    const int pf = gc_prefetch_on();
    uint32_t i_start = vm_read32(GOBJ_FIRST);
    if (gc_par_threads() > 0) {
        par_start(gc_par_threads());
        ParShared& S = *s_par;
        const uint32_t num = vm_read32(GOBJ + 4);
        if (or_mask != BIT_PENDING_KILL || mark_bit != BIT_UNREACHABLE) {
            static int once; if (!once++) fprintf(stderr, "[gc-par] unexpected masks %llx %llx: phase 1 serial\n",
                                                  (unsigned long long)or_mask, (unsigned long long)mark_bit);
        } else {
            par_phase1(vm_read32(GOBJ), i_start, num, keep, outer_match);
            uint32_t visits = 0;
            for (auto& c : S.p1) visits += c.visits;
            vm_write32(VISIT_COUNTER, vm_read32(VISIT_COUNTER) + visits);
            for (auto& c : S.p1)
                for (const auto& e : c.ev) {
                    if (e.kind == 1) { vm_write32(p.slot70, e.v); phase1_class(ctx, e.v, outer_match); }
                    else phase1_one(ctx, p, list, e.v, or_mask, mark_bit, keep, outer_match, 0);
                }
            std::vector<uint32_t> q;
            for (auto& c : S.p1) q.insert(q.end(), c.queued.begin(), c.queued.end());
            list_append(ctx, list, q, DOD3_GC_PC(0x00EE67DC), DOD3_GC_PC(0x00EE6800));
            i_start = num;
        }
    }
    for (uint32_t i = i_start; (int32_t)i < (int32_t)vm_read32(GOBJ + 4); i++) {
        if (pf) gc_prefetch_obj(vm_read32(GOBJ), i + 16u, vm_read32(GOBJ + 4));
        phase1_one(ctx, p, list, i, or_mask, mark_bit, keep, outer_match, 1);
    }

    const uint64_t t_p2 = t_all ? dod3_cycles() : 0;
    /* Phase 2: the token streams of everything queued. */
    vm_write64(ctx->gpr[1] + 0x78, 0x80);
    uint32_t stack_buf;
    {
        const uint32_t m = gmalloc(ctx, DOD3_GC_PC(0x00EE68A0));
        ctx->gpr[3] = m; ctx->gpr[4] = 0; ctx->gpr[5] = 0xC00; ctx->gpr[6] = 8;
        stack_buf = vcall(ctx, m, 0xC, DOD3_GC_PC(0x00EE68C4));
    }
    p.perm_start = vm_read32(PERM_START);
    p.perm_end = vm_read32(PERM_END);
    uint32_t last_num = 0;
    if (gc_par_threads() > 0) {
        last_num = phase2_parallel(ctx, p, list);
        goto phase2_done;
    }
    for (int32_t idx = 0; ; ) {
        last_num = vm_read32(list + 4);
        if (!(idx < (int32_t)last_num)) break;
        if (pf) gc_prefetch_obj(vm_read32(list + 0), (uint32_t)idx + 8u, last_num);
        const uint32_t cur = vm_read32(vm_read32(list + 0) + (uint32_t)idx * 4u);
        idx++;
        vm_write32(list + 0xC, cur);
        ctx->gpr[3] = cur; ctx->gpr[4] = list;
        vcall(ctx, cur, 0xFC, DOD3_GC_PC(0x00EE6940));              /* AddReferencedObjects */

        /* The permanent range is re-read per reference in the lifted code;
         * nothing in the pass writes it, but AddReferencedObjects is guest
         * code, so refresh it once per object. */
        p.perm_start = vm_read32(PERM_START);
        p.perm_end = vm_read32(PERM_END);

        uint32_t data = vm_read32(list + 0xC);           /* r21 */
        uint32_t sp = stack_buf;                         /* r23 */
        const uint32_t cls = vm_read32(data + 0x34);     /* r17 */
        uint32_t ti = 0;                                 /* r19 */
        int32_t ret = 0;                                 /* r20 */
        vm_write32(sp + 0x0, data);
        vm_write32(sp + 0x4, 0);
        vm_write32(sp + 0x8, 0xFFFFFFFFu);
        vm_write32(sp + 0xC, 0xFFFFFFFFu);
        for (;;) {
            /* loc_00EE6960: unwind TokenReturnCount levels. */
            if (ret > 0) {
                for (int32_t k = 0; ; ) {
                    const uint32_t e = sp;
                    const int32_t cnt = (int32_t)vm_read32(e + 8) - 1;
                    vm_write32(e + 8, (uint32_t)cnt);
                    if (cnt > 0) {
                        data = vm_read32(sp + 0) + vm_read32(sp + 4);
                        ti = vm_read32(sp + 0xC);
                        vm_write32(sp + 0, data);
                        break;
                    }
                    k++;
                    sp -= 0x10;
                    data = vm_read32(e - 0x10);
                    if (!(k < ret)) break;
                }
            }
            /* loc_00EE69C4 */
            const uint32_t tokens = vm_read32(cls + 0x150);
            const uint32_t cur_i = ti;
            const uint32_t tok = vm_read32(tokens + cur_i * 4u);
            ti = cur_i + 1;
            const uint32_t type = (tok >> 20) & 0xFu;
            const uint32_t off = tok & 0xFFFFFu;
            const int32_t rc = (int32_t)(tok >> 24);
            const uint32_t entry = sp;                   /* g7 */
            switch (type) {
            case 1:                                      /* object */
                ret = rc;
                p.handle(data + off, 1, DOD3_GC_PC(0x00EE6B78), DOD3_GC_PC(0x00EE6B98));
                break;
            case 2:                                      /* persistent object */
                ret = rc;
                p.handle(data + off, 0, DOD3_GC_PC(0x00EE6FD0), DOD3_GC_PC(0x00EE6FF0));
                break;
            case 3: {                                    /* array of objects */
                const uint32_t arr = data + off;
                ret = rc;
                for (int32_t j = 0; j < (int32_t)vm_read32(arr + 4); j++)
                    p.handle(vm_read32(arr + 0) + (uint32_t)j * 4u, 1, DOD3_GC_PC(0x00EE6D64), DOD3_GC_PC(0x00EE6D84));
                break;
            }
            case 4: {                                    /* array of structs */
                const uint32_t arr = data + off;
                const uint32_t i1 = cur_i + 1, i2 = cur_i + 2;
                ti = cur_i + 3;
                sp += 0x10;
                data = vm_read32(arr + 0);
                vm_write32(entry + 0x10, data);
                vm_write32(entry + 0x14, vm_read32(vm_read32(cls + 0x150) + i1 * 4u));
                const uint32_t count = vm_read32(arr + 4);
                vm_write32(entry + 0x18, count);
                const uint32_t skip = vm_read32(vm_read32(cls + 0x150) + i2 * 4u);
                vm_write32(entry + 0x1C, ti);
                const uint32_t si = (skip & 0xFF000000u) | (((skip & 0xFFFFFFu) + i2) & 0xFFFFFFu);
                if (count != 0) { ret = 0; break; }
                ti = si & 0xFFFFFFu;
                const uint32_t prev = vm_read32(vm_read32(cls + 0x150) + (ti - 1u) * 4u);
                ret = (int32_t)(prev >> 24) - (int32_t)(si >> 24);
                break;
            }
            case 5: {                                    /* fixed array */
                const uint32_t i1 = cur_i + 1, i2 = cur_i + 2;
                ti = cur_i + 3;
                vm_write32(entry + 0x10, data);
                sp += 0x10;
                ret = 0;
                vm_write32(entry + 0x14, vm_read32(vm_read32(cls + 0x150) + i1 * 4u));
                vm_write32(entry + 0x1C, ti);
                vm_write32(entry + 0x18, vm_read32(vm_read32(cls + 0x150) + i2 * 4u));
                break;
            }
            case 6:                                      /* end of stream */
                goto next_object;
            case 7: {                                    /* delegate: object, then its name */
                const uint32_t ref = data + off;
                ret = rc;
                if (!p.handle(ref, 1, DOD3_GC_PC(0x00EE7204), DOD3_GC_PC(0x00EE7224))) break;
                if (vm_read32(ref) == 0) { vm_write32(ref + 4, 0); vm_write32(ref + 8, 0); }
                break;
            }
            case 8: {                                    /* conditional: the current object's +0x14 chain */
                const uint32_t skip = vm_read32(tokens + ti * 4u);
                uint32_t g4 = vm_read32(vm_read32(list + 0xC) + 0x14);
                const uint32_t si = (skip & 0xFF000000u) | (((skip & 0xFFFFFFu) + ti) & 0xFFFFFFu);
                ti = cur_i + 2;
                if (g4 != 0) g4 = vm_read32(g4 + 0x1C);
                if (g4 == 0) { ti = si & 0xFFFFFFu; ret = 0; break; }
                data = g4;
                sp += 0x10;
                ret = 0;
                vm_write32(entry + 0x10, data);
                vm_write32(entry + 0x14, 0);
                vm_write32(entry + 0x18, 0);
                vm_write32(entry + 0x1C, 0xFFFFFFFFu);
                break;
            }
            default:                                     /* appErrorf: unknown token */
                ctx->gpr[3] = vm_read32(DOD3_A_GERROR);
                ctx->gpr[4] = DOD3_A_GC_ERROR_FMT;      /* 1.00: 0x01640000 - 31028 */
                ctx->lr = DOD3_GC_PC(0x00EE72FC); DOD3_FN_APP_ERRORF(ctx); drain(ctx);
                break;
            }
        }
    next_object:;
    }
phase2_done:
    ctx->gpr[3] = last_num;
    if (stack_buf != 0) {
        const uint32_t m = gmalloc(ctx, DOD3_GC_PC(0x00EE731C));
        ctx->gpr[3] = m; ctx->gpr[4] = stack_buf;
        vcall(ctx, m, 0x10, DOD3_GC_PC(0x00EE7338));                 /* Free */
    }
    ctx->gpr[1] = sp0;
    ctx->lr = lr0;
    if (t_all) { const uint64_t t_end = dod3_cycles();
        fprintf(stderr, "[gc-stats] phase 1 %.1f Mcycles, phase 2 %.1f Mcycles\n", (t_p2 - t_all) / 1e6, (t_end - t_p2) / 1e6);
        stats_report(t_end - t_all); }
}
