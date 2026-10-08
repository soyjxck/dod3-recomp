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
 * The Advanced Graphics page (src/dod3_settings_menu.cpp) offers the ones
 * that change something worth having on this renderer (dod3_sysset_rows):
 *   DOD3_SHADOWS=high|ultra  higher shadow-map resolution caps and texel
 *       density, a finer filter, shadows kept on smaller and farther objects.
 *       The engine sizes its shadow buffers once, so it takes effect at the
 *       next start. Ultra: ~7% of the frame in the chapter 1 battle (M1 Pro).
 *   DOD3_MOTION_BLUR=0       off (read by the renderer every frame: live)
 *   DOD3_POSTFX=0            the post pass off: depth of field, bloom and the
 *       colour grade together (DepthOfField gates all three; live)
 * Tried and left out: Bloom, AmbientOcclusion, LensFlares, light shafts,
 * FogVolumes, Distortion, decals, DetailMode, MaxDrawDistanceScale, the LOD
 * biases (no visible change in play; the detail settings are already at
 * their highest), bAllowSeparateTranslucency (black screen) and the world
 * texture groups' LODBias 0 (the texture pool overflows: geometry renders
 * black).
 *
 * For finding out what each switch does:
 *   DOD3_SYSSET=Name=value[,Name=value...]   applied once the INI is loaded,
 *       before the engine object exists (so before the render targets are
 *       made). Names as in the INI; TEXTUREGROUP_<group>.LODBias (or
 *       .MinLODMipCount, .MaxLODMipCount, .NumStreamedMips) for a texture group.
 *   DOD3_SYSSET_FILE=<path>   while running: each time the file changes, its
 *       lines (Name=value) are applied.
 *   DOD3_SYSSET_DUMP=1        log every value once the INI is loaded.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <string>
#include <sys/stat.h>
#include "ppu_recomp.h"
#include "dod3_sysset.h"

