// Refused: a sampler of its own.
SamplerState nearest;

float4 effect(EffectInput input) {
    return effectTexture0.Sample(nearest, input.uv);
}
