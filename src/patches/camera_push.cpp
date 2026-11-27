/*
 * The push that keeps the player's pawn clear of a boxed-in camera, at
 * frame rates above the title's own.
 *
 * With the pawn's m_xCollision.m_bFirstCollideXY set, the camera moved in
 * by its own collision test (m_xCamera.m_bColHit) and the frame's move
 * about to leave the pawn within a radius of where that put the camera --
 * 250 units (400 for two of the pawn classes) times the animation's
 * m_fCameraEncScale and the mesh's scale -- the move is pushed along the
 * camera's view by
 *
 *     (radius - distance) x the animation's m_fCameraEncAddScale
 *
 * (func_00954330, the last thing the frame's move does to m_vGpDeltaMove).
 * In play it holds Zero off the camera whenever she runs towards it with
 * rock or a wall behind it. That is a share of the way out each time it
 * runs (0.2 for Zero's runs), and it runs once a frame, not once a 1/60 s
 * step: at 60 frames a second the pawn is pushed out twice as hard as at
 * the console's 30, at 120 four times.
 *
 * The function is called directly, from the pawn's frame move
 * (func_00955740, vtable slot 0x7BC, the same function for all 70 pawn
 * classes), which is reached through the vtable only: that one is wrapped
 * (the function registry replaces it without a re-lift), and for its length
 * the animation's m_fCameraEncAddScale reads as itself times the frame's
 * real time in 1/30 s -- the pawn's m_fDelta, which counts 1/60 s, over its
 * CustomTimeDilation and the world's slow motion, halved. Nothing else in
 * the EBOOT reads the value. This is the rule of the frame-rate patch RPCS3
 * ships for the title ("ceiling collision speed"), as a wrapper, and like
 * it only for the player's pawn (the GameInfo's m_xPlayerPawn).
 *
 * DOD3_CAMERA_PUSH=0 keeps the title's own behaviour. DOD3_CAMERA_PUSH_LOG=1
 * prints, at most ten times a second, the frames in which the push's
 * conditions held as the move began: the distance, the radius, the share
 * and the factor.
 *
 * Measured on the 1.01 EBOOT in "The Warpath", the left stick held towards
 * the camera for 14 s in the fight the part opens on, Zero's mean distance
 * from the camera over the last 7 s (the radius is 250; the fight around
 * her moves a run's figure by some 20 units). As the title has it: 142
 * units at 30 fps, 195 at 60, 214 at 120. With the factor: 129, 152, 132.
 */
