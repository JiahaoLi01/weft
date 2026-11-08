// The shared pass of display lists (src/render/scene_pass.hpp). A scene pass composites encoded premultiplied
// colors, the existing D2D/Chrome scene contract, and the shared conversion pass produces the engine's linear
// target. The SPACE_LINEAR variant is a linear pass: it gets linear premultiplied colors from the CPU and
// composites them unquantized. A compile-time variant keeps the scene pass's code exactly as it was. The WEFT_EFFECT
// variant is the pixel stage of one UI shader effect (cmake/WeftEffects.cmake): its source, included after this file,
// defines effect(). The WEFT_PARTICLES variant is both stages of the particle pass: each vertex a corner of one
// particle's quad. The WEFT_BLEND_FETCH variant (Metal on Apple GPUs) composites blending groups over what the target
// holds, read where the pixel is drawn.

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
// Paint::mode.z of a group's composite (an EndGroup record, render::scene_pass.cpp): it samples the group's image where
// View::sampled places it in the pass's device px, when that has a size.
static const float GroupComposite = 1;
// Paint::mode.x of a backdrop: its blurred copy of the target, bound as image over the device px Paint::uv covers
// (origin and extent); Paint::extra.w enables its color matrix in Paint::stops[0..4].
static const float ImageModeBackdrop = 4;
// Paint::mode.x of a host texture (a scene page's -weft-texture(name)): linear premultiplied colors, which a scene pass
// draws in its encoding.
static const float ImageModeTexture = 5;
// FreeType encodes contour zero at byte 128, not normalized 0.5.
static const float SdfByteMax = 255.0;
static const float SdfZeroByte = 128.0;
static const float MinimumRadius = 1e-6;
// A polygon clip's entry (Clip.radiusX.x, below any radius: constants::render::scene's clip_polygon_nonzero and
// clip_polygon_evenodd), the points an entry carries and the x of the point between two contours
// (src/paint/rounded_clip.hpp).
static const float ClipPolygonNonzero = -1;
static const float ClipPolygonEvenOdd = -2;
static const uint ClipPolygonPointsPerEntry = 9;
static const float ClipPolygonBreak = -1e30;
// A mask clip's entry (Clip.radiusX.x: constants::render::scene's clip_mask) and the floats of one of its layers
// (clip_mask_layer_floats, src/scene/scene_mask.cpp's scene_mask_data).
static const float ClipMask = -3;
// A page's image filtered half a level sharper than its derivatives say, as Skia picks the level it draws at
// (SkMipmap::ComputeLevel's bias of -0.5): the derivatives times 2^-0.5.
static const float MipSharpening = 0.70710678;
// How far, px, the tile that covers the item may fall short of it from rounding.
static const float TileCoverSlack = 1.0 / 64;
static const uint MaskLayerFloats = 36;
// How far inside a box's edge (its px) the point lies whose gradient tile an edge pixel takes (gradient()): a
// LayoutUnit, as constants::scene::gradient_edge_inset.
static const float GradientEdgeInset = 1.0 / 64;
// Instance bounds reach this many device px past an item for its antialiasing (record_bounds_outset).
static const float RecordBoundsOutset = 2;
// The smallest anchor scale whose outset the bounds make up for; a hidden element's corners lie here.
static const float MinimumAnchorScale = .0001;
static const float2 Nowhere = float2(-1e6, -1e6);

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
    // x: first rounded clip, y: rounded clip count, z: 0, or 1 + the anchor that moves the record from its element's
    // space.
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
// A scene pass's own draws (neither particles nor an effect's): a host texture's repeating, filtering sampler (the
// effects' sampler, s1), which a mask's image takes too; and the image of the mask the drawn item's chain holds, where
// an effect's first texture goes (SceneSchedule::Extra::Mask).
#define MASK_IMAGES (!WEFT_PARTICLES && !WEFT_EFFECT && !SPACE_LINEAR)
#if MASK_IMAGES
SamplerState linearRepeat : register(s1);
Texture2D maskImage : register(t3);
// A blending group's backdrop: what its target held under it, copied before it composites, where an effect's second
// texture goes (SceneSchedule::Draw::backdrop).
Texture2D blendBackdrop : register(t4);
#if WEFT_BLEND_FETCH
// Or, in the variant the Metal backend composites blending groups with on Apple GPUs, what the target holds where the
// pixel is drawn (framebuffer fetch: [[color(0)]] in MSL), read without a copy (SceneSchedule::Pipeline::Blend).
[[vk::input_attachment_index(0)]] SubpassInput<float4> blendTarget : register(t31);
#endif
#endif
#if !SPACE_LINEAR
// A scene pass's record for each instance of a draw (at instance + baseInstance), and its anchors
// (docs/engine-communication.md, 5.3): each the map of an anchored element's space to target px, its scale in x and
// offset in y and z (CSS px), w 0 when the element is hidden. A linear pass takes its records in order and moves none.
StructuredBuffer<uint> order : register(t7);
StructuredBuffer<float4> anchors : register(t8);
#endif

// The target drawn into: its size in device px, the scale, and the first record of the draw where the vertex stage
// adds it; its origin among the pass's device px (a group drawn into an image of its own region,
// scene/scene_schedule.hpp's hoisted groups; 0 for the pass's own target and for groups of its size); and the image a
// group's composite samples: its origin and size among the pass's device px (none: the record's uv, over the target).
// Records, clips and pixel positions are all in the pass's device px: a pixel of the target lies at its SV_Position
// plus the origin.
cbuffer View : register(b0) {
    float2 target;
    float scale;
    float baseInstance;
    float2 origin;
    float2 viewUnused;
    float4 sampled;
};

