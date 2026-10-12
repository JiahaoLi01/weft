// Refused: a global without static, a uniform value of a constant buffer of its own.
float4 tint;

float4 effect(EffectInput input) {
    return tint;
}
