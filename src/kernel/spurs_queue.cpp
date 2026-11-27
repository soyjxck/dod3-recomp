/*
 * SPURS queue (cellSpursQueue), PPU side.
 *
 * Drakengard 3 pairs SPU tasks with SPURS queues the task end of which is the
 * game's lifted SPU code, reading the queue out of guest memory. The toolkit's
 * HLE has no cellSpursQueuePopBody at all -- the game read an uninitialised
 * stack slot as the popped item and asked malloc for 3.8 GB -- and its
 * initializer writes a layout of its own. So all three are written here to
 * do what libsre's do, store for store (sre.h):
 *
 *   _cellSpursQueueInitialize  (spurs, taskset, queue, buffer, size, depth, dir)
 *   cellSpursQueuePushBody     (queue, buffer, isBlocking)
 *   cellSpursQueuePopBody      (queue, buffer, isPeek, isBlocking)
 *
 * The queue (128 bytes, 128-aligned):
 *
 *   +0x00  the pop pointer, +0x04 the push pointer: each runs over
 *          [0, 2 x depth), the slot being the pointer mod depth, and each is
 *          kept as its complement while a pop (a push) has the queue -- a
 *          second one finds it negative and is told BUSY
 *   +0x08  an item's size, +0x0C the depth, +0x10 the buffer (64 bits)
 *   +0x18  the lv2 event queue a blocked PPU caller parks on, -1: none
 *   +0x1C  the direction: 1 SPU to PPU (the PPU pops), 2 PPU to SPU
 *   +0x20  set while an SPU task waits to pop: +0x21 its task id, +0x41 its
 *          workload id; +0x30, +0x31, +0x51 the same for one waiting to push
 *   +0x60  the taskset (64 bits), or 0 and +0x68 the SPURS instance
 *
 * As with the LFQueue push, a blocking call is run as a non-blocking one, retried
 * while the queue is full/empty (ERROR_AGAIN) or contended (ERROR_BUSY). libsre's
 * blocking path parks on the lv2 event queue cellSpursQueueAttachLv2EventQueue
 * would have created -- and refuses with ERROR_STAT when there is none, which is
 * what the HLE's stub attach leaves.
 */
#include "sre.h"
#include "ps3emu/nid.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <chrono>
#include <thread>
#include <atomic>
#include <condition_variable>
#include <mutex>

extern "C" {
typedef void (*hle_ctx_fn)(ppu_context*);
void ps3_hle_register_ctx(uint32_t nid, const char* name, hle_ctx_fn fn);
void spu_taskset_signal_task(uint32_t taskset_ea, uint32_t taskId);
int spurs_tasksets_on(uint32_t spurs_ea, uint32_t* out, int max);
extern uint8_t* vm_base;
extern void (*g_spu_line_commit_hook2)(uint32_t);   /* runtime/spu/spu_channels.c */
}

enum : uint32_t {
    CELL_SPURS_TASK_ERROR_AGAIN = sre::SPURS_TASK_ERROR_AGAIN,   /* non-blocking: nothing to pop */
    CELL_SPURS_TASK_ERROR_BUSY = sre::SPURS_TASK_ERROR_BUSY,     /* non-blocking: lost a race    */
};

enum : uint32_t {
    Q_POP = 0x00,
    Q_PUSH = 0x04,
    Q_SIZE = 0x08,
    Q_DEPTH = 0x0C,
    Q_BUFFER = 0x10,
    Q_EVENT_QUEUE = 0x18,
    Q_DIRECTION = 0x1C,
    Q_POP_WAITER = 0x20,    /* +0 set, +1 the task, +0x21 the workload */
    Q_PUSH_WAITER = 0x30,
    Q_TASKSET = 0x60,
    Q_SPURS = 0x68,
};

