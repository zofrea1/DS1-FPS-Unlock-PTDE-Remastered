// The depth of field's near blur mask (the game's FRPG_Fil_Dof_NearRate, ps_3_0), for a scene rendered above the
// original 1024x720 (RenderResolution). The game reads the depth once at the centre of each 256x180 mask pixel and
// turns it into a near blur rate (alpha); the next pass spreads the largest rate over the 3x3 around it. One depth
// sample stood for 4x4 scene pixels at 1024x720 but for 15x12 at 4K, and whichever pixel it lands on decides the
// whole cell: along the edge of anything near (grass, a railing) the cells flip on and off as the camera moves, and
// the near blur flickers in blocks. Here the rate is worked out, with the game's math, for every depth pixel the mask
// pixel covers and averaged (partly covered ones by the part covered): on average the original's single sample, but
// steady. The colour is the game's (one read at the centre; the next pass sets it to white anyway).
//
// Built with fxc (tools/build_shaders.bat) into dof_near_rate_ps.h; the DLL carries the compiled bytecode. The DLL
// sets stage 1 to point sampling and clamping for the draw.

sampler2D gColor : register(s0);
sampler2D gDepth : register(s1);       // the depth, packed into RGB (the game's DecodeDepthTexture)
float4 gDepthRange : register(c8);     // the game's DL_FREG_008: x, y the depth range
float4 gNearParams : register(c10);    // the game's DL_FREG_010: x near begin, y near end, z the rate's scale
float4 gDepthSize : register(c200);    // s1: width, height, 1 / width, 1 / height (set by the DLL)
float4 gDstSize : register(c201);      // the mask target: the same

// The game's near rate for one depth pixel, step for step.
float near_rate(float3 packed) {
    float d = dot(packed, float3(0.99609375, 0.00389099121, 1.51991844e-005));
    float z = 1.0 / (d * gDepthRange.x + (1.0 - d) * gDepthRange.y);
    float inv = 1.0 / gDepthRange.y;
    float begin = inv * gNearParams.x;
    float range = gNearParams.y * inv - begin;
    return saturate((gDepthRange.x * z - begin) * (1.0 / range)) * gNearParams.z;
}

float4 main(float2 uv : TEXCOORD0) : COLOR0 {
    float4 o = tex2D(gColor, uv);
    float2 a = (uv - 0.5 * gDstSize.zw) * gDepthSize.xy;
    float2 b = (uv + 0.5 * gDstSize.zw) * gDepthSize.xy;
    float2 first = floor(a);
    float2 last = ceil(b);
    float sum = 0;
    float total = 0;
    [loop] for (float y = first.y; y < last.y; y += 1.0) {
        float wy = min(y + 1.0, b.y) - max(y, a.y);
        [loop] for (float x = first.x; x < last.x; x += 1.0) {
            float w = wy * (min(x + 1.0, b.x) - max(x, a.x));
            sum += w * near_rate(tex2Dlod(gDepth, float4((float2(x, y) + 0.5) * gDepthSize.zw, 0, 0)).rgb);
            total += w;
        }
    }
    o.w = sum / max(total, 1e-6);
    return o;
}
