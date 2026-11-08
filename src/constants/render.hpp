#pragma once

#include "constants/common.hpp"
#include "constants/scene.hpp"

// Budgets, limits and validation tolerances of the renderer: the ResourceLifetime model, the
// experimental renderer C ABI (src/render/render_backend.cpp), the rounded clip mask, the D3D11
// reference renderer and its native scene pass.
namespace weft::constants::render {
    // Frame pipelining defaults shared by LifetimeConfig and the default weft_renderer_desc.
    inline constexpr int default_max_prepared_frames = 2;
    // Encoded but not yet submitted uses (ResourceLifetime use tokens).
    inline constexpr int default_max_encoded_uses = 2;
    // Encoded plus submitted uses whose submission has not completed yet.
    inline constexpr int default_max_in_flight_uses = 64;
    // Live ResourceLifetime resources (CPU leased or pinned by a frame or use).
    inline constexpr int default_max_resources = 4096;

    // The shared scene shader draws each instance as two triangles from its vertex index (scene.hlsl's vs_main; its
    // particle pass, a particle per six vertices).
    inline constexpr int quad_vertex_count = 6;

    // Remaining default weft_renderer_desc budgets.
    inline constexpr int default_max_draws = 65536;
    inline constexpr int default_max_assets = 512;
    inline constexpr unsigned default_max_frame_asset_bytes = 16u * constants::bytes_per_mib;

    // The host textures a built-in backend of View frames (weft_render_d3d11_create, weft_render_vulkan_create)
    // imports at once, and their logical bytes.
    inline constexpr int view_external_textures = 32;
    inline constexpr std::uint64_t view_external_texture_bytes = 256ull * constants::bytes_per_mib;
    // The export images (render_export_texture.h) a built-in backend of View frames holds at once, and their logical
    // bytes, every level.
    inline constexpr int view_export_images = 32;
    inline constexpr std::uint64_t view_export_image_bytes = 256ull * constants::bytes_per_mib;

    // Upper bounds a host may request; they cap the slot tables allocated up front.
    inline constexpr int max_prepared_frames_limit = 1024;
    inline constexpr int max_in_flight_uses_limit = 4096;
    inline constexpr int max_external_textures_limit = 4096;
    inline constexpr int max_export_images_limit = 4096;

    // Bits a backend may report: WEFT_RENDER_FEATURE_* (six features) and one bit per
    // weft_render_target_format (seven formats).
    inline constexpr unsigned known_feature_bits = 63u;
    inline constexpr unsigned known_target_format_bits = 127u;

    // Accepted sdf_spread range, in texels, for signed distance assets.
    inline constexpr int min_sdf_spread = 2;
    inline constexpr int max_sdf_spread = 32;
    // Largest normalized UV origin + extent; the slack absorbs float rounding of a full extent.
    inline constexpr float max_uv_extent = 1.000001f;
    // A diagnostic clipped quad (Draw::clipped) is clipped this many pixels inside each edge.
    inline constexpr int diagnostic_clip_inset = 16;

    // Bytes per texel of the linear RGBA texture formats.
    inline constexpr int rgba16f_bytes_per_pixel = 8;
    inline constexpr int rgba32f_bytes_per_pixel = 16;

    // sRGB transfer function of IEC 61966-2-1, as in src/render/backends/d3d11/srgb_convert.hlsl.
    inline constexpr float srgb_decode_threshold = 0.04045f;
    inline constexpr float srgb_encode_threshold = 0.0031308f;
    inline constexpr float srgb_linear_slope = 12.92f;
    inline constexpr float srgb_offset = 0.055f;
    inline constexpr float srgb_scale = 1.055f;
    inline constexpr float srgb_gamma = 2.4f;
    // constants::color_channel_max as float, for byte <-> [0, 1] conversions.
    inline constexpr float color_channel_max_float = 255.0f;

