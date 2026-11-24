/*
 * The Intoner gauge's drain at frame rates above the title's own.
 *
 * While Zero is in Intoner mode her blood gauge (gauge 5, 1800 when full,
 * 1620 once the change has taken its tenth) runs down. The pawn's mode
 * update (func_00986C40, vtable slot 0xB48, the same function for all 70
 * pawn classes) works out the frame's share,
 *
 *     m_fSubTimeMagForModeChange (the pawn's parameters, +0xF0)
 *       x the frame's time in 1/60 s (+0x51C)
 *       x (1 - the pawn's rate 8, when that is above 0)
 *
 * and cuts it to a whole number before taking it off the gauge. At 30
 * frames a second Zero's share is 5: 150 a second. At 60 it is 2.5 and 2 is
 * taken, at 90 1.67 and 1, at 120 1.25 and 1; above 150 it is under 1 and
 * nothing is taken, the mode never ends. What is cut off is lost on every
 * frame, so the mode lasts longer the higher the frame rate.
 *
 * Here the part cut off is carried to the next frame, per pawn, as in the
 * frame-rate patch RPCS3 ships for the title, so the gauge loses in a second
 * what it loses at 30 frames a second, at any frame rate. The cut is made in
 * the middle of the function, between two calls it makes through the
 * vtable; those are wrapped (the function registry replaces them without a
 * re-lift):
 *
 *   - the mode update itself, to know the calls below are its own;
 *   - the pawn's rate (slot 0x88C, func_00959B28), whose answer for rate 8
 *     is the last thing the share needs;
 *   - the gauge's change (slot 0x8F0, func_0095BA40), which receives the
 *     share already cut. The share is worked out again from the same
 *     values; when its whole part is what the title passed, the carried
 *     part is added and the whole of the sum passed on instead.
 *
 * The carried part is dropped when the pawn is not in the mode (+0x102D is
 * not 1), as RPCS3's patch does. The change itself (the world slowed to a
 * tenth for some three seconds) takes nothing off the gauge either way: the
 * share is not asked for until it is over.
 *
 * DOD3_GAUGE_DRAIN=0 keeps the title's own behaviour. DOD3_GAUGE_LOG=1
 * prints, about once a second while a gauge runs down, the units taken and
 * the frames they took.
 *
 * Measured on the 1.01 EBOOT in "The Warpath", a full gauge from L3+R3 to
 * empty. As the title has it: 150 a second and 14.1 s at 30 fps, 120 and
 * 17.0 s at 60, 90 and 21.5 s at 90, 119 and 17.2 s at 120. With the part
 * carried: 150 a second at each, 14.1 to 14.3 s.
 */
