/*
 * dod3_spu_hooks.c -- native fast paths inside lifted SPU code.
 *
 * Each hook is called first thing in one lifted function (tools/lift_spu.sh,
 * --native-hook) and returns 1 when it has done that function's work, 0 to
 * fall through into the lifted body.
 *
 * ShaderPatching (spurs_job_01785E00) runs ~200 times a frame, inline on the
 * render thread (SPURS_JC_SYNC), and most of each run is an LZF decompressor
 * moving one byte per iteration through two 16-byte local-store accesses, a
 * rotate and a shuffle. Its two copy loops are hooked here: the hook copies
 * every byte but the last natively and advances the loop's registers by the
 * same count -- each iteration adds 1 (or -1) to every lane of them -- then
 * falls through, so the lifted loop runs the final iteration itself and every
 * register, temporaries included, ends exactly as it always did.
 *
 * DOD3_SPU_NATIVE=0 turns the hooks off. DOD3_SPU_NATIVE_CHECK=1 runs each
 * hooked loop twice from the same state -- fast-forwarded, then fully lifted --
 * compares every register, the bytes it wrote, the pc and the next transfer,
 * and keeps the lifted result.
 */
#include "spu_context.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void spurs_job_01785E00_spu_func_00000528(spu_context* ctx);
void spurs_job_01785E00_spu_func_00000560(spu_context* ctx);
void spurs_job_01785E00_spu_func_00000638(spu_context* ctx);

static _Thread_local int t_in_check;      /* 1: fast-forward run, 2: plain run */
static unsigned long long s_checked, s_bad;
static void check_summary(void)
{
    fprintf(stderr, "[spu-native-check] %llu loops compared, %llu mismatched\n", s_checked, s_bad);
}
static int checking(void)
{
    static int on = -1;
    if (on < 0) { on = getenv("DOD3_SPU_NATIVE_CHECK") ? 1 : 0; if (on) atexit(check_summary); }
    return on;
}

/* Run fn from the current state twice and compare; leaves the plain run's
 * state. dst/n: the window the loop writes. */
static void check_run(spu_context* ctx, void (*fn)(spu_context*), uint32_t dst, uint32_t n)
{
    static _Thread_local u128 g0[128], ga[128];
    static _Thread_local uint8_t w0[SPU_LS_SIZE], wa[SPU_LS_SIZE];
    memcpy(g0, ctx->gpr, sizeof g0);
    for (uint32_t i = 0; i < n; i++) w0[i] = ctx->ls[(dst + i) & SPU_LS_MASK];
    const uint32_t pc0 = (uint32_t)ctx->pc;
    t_in_check = 1; fn(ctx); t_in_check = 0;
    memcpy(ga, ctx->gpr, sizeof ga);
    for (uint32_t i = 0; i < n; i++) wa[i] = ctx->ls[(dst + i) & SPU_LS_MASK];
    const uint32_t pca = (uint32_t)ctx->pc; void (*tfa)(spu_context*) = g_spu_trampoline_fn;
    memcpy(ctx->gpr, g0, sizeof g0);
    for (uint32_t i = 0; i < n; i++) ctx->ls[(dst + i) & SPU_LS_MASK] = w0[i];
    ctx->pc = pc0;
    t_in_check = 2; fn(ctx); t_in_check = 0;
    int bad = memcmp(ga, ctx->gpr, sizeof ga) != 0 || pca != (uint32_t)ctx->pc ||
              tfa != g_spu_trampoline_fn;
    for (uint32_t i = 0; i < n && !bad; i++) bad = wa[i] != ctx->ls[(dst + i) & SPU_LS_MASK];
    s_checked++;
    if ((s_checked % 20000) == 0) check_summary();
    if (bad && ++s_bad <= 8)
        fprintf(stderr, "[spu-native-check] MISMATCH at pc 0x%X (n=%u)\n", pc0 & SPU_LS_MASK, n);
}

static int hooks_on(void)
{
    static int on = -1;
    if (on < 0) { const char* e = getenv("DOD3_SPU_NATIVE"); on = !(e && e[0] == '0'); }
    /* A local-store watch or probe sees every access the lifted code makes;
     * a native copy would hide them. */
    return on && !g_spu_ls_watch_n && !g_spu_smc_watch && !g_spu_ls_probe;
}

