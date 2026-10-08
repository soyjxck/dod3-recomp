/*
 * dod3_mp3_native.c -- MultiStream's MP3 decoder, natively.
 *
 * MultiStream (SPU task image 1) decodes its MP3 streams with the firmware's
 * flashMP3.pic, loaded at LS 0x1A900 (tools/make_spu_overlays.py lifts it as
 * spu_ovl_mp3_1A900). The mixer calls its decodeFrame (0x21D20) once per
 * frame; that call is hooked here and can be served by our own decoder
 * (src/dod3_mp3dec.c) instead.
 *
 * decodeFrame(r3 frame, r4 &consumed, r5 &produced, r6 work area, r7..r11
 * stream state, r12 &word), as the firmware does it:
 *   - *consumed = *produced = 0, and the work area's 0x5800 bytes cleared;
 *   - "ID3" at the frame: an ID3v2 tag, skipped (consumed = 10 + its size;
 *     128 if the version byte is 1); "TAG": 128 bytes skipped. Returns 0.
 *   - a header it cannot parse: returns -1 (the mixer reports an error).
 *   - otherwise one frame: consumed = its length, the PCM as 16-bit big-
 *     endian planar channels at r6+0x900 (left) and r6+0x1900 (right),
 *     produced = bytes per channel (2 x samples), *r12 stepped, and returns
 *     r6+0x900. A Xing/Info header frame is decoded the same but reports
 *     produced = -1, so nothing of it is played.
 * The stream state lives in local store: the mixer DMAs 4992 bytes per
 * channel (0x34100 left, 0x35480 right) in and out around each stream's
 * call, plus the word at r12. Sony's decoder keeps its synthesis buffer at
 * r9/r10, its overlap at r7/r8 and the bit reservoir at r11; ours keeps its
 * own state in the same regions (host byte order, behind a magic word), so
 * the mixer moves it the same way:
 *   r9  (0x880): header 16 bytes + left synthesis history (2048)
 *   r7  (0x900): left overlap (2304)
 *   r11 (0x200): bit reservoir (512)
 *   r10 (0x880): right synthesis history; r8 (0x900): right overlap
 * A region without our magic is a stream start (the mixer zeroes a new
 * stream's state); for a mono stream the right-channel regions are not
 * MultiStream's to keep and are neither read nor written.
 *
 * Ours matches the reference decoders (mpg123, ffmpeg) to 1 LSB; Sony's
 * firmware decoder differs from all of them by up to ~130 LSB on music and
 * more on mono effects decoded interleaved with other streams.
 *
 * Measured on a 120 s run (1827 streams, 52k frames, music and effects):
 * ours is within 1 LSB of mpg123 on every sample; Sony's is 27 dB from it.
 * What the mixer sees (consumed, produced, return value) matched Sony's on
 * all 56k frames of a check run. 45 us per call against 210 for the lifted
 * firmware decoder.
 *
 * The game loads the decoder from /dev_flash/sys/external/flashMP3.pic. By
 * default that is our stand-in (src/dod3_mp3_standin.h, written out by
 * main.cpp): an SPU ELF of the same shape whose entry is
 * dod3_mp3_standin_hook, so nothing of the firmware is needed to build or
 * play. DOD3_MP3_NATIVE=0 loads Sony's file from a firmware dev_flash and
 * decodes with it (lifted; the build lifts it when it finds the firmware);
 * the check and recording modes below need it too.
 * DOD3_MP3_CHECK=1 runs Sony's decoder (its result stands) and ours side by
 * side on every frame -- each stream followed through Sony's state -- and
 * reports any difference in what the mixer sees (consumed, produced, return
 * value) and how far the PCM differs.
 * DOD3_MP3_DUMP=<file>[,<n>] records the first n calls (default 400): the
 * argument registers, the out-parameters and local store 0x2A880-0x37100
 * before and after each, for testing decoders offline.
 */
#include "spu_context.h"
#include "spu_helpers.h"
#include "dod3_mp3dec.h"
#include "dod3_spu_check.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* Sony's lifted decodeFrame, when the build found the firmware to lift it
 * (spu/spu_overlays.c, from tools/make_spu_overlays.py); 0 otherwise. */
