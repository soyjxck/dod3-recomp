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
#include "spu_helpers.h"   /* spu_ls_read128 */
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

static int lzf_decode_native(spu_context* ctx, uint32_t* out_tokens, uint32_t* back);

/* ---- decode memo -----------------------------------------------------------
 * The jobs decompress the same shaders over and over (every draw of a mesh
 * patches its fragment program again; a broken-up scene draws the same
 * debris shaders hundreds of times a frame). What lzf_decode_native does is a
 * function of the bytes it reads alone: the input window [ip & ~15, end
 * rounded up + 16) -- the scan reads to `end`, and the lane bookkeeping reads
 * the 16-byte lines of the control bytes -- and the output that precedes it:
 * the shaders are decoded block after block into one buffer, and a block's
 * back-references reach into the blocks before it (`back` bytes before its
 * output; at most 8 KiB in LZF). As long as the output does not overlap the
 * window, a decode is kept, keyed by the window's bytes and the input's place
 * in it, as the bytes it wrote, the `back` bytes before them, and what it
 * added to every lane of r16 and r17; a later decode of the same window, with
 * the same bytes before its own output, writes the same bytes and adds the
 * same amounts. Hits compare the whole window and the preceding bytes, not
 * just the hash. DOD3_LZF_MEMO=0 turns it off;
 * DOD3_SPU_NATIVE_CHECK=1 checks it along with the hooks. */
typedef struct {
    uint64_t hash;
    uint32_t in_off, span, win_len, out_len, stop, back;
    uint32_t d16[4], d17[4];
    uint8_t* bytes;          /* the window, the output, the `back` bytes before it */
} lzf_memo;
#define LZF_MEMO_N 4096
static _Thread_local lzf_memo t_memo[LZF_MEMO_N];
static unsigned long long s_memo_hit, s_memo_miss, s_memo_skip, s_memo_outside, s_memo_overlap, s_memo_stored;

/* Switchable at run time by main.cpp's DOD3_AB=lzfmemo. */
int g_dod3_lzf_memo = -1;
static int memo_on(void)
{
    if (g_dod3_lzf_memo < 0) { const char* e = getenv("DOD3_LZF_MEMO"); g_dod3_lzf_memo = !(e && e[0] == '0'); }
    return g_dod3_lzf_memo;
}

static void memo_report(void)
{
    static int lg = -1;
    if (lg < 0) lg = getenv("DOD3_LZF_MEMO_LOG") ? 1 : 0;
    const unsigned long long n = s_memo_hit + s_memo_miss;
    if (lg && n && (n % 50000) == 0)
        fprintf(stderr, "[lzf-memo] %llu decodes: %llu hits (%.1f%%), %llu misses (%llu stored, %llu reaching too far back, %llu overlapping), %llu not cacheable\n",
                n, s_memo_hit, 100.0 * (double)s_memo_hit / (double)n, s_memo_miss,
                s_memo_stored, s_memo_outside, s_memo_overlap, s_memo_skip);
}

static uint64_t memo_hash(const uint8_t* p, uint32_t n, uint64_t h)
{
    uint32_t i = 0;
    for (; i + 8 <= n; i += 8) {
        uint64_t v; memcpy(&v, p + i, 8);
        h = (h ^ v) * 0x9E3779B97F4A7C15ull;
        h ^= h >> 29;
    }
    for (; i < n; i++) h = (h ^ p[i]) * 0x100000001B3ull;
    return h ^ (h >> 32);
}

