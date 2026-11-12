// The depth of field's final composite (the game's FRPG_Fil_Dof composite, ps_3_0), for a scene rendered above the
// original 1024x720 (RenderResolution). The math is the game's: a blur weight from the scene's alpha and the near
// blur mask, four blend weights from it (c66, c67), and the sharp scene mixed with three blur levels. Only the reads
// of the low-resolution textures differ. The game read its 512x360 (and 256x180) targets bilinearly into its
// 1024x720 scene, which was then stretched to the screen, bilinearly again: two mild stretches, smooth on any screen.
// At 4K one bilinear read stretches them 7.5x (15x) and shows their grid. Here the blur is read as the original
// showed it: bilinearly at the four nearest pixel centres of the original 1024x720 grid, those four blended by where
// the pixel lies between them (gMode.x = 0, the default). gMode.x = 1 reads with a cubic B-spline instead (smoother,
// a little more blur; for comparison). The blur levels themselves, and so the amount of blur, are the game's.
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
float4 gMode : register(c202);         // x: 0 = as the original showed it, 1 = cubic B-spline (set by the DLL)

static const float2 kGrid = float2(1024.0, 720.0);  // the original scene's size

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

// The texture as the original showed it: bilinear reads at the original 1024x720 grid's four nearest pixel centres,
// blended bilinearly.
float4 original(sampler2D s, float2 uv) {
    float2 p = uv * kGrid - 0.5;
    float2 c = floor(p);
    float2 f = p - c;
    float2 a = (c + 0.5) / kGrid;
    float2 b = (c + 1.5) / kGrid;
    float4 t00 = tex2Dlod(s, float4(a.x, a.y, 0, 0));
    float4 t10 = tex2Dlod(s, float4(b.x, a.y, 0, 0));
    float4 t01 = tex2Dlod(s, float4(a.x, b.y, 0, 0));
    float4 t11 = tex2Dlod(s, float4(b.x, b.y, 0, 0));
    return lerp(lerp(t00, t10, f.x), lerp(t01, t11, f.x), f.y);
}

float4 smooth(sampler2D s, float2 uv, float4 size) {
    float4 r;
    [branch] if (gMode.x > 0.5) {
        r = bspline(s, uv, size);
    } else {
        r = original(s, uv);
    }
    return r;
}

float4 main(float2 uv : TEXCOORD1) : COLOR0 {
    float nearBlur = smooth(gNear, uv, gNearSize).w;
    float4 src = tex2Dlod(gScene, float4(uv, 0, 0));
    float blurW = max(src.w, nearBlur);
    float4 w = saturate(blurW * gWeightScale + gWeightBias);
    float4 blend = float4(w.x, w.y - w.x, w.z - w.y, w.w - w.z);
    float4 o = src * blend.x;
    [branch] if (blend.y != 0.0 || blend.z != 0.0 || blend.w != 0.0) {
        o += smooth(gBlur1, uv, gBlurSize) * blend.y;
        o += smooth(gBlur2, uv, gBlurSize) * blend.z;
        o += smooth(gBlur3, uv, gBlurSize) * blend.w;
    }
    return o;
}
