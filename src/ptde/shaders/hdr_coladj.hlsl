// The final composite (the game's FRPG_Fil_HDR_ColAdj, ps_3_0), for a scene rendered above the original resolution
// (RenderResolution). The math is the game's, step for step: the scene scaled (c56.z), the bloom added by its rate
// (c56.x, c71.y), the light shafts added (c56.y), the colour matrix (c62-c65), and the film grain overlay (the noise
// tile, s2, at the original 1280x720 grid through the vertex shader's v0.zw) by c68.x. Only the reads of the glows
// differ: the bloom (128x90) and the light shafts (256x180) were one bilinear read each, a 30x and a 15x stretch at
// 4K that showed their grid as blocks and diamonds in every glow; here they are read with a cubic B-spline (four
// bilinear reads), as the depth of field's blur is (dof_composite.hlsl). At the original 1280x720 they were stretched
// 10x and 5x, and again to the screen.
//
// With HighPrecisionColor (the scene in 16-bit float) the result gets a fine ordered dither (c203.x), so the smooth
// float gradients do not band in the 8-bit frame.
//
// Built with fxc (tools/build_shaders.bat) into hdr_coladj_ps.h; the DLL carries the compiled bytecode.

sampler2D gScene : register(s0);   // the scene at the render resolution
sampler2D gBloom : register(s1);   // 128x90: the bloom (alpha = its rate)
sampler2D gNoise : register(s2);   // 256x256 tile: the film grain
sampler2D gShaft : register(s3);   // 256x180: the light shafts

float4 gC56 : register(c56);       // the game's DL_FREG_056: x bloom rate base, y light shaft rate, z scene scale
// The game's DL_FREG_062: the colour matrix, one row a register (dp4 with each, as the game does; a float4x4 here
// would be packed by columns).
float4 gColor0 : register(c62);
float4 gColor1 : register(c63);
float4 gColor2 : register(c64);
float4 gColor3 : register(c65);
float4 gC68 : register(c68);       // the game's DL_FREG_068: x the film grain's strength
float4 gC71 : register(c71);       // the game's DL_FREG_071: y the bloom rate's top
float4 gBloomSize : register(c200);  // s1: width, height, 1 / width, 1 / height (set by the DLL)
float4 gShaftSize : register(c201);  // s3: the same
float4 gDither : register(c203);     // x: the dither's size (1/255 with HighPrecisionColor's float scene, else 0)

// The 4x4 ordered (Bayer) dither index of a pixel, 0 to 15.
float bayer4(float2 p) {
    float2 a = fmod(p, 2.0);
    float2 b = fmod(floor(p * 0.5), 2.0);
    return 4.0 * (abs(a.x - a.y) * 2.0 + a.y) + (abs(b.x - b.y) * 2.0 + b.y);
}

float4 bspline(sampler2D s, float2 uv, float4 size) {
    float2 p = uv * size.xy - 0.5;
    float2 c = floor(p);
    float2 f = p - c;
    float2 f2 = f * f;
    float2 f3 = f2 * f;
    float2 w0 = (1.0 - 3.0 * f + 3.0 * f2 - f3) / 6.0;
    float2 w1 = (4.0 - 6.0 * f2 + 3.0 * f3) / 6.0;
    float2 w2 = (1.0 + 3.0 * f + 3.0 * f2 - 3.0 * f3) / 6.0;
    float2 w3 = f3 / 6.0;
    float2 g0 = w0 + w1;
    float2 g1 = w2 + w3;
    float2 t0 = (c - 0.5 + w1 / g0) * size.zw;
    float2 t1 = (c + 1.5 + w3 / g1) * size.zw;
    return g0.y * (g0.x * tex2Dlod(s, float4(t0.x, t0.y, 0, 0)) + g1.x * tex2Dlod(s, float4(t1.x, t0.y, 0, 0))) +
           g1.y * (g0.x * tex2Dlod(s, float4(t0.x, t1.y, 0, 0)) + g1.x * tex2Dlod(s, float4(t1.x, t1.y, 0, 0)));
}

float4 main(float4 uv : TEXCOORD1, float2 pos : VPOS) : COLOR0 {
    float4 noise = tex2D(gNoise, uv.zw);
    float3 col = saturate(tex2D(gScene, uv.xy).rgb * gC56.z);
    float4 bloom = bspline(gBloom, uv.xy, gBloomSize);
    float bloomRate = bloom.w * (gC71.y - gC56.x) + gC56.x;
    col += bloomRate * bloom.rgb;
    col += gC56.y * bspline(gShaft, uv.xy, gShaftSize).rgb;
    float4 c4 = float4(col, 1.0);
    float4 graded = float4(dot(c4, gColor0), dot(c4, gColor1), dot(c4, gColor2), dot(c4, gColor3));
    // The game's overlay with the noise, as its cmp pair picks it: noise <= 0.5 takes b, above it a.
    float4 a = 2.0 * noise * graded;
    float4 b = 1.0 - 2.0 * (1.0 - noise) * (1.0 - graded);
    float4 overlay = (0.5 - noise >= 0.0) ? b : a;
    float4 result = gC68.x * (overlay - graded) + graded;
    // HighPrecisionColor: the float scene's smooth gradients reach the 8-bit frame through an ordered dither of less
    // than half a step instead of rounding into bands (the scene times c56.z and the colour matrix widen every step of
    // an 8-bit scene, the reason fog and dark skies band).
    result.rgb += ((bayer4(floor(pos)) + 0.5) / 16.0 - 0.5) * gDither.x;
    return result;
}
