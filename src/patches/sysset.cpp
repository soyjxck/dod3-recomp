/*
 * The engine's own graphics switches: UE3's FSystemSettings, which the title
 * fills from [SystemSettings] in PS3-Sqex03Engine.ini (inside
 * COALESCED_INT.BIN) at start-up and the renderer reads from then on.
 *
 *   GSystemSettings                       0x01A12E08 (FSystemSettings::Initialize
 *                                         is called on it at 0x008EEDD4)
 *   its FSystemSettingsData               +4 = 0x01A12E0C, the offsets below
 *   FSystemSettingsData::LoadFromIni      0x002C62C0: the {name, &field} tables
 *                                         it reads (0x019999A8 bools,
 *                                         0x01999B48 ints, 0x01999BE0 floats)
 *                                         give each key's offset
 *   FTextureLODSettings                   data +0xA4: 28 groups of 6 ints
 *                                         {MinLODMipCount, MaxLODMipCount,
 *                                         LODBias, Filter, NumStreamedMips,
 *                                         MipGenSettings}, read by 0x002C4B00
 *
 * The Advanced Graphics page (src/ui/settings_menu.cpp) offers the ones
 * that change something worth having on this renderer (dod3_sysset_rows):
 *   DOD3_SHADOWS=high|ultra  higher shadow-map resolution caps and texel
 *       density, a finer filter, shadows kept on smaller and farther objects.
 *       The engine sizes its shadow buffers once, so it takes effect at the
 *       next start. Ultra: ~7% of the frame in the chapter 1 battle (M1 Pro).
 *   DOD3_MOTION_BLUR=0       off (read by the renderer every frame: live)
 *   DOD3_POSTFX=0            the post pass off: depth of field, bloom and the
 *       colour grade together (DepthOfField gates all three; live)
 *       The title writes both itself (the title's own switches, below): in
 *       play and in cutscenes DepthOfField is set back to 1 every frame
 *       (func_00A54B58, through the setter's vtable slot 0x38C), so an Off
 *       held only until the next frame; now it is kept.
 *   DOD3_DRAW_DISTANCE=<x>   MaxDrawDistanceScale: the scene renderer
 *       multiplies every primitive's own cull distance by it, each frame
 *       (func_0061D538 squares it against MaxDrawDistanceSquared; live).
 *       Only scenery that has such a distance moves: the props, bushes and
 *       small trees of the open areas (4,000 to 30,000 units on the first
 *       chapter's coast), which otherwise appear as the player comes within
 *       it. The prologue's city has none, and looks the same at any value.
 *       The SpeedTrees have distances of their own: DOD3_TREE_LOD.
 *   DOD3_TREE_LOD=<x>        the distances at which a tree gives way to a
 *       flat picture of itself and then to nothing, times x (the tree detail
 *       section below; from the next level loaded)
 * Tried and left out: Bloom, AmbientOcclusion, LensFlares, light shafts,
 * FogVolumes, Distortion, decals, DetailMode, the LOD biases (no visible
 * change in play; the detail settings are already at their highest),
 * bAllowSeparateTranslucency (black screen) and the world texture groups'
 * LODBias 0 (the texture pool overflows: geometry renders black).
 *
 * For finding out what each switch does:
 *   DOD3_SYSSET=Name=value[,Name=value...]   applied once the INI is loaded,
 *       before the engine object exists (so before the render targets are
 *       made). Names as in the INI; TEXTUREGROUP_<group>.LODBias (or
 *       .MinLODMipCount, .MaxLODMipCount, .NumStreamedMips) for a texture group.
 *   DOD3_SYSSET_FILE=<path>   while running: each time the file changes, its
 *       lines (Name=value) are applied. TreeLOD=<x> is DOD3_TREE_LOD.
 *   DOD3_SYSSET_DUMP=1        log every value once the INI is loaded.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <atomic>
#include <string>
#include <sys/stat.h>
#include "ppu_recomp.h"
#include "cpu/eboot.h"   /* the EBOOT version's addresses */
#include "cpu/ppu.h"
#include "patches/sysset.h"
#ifdef _WIN32
#define strcasecmp _stricmp   /* the CRT's spelling */
#endif

