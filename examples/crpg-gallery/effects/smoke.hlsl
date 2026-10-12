// The CRPG UI's smoke background (src/components/SmokeBg.tsx of the captured UI) as the page shows it: a canvas of
// canvas.x by canvas.y pixels stretched over the box with bilinear filtering, each canvas pixel the rarity color at an
// alpha from a scrolling cloud texel and a scrolling noise texel (nearest, r), shaped by a gamma table and faded along
// an axis. The canvas works in bytes: the gamma table, the noise product and the fade each truncate, as there.
//
// Textures: the cloud and the noise (coverage, wrapped). Parameters, in manifest order:
//   smoke-scales: cloud scale, noise scale (texture repeats across the canvas), gamma, noise strength
//   smoke-speeds: cloud x, cloud y, noise x, noise y, in the original's units
//   smoke-shape: fade power, fade from the right (1) instead of the top (0), texture axes turned 90 degrees (1)
//   smoke-canvas: width and height in canvas pixels
//   smoke-color: the rarity color
//   smoke-start: the effect time, in seconds, at which the original's clock starts (its mount)

static const float SmokeNoiseFloor = .01;

// A canvas pixel's alpha byte at time t.
float smokeAlpha(int2 c, float t, float4 scales, float4 speeds, float3 shape, float2 canvas) {
    uint cw, ch, nw, nh;
    effectTexture0.GetDimensions(cw, ch);
    effectTexture1.GetDimensions(nw, nh);
    float2 cloudSize = float2(cw, ch);
    float2 noiseSize = float2(nw, nh);
    float cloudPerUnit = cloudSize.x / scales.x;
    float noisePerUnit = noiseSize.x / scales.y;
    bool turned = shape.z > .5;
    // Offsets along the texture's u and v.
    float2 cloudOffset = t * speeds.xy * cloudPerUnit / canvas;
    float2 noiseOffset = t * speeds.zw * noisePerUnit / canvas;
    float2 position = float2(c) / canvas;
    // Screen columns feed u and rows v, or the other way round when turned.
    float2 cloud = (turned ? position.yx : position) * cloudPerUnit + cloudOffset;
    float2 noise = (turned ? position.yx : position) * noisePerUnit + noiseOffset;
    // The canvas's positive modulo, truncated: floor(((x % size) + size) % size).
    int2 cloudTexel = int2(floor(cloud - floor(cloud / cloudSize) * cloudSize));
    int2 noiseTexel = int2(floor(noise - floor(noise / noiseSize) * noiseSize));
    float byte = round(effectTexture0.Load(int3(min(cloudTexel, int2(cw, ch) - 1), 0)).r * 255);
    float lum = floor(pow(saturate(byte / 255), scales.z) * 255);
    if (scales.w > SmokeNoiseFloor) {
        float n = round(effectTexture1.Load(int3(min(noiseTexel, int2(nw, nh) - 1), 0)).r * 255) / 255;
        lum = floor(lum * (1 - scales.w + scales.w * n));
    }
    float fade = shape.y > .5 ? pow(saturate(c.x / canvas.x), shape.x) : pow(saturate(1 - c.y / canvas.y), shape.x);
    return floor(lum * fade);
}

float4 effect(EffectInput input) {
    float4 scales = input.parameters[0];
    float4 speeds = input.parameters[1];
    float3 shape = input.parameters[2].xyz;
    float2 canvas = max(floor(input.parameters[3].xy), 1);
    float3 color = input.parameters[4].rgb;
    float t = max(input.time - input.parameters[5].x, 0);
    // Bilinear between the four canvas pixels around the sample, edges clamped, as the page scales the canvas.
    float2 at = input.uv * canvas - .5;
    float2 base = floor(at);
    float2 f = at - base;
    int2 last = int2(canvas) - 1;
    int2 c0 = clamp(int2(base), 0, last);
    int2 c1 = clamp(int2(base) + 1, 0, last);
    float a00 = smokeAlpha(int2(c0.x, c0.y), t, scales, speeds, shape, canvas);
    float a10 = smokeAlpha(int2(c1.x, c0.y), t, scales, speeds, shape, canvas);
    float a01 = smokeAlpha(int2(c0.x, c1.y), t, scales, speeds, shape, canvas);
    float a11 = smokeAlpha(int2(c1.x, c1.y), t, scales, speeds, shape, canvas);
    float alpha = lerp(lerp(a00, a10, f.x), lerp(a01, a11, f.x), f.y) / 255;
    return float4(color * alpha, alpha);
}
