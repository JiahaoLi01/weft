// The color texture's texel at the pixel scaled by the data texture's, texel per CSS px: texture 0 is read as data
// (r), texture 1 as linear premultiplied color.
float4 effect(EffectInput input) {
    int3 texel = int3(floor(input.position), 0);
    return effectTexture1.Load(texel) * effectTexture0.Load(texel).r;
}