namespace {

const uint32_t kData = DOD3_A_SYSTEM_SETTINGS_DATA;   /* GSystemSettings + 4 (1.00: 0x01A12E0C) */
const uint32_t kTexGroups = kData + 0xA4u;
const uint32_t kGEngine = DOD3_A_GENGINE;

enum Kind { B, I, F };
struct Field {
    const char* name;
    uint32_t off;
    Kind kind;
};
const Field kFields[] = {
    { "DetailMode", 0x000, I },
    { "MaxDrawDistanceScale", 0x004, F },
    { "bUseMaxQualityMode", 0x008, B },
    { "SpeedTreeLeaves", 0x00C, B },
    { "SpeedTreeFronds", 0x010, B },
    { "StaticDecals", 0x014, B },
    { "DynamicDecals", 0x018, B },
    { "UnbatchedDecals", 0x01C, B },
    { "DecalCullDistanceScale", 0x020, F },
    { "DynamicLights", 0x024, B },
    { "CompositeDynamicLights", 0x028, B },
    { "SHSecondaryLighting", 0x02C, B },
    { "DirectionalLightmaps", 0x030, B },
    { "MotionBlur", 0x034, B },
    { "MotionBlurPause", 0x038, B },
    { "DepthOfField", 0x03C, B },
    { "AmbientOcclusion", 0x040, B },
    { "Bloom", 0x044, B },
    { "bAllowLightShafts", 0x048, B },
    { "Distortion", 0x04C, B },
    { "FilteredDistortion", 0x050, B },
    { "DropParticleDistortion", 0x054, B },
    { "bAllowDownsampledTranslucency", 0x058, B },
    { "LensFlares", 0x05C, B },
    { "FogVolumes", 0x060, B },
    { "FloatingPointRenderTargets", 0x064, B },
    { "OneFrameThreadLag", 0x068, B },
    { "SkeletalMeshLODBias", 0x06C, I },
    { "ParticleLODBias", 0x070, I },
    { "AllowD3D11", 0x074, B },
    { "AllowOpenGL", 0x078, B },
    { "AllowRadialBlur", 0x07C, B },
    { "AllowSubsurfaceScattering", 0x080, B },
    { "AllowImageReflections", 0x084, B },
    { "AllowImageReflectionShadowing", 0x088, B },
    { "MotionBlurSkinning", 0x08C, I },
    { "TessellationAdaptivePixelsPerTriangle", 0x090, F },
    { "HighPrecisionGBuffers", 0x094, B },
    { "bAllowSeparateTranslucency", 0x098, B },
    { "bAllowPostprocessMLAA", 0x09C, B },
    { "bAllowHighQualityMaterials", 0x0A0, B },
    { "OnlyStreamInTextures", 0x344, B },
    { "MaxAnisotropy", 0x348, I },
    { "SceneCaptureStreamingMultiplier", 0x34C, F },
    { "UseVsync", 0x350, B },
    { "ScreenPercentage", 0x354, F },
    { "UpscaleScreenPercentage", 0x358, B },
    { "ResX", 0x35C, I },
    { "ResY", 0x360, I },
    { "Fullscreen", 0x364, B },
    { "MaxMultiSamples", 0x368, I },
    { "bAllowD3D9MSAA", 0x36C, B },
    { "bAllowTemporalAA", 0x370, B },
    { "TemporalAA_MinDepth", 0x374, F },
    { "TemporalAA_StartDepthVelocityScale", 0x378, F },
    { "DynamicShadows", 0x37C, B },
    { "LightEnvironmentShadows", 0x380, B },
    { "ShadowFilterQualityBias", 0x384, I },
    { "MinShadowResolution", 0x388, I },
    { "MinPreShadowResolution", 0x38C, I },
    { "MaxShadowResolution", 0x390, I },
    { "MaxWholeSceneDominantShadowResolution", 0x394, I },
    { "ShadowTexelsPerPixel", 0x398, F },
    { "PreShadowResolutionFactor", 0x39C, F },
    { "bEnableBranchingPCFShadows", 0x3A0, B },
    { "bAllowHardwareShadowFiltering", 0x3A4, B },
    { "bEnableForegroundShadowsOnWorld", 0x3A8, B },
    { "bEnableForegroundSelfShadowing", 0x3AC, B },
    { "bAllowWholeSceneDominantShadows", 0x3B0, B },
    { "bUseConservativeShadowBounds", 0x3B4, B },
    { "ShadowFilterRadius", 0x3B8, F },
    { "ShadowDepthBias", 0x3BC, F },
    { "PerObjectShadowTransition", 0x3C0, F },
    { "PerSceneShadowTransition", 0x3C4, F },
    { "CSMSplitPenumbraScale", 0x3C8, F },
    { "CSMSplitSoftTransitionDistanceScale", 0x3CC, F },
    { "CSMSplitDepthBiasScale", 0x3D0, F },
    { "CSMMinimumFOV", 0x3D4, F },
    { "CSMFOVRoundFactor", 0x3D8, F },
    { "UnbuiltWholeSceneDynamicShadowRadius", 0x3DC, F },
    { "UnbuiltNumWholeSceneDynamicShadowCascades", 0x3E0, I },
    { "WholeSceneShadowUnbuiltInteractionThreshold", 0x3E4, I },
    { "ShadowFadeResolution", 0x3E8, I },
    { "PreShadowFadeResolution", 0x3EC, I },
    { "ShadowFadeExponent", 0x3F0, F },
    { "bAllowFracturedDamage", 0x3F4, B },
    { "NumFracturedPartsScale", 0x3F8, F },
    { "FractureDirectSpawnChanceScale", 0x3FC, F },
    { "FractureRadialSpawnChanceScale", 0x400, F },
    { "FractureCullDistanceScale", 0x404, F },
    { "bForceCPUAccessToGPUSkinVerts", 0x408, B },
    { "bDisableSkeletalInstanceWeights", 0x40C, B },
    { "AllowSecondaryDisplays", 0x410, B },
    { "SecondaryDisplayMaximumWidth", 0x414, I },
    { "SecondaryDisplayMaximumHeight", 0x418, I },
};

const char* const kGroups[] = {
    "World",
    "WorldNormalMap",
    "WorldSpecular",
    "Character",
    "CharacterNormalMap",
    "CharacterSpecular",
    "Weapon",
    "WeaponNormalMap",
    "WeaponSpecular",
    "Vehicle",
    "VehicleNormalMap",
    "VehicleSpecular",
    "Cinematic",
    "Effects",
    "EffectsNotFiltered",
    "Skybox",
    "UI",
    "Lightmap",
    "RenderTarget",
    "MobileFlattened",
    "ProcBuilding_Face",
    "ProcBuilding_LightMap",
    "Shadowmap",
    "ColorLookupTable",
    "Terrain_Heightmap",
    "Terrain_Weightmap",
    "ImageBasedReflection",
    "Bokeh",
};
const char* const kGroupFields[] = { "MinLODMipCount", "MaxLODMipCount",  "LODBias",
                                     "Filter",         "NumStreamedMips", "MipGenSettings" };

std::string show(const Field& f)
{
    char b[48];
    const uint32_t v = vm_read32(kData + f.off);
    if (f.kind == F) {
        float x;
        memcpy(&x, &v, 4);
        snprintf(b, sizeof b, "%g", x);
    } else if (f.kind == B)
        snprintf(b, sizeof b, "%s", v ? "True" : "False");
    else
        snprintf(b, sizeof b, "%d", (int)v);
    return b;
}

void set_tree_lod(const char* v);

/* "Name=value": true if applied. */
bool apply_one(const std::string& item)
{
    const size_t eq = item.find('=');
    if (eq == std::string::npos) return false;
    std::string name = item.substr(0, eq), val = item.substr(eq + 1);
    while (!name.empty() && name.back() == ' ') name.pop_back();
    while (!val.empty() && val.front() == ' ') val.erase(0, 1);
    while (!val.empty() && (val.back() == ' ' || val.back() == '\r' || val.back() == '\n')) val.pop_back();
    if (!strcasecmp(name.c_str(), "TreeLOD")) {
        set_tree_lod(val.c_str());
        return true;
    }
    if (!strncmp(name.c_str(), "TEXTUREGROUP_", 13)) {
        const size_t dot = name.find('.');
        if (dot == std::string::npos) return false;
        const std::string g = name.substr(13, dot - 13), fld = name.substr(dot + 1);
        for (uint32_t gi = 0; gi < sizeof kGroups / sizeof *kGroups; gi++) {
            if (strcasecmp(g.c_str(), kGroups[gi])) continue;
            for (uint32_t fi = 0; fi < 6; fi++)
                if (!strcasecmp(fld.c_str(), kGroupFields[fi])) {
                    const uint32_t a = kTexGroups + gi * 24u + fi * 4u;
                    const int old = (int)vm_read32(a);
                    vm_write32(a, (uint32_t)atoi(val.c_str()));
                    fprintf(stderr, "[sysset] %s: %d -> %d\n", name.c_str(), old, (int)vm_read32(a));
                    return true;
                }
        }
        return false;
    }
    for (const Field& f : kFields) {
        if (strcasecmp(name.c_str(), f.name)) continue;
        const std::string old = show(f);
        uint32_t v;
        if (f.kind == F) {
            const float x = (float)atof(val.c_str());
            memcpy(&v, &x, 4);
        } else if (f.kind == B)
            v = (!strcasecmp(val.c_str(), "true") || atoi(val.c_str()) != 0) ? 1u : 0u;
        else
            v = (uint32_t)atoi(val.c_str());
        vm_write32(kData + f.off, v);
        fprintf(stderr, "[sysset] %s: %s -> %s\n", f.name, old.c_str(), show(f).c_str());
        return true;
    }
    return false;
}

void apply_list(const std::string& all, char sep)
{
    size_t p = 0;
    while (p < all.size()) {
        size_t q = all.find(sep, p);
        if (q == std::string::npos) q = all.size();
        const std::string item = all.substr(p, q - p);
        p = q + 1;
        if (item.find_first_not_of(" \t\r\n") == std::string::npos || item[0] == '#') continue;
        if (!apply_one(item)) fprintf(stderr, "[sysset] '%s' not applied\n", item.c_str());
    }
}

void dump()
{
    for (const Field& f : kFields) fprintf(stderr, "[sysset] %-44s %s\n", f.name, show(f).c_str());
    for (uint32_t gi = 0; gi < sizeof kGroups / sizeof *kGroups; gi++) {
        const uint32_t a = kTexGroups + gi * 24u;
        fprintf(stderr, "[sysset] TEXTUREGROUP_%-28s Min %d Max %d LODBias %d Filter %d Streamed %d MipGen %d\n",
                kGroups[gi], (int)vm_read32(a), (int)vm_read32(a + 4), (int)vm_read32(a + 8), (int)vm_read32(a + 12),
                (int)vm_read32(a + 16), (int)vm_read32(a + 20));
    }
}

/* ---- the Advanced Graphics page ------------------------------------------ */

const char* const kShadows[][2] = {
    { "high", "MaxShadowResolution=1536,MaxWholeSceneDominantShadowResolution=2048,ShadowTexelsPerPixel=2,"
              "MinShadowResolution=48,ShadowFadeResolution=64,PreShadowFadeResolution=12" },
    { "ultra", "MaxShadowResolution=2048,MaxWholeSceneDominantShadowResolution=2048,ShadowTexelsPerPixel=2.5,"
               "ShadowFilterQualityBias=1,MinShadowResolution=32,ShadowFadeResolution=32,PreShadowFadeResolution=8" },
};

bool s_loaded = false;   /* the INI is in: writes stick */

bool off(const char* v) { return v && v[0] == '0'; }
std::atomic<bool> s_blur_off{ false }, s_dof_off{ false };   /* the player's Off, kept (below) */
void set_motion_blur(const char* v)
{
    s_blur_off = off(v);
    if (s_loaded) apply_one(std::string("MotionBlur=") + (off(v) ? "0" : "1"));
}
void set_postfx(const char* v)
{
    s_dof_off = off(v);
    if (s_loaded) apply_one(std::string("DepthOfField=") + (off(v) ? "0" : "1"));
}

/* The title's own switches: it sets and flips MotionBlur and DepthOfField
 * itself, through four small functions it reaches by pointer (two set the
 * switch to r4, two flip it). Seen in chapter 1's play and cutscenes:
 * DepthOfField set to 1 every frame, MotionBlur set to 0 once (func_00920D70).
 * Each runs as the title's, and a switch the player has turned off is put
 * back to off after it. DOD3_SYSSET_LOG=1 says when the title does it. */
#ifdef DOD3_FN_SET_MOTION_BLUR
void keep_off(ppu_context* ctx, void (*lifted)(ppu_context*), uint32_t field, bool hold, const char* what)
{
    const uint32_t lr = (uint32_t)ctx->lr, was = vm_read32(kData + field), arg = (uint32_t)ctx->gpr[4];
    lifted(ctx);
    dod3_drain(ctx);
    const uint32_t now = vm_read32(kData + field);
    if (hold && now) vm_write32(kData + field, 0);
    static int log = -1;
    if (log < 0) log = getenv("DOD3_SYSSET_LOG") ? 1 : 0;
    if (log)
        fprintf(stderr, "[sysset] the title set %s %u -> %u (r4 %u, from %08X)%s\n", what, was, now, arg, lr,
                hold && now ? ": kept off" : "");
}
void hook_set_motion_blur(ppu_context* ctx) { keep_off(ctx, DOD3_FN_SET_MOTION_BLUR, 0x034, s_blur_off, "MotionBlur"); }
void hook_flip_motion_blur(ppu_context* ctx) { keep_off(ctx, DOD3_FN_FLIP_MOTION_BLUR, 0x034, s_blur_off, "MotionBlur"); }
void hook_set_dof(ppu_context* ctx) { keep_off(ctx, DOD3_FN_SET_DEPTH_OF_FIELD, 0x03C, s_dof_off, "DepthOfField"); }
void hook_flip_dof(ppu_context* ctx) { keep_off(ctx, DOD3_FN_FLIP_DEPTH_OF_FIELD, 0x03C, s_dof_off, "DepthOfField"); }
#endif

void set_draw_distance(const char* v)
{
    const double x = v ? atof(v) : 1.0;
    char b[48];
    snprintf(b, sizeof b, "MaxDrawDistanceScale=%g", x >= 1.0 && x <= 100.0 ? x : 1.0);
    if (s_loaded) apply_one(b);
}

/* ---- tree detail -----------------------------------------------------------
 * DOD3_TREE_LOD=<x>. A SpeedTree is drawn as its model up to a distance,
 * crosses over to a flat picture of itself (its billboard) across a stretch,
 * is the billboard from there on, and past the last distance is not drawn
 * at all: the trees of the open areas appear, and then change, as the player
 * walks up to them. The distances are each component's own
 * (USpeedTreeComponent +0x1E0: Lod3DStart, Lod3DEnd, LodBillboardStart,
 * LodBillboardEnd -- 500, 6,000, 8,000 and 12,000 to 16,000 units for most
 * trees of the first chapter's coast), copied into its scene proxy when that
 * is made (FSpeedTreeSceneProxy's constructor: proxy +0x160). The proxy's are
 * what the renderer reads, once: when the tree is added to the scene, its
 * meshes go into the draw lists with the distances between which each is
 * drawn (FSpeedTreeSceneProxy::DrawStaticElements). The scene's own
 * MaxDrawDistanceScale (DOD3_DRAW_DISTANCE) does not reach them.
 *
 * So the proxy's four are multiplied by x as it is made:
 * USpeedTreeComponent::CreateSceneProxy is a virtual and the constructor's
 * only caller, and is replaced in the function registry the call resolves
 * through. The components keep the title's values. A tree that is in the
 * scene already keeps the distances it was added with -- writing its proxy
 * afterwards changes nothing on screen -- so a new x is seen from the next
 * level, or part of a level, that is loaded.
 *
 * The first chapter's coast from its landing stage, the same view in three
 * runs: at 0.2 the woods on either side are gone; at 1 they stand, the pines
 * on the cliff as their soft billboards; at 3 those are their models, branch
 * by branch, and a tree past the far bank that was not drawn is. */
#ifdef DOD3_FN_TREE_CREATE_PROXY
std::atomic<float> s_tree_lod{ 1.0f };   /* DOD3_TREE_LOD */

void hook_tree_create_proxy(ppu_context* ctx)
{
    const uint32_t comp = (uint32_t)ctx->gpr[3];
    DOD3_FN_TREE_CREATE_PROXY(ctx);
    dod3_drain(ctx);
    const uint32_t proxy = (uint32_t)ctx->gpr[3];
    const float k = s_tree_lod.load(std::memory_order_relaxed);
    if (k == 1.0f || !proxy || vm_read32(proxy) != DOD3_A_TREE_PROXY_VTABLE || vm_read32(proxy + 0x98u) != comp) return;
    float d[4];
    for (uint32_t i = 0; i < 4; i++) {
        const uint32_t w = vm_read32(proxy + 0x160u + 4u * i);
        memcpy(&d[i], &w, 4);
        if (!(d[i] >= 1.0f && d[i] <= 1.0e6f)) return;
    }
    for (uint32_t i = 0; i < 4; i++) {
        const float x = d[i] * k;
        uint32_t w;
        memcpy(&w, &x, 4);
        vm_write32(proxy + 0x160u + 4u * i, w);
    }
}
#endif

void set_tree_lod(const char* v)
{
#ifdef DOD3_FN_TREE_CREATE_PROXY
    const double x = v ? atof(v) : 1.0;
    const float k = x >= 0.1 && x <= 20.0 ? (float)x : 1.0f;
    if (k != s_tree_lod.exchange(k, std::memory_order_relaxed))
        fprintf(stderr, "[sysset] tree detail distance x%g, for the trees loaded from here on\n", k);
#else
    (void)v;
#endif
}

void apply_player_settings()
{
    if (const char* v = getenv("DOD3_SHADOWS"))
        for (const auto& p : kShadows)
            if (!strcmp(v, p[0])) apply_list(p[1], ',');
    if (const char* v = getenv("DOD3_MOTION_BLUR")) set_motion_blur(v);
    if (const char* v = getenv("DOD3_POSTFX")) set_postfx(v);
    if (const char* v = getenv("DOD3_DRAW_DISTANCE")) set_draw_distance(v);
    if (const char* v = getenv("DOD3_TREE_LOD")) set_tree_lod(v);
}

}  // namespace

