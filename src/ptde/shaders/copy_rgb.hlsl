// The game's camera blur (FRPG_Fil_CameraBlur, ps_da76a458) with all its taps weighted 0 (c38.yzw and c39 zero, as
// while the camera is still): its result is the centre texel as it is, so this draws that with one read instead of
// ten. Same input (TEXCOORD1, stage 0) and the same colour; alpha 1 as the game's writes (its colour writes leave
// alpha alone anyway).
//
// Built with fxc (tools/build_shaders.bat) into copy_rgb_ps.h.

sampler2D gSrc : register(s0);

float4 main(float2 uv : TEXCOORD1) : COLOR0 {
    return float4(tex2D(gSrc, uv).rgb, 1.0);
}