static void ls_copy_forward(uint8_t* ls, uint32_t dst, uint32_t src, uint32_t n)
{
    /* Byte by byte and forward, as the loop does: an LZF back-reference may
     * overlap its own output. */
    for (uint32_t i = 0; i < n; i++)
        ls[(dst + i) & SPU_LS_MASK] = ls[(src + i) & SPU_LS_MASK];
}

static void lanes_add(u128* r, uint32_t d)
{
    for (int k = 0; k < 4; k++) r->_u32[k] += d;
}

/* 0x528, a literal run: copy LS[r17++] -> LS[r16++] until r17 == r7. */
int dod3_spu_lzf_literal_hook(spu_context* ctx)
{
    if (t_in_check == 2 || !hooks_on()) return 0;
    const uint32_t src = ctx->gpr[17]._u32[0], dst = ctx->gpr[16]._u32[0];
    const uint32_t n = ctx->gpr[7]._u32[0] - src;       /* iterations the loop makes */
    if (n < 2 || n > SPU_LS_SIZE) return 0;
    if (!t_in_check && checking()) {
        check_run(ctx, spurs_job_01785E00_spu_func_00000528, dst & ~0xFu, n + 32);
        return 1;
    }
    ls_copy_forward(ctx->ls, dst, src, n - 1);
    lanes_add(&ctx->gpr[17], n - 1);
    lanes_add(&ctx->gpr[16], n - 1);
    return 0;
}

/* 0x638, the rest of a back-reference: copy LS[r13++] -> LS[r7++], r18 times. */
int dod3_spu_lzf_match_hook(spu_context* ctx)
{
    if (t_in_check == 2 || !hooks_on()) return 0;
    const uint32_t src = ctx->gpr[13]._u32[0], dst = ctx->gpr[7]._u32[0];
    const uint32_t n = ctx->gpr[18]._u32[0];
    if (n < 2 || n > SPU_LS_SIZE) return 0;
    if (!t_in_check && checking()) {
        check_run(ctx, spurs_job_01785E00_spu_func_00000638, dst & ~0xFu, n + 32);
        return 1;
    }
    ls_copy_forward(ctx->ls, dst, src, n - 1);
    lanes_add(&ctx->gpr[13], n - 1);
    lanes_add(&ctx->gpr[7], n - 1);
    lanes_add(&ctx->gpr[18], (uint32_t)-(int32_t)(n - 1));
    return 0;
}

/* ---- 0x560: whole tokens --------------------------------------------------
 *
 * 0x560 starts one LZF token: r17 = input, r16 = output, and the decoder
 * continues while r23 > r17 (unsigned). A token reads nothing of the tokens
 * before it but r16, r17 and local store; every other register it touches
 * it writes first. So this decodes natively every token before the last
 * literal run and the last back-reference, leaves r16 and r17 exactly as the
 * lifted code would -- every lane of them -- and returns 0: the lifted code
 * runs the rest, including the last token of each kind, which is what leaves
 * every other register as it always was.
 *
 * The lanes: a literal run moves all four lanes of r16 and r17 by its length.
 * A back-reference moves lane k by amounts the lifted code derives from the
 * byte four*k along the ctrl byte's 16-byte line (and, for the length, the
 * next byte's line) -- garbage outside lane 0, but deterministic garbage. */
static uint8_t ls_line_byte(const uint8_t* ls, uint32_t a, uint32_t k)
{
    /* Byte 3 of word k of rotqby(LS128[a & ~15], a + 13): the byte 4k past a,
     * wrapping within a's 16-byte line. */
    return ls[((a & ~0xFu) + ((a + 4u * k) & 0xFu)) & SPU_LS_MASK];
}

