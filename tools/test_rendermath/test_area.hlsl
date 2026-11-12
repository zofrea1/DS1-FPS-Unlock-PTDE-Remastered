// test_area's vertex shader: clip-space position and the uv the game's copy draws give its pixel shader (TEXCOORD1).
struct VsOut {
    float4 pos : POSITION;
    float2 uv : TEXCOORD1;
};

VsOut main(float4 pos : POSITION, float2 uv : TEXCOORD1) {
    VsOut o;
    o.pos = pos;
    o.uv = uv;
    return o;
}
