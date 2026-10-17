// Heat haze (docs/effects.md): what lies behind the box, wavering as air over heat does. Ripples wavelength CSS px
// apart rise at speed CSS px per second and move the view by up to amplitude CSS px; the haze is strongest at the box's
// bottom and fades by fade (0 to 1) toward its top, and it eases to nothing within a wavelength of the box's edges, so
// that the box shows no edge. A function of the position and the time alone: the same time draws the same picture.
// Parameters: amplitude, wavelength, speed, fade.

float4 effect(EffectInput input) {
    float amplitude = input.parameters[0].x;
    float wavelength = max(input.parameters[1].x, 1);
    float speed = input.parameters[2].x;
    float fade = saturate(input.parameters[3].x);
    float2 p = input.position / wavelength;
    float rise = input.time * speed / wavelength;
    // Two rising ripples across at frequencies that never line up, and a slower one down, each bent by the other axis.
    float across = .6 * sin(TwoPi * (p.y + rise) + 1.7 * sin(TwoPi * .37 * p.x)) +
                   .4 * sin(TwoPi * (1.73 * p.y + 1.31 * rise + .5 * p.x));
    float down = .5 * sin(TwoPi * (.83 * p.x + .5 * rise) + 2.1 * cos(TwoPi * .29 * p.y));
    float2 edge = min(input.position, input.size - input.position) / wavelength;
    float ease = smoothstep(0, 1, saturate(min(edge.x, edge.y)));
    float2 offset = float2(across, down) * amplitude * (1 - fade * (1 - input.uv.y)) * ease;
    return effectBackdrop(input, effectBackdropUV(input, input.uv + offset / input.size));
}
