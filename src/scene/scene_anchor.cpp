#include "scene/scene_anchor.hpp"

#include "base/css_scalar.hpp"
#include "constants/scene.hpp"

#include <charconv>
#include <cmath>
#include <limits>
#include <string>
#include <string_view>
#include <tuple>

namespace weft {
    namespace {
        bool finite(float v) {
            return std::isfinite(v);
        }

        bool before(const NodeIdentity& a, const NodeIdentity& b) {
            return std::tie(a.view_epoch, a.slot, a.generation) < std::tie(b.view_epoch, b.slot, b.generation);
        }

        Diagnostic invalid(std::string_view name, std::string_view value) {
            return make_diagnostic(Diag::SceneProfile, std::string(name) + " " + std::string(value));
        }
    } // namespace

    const SceneAnchor* SceneAnchorTable::find(std::uint64_t key) const {
        const auto found = std::lower_bound(anchors_.begin(), anchors_.end(), key, [](const SceneAnchor& a, auto k) {
            return a.key < k;
        });
        return found != anchors_.end() && found->key == key ? &*found : nullptr;
    }

    SceneAnchorWrite SceneAnchorChannel::write(std::span<const SceneAnchor> anchors) {
        if (anchors.size() > constants::scene::max_anchors) {
            return SceneAnchorWrite::TooMany;
        }
        for (const auto& a : anchors) {
            if (!finite(a.x) || !finite(a.y) || !finite(a.scale) || !finite(a.depth) || a.scale < 0 ||
                (a.flags & ~scene_anchor_flags::known) || a.reserved) {
                return SceneAnchorWrite::Invalid;
            }
        }
        for (auto& r : readers_) {
            r.tables[r.back].anchors_.reserve(anchors.size());
        }
        const auto serial = serial_.load(std::memory_order_relaxed) + 1;
        // In key order, the entries of a key in the order written (reserved holds the place while sorting), then the
        // last of each key.
        auto& sorted = readers_[0].tables[readers_[0].back];
        sorted.anchors_.assign(anchors.begin(), anchors.end());
        for (std::size_t i = 0; i < sorted.anchors_.size(); ++i) {
            sorted.anchors_[i].reserved = std::uint32_t(i);
        }
        std::sort(sorted.anchors_.begin(), sorted.anchors_.end(), [](const SceneAnchor& a, const SceneAnchor& b) {
            return a.key != b.key ? a.key < b.key : a.reserved < b.reserved;
        });
        std::size_t kept{};
        for (std::size_t i = 0; i < sorted.anchors_.size(); ++i) {
            if (i + 1 < sorted.anchors_.size() && sorted.anchors_[i + 1].key == sorted.anchors_[i].key) {
                continue;
            }
            sorted.anchors_[kept] = sorted.anchors_[i];
            sorted.anchors_[kept++].reserved = 0;
        }
        sorted.anchors_.resize(kept);
        sorted.serial_ = serial;
        for (std::size_t k = 1; k < readers_.size(); ++k) {
            auto& table = readers_[k].tables[readers_[k].back];
            table.anchors_.assign(sorted.anchors_.begin(), sorted.anchors_.end());
            table.serial_ = serial;
        }
        // Publish: each reader's back becomes its newest table, and the table it replaces the writer's next back.
        for (auto& r : readers_) {
            r.back = r.shared.exchange(r.back | Triple::fresh, std::memory_order_acq_rel) & ~Triple::fresh;
        }
        serial_.store(serial, std::memory_order_relaxed);
        return SceneAnchorWrite::Written;
    }

    const SceneAnchorTable& SceneAnchorChannel::take(Reader reader) {
        auto& r = readers_[unsigned(reader)];
        if (r.shared.load(std::memory_order_relaxed) & Triple::fresh) {
            r.front = r.shared.exchange(r.front, std::memory_order_acq_rel) & ~Triple::fresh;
        }
        return r.tables[r.front];
    }

    Result<std::optional<SceneAnchoring>> scene_anchoring(const AttributeMap& attributes) {
        static constexpr std::string_view prefix = "data-anchor";
        const auto declared = attributes.find(prefix);
        if (declared == attributes.end()) {
            return std::optional<SceneAnchoring>{};
        }
        SceneAnchoring out;
        const auto key = css::trim(declared->second);
        const auto parsed = std::from_chars(key.data(), key.data() + key.size(), out.key);
        if (key.empty() || key.size() > constants::scene::max_anchor_key_digits ||
            key.find_first_not_of("0123456789") != std::string_view::npos || parsed.ec != std::errc{} ||
            parsed.ptr != key.data() + key.size()) {
            return invalid("data-anchor key", declared->second);
        }
        // A length: a percentage of extent, or px (a bare 0 too).
        const auto length = [](std::string_view text, float& fraction, float& px) {
            const auto [digits, unit] = css::split_unit(text, {.px = true, .percent = true});
            const auto n = css::parse_number<float>(std::string(digits),
                                                    static_cast<float>(constants::scene::max_numeric_magnitude));
            if (!n || (unit == css::Unit::None && *n != 0)) {
                return false;
            }
            (unit == css::Unit::Percent ? fraction : px) = unit == css::Unit::Percent ? *n / 100 : *n;
            return true;
        };
        // The other data-anchor- attributes, which sort right after it.
        static constexpr std::string_view options = "data-anchor-";
        for (auto it = attributes.lower_bound(options);
             it != attributes.end() && std::string_view(it->first).starts_with(options); ++it) {
            const std::string_view name = it->first;
            const auto value = css::trim(it->second);
            if (name == "data-anchor-pivot") {
                const auto lengths = css::split(value, ' ', {.quotes = false}).parts;
                out.pivot_fraction = {};
                if (lengths.size() != 2 || !length(lengths[0], out.pivot_fraction[0], out.pivot_px[0]) ||
                    !length(lengths[1], out.pivot_fraction[1], out.pivot_px[1])) {
                    return invalid(name, it->second);
                }
            } else if (name == "data-anchor-scale") {
                if (value != "on" && value != "off") {
                    return invalid(name, it->second);
                }
                out.scale = value == "on";
            } else if (name == "data-anchor-offscreen") {
                if (value == "hide") {
                    out.offscreen = SceneAnchoring::Offscreen::Hide;
                } else if (value == "show") {
                    out.offscreen = SceneAnchoring::Offscreen::Show;
                } else if (value == "clamp") {
                    out.offscreen = SceneAnchoring::Offscreen::Clamp;
                } else {
                    return invalid(name, it->second);
                }
            } else if (name == "data-anchor-margin") {
                float fraction{};
                if (!length(value, fraction, out.margin) || value.ends_with('%') || out.margin < 0) {
                    return invalid(name, it->second);
                }
            } else if (name == "data-anchor-missing") {
                if (value != "hide" && value != "stay") {
                    return invalid(name, it->second);
                }
                out.missing = value == "stay" ? SceneAnchoring::Missing::Stay : SceneAnchoring::Missing::Hide;
            } else {
                return invalid("unknown attribute", name);
            }
        }
        return std::optional<SceneAnchoring>{out};
    }