static int lzf_token_native(spu_context* ctx, uint32_t* out_tokens)
{
    if (!memo_on()) { uint32_t back; return lzf_decode_native(ctx, out_tokens, &back); }
    uint8_t* ls = ctx->ls;
    const uint32_t ip0 = ctx->gpr[17]._u32[0], end = ctx->gpr[23]._u32[0], op0 = ctx->gpr[16]._u32[0];
    const uint32_t ws = ip0 & ~0xFu;
    const uint32_t we = ((end + 15u) & ~0xFu) + 16u;
    if (!(end > ip0) || we <= ws || we > SPU_LS_SIZE || ip0 >= SPU_LS_SIZE || (we - ws) > 0x10000u) {
        s_memo_skip++;
        uint32_t back; return lzf_decode_native(ctx, out_tokens, &back);
    }
    const uint32_t win_len = we - ws, in_off = ip0 - ws, span = end - ip0;
    const uint64_t h = memo_hash(ls + ws, win_len, 0xCBF29CE484222325ull ^ ((uint64_t)in_off << 32) ^ span);
    lzf_memo* m = &t_memo[h & (LZF_MEMO_N - 1)];
    if (m->bytes && m->hash == h && m->in_off == in_off && m->span == span && m->win_len == win_len &&
        op0 + m->out_len <= SPU_LS_SIZE && !(op0 < we && op0 + m->out_len > ws) && op0 >= m->back &&
        memcmp(m->bytes, ls + ws, win_len) == 0 &&
        memcmp(m->bytes + win_len + m->out_len, ls + op0 - m->back, m->back) == 0) {
        memcpy(ls + op0, m->bytes + win_len, m->out_len);
        for (int k = 0; k < 4; k++) { ctx->gpr[16]._u32[k] += m->d16[k]; ctx->gpr[17]._u32[k] += m->d17[k]; }
        *out_tokens = m->stop;
        s_memo_hit++; memo_report();
        return 1;
    }
    const u128 r16_0 = ctx->gpr[16], r17_0 = ctx->gpr[17];
    uint32_t back = 0;
    const int ok = lzf_decode_native(ctx, out_tokens, &back);
    s_memo_miss++; memo_report();
    if (!ok) return ok;
    if (back > 0x2400u || back > op0) { s_memo_outside++; return ok; }
    const uint32_t out_len = ctx->gpr[16]._u32[0] - op0;
    /* Keep it only if the output sits clear of the window and of the end. */
    if (out_len > 0x40000u || op0 + out_len > SPU_LS_SIZE || (op0 < we && op0 + out_len > ws)) { s_memo_overlap++; return ok; }
    s_memo_stored++;
    uint8_t* b = (uint8_t*)realloc(m->bytes, (size_t)win_len + out_len + back);
    if (!b) return ok;
    m->bytes = b;
    memcpy(b, ls + ws, win_len);
    memcpy(b + win_len, ls + op0, out_len);
    memcpy(b + win_len + out_len, ls + op0 - back, back);
    m->hash = h; m->in_off = in_off; m->span = span; m->win_len = win_len;
    m->out_len = out_len; m->stop = *out_tokens; m->back = back;
    for (int k = 0; k < 4; k++) {
        m->d16[k] = ctx->gpr[16]._u32[k] - r16_0._u32[k];
        m->d17[k] = ctx->gpr[17]._u32[k] - r17_0._u32[k];
    }
    return ok;
}

