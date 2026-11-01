/* Drakengard 3's depth-of-field and bloom composite (the engine's
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
struct PSInput {
    float4 position : SV_POSITION; float4 col0 : COLOR0; float4 col1 : COLOR1;
    float4 fog : FOG;
    float4 tc0:TEXCOORD0; float4 tc1:TEXCOORD1; float4 tc2:TEXCOORD2; float4 tc3:TEXCOORD3;
    float4 tc4:TEXCOORD4; float4 tc5:TEXCOORD5; float4 tc6:TEXCOORD6; float4 tc7:TEXCOORD7;
    float4 tc8:TEXCOORD8; float4 tc9:TEXCOORD9;
};
Texture2D    rsx_tex[16] : register(t0);
SamplerState rsx_samp[16] : register(s0);
cbuffer PSConstants : register(b1) {
    float4 fp_constants[20];
    float4 fp_alpha;
};
struct PSOutput {
    float4 t0 : SV_TARGET0;
    float4 t1 : SV_TARGET1;
};
/* A texture read as a uniform cubic B-spline over its texels: four bilinear
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

PSOutput main(PSInput input) {
    float4 r[48]; float4 h[48];
    [unroll] for (int _i = 0; _i < 48; _i++) { r[_i] = (float4)0; h[_i] = (float4)0; }
    float4 cc0 = (float4)0; float4 cc1 = (float4)0;
    uint tw, th;
    rsx_tex[0].GetDimensions(tw, th);
    const float k = (float)tw / 1280.0;   /* internal pixels per original pixel */
#ifdef POSTFX_HIGH
    const bool hi = k > 1.01;              /* at 1280x720, the translation as it is */
#else
    const bool hi = false;                 /* Post-Processing Quality: Original */
