// The game's last copy of its frame buffer onto the back buffer in windowed and borderless mode (ps_067116db: one
// bilinear read of the frame buffer, then the in-game Brightness through the 256-entry g_GammaTexture, per channel),
// with RenderResolution: the read becomes the exact average of every frame-buffer pixel the screen pixel covers,
// taken in linear light. With Supersampling (or RenderResolution above the window) the frame buffer is larger than
// the screen, and one bilinear read takes 2x2 of the 1.5x1.5 (or 2x2, 1.25x1.25...) pixels a screen pixel covers:
// at 1.5x the rest are skipped and thin edges shimmer as they move. At the same size it is the game's single read.
// The brightness lookup is the game's own (its texel-centre mapping and channel selection).
//
// Built with fxc (tools/build_shaders.bat) into present_lut_ps.h; the DLL point-samples stage 0 for the draw and
// sets c200 = the frame buffer's size, c201 = the viewport's (w, h, 1 / w, 1 / h).

sampler2D gTexture : register(s0);       // the frame buffer
sampler2D gGammaTexture : register(s1);  // 256x1 brightness table
float4 gSrc : register(c200);
float4 gDst : register(c201);

float3 to_linear(float3 c) { return pow(max(c, 0.0), 2.2); }
float3 to_gamma(float3 c) { return pow(max(c, 0.0), 1.0 / 2.2); }

float4 main(float4 uv : TEXCOORD0) : COLOR0 {
    // The game's uv is half a frame-buffer texel past the corner of the quad; the screen pixel's footprint in
    // frame-buffer pixels starts at uv * size - 0.5 and spans size / viewport.
    float2 scale = gSrc.xy * gDst.zw;
    float3 c;
    [branch] if (scale.x <= 1.0001 && scale.y <= 1.0001) {
        c = tex2Dlod(gTexture, float4(uv.xy, 0, 0)).rgb;  // not smaller than the frame buffer: the game's read
    } else {
        float2 a = uv.xy * gSrc.xy - 0.5;
        float2 b = a + scale;
        a = max(a, 0.0);
        b = min(b, gSrc.xy);
        float2 first = floor(a);
        float2 last = ceil(b);
        float3 sum = 0;
        float total = 0;
        [loop] for (float y = first.y; y < last.y; y += 1.0) {
            float wy = min(y + 1.0, b.y) - max(y, a.y);
            [loop] for (float x = first.x; x < last.x; x += 1.0) {
                float w = wy * (min(x + 1.0, b.x) - max(x, a.x));
                sum += w * to_linear(tex2Dlod(gTexture, float4((float2(x, y) + 0.5) * gSrc.zw, 0, 0)).rgb);
                total += w;
            }
        }
        c = to_gamma(sum / max(total, 1e-6));
    }
    float3 r0 = c * 0.99609375 + 0.001953125;
    return float4(tex2Dlod(gGammaTexture, float4(r0.x, 0.5, 0, 0)).x, tex2Dlod(gGammaTexture, float4(r0.y, 0.5, 0, 0)).y,
                  tex2Dlod(gGammaTexture, float4(r0.z, 0.5, 0, 0)).z, 1.0);
}
