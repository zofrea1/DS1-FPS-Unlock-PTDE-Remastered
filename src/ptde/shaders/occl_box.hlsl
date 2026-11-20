// OcclusionCulling's test boxes (occl.cpp): a map piece's world box, made relative to the camera on the CPU, drawn with
// the game's own camera-relative view-projection (c0..c3, as the game's scene shaders take it: clip = dot(cN, (p, 1)))
// against the scene's depth, colour writes off, inside an occlusion query.
//
// Built with fxc (tools/build_shaders.bat) into occl_box_vs.h and occl_box_ps.h.

float4 gRow0 : register(c0);
float4 gRow1 : register(c1);
float4 gRow2 : register(c2);
float4 gRow3 : register(c3);

float4 main_vs(float3 p : POSITION) : POSITION {
    const float4 q = float4(p, 1.0);
    return float4(dot(gRow0, q), dot(gRow1, q), dot(gRow2, q), dot(gRow3, q));
}

float4 main_ps() : COLOR0 {
    return float4(0.0, 0.0, 0.0, 1.0);
}
