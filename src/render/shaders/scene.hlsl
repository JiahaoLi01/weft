// The shared pass of display lists (src/render/scene_pass.hpp). A scene pass composites encoded premultiplied
// colors, the existing D2D/Chrome scene contract, and the shared conversion pass produces the engine's linear
// target. The SPACE_LINEAR variant is a linear pass: it gets linear premultiplied colors from the CPU and
// composites them unquantized. A compile-time variant keeps the scene pass's code exactly as it was. The WEFT_EFFECT
// variant is the pixel stage of one UI shader effect (cmake/WeftEffects.cmake): its source, included after this file,
// defines effect(). The WEFT_PARTICLES variant is both stages of the particle pass: each vertex a corner of one
// particle's quad.

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
// Paint::background, signed distances, or a plane's content image seen through a homography: Paint::invX and invY
// then hold the first rows of the inverse homography and Paint::outline its third, the depth.
static const float ImageModeCoverage = 1;
static const float ImageModeSignedDistance = 2;
static const float ImageModeProjective = 3;
// Paint::mode.x of a backdrop: its blurred copy of the target, bound as image over the device px Paint::uv covers
// (origin and extent); Paint::extra.w enables its color matrix in Paint::stops[0..4].
static const float ImageModeBackdrop = 4;
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

#if !WEFT_PARTICLES
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
#endif

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

// How fast v changes across a pixel, from fine derivatives, which every backend computes alike: Vulkan leaves fwidth
// coarse or fine, and an RTX 5090 widened distance edges by up to 18 levels against WARP. Coarse ones also spread across
// a quad where a distance turns a corner (the max of two edges), dimming a box's corner pixel against Chrome's.
float pixelWidth(float v) {
    return abs(ddx_fine(v)) + abs(ddy_fine(v));
}

