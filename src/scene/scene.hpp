#pragma once
#include "style/style.hpp"
#include "dom/geometry.hpp"
#include "scene/scene_paint.hpp"
#include "scene/scene_text.hpp"
#include <functional>
#include <memory>

#ifdef _WIN32
struct ID2D1RenderTarget;
#endif
namespace weft {
    // Owner-only scene formatting/animation probe. Render adapters consume these
    // owned values; neither DOM pointers nor VM handles cross the boundary.
    using SceneMeasure = std::function<SceneTextMetrics(std::string_view, const SceneStyle&)>;

    struct SceneNode {
        NodeIdentity identity;
        std::string tag;
        std::string space;
        std::string text;
        std::map<std::string, std::string> attributes;
        SceneStyle style;
        std::array<float, 4> box{}; // Parent-local border box, before transform.
        std::vector<SceneNode> children;
        std::optional<ScenePaintValues> paint; // Derived numeric cache, not DOM/style identity.

        friend bool operator==(const SceneNode& a, const SceneNode& b) {
            return a.identity == b.identity && a.tag == b.tag && a.space == b.space && a.text == b.text &&
                   a.attributes == b.attributes && a.style == b.style && a.box == b.box && a.children == b.children;
        }
    };

    struct SceneFrame {
        SceneNode root;
        std::vector<ElementGeometry> elements;
        bool animated{};
        double time_ms{};
    };

    // Exact dependency projection for the current geometry adapter. Paint color,
    // opacity and pseudo state do not change geometry; transforms still do.
    inline bool scene_geometry_own_equal(const SceneNode& a, const SceneNode& b, bool identical_style = false) {
        if (a.identity != b.identity || a.tag != b.tag || a.space != b.space || a.text != b.text || a.box != b.box) {
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
            a.attributes != b.attributes || a.style != b.style || a.box != b.box ||
            a.children.size() != b.children.size() || bool(a.paint) != bool(b.paint) ||
            (a.paint && a.paint->values != b.paint->values)) {
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
        SceneSolver(std::string stylesheet, std::string keyframes, SceneMeasure measure, bool incremental = true);
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
