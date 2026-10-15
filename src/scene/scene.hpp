#pragma once
#include "style/style.hpp"
#include "dom/geometry.hpp"
#include "scene/scene_anchor.hpp"
#include "scene/scene_effects.hpp"
#include "scene/scene_paint.hpp"
#include "scene/scene_particles.hpp"
#include "scene/scene_text.hpp"
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

#ifdef _WIN32
struct ID2D1RenderTarget;
#endif
namespace weft {
    // Owner-only scene formatting/animation probe. Render adapters consume these
    // owned values; neither DOM pointers nor VM handles cross the boundary.
    // A line of a node's text in an inline formatting context, in the node's own coordinates: the text on that line
    // (collapsed, without the spaces the line ends with) and the line box of that text alone, its baseline `baseline`
    // below the line box's top.
    struct SceneLine {
        std::string text;
        std::array<float, 4> rect{};
        float baseline{};

        friend bool operator==(const SceneLine&, const SceneLine&) = default;
    };

    // A fragment of an inline element's border box on one line, in the element's own coordinates; first and last tell
    // whether the box starts and ends on that line (its start and end edges are drawn only there).
    struct SceneFragment {
        std::array<float, 4> rect{};
        bool first{};
        bool last{};

        friend bool operator==(const SceneFragment&, const SceneFragment&) = default;
    };

    struct SceneNode {
        NodeIdentity identity;
        std::string tag;
        std::string space;
        std::string text;
        AttributeMap attributes;
        SceneStyle style;
        std::array<float, 4> box{}; // Parent-local border box, before transform.
        // Layout of inline formatting contexts: the lines of the node's text, and the fragments of an inline element's
        // box (its box is their bounding box). Empty outside a context.
        std::vector<SceneLine> lines;
        std::vector<SceneFragment> fragments;
        std::vector<SceneNode> children;
        std::optional<ScenePaintValues> paint; // Derived numeric cache, not DOM/style identity.
        // The -weft-effect() background and its parameters, resolved from the custom properties.
        std::shared_ptr<const SceneEffect> effect;
        // The particle emissions the element shows at the frame, drawn after its background and borders, before its
        // content.
        std::vector<SceneEmission> particles;
        // What its data-anchor attributes declare: the element follows an anchor (scene_anchor.hpp) instead of the place
        // layout gives it.
        std::optional<SceneAnchoring> anchoring;

        friend bool operator==(const SceneNode& a, const SceneNode& b) {
            return a.identity == b.identity && a.tag == b.tag && a.space == b.space && a.text == b.text &&
                   a.attributes == b.attributes && a.style == b.style && a.box == b.box && a.lines == b.lines &&
                   a.fragments == b.fragments &&
                   (a.effect == b.effect || (a.effect && b.effect && *a.effect == *b.effect)) &&
                   a.particles == b.particles && a.anchoring == b.anchoring && a.children == b.children;
        }
    };

    // Whether two nodes show the same effect: none, or one of equal values.
    inline bool scene_effect_equal(const std::shared_ptr<const SceneEffect>& a,
                                   const std::shared_ptr<const SceneEffect>& b) {
        return a == b || (a && b && *a == *b);
    }

    struct SceneFrame {
        SceneNode root;
        std::vector<ElementGeometry> elements;
        bool animated{};
        double time_ms{};
        // Which content the frame holds: frames of one revision draw alike, so a pass may keep what it prepared for
        // one. A solver gives a new revision whenever what it lays out or paints changes, and to every animated frame;
        // revisions are unique in the process. 0 for content nobody vouches for: code that changes a frame's content
        // outside a solver sets it to 0.
        std::uint64_t revision{};
    };

