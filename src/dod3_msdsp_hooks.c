/*
 * dod3_msdsp_hooks.c -- native fast paths in MultiStream's DSP plugin.
 *
 * The MultiStream mixer (SPU task image 1) loads a block of DSP plugins at LS
 * 0x37000 (tools/make_spu_overlays.py lifts it as spu_ovl_msdsp_37000). Two
 * loops in it are half of that task's time, and the task runs at
 * user-interactive QoS in scenes that have every core busy. Both are the
 * same biquad over a 512-sample block,
 *
 *     y = c0*x + c1*x1 + c2*x2 + c3*y1 + c4*y2
 *
 * compiled without optimisation: every variable lives in the stack frame and
 * is reloaded each sample, so the lifted body is ~40 local-store accesses and
 * a trip through the dispatcher per sample.
 *
 *   0x39350 (count at sp+0x30, test at 0x3957C): the filter state is an array
 *            [x1, x2, y1, y2] at the pointer in sp+0x70
 *   0x39630 (count at sp+0x20, test at 0x397D8): the same filter with the
 *            state copied into the frame at sp+0xA0
 * Frame slots: sp+0x50 coefficients c0..c4, sp+0x80 out, sp+0x90 in.
 *
 * Each hook does every sample but the last natively -- the same reads and
 * writes of local store in the same order, so an in-place block (out == in)
 * comes out the same -- then falls through and the lifted body runs the last
 * one, which leaves every register as it always was. The arithmetic is the
 * lifted code's: single-precision multiplies and adds in its order, never
 * fused (spu_fm/spu_fa are plain float operations).
 *
 * DOD3_MSDSP_NATIVE=0 turns the hooks off (as does DOD3_SPU_NATIVE=0).
 * DOD3_SPU_NATIVE_CHECK=1 runs each hooked loop from the same state with the
 * hook and without it, to its exit, compares every register, the whole local
 * store, the pc and the next transfer, and keeps the lifted result.
 */
#include "spu_context.h"
#include "spu_helpers.h"
#include "dod3_spu_check.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#pragma STDC FP_CONTRACT OFF
#if defined(__clang__)
#pragma clang fp contract(off)
#endif

void spu_ovl_msdsp_37000_spu_func_00039350(spu_context* ctx);
void spu_ovl_msdsp_37000_spu_func_00039630(spu_context* ctx);

/* DOD3_AB-style run-time switch; -1 undecided. */
int g_dod3_msdsp_hook = -1;

static _Thread_local int t_mode;   /* 0 normal, 1 check run with the hook, 2 check run without */

static int msdsp_on(void)
{
    if (g_dod3_msdsp_hook < 0) {
        const char* e = getenv("DOD3_MSDSP_NATIVE");
        const char* a = getenv("DOD3_SPU_NATIVE");
        g_dod3_msdsp_hook = !((e && e[0] == '0') || (a && a[0] == '0'));
    }
    /* A local-store watch or probe sees every access the lifted code makes. */
    return g_dod3_msdsp_hook && !g_spu_ls_watch_n && !g_spu_smc_watch && !g_spu_ls_probe;
}
static int msdsp_checking(void)
{
    static int on = -1;
    if (on < 0) on = getenv("DOD3_SPU_NATIVE_CHECK") ? 1 : 0;
    return on;
}

/* Local store is big-endian; a word at a 4-aligned address. */
static inline uint32_t rd32(const uint8_t* ls, uint32_t a)
{
    const uint8_t* p = ls + (a & ~3u & SPU_LS_MASK);
    return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
}
static inline void wr32(uint8_t* ls, uint32_t a, uint32_t w)
{
    uint8_t* p = ls + (a & ~3u & SPU_LS_MASK);
    p[0] = (uint8_t)(w >> 24);
    p[1] = (uint8_t)(w >> 16);
    p[2] = (uint8_t)(w >> 8);
    p[3] = (uint8_t)w;
}
static inline float rdf(const uint8_t* ls, uint32_t a)
{
    const uint32_t w = rd32(ls, a);
    float f;
    memcpy(&f, &w, 4);
    return f;
}
static inline void wrf(uint8_t* ls, uint32_t a, float f)
{
    uint32_t w;
    memcpy(&w, &f, 4);
    wr32(ls, a, w);
}