static int lzf_decode_native(spu_context* ctx, uint32_t* out_tokens, uint32_t* back)
{
    uint8_t* ls = ctx->ls;
    const uint32_t op_start = ctx->gpr[16]._u32[0];
    *back = 0;
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
            if (ref < op_start && op_start - ref > *back) *back = op_start - ref;   /* reads the output before this one */
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

/* ---- 0x6A0: the patch loop -------------------------------------------------
 *
 * After the decode, the job walks the fragment program 16 bytes (one
 * instruction) at a time: 0x6A0 swaps the halfwords of each of the
 * instruction's four words in place (RSX microcode order) and reads a copy
 * count from the byte at r21; 0x750 (re-entered through 0x744, which reloads
 * word 0) copies the swapped instruction, word by word, to r18 + each 16-bit
 * offset read from r84; 0x7E8 counts r22 up to r23 and steps r16. Every
 * register the iteration touches besides r16, r21, r22 and r84 it writes
 * first, so whole iterations are done here -- every one before the last that
 * copies anything -- and the lifted code runs from that one on, which leaves
 * every temporary exactly as it always did. The adds act on all four lanes
 * (spu_ai), so all four lanes advance.
 *
 * Quadword semantics are kept: a word is read from its address with a rotate
 * inside its 16-byte line (rotqby) and written into the word slot its address
 * names in that line (cwd/cwx + shufb). */
void spurs_job_01785E00_spu_func_000006A0(spu_context* ctx);

static uint32_t ls_word_rot(const uint8_t* ls, uint32_t a)
{
    const uint32_t line = a & ~0xFu & SPU_LS_MASK;
    uint32_t w = 0;
    for (uint32_t i = 0; i < 4; i++) w = (w << 8) | ls[line + ((a + i) & 0xFu)];
    return w;
}
static void ls_word_ins(uint8_t* ls, uint32_t a, uint32_t w)
{
    uint8_t* p = ls + (a & ~0x3u & SPU_LS_MASK);
    p[0] = (uint8_t)(w >> 24); p[1] = (uint8_t)(w >> 16); p[2] = (uint8_t)(w >> 8); p[3] = (uint8_t)w;
}
static uint32_t ls_half_at(const uint8_t* ls, uint32_t a)
{
    const uint32_t line = a & ~0xFu & SPU_LS_MASK;
    return ((uint32_t)ls[line + (a & 0xFu)] << 8) | ls[line + ((a + 1u) & 0xFu)];
}

static unsigned long long s_patch_checked, s_patch_bad;

static void patch_run_lifted(spu_context* ctx)
{
    spurs_job_01785E00_spu_func_000006A0(ctx);
    while (g_spu_trampoline_fn && ((uint32_t)ctx->pc & SPU_LS_MASK) != 0x808u) {
        void (*f)(spu_context*) = g_spu_trampoline_fn;
        g_spu_trampoline_fn = 0;
        f(ctx);
    }
}

/* DOD3_SPU_PATCH_HOOK=0 turns this hook alone off; DOD3_AB=patchhook
 * switches it in a run. */
int g_dod3_spu_patch_hook = -1;
int dod3_spu_patch_loop_hook(spu_context* ctx)
{
    if (g_dod3_spu_patch_hook < 0) { const char* e = getenv("DOD3_SPU_PATCH_HOOK"); g_dod3_spu_patch_hook = !(e && e[0] == '0'); }
    if (t_in_check == 2 || !hooks_on() || !g_dod3_spu_patch_hook) return 0;
    if (t_in_check == 0 && checking()) {
        static _Thread_local u128 g0[128], ga[128];
        static _Thread_local uint8_t l0[SPU_LS_SIZE], la[SPU_LS_SIZE];
        memcpy(g0, ctx->gpr, sizeof g0); memcpy(l0, ctx->ls, SPU_LS_SIZE);
        const uint32_t pc0 = (uint32_t)ctx->pc;
        t_in_check = 4; patch_run_lifted(ctx); t_in_check = 0;   /* this hook active */
        memcpy(ga, ctx->gpr, sizeof ga); memcpy(la, ctx->ls, SPU_LS_SIZE);
        const uint32_t pca = (uint32_t)ctx->pc; void (*tfa)(spu_context*) = g_spu_trampoline_fn;
        memcpy(ctx->gpr, g0, sizeof g0); memcpy(ctx->ls, l0, SPU_LS_SIZE); ctx->pc = pc0;
        t_in_check = 2; patch_run_lifted(ctx); t_in_check = 0;
        const int bad = memcmp(ga, ctx->gpr, sizeof ga) || memcmp(la, ctx->ls, SPU_LS_SIZE) ||
                        pca != (uint32_t)ctx->pc || tfa != g_spu_trampoline_fn;
        s_patch_checked++;
        if ((s_patch_checked % 2000) == 0 || (bad && s_patch_bad < 8))
            fprintf(stderr, "[spu-native-check] patch loops: %llu compared, %llu mismatched%s\n",
                    s_patch_checked, s_patch_bad + (bad ? 1 : 0), bad ? " -- MISMATCH" : "");
        if (bad) s_patch_bad++;
        return 1;   /* the lifted result stands */
    }
    uint8_t* ls = ctx->ls;
    const uint32_t r22 = ctx->gpr[22]._u32[0], r23 = ctx->gpr[23]._u32[0];
    if (!(r23 > r22)) return 0;
    const uint32_t n = r23 - r22;                     /* iterations left, this one included */
    if (n < 2 || n > 0x4000u) return 0;
    const uint32_t r21 = ctx->gpr[21]._u32[0];
    /* The last iteration that copies anything runs lifted (or the last one). */
    uint32_t L = n - 1;
    for (uint32_t t = n; t-- > 0; ) if (ls[(r21 + t) & SPU_LS_MASK]) { L = t; break; }
    if (L == 0) return 0;
    const uint32_t a0 = ctx->gpr[16]._u32[0], r18 = ctx->gpr[18]._u32[0];
    uint32_t r84 = ctx->gpr[84]._u32[0];
    uint32_t copies = 0;
    /* r66 is written only at 0x744 (second and later copies of an
     * instruction): if the lifted run from L never reaches it, it must hold
     * what the last skipped 0x744 loaded. */
    int r66_set = 0; u128 r66 = ctx->gpr[66];
    for (uint32_t t = 0; t < L; t++) {
        const uint32_t a = a0 + 16u * t;
        uint32_t w0 = 0;
        for (uint32_t k = 0; k < 4; k++) {
            const uint32_t w = ls_word_rot(ls, a + 4u * k);
            const uint32_t s = (w << 16) | (w >> 16);
            ls_word_ins(ls, a + 4u * k, s);
            if (k == 0) w0 = s;
        }
        const uint32_t c = ls[(r21 + t) & SPU_LS_MASK];
        for (uint32_t j = 0; j < c; j++) {
            if (j > 0) { r66 = spu_ls_read128(ctx, a); r66_set = 1; }   /* 0x744's lq */
            const uint32_t word0 = j == 0 ? w0 : ls_word_rot(ls, a);   /* 0x744 reloads it */
            const uint32_t dst = r18 + ls_half_at(ls, r84);
            r84 += 2u;
            ls_word_ins(ls, dst, word0);
            for (uint32_t k = 1; k < 4; k++)
                ls_word_ins(ls, dst + 4u * k, ls_word_rot(ls, a + 4u * k));
        }
        copies += c;
    }
    if (r66_set) ctx->gpr[66] = r66;
    for (int k = 0; k < 4; k++) {
        ctx->gpr[16]._u32[k] += 16u * L;
        ctx->gpr[21]._u32[k] += L;
        ctx->gpr[22]._u32[k] += L;
        ctx->gpr[84]._u32[k] += 2u * copies;
    }
    return 0;
}
