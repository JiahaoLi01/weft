// Refused: a texture of its own.
Texture2D<float4> noise;

float4 effect(EffectInput input) {
    return noise.Load(int3(0, 0, 0));
}