    // The texture cache every renderer shares (render/texture_cache.hpp).
    namespace texture_cache {
        // Bounds of a renderer's cache: textures and their bytes.
        inline constexpr int max_resources = 1024;
        inline constexpr int max_bytes = 128 * constants::bytes_per_mib;
        // Asset identities: expired entries are pruned from this size; live entries are bounded.
        inline constexpr int identity_prune_threshold = 1024;
        inline constexpr int max_identities = 2048;
        // Glyph atlas: R8 pages of this many pixels a side, at most this many pages, for coverage images of at most
        // atlas_max_image pixels a side, each placed with a border of this many texels that repeat its edge texels.
        inline constexpr int atlas_page_size = 2048;
        inline constexpr int atlas_max_pages = 4;
        inline constexpr int atlas_max_image = 128;
        inline constexpr int atlas_border = 1;
        // A cache key whose version has this bit names the texture of its own that an effect samples, never an atlas
        // region. Display list versions are small counters, and effects do not come through the C ABI's quad frames,
        // so it never meets a host's version.
        inline constexpr unsigned long long standalone_key_bit = 1ull << 63;
    } // namespace texture_cache

    // D3D11 reference renderer and its C ABI adapter (src/render/backends/d3d11).
    namespace d3d11 {
        // The vertex shader draws each instance as two triangles from SV_VertexID.
        inline constexpr int quad_vertex_count = render::quad_vertex_count;
        // Render targets are at most this many pixels wide and high; the backend reports the same
        // max_texture_size and max_target_size.
        inline constexpr int max_surface_dimension = 4096;

        // Host frames that may be encoded or in flight at once: the backend's encoded and in-flight use
        // limits and the TwoFramePipeline slots.
        inline constexpr int frames_in_flight = 2;
        // The backend holds one prepared frame; its native handle is always this value.
        inline constexpr int max_prepared_frames = 1;
        inline constexpr int prepared_frame_handle = 1;
        // CPU frame input budget of the backend. Three 1024x1536 RGBA images of a real game workload fit.
        inline constexpr unsigned max_frame_asset_bytes = 32u * constants::bytes_per_mib;
        // Backend capabilities: all five WEFT_RENDER_FEATURE_* bits, and the SRGB_RGBA8, LINEAR_RGBA8 and
        // LINEAR_RGBA32F bits of weft_render_target_format.
        inline constexpr int features = 31;
        inline constexpr int target_formats = 7;
        // External texture extension.
        inline constexpr int max_imported_textures = 128;
        inline constexpr int external_texture_capacity = 32;
        inline constexpr unsigned external_texture_max_bytes = 64u * constants::bytes_per_mib;

        // Constant buffer ranges bind at this granularity (16 constants), and a bound range holds at most
        // max_constant_buffer_bytes (D3D11.1 constant buffer offsetting).
        inline constexpr int constant_buffer_offset_bytes = 256;
        inline constexpr int max_constant_buffer_bytes = 65536;
        // A shader constant (a float4): the unit of a bound range's first constant and count.
        inline constexpr int shader_constant_bytes = 16;

        // Device epoch and queue of the single D3D11 immediate context.
        inline constexpr int device_epoch = 1;
        inline constexpr int immediate_queue = 1;
        // A UI frame consumes two submission serials: its command list, then the host completion marker.
        // Host-only composition consumes one.
        inline constexpr int serials_per_ui_frame = 2;
        // Bounded wait for an actual GPU completion query.
        inline constexpr int completion_timeout_seconds = 5;
        inline constexpr int completion_poll_ms = 1;

    } // namespace d3d11

