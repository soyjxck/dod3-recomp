/*
 * The camera's stick, and the mouse as one.
 *
 * Sqex03GamePlayerController.SendOrder hands the pawn's native code the
 * camera stick as two floats, GetCameraMoveX() and GetCameraMoveY() -- the
 * right stick's GetAxis -- and that code turns the view by
 *
 *     yaw   += 3440 x X x dt^2   degrees
 *     pitch -= 2000..3000 x Y x dt^2   (by where the pitch is; -48..+17)
 *
 * (dt the frame's time in seconds; SendOrder's fDelta is that in sixtieths)
 * with no dead zone, no limit on the value (16 turns sixteen times as fast)
 * and no easing in or out: measured from the view's rotation frame by frame
 * with constant values in place of the stick's. Two things follow.
 *
 * The frame rate. The square is the title's: a turn speed in degrees a
 * second that is proportional to the frame time, right at the 30 frames a
 * second it was made for (113 degrees a second at full stick) and a quarter
 * of that at 120. The stick's value is scaled by (1/30) / dt, so it turns
 * the view as it does on the console at any rate above 30 (below, it is left
 * alone). DOD3_CAMERA_SPEED=original turns that off. On the dragon the
 * stick is not scaled: its camera has not been measured.
 *
 * The mouse. Its motion since the last frame is a turn in degrees; divided
 * by the constant and dt^2 it is the value that makes exactly that turn,
 * added to the stick's. No stick is imitated, so nothing of a stick's range
 * or dead zone comes between the hand and the view. The pitch's constant
 * falls away towards the ends of its range (the camera eases into them); it
 * is read off the view's own pitch, which the script reports every frame, so
 * the mouse moves the view evenly until the limit itself.
 *
 * On the dragon in the sky, while its shooting cursor is on screen, the
 * mouse moves that cursor instead, and only what goes past the screen's
 * edge turns the view (src/patches/dragon_cursor.cpp).
 *
 *   DOD3_MOUSE=0            no mouse look (and the cursor is left alone)
 *   DOD3_MOUSE_SENS=<x>     1 is 0.05 degrees a count (default)
 *   DOD3_CAM_LOG=1          a line a frame: the view's rotation, the stick,
 *                           the mouse's values, the frame's time
 *   DOD3_CAM_TEST_FILE=<path>  "x y" in the file is added to the two values,
 *                           to measure the camera with
 *   DOD3_MOUSE_TEST_FILE=<path>  "dx dy" in the file is added to the mouse's
 *                           counts every frame, the window's focus not needed
 *
 * The title's own camera settings (Controller Settings: left/right and
 * up/down inverted, their sensitivities) are applied after this, to the
 * mouse's share as to the stick's.
 */
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <atomic>
#include "input/camera.h"
#include "rsx_host_mouse.h"
#include "util.h"

