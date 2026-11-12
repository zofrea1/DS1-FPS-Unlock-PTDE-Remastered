// The depth of field's final composite (the game's FRPG_Fil_Dof composite, ps_3_0), for a scene rendered above the
// original 1024x720 (RenderResolution). The math is the game's: a blur weight from the scene's alpha and the near
// blur mask, four blend weights from it (c66, c67), and the sharp scene mixed with three blur levels. Only the reads
// of the low-resolution textures differ: the game's single bilinear read of a 512x360 (and 256x180) target is a
// cubic B-spline here (four bilinear reads), so at 4K the blur is smooth instead of showing its 512x360 grid. The
// blur levels themselves are the game's. Measured against the original (the same view at 1024x720, Firelink): in a
// half-blurred area the original's detail and kinks (mean |first| and |second| differences, exposure-matched) were
// 0.29 and 0.42, this 0.28 and 0.43, the game's own read at 4K 0.38 and 0.54. (Reading the blur as the original's two
// bilinear stretches did, 1024x720 grid then the screen, gave 0.33 and 0.49: the sharp part mixed into a half-blurred
// area is now full resolution, and the B-spline's slightly wider reach evens that out.)
//
// DepthOfFieldStrength (c202.x) scales the blur weight (1 = the game's).
//
// Built with fxc (tools/build_shaders.bat) into dof_composite_ps.h; the DLL carries the compiled bytecode.

sampler2D gScene : register(s0);   // the scene at the render resolution; alpha = far blur weight
sampler2D gBlur1 : register(s1);   // 512x360: the scene reduced
sampler2D gBlur2 : register(s2);   // 512x360: that blurred
sampler2D gBlur3 : register(s3);   // 512x360: blurred more (from 256x180)
sampler2D gNear : register(s4);    // 256x180: the near blur mask (alpha)

float4 gWeightScale : register(c66);   // the game's DL_FREG_066
float4 gWeightBias : register(c67);    // the game's DL_FREG_067
float4 gBlurSize : register(c200);     // s1..s3: width, height, 1 / width, 1 / height (set by the DLL)
float4 gNearSize : register(c201);     // s4: the same
float4 gStrength : register(c202);     // x: DepthOfFieldStrength (1 = the game's; set by the DLL)

// Cubic B-spline reconstruction with four bilinear reads (all weights positive, so each pair of taps folds into one
// bilinear read between them).
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
    float2 t0 = (c - 0.5 + w1 / g0) * size.zw;  // between texels c-1 and c (texel centres at i + 0.5)
    float2 t1 = (c + 1.5 + w3 / g1) * size.zw;  // between texels c+1 and c+2
    return g0.y * (g0.x * tex2Dlod(s, float4(t0.x, t0.y, 0, 0)) + g1.x * tex2Dlod(s, float4(t1.x, t0.y, 0, 0))) +
           g1.y * (g0.x * tex2Dlod(s, float4(t0.x, t1.y, 0, 0)) + g1.x * tex2Dlod(s, float4(t1.x, t1.y, 0, 0)));
}

float4 main(float2 uv : TEXCOORD1) : COLOR0 {
    float nearBlur = bspline(gNear, uv, gNearSize).w;
    float4 src = tex2Dlod(gScene, float4(uv, 0, 0));
    // DepthOfFieldStrength scales the blur weight before the game's own mapping to its blur levels: 0.5 gives a far
    // area the level the game gives half its blur, 0 the in-focus weights everywhere; 1 is the game's exactly.
    float blurW = max(src.w, nearBlur) * gStrength.x;
    float4 w = saturate(blurW * gWeightScale + gWeightBias);
    float4 blend = float4(w.x, w.y - w.x, w.z - w.y, w.w - w.z);
    float4 o = src * blend.x;
    [branch] if (blend.y != 0.0 || blend.z != 0.0 || blend.w != 0.0) {
        o += bspline(gBlur1, uv, gBlurSize) * blend.y;
        o += bspline(gBlur2, uv, gBlurSize) * blend.z;
        o += bspline(gBlur3, uv, gBlurSize) * blend.w;
    }
    return o;
}