    // The D3D12 backend of View frames (src/render/backends/d3d12).
    namespace d3d12 {
        // Root parameters of the scene root signature: the View constants (b0, four 32-bit values: the target's width
        // and height, the scale and the draw's first record), a table of the pass's buffers (the records t0, rounded
        // clips t2, record order t7 and anchors t8, through structured buffer views, whose reads past their ends give
        // zero as on D3D11: the shader reads the anchor of a record without one before it knows), and a table of the
        // image (t1) and an effect's textures or a particle sprite (t3-t6).
        inline constexpr int root_view = 0;
        inline constexpr int root_buffers = 1;
        inline constexpr int root_images = 2;
        inline constexpr int view_constant_count = 12;
        // The index of the draw's first record among the View constants (scene.hlsl, View::baseInstance).
        inline constexpr int view_base_instance = 3;
        // The buffer table's descriptors: records, clips, order, anchors.
        inline constexpr int buffer_descriptors = 4;
        // An image table's descriptors: the image, then the four effect textures.
        inline constexpr int table_descriptors = 5;
        // Root parameters of the backdrop root signature: its constants (b0) as a root constant buffer view, and the
        // source image (t0) in a table.
        inline constexpr int backdrop_root_constants = 0;
        inline constexpr int backdrop_root_source = 1;
        // Each instance draws two triangles.
        inline constexpr int quad_vertex_count = render::quad_vertex_count;
        // Render targets are at most this many pixels wide and high.
        inline constexpr int max_surface_dimension = 8192;
        // A recording's shader-visible descriptor heap has room for at least this many descriptors, doubling past it;
        // the backend's render target views are allocated from heaps of this many.
        inline constexpr int descriptor_heap_min = 256;
        inline constexpr int render_target_heap_size = 256;
        // Upload buffers: a new one has room for at least spare_buffer_min_bytes, doubling past it.
        inline constexpr unsigned spare_buffer_min_bytes = 64u * constants::bytes_per_kib;
        // What a scene slot keeps of its recordings for its next ones: buffers, scratch images (by count and bytes)
        // and descriptor heaps.
        inline constexpr int scene_slot_spare_buffers = 64;
        inline constexpr int scene_slot_spare_images = 256;
        inline constexpr unsigned scene_slot_spare_image_bytes = 64u * constants::bytes_per_mib;
        inline constexpr int scene_slot_spare_heaps = 4;
    } // namespace d3d12

    // The Vulkan backend (src/render/backends/vulkan).
    namespace vulkan {
        // Descriptor bindings of the shared scene shader's SPIR-V, after the register shifts of cmake/WeftSpirv.cmake:
        // the View constants b0, the records t0, the image t1, the rounded clips t2 and the sampler s0.
        inline constexpr int binding_view = 0;
        inline constexpr int binding_records = 10;
        inline constexpr int binding_image = 11;
        inline constexpr int binding_clips = 12;
        inline constexpr int binding_sampler = 20;
        // An effect's textures (scene.hlsl effectTexture0-3, t3-t6: one binding each from this one) and the sampler
        // that repeats them (effectRepeat, s1).
        inline constexpr int binding_effect_textures = 13;
        inline constexpr int binding_effect_sampler = 21;
        // A scene pass's record order (t7) and anchors (t8).
        inline constexpr int binding_order = 17;
        inline constexpr int binding_anchors = 18;
        // Each instance draws two triangles.
        inline constexpr int quad_vertex_count = render::quad_vertex_count;
        // Descriptor sets one pass may allocate: one per batch.
        inline constexpr int max_pass_descriptor_sets = 4096;
        // Waits on a fence give up after this many nanoseconds.
        inline constexpr unsigned long long fence_timeout_ns = 10'000'000'000ull;
        // Render targets are at most this many pixels wide and high.
        inline constexpr int max_surface_dimension = 4096;
        // The C ABI adapter: the device epoch and queue it reports, its one prepared frame's handle, frames recorded or
        // in flight at once, the features of WEFT_RENDER_FEATURE_* it draws (solid, image, coverage, SDF, display
        // lists) and the CPU input bytes one quad frame may carry.
        inline constexpr int device_epoch = 1;
        inline constexpr int queue = 1;
        inline constexpr int prepared_frame_handle = 1;
        inline constexpr int max_prepared_frames = 1;
        inline constexpr int frames_in_flight = 2;
        inline constexpr int features = 31;
        inline constexpr unsigned max_frame_asset_bytes = 32u * constants::bytes_per_mib;
        // Completed frames give the renderer their buffers and descriptor pools: it keeps at most this many of each
        // kind, buffers of at most spare_buffer_max_bytes. A new buffer has room for at least spare_buffer_min_bytes,
        // a new pool for descriptor_pool_min_sets sets, both doubling past them.
        inline constexpr int spare_frame_resources = 4;
        inline constexpr unsigned spare_buffer_max_bytes = 16u * constants::bytes_per_mib;
        inline constexpr unsigned spare_buffer_min_bytes = 64u * constants::bytes_per_kib;
        inline constexpr int descriptor_pool_min_sets = 16;
        // What a scene slot keeps of its recordings for its next ones: buffers, scratch images (by size; as many as
        // fit in scene_slot_spare_image_bytes, which holds the scratch of 32 glass windows at 4K) and descriptor
        // pools.
        inline constexpr int scene_slot_spare_buffers = 64;
        inline constexpr int scene_slot_spare_images = 256;
        inline constexpr unsigned scene_slot_spare_image_bytes = 64u * constants::bytes_per_mib;
        inline constexpr int scene_slot_spare_pools = 16;
        // Images of at most block_image_max_bytes share blocks of image_block_bytes of device-local memory: one
        // allocation per block instead of one per image. A device keeps one empty block of each memory type.
        inline constexpr unsigned image_block_bytes = 16u * constants::bytes_per_mib;
        inline constexpr unsigned block_image_max_bytes = 2u * constants::bytes_per_mib;
        // A swapchain asks for this many images beyond the surface's minimum, so that acquiring rarely waits.
        inline constexpr int swapchain_extra_images = 1;
    } // namespace vulkan

