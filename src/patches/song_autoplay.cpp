/*
 * Auto-play for the song of branch D's final mission (DOD3_SONG_AUTOPLAY=1,
 * the launcher's System tab): the port's form of the RPCS3 patch "Auto Play
 * Song of The D Branch Final Mission" by bucanero (BLUS31197 1.01).
 *
 * The song's notes are judged by func_0091D8F0, called through the note's
 * vtable: r3 the note, f2 the song's time, which it keeps at note +0x58; the
 * note's own time is at +0x50. Once a note is neither late nor locked out,
 * it asks the pad -- func_0091D4EC, "pressed this frame": the button's byte
 * at pad +0x260 is 1 and its byte of the frame before, at +0x278, is 0 --
 * whether any of eight buttons was just pressed, and judges that press
 * against the note's window. The patch rewrites four instructions so that
 * the question becomes "has the note's time come?": every note is pressed
 * on its time, and the player's own presses do not count.
 *
 * Lifted code is not patched in memory, so the judge is wrapped instead:
 * while the game's own judge runs, the eight buttons read as the patch's
 * answer -- the first pressed once the note's time has come, none before --
 * and they are put back as they were after it. DOD3_SONG_LOG=1 says what
 * each judged press came to.
 *
 * Checked on the 1.01 EBOOT, the final verse's first part played with no
 * button pressed: with it, 235 notes pressed on their time, 235 hits, the
 * song to its end and the ending after it; without it, Game Over a few
 * seconds into the song.
 */
#include "cpu/ppu.h"
#include "cpu/eboot.h"   /* the EBOOT version's addresses */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef DOD3_FN_SONG_JUDGE
namespace {

/* The buttons the judge asks about, in its order. */
const uint32_t kButtons[] = { 2, 3, 0, 1, 10, 13, 11, 14 };

float guest_float(uint32_t ea)
{
    const uint32_t w = vm_read32(ea);
    float f;
    memcpy(&f, &w, 4);
    return f;
}

bool song_log()
{
    static int on = -1;
    if (on < 0) on = getenv("DOD3_SONG_LOG") ? 1 : 0;
    return on != 0;
}

void hook_song_judge(ppu_context* ctx)
{
    const uint32_t note = (uint32_t)ctx->gpr[3];
    /* The pad as the judge finds it (note +0x54, +0x410, +0x598); without
     * one it returns before asking. */
    const uint32_t owner = note ? vm_read32(note + 0x54u) : 0;
    const uint32_t input = owner ? vm_read32(owner + 0x410u) : 0;
    const uint32_t pad = input ? vm_read32(input + 0x598u) : 0;
    if (!pad) {
        if (song_log()) fprintf(stderr, "[song] note %08X judged without a pad\n", note);
        DOD3_FN_SONG_JUDGE(ctx);
        dod3_drain(ctx);
        return;
    }
    /* The time the judge keeps at +0x58 before it asks: f2 as a single. */
    const bool due = !((float)ctx->fpr[2] < guest_float(note + 0x50u));
    uint8_t was[8][2];
    for (int i = 0; i < 8; i++) {
        const uint32_t b = kButtons[i];
        was[i][0] = vm_read8(pad + 0x260u + b);
        was[i][1] = vm_read8(pad + 0x278u + b);
        vm_write8(pad + 0x260u + b, due && i == 0 ? 1 : 0);
        vm_write8(pad + 0x278u + b, 0);
    }
    DOD3_FN_SONG_JUDGE(ctx);
    dod3_drain(ctx);
    for (int i = 0; i < 8; i++) {
        const uint32_t b = kButtons[i];
        vm_write8(pad + 0x260u + b, was[i][0]);
        vm_write8(pad + 0x278u + b, was[i][1]);
    }
    static unsigned calls;
    calls++;
    if (song_log() && (due || calls == 1 || calls % 1024 == 0))
        fprintf(stderr, "[song] judge call %u, note %08X%s: judged %u (1 late, 2 a hit)\n", calls, note,
                due ? " pressed on its time" : "", (unsigned)ctx->gpr[3]);
}

}  // namespace
#endif

extern "C" void ppu_register_function(uint64_t addr, void (*fn)(ppu_context*));

void dod3_song_autoplay_install()
{
#ifdef DOD3_FN_SONG_JUDGE
    const char* e = getenv("DOD3_SONG_AUTOPLAY");
    if (!e || atoi(e) != 1) return;
    ppu_register_function(DOD3_A_SONG_JUDGE, hook_song_judge);
    fprintf(stderr, "[song] auto-play: branch D's final song presses every note on its time\n");
#endif
}
