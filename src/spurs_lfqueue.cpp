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
 *   _cellSpursLFQueueInitialize  reimplemented here, write for write against
 *                                libsre's syncLFQueueInitialize (0x01001F6C).
 *   _cellSpursLFQueuePushBody    the same control flow as libsre's (0x010171B8),
 *                                calling the real Get/CompletePushPointer(2)
 *                                lifted out of libsre.prx at build time by
 *                                tools/gen_libsre_lfqueue.py.
 *
 * One deliberate difference from libsre: it pushes with isBlocking=0 and
 * useEventQueue=0 and does the blocking itself, by retrying while the queue is
 * full. The real call parks on an lv2 event queue that
 * cellSpursLFQueueAttachLv2EventQueue would have created, and the HLE attach is
 * a stub, so there is no queue to park on.
 */
#include "ppu_recomp.h"
#include "ps3emu/nid.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <chrono>
#include <thread>

extern "C" {
typedef void (*hle_ctx_fn)(ppu_context*);
void ps3_hle_register_ctx(uint32_t nid, const char* name, hle_ctx_fn fn);
int  dod3_libsre_call(uint32_t addr, ppu_context* ctx);   /* generated */
void spu_taskset_signal_task(uint32_t taskset_ea, uint32_t taskId);
int  spurs_tasksets_on(uint32_t spurs_ea, uint32_t* out, int max);
}

/* libsre entry points (see tools/gen_libsre_lfqueue.py) */
enum : uint32_t {
    LIBSRE_GET_PUSH_POINTER       = 0x010024F4,
    LIBSRE_COMPLETE_PUSH_POINTER  = 0x01002708,
    LIBSRE_GET_PUSH_POINTER2      = 0x010030B8,
    LIBSRE_COMPLETE_PUSH_POINTER2 = 0x010035C8,
};

enum : uint32_t {
    CELL_SYNC_ERROR_AGAIN      = 0x80410101,
    CELL_SPURS_TASK_ERROR_INVAL = 0x80410902,
    CELL_SPURS_TASK_ERROR_PERM  = 0x80410909,
    CELL_SPURS_TASK_ERROR_ALIGN = 0x80410910,
    CELL_SPURS_TASK_ERROR_NULL_POINTER = 0x80410911,
    CELL_SPURS_TASK_ERROR_SRCH  = 0x80410914,
};

enum : uint32_t {
    LFQ_DIR_ANY2ANY = 3,
    LFQ_M_SIZE      = 0x10,
    LFQ_M_DEPTH     = 0x14,
    LFQ_M_BUFFER    = 0x18,
    LFQ_M_DIRECTION = 0x24,
    LFQ_EA_SIGNAL   = 0x70,
};

/* A cellSync error from the push helpers, re-based into the SPURS task range
 * -- libsre does exactly this: (err & 0xFF) | 0x80410900. */
static inline uint32_t spurs_err(uint32_t sync_err)
{
    return (sync_err & 0xFFu) | 0x80410900u;
}

/* Call a lifted libsre function as if from guest code: a minimal frame below the
 * caller's stack for the out-parameter, LR and TOC put back afterwards. */
static uint32_t call_libsre(ppu_context* ctx, uint32_t fn, uint64_t a3, uint64_t a4,
                            uint64_t a5, uint64_t a6)
{
    const uint64_t lr = ctx->lr, r2 = ctx->gpr[2];
    ctx->gpr[3] = a3; ctx->gpr[4] = a4; ctx->gpr[5] = a5; ctx->gpr[6] = a6;
    dod3_libsre_call(fn, ctx);
    ctx->lr = lr;
    ctx->gpr[2] = r2;
    return (uint32_t)ctx->gpr[3];
}

/* LFQ_DUMP=1: print every queue's 128-byte line once a second, so a stalled
 * producer/consumer pair can be read off the state machine directly. */
static uint32_t s_queues[8];
static int      s_nqueues;

static void lfq_dump_thread()
{
    for (;;) {
        std::this_thread::sleep_for(std::chrono::seconds(1));
        for (int i = 0; i < s_nqueues; i++) {
            const uint32_t q = s_queues[i];
            fprintf(stderr, "[lfq-dump] q=0x%08X", q);
            for (uint32_t o = 0; o < 128; o += 4) {
                if (!(o & 0x1F)) fprintf(stderr, "\n[lfq-dump]   +%02X:", o);
                fprintf(stderr, " %08X", vm_read32(q + o));
            }
            fprintf(stderr, "\n");
        }
    }
}

