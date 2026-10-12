// The shared pass of display lists (src/render/scene_pass.hpp). A scene pass composites encoded premultiplied
// colors, the existing D2D/Chrome scene contract, and the shared conversion pass produces the engine's linear
// target. The SPACE_LINEAR variant is a linear pass: it gets linear premultiplied colors from the CPU and
// composites them unquantized. A compile-time variant keeps the scene pass's code exactly as it was.

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
// Paint::mode.x of images: a scene raster (premultiplied, or a mask with Paint::extra.w), a coverage mask tinted with
// Paint::background, or signed distances.
static const float ImageModeCoverage = 1;
static const float ImageModeSignedDistance = 2;
// FreeType encodes contour zero at byte 128, not normalized 0.5.
static const float SdfByteMax = 255.0;
static const float SdfZeroByte = 128.0;
static const float MinimumRadius = 1e-6;

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
    // Image origin and extent in its asset.
    float4 uv;
    // x: image mode, y: distance spread and z: stroke, in source texels; w: source texels per device pixel.
    float4 mode;
    float4 outline;
    // x: first rounded clip, y: rounded clip count.
    float4 clips;
};

// A rounded clip in CSS px: x, y, width, height, and corner radii TL, TR, BR, BL.
struct Clip {
    float4 rect;
    float4 radiusX;
    float4 radiusY;
    // Rows of the map from target px into the clip's space; invX.w is target px per unit of that space.
    float4 invX;
    float4 invY;
};

StructuredBuffer<Paint> paints : register(t0);
Texture2D image : register(t1);
StructuredBuffer<Clip> clips : register(t2);
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
// Halves round up, as the producer's std::round does (HLSL's round goes to even:
// alpha .7 would quantize to 178 rather than 179).
// A linear pass's colors arrive linear and premultiplied from the CPU, unquantized.
float4 premult(float4 c) {
#if SPACE_LINEAR
    return c;
#else
    return floor(float4(c.rgb * c.a, c.a) * EncodedChannelMax + .5) / EncodedChannelMax;
#endif
}

// Coverage of target (CSS px) by one rounded clip, antialiased across a device pixel: in the clip's own space, the
// distance to the rectangle, or inside a corner's box the first-order distance to its ellipse.
float clipCoverage(Clip c, float2 target) {
    float2 world = float2(dot(c.invX.xyz, float3(target, 1)), dot(c.invY.xyz, float3(target, 1)));
    float2 lo = c.rect.xy;
    float2 hi = c.rect.xy + c.rect.zw;
    float2 q = max(lo - world, world - hi);
    float d = max(q.x, q.y);
    float2 radius = float2(0, 0);
    float2 center = float2(0, 0);
    if (world.x < lo.x + c.radiusX.x && world.y < lo.y + c.radiusY.x) {
        radius = float2(c.radiusX.x, c.radiusY.x);
        center = lo + radius;
    } else if (world.x > hi.x - c.radiusX.y && world.y < lo.y + c.radiusY.y) {
        radius = float2(c.radiusX.y, c.radiusY.y);
        center = float2(hi.x - radius.x, lo.y + radius.y);
    } else if (world.x > hi.x - c.radiusX.z && world.y > hi.y - c.radiusY.z) {
        radius = float2(c.radiusX.z, c.radiusY.z);
        center = hi - radius;
    } else if (world.x < lo.x + c.radiusX.w && world.y > hi.y - c.radiusY.w) {
        radius = float2(c.radiusX.w, c.radiusY.w);
        center = float2(lo.x + radius.x, hi.y - radius.y);
    }
    if (radius.x > MinimumRadius && radius.y > MinimumRadius) {
        float2 n = (world - center) / radius;
        float k = length(n);
        float gradient = length((world - center) / (radius * radius)) / max(k, MinimumRadius);
        d = max(d, (k - 1) / max(gradient, MinimumRadius));
    }
    return saturate(.5 - d * c.invX.w * scale);
}

