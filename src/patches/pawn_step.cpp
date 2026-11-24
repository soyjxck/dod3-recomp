/*
 * Gravity at frame rates off the title's 60 Hz grid (issue #26: "on a higher
 * frame rate your jump is shorter").
 *
 * A pawn's movement runs in steps of 1/60 s. Each frame, the pawn's update
 * (func_009531B0, vtable slot 0x79C, the same function for all 70 pawn
 * classes) takes T = the frame's time in steps, runs floor(T) whole steps
 * and, when more than 0.01 of a step is left over, one more for the rest:
 * a step is the move algorithms (slot 0x7A0, func_009533C4), then slot 0x7B8
 * (func_00954200), which adds the step's move to the frame's, times the
 * step's weight -- 1, or the left-over fraction.
 *
 * The weight reaches the move and nothing else. The gravity step inside the
 * algorithms (func_0099DB48) takes gravity / 120 off the vertical speed
 * (+0x1280) every time it is called, a part step too: the pawn moves by the
 * fraction and its speed ages a whole step. At 30 and 60 frames a second no
 * step is left over and nothing shows; at 120 every frame is half a step, so
 * gravity acts twice as often -- a jump reaches half its height, and three
 * quarters at 72. The console did it too, in the frames where it fell under
 * 30.
 *
 * Here a part step's loss of vertical speed is weighted as its move is: the
 * two slots are wrapped (both are reached through the vtable only from that
 * loop, so the function registry replaces them without a re-lift), the first
 * noting the speed before and after the step, the second, when the weight is
 * under 1 and the step slowed the pawn, giving back the rest. A step that
 * speeds the pawn upward -- the start of a jump, a launch, a landing's reset
 * to rest -- is left as it is.
 *
 * DOD3_STEP_GRAVITY=0 keeps the title's own behaviour. DOD3_STEP_LOG=1
 * prints each jump of the player's pawn: the height reached above the
 * take-off and the steps it took, whole and part (2: every step as well).
 *
 * Measured on the 1.01 EBOOT in the prologue, Zero's jump from a run: 74.1
 * units high at 60 fps (27 whole steps); at 120 fps 48.3 as the title has
 * it, 72.7 with the weighting (the speed falls by 0.817 a whole step, by
 * weight x 0.817 in a part one).
 */
#include "cpu/ppu.h"
#include "cpu/eboot.h"   /* the EBOOT version's addresses */
#include "ppu_vm_page.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef DOD3_FN_PAWN_STEP
namespace {

const uint32_t kSpeedZ = 0x1280;   /* the pawn's vertical speed, a step's worth of move */
const uint32_t kLocationZ = 0x5C;  /* AActor::Location.Z */

struct Step {
    uint32_t pawn;
    float before, after;   /* the vertical speed around the step's algorithms */
};
thread_local Step t_step;

bool s_weighted = true;   /* DOD3_STEP_GRAVITY=0: the title's own */
int s_log;                /* DOD3_STEP_LOG=1 (2: every step of a jump too) */

/* DOD3_STEP_LOG: the player's pawn's jump in progress */
struct Jump {
    bool up, down;
    float from, top;
    int whole, part;
} s_jump;

float guest_float(uint32_t ea)
{
    const uint32_t w = vm_read32(ea);
    float f;
    memcpy(&f, &w, 4);
    return f;
}
void set_guest_float(uint32_t ea, float f)
{
    uint32_t w;
    memcpy(&w, &f, 4);
    vm_write32(ea, w);
}

bool readable(uint32_t a, uint32_t n)
{
    if (!a || a + n < a) return false;
    for (uint32_t p = a >> 16; p <= (a + n - 1) >> 16; p++)
        if (!(__atomic_load_n(&g_ppu_vm_page[p], __ATOMIC_RELAXED) & PPU_VM_PAGE_RD)) return false;
    return true;
}

/* Is this the pawn the player's controller holds? */
bool player_pawn(uint32_t pawn)
{
    if (!readable(pawn + DOD3_O_PAWN_CONTROLLER, 4)) return false;
    const uint32_t ctl = vm_read32(pawn + DOD3_O_PAWN_CONTROLLER);
    return readable(ctl, DOD3_O_CONTROLLER_PAWN + 4) && vm_read32(ctl) == DOD3_A_PLAYER_CONTROLLER_VTABLE &&
           vm_read32(ctl + DOD3_O_CONTROLLER_PAWN) == pawn;
}

/* A step of the player's pawn, for the log: rising from the first step with
 * an upward speed, a jump's line once the pawn is back at rest or rising
 * again. */
void log_step(uint32_t pawn, float speed, float weight)
{
    const float z = guest_float(pawn + kLocationZ);
    Jump& j = s_jump;
    if (j.up || j.down) {
        if (z > j.top) j.top = z;
        (weight < 1.0f ? j.part : j.whole)++;
        if (j.up && speed <= 0.0f) j.up = false, j.down = true;
        if (j.down && speed >= 0.0f) {
            fprintf(stderr, "[step] jump: %.1f high, %d whole steps and %d part\n", (double)(j.top - j.from), j.whole,
                    j.part);
            j = {};
        }
    }
    if (!j.up && !j.down && speed > 0.0f) j = { true, false, z, z, 0, 0 };
}

/* Slot 0x7A0: a step's move algorithms. */
void hook_step(ppu_context* ctx)
{
    const uint32_t pawn = (uint32_t)ctx->gpr[3];
    const float before = guest_float(pawn + kSpeedZ);
    DOD3_FN_PAWN_STEP(ctx);
    dod3_drain(ctx);
    t_step = { pawn, before, guest_float(pawn + kSpeedZ) };
}

/* Slot 0x7B8: the step's move, times its weight (f1), into the frame's. */
void hook_step_move(ppu_context* ctx)
{
    const uint32_t pawn = (uint32_t)ctx->gpr[3];
    const float weight = (float)ctx->fpr[1];
    if (t_step.pawn == pawn) {
        const Step s = t_step;
        t_step.pawn = 0;
        if (s_weighted && weight < 1.0f && weight > 0.0f && s.after < s.before)
            set_guest_float(pawn + kSpeedZ, s.before + (s.after - s.before) * weight);
        if (s_log && player_pawn(pawn)) {
            if (s_log > 1 && (s_jump.up || s_jump.down || s.after > 0.0f))
                fprintf(stderr, "[step] weight %.3f speed %.3f -> %.3f (%.3f) move %.3f z %.2f\n", (double)weight,
                        (double)s.before, (double)s.after, (double)guest_float(pawn + kSpeedZ),
                        (double)guest_float(pawn + 0x1290), (double)guest_float(pawn + kLocationZ));
            log_step(pawn, guest_float(pawn + kSpeedZ), weight);
        }
    }
    DOD3_FN_PAWN_STEP_MOVE(ctx);
    dod3_drain(ctx);
}

}  // namespace
#endif

extern "C" void ppu_register_function(uint64_t addr, void (*fn)(ppu_context*));

void dod3_pawn_step_install()
{
#ifdef DOD3_FN_PAWN_STEP
    if (const char* e = getenv("DOD3_STEP_GRAVITY")) s_weighted = strcmp(e, "0") != 0;
    if (const char* e = getenv("DOD3_STEP_LOG")) s_log = atoi(e);
    if (!s_weighted && !s_log) return;
    ppu_register_function(DOD3_A_PAWN_STEP, hook_step);
    ppu_register_function(DOD3_A_PAWN_STEP_MOVE, hook_step_move);
    if (!s_weighted) fprintf(stderr, "[step] gravity in part steps: the title's own (DOD3_STEP_GRAVITY=0)\n");
#endif
}
