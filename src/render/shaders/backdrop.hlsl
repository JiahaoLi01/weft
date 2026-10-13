// The passes that blur a backdrop (the scene pass's Step::Backdrop, src/render/scene_pass.hpp), each a fullscreen
// triangle over its target sampling its source, with addresses past the source's edges mirrored back into it (Chrome's
// backdrop-filter edge mode). As Skia blurs on the GPU: for a sigma above four target px, the copy is first scaled down
// to the size that brings it to four, here by area across and then down (BACKDROP_AREA: each texel the mean of the
// source texels its span covers); then a horizontal and a vertical pass of a Gaussian kernel three sigmas wide
// (BACKDROP_BLUR); the scene pass's composite upsamples the result bilinearly. A color matrix the filter list applies
// before its blur runs first (BACKDROP_MATRIX), on straight colors.

cbuffer BackdropPass : register(b0) {
    // A blur's: xy the source's texel size, in uv; zw the step between taps, in uv (its direction). An area
    // downsample's: x source texels per target texel, y 1 across (0 down), z the source's texels on the axis.
    float4 texel;
    // The kernel's weights: weights[i / 4][i % 4] for tap i from the centre, normalized over both sides.
    float4 weights[4];
    // x: taps on each side of the centre.
    float4 extent;
    // A 4 x 5 color matrix on straight RGBA: rows of r, g, b and a, each with its offset in the fifth column.
    float4 rows[4];
    float4 offsets;
};

Texture2D source : register(t0);
SamplerState mirror : register(s0);

struct Fullscreen {
    float4 position : SV_Position;
    float2 uv : TEXCOORD0;
};

Fullscreen vs_main(uint vertex : SV_VertexID) {
    Fullscreen v;
    v.uv = float2((vertex << 1) & 2, vertex & 2);
    v.position = float4(v.uv * float2(2, -2) + float2(-1, 1), 0, 1);
    return v;
}

#if BACKDROP_AREA
float4 ps_main(Fullscreen v) : SV_Target {
    // The span [start, end) of source texels the target texel covers on the axis, each texel weighted by its share.
    const bool across = texel.y > .5;
    const int2 at = int2(v.position.xy);
    const float start = (across ? at.x : at.y) * texel.x;
    const float end = start + texel.x;
    const int last = min((int)ceil(end), (int)texel.z);
    float4 sum = 0;
    for (int i = (int)floor(start); i < last; i++) {
        const float w = min(end, i + 1.0) - max(start, (float)i);
        sum += source.Load(int3(across ? int2(i, at.y) : int2(at.x, i), 0)) * w;
    }
    return sum / texel.x;
}
#elif BACKDROP_BLUR
float4 ps_main(Fullscreen v) : SV_Target {
    float4 sum = source.SampleLevel(mirror, v.uv, 0) * weights[0].x;
    const uint taps = (uint)extent.x;
    for (uint i = 1; i <= taps; i++) {
        const float w = weights[i / 4][i % 4];
        sum += (source.SampleLevel(mirror, v.uv + texel.zw * i, 0) + source.SampleLevel(mirror, v.uv - texel.zw * i, 0)) *
               w;
    }
    return sum;
}
#elif BACKDROP_MATRIX
float4 ps_main(Fullscreen v) : SV_Target {
    const float4 c = source.SampleLevel(mirror, v.uv, 0);
    // Premultiplied in, straight through the matrix, premultiplied out.
    const float4 straight = c.a > 0 ? float4(c.rgb / c.a, c.a) : float4(0, 0, 0, 0);
    const float4 m = saturate(float4(dot(rows[0], straight), dot(rows[1], straight), dot(rows[2], straight),
                                     dot(rows[3], straight)) +
                              offsets);
    return float4(m.rgb * m.a, m.a);
}
#endif