float clipChain(Paint p, float2 world) {
    float coverage = 1;
    for (uint i = 0; i < (uint)p.clips.y; i++) {
        coverage *= clipCoverage(clips[(uint)p.clips.x + i], world);
    }
    return coverage;
}

// How fast v changes across a pixel. A linear pass takes fine derivatives, which every backend computes alike: Vulkan
// leaves fwidth coarse or fine, and an RTX 5090 widened distance edges by up to 18 levels against WARP.
float pixelWidth(float v) {
#if SPACE_LINEAR
    return abs(ddx_fine(v)) + abs(ddy_fine(v));
#else
    return fwidth(v);
#endif
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
#if SPACE_LINEAR
    // A linear pass draws transparent black outside its clip and its image instead of discarding: premultiplied
    // source-over leaves the target unchanged, and neighbouring pixels keep defined derivatives for their
    // antialiasing.
    float coverage = world.x < p.clip.x || world.y < p.clip.y || world.x >= p.clip.z || world.y >= p.clip.w
                         ? 0
                         : clipChain(p, world);
#else
    if (world.x < p.clip.x || world.y < p.clip.y || world.x >= p.clip.z || world.y >= p.clip.w) {
        discard;
    }
    float coverage = clipChain(p, world);
#endif
    float2 position = float2(dot(p.invX.xyz, float3(world, 1)), dot(p.invY.xyz, float3(world, 1)));
    if (p.extra.z == FlagEnabled) {
#if SPACE_LINEAR
        coverage = any(position < 0) || any(position >= p.size.xy) ? 0 : coverage;
#else
        if (any(position < 0) || any(position >= p.size.xy)) {
            discard;
        }
#endif
        // precise keeps the division a scene raster has always sampled at: its uv is the identity.
        precise float2 local = position / p.size.xy;
        float2 uv = p.uv.xy + local * p.uv.zw;
        if (p.mode.x == ImageModeSignedDistance) {
            // Distance is linear data; no sRGB transfer, no coverage interpretation.
            float d = (image.SampleLevel(linearClamp, uv, 0).r * SdfByteMax - SdfZeroByte) * p.mode.y / SdfZeroByte;
#if SPACE_LINEAR
            // The distance changes one source texel per texel, so its edge spans the texels of one device pixel (mode.w,
            // from the item's mapping): the same on every backend, unlike derivatives of sampled distances, which jump
            // across the plateaus of 8-bit distances.
            float aa = max(p.mode.w, MinimumAntialiasWidth);
#else
            float aa = max(pixelWidth(d), MinimumAntialiasWidth);
#endif
            float fill = saturate(.5 + d / aa);
            float outer = saturate(.5 + (d + p.mode.z) / aa);
            return (premult(p.background) * fill + premult(p.outline) * (outer - fill)) * p.extra.y * coverage;
        }
        float4 sample = image.Sample(linearClamp, uv);
        if (p.mode.x == ImageModeCoverage) {
            return premult(p.background) * sample.r * p.extra.y * coverage;
        }
        // A tinted mask (scene text, paths) premultiplies its color as a box does: quantized, as the reference
        // producer's brush.
        return (p.extra.w == FlagEnabled ? premult(p.background) * sample.a : sample) * p.extra.y * coverage;
    }
    float sd = distanceRound(position, p.size.xy, p.radii);
    float aa = max(pixelWidth(sd), MinimumAntialiasWidth);
    float outer = saturate(.5 - sd / aa);
#if SPACE_LINEAR
    // A document box without corners or rotation covers the pixels whose centers it contains, as the rasterizer
    // covers a quad, rather than a distance band that leaks past its corners.
    if (all(p.radii == 0) && p.invX.y == 0 && p.invY.x == 0) {
        outer = all(position >= 0) && all(position < p.size.xy) ? 1 : 0;
    }
#endif
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
    return result * outer * p.extra.y * coverage;
}