extern void (*g_dod3_mp3_sony_entry)(void*);

static _Thread_local int t_inner;   /* 1 while the lifted decodeFrame runs under the hook */

#define WORK_BYTES  0x5800u
#define OUT_OFF     0x900u
#define OUT_R_OFF   0x1000u
#define STATE_MAGIC 0x444D3331u     /* "DM31" */

/* ---- local store ------------------------------------------------------------ */

static inline uint32_t ls_rd32(const uint8_t* ls, uint32_t a)
{
    const uint8_t* p = ls + (a & SPU_LS_MASK & ~3u);
    return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
}
static inline void ls_wr32(uint8_t* ls, uint32_t a, uint32_t v)
{
    uint8_t* p = ls + (a & SPU_LS_MASK & ~3u);
    p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16); p[2] = (uint8_t)(v >> 8); p[3] = (uint8_t)v;
}
static inline int ls_ok(uint32_t a, uint32_t n) { return a <= SPU_LS_SIZE && n <= SPU_LS_SIZE - a; }

typedef struct {
    uint32_t in, cons, prod, work, l_syn, l_ov, reserv, r_syn, r_ov, word;
} args_t;

static void get_args(const spu_context* ctx, args_t* a)
{
    a->in = ctx->gpr[3]._u32[0] & SPU_LS_MASK;
    a->cons = ctx->gpr[4]._u32[0] & SPU_LS_MASK;
    a->prod = ctx->gpr[5]._u32[0] & SPU_LS_MASK;
    a->work = ctx->gpr[6]._u32[0] & SPU_LS_MASK;
    a->l_ov = ctx->gpr[7]._u32[0] & SPU_LS_MASK;
    a->r_ov = ctx->gpr[8]._u32[0] & SPU_LS_MASK;
    a->l_syn = ctx->gpr[9]._u32[0] & SPU_LS_MASK;
    a->r_syn = ctx->gpr[10]._u32[0] & SPU_LS_MASK;
    a->reserv = ctx->gpr[11]._u32[0] & SPU_LS_MASK;
    a->word = ctx->gpr[12]._u32[0] & SPU_LS_MASK;
}

static int args_ok(const args_t* a)
{
    return ls_ok(a->in, 4) && ls_ok(a->cons, 4) && ls_ok(a->prod, 4) && ls_ok(a->work, WORK_BYTES) &&
           ls_ok(a->l_syn, 0x880) && ls_ok(a->l_ov, 0x900) && ls_ok(a->reserv, 0x200) &&
           ls_ok(a->r_syn, 0x880) && ls_ok(a->r_ov, 0x900) && ls_ok(a->word, 4);
}

/* ---- our state in the stream's regions -------------------------------------- */

typedef struct {
    uint32_t magic;
    int32_t synth_pos, reserv_len, nch;
} state_hdr;

static void state_load(const uint8_t* ls, const args_t* a, int nch, dod3_mp3* d)
{
    state_hdr h;
    memcpy(&h, ls + a->l_syn, sizeof h);
    if (h.magic != STATE_MAGIC || h.reserv_len < 0 || h.reserv_len > 511) {
        memset(d, 0, sizeof *d);   /* a stream start */
        return;
    }
    d->synth_pos = h.synth_pos & 15;
    d->reserv_len = h.reserv_len;
    memcpy(d->ch[0].synth, ls + a->l_syn + sizeof h, sizeof d->ch[0].synth);
    memcpy(d->ch[0].overlap, ls + a->l_ov, sizeof d->ch[0].overlap);
    memcpy(d->reserv, ls + a->reserv, sizeof d->reserv);
    if (nch == 2 && h.nch == 2) {
        memcpy(d->ch[1].synth, ls + a->r_syn, sizeof d->ch[1].synth);
        memcpy(d->ch[1].overlap, ls + a->r_ov, sizeof d->ch[1].overlap);
    } else {
        memset(&d->ch[1], 0, sizeof d->ch[1]);
    }
}