namespace sre {

Probe* g_probe;

void check_failed(const char* what)
{
    if (g_probe) {
        g_probe->failed(what);
        return;
    }
    fprintf(stderr, "[spurs-queue] %s\n", what);
    abort();
}

uint32_t spurs_send_signal(uint32_t taskset, uint32_t task)
{
    if (g_probe) return g_probe->signal(0, taskset, task);
    spu_taskset_signal_task(taskset, task);
    return 0;
}

/* The taskset running as workload `workload` on a SPURS instance. The HLE
 * gives every taskset workload id 0, so the id cannot be trusted to index
 * anything; with more than one taskset on the instance the first is the best
 * answer available, and it is reported once. */
uint32_t spurs_workload_taskset(uint32_t spurs, uint32_t* taskset, uint32_t workload)
{
    if (g_probe) {
        *taskset = g_probe->signal(1, spurs, workload);
        return 0;
    }
    uint32_t ts[16];
    const int n = spurs_tasksets_on(spurs, ts, 16);
    if (!n) return 0x80410711u;   /* SRCH */
    if (n > 1) {
        static int s_once = 0;
        if (!s_once++)
            fprintf(stderr, "[spurs-queue] workload %u on spurs 0x%08X: %d tasksets, using the first\n", workload, spurs,
                    n);
    }
    *taskset = ts[0];
    return 0;
}

uint32_t queue_initialize(uint32_t spurs, uint32_t taskset, uint32_t q, uint32_t buffer, uint32_t size, uint32_t depth,
                          uint32_t direction)
{
    if (!taskset && !spurs) return SPURS_TASK_ERROR_NULL_POINTER;
    if (!q || (size && !buffer)) return SPURS_TASK_ERROR_NULL_POINTER;
    if ((spurs & 0x7F) || (taskset & 0x7F) || (q & 0x7F) || (buffer & 0xF)) return SPURS_TASK_ERROR_ALIGN;
    uint64_t owner = spurs;
    if (taskset) {
        if (vm_read32(taskset + 0x74) > 0x1F) return SPURS_TASK_ERROR_INVAL;   /* its workload id */
        owner = vm_read64(taskset + 0x60);                                     /* the taskset's SPURS instance */
    }
    if (size > 0x4000 || (size & 0xF) || !depth || (depth & 0xC0000000u) || direction > 2) return SPURS_TASK_ERROR_INVAL;
    vm_write32(q + Q_POP, 0);
    vm_write32(q + Q_SIZE, size);
    vm_write32(q + Q_DEPTH, depth);
    vm_write64(q + Q_BUFFER, buffer);
    vm_write32(q + Q_EVENT_QUEUE, 0xFFFFFFFFu);
    vm_write64(q + Q_TASKSET, taskset);
    vm_write64(q + Q_SPURS, owner);
    vm_write32(q + Q_DIRECTION, direction);
    vm_write32(q + Q_PUSH, 0);
    for (uint32_t i = 0; i < 16; i++) {
        vm_write8(q + 0x20 + i, 0);
        vm_write8(q + 0x30 + i, 0);
        vm_write8(q + 0x40 + i, 0);
        vm_write8(q + 0x50 + i, 0);
    }
    lwsync();
    return 0;
}

/* A push or pop that found a task of the other kind waiting, and has let go
 * of the queue: the task is signalled. */
static uint32_t wake_waiter(uint32_t q, uint32_t task, uint32_t workload)
{
    uint32_t taskset = 0;
    const uint64_t ts = vm_read64(q + Q_TASKSET);
    if (ts)
        taskset = (uint32_t)ts;
    else
        spurs_workload_taskset(vm_read32(q + Q_SPURS + 4), &taskset, workload);
    const uint32_t rc = spurs_send_signal(taskset, task);
    if (rc == SPURS_TASK_ERROR_INVAL || rc == SPURS_TASK_ERROR_STAT) return SPURS_TASK_ERROR_SRCH;
    if (rc) check_failed("a SPURS queue's signal to the task waiting on it failed");
    return 0;
}

/* The number of items between two pointers of a queue `depth` deep. */
static inline uint32_t span(uint32_t from, uint32_t to, uint32_t depth)
{
    return (int32_t)to >= (int32_t)from ? to - from : (to + depth) - (from - depth);
}

uint32_t queue_push(ppu_context* ctx, uint32_t q, uint32_t item)
{
    if (!q || !item) return SPURS_TASK_ERROR_NULL_POINTER;
    if (q & 0x7F) return SPURS_TASK_ERROR_ALIGN;
    if (vm_read32(q + Q_DIRECTION) != 2) return SPURS_TASK_ERROR_PERM;

    /* Take the queue: the push pointer becomes its complement -- unless the
     * queue is full, which leaves it as it is. */
    uint32_t push, slot;
    bool full;
    for (;;) {
        push = reserve32(ctx, q + Q_PUSH);
        if ((int32_t)push < 0) return SPURS_TASK_ERROR_BUSY;
        lwsync();
        uint32_t pop = vm_read32(q + Q_POP);
        if ((int32_t)pop < 0) pop = ~pop;
        const uint32_t depth = vm_read32(q + Q_DEPTH);
        full = span(pop, push, depth) == depth;
        slot = (int32_t)push < (int32_t)depth ? push : push - depth;
        if (commit32(ctx, q + Q_PUSH, full ? push : ~push)) break;
    }
    if (full) return SPURS_TASK_ERROR_AGAIN;

    const uint32_t size = vm_read32(q + Q_SIZE);
    memmove(vm_base + (uint32_t)(size * slot + (uint32_t)vm_read64(q + Q_BUFFER)), vm_base + item, size);

    /* Let go: the pointer moves on. A task waiting to pop is woken if the
     * queue was empty. */
    bool wake;
    uint32_t task, workload;
    for (;;) {
        const uint32_t held = ~reserve32(ctx, q + Q_PUSH);
        lwsync();
        uint32_t pop = vm_read32(q + Q_POP);
        if ((int32_t)pop < 0) pop = ~pop;
        const uint32_t depth = vm_read32(q + Q_DEPTH);
        wake = vm_read8(q + Q_POP_WAITER) != 0 && span(pop, held, depth) == 0;
        task = vm_read8(q + Q_POP_WAITER + 1);
        workload = vm_read8(q + Q_POP_WAITER + 0x21);
        uint32_t next = held + 1;
        if (!((int32_t)(depth << 1) > (int32_t)next)) next = 0;
        if (commit32(ctx, q + Q_PUSH, next)) break;
    }
    return wake ? wake_waiter(q, task, workload) : 0;
}

uint32_t queue_pop(ppu_context* ctx, uint32_t q, uint32_t item, bool peek)
{
    if (!q || !item) return SPURS_TASK_ERROR_NULL_POINTER;
    if (q & 0x7F) return SPURS_TASK_ERROR_ALIGN;
    if (vm_read32(q + Q_DIRECTION) != 1) return SPURS_TASK_ERROR_PERM;

    uint32_t pop, slot;
    bool empty;
    for (;;) {
        pop = reserve32(ctx, q + Q_POP);
        if ((int32_t)pop < 0) return SPURS_TASK_ERROR_BUSY;
        lwsync();
        uint32_t push = vm_read32(q + Q_PUSH);
        if ((int32_t)push < 0) push = ~push;
        const uint32_t depth = vm_read32(q + Q_DEPTH);
        empty = span(pop, push, depth) == 0;
        slot = (int32_t)pop < (int32_t)depth ? pop : pop - depth;
        if (commit32(ctx, q + Q_POP, empty ? pop : ~pop)) break;
    }
    if (empty) return SPURS_TASK_ERROR_AGAIN;

    const uint32_t size = vm_read32(q + Q_SIZE);
    memmove(vm_base + item, vm_base + (uint32_t)(size * slot + (uint32_t)vm_read64(q + Q_BUFFER)), size);

    /* Let go: the pointer moves on unless this was a look. A task waiting to
     * push is woken if the queue was full. */
    bool wake;
    uint32_t task, workload;
    for (;;) {
        uint32_t held = ~reserve32(ctx, q + Q_POP);
        lwsync();
        uint32_t push = vm_read32(q + Q_PUSH);
        if ((int32_t)push < 0) push = ~push;
        const uint32_t depth = vm_read32(q + Q_DEPTH);
        wake = vm_read8(q + Q_PUSH_WAITER) != 0 && span(held, push, depth) == depth;
        task = vm_read8(q + Q_PUSH_WAITER + 1);
        workload = vm_read8(q + Q_PUSH_WAITER + 0x21);
        if (!peek) {
            held++;
            if (!((int32_t)(depth << 1) > (int32_t)held)) held = 0;
        }
        if (commit32(ctx, q + Q_POP, held)) break;
    }
    return wake && !peek ? wake_waiter(q, task, workload) : 0;
}

}  // namespace sre

