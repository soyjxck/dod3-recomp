#!/usr/bin/env python3
"""Write shaders/override/a3e8d0b0476b7c09.hlsl, our own version of Drakengard 3's depth-of-field and bloom
composite (the engine's UberPostProcessEffect, fragment program fp-struct a3e8d0b0476b7c09), from the renderer's
translation of that program.

  python tools/make_postfx_override.py <translation.hlsl>

The translation: run the game (or rsx_replay on a capture of play) with PS3RECOMP_SHADER_DUMP=<folder> and
RSX_PIPE_LOG=1; the log's pipeline line with fp-struct=a3e8d0b0476b7c09 names its fp-hlsl=<hash>, and the file is
<folder>/fp_<hash>.hlsl. Run this again whenever the decompiler's output changes: its bindings are what the
override is written against.

What it changes, where src/main.cpp gives the renderer shaders/override (RSX_FP_OVERRIDE_DIR):
  - POSTFX_HIGH (Post-Processing Quality, High): above 1280x720, the quarter-size blur and bloom are read with a
    cubic B-spline instead of one bilinear tap stretched over three times the pixels, and the full-size picture is
    softened as far as a pixel is out of focus (at 4K a half-defocused tree was sharp detail laid over the blur).
    At 1280x720 the translation as it is.
  - Always: the renderer's HDR highlight plane at the second target (rsx_hdr_plane): what went to the frame and
    how many times brighter the pixel was before the title clipped it.
"""
import os
import sys

if len(sys.argv) != 2:
    sys.exit(__doc__)
SRC = sys.argv[1]
OUT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "shaders", "override", "a3e8d0b0476b7c09.hlsl")


def once(old, new):
    global s
    assert s.count(old) == 1, "the translation is not in the form this script knows: " + old[:60]
    s = s.replace(old, new)


s = open(SRC, encoding="utf-8").read().replace("\r\n", "\n")