static void state_save(uint8_t* ls, const args_t* a, int nch, const dod3_mp3* d)
{
    const state_hdr h = { STATE_MAGIC, d->synth_pos, d->reserv_len, nch };
    memcpy(ls + a->l_syn, &h, sizeof h);
    memcpy(ls + a->l_syn + sizeof h, d->ch[0].synth, sizeof d->ch[0].synth);
    memcpy(ls + a->l_ov, d->ch[0].overlap, sizeof d->ch[0].overlap);
    memcpy(ls + a->reserv, d->reserv, sizeof d->reserv);
    if (nch == 2) {
        memcpy(ls + a->r_syn, d->ch[1].synth, sizeof d->ch[1].synth);
        memcpy(ls + a->r_ov, d->ch[1].overlap, sizeof d->ch[1].overlap);
    }
}

/* ---- decodeFrame, natively -------------------------------------------------- */

typedef struct {
    uint32_t ret, cons, prod;   /* what the mixer sees */
    int samples, nch;
    int16_t pcm[2][1152];
} result_t;

/* The firmware's decodeFrame on our decoder. `d` is the stream's state. */
static void decode_frame(uint8_t* ls, const args_t* a, dod3_mp3* d, result_t* r)
{
    const uint8_t* f = ls + a->in;
    r->ret = 0; r->cons = 0; r->prod = 0; r->samples = 0; r->nch = 0;
    if (f[0] == 'I' && f[1] == 'D' && f[2] == '3') {
        r->cons = f[3] == 1 ? 128u
                            : 10u + ((uint32_t)(f[6] & 0x7F) << 21 | (uint32_t)(f[7] & 0x7F) << 14 |
                                     (uint32_t)(f[8] & 0x7F) << 7 | (f[9] & 0x7F));
        return;
    }
    if (f[0] == 'T' && f[1] == 'A' && f[2] == 'G') { r->cons = 128; return; }
    dod3_mp3_info info;
    const int bytes = dod3_mp3_frame_bytes(f, &info);
    int16_t* out[2] = { r->pcm[0], r->pcm[1] };
    if (!bytes || !ls_ok(a->in, (uint32_t)bytes) || dod3_mp3_decode(d, f, out, &info) < 0) {
        r->ret = 0xFFFFFFFFu;
        return;
    }
    /* The firmware looks for a Xing/Info tag where an MPEG-1 frame's main
     * data would start, without allowing for a CRC. */
    const uint8_t* x = f + 4 + (info.channels == 1 ? 17 : 32);
    const int xing = (x[0] == 'X' && x[1] == 'i' && x[2] == 'n' && x[3] == 'g') ||
                     (x[0] == 'I' && x[1] == 'n' && x[2] == 'f' && x[3] == 'o');
    r->cons = (uint32_t)bytes;
    r->prod = xing ? 0xFFFFFFFFu : (uint32_t)(2 * info.samples);
    r->ret = a->work + OUT_OFF;
    r->samples = info.samples;
    r->nch = info.channels;
}

static void write_result(uint8_t* ls, const args_t* a, const result_t* r)
{
    memset(ls + a->work, 0, WORK_BYTES);
    for (int c = 0; c < r->nch; c++) {
        uint8_t* o = ls + a->work + OUT_OFF + (c ? OUT_R_OFF : 0);
        for (int i = 0; i < r->samples; i++) {
            o[2 * i] = (uint8_t)((uint16_t)r->pcm[c][i] >> 8);
            o[2 * i + 1] = (uint8_t)r->pcm[c][i];
        }
    }
    ls_wr32(ls, a->cons, r->cons);
    ls_wr32(ls, a->prod, r->ret == 0xFFFFFFFFu ? 0 : r->prod);
    if (r->samples) {
        /* the firmware's synthesis offset, stepped 18 per granule */
        const uint32_t bo = ls_rd32(ls, a->word);
        ls_wr32(ls, a->word, (bo - 18u * (uint32_t)(r->samples / 576)) & 15u);
    }
}

static int native_on(void)
{
    static int on = -1;
    if (on < 0) { const char* e = getenv("DOD3_MP3_NATIVE"); on = !(e && e[0] == '0'); }
    return on;
}

/* Return to the caller as `bi $r0` does. */
static int return_to_caller(spu_context* ctx, uint32_t r3)
{
    for (int k = 0; k < 4; k++) ctx->gpr[3]._u32[k] = r3;
    ctx->pc = ctx->gpr[0]._u32[0];
    if (ctx->host_depth == 0) g_spu_trampoline_fn = spu_indirect_branch;
    return 1;
}