// The share of a pixel `footprint` across, centered at position, inside the rectangle from lo to hi.
float overlap(float2 position, float2 footprint, float2 lo, float2 hi) {
    float2 share = saturate((min(position + footprint * .5, hi) - max(position - footprint * .5, lo)) / footprint);
    return share.x * share.y;
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

#if WEFT_EFFECT || WEFT_PARTICLES
// The sRGB transfer, for effects that work with CSS colors: an encoded channel to linear light, and back. Scalar
// conditions: HLSL 2021 (DXC) takes no vector condition in ?:, and FXC has no select().
float effectLinear(float c) {
    return c <= .04045 ? c / 12.92 : pow((c + .055) / 1.055, 2.4);
}

float3 effectLinear(float3 c) {
    return float3(effectLinear(c.r), effectLinear(c.g), effectLinear(c.b));
}

float effectEncoded(float c) {
    return c <= .0031308 ? c * 12.92 : 1.055 * pow(c, 1 / 2.4) - .055;
}

float3 effectEncoded(float3 c) {
    return float3(effectEncoded(c.r), effectEncoded(c.g), effectEncoded(c.b));
}
#endif

#if WEFT_PARTICLES
// UI particles (docs/architecture.md, "粒子"): a record per emission (scene_pass.cpp; render::ParticleField names its
// fields), drawn as 6 vertices per particle. Each particle's state follows the shared model (src/paint/particles.cpp)
// in float: the same random values, start, motion, curves and frames.
Texture2D particleSprite : register(t3);

static const uint ParticleIndexPeriod = 16777216;
// A particle's random values by index (paint/particles.cpp's Draw), and how many the hash spaces per particle.
static const uint ParticleDraws = 16;
static const uint DrawLife = 0;
static const uint DrawStartU = 1;
static const uint DrawStartV = 2;
static const uint DrawSpeed = 3;
static const uint DrawDirection = 4;
static const uint DrawScale = 5;
static const uint DrawAngle = 6;
static const uint DrawSpin = 7;
static const uint DrawFrame = 8;
static const uint DrawEmission = 9;
static const float ParticleDragEpsilon = .0001;
// paint::ParticleEmitter::Shape.
static const float ShapeRect = 1;
static const float ShapeCircle = 2;
static const float ShapeEdge = 3;
// The sprite mode, render::ParticleSprite's w (constants::render::scene::particle_sprite_*).
static const float SpriteNone = 0;
static const float SpriteCoverage = 1;
// A disc's quad reaches this many device px past its rim, which antialiases across it.
static const float ParticleRimPixels = 1;
static const float2 OutsideTarget = float2(-2, -2);

struct ParticleVertex {
    float4 position : SV_Position;
    nointerpolation uint index : TEXCOORD0;
    // Across the particle: -1 to 1 over its size, beyond over a disc's rim.
    float2 local : TEXCOORD1;
    // Straight linear color.
    nointerpolation float4 color : TEXCOORD2;
    // The sprite frame's region of the bound texture: origin and extent.
    nointerpolation float4 frame : TEXCOORD3;
};

// lowbias32 (Chris Wellons), as paint/particles.cpp.
uint particleHash(uint x) {
    x ^= x >> 16;
    x *= 0x7feb352dU;
    x ^= x >> 15;
    x *= 0x846ca68bU;
    x ^= x >> 16;
    return x;
}

float particleRandom(uint seed, uint particle, uint k) {
    return (float)(particleHash(seed ^ particleHash(particle * ParticleDraws + k)) >> 8) * (1.0 / 16777216.0);
}

// keys at 0, 1/3, 2/3 and 1, linear between, at fraction (clamped to [0, 1]).
float particleCurve(float4 keys, float fraction) {
    float t = saturate(fraction) * 3;
    float i = min(floor(t), 2);
    float a = i < 1 ? keys.x : i < 2 ? keys.y : keys.z;
    float b = i < 1 ? keys.y : i < 2 ? keys.z : keys.w;
    return a + (b - a) * (t - i);
}

// Record fields (render::ParticleField): stops[0] and [1] the map into target px, [2] the timing, [3] the count,
// duration, seed and shape, [4] the area, [5] life and speed, [6] direction, spread, drag and softness, [7] gravity
// and spin; edges[0] angle and scale, [1] sizes, [2] and [3] then offsets[0] and [1] the colors; tile the sprite;
// background the target.
ParticleVertex vs_main(uint vertex : SV_VertexID, uint instance : SV_InstanceID) {
    const float2 corners[6] = {float2(-1, -1), float2(1, -1), float2(-1, 1), float2(-1, 1), float2(1, -1), float2(1, 1)};
    uint index = instance + (uint)baseInstance;
    Paint p = paints[index];
    float4 timing = p.stops[2];
    float4 emission = p.stops[3];
    float4 lifeSpeed = p.stops[5];
    float4 heading = p.stops[6];
    float4 forces = p.stops[7];
    float4 turn = p.edges[0];
    float4 sprite = p.tile;
    uint particle = vertex / 6;
    uint seed = (uint)emission.z;
    ParticleVertex v;
    v.index = index;
    v.local = corners[vertex % 6];
    v.color = float4(0, 0, 0, 0);
    v.frame = float4(0, 0, 0, 0);
    v.position = float4(OutsideTarget, 0, 1);
    // Its identity and age: a burst's particles by instance, a stream's newest first.
    uint id;
    float age;
    if (timing.w == FlagEnabled) {
        id = particle;
        age = timing.x - emission.y * particleRandom(seed, id, DrawEmission);
    } else {
        id = ((uint)timing.z + ParticleIndexPeriod - particle) % ParticleIndexPeriod;
        age = timing.x + (float)particle * timing.y;
    }
    float life = lerp(lifeSpeed.x, lifeSpeed.y, particleRandom(seed, id, DrawLife));
    if (!(age >= 0 && age < life)) {
        return v;
    }
    // Where it starts.
    float u = particleRandom(seed, id, DrawStartU);
    float w = particleRandom(seed, id, DrawStartV);
    float4 area = p.stops[4];
    float2 start = area.xy;
    if (emission.w == ShapeRect) {
        start = area.xy + area.zw * float2(u, w);
    } else if (emission.w == ShapeCircle) {
        float r = area.z * sqrt(w);
        float a = TwoPi * u;
        start = area.xy + r * float2(cos(a), sin(a));
    } else if (emission.w == ShapeEdge) {
        // Clockwise from the top left corner.
        float d = u * 2 * (area.z + area.w);
        if (d < area.z) {
            start = area.xy + float2(d, 0);
        } else if (d < area.z + area.w) {
            start = area.xy + float2(area.z, d - area.z);
        } else if (d < 2 * area.z + area.w) {
            start = area.xy + float2(area.z - (d - area.z - area.w), area.w);
        } else {
            start = area.xy + float2(0, area.w - (d - 2 * area.z - area.w));
        }
    }
    // How it moves.
    float speed = lerp(lifeSpeed.z, lifeSpeed.w, particleRandom(seed, id, DrawSpeed));
    float travel = (heading.x + (particleRandom(seed, id, DrawDirection) - .5) * heading.y) * DegreesToRadians;
    float2 velocity = speed * float2(cos(travel), sin(travel));
    float fraction = age / life;
    float2 position;
    if (p.background.z == FlagEnabled) {
        // To the target along a quadratic curve that leaves along the velocity, eased in and out.
        float f = fraction * fraction * (3 - 2 * fraction);
        float2 control = start + velocity * life * .5;
        position = (1 - f) * (1 - f) * start + 2 * (1 - f) * f * control + f * f * p.background.xy;
    } else if (heading.z > ParticleDragEpsilon) {
        float decay = (1 - exp(-heading.z * age)) / heading.z;
        position = start + velocity * decay + forces.xy * (age - decay) / heading.z;
    } else {
        position = start + velocity * age + .5 * forces.xy * age * age;
    }
    // How it looks.
    float size = particleCurve(p.edges[1], fraction) * lerp(turn.z, turn.w, particleRandom(seed, id, DrawScale));
    float4 color;
    color.r = particleCurve(float4(p.edges[2].r, p.edges[3].r, p.offsets[0].r, p.offsets[1].r), fraction);
    color.g = particleCurve(float4(p.edges[2].g, p.edges[3].g, p.offsets[0].g, p.offsets[1].g), fraction);
    color.b = particleCurve(float4(p.edges[2].b, p.edges[3].b, p.offsets[0].b, p.offsets[1].b), fraction);
    color.a = particleCurve(float4(p.edges[2].a, p.edges[3].a, p.offsets[0].a, p.offsets[1].a), fraction);
    float angle = (lerp(turn.x, turn.y, particleRandom(seed, id, DrawAngle)) +
                   lerp(forces.z, forces.w, particleRandom(seed, id, DrawSpin)) * age) *
                  DegreesToRadians;
    if (sprite.w != SpriteNone) {
        // Its frame, in integers: a GPU's float division need not round correctly.
        uint frames = (uint)sprite.x * (uint)sprite.y;
        uint pick = 0;
        if (frames > 1) {
            pick = (uint)(sprite.z > 0 ? floor(age * sprite.z) : floor(particleRandom(seed, id, DrawFrame) * frames)) %
                   frames;
        }
        float2 extent = p.uv.zw / sprite.xy;
        v.frame = float4(p.uv.xy + float2(pick % (uint)sprite.x, pick / (uint)sprite.x) * extent, extent);
    }
    if (!(size > 0)) {
        return v;
    }
    // The quad: the size across, turned by the angle, and for a disc a device px more around its rim.
    float3 mapX = p.stops[0].xyz;
    float3 mapY = p.stops[1].xyz;
    float stretch = sqrt(abs(mapX.x * mapY.y - mapX.y * mapY.x)) * scale;
    float half = size * .5;
    float reach = half + (sprite.w == SpriteNone ? ParticleRimPixels / max(stretch, MinimumRadius) : 0);
    float2 corner = corners[vertex % 6] * reach;
    v.local = corner / half;
    float c = cos(angle);
    float s = sin(angle);
    float2 item = position + float2(corner.x * c - corner.y * s, corner.x * s + corner.y * c);
    float2 world = float2(dot(mapX, float3(item, 1)), dot(mapY, float3(item, 1)));
    v.position = float4(world * scale / target * float2(2, -2) + float2(-1, 1), 0, 1);
    v.color = color;
    return v;
}

// A disc antialiased across a device px at its rim or softened inward over softness of its radius, or a sprite frame
// tinted by the color; clipped as an image, then the item's opacity.
float4 ps_main(ParticleVertex v) : SV_Target {
    Paint p = paints[v.index];
    float2 world = v.position.xy / scale;
#if SPACE_LINEAR
    float coverage = world.x < p.clip.x || world.y < p.clip.y || world.x >= p.clip.z || world.y >= p.clip.w
                         ? 0
                         : clipChain(p, world);
#else
    if (world.x < p.clip.x || world.y < p.clip.y || world.x >= p.clip.z || world.y >= p.clip.w) {
        discard;
    }
    float coverage = clipChain(p, world);
#endif
    float4 color = float4(v.color.rgb * v.color.a, v.color.a);
    if (p.tile.w == SpriteNone) {
        float d = length(v.local);
        float aa = max(pixelWidth(d), MinimumAntialiasWidth);
        float softness = p.stops[6].w;
        color *= softness > 0 ? saturate((1 - d) / max(softness, aa)) : saturate(.5 - (d - 1) / aa);
    } else {
        // Texels of the frame only, never its neighbours': half a texel in from its edges.
        float width;
        float height;
        particleSprite.GetDimensions(width, height);
        float2 texel = .5 / float2(width, height);
        float2 uv = clamp(v.frame.xy + (v.local * .5 + .5) * v.frame.zw, v.frame.xy + texel,
                          v.frame.xy + v.frame.zw - texel);
        float4 sample = particleSprite.Sample(linearClamp, uv);
        color *= p.tile.w == SpriteCoverage ? sample.r : sample;
    }
#if !SPACE_LINEAR
    // An encoded pass composites encoded premultiplied colors, unquantized.
    color = color.a > 0 ? float4(effectEncoded(saturate(color.rgb / color.a)) * color.a, color.a) : float4(0, 0, 0, 0);
#endif
    return color * p.extra.y * coverage;
}
#elif WEFT_EFFECT
// UI shader effects (docs/architecture.md, "UI 着色器效果、粒子与背景拷贝"). An effect reads only its EffectInput and
// the textures and sampler declared here; cmake/check_effect.cmake refuses sources that declare resources.
static const uint EffectParameters = 16;
// The effect's textures, in the order of paint::Effect::textures: coverage assets as data in r, sRGB images as linear
// premultiplied color. effectRepeat filters them linearly and repeats them.
Texture2D effectTexture0 : register(t3);
Texture2D effectTexture1 : register(t4);
Texture2D effectTexture2 : register(t5);
Texture2D effectTexture3 : register(t6);
SamplerState effectRepeat : register(s1);

// One pixel as an effect sees it: its uv across the item's box, the box's size and the pixel's position in the box in
// CSS px, the pixel's position in the target in device px, device px per CSS px, the effect's time in seconds and its
// parameters (absent ones zero).
struct EffectInput {
    float2 uv;
    float2 size;
    float2 position;
    float2 pixel;
    float scale;
    float time;
    float4 parameters[EffectParameters];
};

// The effect's linear premultiplied color at one pixel, defined by the effect's source.
float4 effect(EffectInput input);

// An effect record (scene_pass.cpp) is an image record whose texels the effect computes: clipped as an image, shaped
// by the box's corners as a box, with the time in extra.x and the parameters in the fields effects leave unused
// (render::effect_parameter).
float4 ps_main(Vertex v) : SV_Target {
    Paint p = paints[v.index];
    float2 world = v.position.xy / scale;
#if SPACE_LINEAR
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
    EffectInput input;
    input.uv = position / p.size.xy;
    input.size = p.size.xy;
    input.position = position;
    input.pixel = v.position.xy;
    input.scale = scale;
    input.time = p.extra.x;
    [unroll]
    for (uint i = 0; i < MaxGradientStops; i++) {
        input.parameters[i] = p.stops[i];
    }
    [unroll]
    for (uint e = 0; e < QuadCorners; e++) {
        input.parameters[MaxGradientStops + e] = p.edges[e];
    }
    input.parameters[12] = p.offsets[0];
    input.parameters[13] = p.offsets[1];
    input.parameters[14] = p.tile;
    input.parameters[15] = p.background;
    float4 color = effect(input);
    float sd = distanceRound(position, p.size.xy, p.radii);
    float aa = max(pixelWidth(sd), MinimumAntialiasWidth);
    float outer = saturate(.5 - sd / aa);
#if SPACE_LINEAR
    // As a document box's background: without corners or rotation it covers the pixels whose centers it contains.
    if (all(p.radii == 0) && p.invX.y == 0 && p.invY.x == 0) {
        outer = all(position >= 0) && all(position < p.size.xy) ? 1 : 0;
    }
#else
    // An encoded pass composites encoded premultiplied colors, unquantized.
    color = color.a > 0 ? float4(effectEncoded(saturate(color.rgb / color.a)) * color.a, color.a) : float4(0, 0, 0, 0);
#endif
    return color * outer * p.extra.y * coverage;
}
#else
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
    if (p.mode.x == ImageModeBackdrop) {
        // What lay behind the box, blurred, through its color matrix on straight values (rows of r, g, b and a in
        // stops[0..3], their offsets in stops[4]), clamped, in the box's shape.
        float4 c = image.SampleLevel(linearClamp, (v.position.xy - p.uv.xy) / p.uv.zw, 0);
        if (p.extra.w == FlagEnabled) {
            float4 s = c.a > 0 ? float4(c.rgb / c.a, c.a) : float4(0, 0, 0, 0);
            s = saturate(float4(dot(p.stops[0], s), dot(p.stops[1], s), dot(p.stops[2], s), dot(p.stops[3], s)) +
                         p.stops[4]);
            c = float4(s.rgb * s.a, s.a);
        }
        float sd = distanceRound(position, p.size.xy, p.radii);
        float shape = saturate(.5 - sd / max(pixelWidth(sd), MinimumAntialiasWidth));
        if (all(p.radii == 0) && p.invX.y == 0 && p.invY.x == 0) {
            shape = overlap(position, max(float2(abs(p.invX.x), abs(p.invY.y)) / scale, MinimumAntialiasWidth), 0,
                            p.size.xy);
        }
        return c * shape * p.extra.y * coverage;
    }
    if (p.mode.x == ImageModeProjective) {
        // Nothing behind the viewer.
        float depth = dot(p.outline.xyz, float3(world, 1));
        position = depth > 0 ? position / depth : float2(-1, -1);
    }
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
        // producer's brush. Its coverage is in r: an image of a byte per texel, or white content, whose premultiplied
        // channels all equal its alpha.
        return (p.extra.w == FlagEnabled ? premult(p.background) * sample.r : sample) * p.extra.y * coverage;
    }
    float sd = distanceRound(position, p.size.xy, p.radii);
    float aa = max(pixelWidth(sd), MinimumAntialiasWidth);
    float outer = saturate(.5 - sd / aa);
    bool rectangular = all(p.radii == 0) && p.invX.y == 0 && p.invY.x == 0;
    // A device pixel's extent in the box's px.
    float2 footprint = max(float2(abs(p.invX.x), abs(p.invY.y)) / scale, MinimumAntialiasWidth);
