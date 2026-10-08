/*
 * Shared by the native SPU fast paths (dod3_spu_hooks.c, dod3_msdsp_hooks.c,
 * dod3_mp3_native.c): running a stretch of lifted SPU code to a pc, and the
 * check mode that runs a hooked stretch twice -- native path taken, then the
 * lifted code alone -- and compares the two.
 */
#pragma once
#include "spu_context.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Run from `body` until the lifted code reaches `stop`; the stretch branches
 * only among lifted functions. */
static inline void dod3_spu_run_to(spu_context* ctx, void (*body)(spu_context*), uint32_t stop)
{
    body(ctx);
    while (g_spu_trampoline_fn && ((uint32_t)ctx->pc & SPU_LS_MASK) != stop) {
        void (*f)(spu_context*) = g_spu_trampoline_fn;
        g_spu_trampoline_fn = 0;
        f(ctx);
    }
}

/* The check runs' scratch: the registers and the local store before and
 * after. One per thread, allocated the first time a check runs -- as
 * thread-local arrays its 2 MB would be part of every thread. */
typedef struct {
    u128 g0[128], ga[128];
    uint8_t l0[SPU_LS_SIZE], la[SPU_LS_SIZE];
} dod3_spu_scratch;

static inline dod3_spu_scratch* dod3_spu_check_scratch(void)
{
    static _Thread_local dod3_spu_scratch* t;
    if (!t && !(t = (dod3_spu_scratch*)malloc(sizeof *t))) {
        fprintf(stderr, "[spu-native-check] out of memory\n");
        abort();
    }
    return t;
}

/* Run body..stop twice from the current state: first with *mode = `with`
 * (the hooks active), then with *mode = `plain` (the lifted code alone).
 * Leaves the second run's state; 1 when the two differ in any register, the
 * local store, the pc or the next transfer. */
static inline int dod3_spu_check_both(spu_context* ctx, void (*body)(spu_context*), uint32_t stop, int* mode, int with,
                                      int plain)
{
    dod3_spu_scratch* c = dod3_spu_check_scratch();
    memcpy(c->g0, ctx->gpr, sizeof c->g0);
    memcpy(c->l0, ctx->ls, SPU_LS_SIZE);
    const uint32_t pc0 = (uint32_t)ctx->pc;
    *mode = with;
    dod3_spu_run_to(ctx, body, stop);
    *mode = 0;
    memcpy(c->ga, ctx->gpr, sizeof c->ga);
    memcpy(c->la, ctx->ls, SPU_LS_SIZE);
    const uint32_t pca = (uint32_t)ctx->pc;
    void (*tfa)(spu_context*) = g_spu_trampoline_fn;
    memcpy(ctx->gpr, c->g0, sizeof c->g0);
    memcpy(ctx->ls, c->l0, SPU_LS_SIZE);
    ctx->pc = pc0;
    *mode = plain;
    dod3_spu_run_to(ctx, body, stop);
    *mode = 0;
    return memcmp(c->ga, ctx->gpr, sizeof c->ga) || memcmp(c->la, ctx->ls, SPU_LS_SIZE) || pca != (uint32_t)ctx->pc ||
           tfa != g_spu_trampoline_fn;
}