void dod3_sysset_rows(std::vector<Dod3RowSpec>& rows)
{
    rows.push_back({ "DOD3_SHADOWS",
                     "Shadow Quality",
                     "Sharper, more defined shadows.",
                     { { nullptr, "Original" }, { "high", "High" }, { "ultra", "Ultra" } },
                     0,
                     false,
                     nullptr });
    rows.push_back({ "DOD3_MOTION_BLUR",
                     "Motion Blur",
                     "Blur on fast movement of the camera and characters.",
                     { { nullptr, "On" }, { "0", "Off" } },
                     0,
                     true,
                     set_motion_blur });
    rows.push_back({ "DOD3_POSTFX",
                     "Post-Processing",
                     "Depth of field, bloom and the game's colour grading.",
                     { { nullptr, "On" }, { "0", "Off" } },
                     0,
                     true,
                     set_postfx });
    rows.push_back({ "DOD3_DRAW_DISTANCE",
                     "Draw Distance",
                     "How far away props, plants and other scenery are drawn.",
                     { { nullptr, "Original" }, { "2", "2x" }, { "3", "3x" }, { "5", "5x" }, { "10", "10x" } },
                     0,
                     true,
                     set_draw_distance });
#ifdef DOD3_FN_TREE_CREATE_PROXY
    rows.push_back({ "DOD3_TREE_LOD",
                     "Tree Detail",
                     "How far trees show and stay detailed. From the next area.",
                     { { nullptr, "Original" }, { "2", "2x" }, { "3", "3x" }, { "5", "5x" }, { "10", "10x" } },
                     0,
                     true,
                     set_tree_lod });
#endif
}