static int lzf_token_native(spu_context* ctx, uint32_t* out_tokens)
{
    uint8_t* ls = ctx->ls;
    const uint32_t end = ctx->gpr[23]._u32[0];
    uint32_t ip = ctx->gpr[17]._u32[0];
    /* Pass 1: where are the last literal run and the last back-reference? */
    uint32_t n = 0, last_lit = UINT32_MAX, last_ref = UINT32_MAX, p = ip;
    for (;;) {
        const uint32_t c = ls[p & SPU_LS_MASK];
        if (c < 32) { last_lit = n; p += c + 2; }
        else { last_ref = n; p += ((c >> 5) == 7) ? 3 : 2; }
        n++;
        if (!(end > p)) break;
        if (n > SPU_LS_SIZE) return 0;
    }
    const uint32_t stop = last_lit < last_ref ? last_lit : last_ref;
    if (!stop || stop == UINT32_MAX) return 0;
    /* Pass 2: decode tokens [0, stop). */
    u128 r16 = ctx->gpr[16], r17 = ctx->gpr[17];
    for (uint32_t t = 0; t < stop; t++) {
        const uint32_t c = ls[ip & SPU_LS_MASK];
        const uint32_t op = r16._u32[0];
        if (c < 32) {
            const uint32_t len = c + 1;
            ls_copy_forward(ls, op, ip + 1, len);
            lanes_add(&r16, len);
            lanes_add(&r17, len + 1);
            ip += len + 1;
        } else {
            uint32_t len = c >> 5, q = ip + 1;
            if (len == 7) { len += ls[q & SPU_LS_MASK]; q++; }
            const uint32_t ref = op - ((c & 31u) << 8) - 1u - ls[q & SPU_LS_MASK];
            for (uint32_t k = 0; k < 4; k++) {
                const uint32_t r8 = ls_line_byte(ls, ip, k) >> 5;
                const uint32_t cnt = r8 != 7 ? r8 : (uint32_t)ls_line_byte(ls, ip + 1, k) + 7u;
                r16._u32[k] += 2u + cnt;
                r17._u32[k] += r8 != 7 ? 2u : 3u;
            }
            ls_copy_forward(ls, op, ref, len + 2);
            ip = q + 1;
        }
    }
    ctx->gpr[16] = r16;
    ctx->gpr[17] = r17;
    *out_tokens = stop;
    return 1;
}

static unsigned long long s_tok_checked, s_tok_bad;
/* Run the decoder from 0x560 to its exit at 0x678 through the lifted
 * functions alone (they only branch among themselves). */
static void lzf_run_lifted(spu_context* ctx)
{
    spurs_job_01785E00_spu_func_00000560(ctx);
    while (g_spu_trampoline_fn && ((uint32_t)ctx->pc & SPU_LS_MASK) != 0x678u) {
        void (*f)(spu_context*) = g_spu_trampoline_fn;
        g_spu_trampoline_fn = 0;
        f(ctx);
    }
}

int dod3_spu_lzf_token_hook(spu_context* ctx)
{
    if (t_in_check == 2 || !hooks_on()) return 0;
    if (t_in_check == 0 && checking()) {
        /* Compare the whole decode, both ways, then keep the lifted one. The
         * output window is bounded by the remaining input's decoded size. */
        static _Thread_local u128 g0[128], ga[128];
        static _Thread_local uint8_t l0[SPU_LS_SIZE], la[SPU_LS_SIZE];
        memcpy(g0, ctx->gpr, sizeof g0); memcpy(l0, ctx->ls, SPU_LS_SIZE);
        const uint32_t pc0 = (uint32_t)ctx->pc;
        t_in_check = 3; lzf_run_lifted(ctx); t_in_check = 0;   /* 3: hooks active */
        memcpy(ga, ctx->gpr, sizeof ga); memcpy(la, ctx->ls, SPU_LS_SIZE);
        const uint32_t pca = (uint32_t)ctx->pc; void (*tfa)(spu_context*) = g_spu_trampoline_fn;
        memcpy(ctx->gpr, g0, sizeof g0); memcpy(ctx->ls, l0, SPU_LS_SIZE); ctx->pc = pc0;
        t_in_check = 2; lzf_run_lifted(ctx); t_in_check = 0;
        const int bad = memcmp(ga, ctx->gpr, sizeof ga) || memcmp(la, ctx->ls, SPU_LS_SIZE) ||
                        pca != (uint32_t)ctx->pc || tfa != g_spu_trampoline_fn;
        s_tok_checked++;
        if ((s_tok_checked % 2000) == 0 || (bad && s_tok_bad < 8))
            fprintf(stderr, "[spu-native-check] lzf decodes: %llu compared, %llu mismatched%s\n",
                    s_tok_checked, s_tok_bad + (bad ? 1 : 0), bad ? " -- MISMATCH" : "");
        if (bad) s_tok_bad++;
        return 1;   /* the lifted result stands; the trampoline is set */
    }
    uint32_t tokens = 0;
    lzf_token_native(ctx, &tokens);
    return 0;
}