    // The Metal backend (docs/render-backend-api.md, "Metal"): argument table indices of the shared shaders' MSL,
    // after the register shifts of cmake/WeftMetal.cmake (constant buffer b<n> at buffer n, structured buffer or
    // texture t<n> at 1 + n of its table, sampler s<n> at sampler n). render_metal_shader_contract checks every one.
    namespace metal {
        // scene.hlsl: the View constants b0, the records t0, the image t1, the rounded clips t2, a scene pass's record
        // order t7 and anchors t8.
        inline constexpr int buffer_view = 0;
        inline constexpr int buffer_records = 1;
        inline constexpr int texture_image = 2;
        inline constexpr int buffer_clips = 3;
        inline constexpr int buffer_order = 8;
        inline constexpr int buffer_anchors = 9;
        // An effect's textures (effectTexture0-3, t3-t6: one index each from this one), where the particles' sprite
        // (particleSprite, t3), a scene draw's mask image (maskImage, t3) and a blending group's backdrop
        // (blendBackdrop, t4) go too.
        inline constexpr int texture_effects = 4;
        // linearClamp s0, and linearRepeat or an effect's effectRepeat s1.
        inline constexpr int sampler_clamp = 0;
        inline constexpr int sampler_repeat = 1;
        // backdrop.hlsl: a pass's constants b0, its source t0 and the mirroring sampler s0.
        inline constexpr int backdrop_buffer_constants = 0;
        inline constexpr int backdrop_texture_source = 1;
        inline constexpr int backdrop_sampler_mirror = 0;
        // export_resolve.hlsl: the scene pass's scratch target t0.
        inline constexpr int export_texture_encoded = 1;
        // Scratch targets of finished export recordings kept for the next ones.
        inline constexpr int export_spare_scratch = 4;
        // The MSL version of the translation (major * 10000 + minor * 100): 2.1, macOS 10.14 and iOS 12 or later.
        inline constexpr int msl_version = 20100;
        // Render targets are at most this many pixels wide and high (Apple GPUs' 2D textures; a Retina display's window
        // passes Vulkan's 4096).
        inline constexpr int max_surface_dimension = 16384;
        // What a scene slot keeps of its recordings for its next ones, as the Vulkan backend's: buffers (a new one has
        // room for at least spare_buffer_min_bytes, doubling past it), scratch textures by size, as many as fit in
        // scene_slot_spare_image_bytes.
        inline constexpr unsigned spare_buffer_min_bytes = 64u * constants::bytes_per_kib;
        inline constexpr int scene_slot_spare_buffers = 64;
        inline constexpr int scene_slot_spare_images = 256;
        inline constexpr unsigned scene_slot_spare_image_bytes = 64u * constants::bytes_per_mib;
        // Uploads stage rows of a multiple of this many bytes, at offsets of a multiple of it.
        inline constexpr int staging_alignment = 16;
    } // namespace metal

