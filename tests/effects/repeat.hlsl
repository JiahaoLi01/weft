// Texture 0 sampled with effectRepeat at twice the box's uv, half a texel to the right: under a box twice the size of a
// 16 x 16 texture each pixel filters the two texels beside it equally, and the second tile and the last texel's
// neighbor wrap to the first.
float4 effect(EffectInput input) {
    return effectTexture0.Sample(effectRepeat, input.uv * 2 + float2(.5 / 16, 0));
}
