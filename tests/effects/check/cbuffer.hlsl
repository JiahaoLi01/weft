// Refused: a constant buffer of its own.
cbuffer Tint {
    float4 tint;
};

float4 effect(EffectInput input) {
    return tint;
}