#endif
    { float4 _v = (float4)((input.tc1).zwzz); r[0].xy = _v.xy; }
    { float4 _v = (float4)(min(((r[0]).xyzw), ((fp_constants[0]).zwzz))); r[0].xy = _v.xy; }
    { float4 _v = (float4)(max(((r[0]).xyzw), ((fp_constants[1]).xyxx))); r[0].xy = _v.xy; }
    { float4 _v = hi ? bspline(rsx_tex[1], rsx_samp[1], r[0].xy) : rsx_tex[1].Sample(rsx_samp[1], ((r[0]).xyzw).xy); r[0].xyzw = _v.xyzw; }
    { float4 _v = (float4)(((r[0]).xyzw) * ((fp_constants[2]).xxxx)); r[0].xyz = _v.xyz; }
    {
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
    { float4 _v = (float4)(((r[1]).xyzw) + (-((r[0]).xyzw))); r[1].xyz = _v.xyz; }
    { float4 _v = (float4)(((r[0]).wwww) * ((r[1]).xyzw) + ((r[0]).xyzw)); h[0].xyz = _v.xyz; }
    { float4 _v = (float4)(dot(((h[0]).xyzw).xyz, ((fp_constants[4]).xyzw).xyz)); _v = clamp(_v, -65504.0, 65504.0); h[0].w = _v.w; }
    { float4 _v = hi ? bspline(rsx_tex[2], rsx_samp[2], input.tc0.zw) : rsx_tex[2].Sample(rsx_samp[2], ((input.tc0).zwzz).xy); r[1].xyz = _v.xyz; }
    { float4 _v = (float4)(exp2(((h[0]).wwww).x)); h[0].w = _v.w; }
    { float4 _v = (float4)(((r[1]).xyzw) * ((fp_constants[5]).xyzw)) * 4.0; h[1].xyz = _v.xyz; }
    { float4 _v = (float4)(((h[0]).xyzw) * ((fp_constants[6]).xyzw)); _v = saturate(_v); h[0].w = _v.w; }
    { float4 _v = (float4)(((h[1]).xyzw) * ((h[0]).wwww) + ((h[0]).xyzw)); _v = clamp(_v, -65504.0, 65504.0); h[2].xyz = _v.xyz; }
    { float4 _v = (float4)(log2(((h[2]).xyzw).x)); r[0].x = _v.x; }
    { float4 _v = (float4)(((r[0]).xyzw) * ((fp_constants[7]).xxxx)); r[0].x = _v.x; }
    { float4 _v = (float4)(log2(((h[2]).zzzz).x)); r[0].z = _v.z; }
    { float4 _v = (float4)(log2(((h[2]).yyyy).x)); r[0].y = _v.y; }
    { float4 _v = (float4)(((r[0]).zzzz) * ((fp_constants[8]).xxxx)); r[1].x = _v.x; }
    { float4 _v = (float4)(exp2(((r[1]).xyzw).x)); _v = saturate(_v); r[0].w = _v.w; }
    { float4 _v = (float4)(((r[0]).yyyy) * ((fp_constants[9]).xxxx)); r[1].x = _v.x; }
    { float4 _v = (float4)(((r[0]).wwww) * ((fp_constants[10]).xxxx)); r[0].y = _v.y; }
    { float4 _v = (float4)(floor((r[0]).xyzw)); r[1].y = _v.y; }
    { float4 _v = (float4)(exp2(((r[1]).xyzw).x)); _v = saturate(_v); r[1].x = _v.x; }
    { float4 _v = (float4)(((r[1]).yyyy) * ((fp_constants[11]).xxxx)); r[1].w = _v.w; }
    { float4 _v = (float4)(((r[1]).xxxx) * ((fp_constants[12]).yyyy)); r[1].z = _v.z; }
    { float4 _v = (float4)(exp2(((r[0]).xyzw).x)); _v = saturate(_v); r[0].x = _v.x; }
    { float4 _v = (float4)((r[1]).zzzz); r[2].w = _v.w; }
    { float4 _v = (float4)(((r[0]).xxxx) * ((fp_constants[13]).xxxx) + ((r[1]).wwww)); r[2].z = _v.z; }
    { float4 _v = (float4)((-((r[1]).xyzw)) * ((fp_constants[14]).xxxx)); r[1].w = _v.w; }
    { float4 _v = (float4)(((r[2]).zwzz) + ((fp_constants[15]).xyxx)); r[0].xy = _v.xy; }
    { float4 _v = (float4)(rsx_tex[3].Sample(rsx_samp[3], ((r[0]).xyzw).xy)); r[0].xyz = _v.xyz; }
    { float4 _v = (float4)((r[1]).zzzz); r[2].w = _v.w; }
    { float4 _v = (float4)(((r[2]).xyzw) + ((fp_constants[16]).xxxx)); r[2].z = _v.z; }
    { float4 _v = (float4)(((r[2]).zwzz) + ((fp_constants[17]).xyxx)); r[1].xy = _v.xy; }
    { float4 _v = (float4)(rsx_tex[3].Sample(rsx_samp[3], ((r[1]).xyzw).xy)); r[1].xyz = _v.xyz; }
    { float4 _v = (float4)(((r[1]).xyzw) + (-((r[0]).xyzw))); r[1].xyz = _v.xyz; }
    { float4 _v = (float4)(((r[0]).xyzw) * ((fp_constants[18]).xxxx) + ((r[1]).xyzw)); r[0].w = _v.w; }
    { float4 _v = (float4)(((r[0]).wwww) * ((r[1]).xyzw) + ((r[0]).xyzw)); r[0].xyz = _v.xyz; }
    { float4 _v = (float4)(dot(((r[0]).xyzw).xyz, ((fp_constants[19]).xyzw).xyz)); r[0].w = _v.w; }
    PSOutput _out;
    float4 _o0 = r[0];
    _out.t0 = (_o0 == _o0) ? _o0 : (float4)0;
    /* The HDR highlight plane (rsx_hdr_plane: the renderer binds it here, as
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
    return _out;
}
