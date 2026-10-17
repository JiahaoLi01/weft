// What lay behind the box, read shift CSS px away along the box's own axes: at an integer number of device px, the
// copy's texels exactly, so that the drawn box is the backdrop moved by -shift.
float4 effect(EffectInput input) {
    float2 shift = input.parameters[0].xy;
    return effectBackdrop(input, effectBackdropUV(input, input.uv + shift / input.size));
}