static int decode_native(spu_context* ctx, const args_t* a)
{
    uint8_t* ls = ctx->ls;
    dod3_mp3_info info;
    const int nch = dod3_mp3_frame_bytes(ls + a->in, &info) ? info.channels : 1;
    static _Thread_local dod3_mp3 d;
    static _Thread_local result_t r;
    state_load(ls, a, nch, &d);
    decode_frame(ls, a, &d, &r);
    if (r.samples) state_save(ls, a, r.nch, &d);
    write_result(ls, a, &r);
    return return_to_caller(ctx, r.ret);
}

/* ---- the lifted decoder ----------------------------------------------------- */

/* Run the lifted decodeFrame from its entry until it returns to the caller. */
static void run_lifted(spu_context* ctx)
{
    const uint32_t ret = ctx->gpr[0]._u32[0] & SPU_LS_MASK;
    t_inner = 1;
    dod3_spu_run_to(ctx, (void (*)(spu_context*))g_dod3_mp3_sony_entry, ret);
    t_inner = 0;
}

/* ---- check mode: both decoders, Sony's result stands -------------------------- */

static int check_on(void)
{
    static int on = -1;
    if (on < 0) on = getenv("DOD3_MP3_CHECK") ? 1 : 0;
    return on;
}

/* A stream is followed through Sony's state: the hash of its left-channel
 * regions and word after one call is the key the next call of the same
 * stream arrives with. */
typedef struct { uint64_t key; uint64_t used; dod3_mp3 d; } shadow_t;
static shadow_t s_shadow[256];
static uint64_t s_clock;
static volatile int s_lock;

static uint64_t fnv(uint64_t h, const uint8_t* p, uint32_t n)
{
    for (uint32_t i = 0; i < n; i++) h = (h ^ p[i]) * 0x100000001B3ull;
    return h;
}
static uint64_t sony_key(const uint8_t* ls, const args_t* a)
{
    uint64_t h = 0xCBF29CE484222325ull;
    h = fnv(h, ls + a->l_syn, 0x880);
    h = fnv(h, ls + a->l_ov, 0x900);
    h = fnv(h, ls + a->reserv, 0x200);
    return fnv(h, ls + a->word, 4);
}

static struct {
    uint64_t frames, contract_bad, off2, off64, new_streams;
    double err, sig;
    int maxdiff;
} s_st;

static void check_report(void)
{
    fprintf(stderr, "[mp3-check] %llu frames: %llu contract mismatches; PCM vs Sony: max %d LSB, %.1f dB, "
            "%llu frames > 2 LSB, %llu > 64 LSB; %llu stream starts\n",
            (unsigned long long)s_st.frames, (unsigned long long)s_st.contract_bad, s_st.maxdiff,
            s_st.err > 0 ? 10.0 * log10(s_st.sig / s_st.err) : 999.0,
            (unsigned long long)s_st.off2, (unsigned long long)s_st.off64, (unsigned long long)s_st.new_streams);
}

