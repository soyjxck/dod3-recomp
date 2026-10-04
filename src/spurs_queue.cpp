/*
 * SPURS queue (cellSpursQueue), PPU side, on Sony's own code.
 *
 * Drakengard 3 pairs SPU tasks with SPURS queues the task end of which is the
 * game's lifted SPU code, reading the queue out of guest memory. The toolkit's
 * HLE has no cellSpursQueuePopBody at all -- the game read an uninitialised
 * stack slot as the popped item and asked malloc for 3.8 GB -- and its
 * initializer writes a layout of its own. So all three run libsre's code,
 * lifted at build time by tools/gen_libsre.py:
 *
 *   _cellSpursQueueInitialize  0x010164C0  (spurs, taskset, queue, buffer, size, depth, dir)
 *   cellSpursQueuePushBody     0x010169D0  (queue, buffer, isBlocking)
 *   cellSpursQueuePopBody      0x01016CFC  (queue, buffer, isPeek, isBlocking)
 *
 * As with the LFQueue push, a blocking call is run as a non-blocking one, retried
 * while the queue is full/empty (ERROR_AGAIN) or contended (ERROR_BUSY). The real blocking
 * path parks on the lv2 event queue cellSpursQueueAttachLv2EventQueue would have
 * created -- and refuses with ERROR_STAT when there is none, which is what the
 * HLE's stub attach leaves.
 */
#include "ppu_recomp.h"
#include "ps3emu/nid.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <chrono>
#include <thread>

extern "C" {
typedef void (*hle_ctx_fn)(ppu_context*);
void ps3_hle_register_ctx(uint32_t nid, const char* name, hle_ctx_fn fn);
int  dod3_libsre_call(uint32_t addr, ppu_context* ctx);   /* generated */
void spu_taskset_signal_task(uint32_t taskset_ea, uint32_t taskId);
int  spurs_tasksets_on(uint32_t spurs_ea, uint32_t* out, int max);
extern uint8_t* vm_base;
}

enum : uint32_t {
    LIBSRE_QUEUE_INITIALIZE = 0x010164C0,
    LIBSRE_QUEUE_PUSH_BODY  = 0x010169D0,
    LIBSRE_QUEUE_POP_BODY   = 0x01016CFC,
};

enum : uint32_t {
    CELL_SPURS_TASK_ERROR_AGAIN = 0x80410901,   /* non-blocking: nothing to pop */
    CELL_SPURS_TASK_ERROR_BUSY  = 0x8041090A,   /* non-blocking: lost a race    */
};

/* Run a lifted libsre function on the caller's own arguments: r3..r10 as the
 * guest passed them, LR and TOC put back afterwards. */
static uint32_t run_libsre(ppu_context* ctx, uint32_t fn, const uint64_t args[4])
{
    const uint64_t lr = ctx->lr, r2 = ctx->gpr[2];
    for (int i = 0; i < 4; i++) ctx->gpr[3 + i] = args[i];
    dod3_libsre_call(fn, ctx);
    ctx->lr = lr;
    ctx->gpr[2] = r2;
    return (uint32_t)ctx->gpr[3];
}

/* Run non-blocking, and retry until the queue has room/an item if the caller
 * asked to block. args[block_arg] is the call's isBlocking flag. */
static uint32_t run_blocking(ppu_context* ctx, uint32_t fn, uint64_t args[4], int block_arg)
{
    const bool blocking = (uint8_t)args[block_arg] != 0;
    args[block_arg] = 0;
    uint32_t rc;
    for (unsigned spins = 0;; spins++) {
        rc = run_libsre(ctx, fn, args);
        if (!blocking || (rc != CELL_SPURS_TASK_ERROR_AGAIN && rc != CELL_SPURS_TASK_ERROR_BUSY))
            return rc;
        if (spins < 64) std::this_thread::yield();
        else std::this_thread::sleep_for(std::chrono::microseconds(100));
    }
}

static void queue_initialize(ppu_context* ctx)
{
    /* size and depth ride in r7/r8, which run_libsre leaves alone. */
    uint64_t args[4] = { ctx->gpr[3], ctx->gpr[4], ctx->gpr[5], ctx->gpr[6] };
    const uint64_t r7 = ctx->gpr[7], r8 = ctx->gpr[8];
    const uint32_t rc = run_libsre(ctx, LIBSRE_QUEUE_INITIALIZE, args);
    printf("[spurs-queue] init taskset=0x%08X q=0x%08X buf=0x%08X size=%u depth=%u -> 0x%08X\n",
           (uint32_t)args[1], (uint32_t)args[2], (uint32_t)args[3],
           (uint32_t)r7, (uint32_t)r8, rc);
}