/* 0x39350: state in the array at sp+0x70. Every sample before the last. */
static void biquad_a_forward(spu_context* ctx)
{
    uint8_t* ls = ctx->ls;
    const uint32_t sp = ctx->gpr[1]._u32[0] & SPU_LS_MASK;
    int32_t i = (int32_t)rd32(ls, sp + 0x30);
    if (i >= 511) return;
    const uint32_t c = rd32(ls, sp + 0x50), s = rd32(ls, sp + 0x70);
    uint32_t out = rd32(ls, sp + 0x80), in = rd32(ls, sp + 0x90);
    if ((c | s | out | in) & 3u) return;          /* the lifted code then */
    for (; i < 511; i++) {
        float t, y;
        y = rdf(ls, c) * rdf(ls, in);
        t = rdf(ls, c + 4) * rdf(ls, s);
        y = y + t;
        t = rdf(ls, c + 8) * rdf(ls, s + 4);
        y = y + t;
        t = rdf(ls, c + 12) * rdf(ls, s + 8);
        y = y + t;
        t = rdf(ls, c + 16) * rdf(ls, s + 12);
        y = y + t;
        wrf(ls, out, y);
        wr32(ls, s + 4, rd32(ls, s));             /* x2 = x1 */
        wr32(ls, s, rd32(ls, in));                /* x1 = x (re-read: out may be in) */
        in += 4;
        wr32(ls, s + 12, rd32(ls, s + 8));        /* y2 = y1 */
        wr32(ls, s + 8, rd32(ls, out));           /* y1 = y  */
        out += 4;
    }
    wr32(ls, sp + 0x30, (uint32_t)i);
    wr32(ls, sp + 0x80, out);
    wr32(ls, sp + 0x90, in);
}

/* 0x39630: state in the frame at sp+0xA0. Every sample before the last. */
static void biquad_b_forward(spu_context* ctx)
{
    uint8_t* ls = ctx->ls;
    const uint32_t sp = ctx->gpr[1]._u32[0] & SPU_LS_MASK;
    int32_t j = (int32_t)rd32(ls, sp + 0x20);
    if (j >= 511) return;
    const uint32_t c = rd32(ls, sp + 0x50), st = sp + 0xA0;
    uint32_t out = rd32(ls, sp + 0x80), in = rd32(ls, sp + 0x90);
    if ((c | out | in) & 3u) return;
    for (; j < 511; j++) {
        float t, y;
        y = rdf(ls, c) * rdf(ls, in);
        t = rdf(ls, c + 4) * rdf(ls, st);
        y = y + t;
        t = rdf(ls, c + 8) * rdf(ls, st + 4);
        y = y + t;
        t = rdf(ls, c + 12) * rdf(ls, st + 8);
        y = y + t;
        t = rdf(ls, c + 16) * rdf(ls, st + 12);
        y = y + t;
        wrf(ls, out, y);
        wr32(ls, st + 4, rd32(ls, st));
        wr32(ls, st, rd32(ls, in));
        in += 4;
        wr32(ls, st + 12, rd32(ls, st + 8));
        wr32(ls, st + 8, rd32(ls, out));
        out += 4;
    }
    wr32(ls, sp + 0x20, (uint32_t)j);
    wr32(ls, sp + 0x80, out);
    wr32(ls, sp + 0x90, in);
}

static unsigned long long s_checked, s_bad;
/* Run a hooked loop from its body to `stop` both ways and compare. */
static int check(spu_context* ctx, void (*body)(spu_context*), uint32_t stop)
{
    const int bad = dod3_spu_check_both(ctx, body, stop, &t_mode, 1, 2);
    s_checked++;
    if ((s_checked % 4000) == 0 || (bad && s_bad < 8))
        fprintf(stderr, "[spu-native-check] msdsp biquads: %llu compared, %llu mismatched%s\n", s_checked,
                s_bad + (bad ? 1 : 0), bad ? " -- MISMATCH" : "");
    if (bad) s_bad++;
    return 1;   /* the lifted result stands; the trampoline is set */
}

int dod3_msdsp_biquad_a_hook(spu_context* ctx)
{
    if (t_mode == 2 || !msdsp_on()) return 0;
    if (t_mode == 0 && msdsp_checking()) return check(ctx, spu_ovl_msdsp_37000_spu_func_00039350, 0x397D8u);
    biquad_a_forward(ctx);
    return 0;   /* the lifted body runs the last sample */
}

int dod3_msdsp_biquad_b_hook(spu_context* ctx)
{
    if (t_mode == 2 || !msdsp_on()) return 0;
    if (t_mode == 0 && msdsp_checking()) return check(ctx, spu_ovl_msdsp_37000_spu_func_00039630, 0x397ECu);
    biquad_b_forward(ctx);
    return 0;
}
