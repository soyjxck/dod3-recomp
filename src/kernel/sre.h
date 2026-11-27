/*
 * The queue routines of libsre (the firmware's SPURS and sync library) that
 * the port needs on the PPU side, written out here: what the SPURS queue's
 * initialize, push and pop do (spurs_queue.cpp) and what an LFQueue's push
 * does to its 128-byte line (spurs_lfqueue.cpp). The other end of each queue
 * is the game's own SPU code, working on the same guest memory with lock-line
 * atomics, so every load, store and reservation here is made where libsre
 * makes it and in its order.
 *
 * Until 0.6.0 these ran as libsre's own code, translated at build time from
 * a decrypted libsre.prx. A build can still hold that translation beside
 * these (cmake -DDOD3_FW_LIBSRE=ON) to compare the two: src/kernel/sre_check.cpp.
 *
 * Not written: the forms that block. libsre parks a blocked caller on an lv2
 * event queue attached to the queue, the port attaches none, and its callers
 * ask without blocking and wait themselves.
 */
#pragma once
#include "ppu_recomp.h"
#include <stdint.h>

namespace sre {

enum : uint32_t {
    SYNC_ERROR_AGAIN = 0x80410101,
    SYNC_ERROR_PERM = 0x80410109,
    SPURS_TASK_ERROR_AGAIN = 0x80410901,
    SPURS_TASK_ERROR_INVAL = 0x80410902,
    SPURS_TASK_ERROR_PERM = 0x80410909,
    SPURS_TASK_ERROR_BUSY = 0x8041090A,
    SPURS_TASK_ERROR_STAT = 0x8041090F,
    SPURS_TASK_ERROR_ALIGN = 0x80410910,
    SPURS_TASK_ERROR_NULL_POINTER = 0x80410911,
    SPURS_TASK_ERROR_SRCH = 0x80410914,
};

/* lwarx / ldarx and stwcx. / stdcx., as the lifted PPU code makes them: the
 * value read is the reservation, and the store goes through the runtime's
 * conditional store, which is what keeps it apart from an SPU's PUTLLC on
 * the same line. */
inline uint32_t reserve32(ppu_context* ctx, uint32_t ea)
{
    const uint32_t v = vm_read32(ea);
    ctx->reserve_addr = ea;
    ctx->reserve_value = v;
    return v;
}
inline uint64_t reserve64(ppu_context* ctx, uint32_t ea)
{
    const uint64_t v = vm_read64(ea);
    ctx->reserve_addr = ea;
    ctx->reserve_value = v;
    return v;
}
inline bool commit32(ppu_context* ctx, uint32_t ea, uint32_t v)
{
    return ctx->reserve_addr == ea && ppu_stwcx32(ea, (uint32_t)ctx->reserve_value, v) != 0;
}
inline bool commit64(ppu_context* ctx, uint32_t ea, uint64_t v)
{
    return ctx->reserve_addr == ea && ppu_stdcx64(ea, ctx->reserve_value, v) != 0;
}
inline void lwsync() { PPU_FENCE(acq_rel); }

/* slw and cntlzw */
inline uint32_t shl32(uint32_t v, uint32_t by) { return (by & 0x3F) < 32 ? v << (by & 0x3F) : 0; }
inline uint32_t clz32(uint32_t v)
{
    uint32_t n = 0;
    for (uint32_t bit = 0x80000000u; bit && !(v & bit); bit >>= 1) n++;
    return n;
}

/* ---- spurs_queue.cpp: cellSpursQueue ---- */
uint32_t queue_initialize(uint32_t spurs, uint32_t taskset, uint32_t q, uint32_t buffer, uint32_t size, uint32_t depth,
                          uint32_t direction);
uint32_t queue_push(ppu_context* ctx, uint32_t q, uint32_t item);              /* without blocking */
uint32_t queue_pop(ppu_context* ctx, uint32_t q, uint32_t item, bool peek);    /* without blocking */

/* ---- spurs_lfqueue.cpp: cellSyncLFQueue's push ---- */
uint32_t lfq_get_push_pointer(ppu_context* ctx, uint32_t q, int32_t* pointer);    /* without blocking */
uint32_t lfq_complete_push_pointer(ppu_context* ctx, uint32_t q, int32_t pointer);
uint32_t lfq_get_push_pointer2(ppu_context* ctx, uint32_t q, int32_t* pointer);   /* an ANY2ANY queue's */
uint32_t lfq_complete_push_pointer2(ppu_context* ctx, uint32_t q, int32_t pointer);

/* What the routines send: a signal to an SPU task waiting on a queue. */
uint32_t spurs_send_signal(uint32_t taskset, uint32_t task);                             /* _cellSpursSendSignal */
uint32_t spurs_workload_taskset(uint32_t spurs, uint32_t* taskset, uint32_t workload);   /* the taskset of a workload */
uint32_t lfq_send_signal(uint32_t ea_signal, uint32_t id);                               /* an LFQueue's fpSendSignal */

/* One of libsre's own checks failed: it prints and stops the process, and
 * so does this. */
void check_failed(const char* what);

/* The comparison with libsre's own code (sre_check.cpp) sets this while it
 * runs: signals and failed checks are noted, not acted on. */
struct Probe {
    uint32_t (*signal)(int kind, uint32_t a, uint32_t b);   /* 0 SPURS signal, 1 workload's taskset, 2 LFQueue signal */
    void (*failed)(const char* what);
};
extern Probe* g_probe;

#ifdef DOD3_FW_LIBSRE
/* A build that holds libsre's routines too (sre_check.cpp). */
struct Firmware {
    uint32_t (*queue_initialize)(ppu_context* ctx, uint32_t spurs, uint32_t taskset, uint32_t q, uint32_t buffer,
                                 uint32_t size, uint32_t depth, uint32_t direction);
    uint32_t (*queue_push)(ppu_context* ctx, uint32_t q, uint32_t item);
    uint32_t (*queue_pop)(ppu_context* ctx, uint32_t q, uint32_t item, bool peek);
    uint32_t (*lfq_get_push_pointer)(ppu_context* ctx, uint32_t q, int32_t* pointer);
    uint32_t (*lfq_complete_push_pointer)(ppu_context* ctx, uint32_t q, int32_t pointer);
    uint32_t (*lfq_get_push_pointer2)(ppu_context* ctx, uint32_t q, int32_t* pointer);
    uint32_t (*lfq_complete_push_pointer2)(ppu_context* ctx, uint32_t q, int32_t pointer);
};
extern Firmware* g_firmware;                  /* DOD3_LIBSRE=fw: the title's calls go to libsre's */
extern void (*g_first_queue)(ppu_context*);   /* run once, when the title makes its first queue */
#endif
/* The title makes a queue: the first time, a build with libsre's routines
 * reads its switches (and runs the comparison, if asked). */
inline void queue_made(ppu_context* ctx)
{
#ifdef DOD3_FW_LIBSRE
    if (void (*f)(ppu_context*) = g_first_queue) {
        g_first_queue = nullptr;
        f(ctx);
    }
#else
    (void)ctx;
#endif
}
/* A routine by its name: libsre's when the build has them and DOD3_LIBSRE=fw asks. */
#ifdef DOD3_FW_LIBSRE
#define SRE_CALL(fn, ...) (sre::g_firmware ? sre::g_firmware->fn(__VA_ARGS__) : sre::fn(__VA_ARGS__))
#else
#define SRE_CALL(fn, ...) sre::fn(__VA_ARGS__)
#endif

}  // namespace sre