namespace {

const uint32_t kData = 0x01A12E0Cu;         /* GSystemSettings + 4 */
const uint32_t kTexGroups = kData + 0xA4u;
const uint32_t kGEngine = 0x01999164u;

enum Kind { B, I, F };
struct Field { const char* name; uint32_t off; Kind kind; };
const Field kFields[] = {
    {"DetailMode", 0x000, I},
    {"MaxDrawDistanceScale", 0x004, F},
    {"bUseMaxQualityMode", 0x008, B},
    {"SpeedTreeLeaves", 0x00C, B},
    {"SpeedTreeFronds", 0x010, B},
    {"StaticDecals", 0x014, B},
    {"DynamicDecals", 0x018, B},
    {"UnbatchedDecals", 0x01C, B},
    {"DecalCullDistanceScale", 0x020, F},
    {"DynamicLights", 0x024, B},
    {"CompositeDynamicLights", 0x028, B},
    {"SHSecondaryLighting", 0x02C, B},
    {"DirectionalLightmaps", 0x030, B},
    {"MotionBlur", 0x034, B},
    {"MotionBlurPause", 0x038, B},
    {"DepthOfField", 0x03C, B},
    {"AmbientOcclusion", 0x040, B},
    {"Bloom", 0x044, B},
    {"bAllowLightShafts", 0x048, B},
    {"Distortion", 0x04C, B},
    {"FilteredDistortion", 0x050, B},
    {"DropParticleDistortion", 0x054, B},
    {"bAllowDownsampledTranslucency", 0x058, B},
    {"LensFlares", 0x05C, B},
    {"FogVolumes", 0x060, B},
    {"FloatingPointRenderTargets", 0x064, B},
    {"OneFrameThreadLag", 0x068, B},
    {"SkeletalMeshLODBias", 0x06C, I},
    {"ParticleLODBias", 0x070, I},
    {"AllowD3D11", 0x074, B},
    {"AllowOpenGL", 0x078, B},
    {"AllowRadialBlur", 0x07C, B},
    {"AllowSubsurfaceScattering", 0x080, B},
    {"AllowImageReflections", 0x084, B},
    {"AllowImageReflectionShadowing", 0x088, B},
    {"MotionBlurSkinning", 0x08C, I},
    {"TessellationAdaptivePixelsPerTriangle", 0x090, F},
    {"HighPrecisionGBuffers", 0x094, B},
    {"bAllowSeparateTranslucency", 0x098, B},
    {"bAllowPostprocessMLAA", 0x09C, B},
    {"bAllowHighQualityMaterials", 0x0A0, B},
    {"OnlyStreamInTextures", 0x344, B},
    {"MaxAnisotropy", 0x348, I},
    {"SceneCaptureStreamingMultiplier", 0x34C, F},
    {"UseVsync", 0x350, B},
    {"ScreenPercentage", 0x354, F},
    {"UpscaleScreenPercentage", 0x358, B},
    {"ResX", 0x35C, I},
    {"ResY", 0x360, I},
    {"Fullscreen", 0x364, B},
    {"MaxMultiSamples", 0x368, I},
    {"bAllowD3D9MSAA", 0x36C, B},
    {"bAllowTemporalAA", 0x370, B},
    {"TemporalAA_MinDepth", 0x374, F},
    {"TemporalAA_StartDepthVelocityScale", 0x378, F},
    {"DynamicShadows", 0x37C, B},
    {"LightEnvironmentShadows", 0x380, B},
    {"ShadowFilterQualityBias", 0x384, I},
    {"MinShadowResolution", 0x388, I},
    {"MinPreShadowResolution", 0x38C, I},
    {"MaxShadowResolution", 0x390, I},
    {"MaxWholeSceneDominantShadowResolution", 0x394, I},
    {"ShadowTexelsPerPixel", 0x398, F},
    {"PreShadowResolutionFactor", 0x39C, F},
    {"bEnableBranchingPCFShadows", 0x3A0, B},
    {"bAllowHardwareShadowFiltering", 0x3A4, B},
    {"bEnableForegroundShadowsOnWorld", 0x3A8, B},
    {"bEnableForegroundSelfShadowing", 0x3AC, B},
    {"bAllowWholeSceneDominantShadows", 0x3B0, B},
    {"bUseConservativeShadowBounds", 0x3B4, B},
    {"ShadowFilterRadius", 0x3B8, F},
    {"ShadowDepthBias", 0x3BC, F},
    {"PerObjectShadowTransition", 0x3C0, F},
    {"PerSceneShadowTransition", 0x3C4, F},
    {"CSMSplitPenumbraScale", 0x3C8, F},
    {"CSMSplitSoftTransitionDistanceScale", 0x3CC, F},
    {"CSMSplitDepthBiasScale", 0x3D0, F},
    {"CSMMinimumFOV", 0x3D4, F},
    {"CSMFOVRoundFactor", 0x3D8, F},
    {"UnbuiltWholeSceneDynamicShadowRadius", 0x3DC, F},
    {"UnbuiltNumWholeSceneDynamicShadowCascades", 0x3E0, I},
    {"WholeSceneShadowUnbuiltInteractionThreshold", 0x3E4, I},
    {"ShadowFadeResolution", 0x3E8, I},
    {"PreShadowFadeResolution", 0x3EC, I},
    {"ShadowFadeExponent", 0x3F0, F},
    {"bAllowFracturedDamage", 0x3F4, B},
    {"NumFracturedPartsScale", 0x3F8, F},
    {"FractureDirectSpawnChanceScale", 0x3FC, F},
    {"FractureRadialSpawnChanceScale", 0x400, F},
    {"FractureCullDistanceScale", 0x404, F},
    {"bForceCPUAccessToGPUSkinVerts", 0x408, B},
    {"bDisableSkeletalInstanceWeights", 0x40C, B},
    {"AllowSecondaryDisplays", 0x410, B},
    {"SecondaryDisplayMaximumWidth", 0x414, I},
    {"SecondaryDisplayMaximumHeight", 0x418, I},
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
const char* const kGroupFields[] = {"MinLODMipCount", "MaxLODMipCount", "LODBias", "Filter", "NumStreamedMips",
                                    "MipGenSettings"};

std::string show(const Field& f)
{
    char b[48];
    const uint32_t v = vm_read32(kData + f.off);
    if (f.kind == F) { float x; memcpy(&x, &v, 4); snprintf(b, sizeof b, "%g", x); }
    else if (f.kind == B) snprintf(b, sizeof b, "%s", v ? "True" : "False");
    else snprintf(b, sizeof b, "%d", (int)v);
    return b;
}

/* "Name=value": true if applied. */
bool apply_one(const std::string& item)
{
    const size_t eq = item.find('=');
    if (eq == std::string::npos) return false;
    std::string name = item.substr(0, eq), val = item.substr(eq + 1);
    while (!name.empty() && name.back() == ' ') name.pop_back();
    while (!val.empty() && val.front() == ' ') val.erase(0, 1);
    while (!val.empty() && (val.back() == ' ' || val.back() == '\r' || val.back() == '\n')) val.pop_back();
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
        if (f.kind == F) { const float x = (float)atof(val.c_str()); memcpy(&v, &x, 4); }
        else if (f.kind == B) v = (!strcasecmp(val.c_str(), "true") || atoi(val.c_str()) != 0) ? 1u : 0u;
        else v = (uint32_t)atoi(val.c_str());
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
                kGroups[gi], (int)vm_read32(a), (int)vm_read32(a + 4), (int)vm_read32(a + 8),
                (int)vm_read32(a + 12), (int)vm_read32(a + 16), (int)vm_read32(a + 20));
    }
}