static int decode_check(spu_context* ctx, const args_t* a)
{
    uint8_t* ls = ctx->ls;
    while (__atomic_exchange_n(&s_lock, 1, __ATOMIC_ACQUIRE)) { }
    const uint64_t k0 = sony_key(ls, a);
    shadow_t* sh = NULL;
    for (int i = 0; i < 256; i++)
        if (s_shadow[i].used && s_shadow[i].key == k0) { sh = &s_shadow[i]; break; }
    if (!sh) {
        sh = &s_shadow[0];
        for (int i = 1; i < 256; i++) if (s_shadow[i].used < sh->used) sh = &s_shadow[i];
        memset(&sh->d, 0, sizeof sh->d);
        s_st.new_streams++;
    }
    /* Ours first, from the frame as it is now (Sony's run rewrites the
     * regions but not the frame). */
    static result_t r;
    decode_frame(ls, a, &sh->d, &r);
    run_lifted(ctx);
    const uint32_t ret = ctx->gpr[3]._u32[0], cons = ls_rd32(ls, a->cons), prod = ls_rd32(ls, a->prod);
    const uint32_t want_prod = r.ret == 0xFFFFFFFFu ? 0 : r.prod;
    s_st.frames++;
    if (ret != r.ret || cons != r.cons || prod != want_prod) {
        if (++s_st.contract_bad <= 16)
            fprintf(stderr, "[mp3-check] call %llu: Sony ret 0x%X consumed %u produced 0x%X; ours ret 0x%X consumed %u produced 0x%X\n",
                    (unsigned long long)s_st.frames, ret, cons, prod, r.ret, r.cons, want_prod);
    } else if (r.samples && prod != 0xFFFFFFFFu) {
        int md = 0;
        for (int c = 0; c < r.nch; c++) {
            const uint8_t* o = ls + a->work + OUT_OFF + (c ? OUT_R_OFF : 0);
            for (int i = 0; i < r.samples; i++) {
                const int sv = (int16_t)(o[2 * i] << 8 | o[2 * i + 1]), ov = r.pcm[c][i];
                const int dd = sv > ov ? sv - ov : ov - sv;
                if (dd > md) md = dd;
                s_st.err += (double)dd * dd;
                s_st.sig += (double)sv * sv;
            }
        }
        if (md > s_st.maxdiff) s_st.maxdiff = md;
        s_st.off2 += md > 2;
        s_st.off64 += md > 64;
    }
    sh->key = sony_key(ls, a);
    sh->used = ++s_clock;
    if (s_st.frames % 2000 == 0) check_report();
    __atomic_store_n(&s_lock, 0, __ATOMIC_RELEASE);
    return 1;   /* Sony's result stands; the pc is the caller's */
}

/* ---- recording ---------------------------------------------------------------- */

#define DUMP_LO 0x2A880u
#define DUMP_HI 0x37100u
static FILE* s_dump;
static int s_dump_left = -1;

static void dump_open(void)
{
    const char* e = getenv("DOD3_MP3_DUMP");
    s_dump_left = 0;
    if (!e || !*e) return;
    char path[1024];
    snprintf(path, sizeof path, "%s", e);
    char* comma = strchr(path, ',');
    int n = 400;
    if (comma) { *comma = 0; n = atoi(comma + 1); }
    s_dump = fopen(path, "wb");
    if (!s_dump) { fprintf(stderr, "[mp3] DOD3_MP3_DUMP: cannot write %s\n", path); return; }
    s_dump_left = n;
    fprintf(stderr, "[mp3] recording %d decodeFrame calls to %s\n", n, path);
}

static int dump_call(spu_context* ctx, const args_t* a)
{
    static uint8_t before[DUMP_HI - DUMP_LO];
    uint32_t regs[16];
    for (int r = 0; r < 16; r++) regs[r] = ctx->gpr[r]._u32[0];
    memcpy(before, ctx->ls + DUMP_LO, sizeof before);
    run_lifted(ctx);
    const uint32_t hdr[8] = { 0x4433504Du /* "MP3D" */, DUMP_LO, DUMP_HI, ctx->gpr[3]._u32[0],
                              ls_rd32(ctx->ls, a->cons), ls_rd32(ctx->ls, a->prod), 0, 0 };
    fwrite(hdr, 4, 8, s_dump);
    fwrite(regs, 4, 16, s_dump);
    fwrite(before, 1, sizeof before, s_dump);
    fwrite(ctx->ls + DUMP_LO, 1, sizeof before, s_dump);
    if (--s_dump_left == 0) { fclose(s_dump); s_dump = NULL; fprintf(stderr, "[mp3] recording done\n"); }
    return 1;
}

/* DOD3_MP3_TRACE=<file>: every call, compactly -- Sony's state key before
 * and after (to follow streams), consumed / produced / return value, the
 * frame's bytes and Sony's PCM -- for checking whole streams offline. */