    // Exact dependency projection for the current geometry adapter. Paint color,
    // opacity and pseudo state do not change geometry; transforms still do.
    inline bool scene_geometry_own_equal(const SceneNode& a, const SceneNode& b, bool identical_style = false) {
        if (a.identity != b.identity || a.tag != b.tag || a.space != b.space || a.text != b.text || a.box != b.box ||
            a.fragments != b.fragments || a.anchoring != b.anchoring) {
            return false;
        }
        auto equal_keys = [](const auto& x, const auto& y, const auto& keys) {
            for (const auto* k : keys) {
                auto p = x.find(k);
                auto q = y.find(k);
                if ((p == x.end()) != (q == y.end()) || (p != x.end() && p->second != q->second)) {
                    return false;
                }
            }
            return true;
        };
        auto equal_styles = [](const SceneStyle& x, const SceneStyle& y, const auto& ids) {
            for (const auto id : ids) {
                const auto* p = x.find(id);
                const auto* q = y.find(id);
                if (!p != !q || (p && !(*p == *q))) {
                    return false;
                }
            }
            return true;
        };
        static constexpr PropertyId styles[]{
            PropertyId::Transform,         PropertyId::TransformOrigin,  PropertyId::Position,
            PropertyId::BorderLeftWidth,   PropertyId::BorderRightWidth, PropertyId::BorderTopWidth,
            PropertyId::BorderBottomWidth, PropertyId::FontSize,         PropertyId::FontWeight,
            PropertyId::FontFamily,        PropertyId::LetterSpacing,    PropertyId::TextAnchor,
            PropertyId::DominantBaseline};
        static constexpr const char* attrs[]{"transform", "width", "height", "viewBox", "preserveAspectRatio",
                                             "x",         "y",     "dy",     "x1",      "y1",
                                             "x2",        "y2",    "rx",     "ry",      "r",
                                             "cx",        "cy",    "d",      "points"};
        return (identical_style || equal_styles(a.style, b.style, styles)) &&
               equal_keys(a.attributes, b.attributes, attrs);
    }

    inline bool scene_geometry_equal(const SceneNode& a, const SceneNode& b) {
        if (!scene_geometry_own_equal(a, b) || a.children.size() != b.children.size()) {
            return false;
        }
        for (std::size_t i = 0; i < a.children.size(); ++i) {
            if (!scene_geometry_equal(a.children[i], b.children[i])) {
                return false;
            }
        }
        return true;
    }

    // Source reuse must compare the numeric samples too: diagnostic style strings
    // may round two different samples to the same spelling. Logical DOM equality
    // above intentionally excludes this derived cache.
    inline bool scene_paint_source_equal(const SceneNode& a, const SceneNode& b) {
        if (a.identity != b.identity || a.tag != b.tag || a.space != b.space || a.text != b.text ||
            a.attributes != b.attributes || a.style != b.style || a.box != b.box || a.lines != b.lines ||
            a.fragments != b.fragments || a.children.size() != b.children.size() || bool(a.paint) != bool(b.paint) ||
            (a.paint && a.paint->values != b.paint->values) || !scene_effect_equal(a.effect, b.effect) ||
            a.particles != b.particles) {
            return false;
        }
        for (std::size_t i = 0; i < a.children.size(); ++i) {
            if (!scene_paint_source_equal(a.children[i], b.children[i])) {
                return false;
            }
        }
        return true;
    }

    // Damage is relative to an actual retained surface, never a previous candidate
    // packet. Unknown paint bounds conservatively invalidate the entire surface.
    struct SceneDamage {
        bool changed{};
        bool full{};
        std::array<float, 4> rect{};
    }; // left/top/right/bottom

    SceneDamage scene_damage(const SceneFrame&, const SceneFrame&, float scale);

    class SceneSolver {
    public:
        // Text is shaped and laid out in lines by `text`, which outlives the solver.
        SceneSolver(std::string stylesheet, std::string keyframes, SceneText& text, bool incremental = true);
        ~SceneSolver();
        SceneSolver(const SceneSolver&) = delete;
        SceneSolver& operator=(const SceneSolver&) = delete;
        SceneFrame frame(const DomSnapshot&, float width, float height, double time_ms);
        // Owner-local retained scene; unchanged branches keep their owned values.
        // Publish a separate immutable snapshot before crossing threads. On error
        // discard the output and reset this solver before using it again.
        void update(const DomSnapshot&, SceneFrame&, float width, float height, double time_ms);
        void reset();
        void set_profiling(bool);
        // The effects background-image: -weft-effect(name) names; the host keeps them alive. Without them such a
        // background is a scene profile diagnostic.
        void set_effects(const SceneEffects* effects);
        // The particles -weft-particles: name and emit_particles name; the host keeps them alive. Without them such a
        // name is a scene profile diagnostic.
        //
        // An element streams its -weft-particles while it shows (it and its ancestors are not display: none): from
        // the frame its value names the particles, until a frame where it names none, other particles or other
        // override values (SceneParticles::resolve), or the element hides; a stopped stream's particles live out
        // their lives. A hidden element, and one the frame no longer has, drops its particles. Frames keep coming
        // while an element shows particles. More than constants::scene::max_particles_per_frame particles in a frame
        // is a scene profile diagnostic.
        void set_particles(const SceneParticles* particles);
        // A burst of the particles registered as name from element (weft.emitParticles), which starts at the next
        // update's time with the element's override values then; a target element is placed in that frame's layout,
        // its centre and a point mapped into the element's border box px. The next frame drops a burst on an element
        // it does not show. Throws DiagnosticError (Diag::SceneProfile) without registered particles, for a name
        // nobody registered, a count outside [1, constants::max_particles_per_emission], a duration that is negative
        // or not finite, a point that is not finite, or more than constants::scene::max_pending_particle_bursts
        // bursts before an update.
        void emit_particles(NodeIdentity element, std::string_view name, const SceneBurst& burst);

