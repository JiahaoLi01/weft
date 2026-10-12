// Refused: a register binding, even on a static value.
static float4 tint : register(c0);

float4 effect(EffectInput input) {
    return tint;
}