    // Scene passes (scene_pass.hpp and its backends). The shader-mirrored values match the static consts at the
    // top of scene.hlsl.
    namespace scene {
        // Paint::extra.z / extra.w flags: FlagEnabled, and its disabled counterpart.
        inline constexpr float flag_enabled = 1.f;
        inline constexpr float flag_disabled = 0.f;
        // MaxGradientStops and StopsPerOffsetVector.
        inline constexpr int max_gradient_stops = constants::scene::max_gradient_stops;
        inline constexpr int stops_per_offset_vector = 4;
        inline constexpr int gradient_offset_vectors = max_gradient_stops / stops_per_offset_vector;
        // Size of the shader Paint record (28 float4 registers).
        inline constexpr int record_bytes = 448;
        // Size of the shader Clip record (5 float4 registers).
        inline constexpr int clip_bytes = 80;
        // Size of the View constant buffer (three float4: the target's size, the scale and the first record; the target's
        // origin; the image a group's composite samples).
        inline constexpr int view_constants_bytes = 48;
        inline constexpr int view_constant_floats = 12;
        // Paint::mode.z of a group's composite (an EndGroup record): it samples the group's image where the View's
        // sampled field places it (scene.hlsl, GroupComposite).
        inline constexpr float group_composite = 1.f;
        // A hoisted group's image (scene/scene_schedule.hpp): its sides rounded up to this many device px, so that a
        // group whose region moves or grows a little keeps the image size a backend reuses; and the images of one pass
        // together take at most this many times the pass's area.
        inline constexpr int group_image_granularity = 64;
        inline constexpr int hoisted_group_budget = 1;
        // Paint::mode.x of image items: ImageModeCoverage and ImageModeSignedDistance (other images are 0).
        inline constexpr float image_mode_coverage = 1.f;
        inline constexpr float image_mode_signed_distance = 2.f;
        // Paint::mode.x of a plane: its content image seen through a homography (ImageModeProjective).
        inline constexpr float image_mode_projective = 3.f;
        // Paint::mode.x of a backdrop: its blurred copy of the target over the device px Paint::uv covers
        // (ImageModeBackdrop).
        inline constexpr float image_mode_backdrop = 4.f;
        // Paint::mode.x of a host texture (a named texture of a scene pass): linear premultiplied colors, drawn in the
        // pass's encoding (ImageModeTexture).
        inline constexpr float image_mode_texture = 5.f;
        // A backdrop blurs with a standard deviation of at most this many device px: for a larger one the copy is first
        // downsampled by its area to the size that brings it to this, as Skia's GPU blur scales its source.
        inline constexpr float max_backdrop_sigma = 4.f;
        // A backdrop's Gaussian kernel reaches this many standard deviations, at most max_backdrop_taps taps on each
        // side of its centre (src/render/shaders/backdrop.hlsl).
        inline constexpr float backdrop_kernel_sigmas = 3.f;
        inline constexpr int max_backdrop_taps = 12;
        // Size of the backdrop passes' constant buffer (backdrop.hlsl's BackdropPass: 11 float4).
        inline constexpr int backdrop_constants_bytes = 176;
        // The sprite mode of a particles record (render::ParticleSprite's w): discs, coverage sprites tinted by the
        // particle's color, or color sprites (linear premultiplied) times it (ParticleSpriteCoverage and
        // ParticleSpriteColor).
        inline constexpr float particle_sprite_none = 0.f;
        inline constexpr float particle_sprite_coverage = 1.f;
        inline constexpr float particle_sprite_color = 2.f;
        inline constexpr int record_initial_capacity = 16;
        // Patches of a kept pass whose ranges a backend remembers, for the slots holding earlier versions of it.
        inline constexpr int kept_patch_history = 8;
        // Nested transparent groups available per slot.
        inline constexpr int max_group_depth = 4;
        // Rounded clips an item's chain may hold; the shader multiplies the coverage of each, so a longer chain is
        // refused (Diag::PaintList, or WEFT_RENDER_UNSUPPORTED_FEATURE at the C ABI). The CPU reference coverage
        // takes the same bound (rounded_mask_max_clips).
        inline constexpr int max_rounded_clips = 16;
        // A polygon clip's entry marks its fill rule in its radius_x[0], below any radius (scene.hlsl's
        // ClipPolygonNonzero and ClipPolygonEvenOdd).
        inline constexpr float clip_polygon_nonzero = -1.f;
        inline constexpr float clip_polygon_evenodd = -2.f;
        // A mask clip's entry marks itself the same way (scene.hlsl's ClipMask).
        inline constexpr float clip_mask = -3.f;
        // Text and layer images keep a transparent margin around the node on each side.
        inline constexpr int image_margin = 2;
        inline constexpr int image_margins = 2 * image_margin;
        // Instance bounds grow by this many device pixels on each side for antialiasing, and stop as far past the clip.
        inline constexpr int record_bounds_outset = 2;
        // Items that cover pixel centers need no antialiasing, but their bounds reach this far past them, in device
        // pixels: a rasterizer may drop a pixel whose center lies exactly on an edge (Vulkan leaves the tie open).
        inline constexpr float pixel_center_outset = .5f;
        // Initial extent of the bounds accumulation.
        inline constexpr float record_bounds_sentinel = 1e10f;
        // Overflow tolerances for text metrics and for layer descendants, in CSS pixels.
        inline constexpr float text_fit_tolerance = .01f;
        inline constexpr double layer_bounds_tolerance = .01;
        // Each image cache of a scene preparer (text and layers, shadows, paths): the entries and bytes past which the
        // least recently used images of earlier passes go (SceneCacheBounds).
        inline constexpr int image_cache_max_entries = 1024;
        inline constexpr int image_cache_budget_bytes = 32 * constants::bytes_per_mib;
        // The bytes of one text, layer, shadow or path image.
        inline constexpr int image_max_bytes = 16 * constants::bytes_per_mib;
        // Steps of a device pixel in which a path image's key takes its map's translation past whole pixels: maps
        // closer than a step share an image.
        inline constexpr int path_key_subpixels = 64;
        // Atlases of a scene preparer: text, shadow and path images that, with a border of atlas_border texels that
        // repeat their edge texels, are at most half a page wide and atlas_max_height texels high share pages of their
        // format, atlas_page_size texels a side for coverage (a byte per texel) and atlas_color_page_size for colors
        // (4 MiB a page either way), at most atlas_max_pages pages of each.
        inline constexpr int atlas_page_size = 2048;
        inline constexpr int atlas_color_page_size = 1024;
        inline constexpr int atlas_max_pages = 8;
        inline constexpr int atlas_max_height = 128;
        inline constexpr int atlas_border = 1;
        // Transparent-group scratch targets across all slots.
        inline constexpr int scratch_budget_bytes = 128 * constants::bytes_per_mib;
    } // namespace scene