        struct Work {
            std::size_t style_evaluations{};
            std::size_t layout_evaluations{};
            std::size_t style_reuses{};
            std::size_t layout_reuses{};
            std::size_t animation_compilations{};
            std::size_t animation_reuses{};
            std::size_t typed_samples{};
            std::size_t paint_compilations{};
            std::size_t static_subtree_reuses{};
            double style_ms{};
            double build_ms{};
            double layout_ms{};
            bool geometry_changed{};
            bool paint_changed{};
        };

        Work work() const;

    private:
        struct Impl;
        std::unique_ptr<Impl> impl_;
    };

    StyleProfile scene_style_profile();

    // Windows software raster adapter, separate from the owner formatting core.
    // RGBA is straight encoded sRGB, ready for the existing image upload contract.
    struct SceneBitmap {
        unsigned width{};
        unsigned height{};
        std::vector<std::uint8_t> rgba;
    };

    // A host texture the reference raster draws for a -weft-texture(name) background, as pixels: width x height texels
    // of linear premultiplied RGBA, rows top down.
    struct SceneRasterTexture {
        unsigned width{};
        unsigned height{};
        std::vector<float> rgba;
    };

    class SceneRaster {
    public:
        // Text is measured and shaped by text (owner thread); the raster draws the runs.
        explicit SceneRaster(SceneText& text);
        ~SceneRaster();
        SceneRaster(const SceneRaster&) = delete;
        SceneRaster& operator=(const SceneRaster&) = delete;
        // Reference geometry (D2D geometry objects): SceneGeometry is the portable implementation; tests compare
        // the two (tests/scene_geometry_compare.hpp).
        std::array<double, 6> transform_matrix(const SceneNode&) const;
        SceneBitmap raster(const SceneFrame&, float scale = 1);
        // The textures -weft-texture(name) backgrounds show, by name, as the scene shader lays them out
        // (scene_background.hpp); an unbound name draws nothing, as on the native passes.
        void bind_textures(std::map<std::string, SceneRasterTexture> textures);

        struct Work {
            std::size_t gpu_projective_draws{};
            std::size_t gpu_plane_uploads{};
            std::size_t uniform_border_draws{};
            std::size_t fallback_border_draws{};
            std::size_t border_geometry_builds{};
            std::size_t border_geometry_reuses{};
            std::size_t paint_program_builds{};
            std::size_t paint_program_reuses{};
            std::size_t paint_program_evictions{};
            std::size_t typed_draws{};
            std::size_t layer_builds{};
            std::size_t layer_reuses{};
            std::size_t layer_fallbacks{};
            std::size_t layer_evictions{};
            std::size_t partial_paints{};
        };

        Work work() const;
        // Reference geometry, see transform_matrix.
        std::vector<ElementGeometry> geometry(const SceneFrame&, bool svg_descendants = false) const;
#ifdef _WIN32
        // Private Windows adapter: borrowed target for this call, owned scene only.
        void paint_d2d(const SceneFrame&, ID2D1RenderTarget*, float scale = 1,
                       std::optional<std::array<float, 4>> clip = std::nullopt);
        // Explicit host opt-in; identities are weak, copied values. No promotion
        // heuristic or change to CSS stacking semantics. Unsupported sources fall
        // back to normal painting. Caches are local to the target resource domain.
        void set_layers(std::vector<NodeIdentity>);
        void paint_d2d_damage(const SceneFrame&, ID2D1RenderTarget*, float scale, std::array<float, 4> damage,
                              std::optional<std::array<float, 4>> clip = std::nullopt);
#endif
        // Reference hit testing; D2D treats points within its 0.25 DIP flattening tolerance as inside.
        NodeIdentity hit_test(const SceneFrame&, float x, float y) const;

    private:
        struct Impl;
        std::unique_ptr<Impl> impl_;
    };
} // namespace weft
