// The final composite (the game's FRPG_Fil_HDR_ColAdj, ps_3_0), for a scene rendered above the original resolution
// (RenderResolution). The math is the game's, step for step: the scene scaled (c56.z), the bloom added by its rate
// (c56.x, c71.y), the light shafts added (c56.y), the colour matrix (c62-c65), and the film grain overlay (the noise
// tile, s2, at the original 1280x720 grid through the vertex shader's v0.zw) by c68.x. Only the reads of the glows
// differ: the bloom (128x90) and the light shafts (256x180) were one bilinear read each, a 30x and a 15x stretch at
// 4K that showed their grid as blocks and diamonds in every glow; here they are read with a cubic B-spline (four
// bilinear reads), as the depth of field's blur is (dof_composite.hlsl). At the original 1280x720 they were stretched
// 10x and 5x, and again to the screen.
//
// Built with fxc (tools/build_shaders.bat) into hdr_coladj_ps.h; the DLL carries the compiled bytecode.

sampler2D gScene : register(s0);   // the scene at the render resolution
sampler2D gBloom : register(s1);   // 128x90: the bloom (alpha = its rate)
sampler2D gNoise : register(s2);   // 256x256 tile: the film grain
sampler2D gShaft : register(s3);   // 256x180: the light shafts

float4 gC56 : register(c56);       // the game's DL_FREG_056: x bloom rate base, y light shaft rate, z scene scale
float4x4 gColor : register(c62);   // the game's DL_FREG_062 (rows c62-c65): the colour matrix
float4 gC68 : register(c68);       // the game's DL_FREG_068: x the film grain's strength
float4 gC71 : register(c71);       // the game's DL_FREG_071: y the bloom rate's top
float4 gBloomSize : register(c200);  // s1: width, height, 1 / width, 1 / height (set by the DLL)
float4 gShaftSize : register(c201);  // s3: the same

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

float4 main(float4 uv : TEXCOORD1) : COLOR0 {
    float4 noise = tex2D(gNoise, uv.zw);
    float3 col = saturate(tex2D(gScene, uv.xy).rgb * gC56.z);
    float4 bloom = bspline(gBloom, uv.xy, gBloomSize);
    float bloomRate = bloom.w * (gC71.y - gC56.x) + gC56.x;
    col += bloomRate * bloom.rgb;
    col += gC56.y * bspline(gShaft, uv.xy, gShaftSize).rgb;
    float4 c4 = float4(col, 1.0);
    float4 graded = float4(dot(c4, gColor[0]), dot(c4, gColor[1]), dot(c4, gColor[2]), dot(c4, gColor[3]));
    // The game's overlay with the noise, as its cmp pair picks it: noise <= 0.5 takes b, above it a.
    float4 a = 2.0 * noise * graded;
    float4 b = 1.0 - 2.0 * (1.0 - noise) * (1.0 - graded);
    float4 overlay = (0.5 - noise >= 0.0) ? b : a;
    return gC68.x * (overlay - graded) + graded;
}