    // Vector paths (vector/path_raster.hpp).
    namespace path {
        // Largest coverage mask, in device pixels; its image (a byte per pixel) fits the scene image budget.
        inline constexpr int max_mask_pixels = 2048 * 2048;
        // Coverage reaches this many device pixels past a path's bounds.
        inline constexpr int antialias_margin = 1;
        // Curves flatten for dashing within this many device pixels, and for the rasterizer within this many.
        inline constexpr double dash_flattening_tolerance = .1;
        inline constexpr double raster_flattening_tolerance = .01;
        // Most dashes one path may cut into.
        inline constexpr int max_dash_segments = 65536;
        // Deepest subdivision of one cubic while flattening.
        inline constexpr int max_flattening_depth = 16;
        // FreeType points per pixel (26.6 fixed point) and the unit of its 16.16 miter limit.
        inline constexpr int freetype_subpixels = 64;
        inline constexpr int freetype_fixed_one = 65536;
        // FreeType's stroker takes points nearer than this per axis (in its 26.6 units) as one point: FT_EPSILON in
        // ftstroke.c.
        inline constexpr int freetype_stroker_epsilon = 2;
        // A fill whose contours overlap rasterizes at this many subpixels per pixel along each axis (FreeType's
        // FT_OUTLINE_OVERLAP rendering), where its spans' x (16 bits) and its outline's points (FT_Outline_Render's
        // bound, in 26.6) reach.
        inline constexpr int overlap_subpixels = 4;
        inline constexpr int freetype_max_span_x = 0x7fff;
        inline constexpr long freetype_max_outline_coordinate = 0x1000000;
    } // namespace path

    // Rounded clip probe work bounds.
    // Largest absolute rectangle coordinate, size or corner radius.
    inline constexpr int rounded_clip_max_magnitude = constants::max_logical_px_magnitude;
    inline constexpr int rounded_mask_max_clips = 16;
    inline constexpr int rounded_mask_max_dimension = 4096;
    inline constexpr int rounded_mask_max_pixels = 8 * 1024 * 1024;
    // Supersampling grid per axis for partially covered mask pixels.
    inline constexpr unsigned rounded_mask_samples_per_axis = 16;
} // namespace weft::constants::render
