// An effect that CSS attaches: the tint (straight linear color) at amount times the mask under the pixel, the mask
// repeated every 16 CSS px and moved by offset, and times the fraction of the time's second; premultiplied.
float4 effect(EffectInput input) {
    float4 tint = input.parameters[0];
    float amount = input.parameters[1].x;
    float2 offset = input.parameters[2].xy;
    float mask = effectTexture0.Sample(effectRepeat, (input.position - offset) / 16).r;
    float alpha = saturate(tint.a * amount * mask * frac(input.time));
    return float4(tint.rgb * alpha, alpha);
}
