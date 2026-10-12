// Parameter 0, a straight linear color, premultiplied: drawn as a box's background would be.
float4 effect(EffectInput input) {
    float4 c = input.parameters[0];
    return float4(c.rgb * c.a, c.a);
}
