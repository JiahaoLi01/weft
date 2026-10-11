// Encoded premultiplied intermediate matches the existing D2D/Chrome scene
// contract; the shared conversion pass produces the engine's linear target.

// paint::GradientKind values (src/paint/display_list.hpp).
static const float GradientLinear = 1;
static const float GradientRadial = 2;
// Paint::extra.z / extra.w flags written by scene_renderer.cpp.
static const float FlagEnabled = 1;
static const uint MaxGradientStops = 8;
static const uint StopsPerOffsetVector = 4;
static const uint QuadCorners = 4;

static const float DegreesToRadians = .0174532925199433;
static const float TwoPi = 6.283185307179586;
static const float EncodedChannelMax = 255;
static const float MinimumAntialiasWidth = .0001;
static const float MinimumStopSpan = 1e-10;
static const float WedgeEdgeEpsilon = .0001;
static const float EqualBorderWidthEpsilon = .00001;

struct Paint {
    float4 bounds;
    float4 invX;
    float4 invY;
    float4 size;
    float4 clip;
    float4 radii;
    float4 widths;
    float4 background;
    float4 edges[4];
    float4 tile;
    float4 extra;
    float4 stops[8];
    float4 offsets[2];
};

StructuredBuffer<Paint> paints : register(t0);
Texture2D image : register(t1);
SamplerState linearClamp : register(s0);

cbuffer View : register(b0) {
    float2 target;
    float scale;
    float baseInstance;
};

struct Vertex {
    float4 position : SV_Position;
    nointerpolation uint index : TEXCOORD0;
};

Vertex vs_main(uint vertex : SV_VertexID, uint index : SV_InstanceID) {
    const float2 corners[6] = {float2(0, 0), float2(1, 0), float2(0, 1), float2(0, 1), float2(1, 0), float2(1, 1)};
    index += (uint)baseInstance;
    Paint p = paints[index];
    float2 position = (p.bounds.xy + corners[vertex] * p.bounds.zw) * scale;
    Vertex v;
    v.position = float4(position / target * float2(2, -2) + float2(-1, 1), 0, 1);
    v.index = index;
    return v;
}

float distanceRound(float2 position, float2 size, float4 radii) {
    // The four corner arcs have the same normalized circular geometry as the
    // full reference, including nonuniform radii and negative/skew transforms.
    if (position.x < radii.x && position.y < radii.x) {
        return length(position - radii.xx) - radii.x;
    }
    if (position.x > size.x - radii.y && position.y < radii.y) {
        return length(position - float2(size.x - radii.y, radii.y)) - radii.y;
    }
    if (position.x > size.x - radii.z && position.y > size.y - radii.z) {
        return length(position - (size - radii.zz)) - radii.z;
    }
    if (position.x < radii.w && position.y > size.y - radii.w) {
        return length(position - float2(radii.w, size.y - radii.w)) - radii.w;
    }
    float2 q = abs(position - size * .5) - size * .5;
    return max(q.x, q.y);
}

// The reference producer quantizes its encoded brush before source-over.
// Keep that order: low alpha (for example gold/10) otherwise crosses an
// entire flat region's 8-bit foreground boundary before the final conversion.
float4 premult(float4 c) {
    return round(float4(c.rgb * c.a, c.a) * EncodedChannelMax) / EncodedChannelMax;
}

float4 over(float4 a, float4 b) {
    return a + b * (1 - a.a);
}

float offset(Paint p, uint i) {
    return p.offsets[i / StopsPerOffsetVector][i % StopsPerOffsetVector];
}

float4 gradient(Paint p, float2 position) {
    float2 q = position - p.tile.zw;
    q = q - floor(q / p.tile.xy) * p.tile.xy;
    float angle = p.extra.x * DegreesToRadians;
    float2 d = float2(sin(angle), -cos(angle));
    float extent = dot(p.tile.xy, abs(d));
    float t;
    if (p.size.z == GradientLinear) {
        t = dot(q - p.tile.xy * .5, d) / extent + .5;
    } else if (p.size.z == GradientRadial) {
        t = length(q - p.tile.xy * .5) / length(p.tile.xy * .5);
    } else {
        t = (atan2(q.x - p.tile.x * .5, -(q.y - p.tile.y * .5)) - angle) / TwoPi;
        t = frac(t + 2);
    }
    uint count = (uint)p.size.w;
    uint i = 1;
    if (t <= offset(p, 0)) {
        return premult(p.stops[0]);
    }
    if (t >= offset(p, count - 1)) {
        return premult(p.stops[count - 1]);
    }
    [unroll]
    for (uint k = 1; k < MaxGradientStops; k++) {
        if (k < count && offset(p, k) <= t) {
            i = k + 1;
        }
    }
    return lerp(premult(p.stops[i - 1]), premult(p.stops[i]),
                saturate((t - offset(p, i - 1)) / max(MinimumStopSpan, offset(p, i) - offset(p, i - 1))));
}

float cross2(float2 a, float2 b) {
    return a.x * b.y - a.y * b.x;
}

bool wedge(float2 position, float2 a, float2 b, float2 c, float2 d) {
    float4 q = float4(cross2(b - a, position - a), cross2(c - b, position - b), cross2(d - c, position - c),
                      cross2(a - d, position - d));
    return all(q >= -WedgeEdgeEpsilon) || all(q <= WedgeEdgeEpsilon);
}

float4 ps_main(Vertex v) : SV_Target {
    Paint p = paints[v.index];
    float2 world = v.position.xy / scale;
    if (world.x < p.clip.x || world.y < p.clip.y || world.x >= p.clip.z || world.y >= p.clip.w) {
        discard;
    }
    float2 position = float2(dot(p.invX.xyz, float3(world, 1)), dot(p.invY.xyz, float3(world, 1)));
    if (p.extra.z == FlagEnabled) {
        if (any(position < 0) || any(position >= p.size.xy)) {
            discard;
        }
        float4 sample = image.Sample(linearClamp, position / p.size.xy);
        return (p.extra.w == FlagEnabled ? float4(p.background.rgb * p.background.a, p.background.a) * sample.a
                                         : sample) *
               p.extra.y;
    }
    float sd = distanceRound(position, p.size.xy, p.radii);
    float aa = max(fwidth(sd), MinimumAntialiasWidth);
    float outer = saturate(.5 - sd / aa);
    float4 result = premult(p.background);
    if (p.size.z > 0) {
        result = over(gradient(p, position), result);
    }
    float2 corners[4] = {float2(0, 0), float2(p.size.x, 0), p.size.xy, float2(0, p.size.y)};
    float2 inner[4] = {float2(p.widths.w, p.widths.x), float2(p.size.x - p.widths.y, p.widths.x),
                       float2(p.size.x - p.widths.y, p.size.y - p.widths.z), float2(p.widths.w, p.size.y - p.widths.z)};
    bool equal = all(abs(p.widths - p.widths.x) < EqualBorderWidthEpsilon);
    [unroll]
    for (uint i = 0; i < QuadCorners; i++) {
        if (p.widths[i] > 0 && wedge(position, equal ? p.size.xy * .5 : inner[i], corners[i],
                                     corners[(i + 1) % QuadCorners],
                                     equal ? p.size.xy * .5 : inner[(i + 1) % QuadCorners])) {
            float coverage = saturate(.5 + (sd + p.widths[i]) / aa);
            result = over(premult(p.edges[i]) * coverage, result);
            break;
        }
    }
    return result * outer * p.extra.y;
}