extern "C" void ppu_register_function(uint64_t addr, void (*fn)(ppu_context*));

void dod3_sysset_install()
{
#ifdef DOD3_FN_TREE_CREATE_PROXY
    ppu_register_function(DOD3_A_TREE_CREATE_PROXY, hook_tree_create_proxy);
#endif
#ifdef DOD3_FN_SET_MOTION_BLUR
    ppu_register_function(DOD3_A_SET_MOTION_BLUR, hook_set_motion_blur);
    ppu_register_function(DOD3_A_FLIP_MOTION_BLUR, hook_flip_motion_blur);
    ppu_register_function(DOD3_A_SET_DEPTH_OF_FIELD, hook_set_dof);
    ppu_register_function(DOD3_A_FLIP_DEPTH_OF_FIELD, hook_flip_dof);
#endif
}

/* From the vblank loop (main.cpp), often. */
void dod3_sysset_poll()
{
    static int state = 0;           /* 0 waiting for the INI, 1 applied */
    static const char* file = getenv("DOD3_SYSSET_FILE");
    if (state == 0) {
        /* The INI is in once MaxShadowResolution is (the struct is zero
         * before); the engine object, which makes the render targets from
         * these values, comes later. */
        if (!vm_read32(kData + 0x390u)) return;
        static int settle = 0;
        if (settle < 50 && ++settle) return;  /* ~0.1 s for LoadFromIni to finish the floats and texture groups */
        state = 1;
        s_loaded = true;
        if (getenv("DOD3_SYSSET_DUMP")) dump();
        apply_player_settings();
        if (const char* e = getenv("DOD3_SYSSET")) apply_list(e, ',');
        if (vm_read32(kGEngine)) fprintf(stderr, "[sysset] note: applied after the engine object was made\n");
    }
    if (file && *file) {
        static long long last = 0;
        static unsigned tick = 0;
        if ((tick++ & 63) != 0) return;
        struct stat st;
        if (stat(file, &st) != 0) return;
        const long long m = (long long)st.st_mtime * 1000000000ll +
#ifdef __APPLE__
                            st.st_mtimespec.tv_nsec;
#else
                            0;
#endif
        if (m == last) return;
        last = m;
        FILE* fp = fopen(file, "rb");
        if (!fp) return;
        std::string all;
        char buf[4096];
        size_t n;
        while ((n = fread(buf, 1, sizeof buf, fp)) > 0) all.append(buf, n);
        fclose(fp);
        apply_list(all, '\n');
        fprintf(stderr, "[sysset] file applied\n");
    }
}
