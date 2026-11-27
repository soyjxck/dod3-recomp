/*
 * SPURS LFQueue, PPU side.
 *
 * Drakengard 3 hands work to its SPU zlib task through a SPURS LFQueue created
 * ANY2ANY (direction 3). The consumer is the game's own lifted SPU code, which
 * pops the queue straight out of guest memory with lock-line atomics, so the
 * PPU side has to keep the 128-byte queue line in exactly the state Sony's
 * libsre would. The toolkit's HLE has no _cellSpursLFQueuePushBody at all, and
 * its _cellSpursLFQueueInitialize leaves the line in a state the real code
 * never produces (no direction at +0x24, depth written into push1.m_h7/m_h8).
 *
 *   _cellSpursLFQueueInitialize  written here, write for write against
 *                                libsre's syncLFQueueInitialize.
 *   _cellSpursLFQueuePushBody    the same control flow as libsre's, on
 *                                _cellSyncLFQueueGetPushPointer(2) and
 *                                _cellSyncLFQueueCompletePushPointer(2), which
 *                                are written here too (sre.h): what libsre's do
 *                                to the line, load for load and store for store.
 *
 * One deliberate difference from libsre: it pushes with isBlocking=0 and
 * useEventQueue=0 and does the blocking itself, by retrying while the queue is
 * full. The real call parks on an lv2 event queue that
 * cellSpursLFQueueAttachLv2EventQueue would have created, and the HLE attach is
 * a stub, so there is no queue to park on. The pointer routines are written
 * for those arguments only.
 *
 * The line (128 bytes, 128-aligned; all big-endian):
 *
 *   +0x00  pop1: four 16-bit words, m_h1 .. m_h4      +0x08  push1: m_h5 .. m_h8
 *            m_h1 the pop pointer up to which pops are complete, m_h4 the
 *            pointer the next pop takes; m_h5 and m_h8 the same for pushes.
 *            Pointers run over [0, 2 x depth); the slot is the pointer mod depth.
 *            m_h2 / m_h6: a bit for each pop / push taken but not yet complete
 *            out of order, bit 15 the one at m_h1 / m_h5.
 *            m_h3 / m_h7 (an ANY2ANY queue): three counters modulo 30 --
 *            bits 0-4 waiters entered, 5-9 waiters served, 10-14 waiters woken
 *            -- for poppers / pushers that found the queue empty / full.
 *   +0x10  an item's size, +0x14 the depth, +0x18 the buffer (64 bits; bit 0
 *          marks ANY2ANY), +0x24 the direction (2 PPU to SPU, 3 ANY2ANY)
 *   +0x28  the event port a waiting PPU popper is woken through
 *   +0x30  16 words: the first the same three counters for a PPU-to-SPU
 *          queue's waiting poppers (and the high half of the PPU pushers'
 *          event port); from +0x32 the ids of waiting poppers, by counter
 *   +0x50  the same for waiting pushers
 *   +0x70  eaSignal, what fpSendSignal is called with (64 bits)
 */
#include "sre.h"
#include "ps3emu/nid.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <chrono>
#include <thread>

extern "C" {
typedef void (*hle_ctx_fn)(ppu_context*);
void ps3_hle_register_ctx(uint32_t nid, const char* name, hle_ctx_fn fn);
void lv2_syscall(ppu_context* ctx);
void spu_taskset_signal_task(uint32_t taskset_ea, uint32_t taskId);
int spurs_tasksets_on(uint32_t spurs_ea, uint32_t* out, int max);
int dod3_edgezlib_push(uint32_t lr, uint32_t req[8]);
}

enum : uint32_t {
    CELL_SYNC_ERROR_AGAIN = 0x80410101,
    CELL_SPURS_TASK_ERROR_INVAL = 0x80410902,
    CELL_SPURS_TASK_ERROR_STAT = 0x8041090F,
    CELL_SPURS_TASK_ERROR_ALIGN = 0x80410910,
    CELL_SPURS_TASK_ERROR_NULL_POINTER = 0x80410911,
    CELL_SPURS_TASK_ERROR_SRCH = 0x80410914,
};