once("""PSOutput main(PSInput input) {""", """/* A texture read as a uniform cubic B-spline over its texels: four bilinear
 * taps (the sampler filters linearly) weighted to the spline. Smooth where
 * a quarter-size target is stretched over a picture three times the
 * original's, where one bilinear tap shows its texels as soft squares. */
float4 bspline(Texture2D t, SamplerState s, float2 uv)
{
    uint w, h;
    t.GetDimensions(w, h);
    const float2 size = float2(w, h);
    const float2 p = uv * size - 0.5;
    const float2 c = floor(p);
    const float2 f = p - c, f2 = f * f, f3 = f2 * f;
    const float2 w0 = (-f3 + 3.0 * f2 - 3.0 * f + 1.0) / 6.0;
    const float2 w1 = (3.0 * f3 - 6.0 * f2 + 4.0) / 6.0;
    const float2 w2 = (-3.0 * f3 + 3.0 * f2 + 3.0 * f + 1.0) / 6.0;
    const float2 w3 = f3 / 6.0;
    const float2 g0 = w0 + w1, g1 = w2 + w3;
    const float2 t0 = (c - 0.5 + w1 / g0) / size, t1 = (c + 1.5 + w3 / g1) / size;
    return g0.y * (g0.x * t.Sample(s, float2(t0.x, t0.y)) + g1.x * t.Sample(s, float2(t1.x, t0.y)))
         + g1.y * (g0.x * t.Sample(s, float2(t0.x, t1.y)) + g1.x * t.Sample(s, float2(t1.x, t1.y)));
}

PSOutput main(PSInput input) {""")
once("""    float4 cc0 = (float4)0; float4 cc1 = (float4)0;
""", """    float4 cc0 = (float4)0; float4 cc1 = (float4)0;
    uint tw, th;
    rsx_tex[0].GetDimensions(tw, th);
    const float k = (float)tw / 1280.0;   /* internal pixels per original pixel */
#ifdef POSTFX_HIGH
    const bool hi = k > 1.01;              /* at 1280x720, the translation as it is */
#else
    const bool hi = false;                 /* Post-Processing Quality: Original */
#endif
""")
once("""    { float4 _v = (float4)(rsx_tex[1].Sample(rsx_samp[1], ((r[0]).xyzw).xy)); r[0].xyzw = _v.xyzw; }
""", """    { float4 _v = hi ? bspline(rsx_tex[1], rsx_samp[1], r[0].xy) : rsx_tex[1].Sample(rsx_samp[1], ((r[0]).xyzw).xy); r[0].xyzw = _v.xyzw; }
""")
once("""    { float4 _v = (float4)(rsx_tex[2].Sample(rsx_samp[2], ((input.tc0).zwzz).xy)); r[1].xyz = _v.xyz; }
""", """    { float4 _v = hi ? bspline(rsx_tex[2], rsx_samp[2], input.tc0.zw) : rsx_tex[2].Sample(rsx_samp[2], ((input.tc0).zwzz).xy); r[1].xyz = _v.xyz; }
""")
once("""    float4 _o1 = r[2];
    _out.t1 = (_o1 == _o1) ? _o1 : (float4)0;
""", """    /* The HDR highlight plane (rsx_hdr_plane: the renderer binds it here, as
     * the second target, while HDR is on; otherwise nothing is). rgb: what
     * went to the frame; a: how many times brighter the pixel was, in linear
     * light, before the clip -- the scene and its bloom (h[2]) through the
     * title's own encode (its exponents, fp_constants 7, 9 and 8 for red,
     * green and blue) above 1, then as a display decodes it (2.2). */
    {
        const float3 e = pow(max(h[2].xyz, 1e-6), float3(fp_constants[7].x, fp_constants[9].x, fp_constants[8].x));
        const float over = max(max(e.x, e.y), max(e.z, 1.0));
        const float4 plane = float4(saturate(r[0].xyz), min(pow(over, 2.2), 64.0));
        _out.t1 = (plane == plane) ? plane : float4(saturate(r[0].xyz), 1.0);
    }
""")
once("""    { float4 _v = (float4)(rsx_tex[0].SampleLevel(rsx_samp[0], ((input.tc1).xyzw).xy, ((fp_constants[3]).xxxx).x)); r[1].xyz = _v.xyz; }
""", """    {
        /* The full-size picture the blur is blended with (r[0].w: how much of
         * it the game keeps here, 1 in focus). At 1280x720 the picture is as
         * soft as its pixels; at a higher internal resolution it is sharp, and
         * a pixel half out of focus came out as sharp detail laid over the
         * blur. Where the pixel is out of focus at all, the picture is
         * averaged over about one and a half of the original's pixels -- what
         * the original showed there -- and stays sharp where it is in focus. */
        const float2 uv = input.tc1.xy;
        const float lod = fp_constants[3].x;
        float3 sharp = rsx_tex[0].SampleLevel(rsx_samp[0], uv, lod).xyz;
        const float soften = smoothstep(0.0, 0.3, 1.0 - r[0].w);
        if (hi && soften > 0.0) {
            const float span = k * 1.5;
            const int n = clamp((int)ceil(span - 0.01), 2, 8);
            const float2 step = span / float2(tw, th);
            float3 acc = (float3)0;
            [loop] for (int y = 0; y < n; y++)
                [loop] for (int x = 0; x < n; x++)
                    acc += rsx_tex[0].SampleLevel(rsx_samp[0], uv + (float2(x + 0.5, y + 0.5) / n - 0.5) * step, lod).xyz;
            sharp = lerp(sharp, acc / (float)(n * n), soften);
        }
        r[1].xyz = sharp;
    }
""")
head = """/* Drakengard 3's depth-of-field and bloom composite (the engine's
 * UberPostProcessEffect; fragment program fp-struct a3e8d0b0476b7c09), in
 * place of its translation when the renderer is given this folder
 * (RSX_FP_OVERRIDE_DIR; src/main.cpp). Made from the translation by
 * tools/make_postfx_override.py: run it again when the decompiler's output
 * changes.
 *   POSTFX_HIGH (Post-Processing Quality, High; RSX_FP_OVERRIDE_DEFINES):
 *     above 1280x720 the quarter-size blur and bloom are read smoothly and
 *     the full-size picture is softened where it is out of focus; at
 *     1280x720 the translation as it is.
 *   Always: the HDR highlight plane at the second target (rsx_hdr_plane),
 *     which the title's single target leaves unbound unless HDR is on. */
"""
os.makedirs(os.path.dirname(OUT), exist_ok=True)
open(OUT, "w", encoding="utf-8", newline="\n").write(head + s)
print("wrote", os.path.normpath(OUT))