#include "cpu/ppu.h"
#include "cpu/eboot.h"   /* the EBOOT version's addresses */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#ifdef DOD3_FN_PAWN_FRAME_MOVE
namespace {

const uint32_t kLocation = 0x54;            /* AActor::Location */
const uint32_t kCustomTimeDilation = 0x8C;  /* AActor */
const uint32_t kMesh = 0x384;               /* APawn::Mesh */
const uint32_t kMeshScale = 0x1B8;          /* ... its scale */
const uint32_t kGameInfo = 0x460;           /* ASqex03GamePawn::m_xGameInfo */
const uint32_t kPlayerPawn = 0x414;         /* ... m_xPlayerPawn */
const uint32_t kSlomo = 0x460;              /* ... m_xSlomo */
const uint32_t kWorldSlomo = 0x1CC;         /* USqex03GameSlomo::m_fWorldSlomoValue */
const uint32_t kFrameSteps = 0x51C;         /* m_fDelta: the frame's time in 1/60 s */
const uint32_t kDeltaMove = 0x574;          /* m_vGpDeltaMove: the frame's move */
const uint32_t kClassTag = 0x5E0;           /* m_xTag.m_eClass */
const uint32_t kAnimSequence = 0x5EC;       /* m_AnimSequence */
const uint32_t kEncScale = 0x188;           /* ... m_fCameraEncScale */
const uint32_t kEncAddScale = 0x18C;        /* ... m_fCameraEncAddScale */
const uint32_t kCollisionFlags = 0x7E0;     /* m_xCollision's flags: 0x80000000 m_bFirstCollideXY */
const uint32_t kCameraType = 0x840;         /* m_xCamera.m_eType */
const uint32_t kCameraFlags = 0x844;        /* m_xCamera's flags: 0x00010000 m_bColHit */
const uint32_t kCameraPos = 0x8F0;          /* m_xCamera.m_xNowParam.m_vLocationAfterCol (x, y) */
const uint32_t kCameraOfsFlags = 0xF80;     /* m_xCamera.m_xOfsProduct's flags: 0x20000000 m_bCalcCollision */

bool s_fix = true;   /* DOD3_CAMERA_PUSH=0: the title's own */
bool s_log;          /* DOD3_CAMERA_PUSH_LOG=1 */

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

/* The frame's real time in 1/30 s for the player's pawn; 1 for any other
 * pawn and whenever a value is not a time (RPCS3's guards). */
float frame_factor(uint32_t pawn)
{
    const uint32_t gi = vm_read32(pawn + kGameInfo);
    if (!gi || vm_read32(gi + kPlayerPawn) != pawn) return 1.0f;
    const uint32_t slomo = vm_read32(gi + kSlomo);
    if (!slomo) return 1.0f;
    const float dilation = guest_float(pawn + kCustomTimeDilation) * guest_float(slomo + kWorldSlomo);
    const float steps = guest_float(pawn + kFrameSteps);
    if (!(dilation > 0.0f) || !isfinite(dilation) || !(steps > 0.0f) || !isfinite(steps)) return 1.0f;
    const float real = steps / dilation;
    if (!isfinite(real)) return 1.0f;
    return real * 0.5f;
}

/* DOD3_CAMERA_PUSH_LOG: do the push's conditions hold as the move begins?
 * (The move is still changed before the push sees it: a near miss here can
 * be a push there, and the other way round.) */
void log_push(uint32_t pawn, uint32_t seq, float factor)
{
    static unsigned s_frames;
    static double s_next;
    const uint8_t cls = vm_read8(pawn + kClassTag);
    if (cls != 1 && cls != 2 && cls != 3 && cls != 6) return;
    const uint8_t type = vm_read8(pawn + kCameraType);
    if (type != 0 && type != 1 && type != 2 && type != 5) return;
    if (type == 5 && !(vm_read32(pawn + kCameraOfsFlags) & 0x20000000u)) return;
    if (!(vm_read32(pawn + kCollisionFlags) & 0x80000000u)) return;
    if (!(vm_read32(pawn + kCameraFlags) & 0x00010000u)) return;
    const uint32_t mesh = vm_read32(pawn + kMesh);
    if (!mesh || !seq) return;
    const float radius = guest_float(seq + kEncScale) * ((cls == 2 || cls == 3) ? 400.0f : 250.0f) *
                         guest_float(mesh + kMeshScale);
    const float dx = guest_float(pawn + kLocation) + guest_float(pawn + kDeltaMove) - guest_float(pawn + kCameraPos);
    const float dy = guest_float(pawn + kLocation + 4) + guest_float(pawn + kDeltaMove + 4) -
                     guest_float(pawn + kCameraPos + 4);
    const float d = sqrtf(dx * dx + dy * dy);
    if (d > radius) return;
    s_frames++;
    const double now = (double)clock() / CLOCKS_PER_SEC;
    if (now < s_next) return;
    s_next = now + 0.1;
    fprintf(stderr, "[camera-push] pawn %08X class %u camera %u: %.1f of %.1f units from the camera, share %.3f x %.3f "
                    "(%u frames so far)\n",
            pawn, cls, type, (double)d, (double)radius, (double)guest_float(seq + kEncAddScale), (double)factor,
            s_frames);
}

void hook_frame_move(ppu_context* ctx)
{
    const uint32_t pawn = (uint32_t)ctx->gpr[3];
    const uint32_t seq = vm_read32(pawn + kAnimSequence);
    const float factor = frame_factor(pawn);
    if (s_log) log_push(pawn, seq, factor);
    if (!s_fix || !seq || factor == 1.0f) {
        DOD3_FN_PAWN_FRAME_MOVE(ctx);
        dod3_drain(ctx);
        return;
    }
    const uint32_t share = vm_read32(seq + kEncAddScale);
    set_guest_float(seq + kEncAddScale, guest_float(seq + kEncAddScale) * factor);
    DOD3_FN_PAWN_FRAME_MOVE(ctx);
    dod3_drain(ctx);
    vm_write32(seq + kEncAddScale, share);
}

}  // namespace
#endif

extern "C" void ppu_register_function(uint64_t addr, void (*fn)(ppu_context*));

void dod3_camera_push_install()
{
#ifdef DOD3_FN_PAWN_FRAME_MOVE
    if (const char* e = getenv("DOD3_CAMERA_PUSH")) s_fix = strcmp(e, "0") != 0;
    if (const char* e = getenv("DOD3_CAMERA_PUSH_LOG")) s_log = atoi(e) != 0;
    if (!s_fix && !s_log) return;
    ppu_register_function(DOD3_A_PAWN_FRAME_MOVE, hook_frame_move);
    if (!s_fix) fprintf(stderr, "[camera-push] the push away from a boxed-in camera: the title's own (DOD3_CAMERA_PUSH=0)\n");
#endif
}
