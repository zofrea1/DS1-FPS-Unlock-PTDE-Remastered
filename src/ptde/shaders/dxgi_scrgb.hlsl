// DXVK through DXGI in HDR (dxgipresent.cpp, DxvkHdrSwapChain): the game's SDR picture into an scRGB swap chain
// (FP16, linear, BT.709 primaries, 1.0 = 80 nits), the way Windows composes an SDR window on an HDR desktop: the
// sRGB curve decoded, then scaled to the SDR brightness setting (c0.x = SDR white in nits / 80). The swap chain then
// matches what the display runs, so Windows can flip it straight to the display instead of an overlay plane.
Texture2D<float4> gSource : register(t0);

cbuffer Params : register(b0) {
    float4 gScale;  // x: SDR white / 80 nits
};

// A triangle over the whole target from the vertex index alone.
float4 main_vs(uint id : SV_VertexID) : SV_Position {
    const float2 t = float2((id << 1) & 2, id & 2);
    return float4(t * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0);
}

float3 srgb_to_linear(float3 c) {
    const float3 lo = c / 12.92;
    const float3 hi = pow((max(c, 0.0) + 0.055) / 1.055, 2.4);
    return float3(c.r <= 0.04045 ? lo.r : hi.r, c.g <= 0.04045 ? lo.g : hi.g, c.b <= 0.04045 ? lo.b : hi.b);
}

float4 main_ps(float4 pos : SV_Position) : SV_Target {
    const float3 c = gSource.Load(int3(pos.xy, 0)).rgb;
    return float4(srgb_to_linear(c) * gScale.x, 1.0);
}

// HDR10 (DxvkHdrSwapChain = hdr10): the same light (SDR white / 80 nits scale, in nits = scRGB x 80), into BT.2020
// primaries and the PQ curve (SMPTE ST 2084), for a 10-bit swap chain in the display's own scanout format.
float3 pq(float3 nits) {
    const float m1 = 0.1593017578125, m2 = 78.84375, c1 = 0.8359375, c2 = 18.8515625, c3 = 18.6875;
    const float3 y = pow(saturate(nits / 10000.0), m1);
    return pow((c1 + c2 * y) / (1.0 + c3 * y), m2);
}

float4 main_ps_pq(float4 pos : SV_Position) : SV_Target {
    const float3 c = gSource.Load(int3(pos.xy, 0)).rgb;
    const float3 lin = srgb_to_linear(c) * gScale.x * 80.0;
    const float3x3 to2020 = float3x3(0.6274040, 0.3292820, 0.0433136,
                                     0.0690970, 0.9195400, 0.0113612,
                                     0.0163916, 0.0880132, 0.8955950);
    return float4(pq(max(mul(to2020, lin), 0.0)), 1.0);
}