/* ---- the Advanced Graphics page ------------------------------------------ */

const char* const kShadows[][2] = {
    {"high",  "MaxShadowResolution=1536,MaxWholeSceneDominantShadowResolution=2048,ShadowTexelsPerPixel=2,"
              "MinShadowResolution=48,ShadowFadeResolution=64,PreShadowFadeResolution=12"},
    {"ultra", "MaxShadowResolution=2048,MaxWholeSceneDominantShadowResolution=2048,ShadowTexelsPerPixel=2.5,"
              "ShadowFilterQualityBias=1,MinShadowResolution=32,ShadowFadeResolution=32,PreShadowFadeResolution=8"},
};

bool s_loaded = false;   /* the INI is in: writes stick */

bool off(const char* v) { return v && v[0] == '0'; }
void set_motion_blur(const char* v) { if (s_loaded) apply_one(std::string("MotionBlur=") + (off(v) ? "0" : "1")); }
void set_postfx(const char* v) { if (s_loaded) apply_one(std::string("DepthOfField=") + (off(v) ? "0" : "1")); }

void apply_player_settings()
{
    if (const char* v = getenv("DOD3_SHADOWS"))
        for (const auto& p : kShadows)
            if (!strcmp(v, p[0])) apply_list(p[1], ',');
    if (const char* v = getenv("DOD3_MOTION_BLUR")) set_motion_blur(v);
    if (const char* v = getenv("DOD3_POSTFX")) set_postfx(v);
}

}  // namespace

void dod3_sysset_rows(std::vector<Dod3RowSpec>& rows)
{
    rows.push_back({"DOD3_SHADOWS", "Shadow Quality", "Shadow sharpness. Applies the next time the game starts.",
                    {{nullptr, "Original"}, {"high", "High"}, {"ultra", "Ultra"}}, 0, false, nullptr});
    rows.push_back({"DOD3_MOTION_BLUR", "Motion Blur", "Blur on fast movement of the camera and characters.",
                    {{nullptr, "On"}, {"0", "Off"}}, 0, true, set_motion_blur});
    rows.push_back({"DOD3_POSTFX", "Post-Processing", "Depth of field, bloom and the game's colour grading.",
                    {{nullptr, "On"}, {"0", "Off"}}, 0, true, set_postfx});
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
        if (settle++ < 50) return;  /* ~0.1 s for LoadFromIni to finish the floats and texture groups */
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
