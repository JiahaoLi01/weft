// The box's uv as red and green, opaque: each pixel shows where in the box the effect ran.
float4 effect(EffectInput input) {
    return float4(input.uv, 0, 1);
}
