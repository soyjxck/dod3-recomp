/*
 * The mouse moves the dragon's shooting cursor (issue #22).
 *
 * In the sky the dragon aims with a cursor on the screen -- the red ring of
 * the lock-on -- and holding Square sweeps it over enemies to lock them, one
 * shot each at the release. The title moves that cursor with the left stick:
 * ASqex03GamePawnPlayerDragon::UpdateShootingCursor (func_009D26F8, reached by
 * the pawn's vtable) keeps it in m_xTarget.m_xShooting.m_vCurPos, a fraction
 * of the screen's width and height, inside m_fShootingRect_Up/_Bottom/_Left/
 * _Right (0.05 to 0.95), and lets go of it as the stick does: it springs back
 * to the top of the screen. The mouse look (src/input/camera.cpp) turns the
 * view through the right stick's values, so on the keyboard and mouse the
 * cursor answered only to WASD and stayed at the top however the mouse moved:
 * the lock-on found nothing to lock, and the release fired a single shot.
 *
 * So while the cursor is on screen (IsEnableShootingCursor, func_0096B9A8) the
 * mouse moves it, before the title's own update reads it; the part of a move
 * that goes past the edge of its rectangle turns the view as before. Once
 * the mouse has moved it, the cursor stays where the mouse put it -- the
 * title's spring back is undone after its update -- until a stick (a pad's,
 * or WASD) moves it again, which hands it back to the title as it was.
 * DOD3_MOUSE=0 leaves the mouse out, as everywhere.
 *
 * Checked on the 1.01 EBOOT, Ch.2 Verse 6 Part 1 (the first flight), at 120
 * fps: before this the mouse's values reached the view only, never the cursor
 * (it stayed at the top), while a pad's left stick locked and fired at 30 and
 * at 120 alike. With it, mouse counts (DOD3_MOUSE_TEST_FILE) move the cursor,
 * it stays where they leave it, and the lock-on locks a wyvern and fires; a
 * pad with the mouse look on and the mouse still is as before (the mouse
 * never takes the cursor). DOD3_CURSOR_LOG=1 says when it does.
 */
#include "cpu/ppu.h"
#include "cpu/eboot.h"   /* the EBOOT version's addresses */
#include "input/camera.h"
#include <algorithm>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef DOD3_FN_DRAGON_SHOOTING_CURSOR
namespace {

/* ASqex03GamePawn: the stick the cursor's update reads (the pair at kStickA
 * when the byte at kStickModeA is 3, else the one at kStickB when the byte at
 * kStickModeB is 3), and the cursor's speed (m_fCurSpeedX, _Y). */
const uint32_t kStickA = 0x464, kStickB = 0x46C, kStickModeA = 0x5E0, kStickModeB = 0x840, kCursorSpeed = 0x13E0;
const float kStickDead = 0.2f;

bool s_held;            /* the mouse put the cursor where it is */
float s_held_x, s_held_y;
int s_log = -1;         /* DOD3_CURSOR_LOG=1: a line when the mouse takes or gives back the cursor */

float guest_float(uint32_t ea)
{
    const uint32_t w = vm_read32(ea);
    float f;
    memcpy(&f, &w, 4);
    return f;
}

void put_guest_float(uint32_t ea, float f)
{
    uint32_t w;
    memcpy(&w, &f, 4);
    vm_write32(ea, w);
}

/* Is a stick moving the cursor? The update reads (x, y) from one pair or the
 * other by two mode bytes, as below; y is +1 -- up, the cursor's home -- (or
 * 0) once the stick is let go, so a stick held fully up reads as let go: it
 * sends the cursor home all the same. */
bool stick_pushed(uint32_t pawn)
{
    const uint32_t o = vm_read8(pawn + kStickModeA) == 3 ? kStickA : vm_read8(pawn + kStickModeB) == 3 ? kStickB : 0;
    if (!o) return false;
    const float x = guest_float(pawn + o), y = guest_float(pawn + o + 4);
    return fabsf(x) > kStickDead || y < -kStickDead || (y > kStickDead && y < 1.0f - kStickDead);
}

void say(const char* what)
{
    if (s_log < 0) s_log = getenv("DOD3_CURSOR_LOG") ? 1 : 0;
    if (s_log) fprintf(stderr, "[cursor] %s\n", what);
}

void hook_dragon_cursor(ppu_context* ctx)
{
    const uint32_t pawn = (uint32_t)ctx->gpr[3];
    bool on = false;
    if (pawn) {
        DOD3_FN_SHOOTING_CURSOR_ON(ctx);   /* a leaf: r3 = this in, the answer out */
        dod3_drain(ctx);
        on = (uint32_t)ctx->gpr[3] != 0;
        ctx->gpr[3] = pawn;
    }
    const uint32_t cur = pawn + DOD3_O_SHOOTING_CURSOR, rect = pawn + DOD3_O_SHOOTING_RECT;
    float dx, dy;
    if (!on) {
        if (s_held) say("given back (the cursor is off)");
        s_held = false;
    } else if (s_held && stick_pushed(pawn)) {
        s_held = false;
        char why[96];
        snprintf(why, sizeof why, "given back to the stick (%.2f %.2f / %.2f %.2f)", guest_float(pawn + kStickA),
                 guest_float(pawn + kStickA + 4), guest_float(pawn + kStickB), guest_float(pawn + kStickB + 4));
        say(why);
    } else if (dod3_camera_cursor_motion(&dx, &dy)) {
        const float up = guest_float(rect), bottom = guest_float(rect + 4);
        const float left = guest_float(rect + 8), right = guest_float(rect + 12);
        const float x = (s_held ? s_held_x : guest_float(cur)) + dx;
        const float y = (s_held ? s_held_y : guest_float(cur + 4)) + dy;
        if (left < right && up < bottom) {
            if (!s_held) say("taken by the mouse");
            s_held = true;
            s_held_x = std::clamp(x, left, right);
            s_held_y = std::clamp(y, up, bottom);
            dod3_camera_cursor_spill(x - s_held_x, y - s_held_y);
        }
    }
    if (s_held) {   /* where the mouse left it, for the update to aim from */
        put_guest_float(cur, s_held_x);
        put_guest_float(cur + 4, s_held_y);
    }
    DOD3_FN_DRAGON_SHOOTING_CURSOR(ctx);
    dod3_drain(ctx);
    if (s_held) {   /* the title's spring back to the top, undone */
        put_guest_float(cur, s_held_x);
        put_guest_float(cur + 4, s_held_y);
        put_guest_float(pawn + kCursorSpeed, 0.0f);
        put_guest_float(pawn + kCursorSpeed + 4, 0.0f);
    }
}

}  // namespace
#endif

extern "C" void ppu_register_function(uint64_t addr, void (*fn)(ppu_context*));

void dod3_dragon_cursor_install()
{
#ifdef DOD3_FN_DRAGON_SHOOTING_CURSOR
    ppu_register_function(DOD3_A_DRAGON_SHOOTING_CURSOR, hook_dragon_cursor);
#endif
}