#if SPACE_LINEAR
    // A document box without corners or rotation covers the pixels whose centers it contains, as the rasterizer
    // covers a quad, rather than a distance band that leaks past its corners.
    if (rectangular) {
        outer = all(position >= 0) && all(position < p.size.xy) ? 1 : 0;
    }
#else
    // A scene box without corners or rotation covers its share of each pixel, as Chrome's and the reference raster's
    // rectangles do: a box on whole pixels stays crisp however small (a dot of a dotted border), where the distance
    // band of a box a pixel wide spreads it over its neighbours.
    if (rectangular) {
        outer = overlap(position, footprint, 0, p.size.xy);
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
    // A pixel whose center lies just outside the box (its antialiased edge) takes the side it is beside, under a
    // rotation or round corners too: otherwise it lies in no side's wedge and the edge loses its antialiasing.
    float2 sided = clamp(position, 0, p.size.xy);
    [unroll]
    for (uint i = 0; i < QuadCorners; i++) {
        if (p.widths[i] > 0 && wedge(sided, equal ? p.size.xy * .5 : inner[i], corners[i],
                                     corners[(i + 1) % QuadCorners],
                                     equal ? p.size.xy * .5 : inner[(i + 1) % QuadCorners])) {
            float coverage = saturate(.5 + (sd + p.widths[i]) / aa);
#if !SPACE_LINEAR
            // The share of the box's part of the pixel that lies outside the padding box.
            if (rectangular) {
                coverage = 1 - overlap(position, footprint, inner[0], inner[2]) / max(outer, MinimumAntialiasWidth);
            }
#endif
            result = over(premult(p.edges[i]) * coverage, result);
            break;
        }
    }
    return result * outer * p.extra.y * coverage;
}
#endif