enum : uint32_t {
    LFQ_DIR_ANY2ANY = 3,
    LFQ_M_SIZE = 0x10,
    LFQ_M_DEPTH = 0x14,
    LFQ_M_BUFFER = 0x18,
    LFQ_M_DIRECTION = 0x24,
    LFQ_EA_SIGNAL = 0x70,
};

/* A cellSync error from the push helpers, re-based into the SPURS task range
 * -- libsre does exactly this: (err & 0xFF) | 0x80410900. */
static inline uint32_t spurs_err(uint32_t sync_err) { return (sync_err & 0xFFu) | 0x80410900u; }

namespace sre {

enum : uint32_t {
    L_POP1 = 0x00,
    L_PUSH1 = 0x08,
    L_DEPTH = 0x14,
    L_DIRECTION = 0x24,
    L_POP_PORT = 0x28,
    L_POPPERS = 0x30,
    L_PUSHERS = 0x50,
    L_EA_SIGNAL = 0x74,   /* the low word of eaSignal */
};

/* The three counters of a waiters' word, each modulo 30. */
static inline uint32_t entered(uint32_t w) { return w & 0x1F; }
static inline uint32_t served(uint32_t w) { return (w >> 5) & 0x1F; }
static inline uint32_t woken(uint32_t w) { return (w >> 10) & 0x1F; }
static inline uint32_t mod30(uint32_t a, uint32_t b) { return a >= b ? a - b : a - b + 30; }   /* a - b */
static inline uint32_t with_woken(uint32_t w, uint32_t c) { return (w & ~(0x1Fu << 10)) | (c != 29 ? (c + 1) << 10 : 0); }
/* The id a waiter left: its counter value picks one of 15 slots after the word at `list`. */
static inline uint32_t waiter_id(uint32_t q, uint32_t list, uint32_t c)
{
    return vm_read16(q + list + 2 + 2 * (c > 14 ? c - 15 : c));
}

/* How far `to` is ahead of `from` on a ring of 2 x depth pointers. */
static inline uint32_t ahead(int32_t from, int32_t to, uint32_t depth)
{
    return to >= from ? (uint32_t)(to - from) : ((uint32_t)to + depth) - ((uint32_t)from - depth);
}

/* A SPURS-tagged eaSignal names the taskset by workload id; the HLE gives
 * every taskset workload id 0, so every taskset on that instance gets the
 * signal. Signals are latched in guest state and the SPU wait loop re-checks
 * the queue, so a spurious one costs one extra pass. */
uint32_t lfq_send_signal(uint32_t ea, uint32_t id)
{
    if (g_probe) return g_probe->signal(2, ea, id);
    const uint32_t task = id & 0xFF;
    if ((ea & 0xF) == 1) {
        uint32_t ts[16];
        const int n = spurs_tasksets_on(ea & ~0xFu, ts, 16);
        for (int i = 0; i < n && i < 16; i++) spu_taskset_signal_task(ts[i], task);
        if (!n) fprintf(stderr, "[lfq] signal: no taskset on spurs 0x%08X (task %u)\n", ea & ~0xFu, task);
    } else {
        spu_taskset_signal_task(ea & ~0xFu, task);
    }
    return 0;
}

/* _cellSyncLFQueueGetPushPointer, for a PPU-to-SPU queue: the pointer the
 * push may write at, taken, or AGAIN when the queue is full. */
uint32_t lfq_get_push_pointer(ppu_context* ctx, uint32_t q, int32_t* pointer)
{
    if (vm_read32(q + L_DIRECTION) != 2) return SYNC_ERROR_PERM;
    const uint32_t depth = vm_read32(q + L_DEPTH);
    for (;;) {
        uint64_t push = reserve64(ctx, q + L_PUSH1);
        lwsync();
        const int32_t done = vm_read16(q + L_POP1);   /* m_h1 */
        const int32_t at = (int16_t)push;             /* m_h8 */
        if (!((int32_t)ahead(done, at, depth) < (int32_t)depth)) return SYNC_ERROR_AGAIN;
        *pointer = at;
        const uint32_t next = at + 1 >= (int32_t)(depth << 1) ? 0 : (uint32_t)push + 1;
        push = (push & ~0xFFFFull) | (next & 0xFFFF);
        if (!commit64(ctx, q + L_PUSH1, push)) continue;
        /* m_h7 is set by a pusher that blocked; libsre waits its turn behind it here */
        if ((push >> 16) & 0xFFFF) check_failed("an LFQueue push found a blocked pusher's mark; the port blocks none");
        return 0;
    }
}

/* _cellSyncLFQueueCompletePushPointer, for a PPU-to-SPU queue: the push at
 * `pointer` is written. m_h5 moves past every push complete in order, and
 * one waiting popper is woken. */
uint32_t lfq_complete_push_pointer(ppu_context* ctx, uint32_t q, int32_t pointer)
{
    if (vm_read32(q + L_DIRECTION) != 2) return SYNC_ERROR_PERM;
    const uint32_t depth = vm_read32(q + L_DEPTH);
    for (;;) {
        const uint32_t poppers = reserve32(ctx, q + L_POPPERS);   /* the counters, and the first id */
        const uint32_t counters = poppers >> 16;
        lwsync();
        const uint32_t push = vm_read32(q + L_PUSH1);             /* m_h5, m_h6 */
        const int32_t popped = vm_read16(q + L_POP1);             /* m_h1 */
        const int32_t pushed = push >> 16;                        /* m_h5 */
        const int32_t popping = (int16_t)vm_read16(q + L_POP1 + 6);   /* m_h4 */
        const uint32_t in_flight = ahead(popped, popping, depth);
        uint32_t bits = (push & 0xFFFF) | shl32(1, 15 - ahead(pushed, pointer, depth));
        const uint32_t n = clz32(~(bits | 0xFFFF0000u)) - 16;     /* pushes complete in order from m_h5 */
        uint32_t next = pushed + n;
        bits = shl32(bits, n);
        if ((int32_t)next >= (int32_t)(depth << 1)) next -= depth << 1;
        const uint32_t new_push = (next << 16) | (bits & 0xFFFF);

        const uint32_t c = woken(counters);
        const uint32_t load = in_flight + mod30(c, served(counters));
        uint32_t new_counters = counters & 0xFFFF;
        uint32_t id = 0xFFFFFFFFu;   /* nobody to wake */
        if (!((int32_t)load > 15) && c != entered(counters)) {
            if ((int32_t)n > 1 && mod30(entered(counters), c) > 1 && !((int32_t)(16 - in_flight) <= 1))
                check_failed("an LFQueue push completed several at once with several poppers waiting");
            id = waiter_id(q, L_POPPERS, c);
            new_counters = with_woken(counters, c) & 0xFFFF;
        }
        if (!commit32(ctx, q + L_POPPERS, (new_counters << 16) | (poppers & 0xFFFF))) continue;
        if ((int32_t)load > 16) check_failed("an LFQueue's waiting poppers ran past its 15 slots");
        lwsync();
        bool again = false;
        for (;;) {
            if (reserve32(ctx, q + L_PUSH1) != push) {
                if (id != 0xFFFFFFFFu) check_failed("an LFQueue's push pointers moved under a push that had a popper to wake");
                again = true;
                break;
            }
            lwsync();
            if (id == 0xFFFFFFFFu) {
                /* a popper entered since: go round again and wake it */
                const uint32_t now = vm_read16(q + L_POPPERS);
                if (woken(now) != entered(now)) {
                    again = true;
                    break;
                }
            }
            if (commit32(ctx, q + L_PUSH1, new_push)) break;
        }
        if (again) continue;
        return id == 0xFFFFFFFFu ? 0 : lfq_send_signal(vm_read32(q + L_EA_SIGNAL), id);
    }
}

/* _cellSyncLFQueueGetPushPointer2, for an ANY2ANY queue: the pointer the push
 * may write at, taken, or AGAIN -- the queue is full, the pushes in flight
 * out of order have used the 16 bits there are to mark them, or a pusher is
 * waiting its turn. */
uint32_t lfq_get_push_pointer2(ppu_context* ctx, uint32_t q, int32_t* pointer)
{
    const uint32_t depth = vm_read32(q + L_DEPTH);
    for (;;) {
        uint64_t push = reserve64(ctx, q + L_PUSH1);
        const uint64_t pop = vm_read64(q + L_POP1);
        const int32_t at = (int16_t)push;                          /* m_h8 */
        const int32_t popped = (int32_t)((pop >> 48) & 0xFFFF);    /* m_h1 */
        const int32_t pushed = (int32_t)((push >> 48) & 0xFFFF);   /* m_h5 */
        const uint32_t w = (uint32_t)(push >> 16) & 0xFFFF;        /* m_h7 */
        const uint32_t owed = mod30(woken(w), served(w));          /* pushers woken, their pushes not yet taken */
        const bool full = (int32_t)depth <= (int32_t)(ahead(popped, at, depth) + owed);
        const bool marks_used = (int32_t)(ahead(pushed, at, depth) + owed) > 15;
        *pointer = at;
        if (full || marks_used || mod30(entered(w), woken(w)) != 0) return SYNC_ERROR_AGAIN;
        uint32_t next = (uint32_t)at + 1;
        if (!((int32_t)next < (int32_t)(depth << 1))) next = 0;
        push = (push & ~0xFFFFull) | (next & 0xFFFF);
        if (commit64(ctx, q + L_PUSH1, push)) return 0;
    }
}

/* What a completed push (or pop) wakes: the tasks through fpSendSignal, a PPU
 * thread through the event port it waits on. */
static uint32_t lfq_wake(ppu_context* ctx, uint32_t q, const uint32_t* ids, uint32_t count)
{
    for (uint32_t i = 0; i < count; i++) {
        const uint32_t id = ids[i];
        if ((id & 0xFF00) != 0xFF00) {
            const uint32_t rc = lfq_send_signal(vm_read32(q + L_EA_SIGNAL), id);
            if (rc) return rc;
            continue;
        }
        uint32_t port = ((uint32_t)vm_read16(q + L_POPPERS) << 16) | vm_read16(q + L_PUSHERS);
        if ((id & 0xFF) == 1) port = vm_read32(q + L_POP_PORT);
        if (port == 0xFFFFFFFFu) return SYNC_ERROR_PERM;
        const uint64_t r3 = ctx->gpr[3], r4 = ctx->gpr[4], r5 = ctx->gpr[5], r6 = ctx->gpr[6], r11 = ctx->gpr[11];
        ctx->gpr[3] = port;   /* sys_event_port_send(port, 0, 0, 0) */
        ctx->gpr[4] = ctx->gpr[5] = ctx->gpr[6] = 0;
        ctx->gpr[11] = 0x8A;
        lv2_syscall(ctx);
        ctx->gpr[3] = r3, ctx->gpr[4] = r4, ctx->gpr[5] = r5, ctx->gpr[6] = r6, ctx->gpr[11] = r11;
    }
    return 0;
}

/* _cellSyncLFQueueCompletePushPointer2, for an ANY2ANY queue: the push at
 * `pointer` is written. m_h5 moves past every push complete in order -- n of
 * them -- and for each a waiting popper is woken, while there is one and the
 * pops in flight leave it a mark, and a waiting pusher, while there is one
 * and room.
 *
 * The pushers' counters are in push1, which the routine holds a reservation
 * on; the poppers' are in pop1. With a popper to wake it starts again holding
 * pop1 instead, stores pop1 first and then push1 if neither moved meanwhile;
 * if push1 did, the poppers already taken off the list stay taken (their ids
 * kept) and the rest is worked out again. */
uint32_t lfq_complete_push_pointer2(ppu_context* ctx, uint32_t q, int32_t pointer)
{
    const uint32_t depth = vm_read32(q + L_DEPTH);
    uint32_t ids[64];
    uint32_t kept = 0;       /* ids of the rounds before */
    uint32_t base = 0;       /* where this round's ids go */
    bool by_pop = false;     /* the reservation is on pop1 */
    for (;;) {
        uint64_t push, pop;
        if (!by_pop) {
            push = reserve64(ctx, q + L_PUSH1);
            lwsync();
            pop = vm_read64(q + L_POP1);
        } else {
            pop = reserve64(ctx, q + L_POP1);
            lwsync();
            push = vm_read64(q + L_PUSH1);
        }
        const uint64_t push_was = push;
        const int32_t pushed = (int32_t)((push >> 48) & 0xFFFF);   /* m_h5 */
        const int32_t popped = (int32_t)((pop >> 48) & 0xFFFF);    /* m_h1 */
        uint32_t wpop = (uint32_t)(pop >> 16) & 0xFFFF;            /* m_h3 */
        uint32_t wpush = (uint32_t)(push >> 16) & 0xFFFF;          /* m_h7 */
        int32_t pop_load = (int32_t)(ahead(popped, (int16_t)pop, depth) + mod30(woken(wpop), served(wpop)));
        int32_t push_load = (int32_t)(ahead(popped, (int16_t)push, depth) + mod30(woken(wpush), served(wpush)));
        uint32_t bits = ((uint32_t)(push >> 32) & 0xFFFF) | shl32(1, 15 - ahead(pushed, pointer, depth));
        const uint32_t n = clz32(~(bits | 0xFFFF0000u)) - 16;
        uint32_t next = (uint32_t)pushed + n;
        if ((int32_t)next >= (int32_t)(depth << 1)) next -= depth << 1;
        int32_t poppers = pop_load > 15 ? 0 : (int32_t)mod30(entered(wpop), woken(wpop));
        int32_t pushers = (int32_t)depth > push_load ? (int32_t)mod30(entered(wpush), woken(wpush)) : 0;
        push = (push & 0x0000FFFFFFFFFFFFull) | ((uint64_t)(next & 0xFFFF) << 48);
        push = (push & ~(0xFFFFull << 32)) | ((uint64_t)(shl32(bits, n) & 0xFFFF) << 32);

        uint32_t found = 0;
        bool take_pop = false;
        for (uint32_t i = 0; i < n; i++) {
            const int32_t left = (int32_t)(n - 1 - i);
            if (!((int32_t)kept > left) && poppers != 0 && !(pop_load > 15)) {
                const uint32_t c = woken(wpop);
                const uint32_t id = waiter_id(q, L_POPPERS, c);
                if (base + found >= 62) check_failed("an LFQueue push had more waiters to wake than libsre keeps room for");
                ids[base + found] = id == 0xFFFF ? 0xFF01 : id;   /* no id: a PPU thread, on the event port */
                if (!by_pop) {
                    take_pop = true;
                    break;
                }
                wpop = with_woken(wpop, c);
                found++, pop_load++, poppers--;
            }
            if (pushers != 0 && push_load < (int32_t)depth) {
                const uint32_t c = woken(wpush);
                const uint32_t id = waiter_id(q, L_PUSHERS, c);
                ids[base + found] = id == 0xFFFF ? 0xFF02 : id;
                wpush = with_woken(wpush, c);
                found++, push_load++, pushers--;
            }
        }
        if (take_pop) {
            by_pop = true;
            continue;
        }
        push = (push & ~(0xFFFFull << 16)) | ((uint64_t)(wpush & 0xFFFF) << 16);
        pop = (pop & ~(0xFFFFull << 16)) | ((uint64_t)(wpop & 0xFFFF) << 16);
        if (!(by_pop ? commit64(ctx, q + L_POP1, pop) : commit64(ctx, q + L_PUSH1, push))) continue;
        kept += found;
        if (by_pop) {
            bool again = false;
            for (;;) {
                if (reserve64(ctx, q + L_PUSH1) != push_was) {
                    again = true;
                    break;
                }
                lwsync();
                if (vm_read16(q + L_POP1 + 4) != (wpop & 0xFFFF)) {
                    again = true;
                    break;
                }
                if (commit64(ctx, q + L_PUSH1, push)) break;
            }
            if (again) {
                base = kept;
                continue;
            }
        }
        return lfq_wake(ctx, q, ids, kept);
    }
}

}  // namespace sre