/* ---- waking a blocked push/pop -----------------------------------------
 *
 * A blocking call retries until the queue has an item/room. Between tries a
 * waiter sleeps until the queue's line changes -- an SPU task pushing or
 * popping commits it with PUTLLC, which the SPU runtime reports through
 * g_spu_line_commit_hook2, and the PPU side reports its own calls -- with a
 * 1 ms timeout in case a change arrives some other way. (PhysX pops 20-30
 * SPU task results a frame on the thread the game thread then waits on;
 * retrying on a timer held the game thread up for a sixth of every frame.) */
static std::mutex s_wait_mu;
static std::condition_variable s_wait_cv;
static std::atomic<unsigned> s_wait_gen{ 0 };
static std::atomic<int> s_waiters{ 0 };
static uint32_t s_lines[64];
static std::atomic<int> s_nlines{ 0 };

static void queue_changed()
{
    s_wait_gen.fetch_add(1, std::memory_order_release);
    if (s_waiters.load(std::memory_order_acquire)) {
        std::lock_guard<std::mutex> lk(s_wait_mu);
        s_wait_cv.notify_all();
    }
}

static void queue_line_committed(uint32_t line)
{
    const int n = s_nlines.load(std::memory_order_acquire);
    for (int i = 0; i < n; i++)
        if (s_lines[i] == line) {
            queue_changed();
            return;
        }
}

