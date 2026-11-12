// The game's low-resolution effects at the render resolution (FullResolutionEffects). The game draws its soft
// effects (glows, mist, fog, smoke) into a 512x360 target over a 512x360 copy of the depth, then lays that over the
// scene with FRPG_Fil_Bilateral (four depth-weighted reads, blend ONE + SRCALPHA: the scene times what shows through,
// plus the effects). At the original 1024x720 that was a 2x upscale; at 4K it is 7.5x6, and both the effects and the
// depth they are tested against come out in blocks (a glow cut off in steps where grass or a wall crosses it, fog
// ending in hard edges). The DLL draws those effects into a target of the scene's size instead, tested against the
// scene's own depth, and this shader lays it over the scene pixel for pixel, with the game's blend.
//
// The DLL makes that target 16-bit float where the graphics card can blend into one: layered fog then adds up
// without the rounding to 8 bits after every layer. The one rounding left, into the 8-bit scene, gets an ordered
// dither of less than half a step, so fog thins out without bands. A pixel the effects leave alone keeps its exact
// value (an 8-bit value plus less than half a step rounds back to itself).
//
// Built with fxc (tools/build_shaders.bat) into effects_composite_ps.h; the DLL carries the compiled bytecode. The
// DLL sets stage 0 to point sampling and clamping for the draw.

sampler2D gEffects : register(s0);  // the effects at the scene's size: rgb the effects, a what of the scene shows
float4 gDither : register(c200);    // x: the dither's size in 8-bit steps (1/255 for a float target, 0 otherwise)

// The 4x4 ordered (Bayer) dither index of a pixel, 0 to 15.
float bayer4(float2 p) {
    float2 a = fmod(p, 2.0);
    float2 b = fmod(floor(p * 0.5), 2.0);
    return 4.0 * (abs(a.x - a.y) * 2.0 + a.y) + (abs(b.x - b.y) * 2.0 + b.y);
}

float4 main(float2 uv : TEXCOORD1, float2 pos : VPOS) : COLOR0 {
    float4 c = tex2Dlod(gEffects, float4(uv, 0, 0));
    c.rgb += ((bayer4(floor(pos)) + 0.5) / 16.0 - 0.5) * gDither.x;
    return c;
}