static void lfq_track(uint32_t q)
{
    static bool on = getenv("LFQ_DUMP") != nullptr;
    if (!on || s_nqueues >= 8) return;
    s_queues[s_nqueues++] = q;
    if (s_nqueues == 1) std::thread(lfq_dump_thread).detach();
}

/* _cellSpursLFQueueInitialize(void* pTasksetOrSpurs, CellSpursLFQueue* q,
 *                             const void* buffer, u32 size, u32 depth, u32 dir)
 *
 * The owner becomes the queue's eaSignal. A SPURS instance is passed with bit 0
 * set, a taskset untagged; dod3_lfq_send_signal tells the two apart. */
static void lfq_initialize(ppu_context* ctx)
{
    const uint32_t owner = (uint32_t)ctx->gpr[3];
    const uint32_t q     = (uint32_t)ctx->gpr[4];
    const uint32_t buf   = (uint32_t)ctx->gpr[5];
    const uint32_t size  = (uint32_t)ctx->gpr[6];
    const uint32_t depth = (uint32_t)ctx->gpr[7];
    const uint32_t dir   = (uint32_t)ctx->gpr[8];

    uint32_t rc = 0;
    if (!q || (size && !buf))                              rc = CELL_SPURS_TASK_ERROR_NULL_POINTER;
    else if ((q & 0x7F) || (buf & 0xF))                    rc = CELL_SPURS_TASK_ERROR_ALIGN;
    else if (size > 0x4000 || (size & 0xF) || !depth ||
             depth > 0x7FFF || dir > LFQ_DIR_ANY2ANY)      rc = CELL_SPURS_TASK_ERROR_INVAL;
    if (rc) { ctx->gpr[3] = (int64_t)(int32_t)rc; return; }

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
        vm_write8 (q + 0x20, 0xFF);                       /* m_bs[0] */
        vm_write8 (q + 0x21, 0xFF);                       /* m_bs[1] */
        vm_write32(q + 0x28, 0xFFFFFFFFu);                /* m_v1 */
        vm_write16(q + 0x30, 0xFFFF);                     /* push2.pack */
        vm_write16(q + 0x50, 0xFFFF);                     /* pop2.pack */
    } else {
        vm_write32(q + 0x20, 0xFFFFFFFFu);                /* m_bs[0..3] */
    }
    /* m_v2, m_eq_id and init stay 0. */

    lfq_track(q);
    printf("[lfq] init q=0x%08X owner=0x%08X buf=0x%08X size=%u depth=%u dir=%u\n",
           q, owner, buf, size, depth, dir);
    ctx->gpr[3] = 0;
}