namespace {

const float kYaw = 3440.0f;           /* degrees per unit of X per second squared */
const float kDegreesPerCount = 0.05f; /* at DOD3_MOUSE_SENS 1 */
const float kReferenceFrame = 1.0f / 30.0f;

/* The pitch's constant by the view's pitch in degrees (it eases into both
 * ends of -48.2..+17.1), from the same measurement. */
const struct {
    float pitch, k;
} kPitch[] = { { -48.0f, 320.0f }, { -42.0f, 620.0f }, { -36.0f, 1560.0f }, { -30.0f, 2150.0f }, { -18.0f, 2500.0f },
               { -6.0f, 3000.0f }, { 6.0f, 2900.0f },  { 12.0f, 2760.0f },  { 17.0f, 2000.0f } };
const float kPitchFloor = 700.0f;     /* no more gain than this near the ends */

float pitch_constant(float pitch)
{
    const int n = (int)(sizeof kPitch / sizeof kPitch[0]);
    float k = kPitch[n - 1].k;
    if (pitch <= kPitch[0].pitch) {
        k = kPitch[0].k;
    } else {
        for (int i = 1; i < n; i++)
            if (pitch < kPitch[i].pitch) {
                const float t = (pitch - kPitch[i - 1].pitch) / (kPitch[i].pitch - kPitch[i - 1].pitch);
                k = kPitch[i - 1].k + t * (kPitch[i].k - kPitch[i - 1].k);
                break;
            }
    }
    return k < kPitchFloor ? kPitchFloor : k;
}

bool s_init;
bool s_mouse = true, s_fps_fix = true, s_log;
float s_sens = 1.0f;
const char* s_test_file;
const char* s_mouse_test_file;

float s_dt = kReferenceFrame;   /* this frame's time, seconds */
float s_scale = 1.0f;           /* the stick's, for the frame rate */
float s_mouse_x, s_mouse_y;     /* the mouse's share of the two values this frame */
float s_test_x, s_test_y;
float s_stick_x, s_stick_y;     /* the title's own values, as last asked */
float s_view_pitch;             /* the gameplay view's pitch, degrees */
uint64_t s_frame_time;
std::atomic<uint64_t> s_mouse_time{ 0 };

/* The dragon's shooting cursor (src/patches/dragon_cursor.cpp): while it is on
 * screen, the mouse moves it and only what goes past the screen's edge turns
 * the view. A count moves it about as far across the screen as it turns the
 * view (0.05 degrees of a view some 70 degrees wide), the same distance up
 * as across. */
const float kCursorPerCount = 0.0007f;   /* of the screen's width, at DOD3_MOUSE_SENS 1 */
const float kScreenAspect = 16.0f / 9.0f;
uint64_t s_cursor_time;                  /* when the cursor last took the mouse */
float s_spill_x, s_spill_y;              /* counts past the screen's edge, for the view */

void read_sensitivity()
{
    s_sens = 1.0f;
    if (const char* e = getenv("DOD3_MOUSE_SENS")) {
        const double v = atof(e);
        if (v > 0.01 && v < 100.0) s_sens = (float)v;
    }
}

void init()
{
    if (s_init) return;
    s_init = true;
    if (const char* e = getenv("DOD3_MOUSE")) s_mouse = strcmp(e, "0") != 0;
    read_sensitivity();
    if (const char* e = getenv("DOD3_CAMERA_SPEED")) s_fps_fix = strcmp(e, "original") != 0;
    s_log = getenv("DOD3_CAM_LOG") != nullptr;
    s_test_file = getenv("DOD3_CAM_TEST_FILE");
    s_mouse_test_file = getenv("DOD3_MOUSE_TEST_FILE");
}

/* The mouse's counts since the last call, and DOD3_MOUSE_TEST_FILE's. */
void take_mouse(int* dx, int* dy)
{
    rsx_host_mouse_take_motion(dx, dy);
    if (!s_mouse_test_file) return;
    int a[2] = { 0, 0 };
    if (FILE* f = fopen(s_mouse_test_file, "r")) {
        if (fscanf(f, "%d %d", &a[0], &a[1]) < 2) a[0] = a[1] = 0;
        fclose(f);
    }
    *dx += a[0];
    *dy += a[1];
}

}  // namespace

/* The frame's fDelta, at the head of the camera's part of SendOrder: the
 * mouse's motion since the last one becomes this frame's share. */
