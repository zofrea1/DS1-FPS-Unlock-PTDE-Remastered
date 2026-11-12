// The last scale-down of the frame when the game draws above the screen's size (Supersampling, or RenderResolution
// above the display): the game's StretchRect of its frame buffer onto the back buffer is replaced by a draw of this
// pair. Each screen pixel is the exact average of every frame-buffer pixel it covers (partly covered ones by the
// part covered), which is what supersampling needs: the driver's filtered StretchRect is bilinear, which at 1.5x
// takes 2x2 of the 1.5x1.5 pixels a screen pixel covers and skips the rest (edges and thin lines shimmer as they
// move). The DLL turns on sRGB reads and writes when the device has them, so the average is taken in linear light,
// as the screen shows it.
//
// Built with fxc (tools/build_shaders.bat) into present_reduce_vs.h / present_reduce_ps.h.

float4 gMap : register(c200);  // source pixels per screen pixel (x, y); source pixel at screen pixel 0 (x, y)
float4 gTex : register(c201);  // 1 / source texture width, 1 / height, source width, source height
sampler2D gSrc : register(s0);

float4 main_vs(float4 pos : POSITION) : POSITION {
    return pos;
}

float4 main_ps(float2 vpos : VPOS) : COLOR0 {
    // vpos: the screen pixel's integer position (Direct3D 9). Its footprint in source pixels:
    float2 a = vpos * gMap.xy + gMap.zw;
    float2 b = a + gMap.xy;
    a = max(a, 0.0);
    b = min(b, gTex.zw);
    float2 first = floor(a);
    float2 last = ceil(b);
    float4 sum = 0;
    float total = 0;
    [loop] for (float y = first.y; y < last.y; y += 1.0) {
        float wy = min(y + 1.0, b.y) - max(y, a.y);
        [loop] for (float x = first.x; x < last.x; x += 1.0) {
            float w = wy * (min(x + 1.0, b.x) - max(x, a.x));
            sum += w * tex2Dlod(gSrc, float4((float2(x, y) + 0.5) * gTex.xy, 0, 0));
            total += w;
        }
    }
    return sum / max(total, 1e-6);
}