/* _cellSpursLFQueueInitialize(void* pTasksetOrSpurs, CellSpursLFQueue* q,
 *                             const void* buffer, u32 size, u32 depth, u32 dir)
 *
 * The owner becomes the queue's eaSignal. A SPURS instance is passed with bit 0
 * set, a taskset untagged; dod3_lfq_send_signal tells the two apart. */
static void lfq_initialize(ppu_context* ctx)
{
    const uint32_t owner = (uint32_t)ctx->gpr[3];
    const uint32_t q = (uint32_t)ctx->gpr[4];
    const uint32_t buf = (uint32_t)ctx->gpr[5];
    const uint32_t size = (uint32_t)ctx->gpr[6];
    const uint32_t depth = (uint32_t)ctx->gpr[7];
    const uint32_t dir = (uint32_t)ctx->gpr[8];

    sre::queue_made(ctx);
    uint32_t rc = 0;
    if (!q || (size && !buf))
        rc = CELL_SPURS_TASK_ERROR_NULL_POINTER;
    else if ((q & 0x7F) || (buf & 0xF))
        rc = CELL_SPURS_TASK_ERROR_ALIGN;
    else if (size > 0x4000 || (size & 0xF) || !depth || depth > 0x7FFF || dir > LFQ_DIR_ANY2ANY)
        rc = CELL_SPURS_TASK_ERROR_INVAL;
    if (rc) {
        ctx->gpr[3] = (int64_t)(int32_t)rc;
        return;
    }

    /* libsre refuses a line that is not all zero (CELL_SYNC_ERROR_STAT), so a
     * successful init always starts from zero; clearing it here is equivalent. */
    for (uint32_t o = 0; o < 128; o += 8) vm_write64(q + o, 0);

    vm_write32(q + LFQ_M_SIZE, size);
    vm_write32(q + LFQ_M_DEPTH, depth);
    vm_write64(q + LFQ_M_BUFFER, buf);
    vm_write32(q + LFQ_M_DIRECTION, dir);
    vm_write64(q + LFQ_EA_SIGNAL, owner);

    if (dir == LFQ_DIR_ANY2ANY) {
        vm_write64(q + LFQ_M_BUFFER, (uint64_t)buf | 1);  /* ANY2ANY marker */
        vm_write8(q + 0x20, 0xFF);                       /* m_bs[0] */
        vm_write8(q + 0x21, 0xFF);                       /* m_bs[1] */
        vm_write32(q + 0x28, 0xFFFFFFFFu);                /* m_v1 */
        vm_write16(q + 0x30, 0xFFFF);                     /* push2.pack */
        vm_write16(q + 0x50, 0xFFFF);                     /* pop2.pack */
    } else {
        vm_write32(q + 0x20, 0xFFFFFFFFu);                /* m_bs[0..3] */
    }
    /* m_v2, m_eq_id and init stay 0. */

    fprintf(stderr, "[lfq] init q=0x%08X owner=0x%08X buf=0x%08X size=%u depth=%u dir=%u\n", q, owner, buf, size, depth,
            dir);
    ctx->gpr[3] = 0;
}

