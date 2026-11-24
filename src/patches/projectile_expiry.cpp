/*
 * A projectile out of its range, above 100 frames a second (issue #38: "the
 * dragon is not shooting after locking multiple enemies").
 *
 * A projectile's update (ASqex03Projectile, func_00A4A5F8, vtable slot
 * 0x1DC) ends one that has flown past its range (+0x264, when above 0) by
 * setting its LifeSpan (AActor, +0xD8) to 10 ms and calling its event
 * InformAndDestroyProjectile, and it does so on every update for as long as
 * the projectile is out of range. The engine takes the frame's time off
 * LifeSpan before that, once an update: at 30 or 60 frames a second the
 * 10 ms are gone at the next one and the projectile ends. With a frame
 * under 10 ms something is left, the update sets 10 ms again, and so on: the
 * projectile never ends, and the event runs every frame.
 *
 * The dragon's homing volley waits for such a projectile: one is made when
 * the lock-on is released, and the shots at the locked enemies are sent when
 * it ends. So above 100 frames a second the volley did not come: the locks
 * were taken and a lone shot left the dragon's mouth.
 *
 * Here the update is wrapped (it is reached through the vtable only, so the
 * function registry replaces it without a re-lift). While a projectile's
 * LifeSpan is running out -- above 0 and at most 10 ms, or negative -- the
 * range rule is kept from it for that update: the range reads 0, "no range",
 * and is put back afterwards, so the time left keeps falling and the event
 * is not called again. A LifeSpan of 0 (none) or above 10 ms is the title's
 * own case: 10 ms and the event, once. This is the rule of the frame-rate
 * patch RPCS3 ships for the title, as a wrapper.
 *
 * DOD3_PROJECTILE_EXPIRY=0 keeps the title's own behaviour.
 * DOD3_PROJECTILE_LOG=1 prints each projectile sent to its end by the range
 * rule and, with the fix off, how many updates in a row set the 10 ms again.
 *
 * Measured on the 1.01 EBOOT in "Taking Flight" with every frame 5 ms of
 * game time: as the title has it, the released lock-on's projectile was set
 * to 10 ms again more than 64 updates in a row and no homing shot followed
 * any of three releases (five, two and one locks); with the rule here it
 * ended at once and each release sent one shot for each lock.
 */
#include "cpu/ppu.h"
#include "cpu/eboot.h"   /* the EBOOT version's addresses */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef DOD3_FN_PROJECTILE_UPDATE
namespace {

const uint32_t kLifeSpan = 0xD8;          /* AActor::LifeSpan */
const uint32_t kRange = 0x264;            /* the projectile's range; 0 or less: none */
const uint32_t kTenMs = 0x3C23D70Au;      /* 0.01f, what the range rule sets */

bool s_fix = true;   /* DOD3_PROJECTILE_EXPIRY=0: the title's own */
bool s_log;          /* DOD3_PROJECTILE_LOG=1 */

/* for the log with the fix off: the projectile last set to 10 ms again */
thread_local uint32_t t_again;
thread_local unsigned t_again_n;

float as_float(uint32_t w)
{
    float f;
    memcpy(&f, &w, 4);
    return f;
}

void hook_update(ppu_context* ctx)
{
    const uint32_t p = (uint32_t)ctx->gpr[3];
    const uint32_t life = vm_read32(p + kLifeSpan);
    /* running out: a positive float of at most 10 ms (bit patterns of positive floats order as the floats do), or a negative one */
    const bool running_out = (life & 0x80000000u) ? (life << 1) != 0 : (life != 0 && life <= kTenMs);
    if (s_fix && running_out) {
        const uint32_t range = vm_read32(p + kRange);
        vm_write32(p + kRange, 0);
        DOD3_FN_PROJECTILE_UPDATE(ctx);
        dod3_drain(ctx);
        vm_write32(p + kRange, range);
        return;
    }
    DOD3_FN_PROJECTILE_UPDATE(ctx);
    dod3_drain(ctx);
    if (!s_log) return;
    const uint32_t after = vm_read32(p + kLifeSpan);
    if (after != kTenMs || after == life) {
        if (running_out && after == kTenMs && t_again == p) t_again_n++;   /* exactly 10 ms left, set again */
        return;
    }
    if (!running_out) {
        fprintf(stderr, "[projectile] %08X is out of its range: 10 ms to its end (it had %g s)\n", p,
                (double)as_float(life));
        t_again = p, t_again_n = 0;
    } else {   /* the fix is off */
        if (t_again != p) t_again = p, t_again_n = 0;
        t_again_n++;
        if ((t_again_n & (t_again_n - 1)) == 0)   /* 1, 2, 4, 8 ... */
            fprintf(stderr, "[projectile] %08X: %g s left, set to 10 ms again (%u in a row)\n", p,
                    (double)as_float(life), t_again_n);
    }
}

}  // namespace
#endif

extern "C" void ppu_register_function(uint64_t addr, void (*fn)(ppu_context*));

void dod3_projectile_expiry_install()
{
#ifdef DOD3_FN_PROJECTILE_UPDATE
    if (const char* e = getenv("DOD3_PROJECTILE_EXPIRY")) s_fix = strcmp(e, "0") != 0;
    if (const char* e = getenv("DOD3_PROJECTILE_LOG")) s_log = atoi(e) != 0;
    if (!s_fix && !s_log) return;
    ppu_register_function(DOD3_A_PROJECTILE_UPDATE, hook_update);
    if (!s_fix) fprintf(stderr, "[projectile] the range rule: the title's own (DOD3_PROJECTILE_EXPIRY=0)\n");
#endif
}
