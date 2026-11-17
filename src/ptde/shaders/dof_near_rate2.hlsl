// The depth of field's near blur mask (dof_near_rate.hlsl: each 256x180 mask pixel the average of the near rate over
// every depth pixel it covers) in two passes, the same average with far less work. dof_near_rate.hlsl lets each mask
// pixel loop over its whole footprint (at 3840x2160, 15 x 12 depth pixels: 180 reads in one long thread), measured at
// 0.22 ms a frame there. The rate is not linear in the depth, so it is worked out for every depth pixel either way:
// main_h averages it along x into a target of the mask's width and the depth's height, main_v averages that along y
// into the mask with the game's own draw (and the colour as the game's shader takes it).
//
// Built with fxc (tools/build_shaders.bat) into dof_near_rate2_h_ps.h and dof_near_rate2_v_ps.h.

sampler2D gColor : register(s0);
sampler2D gSrc : register(s1);         // main_h: the depth, packed into RGB (the game's DecodeDepthTexture);
                                       // main_v: main_h's target (the rate in x)
float4 gDepthRange : register(c8);     // the game's DL_FREG_008: x, y the depth range
float4 gNearParams : register(c10);    // the game's DL_FREG_010: x near begin, y near end, z the rate's scale
float4 gSrcSize : register(c200);      // s1: width, height, 1 / width, 1 / height (set by the DLL)
float4 gDstSize : register(c201);      // this pass's target: the same

float near_rate(float3 packed) {
    float d = dot(packed, float3(0.99609375, 0.00389099121, 1.51991844e-005));
    float z = 1.0 / (d * gDepthRange.x + (1.0 - d) * gDepthRange.y);
    float inv = 1.0 / gDepthRange.y;
    float begin = inv * gNearParams.x;
    float range = gNearParams.y * inv - begin;
    return saturate((gDepthRange.x * z - begin) * (1.0 / range)) * gNearParams.z;
}

// Pass 1, drawn with dof_hr.hlsl's vertex shader: uv is the centre of the target pixel, so uv.y is the centre of a
// depth row (the target has the depth's height).
float4 main_h(float2 uv : TEXCOORD0) : COLOR0 {
    float a = (uv.x - 0.5 * gDstSize.z) * gSrcSize.x;
    float b = (uv.x + 0.5 * gDstSize.z) * gSrcSize.x;
    float last = ceil(b);
    float sum = 0;
    float total = 0;
    [loop] for (float x = floor(a); x < last; x += 1.0) {
        float w = min(x + 1.0, b) - max(x, a);
        sum += w * near_rate(tex2Dlod(gSrc, float4((x + 0.5) * gSrcSize.z, uv.y, 0, 0)).rgb);
        total += w;
    }
    float r = sum / max(total, 1e-6);
    return float4(r, r, r, r);
}

// Pass 2, drawn with the game's own quad and vertex shader (uv the centre of the mask pixel, TEXCOORD0): pass 1's
// target has the mask's width, so uv.x is the centre of one of its columns.
float4 main_v(float2 uv : TEXCOORD0) : COLOR0 {
    float4 o = tex2D(gColor, uv);
    float a = (uv.y - 0.5 * gDstSize.w) * gSrcSize.y;
    float b = (uv.y + 0.5 * gDstSize.w) * gSrcSize.y;
    float last = ceil(b);
    float sum = 0;
    float total = 0;
    [loop] for (float y = floor(a); y < last; y += 1.0) {
        float w = min(y + 1.0, b) - max(y, a);
        sum += w * tex2Dlod(gSrc, float4(uv.x, (y + 0.5) * gSrcSize.w, 0, 0)).x;
        total += w;
    }
    o.w = sum / max(total, 1e-6);
    return o;
}
