// FPS Unlock Settings in Remastered (settingsmenu.cpp): a picture drawn over the frame, pixel for pixel. The quad comes
// from the vertex number (a strip of four, no vertex buffer); g_rect is its corners in clip space (x0, y0, x1, y1).
cbuffer Rect : register(b0) {
    float4 g_rect;
};
Texture2D g_tex : register(t0);
SamplerState g_point : register(s0);

struct VsOut {
    float4 pos : SV_Position;
    float2 uv : TEXCOORD0;
};

VsOut vs_main(uint id : SV_VertexID) {
    const float2 t = float2(id & 1, id >> 1);
    VsOut o;
    o.pos = float4(lerp(g_rect.x, g_rect.z, t.x), lerp(g_rect.y, g_rect.w, t.y), 0.0, 1.0);
    o.uv = t;
    return o;
}

float4 ps_main(VsOut i) : SV_Target {
    return g_tex.Sample(g_point, i.uv);
}