#include "cpu/ppu.h"
#include "cpu/eboot.h"   /* the EBOOT version's addresses */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef DOD3_FN_MODE_UPDATE
namespace {

const uint32_t kFrameSteps = 0x51C;   /* the frame's time in 1/60 s */
const uint32_t kParams = 0x2CB8;      /* the pawn's parameters (Sqex03DataGamePawnEdParam) */
const uint32_t kDrainMag = 0xF0;      /* ... m_fSubTimeMagForModeChange */
const uint32_t kMode = 0x102D;        /* 1 while the gauge runs down */
const unsigned kBlood = 5;            /* the gauge */
const unsigned kRate = 8;             /* the rate the share is scaled by */

bool s_carry = true;   /* DOD3_GAUGE_DRAIN=0: the title's own */
bool s_log;            /* DOD3_GAUGE_LOG=1 */

/* the mode update in progress on this thread, and its rate 8 once asked */
struct Update {
    uint32_t pawn;
    bool has_rate;
    float rate;
};
thread_local Update t_update;

/* the part of a pawn's share not yet taken off its gauge (the game thread's) */
struct Rest {
    uint32_t pawn;
    double rest;
    /* the log's second */
    double taken;
    unsigned frames;
    float steps;
};
Rest s_rest[8];
unsigned s_next;

Rest* rest_of(uint32_t pawn, bool make)
{
    for (Rest& r : s_rest)
        if (r.pawn == pawn) return &r;
    if (!make) return nullptr;
    Rest& r = s_rest[s_next++ % 8];
    r = { pawn, 0.0, 0.0, 0, 0.0f };
    return &r;
}

float guest_float(uint32_t ea)
{
    const uint32_t w = vm_read32(ea);
    float f;
    memcpy(&f, &w, 4);
    return f;
}

/* Slot 0xB48: the pawn's mode update. */
void hook_mode_update(ppu_context* ctx)
{
    const uint32_t pawn = (uint32_t)ctx->gpr[3];
    const Update outer = t_update;
    t_update = { pawn, false, 0.0f };
    if (vm_read8(pawn + kMode) != 1)
        if (Rest* r = rest_of(pawn, false)) r->rest = 0.0;
    DOD3_FN_MODE_UPDATE(ctx);
    dod3_drain(ctx);
    t_update = outer;
}

/* Slot 0x88C: one of the pawn's rates (r4). */
void hook_rate(ppu_context* ctx)
{
    const bool mine = t_update.pawn && t_update.pawn == (uint32_t)ctx->gpr[3] && (uint32_t)ctx->gpr[4] == kRate;
    DOD3_FN_PAWN_RATE(ctx);
    dod3_drain(ctx);
    if (mine) t_update.has_rate = true, t_update.rate = (float)ctx->fpr[1];
}

/* Slot 0x8F0: a gauge (r4) changed by f1. */
void hook_gauge_change(ppu_context* ctx)
{
    const uint32_t pawn = (uint32_t)ctx->gpr[3];
    if (t_update.pawn == pawn && t_update.has_rate && ((uint32_t)ctx->gpr[4] & 0xFF) == kBlood) {
        t_update.has_rate = false;
        /* the share as the title works it out (single precision, in its order) */
        const float steps = guest_float(pawn + kFrameSteps);
        const float whole = guest_float(vm_read32(pawn + kParams) + kDrainMag) * steps;
        const float rate = t_update.rate;
        const float share = (0.0 - (double)rate >= 0.0) ? whole : whole * (1.0f - rate);
        const double passed = ctx->fpr[1];
        if (isfinite(share) && trunc((double)share) == passed) {
            Rest* r = rest_of(pawn, true);
            if (s_carry) {
                const double sum = r->rest + (double)share;
                const double take = trunc(sum);
                r->rest = sum - take;
                ctx->fpr[1] = take;
            }
            if (s_log) {
                r->taken += ctx->fpr[1];
                r->frames++;
                r->steps += steps;
                if (r->steps >= 60.0f) {
                    fprintf(stderr, "[gauge] %08X: %.0f off the gauge in %u frames (%.2f s)\n", pawn, r->taken,
                            r->frames, (double)r->steps / 60.0);
                    r->taken = 0.0, r->frames = 0, r->steps = 0.0f;
                }
            }
        } else {
            if (Rest* r = rest_of(pawn, false)) r->rest = 0.0;   /* not the share (or not a number): nothing carried over it */
            if (s_log)
                fprintf(stderr, "[gauge] %08X: the title took %g where the share works out as %g: left alone\n", pawn,
                        passed, (double)share);
        }
    }
    DOD3_FN_GAUGE_CHANGE(ctx);
    dod3_drain(ctx);
}

}  // namespace
#endif

extern "C" void ppu_register_function(uint64_t addr, void (*fn)(ppu_context*));

void dod3_gauge_drain_install()
{
#ifdef DOD3_FN_MODE_UPDATE
    if (const char* e = getenv("DOD3_GAUGE_DRAIN")) s_carry = strcmp(e, "0") != 0;
    if (const char* e = getenv("DOD3_GAUGE_LOG")) s_log = atoi(e) != 0;
    if (!s_carry && !s_log) return;
    ppu_register_function(DOD3_A_MODE_UPDATE, hook_mode_update);
    ppu_register_function(DOD3_A_PAWN_RATE, hook_rate);
    ppu_register_function(DOD3_A_GAUGE_CHANGE, hook_gauge_change);
    if (!s_carry) fprintf(stderr, "[gauge] the Intoner gauge's drain: the title's own (DOD3_GAUGE_DRAIN=0)\n");
#endif
}