static void watch_queue(uint32_t q)
{
    std::lock_guard<std::mutex> lk(s_wait_mu);
    const int n = s_nlines.load();
    for (int i = 0; i < n; i++)
        if (s_lines[i] == (q & ~127u)) return;
    if (n >= 64) return;
    s_lines[n] = q & ~127u;
    s_nlines.store(n + 1, std::memory_order_release);
    g_spu_line_commit_hook2 = queue_line_committed;
}

/* Run non-blocking, and retry until the queue has room/an item if the caller
 * asked to block. */
template <class Call>
static uint32_t run_blocking(bool blocking, Call call)
{
    uint32_t rc;
    for (unsigned spins = 0;; spins++) {
        const unsigned gen = s_wait_gen.load(std::memory_order_acquire);
        rc = call();
        if (!blocking || (rc != CELL_SPURS_TASK_ERROR_AGAIN && rc != CELL_SPURS_TASK_ERROR_BUSY)) {
            if (rc == 0) queue_changed();   /* a PPU push/pop: room or an item for someone */
            return rc;
        }
        if (rc == CELL_SPURS_TASK_ERROR_BUSY || spins < 8) {
            std::this_thread::yield();
            continue;
        }
        std::unique_lock<std::mutex> lk(s_wait_mu);
        s_waiters.fetch_add(1);
        s_wait_cv.wait_for(lk, std::chrono::milliseconds(1),
                           [&] { return s_wait_gen.load(std::memory_order_acquire) != gen; });
        s_waiters.fetch_sub(1);
    }
}

static void queue_initialize(ppu_context* ctx)
{
    const uint32_t spurs = (uint32_t)ctx->gpr[3], taskset = (uint32_t)ctx->gpr[4], q = (uint32_t)ctx->gpr[5];
    const uint32_t buf = (uint32_t)ctx->gpr[6], size = (uint32_t)ctx->gpr[7], depth = (uint32_t)ctx->gpr[8];
    const uint32_t dir = (uint32_t)ctx->gpr[9];
    sre::queue_made(ctx);
#ifdef DOD3_FW_LIBSRE
    const uint32_t rc = sre::g_firmware ? sre::g_firmware->queue_initialize(ctx, spurs, taskset, q, buf, size, depth, dir) :
                                          sre::queue_initialize(spurs, taskset, q, buf, size, depth, dir);
#else
    const uint32_t rc = sre::queue_initialize(spurs, taskset, q, buf, size, depth, dir);
#endif
    if (rc == 0) watch_queue(q);
    fprintf(stderr, "[spurs-queue] init taskset=0x%08X q=0x%08X buf=0x%08X size=%u depth=%u -> 0x%08X\n", taskset, q, buf,
            size, depth, rc);
    ctx->gpr[3] = (int64_t)(int32_t)rc;
}

static void queue_push_body(ppu_context* ctx)
{
    const uint32_t q = (uint32_t)ctx->gpr[3], item = (uint32_t)ctx->gpr[4];
    const uint32_t rc = run_blocking((uint8_t)ctx->gpr[5] != 0, [&] { return SRE_CALL(queue_push, ctx, q, item); });
    ctx->gpr[3] = (int64_t)(int32_t)rc;
}

static void queue_pop_body(ppu_context* ctx)
{
    const uint32_t q = (uint32_t)ctx->gpr[3], item = (uint32_t)ctx->gpr[4];
    const bool peek = (uint8_t)ctx->gpr[5] != 0;
    const uint32_t rc = run_blocking((uint8_t)ctx->gpr[6] != 0, [&] { return SRE_CALL(queue_pop, ctx, q, item, peek); });
    ctx->gpr[3] = (int64_t)(int32_t)rc;
}

extern "C" void dod3_register_spurs_queue(void)
{
    ps3_hle_register_ctx(ps3_compute_nid("_cellSpursQueueInitialize"), "_cellSpursQueueInitialize", queue_initialize);
    ps3_hle_register_ctx(ps3_compute_nid("cellSpursQueuePushBody"), "cellSpursQueuePushBody", queue_push_body);
    ps3_hle_register_ctx(ps3_compute_nid("cellSpursQueuePopBody"), "cellSpursQueuePopBody", queue_pop_body);
}
