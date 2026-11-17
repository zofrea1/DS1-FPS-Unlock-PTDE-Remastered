// The averaging copy of a large render target into a small one (area_reduce.hlsl) in two passes: the same exact area
// average (a box is separable), with far less work. area_reduce.hlsl lets each target pixel loop over its whole
// footprint (at 5760x3240 into 512x360, 11.25 x 9 scene pixels): few, long threads reading scattered rows, which
// measured about 0.45 ms per copy there. Here main_h averages along x into a target of the destination's width and
// the source's height (each pixel a run of one source row, read two pixels a fetch), and main_v averages that along
// y into the destination: many short threads reading neighbouring texels.
//
// Built with fxc (tools/build_shaders.bat) into area_reduce2_h_ps.h and area_reduce2_v_ps.h.

sampler2D gSrc : register(s0);
float4 gSrcSize : register(c200);  // this pass's source: width, height, 1 / width, 1 / height (set by the DLL)
float4 gDstSize : register(c201);  // this pass's target: the same

float overlap(float i, float a, float b) {
    return max(min(i + 1.0, b) - max(i, a), 0.0);
}

// Pass 1, drawn with dof_hr.hlsl's vertex shader: uv is the centre of the target pixel, so uv.y is the centre of a
// source row (the target has the source's height).
float4 main_h(float2 uv : TEXCOORD0) : COLOR0 {
    float a = (uv.x - 0.5 * gDstSize.z) * gSrcSize.x;
    float b = (uv.x + 0.5 * gDstSize.z) * gSrcSize.x;
    float last = ceil(b);
    float4 sum = 0;
    float total = 0;
    [loop] for (float x = floor(a); x < last; x += 2.0) {
        float w0 = overlap(x, a, b);
        float w1 = overlap(x + 1.0, a, b);
        float w = w0 + w1;
        float t = w1 / max(w, 1e-6);
        // Clamped as an 8-bit scene was (HighPrecisionColor's float scene can go above 1).
        sum += w * saturate(tex2Dlod(gSrc, float4((x + t + 0.5) * gSrcSize.z, uv.y, 0, 0)));
        total += w;
    }
    return sum / max(total, 1e-6);
}

// Pass 2, drawn with the game's own quad and vertex shader (uv the centre of the target pixel, TEXCOORD1): pass 1's
// target has the destination's width, so uv.x is the centre of one of its columns.
float4 main_v(float2 uv : TEXCOORD1) : COLOR0 {
    float a = (uv.y - 0.5 * gDstSize.w) * gSrcSize.y;
    float b = (uv.y + 0.5 * gDstSize.w) * gSrcSize.y;
    float last = ceil(b);
    float4 sum = 0;
    float total = 0;
    [loop] for (float y = floor(a); y < last; y += 2.0) {
        float w0 = overlap(y, a, b);
        float w1 = overlap(y + 1.0, a, b);
        float w = w0 + w1;
        float t = w1 / max(w, 1e-6);
        sum += w * tex2Dlod(gSrc, float4(uv.x, (y + t + 0.5) * gSrcSize.w, 0, 0));
        total += w;
    }
    return sum / max(total, 1e-6);
}
