/*
 * The port's queue routines (sre.h) against libsre's own, in a build that
 * holds both: cmake -DDOD3_FW_LIBSRE=ON, which needs a decrypted libsre.prx
 * and has tools/gen_libsre.py translate the routines from it. Such a build
 * holds firmware code and is not for release (the packaging scripts refuse
 * it); a normal build has none of this file.
 *
 *   DOD3_LIBSRE_CHECK=<n>  when the title makes its first queue, each routine
 *                          is run n times (1: 200,000) on a made-up queue,
 *                          once as libsre has it and once as the port does,
 *                          from the same bytes: the value returned, the
 *                          queue's 128 bytes, its buffer and the item
 *                          afterwards, and every signal sent -- to whom, in
 *                          what order -- must be the same. One line a
 *                          routine; the first differences in full.
 *   DOD3_LIBSRE=fw         the title's own calls go to libsre's routines, as
 *                          they did up to 0.6.0, for a comparison in play.
 *
 * The made-up queues are ones the routines can meet -- pointers in order on
 * their ring, counters within their range -- with every field that should
 * not matter filled at random, and some that break a rule (a wrong
 * direction, a queue held by another caller, a null or unaligned argument).
 * Left out: what would have libsre wait (the blocking forms, and a
 * PPU-to-SPU LFQueue whose waiting poppers cannot be woken yet, where it
 * spins until another thread moves the queue).
 */
#include "sre.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <vector>

extern "C" int dod3_libsre_call(uint32_t addr, ppu_context* ctx);   /* generated (tools/gen_libsre.py) */
extern "C" uint8_t* vm_base;

namespace sre {
void (*g_first_queue)(ppu_context*);
Firmware* g_firmware;
}  // namespace sre

