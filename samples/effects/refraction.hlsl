// Refraction (docs/effects.md): what lies behind the box seen through a pane of glass. Within bevel CSS px of each
// edge the glass curves away, and the view through it bends toward the box's middle by up to strength CSS px, most at
// the edge (strength times the square of how near the edge the pixel lies); the pane's surface also ripples in a
// smooth wave of cell CSS px, which moves the view by ripple times strength. tint (a CSS color) lies over the view.
// Parameters: strength, bevel, ripple, cell, tint. Its manifest's backdrop margin covers the ripple's reach past the
// box; reads past the margin take the copy's nearest texel.

// How far the view bends at depth px inside one edge of a bevel bevel px wide, as a fraction of strength.
float bend(float depth, float bevel) {
    float t = saturate(1 - depth / max(bevel, 1e-3));
    return t * t;
}

float4 effect(EffectInput input) {
    float strength = input.parameters[0].x;
    float bevel = input.parameters[1].x;
    float ripple = input.parameters[2].x;
    float cell = max(input.parameters[3].x, 1);
    float4 tint = input.parameters[4];
    float2 p = input.position;
    float2 far = input.size - p;
    // Toward the middle: right near the left edge, left near the right one, and so on.
    float2 offset = float2(bend(p.x, bevel) - bend(far.x, bevel), bend(p.y, bevel) - bend(far.y, bevel)) * strength;
    float2 q = p / cell * TwoPi;
    offset += float2(sin(q.y + .6 * sin(.7 * q.x)), sin(.9 * q.x + .6 * cos(.8 * q.y))) * ripple * strength;
    float4 view = effectBackdrop(input, effectBackdropUV(input, input.uv + offset / input.size));
    // The tint, straight linear color, over the view.
    return float4(tint.rgb * tint.a, tint.a) + view * (1 - tint.a);
}
