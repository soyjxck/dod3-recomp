/*
 * dod3_mp3dec.c -- an MPEG-1/2/2.5 audio Layer III decoder (see dod3_mp3dec.h).
 *
 * Straight from the standard (ISO/IEC 11172-3 2.4.3.4, 13818-3): header and
 * side information, the bit reservoir, scale factors, Huffman decoding,
 * requantization, mid/side and intensity stereo, short-block reordering,
 * alias reduction, the IMDCT with overlap-add, frequency inversion and the
 * polyphase synthesis filterbank. Floating point throughout; output is
 * rounded to 16 bits. The tables come from tools/gen_mp3_tables.py.
 *
 * The synthesis keeps 32 values per step instead of the standard's 64-value
 * V vectors: V[i] = X[16+i] with X[m] = sum_k S[k] cos(m(2k+1)pi/64), and X
 * is antisymmetric about 32 and 64, so X[0..31] determine all of it.
 */
#include "dod3_mp3dec.h"
#include "dod3_mp3dec_tables.h"
#include <math.h>
#include <string.h>

/* ---- bits ------------------------------------------------------------------ */

typedef struct {
    const uint8_t* buf;   /* readable up to 4 bytes past the last bit used */
    int pos;              /* bit position */
} bits_t;

static inline uint32_t bits_get(bits_t* b, int n)   /* n = 0..24 */
{
    if (!n) return 0;
    const uint8_t* p = b->buf + (b->pos >> 3);
    const uint32_t w = (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
    const uint32_t v = (w << (b->pos & 7)) >> (32 - n);
    b->pos += n;
    return v;
}

static inline uint32_t bits_get1(bits_t* b)
{
    const uint32_t v = (b->buf[b->pos >> 3] >> (7 - (b->pos & 7))) & 1u;
    b->pos++;
    return v;
}

/* One Huffman codeword of table t (k_huff_start index): returns x << 4 | y. */
static inline int huff_word(bits_t* b, int t)
{
    const uint16_t* tree = k_huff_tree + k_huff_start[t];
    unsigned node = 0;
    for (;;) {
        const unsigned c = tree[2 * node + bits_get1(b)];
        if (c & 0x8000u) return (int)(c & 0xFFu);
        node = c;
    }
}

/* ---- header and side information ------------------------------------------ */

typedef struct {
    int mpeg1, crc, mode, mode_ext, nch, sfreq, hz, bytes, granules, side_bytes;
    int ms, is;   /* joint stereo tools in use */
} hdr_t;

static int parse_header(const uint8_t* h, hdr_t* o)
{
    static const uint16_t kbps[2][15] = {
        { 0, 32, 40, 48, 56, 64, 80, 96, 112, 128, 160, 192, 224, 256, 320 },
        { 0, 8, 16, 24, 32, 40, 48, 56, 64, 80, 96, 112, 128, 144, 160 },
    };
    static const uint16_t base_hz[3] = { 44100, 48000, 32000 };
    if (h[0] != 0xFF || (h[1] & 0xE0) != 0xE0) return 0;
    const int ver = (h[1] >> 3) & 3, layer = (h[1] >> 1) & 3;   /* ver 3: MPEG-1, 2: MPEG-2, 0: 2.5 */
    const int bri = h[2] >> 4, sri = (h[2] >> 2) & 3;
    if (ver == 1 || layer != 1 || bri == 0 || bri == 15 || sri == 3) return 0;
    o->mpeg1 = ver == 3;
    o->hz = base_hz[sri] >> (ver == 3 ? 0 : ver == 2 ? 1 : 2);
    const int r = sri + 3 * (((h[1] >> 3) & 1) + ((h[1] >> 4) & 1));
    o->sfreq = r - (r != 0);          /* the band table row (see the generator) */
    o->crc = !(h[1] & 1);
    o->mode = h[3] >> 6;
    o->mode_ext = (h[3] >> 4) & 3;
    o->nch = o->mode == 3 ? 1 : 2;
    o->ms = o->mode == 1 && (o->mode_ext & 2);
    o->is = o->mode == 1 && (o->mode_ext & 1);
    o->granules = o->mpeg1 ? 2 : 1;
    o->bytes = (o->mpeg1 ? 144 : 72) * 1000 * kbps[!o->mpeg1][bri] / o->hz + ((h[2] >> 1) & 1);
    o->side_bytes = o->mpeg1 ? (o->nch == 1 ? 17 : 32) : (o->nch == 1 ? 9 : 17);
    return 1;
}

int dod3_mp3_frame_bytes(const uint8_t* hdr, dod3_mp3_info* info)
{
    hdr_t h;
    if (!parse_header(hdr, &h)) return 0;
    if (info) {
        info->frame_bytes = h.bytes;
        info->channels = h.nch;
        info->samples = 576 * h.granules;
        info->hz = h.hz;
    }
    return h.bytes;
}

typedef struct {
    int part23, big_values, global_gain, sf_compress;
    int win_switch, block_type, mixed;
    int table[3], subblock_gain[3], region0, region1;
    int preflag, sf_scale, count1_table;
} granule_t;

static int read_side_info(bits_t* b, const hdr_t* h, int* main_data_begin, int scfsi[2][4], granule_t gr[2][2])
{
    if (h->mpeg1) {
        *main_data_begin = (int)bits_get(b, 9);
        bits_get(b, h->nch == 1 ? 5 : 3);
        for (int ch = 0; ch < h->nch; ch++)
            for (int k = 0; k < 4; k++) scfsi[ch][k] = (int)bits_get1(b);
    } else {
        *main_data_begin = (int)bits_get(b, 8);
        bits_get(b, h->nch == 1 ? 1 : 2);
        memset(scfsi, 0, sizeof(int) * 8);
    }
    for (int g = 0; g < h->granules; g++)
        for (int ch = 0; ch < h->nch; ch++) {
            granule_t* r = &gr[g][ch];
            r->part23 = (int)bits_get(b, 12);
            r->big_values = (int)bits_get(b, 9);
            if (r->big_values > 288) return 0;
            r->global_gain = (int)bits_get(b, 8);
            r->sf_compress = (int)bits_get(b, h->mpeg1 ? 4 : 9);
            r->win_switch = (int)bits_get1(b);
            if (r->win_switch) {
                r->block_type = (int)bits_get(b, 2);
                r->mixed = (int)bits_get1(b);
                r->table[0] = (int)bits_get(b, 5);
                r->table[1] = (int)bits_get(b, 5);
                r->table[2] = 0;
                for (int w = 0; w < 3; w++) r->subblock_gain[w] = (int)bits_get(b, 3);
                if (r->block_type == 0) return 0;
                /* Implicit: region1 starts after 36 lines (short) or band 8,
                 * and there is no region2. */
                r->region0 = (r->block_type == 2 && !r->mixed) ? 8 : 7;
                r->region1 = 20 - r->region0;
            } else {
                r->block_type = r->mixed = 0;
                for (int k = 0; k < 3; k++) r->table[k] = (int)bits_get(b, 5);
                r->subblock_gain[0] = r->subblock_gain[1] = r->subblock_gain[2] = 0;
                r->region0 = (int)bits_get(b, 4);
                r->region1 = (int)bits_get(b, 3);
            }
            if (h->mpeg1)
                r->preflag = (int)bits_get1(b);
            else
                r->preflag = r->sf_compress >= 500 && !(h->is && ch == 1);
            r->sf_scale = (int)bits_get1(b);
            r->count1_table = (int)bits_get1(b);
        }
    return 1;
}

/* ---- scale factors ---------------------------------------------------------- */

typedef struct {
    int l[22];         /* long bands; 21 has none */
    int s[13][3];      /* short bands x windows; 12 has none */
    int lmax[22];      /* the intensity-stereo "illegal" position per band */
    int smax[13];
} scf_t;

/* Long bands a mixed block keeps: those within the first 36 lines. */
static int mixed_long_bands(const hdr_t* h)
{
    int n = 0, at = 0;
    while (at < 36) at += k_sfb_long[h->sfreq][n++];
    return n;
}

static void read_scalefactors_mpeg1(bits_t* b, const granule_t* r, int g, const int scfsi[4], scf_t* s)
{
    static const uint8_t slen[2][16] = {
        { 0, 0, 0, 0, 3, 1, 1, 1, 2, 2, 2, 3, 3, 3, 4, 4 },
        { 0, 1, 2, 3, 0, 1, 2, 3, 1, 2, 3, 1, 2, 3, 2, 3 },
    };
    const int s1 = slen[0][r->sf_compress], s2 = slen[1][r->sf_compress];
    if (r->block_type == 2) {
        int sfb = 0;
        if (r->mixed) {
            for (; sfb < 8; sfb++) s->l[sfb] = (int)bits_get(b, s1);
            sfb = 3;
        }
        for (; sfb < 6; sfb++)
            for (int w = 0; w < 3; w++) s->s[sfb][w] = (int)bits_get(b, s1);
        for (; sfb < 12; sfb++)
            for (int w = 0; w < 3; w++) s->s[sfb][w] = (int)bits_get(b, s2);
        s->s[12][0] = s->s[12][1] = s->s[12][2] = 0;
    } else {
        static const uint8_t part[5] = { 0, 6, 11, 16, 21 };
        for (int p = 0; p < 4; p++) {
            if (g == 1 && scfsi[p]) continue;   /* granule 0's stand */
            for (int sfb = part[p]; sfb < part[p + 1]; sfb++) s->l[sfb] = (int)bits_get(b, p < 2 ? s1 : s2);
        }
        s->l[21] = 0;
    }
    for (int sfb = 0; sfb < 22; sfb++) s->lmax[sfb] = 7;
    for (int sfb = 0; sfb < 13; sfb++) s->smax[sfb] = 7;
}

static void read_scalefactors_lsf(bits_t* b, const hdr_t* h, const granule_t* r, int ch, scf_t* s)
{
    static const uint8_t nr[6][3][4] = {
        { { 6, 5, 5, 5 }, { 9, 9, 9, 9 }, { 6, 9, 9, 9 } },
        { { 6, 5, 7, 3 }, { 9, 9, 12, 6 }, { 6, 9, 12, 6 } },
        { { 11, 10, 0, 0 }, { 18, 18, 0, 0 }, { 15, 18, 0, 0 } },
        { { 7, 7, 7, 0 }, { 12, 12, 12, 0 }, { 6, 15, 12, 0 } },
        { { 6, 6, 6, 3 }, { 12, 9, 9, 6 }, { 6, 12, 9, 6 } },
        { { 8, 8, 5, 0 }, { 15, 12, 9, 0 }, { 6, 18, 9, 0 } },
    };
    int sl[4] = { 0, 0, 0, 0 }, idx, sfc = r->sf_compress;
    if (!(h->is && ch == 1)) {
        if (sfc < 400) {
            sl[0] = (sfc >> 4) / 5;
            sl[1] = (sfc >> 4) % 5;
            sl[2] = (sfc & 15) >> 2;
            sl[3] = sfc & 3;
            idx = 0;
        } else if (sfc < 500) {
            sfc -= 400;
            sl[0] = (sfc >> 2) / 5;
            sl[1] = (sfc >> 2) % 5;
            sl[2] = sfc & 3;
            idx = 1;
        } else {
            sfc -= 500;
            sl[0] = sfc / 3;
            sl[1] = sfc % 3;
            idx = 2;
        }
    } else {
        sfc >>= 1;
        if (sfc < 180) {
            sl[0] = sfc / 36;
            sl[1] = (sfc % 36) / 6;
            sl[2] = (sfc % 36) % 6;
            idx = 3;
        } else if (sfc < 244) {
            sfc -= 180;
            sl[0] = (sfc % 64) >> 4;
            sl[1] = (sfc % 16) >> 2;
            sl[2] = sfc % 4;
            idx = 4;
        } else {
            sfc -= 244;
            sl[0] = sfc / 3;
            sl[1] = sfc % 3;
            idx = 5;
        }
    }
    const int kind = r->block_type == 2 ? (r->mixed ? 2 : 1) : 0;
    int vals[40], maxs[40], n = 0;
    for (int p = 0; p < 4; p++)
        for (int i = 0; i < nr[idx][kind][p]; i++) {
            vals[n] = (int)bits_get(b, sl[p]);
            maxs[n++] = (1 << sl[p]) - 1;
        }
    int k = 0;
    if (kind == 0) {
        for (int sfb = 0; sfb < 21; sfb++, k++) {
            s->l[sfb] = vals[k];
            s->lmax[sfb] = maxs[k];
        }
        s->l[21] = 0;
        s->lmax[21] = s->lmax[20];
    } else {
        int sfb = 0;
        if (kind == 2) {
            const int nl = mixed_long_bands(h);
            for (; sfb < nl && k < n; sfb++, k++) {
                s->l[sfb] = vals[k];
                s->lmax[sfb] = maxs[k];
            }
            sfb = 3;
        }
        for (; sfb < 12; sfb++) {
            s->smax[sfb] = k < n ? maxs[k] : 0;
            for (int w = 0; w < 3; w++, k++) s->s[sfb][w] = k < n ? vals[k] : 0;
        }
        s->s[12][0] = s->s[12][1] = s->s[12][2] = 0;
        s->smax[12] = s->smax[11];
    }
}

/* ---- Huffman ------------------------------------------------------------------ */

/* Decode one channel's spectrum (ends at bit `end`). Returns the count of
 * lines decoded; the rest are zero. */
static int read_huffman(bits_t* b, int end, const hdr_t* h, const granule_t* r, int is[576])
{
    const uint8_t* wl = k_sfb_long[h->sfreq];
    const uint8_t* ws = k_sfb_short[h->sfreq];
    int r1, r2;
    if (r->win_switch && r->block_type == 2) {
        r1 = 3 * (ws[0] + ws[1] + ws[2]);
        r2 = 576;
    } else {
        int at = 0, n1 = r->region0 + 1, n2 = r->region0 + r->region1 + 2;
        r1 = r2 = 576;
        for (int sfb = 0; sfb < 22; sfb++) {
            if (sfb == n1) r1 = at;
            if (sfb == n2) r2 = at;
            at += wl[sfb];
        }
    }
    const int bv = r->big_values * 2;
    if (r1 > bv) r1 = bv;
    if (r2 > bv) r2 = bv;
    int i = 0;
    for (; i < bv; i += 2) {
        const int t = r->table[i < r1 ? 0 : i < r2 ? 1 : 2];
        if (t == 0 || k_huff_start[t] == 0xFFFFu) {
            is[i] = is[i + 1] = 0;
            continue;
        }
        const int xy = huff_word(b, t), lin = k_huff_linbits[t];
        int x = xy >> 4, y = xy & 15;
        if (lin && x == 15) x += (int)bits_get(b, lin);
        if (x && bits_get1(b)) x = -x;
        if (lin && y == 15) y += (int)bits_get(b, lin);
        if (y && bits_get1(b)) y = -y;
        is[i] = x;
        is[i + 1] = y;
    }
    /* count1: quadruples until part2_3_length runs out; one that runs past
     * it is not part of the spectrum. */
    const int t1 = 32 + r->count1_table;
    while (i + 4 <= 576 && b->pos < end) {
        const int v = huff_word(b, t1) >> 4;      /* v w x y, v first */
        int q[4] = { (v >> 3) & 1, (v >> 2) & 1, (v >> 1) & 1, v & 1 };
        for (int k = 0; k < 4; k++)
            if (q[k] && bits_get1(b)) q[k] = -q[k];
        if (b->pos > end) break;
        is[i] = q[0];
        is[i + 1] = q[1];
        is[i + 2] = q[2];
        is[i + 3] = q[3];
        i += 4;
    }
    for (int k = i; k < 576; k++) is[k] = 0;
    b->pos = end;
    return i;
}

/* ---- requantization ---------------------------------------------------------- */

static inline float gain_q(int q) /* 2^(q/4) */ { return ldexpf(k_pow2_quarter[q & 3], q >> 2); }

static inline float pow43(int v)
{
    const int a = v < 0 ? -v : v;
    const float m = a < 256 ? k_pow43[a] : (float)pow((double)a, 4.0 / 3.0);
    return v < 0 ? -m : m;
}

/* Lines in decode order: the long part in frequency order, then each short
 * band's three windows one after another. */
static void requantize(const hdr_t* h, const granule_t* r, const scf_t* s, const int* is, int n, float* xr)
{
    const uint8_t* wl = k_sfb_long[h->sfreq];
    const uint8_t* ws = k_sfb_short[h->sfreq];
    static const uint8_t pretab[22] = { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 3, 3, 3, 2, 0 };
    const int shift = r->sf_scale ? 4 : 2, g0 = r->global_gain - 210;
    int i = 0, sfb = 0, long_end = 576;
    if (r->block_type == 2) long_end = r->mixed ? 36 : 0;
    for (; i < long_end && i < n; sfb++) {
        const float g = gain_q(g0 - shift * (s->l[sfb] + (r->preflag ? pretab[sfb] : 0)));
        const int e = i + wl[sfb];
        for (; i < e && i < n; i++) xr[i] = is[i] ? pow43(is[i]) * g : 0.0f;
    }
    if (r->block_type == 2) {
        for (sfb = r->mixed ? 3 : 0; sfb < 13 && i < n; sfb++)
            for (int w = 0; w < 3; w++) {
                const float g = gain_q(g0 - 8 * r->subblock_gain[w] - shift * s->s[sfb][w]);
                const int e = i + ws[sfb];
                for (; i < e && i < n; i++) xr[i] = is[i] ? pow43(is[i]) * g : 0.0f;
            }
    }
    for (; i < 576; i++) xr[i] = 0.0f;
}

/* ---- stereo ------------------------------------------------------------------ */

/* Mid/side and intensity stereo, in decode order. nz[] are the channels'
 * decoded line counts. */
static void stereo(const hdr_t* h, const granule_t* gr, const scf_t* sr, const int* is_r, const int nz[2], float* xl,
                   float* xr)
{
    /* is_pos per line (-1: not intensity-coded). */
    int8_t pos[576];
    memset(pos, -1, sizeof pos);
    const granule_t* r = &gr[1];
    const uint8_t* wl = k_sfb_long[h->sfreq];
    const uint8_t* ws = k_sfb_short[h->sfreq];
    if (h->is) {
        int last = nz[1] - 1;
        while (last >= 0 && is_r[last] == 0) last--;
        const int short_start = r->block_type == 2 ? (r->mixed ? 36 : 0) : 576;
        /* Long part: bands above the right channel's last nonzero line, if
         * that line is in the long part (in a mixed block, otherwise only the
         * short part is intensity-coded, window by window). */
        int i = 0, sfb = 0;
        const int long_bands = r->block_type == 2 ? (r->mixed ? mixed_long_bands(h) : 0) : 22;
        const int long_live = last < short_start;   /* the right channel ends inside the long part */
        for (; sfb < long_bands; sfb++) {
            const int e = i + wl[sfb];
            if (i > last && long_live) {
                const int b = sfb == 21 ? 20 : sfb;
                const int p = sr->l[b];
                if (p != sr->lmax[b])
                    for (int k = i; k < e && k < 576; k++) pos[k] = (int8_t)p;
            }
            i = e;
        }
        if (r->block_type == 2) {
            /* Short part: per window, bands above that window's last nonzero. */
            int lastb[3] = { -1, -1, -1 };
            int at = short_start;
            for (sfb = r->mixed ? 3 : 0; sfb < 13; sfb++)
                for (int w = 0; w < 3; w++) {
                    for (int k = at; k < at + ws[sfb] && k < 576; k++)
                        if (is_r[k]) lastb[w] = sfb;
                    at += ws[sfb];
                }
            at = short_start;
            for (sfb = r->mixed ? 3 : 0; sfb < 13; sfb++)
                for (int w = 0; w < 3; w++) {
                    const int b = sfb == 12 ? 11 : sfb;
                    const int p = sr->s[b][w];
                    if (sfb > lastb[w] && p != sr->smax[b])
                        for (int k = at; k < at + ws[sfb] && k < 576; k++) pos[k] = (int8_t)p;
                    at += ws[sfb];
                }
        }
    }
    const int n = nz[0] > nz[1] ? nz[0] : nz[1];
    const float rs2 = 0.70710678118654752f;
    float io = 0.0f;
    if (h->is && !h->mpeg1) io = (r->sf_compress & 1) ? 0.70710678118654752f : 0.84089641525371454f;
    for (int i = 0; i < 576; i++) {
        const int p = pos[i];
        if (p >= 0) {
            const float l = xl[i];
            if (h->mpeg1) {
                xl[i] = l * k_is_left[p];
                xr[i] = l * k_is_right[p];
            } else if (p == 0) {
                xr[i] = l;
            } else if (p & 1) {
                xl[i] = l * powf(io, (float)((p + 1) >> 1));
                xr[i] = l;
            } else {
                xr[i] = l * powf(io, (float)(p >> 1));
            }
        } else if (h->ms && i < n) {
            const float m = xl[i], sd = xr[i];
            xl[i] = (m + sd) * rs2;
            xr[i] = (m - sd) * rs2;
        }
    }
}

/* ---- hybrid filterbank -------------------------------------------------------- */

/* Decode-order lines -> 32 subbands x 18 samples, with the channel's overlap. */
static void hybrid(const hdr_t* h, const granule_t* r, const float* xr, int n, dod3_mp3_channel* c, float out[32][18])
{
    const uint8_t* ws = k_sfb_short[h->sfreq];
    float lin[576];
    const int long_lines = r->block_type == 2 ? (r->mixed ? 36 : 0) : 576;
    memcpy(lin, xr, sizeof(float) * (size_t)long_lines);
    /* Short part: reorder into [subband][window][6]. */
    if (long_lines < 576) {
        int at = long_lines;
        for (int sfb = r->mixed ? 3 : 0; sfb < 13; sfb++) {
            int f = 0;
            for (int b = 0; b < sfb; b++) f += ws[b];
            for (int w = 0; w < 3; w++)
                for (int j = 0; j < ws[sfb]; j++, at++) {
                    const int fr = f + j;                 /* frequency within the window */
                    lin[(fr / 6) * 18 + w * 6 + fr % 6] = at < 576 ? xr[at] : 0.0f;
                }
        }
    }
    /* Subbands that hold anything. */
    int live = (n + 17) / 18;
    if (long_lines < 576) live = 32;
    if (live > 32) live = 32;
    /* Alias reduction across long-block subband boundaries. */
    const int aa_end = r->block_type == 2 ? (r->mixed ? 2 : 0) : (live < 32 ? live + 1 : 32);
    for (int sb = 1; sb < aa_end; sb++) {
        float* lo = lin + 18 * sb - 1;
        float* hi = lin + 18 * sb;
        for (int k = 0; k < 8; k++) {
            const float bu = lo[-k], bd = hi[k];
            lo[-k] = bu * k_aa_cs[k] - bd * k_aa_ca[k];
            hi[k] = bd * k_aa_cs[k] + bu * k_aa_ca[k];
        }
    }
    if (aa_end > live) live = aa_end;
    for (int sb = 0; sb < 32; sb++) {
        float* ov = c->overlap[sb];
        float* o = out[sb];
        if (sb >= live) {
            for (int i = 0; i < 18; i++) {
                o[i] = ov[i];
                ov[i] = 0.0f;
            }
            continue;
        }
        const float* x = lin + 18 * sb;
        const int is_short = r->block_type == 2 && !(r->mixed && sb < 2);
        float z[36];
        if (!is_short) {
            const int bt = (r->block_type == 2) ? 0 : r->block_type;
            const float* win = k_imdct_window + 36 * bt;
            for (int i = 0; i < 36; i++) {
                const float* cs = k_imdct36 + 18 * i;
                float a = 0.0f;
                for (int k = 0; k < 18; k++) a += x[k] * cs[k];
                z[i] = a * win[i];
            }
        } else {
            const float* win = k_imdct_window + 36 * 2;
            memset(z, 0, sizeof z);
            for (int w = 0; w < 3; w++) {
                const float* xw = x + 6 * w;
                for (int i = 0; i < 12; i++) {
                    const float* cs = k_imdct12 + 6 * i;
                    float a = 0.0f;
                    for (int k = 0; k < 6; k++) a += xw[k] * cs[k];
                    z[6 + 6 * w + i] += a * win[i];
                }
            }
        }
        for (int i = 0; i < 18; i++) {
            o[i] = z[i] + ov[i];
            ov[i] = z[18 + i];
        }
    }
    /* Frequency inversion. */
    for (int sb = 1; sb < 32; sb += 2)
        for (int i = 1; i < 18; i += 2) out[sb][i] = -out[sb][i];
}

/* 18 steps of the synthesis filterbank: 32 samples each, to `pcm` (every
 * `stride`-th int16). */
static void synthesize(dod3_mp3_channel* c, int pos0, float sub[32][18], int16_t* pcm)
{
    for (int t = 0; t < 18; t++) {
        const int p = (pos0 + t) & 15;
        float* X = c->synth[p];
        for (int m = 0; m < 32; m++) {
            const float* cs = k_matrix + 32 * m;
            float a = 0.0f;
            for (int k = 0; k < 32; k++) a += sub[k][t] * cs[k];
            X[m] = a;
        }
        for (int j = 0; j < 32; j++) {
            float a = 0.0f;
            for (int i = 0; i < 8; i++) {
                const float* ve = c->synth[(p - 2 * i) & 15];
                const float* vo = c->synth[(p - 2 * i - 1) & 15];
                /* first half of an even vector, second half of an odd one */
                const float v1 = j < 16 ? ve[16 + j] : j == 16 ? 0.0f : -ve[48 - j];
                const float v2 = j < 16 ? -vo[16 - j] : -vo[j - 16];
                a += v1 * k_synth_window[64 * i + j] + v2 * k_synth_window[64 * i + 32 + j];
            }
            const long s = lrintf(a);
            pcm[32 * t + j] = (int16_t)(s > 32767 ? 32767 : s < -32768 ? -32768 : s);
        }
    }
}

/* ---- frames ------------------------------------------------------------------- */

int dod3_mp3_decode(dod3_mp3* d, const uint8_t* frame, int16_t* out[2], dod3_mp3_info* info)
{
    hdr_t h;
    if (!parse_header(frame, &h)) return -1;
    const int side_at = 4 + (h.crc ? 2 : 0), md_at = side_at + h.side_bytes;
    const int md_len = h.bytes - md_at;
    if (md_len < 0) return -1;
    uint8_t side[40] = { 0 };
    memcpy(side, frame + side_at, (size_t)h.side_bytes);
    bits_t sb = { side, 0 };
    int mdb, scfsi[2][4];
    granule_t gr[2][2];
    if (!read_side_info(&sb, &h, &mdb, scfsi, gr)) return -1;
    if (info) {
        info->frame_bytes = h.bytes;
        info->channels = h.nch;
        info->samples = 576 * h.granules;
        info->hz = h.hz;
    }

    /* Main data: the reservoir's last mdb bytes, then this frame's. The
     * buffer is zero past the end, so a corrupt length reads zeros. */
    enum { CAP = 8192 };   /* main data is at most 511 + 1441 bytes; the rest absorbs overruns */
    uint8_t md[CAP + 8];
    int have = d->reserv_len;
    if (have < 0 || have > 511) have = 0;
    const int ok = mdb <= have;
    int total = 0;
    if (ok) {
        memcpy(md, d->reserv + have - mdb, (size_t)mdb);
        memcpy(md + mdb, frame + md_at, (size_t)md_len);
        total = mdb + md_len;
    }
    memset(md + total, 0, (size_t)(CAP + 8 - total));
    /* The next frame's reservoir: the main data stream's last 511 bytes. */
    uint8_t keep[512];
    int keep_len;
    if (md_len >= 511) {
        memcpy(keep, frame + md_at + md_len - 511, 511);
        keep_len = 511;
    } else {
        const int old = have < 511 - md_len ? have : 511 - md_len;
        memcpy(keep, d->reserv + have - old, (size_t)old);
        memcpy(keep + old, frame + md_at, (size_t)md_len);
        keep_len = old + md_len;
    }

    const int samples = 576 * h.granules;
    if (!ok) {
        for (int ch = 0; ch < h.nch; ch++) memset(out[ch], 0, sizeof(int16_t) * (size_t)samples);
    } else {
        bits_t b = { md, 0 };
        const int limit = (total + 1024) * 8;
        scf_t scf[2];
        memset(scf, 0, sizeof scf);
        int is[2][576];
        float xr[2][576];
        for (int g = 0; g < h.granules; g++) {
            int nz[2] = { 0, 0 };
            for (int ch = 0; ch < h.nch; ch++) {
                const granule_t* r = &gr[g][ch];
                const int start = b.pos;
                int end = start + r->part23;
                if (end > limit) end = limit;
                if (h.mpeg1)
                    read_scalefactors_mpeg1(&b, r, g, scfsi[ch], &scf[ch]);
                else
                    read_scalefactors_lsf(&b, &h, r, ch, &scf[ch]);
                if (b.pos > end) b.pos = end;
                nz[ch] = read_huffman(&b, end, &h, r, is[ch]);
                requantize(&h, r, &scf[ch], is[ch], nz[ch], xr[ch]);
            }
            if (h.nch == 2 && (h.ms || h.is)) {
                stereo(&h, gr[g], &scf[1], is[1], nz, xr[0], xr[1]);
                if (h.is)
                    nz[0] = nz[1] = 576;
                else
                    nz[0] = nz[1] = nz[0] > nz[1] ? nz[0] : nz[1];
            }
            for (int ch = 0; ch < h.nch; ch++) {
                float sub[32][18];
                hybrid(&h, &gr[g][ch], xr[ch], nz[ch], &d->ch[ch], sub);
                synthesize(&d->ch[ch], d->synth_pos, sub, out[ch] + 576 * g);
            }
            d->synth_pos = (d->synth_pos + 18) & 15;
        }
    }
    memcpy(d->reserv, keep, (size_t)keep_len);
    d->reserv_len = keep_len;
    return samples;
}