static void queue_push_body(ppu_context* ctx)
{
    { static int on = -1; if (on < 0) on = getenv("DOD3_QUEUE_LOG") ? 1 : 0;   /* item trace */
      if (on) { const uint32_t b = (uint32_t)ctx->gpr[4];
          fprintf(stderr, "[spurs-queue] push q=0x%08X item={%08X %08X %08X %08X}\n", (uint32_t)ctx->gpr[3],
                  vm_read32(b), vm_read32(b + 4), vm_read32(b + 8), vm_read32(b + 12)); } }
    uint64_t args[4] = { ctx->gpr[3], ctx->gpr[4], ctx->gpr[5], 0 };
    run_blocking(ctx, LIBSRE_QUEUE_PUSH_BODY, args, 2);   /* isBlocking = r5 */
}

static void queue_pop_body(ppu_context* ctx)
{
    uint64_t args[4] = { ctx->gpr[3], ctx->gpr[4], ctx->gpr[5], ctx->gpr[6] };
    const uint32_t q = (uint32_t)ctx->gpr[3], buf = (uint32_t)ctx->gpr[4];
    run_blocking(ctx, LIBSRE_QUEUE_POP_BODY, args, 3);    /* isBlocking = r6 */
    { static int on = -1; if (on < 0) on = getenv("DOD3_QUEUE_LOG") ? 1 : 0;   /* item trace */
      if (on && (uint32_t)ctx->gpr[3] == 0)
          fprintf(stderr, "[spurs-queue] pop  q=0x%08X item={%08X %08X %08X %08X} tid=%u\n", q,
                  vm_read32(buf), vm_read32(buf + 4), vm_read32(buf + 8), vm_read32(buf + 12),
                  (unsigned)ctx->thread_id); }
}

/* ---- hooks the lifted libsre calls (see tools/gen_libsre.py) ------------- */

/* _cellSpursSendSignal(taskset, taskId) */
extern "C" void dod3_spurs_send_signal(ppu_context* ctx)
{
    spu_taskset_signal_task((uint32_t)ctx->gpr[3], (uint32_t)ctx->gpr[4]);
    ctx->gpr[3] = 0;
}

/* libsre 0x010134B8 (spurs, u32* taskset_out, wid): the taskset running as
 * workload `wid`. The HLE gives every taskset workload id 0, so the id cannot
 * be trusted to index anything; with more than one taskset on the instance
 * the first is the best answer available, and it is reported once. */
extern "C" void dod3_spurs_workload_taskset(ppu_context* ctx)
{
    const uint32_t spurs = (uint32_t)ctx->gpr[3], out = (uint32_t)ctx->gpr[4];
    const uint32_t wid = (uint32_t)ctx->gpr[5];
    uint32_t ts[16];
    const int n = spurs_tasksets_on(spurs, ts, 16);
    if (!n) { ctx->gpr[3] = (int64_t)(int32_t)0x80410711u; return; }   /* SRCH */
    if (n > 1) { static int s_once = 0;
        if (!s_once++) fprintf(stderr, "[spurs-queue] workload %u on spurs 0x%08X: %d tasksets, using the first\n",
                               wid, spurs, n); }
    if (out) vm_write32(out, ts[0]);
    ctx->gpr[3] = 0;
}

/* sysPrxForUser _sys_memcpy(dst, src, n) -> dst, within guest memory. */
extern "C" void dod3_libsre_memcpy(ppu_context* ctx)
{
    const uint32_t dst = (uint32_t)ctx->gpr[3], src = (uint32_t)ctx->gpr[4];
    const uint32_t n = (uint32_t)ctx->gpr[5];
    memmove(vm_base + dst, vm_base + src, n);
}

extern "C" void dod3_register_spurs_queue(void)
{
    ps3_hle_register_ctx(ps3_compute_nid("_cellSpursQueueInitialize"),
                         "_cellSpursQueueInitialize", queue_initialize);
    ps3_hle_register_ctx(ps3_compute_nid("cellSpursQueuePushBody"),
                         "cellSpursQueuePushBody", queue_push_body);
    ps3_hle_register_ctx(ps3_compute_nid("cellSpursQueuePopBody"),
                         "cellSpursQueuePopBody", queue_pop_body);
}