static FILE* s_trace;
static int trace_on(void)
{
    static int on = -1;
    if (on < 0) {
        const char* e = getenv("DOD3_MP3_TRACE");
        on = 0;
        if (e && *e && (s_trace = fopen(e, "wb")) != NULL) on = 1;
    }
    return on;
}
static int trace_call(spu_context* ctx, const args_t* a)
{
    uint8_t* ls = ctx->ls;
    while (__atomic_exchange_n(&s_lock, 1, __ATOMIC_ACQUIRE)) { }
    const uint64_t k0 = sony_key(ls, a);
    static uint8_t frame[2048];
    memcpy(frame, ls + a->in, ls_ok(a->in, sizeof frame) ? sizeof frame : 16);
    run_lifted(ctx);
    const uint64_t k1 = sony_key(ls, a);
    const uint32_t ret = ctx->gpr[3]._u32[0], cons = ls_rd32(ls, a->cons), prod = ls_rd32(ls, a->prod);
    const uint32_t nf = cons <= sizeof frame ? cons : 0, np = (ret != 0xFFFFFFFFu && ret && prod <= 0x1000) ? prod : 0;
    const uint32_t hdr[8] = { 0x5452334Du /* "M3RT" */, ret, cons, prod, nf, np, 0, 0 };
    fwrite(hdr, 4, 8, s_trace);
    fwrite(&k0, 8, 1, s_trace);
    fwrite(&k1, 8, 1, s_trace);
    fwrite(frame, 1, nf, s_trace);
    if (np) {
        fwrite(ls + (ret & SPU_LS_MASK), 1, np, s_trace);
        fwrite(ls + ((ret + OUT_R_OFF) & SPU_LS_MASK), 1, np, s_trace);
    }
    __atomic_store_n(&s_lock, 0, __ATOMIC_RELEASE);
    return 1;
}

/* DOD3_MP3_TIME=1: the decoder's time per call, reported every 5000 calls. */
static int time_on(void)
{
    static int on = -1;
    if (on < 0) on = getenv("DOD3_MP3_TIME") ? 1 : 0;
    return on;
}
static int decode_timed(spu_context* ctx, const args_t* a, int ours)
{
    static uint64_t s_ns, s_calls;
    struct timespec t0, t1;
    timespec_get(&t0, TIME_UTC);
    if (ours) decode_native(ctx, a);
    else run_lifted(ctx);
    timespec_get(&t1, TIME_UTC);
    s_ns += (uint64_t)((t1.tv_sec - t0.tv_sec) * 1000000000LL + (t1.tv_nsec - t0.tv_nsec));
    if (++s_calls % 5000 == 0)
        fprintf(stderr, "[mp3-time] %s decoder: %llu calls, %.1f us per call\n", ours ? "our" : "Sony's",
                (unsigned long long)s_calls, s_ns / 1000.0 / (double)s_calls);
    return 1;
}

static void say_once(const char* which)
{
    static int said;
    if (!__atomic_exchange_n(&said, 1, __ATOMIC_RELAXED))
        fprintf(stderr, "[mp3] MultiStream's MP3 decoder: %s\n", which);
}

/* ---- the hook ----------------------------------------------------------------- */

/* Sony's decodeFrame (0x21D20 of spu_ovl_mp3_1A900): ours by default, Sony's
 * with DOD3_MP3_NATIVE=0, both under the check and recording modes. */
int dod3_mp3_decode_hook(spu_context* ctx)
{
    if (t_inner) return 0;
    args_t a;
    get_args(ctx, &a);
    if (!args_ok(&a)) return 0;
    if (s_dump_left < 0) dump_open();
    if (s_dump_left > 0) return dump_call(ctx, &a);
    if (trace_on()) return trace_call(ctx, &a);
    if (check_on()) return decode_check(ctx, &a);
    say_once(native_on() ? "ours, in place of the firmware's" : "the firmware's (lifted)");
    if (time_on()) return decode_timed(ctx, &a, native_on());
    if (native_on()) return decode_native(ctx, &a);
    return 0;
}

/* The entry of our stand-in flashMP3.pic (spu_ovl_mp3native_1A900, 0x1A910):
 * nothing behind it, so always ours. */
int dod3_mp3_standin_hook(spu_context* ctx)
{
    args_t a;
    get_args(ctx, &a);
    if (!args_ok(&a)) return return_to_caller(ctx, 0xFFFFFFFFu);
    say_once("ours (stand-in flashMP3.pic)");
    if (time_on()) return decode_timed(ctx, &a, 1);
    return decode_native(ctx, &a);
}