/* _cellSpursLFQueuePushBody(CellSpursLFQueue* q, const void* buffer, u32 isBlocking) */
static void lfq_push_body(ppu_context* ctx)
{
    const uint32_t q        = (uint32_t)ctx->gpr[3];
    const uint32_t src      = (uint32_t)ctx->gpr[4];
    const uint32_t blocking = (uint32_t)ctx->gpr[5];

    if (q & 0x7F)    { ctx->gpr[3] = (int64_t)(int32_t)CELL_SPURS_TASK_ERROR_ALIGN; return; }
    if (!q || !src)  { ctx->gpr[3] = (int64_t)(int32_t)CELL_SPURS_TASK_ERROR_NULL_POINTER; return; }

    const bool any2any = vm_read32(q + LFQ_M_DIRECTION) == LFQ_DIR_ANY2ANY;

    /* Frame for the out-parameter: back chain at +0, `pointer` at +0x70, as in
     * libsre's own frame. */
    const uint64_t sp = ctx->gpr[1];
    const uint32_t frame = ((uint32_t)sp - 0x80) & ~0xFu;
    vm_write64(frame, sp);
    ctx->gpr[1] = frame;
    const uint32_t pos_ea = frame + 0x70;

    uint32_t rc;
    for (unsigned spins = 0;; spins++) {
        rc = call_libsre(ctx, any2any ? LIBSRE_GET_PUSH_POINTER2 : LIBSRE_GET_PUSH_POINTER,
                         q, pos_ea, /*isBlocking*/0, /*useEventQueue*/0);
        if (rc != CELL_SYNC_ERROR_AGAIN || !blocking) break;
        if (spins < 64) std::this_thread::yield();
        else std::this_thread::sleep_for(std::chrono::microseconds(100));
    }
    if (rc) {
        ctx->gpr[1] = sp;
        ctx->gpr[3] = (int64_t)(int32_t)spurs_err(rc);
        fprintf(stderr, "[lfq] push q=0x%08X GetPushPointer failed 0x%08X (blocking=%u)\n",
                q, rc, blocking);
        return;
    }

    const uint32_t depth = vm_read32(q + LFQ_M_DEPTH);
    const uint32_t size  = vm_read32(q + LFQ_M_SIZE);
    /* The pointer runs over [0, 2*depth): the slot is pointer mod depth, but
     * CompletePushPointer takes the pointer itself, exactly as libsre passes it.
     * Handing it the slot instead works for the first `depth` pushes and then
     * never advances the completion pointer past the wrap. */
    const int32_t pos  = (int32_t)vm_read32(pos_ea);
    const int32_t slot = pos >= (int32_t)depth ? pos - (int32_t)depth : pos;
    const uint32_t dst = ((uint32_t)vm_read64(q + LFQ_M_BUFFER) & ~1u) + size * (uint32_t)slot;
    for (uint32_t o = 0; o < size; o += 4) vm_write32(dst + o, vm_read32(src + o));

    /* fpSendSignal: the lifted code only dereferences it as an OPD on the way to
     * the (rewritten) indirect call, and refuses a null one, so any readable
     * non-null EA does. The queue's own eaSignal field is one. */
    rc = call_libsre(ctx, any2any ? LIBSRE_COMPLETE_PUSH_POINTER2 : LIBSRE_COMPLETE_PUSH_POINTER,
                     q, (int64_t)pos, q + LFQ_EA_SIGNAL, 0);
    ctx->gpr[1] = sp;

    if (rc == 0x80410902u || rc == 0x8041090Fu) rc = CELL_SPURS_TASK_ERROR_SRCH;
    else if ((int32_t)rc < 0)                   rc = spurs_err(rc);
    ctx->gpr[3] = (int64_t)(int32_t)rc;

    static int s_n = 0;
    if (rc || s_n++ < 64)
        fprintf(stderr, "[lfq] push q=0x%08X pos=%d blocking=%u rc=0x%08X lr=0x%08X tid=%u\n",
                q, pos, blocking, rc, (uint32_t)ctx->lr, (unsigned)ctx->thread_id);
}

/* The fpSendSignal both CompletePushPointer variants call to wake an SPU task
 * parked on the queue: r3 = the queue's eaSignal, r4 = (workload id << 8) |
 * task id, as libsre's own LFQueue signal routine (0x010127CC) decodes them.
 * A SPURS-tagged eaSignal names the taskset by workload id; the HLE gives every
 * taskset workload id 0, so every taskset on that instance gets the signal.
 * Signals are latched in guest state and the SPU wait loop re-checks the queue,
 * so a spurious one costs one extra pass. */
extern "C" void dod3_lfq_send_signal(ppu_context* ctx)
{
    const uint32_t ea   = (uint32_t)ctx->gpr[3];
    const uint32_t task = (uint32_t)ctx->gpr[4] & 0xFF;

    if ((ea & 0xF) == 1) {
        uint32_t ts[16];
        const int n = spurs_tasksets_on(ea & ~0xFu, ts, 16);
        for (int i = 0; i < n && i < 16; i++) spu_taskset_signal_task(ts[i], task);
        if (!n) fprintf(stderr, "[lfq] signal: no taskset on spurs 0x%08X (task %u)\n", ea & ~0xFu, task);
    } else {
        spu_taskset_signal_task(ea & ~0xFu, task);
    }
    ctx->gpr[3] = 0;
}

/* libsre's push path only calls its imports when an internal assertion fails:
 * _sys_printf with the message, then _sys_trap_process. */
extern "C" void dod3_libsre_import_trap(ppu_context* ctx)
{
    fprintf(stderr, "[lfq] libsre assertion failed in the LFQueue push path (lr=0x%08X)\n",
            (uint32_t)ctx->lr);
    abort();
}

extern "C" void dod3_register_spurs_lfqueue(void)
{
    ps3_hle_register_ctx(ps3_compute_nid("_cellSpursLFQueueInitialize"),
                         "_cellSpursLFQueueInitialize", lfq_initialize);
    ps3_hle_register_ctx(ps3_compute_nid("_cellSpursLFQueuePushBody"),
                         "_cellSpursLFQueuePushBody", lfq_push_body);
}
