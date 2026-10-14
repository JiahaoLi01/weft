#pragma once

#include "base/result.hpp"
#include "dom/node.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <utility>
#include <vector>

// Anchors (docs/engine-communication.md, section 5): each frame an engine writes where its anchors lie in a View, and an
// element with data-anchor follows its anchor without JS, DOM changes, styles or layout. The engine writes a whole table
// at a time from any thread; the render thread draws with the latest table it took, the owner hit-tests and measures
// with the latest it took.
namespace weft {
    // Flags of an anchor.
    namespace scene_anchor_flags {
        // The engine hides the anchor's elements: they draw nothing and take no hits.
        inline constexpr std::uint32_t hidden = 1;
        // The point lies behind the camera; its elements are off the View (data-anchor-offscreen).
        inline constexpr std::uint32_t behind = 2;
        // The engine counts the point off screen wherever it lies.
        inline constexpr std::uint32_t offscreen = 4;
        inline constexpr std::uint32_t known = hidden | behind | offscreen;
    } // namespace scene_anchor_flags

    // Where an anchor lies this frame: its point in the View's logical px (CSS px), the scale its elements take about
    // their pivot (1 for their own size), its depth (farther is greater) and flags. As the C ABI's weft_anchor, 32 bytes.
    struct SceneAnchor {
        std::uint64_t key{};
        float x{};
        float y{};
        float scale = 1;
        float depth{};
        std::uint32_t flags{};
        std::uint32_t reserved{};

        friend bool operator==(const SceneAnchor&, const SceneAnchor&) = default;
    };

    static_assert(sizeof(SceneAnchor) == 32);

    // A View's anchors as one write left them: one per key, in key order. serial counts the writes up to it (0 before
    // the first).
    class SceneAnchorTable {
    public:
        // The anchor of key; null when the table has none.
        const SceneAnchor* find(std::uint64_t key) const;

        std::span<const SceneAnchor> anchors() const {
            return anchors_;
        }

        std::uint64_t serial() const {
            return serial_;
        }

    private:
        friend class SceneAnchorChannel;
        std::vector<SceneAnchor> anchors_;
        std::uint64_t serial_{};
    };

    // Why a write published nothing.
    enum class SceneAnchorWrite : std::uint8_t {
        Written,
        // A value that is not finite, a negative scale, flags past scene_anchor_flags::known, or reserved bits.
        Invalid,
        // More than constants::scene::max_anchors anchors.
        TooMany
    };

    // A View's anchors between the engine and the View's two readers: the render thread, which draws with them, and the
    // owner, which hit-tests and measures with them. A write replaces every anchor; each reader takes the latest whole
    // table written and keeps it until its next take, never waiting for the writer or the other reader (each reader has
    // a triple buffer of its own: the writer fills a free table and swaps it in, the reader swaps the newest out).
    //
    // write: any thread, one at a time (consecutive writes on different threads must be ordered by the caller). take:
    // the reader's thread alone.
    class SceneAnchorChannel {
    public:
        enum class Reader : unsigned {
            Render,
            Owner
        };

        // Replaces the anchors with a copy of anchors; of entries with the same key the last counts. Invalid and
        // TooMany publish nothing.
        SceneAnchorWrite write(std::span<const SceneAnchor> anchors);

        // The latest table written (an empty one before any write), the reader's until its next take.
        const SceneAnchorTable& take(Reader reader);

        // Writes so far.
        std::uint64_t writes() const {
            return serial_.load(std::memory_order_relaxed);
        }

    private:
        // A reader's tables: the writer owns back, the reader front, and shared names the third and whether it is newer
        // than the reader's (fresh).
        struct Triple {
            static constexpr unsigned fresh = 4;
            std::array<SceneAnchorTable, 3> tables;
            std::atomic<unsigned> shared{1};
            unsigned back{};
            unsigned front = 2;
        };

        std::array<Triple, 2> readers_;
        std::atomic<std::uint64_t> serial_{};
    };

    // What an element's data-anchor attributes say (docs/engine-communication.md, 5.2): the key it follows; the point of
    // its border box put on the anchor, a fraction of the box plus px per axis (data-anchor-pivot, default 50% 50%);
    // whether it takes the anchor's scale (data-anchor-scale); what it does off the View (data-anchor-offscreen, with
    // data-anchor-margin for clamp) and while the table lacks its key (data-anchor-missing).
    struct SceneAnchoring {
        enum class Offscreen : std::uint8_t {
            Hide,
            Show,
            Clamp
        };

