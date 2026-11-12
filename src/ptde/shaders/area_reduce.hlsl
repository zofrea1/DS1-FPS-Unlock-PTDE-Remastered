// The copy of the full scene into a small post-processing target (the depth of field's 512x360 blur source, the
// glare's and the eye adaptation's 256x180), for a scene rendered above the original 1024x720 (RenderResolution).
// The game's copy shader is one bilinear read at the centre of each target pixel: at 1024x720 into 512x360 that is
// the exact average of the 2x2 scene pixels the target pixel covers. At 4K the same pixel covers 7.5x6 scene pixels,
// and one bilinear read takes 2x2 of them: the rest are skipped, so fine detail and highlights come and go in the
// blur and the glow as the camera moves (they shimmer and flicker). Here each target pixel is the exact average of
// every scene pixel it covers (partly covered ones by the part covered): what the original's 2x2 average gives, of a
// 1024x720 scene that was itself the average of the 4K one.
//
// The weights are separable (a pixel's x overlap times its y overlap), so two neighbouring pixels in x and two in y
// are read with one bilinear fetch placed between them at the ratio of their weights, times the sum of the weights:
// the same average with a quarter of the reads (at 4K, 4x4 or 5x4 fetches instead of 8x7 point reads; the bilinear
// weights the hardware uses are within 1/512 of the exact ones).
//
// Built with fxc (tools/build_shaders.bat) into area_reduce_ps.h; the DLL carries the compiled bytecode. The DLL sets
// stage 0 to bilinear filtering and clamping for the draw.

sampler2D gScene : register(s0);   // the scene at the render resolution
float4 gSrcSize : register(c200);  // the scene: width, height, 1 / width, 1 / height (set by the DLL)
float4 gDstSize : register(c201);  // the target: the same

// How much of scene pixel [i, i + 1) lies inside [a, b).
float overlap(float i, float a, float b) {
    return max(min(i + 1.0, b) - max(i, a), 0.0);
}

float4 main(float2 uv : TEXCOORD1) : COLOR0 {
    // The target pixel's footprint in scene pixels (the game's uv is the target pixel's centre).
    float2 a = (uv - 0.5 * gDstSize.zw) * gSrcSize.xy;
    float2 b = (uv + 0.5 * gDstSize.zw) * gSrcSize.xy;
    float2 first = floor(a);
    float2 last = ceil(b);
    float4 sum = 0;
    float total = 0;
    [loop] for (float y = first.y; y < last.y; y += 2.0) {
        float wy0 = overlap(y, a.y, b.y);
        float wy1 = overlap(y + 1.0, a.y, b.y);
        float wy = wy0 + wy1;
        float ty = wy1 / max(wy, 1e-6);
        [loop] for (float x = first.x; x < last.x; x += 2.0) {
            float wx0 = overlap(x, a.x, b.x);
            float wx1 = overlap(x + 1.0, a.x, b.x);
            float wx = wx0 + wx1;
            float tx = wx1 / max(wx, 1e-6);
            float w = wx * wy;
            sum += w * tex2Dlod(gScene, float4((float2(x + tx, y + ty) + 0.5) * gSrcSize.zw, 0, 0));
            total += w;
        }
    }
    return sum / max(total, 1e-6);
}