    const SceneAnchor* scene_anchor_resolve(const SceneAnchorInput& input, NodeIdentity element,
                                            const SceneAnchoring& anchoring, std::vector<ScenePlacedAnchor>* placed) {
        const SceneAnchor* anchor = input.table ? input.table->find(anchoring.key) : nullptr;
        if (!anchor && anchoring.missing == SceneAnchoring::Missing::Stay && input.last) {
            const auto& last = *input.last;
            const auto found = std::lower_bound(last.begin(), last.end(), element,
                                                [](const ScenePlacedAnchor& p, const NodeIdentity& e) {
                                                    return before(p.element, e);
                                                });
            if (found != last.end() && found->element == element && found->anchor.key == anchoring.key) {
                anchor = &found->anchor;
            }
        }
        if (anchor && placed) {
            placed->push_back({element, *anchor});
        }
        return anchor;
    }

    void scene_anchor_sort(std::vector<ScenePlacedAnchor>& placed) {
        std::sort(placed.begin(), placed.end(), [](const ScenePlacedAnchor& a, const ScenePlacedAnchor& b) {
            return before(a.element, b.element);
        });
        placed.erase(std::unique(placed.begin(), placed.end(),
                                 [](const ScenePlacedAnchor& a, const ScenePlacedAnchor& b) {
                                     return a.element == b.element;
                                 }),
                     placed.end());
    }

    std::optional<std::array<double, 6>> scene_anchor_map(const SceneAnchoring& anchoring, const SceneAnchor* anchor,
                                                          std::array<double, 2> box, std::array<double, 2> view,
                                                          const std::array<double, 6>& own) {
        if (!anchor || (anchor->flags & scene_anchor_flags::hidden)) {
            return std::nullopt;
        }
        const bool off = (anchor->flags & (scene_anchor_flags::behind | scene_anchor_flags::offscreen)) ||
                         !(anchor->x >= 0 && anchor->x <= view[0] && anchor->y >= 0 && anchor->y <= view[1]);
        if (off && anchoring.offscreen == SceneAnchoring::Offscreen::Hide) {
            return std::nullopt;
        }
        const double s = anchoring.scale ? double(anchor->scale) : 1;
        const double px = double(anchoring.pivot_fraction[0]) * box[0] + anchoring.pivot_px[0];
        const double py = double(anchoring.pivot_fraction[1]) * box[1] + anchoring.pivot_px[1];
        std::array<double, 6> map{s, 0, 0, s, anchor->x - s * px, anchor->y - s * py};
        if (anchoring.offscreen == SceneAnchoring::Offscreen::Clamp) {
            // The bounds of the box under its own transform, then the map.
            constexpr double unbounded = std::numeric_limits<double>::infinity();
            std::array<double, 4> bounds{unbounded, unbounded, -unbounded, -unbounded};
            for (const auto& [x, y] :
                 std::array<std::array<double, 2>, 4>{{{0, 0}, {box[0], 0}, {0, box[1]}, {box[0], box[1]}}}) {
                const double ox = x * own[0] + y * own[2] + own[4];
                const double oy = x * own[1] + y * own[3] + own[5];
                const double vx = ox * map[0] + map[4];
                const double vy = oy * map[3] + map[5];
                bounds = {std::min(bounds[0], vx), std::min(bounds[1], vy), std::max(bounds[2], vx),
                          std::max(bounds[3], vy)};
            }
            const auto shift = [&](double low, double high, double extent) {
                const double inner_low = anchoring.margin;
                const double inner_high = extent - anchoring.margin;
                if (high - low > inner_high - inner_low) {
                    return (inner_low + inner_high) / 2 - (low + high) / 2;
                }
                return low < inner_low ? inner_low - low : high > inner_high ? inner_high - high : 0.0;
            };
            map[4] += shift(bounds[0], bounds[2], view[0]);
            map[5] += shift(bounds[1], bounds[3], view[1]);
        }
        return map;
    }
} // namespace weft