/* _cellSpursLFQueuePushBody(CellSpursLFQueue* q, const void* buffer, u32 isBlocking) */
static void lfq_push_body(ppu_context* ctx)
{
    const uint32_t q = (uint32_t)ctx->gpr[3];
    const uint32_t src = (uint32_t)ctx->gpr[4];
    const uint32_t blocking = (uint32_t)ctx->gpr[5];

    if (q & 0x7F) {
        ctx->gpr[3] = (int64_t)(int32_t)CELL_SPURS_TASK_ERROR_ALIGN;
        return;
    }
    if (!q || !src) {
        ctx->gpr[3] = (int64_t)(int32_t)CELL_SPURS_TASK_ERROR_NULL_POINTER;
        return;
    }

    /* An Edge zlib request may be inflated here and queued as a token copy
     * instead (src/patches/edgezlib.cpp). */
    uint32_t zreq[8];
    bool zlib_native = false;
    if (vm_read32(q + LFQ_M_SIZE) == sizeof zreq) {
        for (int i = 0; i < 8; i++) zreq[i] = vm_read32(src + 4u * i);
        zlib_native = dod3_edgezlib_push((uint32_t)ctx->lr, zreq) != 0;
    }

    const bool any2any = vm_read32(q + LFQ_M_DIRECTION) == LFQ_DIR_ANY2ANY;

    uint32_t rc;
    int32_t pos = 0;
    for (unsigned spins = 0;; spins++) {
        rc = any2any ? SRE_CALL(lfq_get_push_pointer2, ctx, q, &pos) : SRE_CALL(lfq_get_push_pointer, ctx, q, &pos);
        if (rc != CELL_SYNC_ERROR_AGAIN || !blocking) break;
        if (spins < 64)
            std::this_thread::yield();
        else
            std::this_thread::sleep_for(std::chrono::microseconds(100));
    }
    if (rc) {
        ctx->gpr[3] = (int64_t)(int32_t)spurs_err(rc);
        fprintf(stderr, "[lfq] push q=0x%08X GetPushPointer failed 0x%08X (blocking=%u)\n", q, rc, blocking);
        return;
    }

    const uint32_t depth = vm_read32(q + LFQ_M_DEPTH);
    const uint32_t size = vm_read32(q + LFQ_M_SIZE);
    /* The pointer runs over [0, 2*depth): the slot is pointer mod depth, but
     * CompletePushPointer takes the pointer itself, exactly as libsre passes it.
     * Handing it the slot instead works for the first `depth` pushes and then
     * never advances the completion pointer past the wrap. */
    const int32_t slot = pos >= (int32_t)depth ? pos - (int32_t)depth : pos;
    const uint32_t dst = ((uint32_t)vm_read64(q + LFQ_M_BUFFER) & ~1u) + size * (uint32_t)slot;
    if (zlib_native)
        for (int i = 0; i < 8; i++) vm_write32(dst + 4u * i, zreq[i]);
    else
        for (uint32_t o = 0; o < size; o += 4) vm_write32(dst + o, vm_read32(src + o));

    /* What libsre calls fpSendSignal for is sre::lfq_send_signal: the queue's
     * eaSignal and (workload id << 8) | task id, as libsre's own LFQueue
     * signal routine decodes them. */
    rc = any2any ? SRE_CALL(lfq_complete_push_pointer2, ctx, q, pos) : SRE_CALL(lfq_complete_push_pointer, ctx, q, pos);

    if (rc == CELL_SPURS_TASK_ERROR_INVAL || rc == CELL_SPURS_TASK_ERROR_STAT)
        rc = CELL_SPURS_TASK_ERROR_SRCH;
    else if ((int32_t)rc < 0)
        rc = spurs_err(rc);
    ctx->gpr[3] = (int64_t)(int32_t)rc;

    static int s_n = 0;
    if (rc || (s_n < 64 && ++s_n))
        fprintf(stderr, "[lfq] push q=0x%08X pos=%d blocking=%u rc=0x%08X lr=0x%08X tid=%u\n", q, pos, blocking, rc,
                (uint32_t)ctx->lr, (unsigned)ctx->thread_id);
}

extern "C" void dod3_register_spurs_lfqueue(void)
{
    ps3_hle_register_ctx(ps3_compute_nid("_cellSpursLFQueueInitialize"), "_cellSpursLFQueueInitialize", lfq_initialize);
    ps3_hle_register_ctx(ps3_compute_nid("_cellSpursLFQueuePushBody"), "_cellSpursLFQueuePushBody", lfq_push_body);
}
