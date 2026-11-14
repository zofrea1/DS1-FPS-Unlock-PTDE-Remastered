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