void dod3_camera_frame(float delta, int dragon)
{
    init();
    const uint64_t t = dod3_now_us();
    const bool stale = t - s_frame_time > 250000;   /* the camera was not ours for a while: a menu, a cutscene */
    s_frame_time = t;
    float dt = delta / 60.0f;   /* SendOrder's fDelta is in sixtieths of a second */
    if (!(dt > 0.0005f)) dt = 0.0005f;
    if (dt > 0.1f) dt = 0.1f;
    s_dt = dt;
    /* Zero's camera is the one measured; the dragon's stick is left as the title has it. */
    s_scale = s_fps_fix && !dragon && dt < kReferenceFrame ? kReferenceFrame / dt : 1.0f;

    int dx = 0, dy = 0;
    if (s_mouse && t - s_cursor_time < 100000) {   /* the dragon's cursor has the mouse: what went past the edge */
        dx = (int)s_spill_x;
        dy = (int)s_spill_y;
        s_spill_x -= (float)dx;
        s_spill_y -= (float)dy;
    } else if (s_mouse) {
        take_mouse(&dx, &dy);
    }
    if (stale) dx = dy = 0;
    const float degrees = s_sens * kDegreesPerCount;
    s_mouse_x = (float)dx * degrees / (kYaw * dt * dt);
    s_mouse_y = (float)dy * degrees / (pitch_constant(s_view_pitch) * dt * dt);
    if (dx || dy) s_mouse_time.store(t, std::memory_order_relaxed);

    if (s_test_file) {
        static uint64_t next;
        if (t >= next) {
            next = t + 20000;
            float a[2] = { 0, 0 };
            if (FILE* f = fopen(s_test_file, "r")) {
                if (fscanf(f, "%f %f", &a[0], &a[1]) < 1) a[0] = a[1] = 0;
                fclose(f);
            }
            s_test_x = a[0];
            s_test_y = a[1];
        }
    }
}

bool dod3_camera_cursor_motion(float* dx, float* dy)
{
    init();
    *dx = *dy = 0.0f;
    if (!s_mouse) return false;
    const uint64_t t = dod3_now_us();
    const bool fresh = t - s_cursor_time < 250000;   /* at the cursor's first frame, the motion before was the view's */
    s_cursor_time = t;
    int mx = 0, my = 0;
    take_mouse(&mx, &my);
    if (!fresh) {
        s_spill_x = s_spill_y = 0.0f;
        return false;
    }
    if (mx || my) s_mouse_time.store(t, std::memory_order_relaxed);
    *dx = (float)mx * s_sens * kCursorPerCount;
    *dy = (float)my * s_sens * kCursorPerCount * kScreenAspect;
    return mx || my;
}

void dod3_camera_cursor_spill(float dx, float dy)
{
    s_spill_x += dx / (s_sens * kCursorPerCount);
    s_spill_y += dy / (s_sens * kCursorPerCount * kScreenAspect);
}

int dod3_camera_active() { return s_mouse_x != 0.0f || s_mouse_y != 0.0f || s_test_x != 0.0f || s_test_y != 0.0f; }

float dod3_camera_axis(int axis, float stick)
{
    if (axis == 0) {
        s_stick_x = stick;
        return stick * s_scale + s_mouse_x + s_test_x;
    }
    s_stick_y = stick;
    return stick * s_scale + s_mouse_y + s_test_y;
}

void dod3_camera_view(int pitch, int yaw, int gameplay)
{
    init();
    const float p = (float)(int16_t)pitch * (360.0f / 65536.0f);
    if (gameplay) s_view_pitch = p;
    if (!s_log) return;
    static uint64_t t0;
    static int last, turns;
    const uint64_t t = dod3_now_us();
    if (!t0) t0 = t;
    yaw &= 0xFFFF;
    if (yaw - last > 32768) turns--;
    if (yaw - last < -32768) turns++;
    last = yaw;
    fprintf(stderr, "[cam] %.4f yaw %.3f pitch %.3f add %.3f %.3f base %.3f %.3f dt %.5f scale %.2f %s\n",
            (double)(t - t0) / 1e6, (yaw + turns * 65536) * 360.0 / 65536.0, (double)p, (double)(s_mouse_x + s_test_x),
            (double)(s_mouse_y + s_test_y), (double)s_stick_x, (double)s_stick_y, (double)s_dt, (double)s_scale,
            gameplay ? "game" : "other");
}

unsigned long long dod3_camera_mouse_time() { return s_mouse_time.load(std::memory_order_relaxed); }

void dod3_camera_reload() { read_sensitivity(); }