namespace {

enum : uint32_t {
    FW_LFQ_GET_PUSH_POINTER = 0x010024F4,
    FW_LFQ_COMPLETE_PUSH_POINTER = 0x01002708,
    FW_LFQ_GET_PUSH_POINTER2 = 0x010030B8,
    FW_LFQ_COMPLETE_PUSH_POINTER2 = 0x010035C8,
    FW_QUEUE_INITIALIZE = 0x010164C0,
    FW_QUEUE_PUSH_BODY = 0x010169D0,
    FW_QUEUE_POP_BODY = 0x01016CFC,
};

/* A lifted libsre routine called as if from guest code, on a frame of its
 * own below `frame`; LR, TOC and the stack pointer put back afterwards. */
uint32_t fw_call(ppu_context* ctx, uint32_t fn, uint32_t frame, const uint64_t* args, int nargs)
{
    const uint64_t lr = ctx->lr, r2 = ctx->gpr[2], sp = ctx->gpr[1];
    vm_write64(frame, sp);
    ctx->gpr[1] = frame;
    for (int i = 0; i < nargs; i++) ctx->gpr[3 + i] = args[i];
    dod3_libsre_call(fn, ctx);
    ctx->lr = lr;
    ctx->gpr[2] = r2;
    ctx->gpr[1] = sp;
    return (uint32_t)ctx->gpr[3];
}

uint32_t frame_below(ppu_context* ctx, uint32_t by) { return ((uint32_t)ctx->gpr[1] - by) & ~0xFu; }

/* ---- DOD3_LIBSRE=fw: the title's calls on libsre's routines ---- */

uint32_t fw_queue_initialize(ppu_context* ctx, uint32_t spurs, uint32_t taskset, uint32_t q, uint32_t buffer,
                             uint32_t size, uint32_t depth, uint32_t direction)
{
    const uint64_t a[7] = { spurs, taskset, q, buffer, size, depth, direction };
    return fw_call(ctx, FW_QUEUE_INITIALIZE, frame_below(ctx, 0x80), a, 7);
}
uint32_t fw_queue_push(ppu_context* ctx, uint32_t q, uint32_t item)
{
    const uint64_t a[3] = { q, item, 0 };
    return fw_call(ctx, FW_QUEUE_PUSH_BODY, frame_below(ctx, 0x80), a, 3);
}
uint32_t fw_queue_pop(ppu_context* ctx, uint32_t q, uint32_t item, bool peek)
{
    const uint64_t a[4] = { q, item, peek ? 1u : 0u, 0 };
    return fw_call(ctx, FW_QUEUE_POP_BODY, frame_below(ctx, 0x80), a, 4);
}
/* the out-parameter in a frame of its own: back chain at +0, `pointer` at +0x70 */
uint32_t fw_get(ppu_context* ctx, uint32_t fn, uint32_t q, int32_t* pointer)
{
    const uint32_t frame = frame_below(ctx, 0x80);
    vm_write32(frame + 0x70, (uint32_t)*pointer);
    const uint64_t a[4] = { q, frame + 0x70, 0, 0 };
    const uint32_t rc = fw_call(ctx, fn, frame, a, 4);
    *pointer = (int32_t)vm_read32(frame + 0x70);
    return rc;
}
/* fpSendSignal: the lifted code only reads it as an OPD on its way to the
 * call, which tools/gen_libsre.py has rewritten, and refuses a null one; the
 * queue's eaSignal field is a readable address that is not null. */
uint32_t fw_complete(ppu_context* ctx, uint32_t fn, uint32_t q, int32_t pointer)
{
    const uint64_t a[3] = { q, (uint64_t)(int64_t)pointer, q + 0x70 };
    return fw_call(ctx, fn, frame_below(ctx, 0x80), a, 3);
}
uint32_t fw_lfq_get_push_pointer(ppu_context* ctx, uint32_t q, int32_t* p) { return fw_get(ctx, FW_LFQ_GET_PUSH_POINTER, q, p); }
uint32_t fw_lfq_get_push_pointer2(ppu_context* ctx, uint32_t q, int32_t* p) { return fw_get(ctx, FW_LFQ_GET_PUSH_POINTER2, q, p); }
uint32_t fw_lfq_complete_push_pointer(ppu_context* ctx, uint32_t q, int32_t p)
{
    return fw_complete(ctx, FW_LFQ_COMPLETE_PUSH_POINTER, q, p);
}
uint32_t fw_lfq_complete_push_pointer2(ppu_context* ctx, uint32_t q, int32_t p)
{
    return fw_complete(ctx, FW_LFQ_COMPLETE_PUSH_POINTER2, q, p);
}

sre::Firmware s_firmware = { fw_queue_initialize,     fw_queue_push,                fw_queue_pop,
                             fw_lfq_get_push_pointer, fw_lfq_complete_push_pointer, fw_lfq_get_push_pointer2,
                             fw_lfq_complete_push_pointer2 };

/* ---- DOD3_LIBSRE_CHECK ---- */

struct Rng {
    uint64_t s;
    uint32_t next()
    {
        s ^= s << 13;
        s ^= s >> 7;
        s ^= s << 17;
        return (uint32_t)(s >> 16);
    }
    uint32_t below(uint32_t n) { return next() % n; }
    bool one_in(uint32_t n) { return below(n) == 0; }
};

struct Event {
    int kind;
    uint32_t a, b, answer;
    bool operator==(const Event& o) const { return kind == o.kind && a == o.a && b == o.b && answer == o.answer; }
};
struct Run {
    uint32_t rc;
    int32_t pointer;
    std::vector<uint8_t> mem;
    std::vector<Event> events;
    bool failed;
};
Run* s_run;
Rng s_answers;

uint32_t probe_signal(int kind, uint32_t a, uint32_t b)
{
    uint32_t answer = 0;
    if (kind == 1)
        answer = 0x00C0FF80u;   /* the taskset of a workload */
    else if (s_answers.one_in(6))
        answer = s_answers.one_in(2) ? sre::SPURS_TASK_ERROR_INVAL : sre::SPURS_TASK_ERROR_STAT;
    s_run->events.push_back({ kind, a, b, answer });
    return answer;
}
void probe_failed(const char*) { s_run->failed = true; }
sre::Probe s_probe = { probe_signal, probe_failed };

/* The guest memory a test uses, below the caller's stack: the queue (128
 * bytes, 128-aligned), its buffer, an item, a taskset; libsre's frames go
 * below all of it. */
struct Area {
    uint32_t q, buffer, item, taskset, frame, lo, size;
};
Area area_below(ppu_context* ctx)
{
    Area a;
    a.lo = (((uint32_t)ctx->gpr[1] - 0x1000) & ~0x7Fu) - 0x800;
    a.size = 0x800;
    a.q = a.lo + 0x80;          /* a spare line on each side */
    a.taskset = a.lo + 0x180;
    a.buffer = a.lo + 0x280;    /* up to 16 items of 32 bytes */
    a.item = a.lo + 0x500;
    a.frame = a.lo - 0x100;
    return a;
}

void put16(uint32_t ea, uint32_t v) { vm_write16(ea, (uint16_t)v); }

/* Pointers of a ring 2 x depth long, `n` apart. */
uint32_t ring(uint32_t from, uint32_t n, uint32_t depth) { return (from + n) % (2 * depth); }

/* Three counters modulo 30 in a waiters' word: `served` behind `woken` by
 * `owed`, `woken` behind `entered` by `waiting`. */
uint32_t counters(Rng& r, uint32_t owed, uint32_t waiting)
{
    const uint32_t b = r.below(30), c = (b + owed) % 30, a = (c + waiting) % 30;
    return a | (b << 5) | (c << 10) | (r.one_in(2) ? 0x8000u : 0);
}

struct Test {
    const char* name;
    void (*make)(Rng& r, const Area& a, uint64_t args[8]);
    uint32_t (*fw)(ppu_context* ctx, const Area& a, const uint64_t args[8], int32_t* pointer);
    uint32_t (*ours)(ppu_context* ctx, const uint64_t args[8], int32_t* pointer);
};

void fill(Rng& r, const Area& a)
{
    for (uint32_t i = 0; i < a.size; i += 4) vm_write32(a.lo + i, r.next());
}

/* -- cellSpursQueue -- */
void make_queue_init(Rng& r, const Area& a, uint64_t g[8])
{
    fill(r, a);
    vm_write32(a.taskset + 0x74, r.one_in(8) ? 0x20 : r.below(0x20));
    g[0] = r.one_in(4) ? 0 : r.one_in(10) ? a.lo + 4 : a.lo;
    g[1] = r.one_in(3) ? 0 : r.one_in(10) ? a.taskset + 8 : a.taskset;
    g[2] = r.one_in(12) ? 0 : r.one_in(10) ? a.q + 4 : a.q;
    g[3] = r.one_in(12) ? 0 : r.one_in(10) ? a.buffer + 4 : a.buffer;
    static const uint32_t sizes[] = { 16, 32, 0, 24, 0x4000, 0x4010 }, depths[] = { 1, 5, 128, 0, 0x40000000u, 0x3FFFFFFFu };
    g[4] = sizes[r.one_in(4) ? r.below(6) : r.below(2)];
    g[5] = depths[r.one_in(4) ? r.below(6) : r.below(3)];
    g[6] = r.below(4);
}
void make_queue(Rng& r, const Area& a, uint64_t g[8], uint32_t direction, uint32_t waiter)
{
    fill(r, a);
    const uint32_t depth = 1 + r.below(6), size = r.one_in(2) ? 16 : 32;
    uint32_t pop = r.below(2 * depth), push = ring(pop, r.below(depth + 1), depth);
    if (r.one_in(25)) push = r.below(3 * depth), pop = r.below(3 * depth);   /* out of order */
    if (r.one_in(8)) pop = ~pop;
    if (r.one_in(8)) push = ~push;
    vm_write32(a.q + 0x00, pop);
    vm_write32(a.q + 0x04, push);
    vm_write32(a.q + 0x08, size);
    vm_write32(a.q + 0x0C, depth);
    vm_write64(a.q + 0x10, a.buffer);
    vm_write32(a.q + 0x1C, r.one_in(8) ? r.below(4) : direction);
    vm_write8(a.q + waiter, r.one_in(2) ? 0 : (uint8_t)(1 + r.below(255)));
    vm_write64(a.q + 0x60, r.one_in(3) ? 0 : 0x00D00D80u);
    g[0] = r.one_in(40) ? 0 : r.one_in(40) ? a.q + 8 : a.q;
    g[1] = r.one_in(40) ? 0 : a.item;
    g[2] = r.below(2);   /* a pop: only look */
}
void make_queue_push(Rng& r, const Area& a, uint64_t g[8]) { make_queue(r, a, g, 2, 0x20); }
void make_queue_pop(Rng& r, const Area& a, uint64_t g[8]) { make_queue(r, a, g, 1, 0x30); }

/* -- cellSyncLFQueue -- */
/* A line whose pointers are in order: popped <= popping <= pushed <= pushing,
 * within `depth` of each other, the pushes complete out of order marked. */
void make_line(Rng& r, const Area& a, uint64_t g[8], uint32_t direction, bool any2any)
{
    fill(r, a);
    const uint32_t depth = 1 + r.below(any2any ? 40 : 24);
    const uint32_t total = r.below(depth + 1);                 /* popped .. pushing */
    const uint32_t flight = r.below((total < 18 ? total : 18) + 1);   /* pushed .. pushing: 16 have a mark */
    const uint32_t popping = r.below(total - flight + 1);      /* popped .. popping */
    const uint32_t h1 = r.below(2 * depth), h4 = ring(h1, popping, depth), h5 = ring(h1, total - flight, depth),
                   h8 = ring(h1, total, depth);
    uint32_t marks = 0, pointer = h5;
    if (flight) {
        const uint32_t mine = r.below(flight);                 /* the push that completes now */
        for (uint32_t k = 1; k < flight && k < 16; k++)
            if (k != mine && r.one_in(2)) marks |= 0x8000u >> k;
        pointer = ring(h5, mine, depth);
    }
    if (r.one_in(30)) pointer = r.below(2 * depth);            /* not one in flight */
    put16(a.q + 0x00, h1);
    put16(a.q + 0x06, h4);
    put16(a.q + 0x08, h5);
    put16(a.q + 0x0A, marks);
    put16(a.q + 0x0E, h8);
    vm_write32(a.q + 0x14, depth);
    vm_write32(a.q + 0x24, r.one_in(10) ? r.below(4) : direction);
    /* the event ports PPU waiters are woken through: none, or one lv2 does not have */
    vm_write32(a.q + 0x28, r.one_in(4) ? 0x7FF00000u + r.below(256) : 0xFFFFFFFFu);
    if (any2any) {
        put16(a.q + 0x04, counters(r, r.below(6), r.one_in(2) ? 0 : r.below(4)));   /* m_h3 */
        put16(a.q + 0x0C, counters(r, r.below(6), r.one_in(2) ? 0 : r.below(4)));   /* m_h7 */
        put16(a.q + 0x30, r.one_in(4) ? 0x7FF0 : 0xFFFF);
        put16(a.q + 0x50, r.one_in(4) ? r.below(0x10000) : 0xFFFF);
    } else {
        put16(a.q + 0x0C, r.one_in(20) ? 3 : 0);               /* m_h7: a blocked pusher's mark */
        put16(a.q + 0x30, counters(r, r.below(4), r.one_in(2) ? 0 : r.below(4)));
    }
    for (uint32_t k = 0; k < 15; k++) {
        if (r.one_in(5)) put16(a.q + 0x32 + 2 * k, 0xFFFF);
        if (r.one_in(5)) put16(a.q + 0x52 + 2 * k, 0xFFFF);
    }
    g[0] = a.q;
    g[1] = pointer;
}
void make_line_ppu2spu(Rng& r, const Area& a, uint64_t g[8]) { make_line(r, a, g, 2, false); }
void make_line_any2any(Rng& r, const Area& a, uint64_t g[8]) { make_line(r, a, g, 3, true); }

const Test kTests[] = {
    { "_cellSpursQueueInitialize", make_queue_init,
      [](ppu_context* ctx, const Area& a, const uint64_t g[8], int32_t*) { return fw_call(ctx, FW_QUEUE_INITIALIZE, a.frame, g, 7); },
      [](ppu_context*, const uint64_t g[8], int32_t*) {
          return sre::queue_initialize((uint32_t)g[0], (uint32_t)g[1], (uint32_t)g[2], (uint32_t)g[3], (uint32_t)g[4],
                                       (uint32_t)g[5], (uint32_t)g[6]);
      } },
    { "cellSpursQueuePushBody", make_queue_push,
      [](ppu_context* ctx, const Area& a, const uint64_t g[8], int32_t*) {
          const uint64_t c[3] = { g[0], g[1], 0 };
          return fw_call(ctx, FW_QUEUE_PUSH_BODY, a.frame, c, 3);
      },
      [](ppu_context* ctx, const uint64_t g[8], int32_t*) { return sre::queue_push(ctx, (uint32_t)g[0], (uint32_t)g[1]); } },
    { "cellSpursQueuePopBody", make_queue_pop,
      [](ppu_context* ctx, const Area& a, const uint64_t g[8], int32_t*) {
          const uint64_t c[4] = { g[0], g[1], g[2], 0 };
          return fw_call(ctx, FW_QUEUE_POP_BODY, a.frame, c, 4);
      },
      [](ppu_context* ctx, const uint64_t g[8], int32_t*) {
          return sre::queue_pop(ctx, (uint32_t)g[0], (uint32_t)g[1], g[2] != 0);
      } },
    { "_cellSyncLFQueueGetPushPointer", make_line_ppu2spu,
      [](ppu_context* ctx, const Area& a, const uint64_t g[8], int32_t* p) {
          vm_write32(a.frame + 0x70, (uint32_t)*p);
          const uint64_t c[4] = { g[0], a.frame + 0x70, 0, 0 };
          const uint32_t rc = fw_call(ctx, FW_LFQ_GET_PUSH_POINTER, a.frame - 0x80, c, 4);
          *p = (int32_t)vm_read32(a.frame + 0x70);
          return rc;
      },
      [](ppu_context* ctx, const uint64_t g[8], int32_t* p) { return sre::lfq_get_push_pointer(ctx, (uint32_t)g[0], p); } },
    { "_cellSyncLFQueueCompletePushPointer", make_line_ppu2spu,
      [](ppu_context* ctx, const Area& a, const uint64_t g[8], int32_t*) {
          const uint64_t c[3] = { g[0], g[1], g[0] + 0x70 };
          return fw_call(ctx, FW_LFQ_COMPLETE_PUSH_POINTER, a.frame, c, 3);
      },
      [](ppu_context* ctx, const uint64_t g[8], int32_t*) {
          return sre::lfq_complete_push_pointer(ctx, (uint32_t)g[0], (int32_t)g[1]);
      } },
    { "_cellSyncLFQueueGetPushPointer2", make_line_any2any,
      [](ppu_context* ctx, const Area& a, const uint64_t g[8], int32_t* p) {
          vm_write32(a.frame + 0x70, (uint32_t)*p);
          const uint64_t c[4] = { g[0], a.frame + 0x70, 0, 0 };
          const uint32_t rc = fw_call(ctx, FW_LFQ_GET_PUSH_POINTER2, a.frame - 0x80, c, 4);
          *p = (int32_t)vm_read32(a.frame + 0x70);
          return rc;
      },
      [](ppu_context* ctx, const uint64_t g[8], int32_t* p) { return sre::lfq_get_push_pointer2(ctx, (uint32_t)g[0], p); } },
    { "_cellSyncLFQueueCompletePushPointer2", make_line_any2any,
      [](ppu_context* ctx, const Area& a, const uint64_t g[8], int32_t*) {
          const uint64_t c[3] = { g[0], g[1], g[0] + 0x70 };
          return fw_call(ctx, FW_LFQ_COMPLETE_PUSH_POINTER2, a.frame, c, 3);
      },
      [](ppu_context* ctx, const uint64_t g[8], int32_t*) {
          return sre::lfq_complete_push_pointer2(ctx, (uint32_t)g[0], (int32_t)g[1]);
      } },
};

/* A PPU-to-SPU line the completing push would spin on: poppers wait, and the
 * pops in flight leave no mark to wake one with. */
bool would_spin(const Test& t, const Area& a)
{
    if (t.fw == kTests[4].fw) {
        const uint32_t w = vm_read16(a.q + 0x30), depth = vm_read32(a.q + 0x14);
        const int32_t h1 = vm_read16(a.q), h4 = (int16_t)vm_read16(a.q + 6);
        const uint32_t in_flight = h4 >= h1 ? (uint32_t)(h4 - h1) : ((uint32_t)h4 + depth) - ((uint32_t)h1 - depth);
        const uint32_t c = (w >> 10) & 0x1F, b = (w >> 5) & 0x1F;
        return (w & 0x1F) != c && (int32_t)(in_flight + (c >= b ? c - b : c - b + 30)) > 15;
    }
    /* ... or the taking push would wait behind a blocked pusher */
    return t.fw == kTests[3].fw && vm_read16(a.q + 0x0C) != 0;
}

void dump(const char* who, const Run& r, const Area& a)
{
    fprintf(stderr, "    %s: returned %08X, pointer %d%s, %zu signals", who, r.rc, r.pointer, r.failed ? ", a check failed" : "",
            r.events.size());
    for (const Event& e : r.events) fprintf(stderr, " [%d %08X %08X -> %08X]", e.kind, e.a, e.b, e.answer);
    fprintf(stderr, "\n      queue:");
    for (uint32_t i = 0; i < 128; i++) fprintf(stderr, "%s%02X", i % 16 == 0 ? " " : "", r.mem[(a.q - a.lo) + i]);
    fprintf(stderr, "\n");
}

void self_check(ppu_context* ctx, unsigned n)
{
    const Area a = area_below(ctx);
    const uint64_t reserve_addr = ctx->reserve_addr, reserve_value = ctx->reserve_value;
    std::vector<uint8_t> before(a.size), keep(a.size);
    memcpy(keep.data(), vm_base + a.lo, a.size);
    uint64_t saved[32];
    memcpy(saved, ctx->gpr, sizeof saved);
    sre::g_probe = &s_probe;
    unsigned total_bad = 0;
    for (const Test& t : kTests) {
        Rng r = { 0x9E3779B97F4A7C15ull ^ (uint64_t)(&t - kTests) };
        unsigned bad = 0, skipped = 0, signals = 0, errors = 0, checks = 0;
        for (unsigned i = 0; i < n; i++) {
            uint64_t g[8] = {};
            t.make(r, a, g);
            if (would_spin(t, a)) {
                skipped++;
                continue;
            }
            memcpy(before.data(), vm_base + a.lo, a.size);
            const uint64_t seed = r.next() | 1;
            Run run[2];
            for (int k = 0; k < 2; k++) {
                memcpy(vm_base + a.lo, before.data(), a.size);
                s_run = &run[k];
                run[k].failed = false;
                run[k].pointer = 0x5EED;
                s_answers = { seed };
                run[k].rc = k == 0 ? t.fw(ctx, a, g, &run[k].pointer) : t.ours(ctx, g, &run[k].pointer);
                run[k].mem.assign(vm_base + a.lo, vm_base + a.lo + a.size);
            }
            signals += (unsigned)run[0].events.size();
            errors += run[0].rc != 0;
            checks += run[0].failed;
            if (run[0].rc == run[1].rc && run[0].pointer == run[1].pointer && run[0].mem == run[1].mem &&
                run[0].events == run[1].events && run[0].failed == run[1].failed)
                continue;
            if (bad++ < 3) {
                fprintf(stderr, "[libsre-check] %s, case %u (%08X %08X %08X ...):\n", t.name, i, (uint32_t)g[0],
                        (uint32_t)g[1], (uint32_t)g[2]);
                Run was = { 0, 0, before, {}, false };
                dump("before  ", was, a);
                dump("libsre's", run[0], a);
                dump("the port's", run[1], a);
            }
        }
        fprintf(stderr, "[libsre-check] %s: %u compared (%u returned an error, %u signals, %u failed checks, %u left out), "
                        "%u differ%s\n",
                t.name, n - skipped, errors, signals, checks, skipped, bad, bad ? " -- MISMATCH" : "");
        total_bad += bad;
    }
    sre::g_probe = nullptr;
    memcpy(vm_base + a.lo, keep.data(), a.size);
    memcpy(ctx->gpr, saved, sizeof saved);
    ctx->reserve_addr = reserve_addr;
    ctx->reserve_value = reserve_value;
    fprintf(stderr, "[libsre-check] %s\n", total_bad ? "the port's queue routines DIFFER from libsre's" :
                                                       "the port's queue routines do what libsre's do");
}

void first_queue(ppu_context* ctx)
{
    fprintf(stderr, "[libsre] this build holds libsre's own queue routines beside the port's: not one to release\n");
    if (const char* e = getenv("DOD3_LIBSRE_CHECK")) {
        const unsigned n = (unsigned)strtoul(e, nullptr, 10);
        if (n) self_check(ctx, n == 1 ? 200000 : n);
    }
    if (const char* e = getenv("DOD3_LIBSRE")) {
        if (!strcmp(e, "fw")) {
            sre::g_firmware = &s_firmware;
            fprintf(stderr, "[libsre] the title's queues run on libsre's routines (DOD3_LIBSRE=fw)\n");
        }
    }
}

struct Install {
    Install() { sre::g_first_queue = first_queue; }
} s_install;

}  // namespace

