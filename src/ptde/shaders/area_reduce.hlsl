// The copy of the full scene into a small post-processing target (the depth of field's 512x360 blur source, the
// glare's and the eye adaptation's 256x180), for a scene rendered above the original 1024x720 (RenderResolution).
// The game's copy shader is one bilinear read at the centre of each target pixel: at 1024x720 into 512x360 that is
// the exact average of the 2x2 scene pixels the target pixel covers. At 4K the same pixel covers 7.5x6 scene pixels,
// and one bilinear read takes 2x2 of them: the rest are skipped, so fine detail and highlights come and go in the
// blur and the glow as the camera moves (they shimmer and flicker). Here each target pixel is the exact average of
// every scene pixel it covers (partly covered ones by the part covered): what the original's 2x2 average gives, of a
// 1024x720 scene that was itself the average of the 4K one.
//
// Built with fxc (tools/build_shaders.bat) into area_reduce_ps.h; the DLL carries the compiled bytecode. The DLL sets
// stage 0 to point sampling and clamping for the draw.

sampler2D gScene : register(s0);   // the scene at the render resolution
float4 gSrcSize : register(c200);  // the scene: width, height, 1 / width, 1 / height (set by the DLL)
float4 gDstSize : register(c201);  // the target: the same

float4 main(float2 uv : TEXCOORD1) : COLOR0 {
    // The target pixel's footprint in scene pixels (the game's uv is the target pixel's centre).
    float2 a = (uv - 0.5 * gDstSize.zw) * gSrcSize.xy;
    float2 b = (uv + 0.5 * gDstSize.zw) * gSrcSize.xy;
    float2 first = floor(a);
    float2 last = ceil(b);
    float4 sum = 0;
    float total = 0;
    [loop] for (float y = first.y; y < last.y; y += 1.0) {
        float wy = min(y + 1.0, b.y) - max(y, a.y);
        [loop] for (float x = first.x; x < last.x; x += 1.0) {
            float w = wy * (min(x + 1.0, b.x) - max(x, a.x));
            sum += w * tex2Dlod(gScene, float4((float2(x, y) + 0.5) * gSrcSize.zw, 0, 0));
            total += w;
        }
    }
    return sum / max(total, 1e-6);
}