struct Vertex {
    float4 position : SV_Position;
    nointerpolation uint index : TEXCOORD0;
};

#if !WEFT_PARTICLES
Vertex vs_main(uint vertex : SV_VertexID, uint instance : SV_InstanceID) {
    const float2 corners[6] = {float2(0, 0), float2(1, 0), float2(0, 1), float2(0, 1), float2(1, 0), float2(1, 1)};
#if SPACE_LINEAR
    uint index = instance + (uint)baseInstance;
#else
    uint index = order[instance + (uint)baseInstance];
#endif
    Paint p = paints[index];
    float2 corner = corners[vertex];
    float2 local = p.bounds.xy + corner * p.bounds.zw;
#if !SPACE_LINEAR
    if (p.clips.z > 0) {
        // Moved by its anchor; drawn smaller, its bounds keep the outset in device px. A hidden element's quad is a
        // point outside the target.
        float4 a = anchors[(uint)p.clips.z - 1];
        local += (corner * 2 - 1) * max(1 / max(a.x, MinimumAnchorScale) - 1, 0) * RecordBoundsOutset / scale;
        local = a.w > 0 ? local * a.x + a.yz : Nowhere;
    }
#endif
    float2 position = local * scale;
    Vertex v;
    v.position = float4((position - origin) / target * float2(2, -2) + float2(-1, 1), 0, 1);
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

// Coverage of target (CSS px) by one rounded clip, antialiased across a device pixel (pixels device px per CSS px): in
// the clip's own space, the distance to the rectangle, or inside a corner's box the first-order distance to its ellipse.
float clipCoverage(Clip c, float2 target, float pixels) {
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
    return saturate(.5 - d * c.invX.w * pixels);
}

// Point j of the polygon whose points start at clip entry data, ClipPolygonPointsPerEntry an entry: in rect, radiusX,
// radiusY, invX, then invY's x and y (render::plan_scene_clip).
float2 clipPolygonPoint(uint data, uint j) {
    Clip c = clips[data + j / ClipPolygonPointsPerEntry];
    uint k = j % ClipPolygonPointsPerEntry;
    float4 v = k < 2 ? c.rect : k < 4 ? c.radiusX : k < 6 ? c.radiusY : k < 8 ? c.invX : c.invY;
    return (k & 1) != 0 ? v.zw : v.xy;
}

// The coverage of a polygon clip (CSS clip-path polygon() and path()), whose entry c holds its bounds in rect, its fill
// rule and point count in radiusX and its map, and whose points follow in the entries from data on: closed contours
// apart by a point whose x is ClipPolygonBreak. Inside by the winding number (nonzero or even-odd), antialiased over
// the distance to the nearest edge that bounds the filled region, as clipCoverage antialiases its rectangle. Crossing
// an edge from its left changes the winding number by -1 and from its right by +1: under nonzero, an edge between
// windings 1 and 2 (where a polygon crosses itself) bounds nothing, and a point of winding 2 or more lies a whole
// edge's crossing from outside.
float clipPolygonCoverage(Clip c, uint data, float2 target, float pixels) {
    float2 world = float2(dot(c.invX.xyz, float3(target, 1)), dot(c.invY.xyz, float3(target, 1)));
    float scale = c.invX.w * pixels;
    float2 lo = c.rect.xy;
    float2 hi = c.rect.xy + c.rect.zw;
    float2 q = max(lo - world, world - hi);
    if (max(q.x, q.y) * scale > 1) {
        return 0;
    }
    uint count = (uint)c.radiusX.y;
    // The nearest edge with the point on its left, and on its right.
    float left = 3.4e38;
    float right = 3.4e38;
    int winding = 0;
    float2 a = clipPolygonPoint(data, 0);
    for (uint j = 1; j < count; j++) {
        float2 b = clipPolygonPoint(data, j);
        if (a.x > ClipPolygonBreak * .5 && b.x > ClipPolygonBreak * .5) {
            float2 e = b - a;
            float2 w = world - a;
            float t = saturate(dot(w, e) / max(dot(e, e), MinimumRadius));
            float d = length(w - e * t);
            float side = e.x * w.y - e.y * w.x;
            // A point on the edge's line lies on both sides.
            left = side >= 0 ? min(left, d) : left;
            right = side <= 0 ? min(right, d) : right;
            if (a.y <= world.y) {
                winding += b.y > world.y && side > 0 ? 1 : 0;
            } else {
                winding -= b.y <= world.y && side < 0 ? 1 : 0;
            }
        }
        a = b;
    }
    float nearest = min(left, right);
    if (c.radiusX.x == ClipPolygonEvenOdd) {
        return saturate(.5 + ((winding & 1) != 0 ? nearest : -nearest) * scale);
    }
    if (winding == 0) {
        return saturate(.5 - nearest * scale);
    }
    float bounding = winding == 1 ? left : winding == -1 ? right : 3.4e38;
    return saturate(.5 + bounding * scale);
}

#if MASK_IMAGES
// paint::BlendMode values (src/paint/display_list.hpp).
static const float BlendMultiply = 1;
static const float BlendScreen = 2;
static const float BlendOverlay = 3;
static const float BlendDarken = 4;
static const float BlendLighten = 5;
static const float BlendColorDodge = 6;
static const float BlendColorBurn = 7;
static const float BlendHardLight = 8;
static const float BlendSoftLight = 9;
static const float BlendDifference = 10;
static const float BlendExclusion = 11;
static const float BlendHue = 12;
static const float BlendSaturation = 13;
static const float BlendColor = 14;
static const float BlendLuminosity = 15;
static const float BlendPlusLighter = 16;

// The separable blend functions of Compositing and Blending 1 §10.1 on one channel: cb the backdrop's, cs the source's.
float blendChannel(float cb, float cs, float mode) {
    if (mode == BlendMultiply) {
        return cb * cs;
    }
    if (mode == BlendScreen) {
        return cb + cs - cb * cs;
    }
    if (mode == BlendOverlay) {
        return cb <= .5 ? cs * 2 * cb : cs + (2 * cb - 1) - cs * (2 * cb - 1);
    }
    if (mode == BlendDarken) {
        return min(cb, cs);
    }
    if (mode == BlendLighten) {
        return max(cb, cs);
    }
    if (mode == BlendColorDodge) {
        return cb <= 0 ? 0 : cs >= 1 ? 1 : min(1, cb / (1 - cs));
    }
    if (mode == BlendColorBurn) {
        return cb >= 1 ? 1 : cs <= 0 ? 0 : 1 - min(1, (1 - cb) / cs);
    }
    if (mode == BlendHardLight) {
        return cs <= .5 ? cb * 2 * cs : cb + (2 * cs - 1) - cb * (2 * cs - 1);
    }
    if (mode == BlendSoftLight) {
        float d = cb <= .25 ? ((16 * cb - 12) * cb + 4) * cb : sqrt(cb);
        return cs <= .5 ? cb - (1 - 2 * cs) * cb * (1 - cb) : cb + (2 * cs - 1) * (d - cb);
    }
    if (mode == BlendDifference) {
        return abs(cb - cs);
    }
    return cb + cs - 2 * cb * cs;
}

// The non-separable ones' helpers (§10.2): luminosity, saturation and their setters.
float blendLum(float3 c) {
    return dot(c, float3(.3, .59, .11));
}

float3 blendClipColor(float3 c) {
    float l = blendLum(c);
    float n = min(c.r, min(c.g, c.b));
    float x = max(c.r, max(c.g, c.b));
    if (n < 0) {
        c = l + (c - l) * l / max(l - n, MinimumRadius);
    }
    if (x > 1) {
        c = l + (c - l) * (1 - l) / max(x - l, MinimumRadius);
    }
    return c;
}

float3 blendSetLum(float3 c, float l) {
    return blendClipColor(c + (l - blendLum(c)));
}

float blendSat(float3 c) {
    return max(c.r, max(c.g, c.b)) - min(c.r, min(c.g, c.b));
}

float3 blendSetSat(float3 c, float s) {
    float n = min(c.r, min(c.g, c.b));
    float x = max(c.r, max(c.g, c.b));
    return x > n ? (c - n) * s / (x - n) : float3(0, 0, 0);
}

float3 blendColors(float3 cb, float3 cs, float mode) {
    if (mode == BlendHue) {
        return blendSetLum(blendSetSat(cs, blendSat(cb)), blendLum(cb));
    }
    if (mode == BlendSaturation) {
        return blendSetLum(blendSetSat(cb, blendSat(cs)), blendLum(cb));
    }
    if (mode == BlendColor) {
        return blendSetLum(cs, blendLum(cb));
    }
    if (mode == BlendLuminosity) {
        return blendSetLum(cb, blendLum(cs));
    }
    return float3(blendChannel(cb.r, cs.r, mode), blendChannel(cb.g, cs.g, mode), blendChannel(cb.b, cs.b, mode));
}

// A blending group's composite at device pixel `pixel`: s the group's premultiplied color there (its opacity and
// coverage in), over the backdrop the pass copied over region (left, top, width, height in device px). The source mixed
// with the blend function where the backdrop is opaque, as §5.3 composites a blended source, so that the pass's
// source-over gives the blended result; plus-lighter (§9.1.13) adds premultiplied colors up to 1, which the output
// reaches through source-over too. Encoded sRGB values, as Chrome blends them.
float4 blendComposite(float4 s, float mode, float2 pixel, float4 region) {
#if WEFT_BLEND_FETCH
    float4 d = blendTarget.SubpassLoad();
#else
    float4 d = blendBackdrop.SampleLevel(linearClamp, (pixel - region.xy) / max(region.zw, 1), 0);
#endif
    if (mode == BlendPlusLighter) {
        float4 sum = min(1, s + d);
        float a = d.a < 1 ? saturate((sum.a - d.a) / (1 - d.a)) : 0;
        return float4(sum.rgb - d.rgb * (1 - a), a);
    }
    if (s.a <= 0) {
        return float4(0, 0, 0, 0);
    }
    float3 cs = saturate(s.rgb / s.a);
    float3 cb = d.a > 0 ? saturate(d.rgb / d.a) : float3(0, 0, 0);
    float3 mixed = (1 - d.a) * cs + d.a * saturate(blendColors(cb, cs, mode));
    return float4(mixed * s.a, s.a);
}
#endif

// Float k of the floats whose pairs start at clip entry data.
float clipFloat(uint data, uint k) {
    float2 p = clipPolygonPoint(data, k / 2);
    return (k & 1) != 0 ? p.y : p.x;
}

// One layer of a mask (scene_mask_data's floats from base: the gradient's kind, whether it repeats, its angle and stop
// count, its tile and ending shape, whether the tile repeats on x and y, its composite, its painting area and its stops'
// offsets and values) at a point q of the element's px: the gradient's value in the tile q falls in (none past a tile
// that does not repeat), transparent outside the painting area, whose edges are antialiased as a clip's (scale: device
// px per px). Kind 0 is a transparent layer (mask-image none). Kind 4 is the mask's image (maskImage) over the tile,
// its alpha or (where a gradient says whether it repeats) its luminance, sampled as a page's image is (fittedTexture):
// from its smaller levels, half a level sharp, where one tile covers the painting area, and from the full-size image
// where tiles repeat across it; gx and gy: how far q moves for a device pixel to the right and down.
float maskLayer(uint data, uint base, float2 q, float scale, float2 gx, float2 gy) {
    float coverage = 1;
    if (clipFloat(data, base + 15) != 0) {
        float2 lo = float2(clipFloat(data, base + 16), clipFloat(data, base + 17));
        float2 hi = lo + float2(clipFloat(data, base + 18), clipFloat(data, base + 19));
        float2 outside = max(lo - q, q - hi);
        coverage = saturate(.5 - max(outside.x, outside.y) * scale);
    }
    float kind = clipFloat(data, base);
    if (kind == 0 || coverage <= 0) {
        return 0;
    }
    float4 tile = float4(clipFloat(data, base + 4), clipFloat(data, base + 5), clipFloat(data, base + 6),
                         clipFloat(data, base + 7));
    float2 cell = floor((q - tile.zw) / max(tile.xy, MinimumRadius));
    if ((clipFloat(data, base + 12) == 0 && cell.x != 0) || (clipFloat(data, base + 13) == 0 && cell.y != 0)) {
        return 0;
    }
    float2 local = q - tile.zw - cell * tile.xy;
    if (kind == 4) {
#if MASK_IMAGES
        uint width, height, levels;
        maskImage.GetDimensions(0, width, height, levels);
        float2 natural = float2(width, height);
        float2 uv = local / max(tile.xy, MinimumRadius);
        float2 edge = .5 / natural;
        bool2 repeats = bool2(clipFloat(data, base + 12) != 0, clipFloat(data, base + 13) != 0);
        // An axis that does not repeat stops at its edge texels' centers; one that does wraps across the tiles.
        uv.x = repeats.x ? uv.x : clamp(uv.x, edge.x, 1 - edge.x);
        uv.y = repeats.y ? uv.y : clamp(uv.y, edge.y, 1 - edge.y);
        // One tile covers the painting area along an axis that does not repeat, or where the tile reaches past it.
        bool single = true;
        if (clipFloat(data, base + 15) != 0) {
            float2 lo = float2(clipFloat(data, base + 16), clipFloat(data, base + 17));
            float2 hi = lo + float2(clipFloat(data, base + 18), clipFloat(data, base + 19));
            float2 start = tile.zw + floor((lo - tile.zw) / max(tile.xy, MinimumRadius)) * tile.xy;
            bool2 covers = hi <= start + tile.xy + TileCoverSlack;
            single = (!repeats.x || covers.x) && (!repeats.y || covers.y);
        } else {
            single = !repeats.x && !repeats.y;
        }
        float2 dx = gx / max(tile.xy, MinimumRadius);
        float2 dy = gy / max(tile.xy, MinimumRadius);
        float4 texel = single ? maskImage.SampleGrad(linearRepeat, uv, dx * MipSharpening, dy * MipSharpening)
                              : maskImage.SampleLevel(linearRepeat, uv, 0);
        float value = clipFloat(data, base + 1) != 0 ? dot(texel.rgb, float3(.2126, .7152, .0722)) : texel.a;
        return value * coverage;
#else
        return 0;
#endif
    }
    float angle = clipFloat(data, base + 2) * DegreesToRadians;
    float t;
    if (kind == GradientLinear) {
        float2 d = float2(sin(angle), -cos(angle));
        t = dot(local - tile.xy * .5, d) / max(dot(tile.xy, abs(d)), MinimumRadius) + .5;
    } else if (kind == GradientRadial) {
        float4 shape = float4(clipFloat(data, base + 8), clipFloat(data, base + 9), clipFloat(data, base + 10),
                              clipFloat(data, base + 11));
        t = length((local - shape.xy) / max(shape.zw, MinimumRadius));
    } else {
        t = frac((atan2(local.x - tile.x * .5, -(local.y - tile.y * .5)) - angle) / TwoPi + 2);
    }
    uint count = (uint)clipFloat(data, base + 3);
    float first = clipFloat(data, base + 20);
    float last = clipFloat(data, base + 20 + count - 1);
    if (clipFloat(data, base + 1) != 0) {
        // A repeating gradient: the stops again every span from the first's offset to the last's.
        float span = max(last - first, MinimumStopSpan);
        t = first + (t - first) - floor((t - first) / span) * span;
    }
    if (t <= first) {
        return clipFloat(data, base + 28) * coverage;
    }
    if (t >= last) {
        return clipFloat(data, base + 28 + count - 1) * coverage;
    }
    float value = clipFloat(data, base + 28);
    for (uint k = 1; k < count; k++) {
        float from = clipFloat(data, base + 20 + k - 1);
        float to = clipFloat(data, base + 20 + k);
        if (t >= from && t < to) {
            value = lerp(clipFloat(data, base + 28 + k - 1), clipFloat(data, base + 28 + k),
                         saturate((t - from) / max(MinimumStopSpan, to - from)));
        }
    }
    return value * coverage;
}

// The value of a mask clip (CSS mask-image's gradient layers, scene_mask), whose entry c holds its map and layer count
// and whose layers' floats follow in the entries from data on: its layers from the bottom one up, each composited over
// those below by its operator (add: source-over; subtract: source-out; intersect: source-in; exclude: xor).
float clipMaskCoverage(Clip c, uint data, float2 target, float pixels) {
    float2 q = float2(dot(c.invX.xyz, float3(target, 1)), dot(c.invY.xyz, float3(target, 1)));
    float scale = c.invX.w * pixels;
    float2 gx = float2(c.invX.x, c.invY.x) / pixels;
    float2 gy = float2(c.invX.y, c.invY.y) / pixels;
    uint layers = (uint)c.radiusX.z;
    float below = 0;
    for (uint l = layers; l > 0; l--) {
        uint base = (l - 1) * MaskLayerFloats;
        float s = maskLayer(data, base, q, scale, gx, gy);
        float op = clipFloat(data, base + 14);
        below = l == layers ? s
                : op == 0   ? s + below * (1 - s)
                : op == 1   ? s * (1 - below)
                : op == 2   ? s * below
                            : s * (1 - below) + below * (1 - s);
    }
    return below;
}

float clipChain(Paint p, float2 world, float pixels) {
    float coverage = 1;
    uint first = (uint)p.clips.x;
    for (uint i = 0; i < (uint)p.clips.y; i++) {
        Clip c = clips[first + i];
        if (c.radiusX.x == ClipMask) {
            coverage *= clipMaskCoverage(c, first + i + 1, world, pixels);
            i += ((uint)c.radiusX.y + ClipPolygonPointsPerEntry - 1) / ClipPolygonPointsPerEntry;
        } else if (c.radiusX.x < 0) {
            coverage *= clipPolygonCoverage(c, first + i + 1, world, pixels);
            i += ((uint)c.radiusX.y + ClipPolygonPointsPerEntry - 1) / ClipPolygonPointsPerEntry;
        } else {
            coverage *= clipCoverage(c, world, pixels);
        }
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

// A pixel that a box's edge crosses can have its center outside the box: it takes the box's color at that edge, from
// the tile the edge bounds in the box, not from the next tile, which the box clips away. The tile is that of a point a
// little inside the box, so an edge on a tile's boundary ends that tile.
float4 gradient(Paint p, float2 position) {
    float2 inside = clamp(position, 0, p.size.xy);
    float2 tiles = floor((clamp(position, GradientEdgeInset, p.size.xy - GradientEdgeInset) - p.tile.zw) / p.tile.xy);
    float2 q = inside - p.tile.zw - tiles * p.tile.xy;
    float angle = p.extra.x * DegreesToRadians;
    float2 d = float2(sin(angle), -cos(angle));
    float extent = dot(p.tile.xy, abs(d));
    float t;
    if (p.size.z == GradientLinear) {
        t = dot(q - p.tile.xy * .5, d) / extent + .5;
    } else if (p.size.z == GradientRadial) {
        // The ending shape: its center in uv.xy, its radii in uv.zw.
        t = length((q - p.uv.xy) / p.uv.zw);
    } else {
        t = (atan2(q.x - p.tile.x * .5, -(q.y - p.tile.y * .5)) - angle) / TwoPi;
        t = frac(t + 2);
    }
    uint count = (uint)p.size.w;
    if (p.mode.y == FlagEnabled) {
        // A repeating gradient: the stops again every span from the first's offset to the last's.
        float first = offset(p, 0);
        float span = offset(p, count - 1) - first;
        t = first + (t - first) - floor((t - first) / span) * span;
    }
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

#if WEFT_EFFECT || WEFT_PARTICLES || !SPACE_LINEAR
// The sRGB transfer, for effects that work with CSS colors and a scene pass's host textures: an encoded channel to
// linear light, and back. Scalar
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
    v.position = float4((world * scale - origin) / target * float2(2, -2) + float2(-1, 1), 0, 1);
    v.color = color;
    return v;
}

// A disc antialiased across a device px at its rim or softened inward over softness of its radius, or a sprite frame
// tinted by the color; clipped as an image, then the item's opacity.
float4 ps_main(ParticleVertex v) : SV_Target {
    Paint p = paints[v.index];
    float2 pixel = v.position.xy + origin;
    float2 world = pixel / scale;
#if SPACE_LINEAR
    float coverage = world.x < p.clip.x || world.y < p.clip.y || world.x >= p.clip.z || world.y >= p.clip.w
                         ? 0
                         : clipChain(p, world, scale);
#else
    if (world.x < p.clip.x || world.y < p.clip.y || world.x >= p.clip.z || world.y >= p.clip.w) {
        discard;
    }
    float coverage = clipChain(p, world, scale);
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
// parameters (absent ones zero). An effect whose manifest says it reads the backdrop (docs/effects.md) also gets what
// lay behind its element: a copy of the target taken before the element drew, over its box and the manifest's margin
// past it (render::ScenePass::Copy). backdrop is the pixel's uv in the copy; backdropAcross and backdropDown are how
// that uv changes per unit of uv across and down the box (through the box's transform and the device scale), so that
// effectBackdropUV maps any uv of the box into the copy. backdropCopied is 1 when the copy is bound; another effect
// (one whose manifest says nothing of the backdrop) reads a transparent backdrop.
struct EffectInput {
    float2 uv;
    float2 size;
    float2 position;
    float2 pixel;
    float scale;
    float time;
    float4 parameters[EffectParameters];
    float2 backdrop;
    float2 backdropAcross;
    float2 backdropDown;
    float backdropCopied;
};

// The uv in the backdrop's copy of the point at uv of the box (EffectInput::uv's space).
float2 effectBackdropUV(EffectInput input, float2 uv) {
    float2 d = uv - input.uv;
    return input.backdrop + input.backdropAcross * d.x + input.backdropDown * d.y;
}

// What lay behind the element at uv of the backdrop's copy, filtered linearly, as linear premultiplied color; a uv
// outside the copy (past the margin, or past the target's edge) reads the copy's nearest texel.
float4 effectBackdrop(EffectInput input, float2 backdropUV) {
    float4 c = image.SampleLevel(linearClamp, backdropUV, 0) * input.backdropCopied;
#if !SPACE_LINEAR
    c = c.a > 0 ? float4(effectLinear(saturate(c.rgb / c.a)) * c.a, c.a) : float4(0, 0, 0, 0);
#endif
    return c;
}

// What lay behind the pixel itself.
float4 effectBackdrop(EffectInput input) {
    return effectBackdrop(input, input.backdrop);
}

// The effect's linear premultiplied color at one pixel, defined by the effect's source.
float4 effect(EffectInput input);

// An effect record (scene_pass.cpp) is an image record whose texels the effect computes: clipped as an image, shaped
// by the box's corners as a box, with the time in extra.x and the parameters in the fields effects leave unused
// (render::effect_parameter); an effect that reads its backdrop has its copy bound as image, over the device px uv
// covers (origin and extent), as a backdrop's, and mode.x ImageModeBackdrop.
float4 ps_main(Vertex v) : SV_Target {
    Paint p = paints[v.index];
    float2 pixel = v.position.xy + origin;
    float2 world = pixel / scale;
#if SPACE_LINEAR
    float coverage = world.x < p.clip.x || world.y < p.clip.y || world.x >= p.clip.z || world.y >= p.clip.w
                         ? 0
                         : clipChain(p, world, scale);
#else
    if (world.x < p.clip.x || world.y < p.clip.y || world.x >= p.clip.z || world.y >= p.clip.w) {
        discard;
    }
    float coverage = clipChain(p, world, scale);
#endif
    float2 position = float2(dot(p.invX.xyz, float3(world, 1)), dot(p.invY.xyz, float3(world, 1)));
    EffectInput input;
    input.uv = position / p.size.xy;
    input.size = p.size.xy;
    input.position = position;
    input.pixel = pixel;
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
    // A uv of the box moves through the box's px, the inverse of the record's map from target px to them (invX, invY),
    // the scale to device px, and the copy's extent to its uv.
    float determinant = p.invX.x * p.invY.y - p.invX.y * p.invY.x;
    float2 perPixel = scale / p.uv.zw;
    input.backdrop = (pixel - p.uv.xy) / p.uv.zw;
    input.backdropAcross = float2(p.invY.y, -p.invY.x) / determinant * p.size.x * perPixel;
    input.backdropDown = float2(-p.invX.y, p.invX.x) / determinant * p.size.y * perPixel;
    input.backdropCopied = p.mode.x == ImageModeBackdrop ? 1 : 0;
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
#if !SPACE_LINEAR
// paint::TextureFit::Scale and Repeat values (src/paint/display_list.hpp).
static const float FitCover = 1;
static const float FitContain = 2;
static const float RepeatNone = 1;
static const float RepeatSpace = 2;
static const float RepeatRound = 3;

// One axis of a host texture's tiles, as scene_texture_tiles (src/scene/scene_background.cpp) lays them out: where a tile
// starts and the period they repeat at (0 for one tile), from the tile's size, the positioning area's start and length,
// the position's share of the room and its length, and the repeat. space spreads whole tiles over the area, the first
// and the last at its edges, and places one by the position when two do not fit.
float2 textureAxis(float tile, float start, float length, float share, float offset, float repeats) {
    float first = start + (length - tile) * share + offset;
    float period = repeats == RepeatNone ? 0 : tile;
    if (repeats == RepeatSpace) {
        float n = floor(length / max(tile, MinimumRadius));
        first = n >= 2 ? start : first;
        period = n >= 2 ? tile + (length - n * tile) / (n - 1) : 0;
    }
    return float2(first, period);
}

// Whether one tile covers the item's extent along an axis (0 to length): drawn once, not as a pattern.
bool textureSingle(float tile, float first, float period, float length) {
    if (period <= 0) {
        return true;
    }
    float start = first + floor((0 - first) / period) * period;
    return length <= start + tile + TileCoverSlack;
}

// Where a point of an axis lies in its tile: x where in it (0 to 1), y 1 inside a tile and 0 outside.
float2 texturePoint(float tile, float first, float period, float x) {
    float d = x - first;
    d = period > 0 ? d - floor(d / period) * period : d;
    return float2(d / max(tile, MinimumRadius), tile > 0 && d >= 0 && d < tile ? 1 : 0);
}

// A host texture at a point of its item (CSS px), its tiles laid out from its size for paint::TextureFit, which the
// record holds: stops[0] the positioning area, stops[1] the size (below 0 auto) and the scale, stops[2] the position's
// shares of the room and its lengths, stops[3] the repeats, whether the texture is a page's image and whether it is
// pixelated. Transparent outside the tiles. An axis the tile repeats along wraps across the tiles' edges; another stops
// at its edge texels' centers, so that the repeating sampler blends nothing across.
//
// A page's image is sampled as Chrome draws one: pixelated (image-rendering: pixelated, crisp-edges), its nearest texel
// of the full-size image; otherwise filtered, from its smaller levels when it is drawn smaller (trilinear, at the level
// Skia picks: half a level sharper than the derivatives' one) where one tile covers the item, and from the full-size
// image where its tiles repeat across it (Chrome draws a pattern of them without mipmaps). A host texture is filtered at
// its derivatives' level. gx and gy: how far the point moves (CSS px) for a device pixel to the right and down.
float4 fittedTexture(Paint p, float2 position, float2 gx, float2 gy) {
    uint width, height, levels;
    image.GetDimensions(0, width, height, levels);
    float2 natural = float2(width, height);
    float4 area = p.stops[0];
    float2 size = p.stops[1].xy;
    float scaling = p.stops[1].z;
    float2 repeats = p.stops[3].xy;
    bool lengths = scaling != FitCover && scaling != FitContain;
    float2 tile = size;
    if (!lengths) {
        float2 s = area.zw / natural;
        tile = natural * (scaling == FitCover ? max(s.x, s.y) : min(s.x, s.y));
    } else if (tile.x < 0 && tile.y < 0) {
        tile = natural;
    } else if (tile.x < 0) {
        tile.x = tile.y * natural.x / natural.y;
    } else if (tile.y < 0) {
        tile.y = tile.x * natural.y / natural.x;
    }
    // round: as many whole tiles as fit best, scaled to fill the area; the other axis keeps the texture's ratio when
    // its size is auto.
    float2 unrounded = tile;
    if (repeats.x == RepeatRound && unrounded.x > 0) {
        tile.x = area.z / max(1, floor(area.z / unrounded.x + .5));
    }
    if (repeats.y == RepeatRound && unrounded.y > 0) {
        tile.y = area.w / max(1, floor(area.w / unrounded.y + .5));
    }
    if (lengths && repeats.x == RepeatRound && repeats.y != RepeatRound && size.y < 0 && unrounded.x > 0) {
        tile.y = unrounded.y * tile.x / unrounded.x;
    } else if (lengths && repeats.y == RepeatRound && repeats.x != RepeatRound && size.x < 0 && unrounded.y > 0) {
        tile.x = unrounded.x * tile.y / unrounded.y;
    }
    float2 x = textureAxis(tile.x, area.x, area.z, p.stops[2].x, p.stops[2].z, repeats.x);
    float2 y = textureAxis(tile.y, area.y, area.w, p.stops[2].y, p.stops[2].w, repeats.y);
    // The derivatives of the tile's coordinates, which choose the texture's level: from the record's mapping rather
    // than ddx and ddy, which are undefined in a quad with discarded pixels (at the item's edges), and without a seam
    // where the tiles repeat.
    float2 dx = gx / max(tile, MinimumRadius) * p.uv.zw;
    float2 dy = gy / max(tile, MinimumRadius) * p.uv.zw;
    float2 u = texturePoint(tile.x, x.x, x.y, position.x);
    float2 v = texturePoint(tile.y, y.x, y.y, position.y);
    if (u.y == 0 || v.y == 0) {
        return float4(0, 0, 0, 0);
    }
    float2 t = float2(u.x, v.x);
    // Half a texel of the part of the texture the image samples (uv: a border image's slice; the whole texture for a
    // background), so a clamped edge stops at its own texels' centers.
    float2 edge = .5 / max(natural * p.uv.zw, MinimumRadius);
    if (repeats.x == RepeatNone || repeats.x == RepeatSpace) {
        t.x = clamp(t.x, edge.x, 1 - edge.x);
    }
    if (repeats.y == RepeatNone || repeats.y == RepeatSpace) {
        t.y = clamp(t.y, edge.y, 1 - edge.y);
    }
    float2 at = p.uv.xy + t * p.uv.zw;
    if (p.stops[3].w == FlagEnabled) {
        return image.SampleLevel(linearRepeat, (floor(at * natural) + .5) / natural, 0);
    }
    if (p.stops[3].z == FlagEnabled) {
        if (!textureSingle(tile.x, x.x, x.y, p.size.x) || !textureSingle(tile.y, y.x, y.y, p.size.y)) {
            return image.SampleLevel(linearRepeat, at, 0);
        }
        return image.SampleGrad(linearRepeat, at, dx * MipSharpening, dy * MipSharpening);
    }
    return image.SampleGrad(linearRepeat, at, dx, dy);
}
#endif

float4 ps_main(Vertex v) : SV_Target {
    Paint p = paints[v.index];
    float2 pixel = v.position.xy + origin;
    float2 world = pixel / scale;
    // Device px per CSS px of the record's space: a record an anchor moves lies in its element's space, at the anchor's
    // scale.
    float pixels = scale;
#if !SPACE_LINEAR
    if (p.clips.z > 0) {
        float4 a = anchors[(uint)p.clips.z - 1];
        world = (world - a.yz) / a.x;
        pixels = scale * a.x;
    }
#endif
#if SPACE_LINEAR
    // A linear pass draws transparent black outside its clip and its image instead of discarding: premultiplied
    // source-over leaves the target unchanged, and neighbouring pixels keep defined derivatives for their
    // antialiasing.
    float coverage = world.x < p.clip.x || world.y < p.clip.y || world.x >= p.clip.z || world.y >= p.clip.w
                         ? 0
                         : clipChain(p, world, pixels);
#else
    if (world.x < p.clip.x || world.y < p.clip.y || world.x >= p.clip.z || world.y >= p.clip.w) {
        discard;
    }
    float coverage = clipChain(p, world, pixels);
#endif
    float2 position = float2(dot(p.invX.xyz, float3(world, 1)), dot(p.invY.xyz, float3(world, 1)));
    if (p.mode.x == ImageModeBackdrop) {
        // What lay behind the box, blurred, through its color matrix on straight values (rows of r, g, b and a in
        // stops[0..3], their offsets in stops[4]), clamped, in the box's shape.
        float4 c = image.SampleLevel(linearClamp, (pixel - p.uv.xy) / p.uv.zw, 0);
        if (p.extra.w == FlagEnabled) {
            float4 s = c.a > 0 ? float4(c.rgb / c.a, c.a) : float4(0, 0, 0, 0);
            s = saturate(float4(dot(p.stops[0], s), dot(p.stops[1], s), dot(p.stops[2], s), dot(p.stops[3], s)) +
                         p.stops[4]);
            c = float4(s.rgb * s.a, s.a);
        }
        float sd = distanceRound(position, p.size.xy, p.radii);
        float shape = saturate(.5 - sd / max(pixelWidth(sd), MinimumAntialiasWidth));
        if (all(p.radii == 0) && p.invX.y == 0 && p.invY.x == 0) {
            shape = overlap(position, max(float2(abs(p.invX.x), abs(p.invY.y)) / pixels, MinimumAntialiasWidth), 0,
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
        if (p.mode.z == GroupComposite && sampled.z > 0) {
            // A group's image texel for pixel, where the View places it.
            uv = (pixel - sampled.xy) / sampled.zw;
        }
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
#if !SPACE_LINEAR
        if (p.mode.x == ImageModeTexture) {
            // A page's image filtered as it is, premultiplied encoded sRGB, as Chrome filters one; a host texture's
            // filtered straight linear color, encoded and premultiplied again.
            // The record's mapping is affine (a projective image is no texture): a device pixel moves the point by
            // its inverse's columns.
            float4 texel = fittedTexture(p, position, float2(p.invX.x, p.invY.x) / pixels,
                                         float2(p.invX.y, p.invY.y) / pixels);
            if (p.stops[3].z == FlagEnabled) {
                return texel * p.extra.y * coverage;
            }
            float a = saturate(texel.a);
            float4 c = a > 0 ? float4(effectEncoded(saturate(texel.rgb / texel.a)) * a, a) : float4(0, 0, 0, 0);
            return c * p.extra.y * coverage;
        }
#endif
#if MASK_IMAGES
        // A group that blends composites over its backdrop (mode.y its blend mode, outline the copy's region).
        if (p.mode.x == 0 && p.mode.y > 0) {
            return blendComposite(sample * p.extra.y * coverage, p.mode.y, pixel, p.outline);
        }
#endif
        // A tinted mask (scene text, paths) premultiplies its color as a box does: quantized, as the reference
        // producer's brush. Its coverage is in r: an image of a byte per texel, or white content, whose premultiplied
        // channels all equal its alpha.
        return (p.extra.w == FlagEnabled ? premult(p.background) * sample.r : sample) * p.extra.y * coverage;
    }
    bool rectangular = all(p.radii == 0) && p.invX.y == 0 && p.invY.x == 0;
    // A device pixel's extent in the box's px.
    float2 footprint = max(float2(abs(p.invX.x), abs(p.invY.y)) / pixels, MinimumAntialiasWidth);
    if (rectangular && all(p.widths <= 0) && p.size.z <= 0) {
        // A plain box, without corners, rotation, borders or gradient (a page's background: most of its pixels): its
        // color over its share of the pixel, as the general path below computes it, without the distance, its
        // derivatives and the border wedges.
#if SPACE_LINEAR
        float share = all(position >= 0) && all(position < p.size.xy) ? 1 : 0;
#else
        float share = overlap(position, footprint, 0, p.size.xy);
#endif
        return premult(p.background) * share * p.extra.y * coverage;
    }
    float sd = distanceRound(position, p.size.xy, p.radii);
    float aa = max(pixelWidth(sd), MinimumAntialiasWidth);
    float outer = saturate(.5 - sd / aa);
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
    // A rectangular box's pixel takes the side whose band covers most of it when its center lies in no wedge of a side
    // that has a width: a thin side's band (a border-bottom thinner than a pixel, whose padding-box edge lies inside the
    // pixel) covers part of a pixel whose center is above it, in the wedge of a side of no width.
    uint chosen = QuadCorners;
#if !SPACE_LINEAR
    if (rectangular) {
        float4 bands = float4(overlap(position, footprint, float2(0, 0), float2(p.size.x, p.widths.x)),
                              overlap(position, footprint, float2(p.size.x - p.widths.y, 0), p.size.xy),
                              overlap(position, footprint, float2(0, p.size.y - p.widths.z), p.size.xy),
                              overlap(position, footprint, float2(0, 0), float2(p.widths.w, p.size.y)));
        float most = 0;
        [unroll]
        for (uint side = 0; side < QuadCorners; side++) {
            if (p.widths[side] > 0 && bands[side] > most) {
                most = bands[side];
                chosen = side;
            }
        }
    }
#endif
    [unroll]
    for (uint wedged = 0; wedged < QuadCorners; wedged++) {
        if (p.widths[wedged] > 0 && wedge(sided, equal ? p.size.xy * .5 : inner[wedged], corners[wedged],
                                          corners[(wedged + 1) % QuadCorners],
                                          equal ? p.size.xy * .5 : inner[(wedged + 1) % QuadCorners])) {
            chosen = wedged;
            break;
        }
    }
    [unroll]
    for (uint i = 0; i < QuadCorners; i++) {
        if (i == chosen) {
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
