// The depth of field's blur levels made at half the render resolution (DepthOfFieldResolution = half), for a scene
// rendered above the original 1024x720 (RenderResolution). The game makes its three blur levels in 512x360 and
// 256x180 targets from small fixed kernels:
//   level 1: the scene averaged into 512x360 (2x2 of the original 1024x720);
//   level 2: level 1 through a 5-tap cross about one 512x360 texel wide (taps at +-(1, 0.28) and +-(0.28, -1), each
//            0.212, the centre the rest; FRPG_Fil_Dof's ps_daad5659 with vs_88b6c82d);
//   level 3: level 1 halved into 256x180, the same cross there, and the result spread back over 512x360 by four
//            0.25 taps at +-0.5 texel (ps_bed29ac7 with vs_86c284d1).
// Every tap counts only when its blur rate (alpha) is at least the centre's, so a sharp foreground does not bleed into
// the blurred background. At 4K each 512x360 texel covers 7.5x6 screen pixels and a 256x180 one 15x12: the levels'
// grid shows through in blocks, however smoothly they are read.
//
// Here the same three levels are made at half the render resolution (level 3 at a quarter) by separable Gaussians of
// the same widths, measured in the original 1024x720 pixels (the game's picture was 1024 wide stretched to 1280, so
// its blur was 1.25 times wider than tall on screen): standard deviations of 1.3, 1.93 and 4.07 original pixels for
// the three levels (each including the stretch of its read in the composite), measured against the game's levels. The taps keep the game's rule
// (a tap counts when its rate is at least the centre's). The composite (main_composite) is the game's math with these
// levels in place of the small ones.
//
// Built with fxc (tools/build_shaders.bat) into dof_hr_*.h; the DLL carries the compiled bytecode and sets, per pass,
// c0 (the target's half-pixel offset), c1-c3 (see each entry point). Stages are bilinear and clamped.

// --- vertex shader: a full-target quad from clip-space corners, uv with D3D9's half-pixel shift -------------------
float4 gHalfPixel : register(c0);   // x: -1 / target width, y: 1 / target height

struct VsOut {
    float4 pos : POSITION;
    float2 uv : TEXCOORD0;
};

VsOut main_vs(float4 pos : POSITION) {
    VsOut o;
    o.pos = float4(pos.xy + gHalfPixel.xy, 0.0, 1.0);
    o.uv = pos.xy * float2(0.5, -0.5) + 0.5;
    return o;
}

sampler2D gSrc : register(s0);

// --- 2x2 average into a target of half the size: one bilinear read between the four pixels ---------------------
float4 gSrcTexel : register(c1);    // xy: 1 / source size
float4 main_down(float2 uv : TEXCOORD0) : COLOR0 {
    // uv is the centre of the target pixel = the corner the four source pixels share. Clamped as the 8-bit scene was.
    return saturate(tex2Dlod(gSrc, float4(uv, 0, 0)));
}

// --- separable Gaussian along one axis, the game's rate rule on every tap ----------------------------------------
// Two neighbouring texels per read: one bilinear fetch between texels i and i + 1 at the ratio of their weights,
// times the sum of the weights (half the reads); the rate rule is applied to the fetched (interpolated) rate.
float4 gStep : register(c2);        // xy: one source texel along the axis (uv)
float4 gKernel : register(c3);      // x: sigma (texels), y: radius (whole texels), z: 1 / (2 sigma^2)
float4 main_blur(float2 uv : TEXCOORD0) : COLOR0 {
    float4 centre = tex2Dlod(gSrc, float4(uv, 0, 0));
    float4 sum = centre;
    float total = 1.0;
    [loop] for (float i = 1.0; i <= gKernel.y; i += 2.0) {
        float w0 = exp(-i * i * gKernel.z);
        float w1 = exp(-(i + 1.0) * (i + 1.0) * gKernel.z);
        float w = w0 + w1;
        float o = i + w1 / w;
        float4 a = tex2Dlod(gSrc, float4(uv + gStep.xy * o, 0, 0));
        float4 b = tex2Dlod(gSrc, float4(uv - gStep.xy * o, 0, 0));
        // The game's rule on 8-bit rates, whose equal steps made it stable: with float rates a tolerance of about one
        // such step, or neighbours on a smooth surface would count or not pixel by pixel (noise).
        float wa = a.w >= centre.w - 0.006 ? w : 0.0;
        float wb = b.w >= centre.w - 0.006 ? w : 0.0;
        sum += a * wa + b * wb;
        total += wa + wb;
    }
    return sum / total;
}

// --- the composite: the game's FRPG_Fil_Dof with our levels ------------------------------------------------------
sampler2D gScene : register(s0);    // the scene at the render resolution; alpha = far blur rate
sampler2D gLevel1 : register(s1);   // half size
sampler2D gLevel2 : register(s2);   // half size
sampler2D gLevel3 : register(s3);   // quarter size
sampler2D gNear : register(s4);     // the game's 256x180 near blur mask (alpha)
float4 gWeightScale : register(c66);  // the game's DL_FREG_066
float4 gWeightBias : register(c67);   // the game's DL_FREG_067
float4 gL3Size : register(c200);      // s3: width, height, 1 / width, 1 / height
float4 gNearSize : register(c201);    // s4: the same
float4 gStrength : register(c202);    // x: DepthOfFieldStrength (1 = the game's)

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

float4 main_composite(float2 uv : TEXCOORD1) : COLOR0 {
    float nearBlur = bspline(gNear, uv, gNearSize).w;
    float4 src = saturate(tex2Dlod(gScene, float4(uv, 0, 0)));
    float blurW = max(src.w, nearBlur) * gStrength.x;
    float4 w = saturate(blurW * gWeightScale + gWeightBias);
    float4 blend = float4(w.x, w.y - w.x, w.z - w.y, w.w - w.z);
    float4 o = src * blend.x;
    [branch] if (blend.y != 0.0 || blend.z != 0.0 || blend.w != 0.0) {
        o += tex2Dlod(gLevel1, float4(uv, 0, 0)) * blend.y;
        o += tex2Dlod(gLevel2, float4(uv, 0, 0)) * blend.z;
        o += bspline(gLevel3, uv, gL3Size) * blend.w;
    }
    return o;
}
