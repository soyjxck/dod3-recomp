/*
 * Cheats (the launcher's Cheats tab), after the PS3 code list for the
 * Japanese release (NetCheat/Artemis, BLJM61043 1.01; games24.blog.fc2.com),
 * found again in the 1.01 EBOOT by their byte patterns. Those codes rewrite
 * instructions; lifted code is not patched in memory, so here each is the
 * function the code patches, wrapped -- all three are reached only through a
 * vtable, so the function registry replaces them without a re-lift:
 *
 *   DOD3_CHEAT_HEALTH=1   the player's health stays full.
 *   DOD3_CHEAT_INTONER=1  the player's Intoner (blood) gauge stays full.
 *       A pawn's gauges are set through func_0095B0CC (r4 the gauge, 0-6;
 *       r5 the value, which it clamps to the gauge's range and stores at
 *       +0x7EC..+0x80C): gauge 0 is health (soldiers 210, Zero 300 in the
 *       prologue), gauge 5 the blood that fills the Intoner gauge as she
 *       fights. For the player's pawn it is asked for the largest value, so
 *       the game's own clamp makes it the maximum. The codes ("SP gauge does
 *       not decrease", "Intoner gauge max when changed") changed the store
 *       for every pawn, the enemies too; here only the pawn the player's
 *       controller holds (APawn::Controller, whose Pawn is this one): Zero,
 *       or the dragon in the sky.
 *   DOD3_CHEAT_ITEMS=1    items are not used up: RemoveItem(id, count)
 *       (func_00A6C560) takes a count of 0 -- the code nopped the subtraction.
 *   DOD3_CHEAT_TIME=1     a mission's time limit stands still: its tick
 *       (func_009F3ED8), which takes the frame's time from the limit at +0xD4
 *       and uses it for nothing else, gets 0 -- the code nopped that fsubs.
 *
 * The list's other codes -- jump higher, the camera's pitch limit -- patch
 * functions with direct callers: those would need a re-lift (--insn-patches).
 * DOD3_CHEAT_LOG=1 says what each wrapped call was asked for.
 */
#include "cpu/ppu.h"
#include "cpu/eboot.h"   /* the EBOOT version's addresses */
#include "ppu_vm_page.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef DOD3_FN_GAUGE_SET
namespace {

enum { kHealth = 0, kBlood = 5, kGauges = 7 };
/* Where each gauge is kept (the setter's switch). */
const uint32_t kGaugeField[kGauges] = { 0x7EC, 0x7F4, 0x7FC, 0x800, 0x804, 0x808, 0x80C };

unsigned s_full;   /* bit n: the player's gauge n is kept full */

bool cheat_log()
{
    static int on = -1;
    if (on < 0) on = getenv("DOD3_CHEAT_LOG") ? 1 : 0;
    return on != 0;
}

bool cheat_on(const char* name)
{
    const char* e = getenv(name);
    return e && atoi(e) == 1;
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

void hook_gauge_set(ppu_context* ctx)
{
    const uint32_t pawn = (uint32_t)ctx->gpr[3];
    const unsigned type = (unsigned)(ctx->gpr[4] & 0xFF);
    const int32_t asked = (int32_t)ctx->gpr[5];
    const bool full = type < kGauges && (s_full >> type & 1) && player_pawn(pawn);
    if (full) ctx->gpr[5] = 0x7FFFFFFF;
    const bool log = cheat_log() && type < kGauges && readable(pawn + kGaugeField[type], 4);
    const int32_t before = log ? (int32_t)vm_read32(pawn + kGaugeField[type]) : 0;
    DOD3_FN_GAUGE_SET(ctx);
    dod3_drain(ctx);
    if (log) {
        const int32_t after = (int32_t)vm_read32(pawn + kGaugeField[type]);
        if (after != before || full)
            fprintf(stderr, "[cheat] %s %08X gauge %u: %d, asked %d -> %d%s\n", player_pawn(pawn) ? "player" : "pawn",
                    pawn, type, before, asked, after, full ? " (kept full)" : "");
    }
}

void hook_item_remove(ppu_context* ctx)
{
    if (cheat_log())
        fprintf(stderr, "[cheat] RemoveItem(%08X, %d)%s\n", (uint32_t)ctx->gpr[4], (int32_t)ctx->gpr[5],
                (int32_t)ctx->gpr[5] > 0 ? ": none taken" : "");
    if ((int32_t)ctx->gpr[5] > 0) ctx->gpr[5] = 0;
    DOD3_FN_ITEM_REMOVE(ctx);
    dod3_drain(ctx);
}

void hook_time_limit_tick(ppu_context* ctx)
{
    if (cheat_log()) {
        static unsigned n;
        const uint32_t w = vm_read32((uint32_t)ctx->gpr[3] + 0xD4u);
        float left;
        memcpy(&left, &w, 4);
        if (++n % 300 == 1) fprintf(stderr, "[cheat] time limit %08X: %.1f s left, held\n", (uint32_t)ctx->gpr[3], left);
    }
    ctx->fpr[1] = 0.0;
    DOD3_FN_TIME_LIMIT_TICK(ctx);
    dod3_drain(ctx);
}

}  // namespace
#endif

extern "C" void ppu_register_function(uint64_t addr, void (*fn)(ppu_context*));

void dod3_cheats_install()
{
#ifdef DOD3_FN_GAUGE_SET
    if (cheat_on("DOD3_CHEAT_HEALTH")) s_full |= 1u << kHealth;
    if (cheat_on("DOD3_CHEAT_INTONER")) s_full |= 1u << kBlood;
    const bool items = cheat_on("DOD3_CHEAT_ITEMS"), time = cheat_on("DOD3_CHEAT_TIME");
    if (s_full || cheat_log()) ppu_register_function(DOD3_A_GAUGE_SET, hook_gauge_set);
    if (items) ppu_register_function(DOD3_A_ITEM_REMOVE, hook_item_remove);
    if (time) ppu_register_function(DOD3_A_TIME_LIMIT_TICK, hook_time_limit_tick);
    if (s_full || items || time)
        fprintf(stderr, "[cheat] on:%s%s%s%s\n", (s_full >> kHealth & 1) ? " health" : "",
                (s_full >> kBlood & 1) ? " intoner gauge" : "", items ? " items" : "", time ? " time limits" : "");
#endif
}