        enum class Missing : std::uint8_t {
            Hide,
            Stay
        };

        std::uint64_t key{};
        std::array<float, 2> pivot_fraction{.5f, .5f};
        std::array<float, 2> pivot_px{};
        bool scale = true;
        Offscreen offscreen = Offscreen::Hide;
        float margin{};
        Missing missing = Missing::Hide;

        friend bool operator==(const SceneAnchoring&, const SceneAnchoring&) = default;
    };

    // The anchoring an element's attributes declare; nullopt without data-anchor. A value outside the grammar is a
    // scene profile diagnostic: a key of decimal digits below 2^64; a pivot of two lengths, each a percentage or px; on
    // or off; hide, show or clamp; a margin in px, at least 0; hide or stay.
    Result<std::optional<SceneAnchoring>> scene_anchoring(const AttributeMap& attributes);

    // The anchor an element took the last time a list placed it, with the key it then had: data-anchor-missing: stay
    // keeps it while the table lacks the key.
    struct ScenePlacedAnchor {
        NodeIdentity element;
        SceneAnchor anchor;
    };

    // The anchors one consumer places a frame's anchored elements with: the table it took (none counts as empty) and
    // what its previous frame placed (scene_anchor_resolve), for data-anchor-missing: stay.
    struct SceneAnchorInput {
        const SceneAnchorTable* table{};
        const std::vector<ScenePlacedAnchor>* last{};
    };

    // The anchor an anchored element takes: its key's in the table, else for missing: stay the one it last took under
    // the same key; null for none (it is hidden). When placed is given, records what the element took for the next
    // frame's last (sort it with scene_anchor_sort before use).
    const SceneAnchor* scene_anchor_resolve(const SceneAnchorInput& input, NodeIdentity element,
                                            const SceneAnchoring& anchoring,
                                            std::vector<ScenePlacedAnchor>* placed = nullptr);
    // Sorts placed anchors by element and keeps one per element, as scene_anchor_resolve finds them.
    void scene_anchor_sort(std::vector<ScenePlacedAnchor>& placed);

    // The map from an anchored element's border box, at the origin and after its own transform own (row-vector
    // {m11, m12, m21, m22, dx, dy}, as SceneAffine), into the View (view: its width and height): its pivot on the
    // anchor, scaled about it by the anchor's scale for data-anchor-scale: on, and for data-anchor-offscreen: clamp
    // moved the least that keeps the bounds of its box inside the View inset by the margin (centred on an axis it
    // cannot fit). nullopt when the element is hidden: no anchor, a hidden anchor, or for offscreen: hide one off the
    // View (outside it, behind the camera or flagged off screen).
    std::optional<std::array<double, 6>> scene_anchor_map(const SceneAnchoring& anchoring, const SceneAnchor* anchor,
                                                          std::array<double, 2> box, std::array<double, 2> view,
                                                          const std::array<double, 6>& own);

    // Orders anchored children far to near (greater depth first) among the places they hold in a run of children of
    // equal z-index; children of equal depth, and children that are not anchored, keep their places. order holds child
    // indices sorted by z-index; z(i) is child i's z-index and depth(i) its anchor's depth, or nullopt when it is not
    // anchored.
    template <class Z, class Depth>
    void scene_anchor_order(std::vector<std::size_t>& order, Z&& z, Depth&& depth) {
        std::vector<std::size_t> places;
        std::vector<std::pair<float, std::size_t>> anchored;
        for (std::size_t begin = 0; begin < order.size();) {
            auto end = begin + 1;
            while (end < order.size() && z(order[end]) == z(order[begin])) {
                ++end;
            }
            places.clear();
            anchored.clear();
            for (auto k = begin; k < end; ++k) {
                if (const auto d = depth(order[k])) {
                    places.push_back(k);
                    anchored.emplace_back(*d, order[k]);
                }
            }
            if (anchored.size() > 1) {
                std::stable_sort(anchored.begin(), anchored.end(), [](const auto& a, const auto& b) {
                    return a.first > b.first;
                });
                for (std::size_t k = 0; k < places.size(); ++k) {
                    order[places[k]] = anchored[k].second;
                }
            }
            begin = end;
        }
    }
} // namespace weft
