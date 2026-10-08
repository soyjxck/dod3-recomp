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

const uint32_t GOBJ          = 0x01A0C2B4u;
const uint32_t GOBJ_FIRST    = 0x01A0C33Cu;
const uint32_t VISIT_COUNTER = 0x019C816Cu;
const uint32_t PERM_START    = 0x019907CCu;
const uint32_t PERM_END      = 0x019907D0u;
const uint32_t GMALLOC       = 0x0197FFA0u;
const uint32_t MASKS         = 0x00EE6528u;
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
    if (code == 0x00EF8010u) {
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
        func_008EBDD0(ctx); drain(ctx);
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
        ctx->lr = 0x00EE65D4; func_00EE6360(ctx); drain(ctx);
        ctx->lr = 0x00EE65D8; func_00EE6408(ctx); drain(ctx);
    }
    const uint32_t outer_match = (uint32_t)ctx->gpr[3];  /* r31: what func_00EE6408 returned */

    /* Phase 1: every object past the first GC index. */
    const int pf = gc_prefetch_on();
    for (int32_t i = (int32_t)vm_read32(GOBJ_FIRST); i < (int32_t)vm_read32(GOBJ + 4); i++) {
        if (pf) gc_prefetch_obj(vm_read32(GOBJ), (uint32_t)i + 16u, vm_read32(GOBJ + 4));
        const uint32_t obj = vm_read32(vm_read32(GOBJ) + (uint32_t)i * 4u);
        vm_write32(p.slot70, obj);
        if (obj == 0) continue;
        vm_write32(VISIT_COUNTER, vm_read32(VISIT_COUNTER) + 1);
        if (vm_read64(obj + 8) & 0x4000u) {
            ctx->gpr[3] = list; ctx->gpr[4] = p.slot70;
            ctx->lr = 0x00EE6674; func_00ECDD7C(ctx); drain(ctx);
        } else {
            ctx->gpr[3] = obj;
            const uint32_t keep_it = vcall(ctx, obj, 0x30, 0x00EE6690);
            if (keep_it != 0) {
                const uint32_t o = vm_read32(p.slot70);
                vm_write64(o + 8, vm_read64(o + 8) | or_mask);
            }
            const uint32_t o = vm_read32(p.slot70);
            const uint64_t f = vm_read64(o + 8);
            if (((f & keep) == 0 && keep != ~0ull) || (f & BIT_PENDING_KILL)) {
                vm_write64(o + 8, f | mark_bit);         /* unreachable until reached */
            } else {
                p.add_item(0, 1, 0x00EE67DC, 0x00EE6800);
            }
        }
        /* loc_00EE6830 */
        const uint32_t o = vm_read32(p.slot70);
        const uint32_t outer = vm_read32(o + 0x34);
        if (outer != outer_match) continue;
        if (vm_read64(o + 8) & 0x1000u) continue;
        uint32_t arg = 0;
        if (o != 0 && (vm_read32(outer + 0xC0) & 0x20u)) arg = o;
        ctx->gpr[3] = arg;
        ctx->lr = 0x00EE6878; func_000C1D48(ctx); drain(ctx);
    }

    const uint64_t t_p2 = t_all ? dod3_cycles() : 0;
    /* Phase 2: the token streams of everything queued. */
    vm_write64(ctx->gpr[1] + 0x78, 0x80);
    uint32_t stack_buf;
    {
        const uint32_t m = gmalloc(ctx, 0x00EE68A0);
        ctx->gpr[3] = m; ctx->gpr[4] = 0; ctx->gpr[5] = 0xC00; ctx->gpr[6] = 8;
        stack_buf = vcall(ctx, m, 0xC, 0x00EE68C4);
    }
    p.perm_start = vm_read32(PERM_START);
    p.perm_end = vm_read32(PERM_END);
    uint32_t last_num = 0;
    for (int32_t idx = 0; ; ) {
        last_num = vm_read32(list + 4);
        if (!(idx < (int32_t)last_num)) break;
        if (pf) gc_prefetch_obj(vm_read32(list + 0), (uint32_t)idx + 8u, last_num);
        const uint32_t cur = vm_read32(vm_read32(list + 0) + (uint32_t)idx * 4u);
        idx++;
        vm_write32(list + 0xC, cur);
        ctx->gpr[3] = cur; ctx->gpr[4] = list;
        vcall(ctx, cur, 0xFC, 0x00EE6940);              /* AddReferencedObjects */

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
                p.handle(data + off, 1, 0x00EE6B78, 0x00EE6B98);
                break;
            case 2:                                      /* persistent object */
                ret = rc;
                p.handle(data + off, 0, 0x00EE6FD0, 0x00EE6FF0);
                break;
            case 3: {                                    /* array of objects */
                const uint32_t arr = data + off;
                ret = rc;
                for (int32_t j = 0; j < (int32_t)vm_read32(arr + 4); j++)
                    p.handle(vm_read32(arr + 0) + (uint32_t)j * 4u, 1, 0x00EE6D64, 0x00EE6D84);
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
                if (!p.handle(ref, 1, 0x00EE7204, 0x00EE7224)) break;
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
                ctx->gpr[3] = vm_read32(0x0197FF84u);
                ctx->gpr[4] = 0x016386CCu;              /* 0x01640000 - 31028 */
                ctx->lr = 0x00EE72FC; func_00012538(ctx); drain(ctx);
                break;
            }
        }
    next_object:;
    }

    ctx->gpr[3] = last_num;
    if (stack_buf != 0) {
        const uint32_t m = gmalloc(ctx, 0x00EE731C);
        ctx->gpr[3] = m; ctx->gpr[4] = stack_buf;
        vcall(ctx, m, 0x10, 0x00EE7338);                 /* Free */
    }
    ctx->gpr[1] = sp0;
    ctx->lr = lr0;
    if (t_all) { const uint64_t t_end = dod3_cycles();
        fprintf(stderr, "[gc-stats] phase 1 %.1f Mcycles, phase 2 %.1f Mcycles\n", (t_p2 - t_all) / 1e6, (t_end - t_p2) / 1e6);
        stats_report(t_end - t_all); }
}