/* ---- what the lifted libsre calls (see tools/gen_libsre.py) ---- */

/* _cellSpursSendSignal(taskset, taskId) */
extern "C" void dod3_spurs_send_signal(ppu_context* ctx)
{
    ctx->gpr[3] = (uint64_t)(int64_t)(int32_t)sre::spurs_send_signal((uint32_t)ctx->gpr[3], (uint32_t)ctx->gpr[4]);
}

/* libsre 0x010134B8 (spurs, u32* taskset_out, wid): the taskset running as workload `wid` */
extern "C" void dod3_spurs_workload_taskset(ppu_context* ctx)
{
    const uint32_t out = (uint32_t)ctx->gpr[4];
    uint32_t taskset = 0;
    const uint32_t rc = sre::spurs_workload_taskset((uint32_t)ctx->gpr[3], &taskset, (uint32_t)ctx->gpr[5]);
    if (!rc && out) vm_write32(out, taskset);
    ctx->gpr[3] = (uint64_t)(int64_t)(int32_t)rc;
}

/* sysPrxForUser _sys_memcpy(dst, src, n) -> dst, within guest memory */
extern "C" void dod3_libsre_memcpy(ppu_context* ctx)
{
    memmove(vm_base + (uint32_t)ctx->gpr[3], vm_base + (uint32_t)ctx->gpr[4], (uint32_t)ctx->gpr[5]);
}

/* The fpSendSignal both CompletePushPointer variants call: r3 = the queue's
 * eaSignal, r4 = (workload id << 8) | task id. */
extern "C" void dod3_lfq_send_signal(ppu_context* ctx)
{
    ctx->gpr[3] = (uint64_t)(int64_t)(int32_t)sre::lfq_send_signal((uint32_t)ctx->gpr[3], (uint32_t)ctx->gpr[4]);
}

/* libsre's checks. Two of the routines it calls compare a value (r4) with
 * the one expected (r5) and report when they differ; the rest are its
 * imports, _sys_printf with the message and then _sys_trap_process, reached
 * only once a check has failed (and never with r4 equal to r5). */
extern "C" void dod3_libsre_import_trap(ppu_context* ctx)
{
    if ((uint32_t)ctx->gpr[4] == (uint32_t)ctx->gpr[5]) return;
    sre::check_failed("one of libsre's own checks failed");
}
