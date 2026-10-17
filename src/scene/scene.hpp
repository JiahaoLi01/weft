#pragma once
#include "style/style.hpp"
#include "constants/common.hpp"
#include "dom/geometry.hpp"
#include "scene/scene_anchor.hpp"
#include "scene/scene_effects.hpp"
#include "scene/scene_paint.hpp"
#include "scene/scene_particles.hpp"
#include "scene/scene_text.hpp"
#include <algorithm>
#include <atomic>
#include <cstddef>
#include <functional>
#include <initializer_list>
#include <iterator>
#include <map>
#include <memory>
#include <stdexcept>
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

    struct SceneNode;

    // The children of a scene node: values that share their storage while they are equal. Copying a node copies its
    // children's handles, not their subtrees; changing a child (edit, edit_back, edit_all) first gives it storage of its
    // own when another value shares it (copy on write), so no value that other code holds (a published frame, a
    // renderer's cache) ever changes. Reading (operator[], at, front, back, iteration) never copies. Equal handles hold
    // equal subtrees: a renderer may keep what it made of a subtree while the subtree's handle stays.
    //
    // A reference that edit returned is valid until the next change of these children or of the node that holds them;
    // a copy of that node made meanwhile shares the child, which the reference must not change.
    class SceneChildren {
        using Storage = std::vector<std::shared_ptr<SceneNode>>;

    public:
        using value_type = SceneNode;
        using size_type = std::size_t;
        // A child's storage, shared read only.
        using Handle = std::shared_ptr<const SceneNode>;

        class const_iterator {
        public:
            using iterator_category = std::random_access_iterator_tag;
            using value_type = SceneNode;
            using difference_type = std::ptrdiff_t;
            using pointer = const SceneNode*;
            using reference = const SceneNode&;

            const_iterator() = default;

            explicit const_iterator(Storage::const_iterator at) : at_(at) {
            }

            reference operator*() const {
                return **at_;
            }

            pointer operator->() const {
                return at_->get();
            }

            reference operator[](difference_type n) const {
                return *at_[n];
            }

            const_iterator& operator++() {
                ++at_;
                return *this;
            }

            const_iterator operator++(int) {
                auto old = *this;
                ++at_;
                return old;
            }

            const_iterator& operator--() {
                --at_;
                return *this;
            }

            const_iterator operator--(int) {
                auto old = *this;
                --at_;
                return old;
            }

            const_iterator& operator+=(difference_type n) {
                at_ += n;
                return *this;
            }

            const_iterator& operator-=(difference_type n) {
                at_ -= n;
                return *this;
            }

            friend const_iterator operator+(const_iterator i, difference_type n) {
                return i += n;
            }

            friend const_iterator operator+(difference_type n, const_iterator i) {
                return i += n;
            }

            friend const_iterator operator-(const_iterator i, difference_type n) {
                return i -= n;
            }

            friend difference_type operator-(const const_iterator& a, const const_iterator& b) {
                return a.at_ - b.at_;
            }

            friend bool operator==(const const_iterator&, const const_iterator&) = default;
            friend auto operator<=>(const const_iterator&, const const_iterator&) = default;

        private:
            Storage::const_iterator at_;
            friend class SceneChildren;
        };

        using iterator = const_iterator;
        using const_reverse_iterator = std::reverse_iterator<const_iterator>;

        // Changes each child as it is reached: for (auto& c : node.children.edit_all()).
        class EditRange {
        public:
            class iterator {
            public:
                using iterator_category = std::forward_iterator_tag;
                using value_type = SceneNode;
                using difference_type = std::ptrdiff_t;
                using pointer = SceneNode*;
                using reference = SceneNode&;

                iterator() = default;

                iterator(SceneChildren* children, size_type at) : children_(children), at_(at) {
                }

                reference operator*() const {
                    return children_->edit(at_);
                }

                pointer operator->() const {
                    return &children_->edit(at_);
                }

                iterator& operator++() {
                    ++at_;
                    return *this;
                }

                iterator operator++(int) {
                    auto old = *this;
                    ++at_;
                    return old;
                }

                friend bool operator==(const iterator& a, const iterator& b) {
                    return a.at_ == b.at_;
                }

            private:
                SceneChildren* children_{};
                size_type at_{};
            };

            explicit EditRange(SceneChildren& children) : children_(&children) {
            }

            iterator begin() const {
                return {children_, 0};
            }

            iterator end() const {
                return {children_, children_->size()};
            }

        private:
            SceneChildren* children_;
        };

        SceneChildren() = default;
        SceneChildren(std::initializer_list<SceneNode> nodes);
        SceneChildren(std::vector<SceneNode> nodes);
        SceneChildren& operator=(std::initializer_list<SceneNode> nodes);
        SceneChildren& operator=(std::vector<SceneNode> nodes);

        size_type size() const noexcept {
            return nodes_.size();
        }

        bool empty() const noexcept {
            return nodes_.empty();
        }

        void reserve(size_type n) {
            nodes_.reserve(n);
        }

        void clear() noexcept {
            nodes_.clear();
        }

        void pop_back() {
            nodes_.pop_back();
        }

        const_iterator begin() const noexcept {
            return const_iterator(nodes_.begin());
        }

        const_iterator end() const noexcept {
            return const_iterator(nodes_.end());
        }

        const_reverse_iterator rbegin() const noexcept {
            return const_reverse_iterator(end());
        }

        const_reverse_iterator rend() const noexcept {
            return const_reverse_iterator(begin());
        }

        const SceneNode& operator[](size_type i) const {
            return *nodes_[i];
        }

        const SceneNode& at(size_type i) const {
            if (i >= nodes_.size()) {
                throw std::out_of_range("SceneChildren::at");
            }
            return *nodes_[i];
        }

        const SceneNode& front() const {
            return *nodes_.front();
        }

        const SceneNode& back() const {
            return *nodes_.back();
        }

        // Child i to change, unshared first.
        SceneNode& edit(size_type i);

        SceneNode& edit_back() {
            return edit(nodes_.size() - 1);
        }

        EditRange edit_all() {
            return EditRange(*this);
        }

        void push_back(const SceneNode& node);
        void push_back(SceneNode&& node);
        template <class... Args>
        SceneNode& emplace_back(Args&&... args);
        const_iterator insert(const_iterator at, SceneNode node);
        const_iterator erase(const_iterator at);

        void reverse() {
            std::reverse(nodes_.begin(), nodes_.end());
        }

        // Child i's storage, to share.
        Handle handle(size_type i) const {
            return nodes_[i];
        }

        // Appends, or puts at i, a child that shares the handle's storage.
        void push_back(Handle node);
        void set(size_type i, Handle node);

        // Equal handles, or equal values.
        friend bool operator==(const SceneChildren& a, const SceneChildren& b);

    private:
        Storage nodes_;
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
        SceneChildren children;
        std::optional<ScenePaintValues> paint; // Derived numeric cache, not DOM/style identity.
        // The -weft-effect() background and its parameters, resolved from the custom properties.
        std::shared_ptr<const SceneEffect> effect;
        // The url() background's image (SceneImages): straight sRGB RGBA8, one asset for one image, compared by
        // identity. None for another background, or a URL the images have no image of.
        std::shared_ptr<const paint::Asset> background_image;
        // The url() image of border-image-source (scene_border_image.hpp), drawn in nine pieces instead of the border
        // styles; none for no border image or a URL the images have no image of (the border styles draw then).
        std::shared_ptr<const paint::Asset> border_image;
        // The particle emissions the element shows at the frame, drawn after its background and borders, before its
        // content.
        std::vector<SceneEmission> particles;
        // What its data-anchor attributes declare: the element follows an anchor (scene_anchor.hpp) instead of the place
        // layout gives it.
        std::optional<SceneAnchoring> anchoring;
        // A scroll container (overflow hidden, auto or scroll): the scroll position its element asks for (scrollLeft,
        // scrollTop), and what layout made of it: the position by which its children's boxes are moved, clamped to
        // the content (x, y), and the size of that content from its padding box's start (scrollWidth, scrollHeight).
        // Zeros elsewhere.
        std::array<float, 2> scroll{};
        std::array<float, 4> scrolled{};
        // How far its scroll container moved its box (its parent's scrolled position when the box was placed), so
        // that layout can move it again from where layout put it.
        std::array<float, 2> scroll_shift{};

        friend bool operator==(const SceneNode& a, const SceneNode& b) {
            return a.identity == b.identity && a.tag == b.tag && a.space == b.space && a.text == b.text &&
                   a.attributes == b.attributes && a.style == b.style && a.box == b.box && a.lines == b.lines &&
                   a.fragments == b.fragments &&
                   (a.effect == b.effect || (a.effect && b.effect && *a.effect == *b.effect)) &&
                   a.background_image == b.background_image && a.border_image == b.border_image &&
                   a.particles == b.particles && a.anchoring == b.anchoring && a.scroll == b.scroll &&
                   a.scrolled == b.scrolled && a.scroll_shift == b.scroll_shift && a.children == b.children;
        }
    };

    // A scroll container's scroll geometry for script reads: its position and content size (layout's SceneNode::scrolled),
    // the axes script may scroll (overflow other than visible and clip) and those a wheel scrolls (auto or scroll, with
    // content past the client box). Nothing for another node.
    inline void scene_scroll_geometry(const SceneNode& n, ElementGeometry& e) {
        const auto overflow = n.style.text(PropertyId::Overflow, "visible");
        if (overflow == "visible" || overflow == "clip") {
            return;
        }
        e.scroll_left = n.scrolled[0];
        e.scroll_top = n.scrolled[1];
        e.scroll_width = std::max(e.client_width, double(n.scrolled[2]));
        e.scroll_height = std::max(e.client_height, double(n.scrolled[3]));
        e.scroll_x = e.scroll_y = true;
        const bool wheel = overflow == "auto" || overflow == "scroll";
        e.wheel_x = wheel && e.scroll_width > e.client_width;
        e.wheel_y = wheel && e.scroll_height > e.client_height;
    }

    inline SceneChildren::SceneChildren(std::initializer_list<SceneNode> nodes) {
        nodes_.reserve(nodes.size());
        for (const auto& n : nodes) {
            nodes_.push_back(std::make_shared<SceneNode>(n));
        }
    }

    inline SceneChildren::SceneChildren(std::vector<SceneNode> nodes) {
        nodes_.reserve(nodes.size());
        for (auto& n : nodes) {
            nodes_.push_back(std::make_shared<SceneNode>(std::move(n)));
        }
    }

    inline SceneChildren& SceneChildren::operator=(std::vector<SceneNode> nodes) {
        return *this = SceneChildren(std::move(nodes));
    }

    inline SceneChildren& SceneChildren::operator=(std::initializer_list<SceneNode> nodes) {
        Storage storage;
        storage.reserve(nodes.size());
        for (const auto& n : nodes) {
            storage.push_back(std::make_shared<SceneNode>(n));
        }
        nodes_ = std::move(storage);
        return *this;
    }

    inline SceneNode& SceneChildren::edit(size_type i) {
        auto& node = nodes_[i];
        if (node.use_count() > 1) {
            node = std::make_shared<SceneNode>(*node);
        } else {
            // Whoever released the last other handle read the node before; reading the count alone does not order
            // those reads before the changes that follow.
            std::atomic_thread_fence(std::memory_order_acquire);
        }
        return *node;
    }

    inline void SceneChildren::push_back(const SceneNode& node) {
        nodes_.push_back(std::make_shared<SceneNode>(node));
    }

    inline void SceneChildren::push_back(SceneNode&& node) {
        nodes_.push_back(std::make_shared<SceneNode>(std::move(node)));
    }

    template <class... Args>
    SceneNode& SceneChildren::emplace_back(Args&&... args) {
        return *nodes_.emplace_back(std::make_shared<SceneNode>(std::forward<Args>(args)...));
    }

    inline SceneChildren::const_iterator SceneChildren::insert(const_iterator at, SceneNode node) {
        return const_iterator(nodes_.insert(at.at_, std::make_shared<SceneNode>(std::move(node))));
    }

    inline SceneChildren::const_iterator SceneChildren::erase(const_iterator at) {
        return const_iterator(nodes_.erase(at.at_));
    }

    inline void SceneChildren::push_back(Handle node) {
        // Shared storage changes only through edit, which copies it first.
        nodes_.push_back(std::const_pointer_cast<SceneNode>(std::move(node)));
    }

    inline void SceneChildren::set(size_type i, Handle node) {
        nodes_[i] = std::const_pointer_cast<SceneNode>(std::move(node));
    }

    inline bool operator==(const SceneChildren& a, const SceneChildren& b) {
        if (a.nodes_.size() != b.nodes_.size()) {
            return false;
        }
        for (std::size_t i = 0; i < a.nodes_.size(); ++i) {
            if (a.nodes_[i] != b.nodes_[i] && !(*a.nodes_[i] == *b.nodes_[i])) {
                return false;
            }
        }
        return true;
    }

    // Whether two nodes show the same effect: none, or one of equal values.
    inline bool scene_effect_equal(const std::shared_ptr<const SceneEffect>& a,
                                   const std::shared_ptr<const SceneEffect>& b) {
        return a == b || (a && b && *a == *b);
    }

    // Whether two nodes hold the same values of their own, children aside, the numeric paint cache included.
    inline bool scene_own_equal(const SceneNode& a, const SceneNode& b) {
        return a.identity == b.identity && a.tag == b.tag && a.space == b.space && a.text == b.text &&
               a.attributes == b.attributes && a.style == b.style && a.box == b.box && a.lines == b.lines &&
               a.fragments == b.fragments && bool(a.paint) == bool(b.paint) &&
               (!a.paint || a.paint->values == b.paint->values) && scene_effect_equal(a.effect, b.effect) &&
               a.background_image == b.background_image && a.border_image == b.border_image &&
               a.particles == b.particles && a.anchoring == b.anchoring && a.scroll == b.scroll &&
               a.scrolled == b.scrolled && a.scroll_shift == b.scroll_shift;
    }

    // Whether a node's children may stand in another order than the document's (scene_child_order): an HTML flex or
    // grid container of two children or more (flex, inline-flex, grid and inline-grid).
    inline bool scene_orders_children(const SceneNode& n) {
        const auto display = n.style.text(PropertyId::Display, "inline");
        return n.children.size() > 1 && (n.space.empty() || n.space == constants::xhtml_namespace_uri) &&
               (display == "flex" || display == "inline-flex" || display == "grid" || display == "inline-grid");
    }

    // The order-modified document order of a node's children (CSS Display 3 §3.1): those of an HTML flex or grid
    // container stably by their order property (an absolutely positioned or fixed child at 0, CSS Flexbox 1 §5.4), any
    // other node's as they are. Layout places a container's items in this order, and painting and hit testing stack
    // the children in it, stably by z-index after.
    std::vector<std::size_t> scene_child_order(const SceneNode& n);

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
            a.background_image != b.background_image || a.border_image != b.border_image ||
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

    // The images of a scene's background-image: url() backgrounds, by the URL as the style writes it
    // (scene_values::image_url). Owner thread.
    class SceneImages {
    public:
        virtual ~SceneImages() = default;
        // The image url names, straight sRGB RGBA8: the same asset for a URL each time (nodes compare their images by
        // identity), or none when there is no such image (its background shows none).
        virtual std::shared_ptr<const paint::Asset> image(std::string_view url) = 0;
    };

    class SceneSolver {
    public:
        // Text is shaped and laid out in lines by `text`, which outlives the solver.
        SceneSolver(std::string stylesheet, std::string keyframes, SceneText& text, bool incremental = true);
        // forgiving: the stylesheet and keyframes taken as browsers take CSS (StyleProfile::forgiving), what they
        // cannot take dropped with a warning (take_warnings).
        SceneSolver(std::string stylesheet, std::string keyframes, SceneText& text, bool incremental, bool forgiving);
        ~SceneSolver();
        SceneSolver(const SceneSolver&) = delete;
        SceneSolver& operator=(const SceneSolver&) = delete;
        SceneFrame frame(const DomSnapshot&, float width, float height, double time_ms);
        // Owner-local retained scene; unchanged branches keep their owned values.
        // Publish a separate immutable snapshot before crossing threads. On error
        // discard the output and reset this solver before using it again.
        void update(const DomSnapshot&, SceneFrame&, float width, float height, double time_ms);
        void reset();
        // A forgiving solver's warnings since the last take: what its sheet, keyframes and cascades dropped.
        std::vector<std::string> take_warnings();
        void set_profiling(bool);
        // Whether frames carry the element geometry layout gives (SceneFrame::elements: border boxes without
        // transforms, in tree order); on at first. A host that computes geometry itself (SceneGeometry, as View does)
        // turns it off: frames' elements are then empty, and layout skips the walk that lists them.
        void set_element_geometry(bool);
        // The effects background-image: -weft-effect(name) names; the host keeps them alive. Without them such a
        // background is a scene profile diagnostic.
        void set_effects(const SceneEffects* effects);
        // The images background-image: url() names; the host keeps them alive. Without them such a background is a
        // scene profile diagnostic.
        void set_images(SceneImages* images);
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
            // Layouts of the subtrees around what changed alone (each one of layout_evaluations).
            std::size_t partial_layouts{};
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

    // The scene's style profile; forgiving: CSS as browsers take it (StyleProfile::forgiving).
    StyleProfile scene_style_profile(bool forgiving = false);

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
