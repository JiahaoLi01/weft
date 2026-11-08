#include "scene/scene_field.hpp"

#include <algorithm>
#include <cmath>
#include <limits>

namespace weft {
    namespace {
        // The bytes of the UTF-8 sequence that starts with b (1 for a stray continuation byte).
        std::size_t sequence(unsigned char b) {
            return b < 0x80 ? 1 : b >= 0xF0 ? 4 : b >= 0xE0 ? 3 : b >= 0xC0 ? 2 : 1;
        }

        constexpr std::string_view bullet = "\xE2\x80\xA2";

        std::size_t characters(std::string_view text, std::size_t end) {
            std::size_t count{};
            for (std::size_t i = 0; i < text.size() && i < end; i += sequence(static_cast<unsigned char>(text[i]))) {
                ++count;
            }
            return count;
        }
    } // namespace

    std::string scene_field_text(const TextFieldState& f, bool& placeholder) {
        placeholder = f.value.empty() && !f.placeholder.empty();
        if (placeholder) {
            return f.placeholder;
        }
        if (!f.password) {
            return f.value;
        }
        std::string out;
        out.reserve(characters(f.value, f.value.size()) * bullet.size());
        for (auto n = characters(f.value, f.value.size()); n > 0; --n) {
            out += bullet;
        }
        return out;
    }

    std::uint32_t scene_field_shown(const TextFieldState& f, std::uint32_t value_offset) {
        if (!f.password) {
            return value_offset;
        }
        return static_cast<std::uint32_t>(characters(f.value, value_offset) * bullet.size());
    }

    std::uint32_t scene_field_value(const TextFieldState& f, std::uint32_t shown_offset) {
        if (!f.password) {
            return std::min<std::uint32_t>(shown_offset, static_cast<std::uint32_t>(f.value.size()));
        }
        auto n = shown_offset / bullet.size();
        std::size_t i = 0;
        while (i < f.value.size() && n > 0) {
            i += sequence(static_cast<unsigned char>(f.value[i]));
            --n;
        }
        return static_cast<std::uint32_t>(std::min(i, f.value.size()));
    }

    namespace {
        // Each cluster of a run by its first byte: its left x, advance and whether it runs right to left; in byte
        // order.
        struct Cluster {
            std::uint32_t first{};
            float x{};
            float advance{};
            bool rtl{};
        };

        std::vector<Cluster> clusters_of(const SceneTextRun& run) {
            std::vector<Cluster> clusters;
            for (const auto& g : run.glyphs) {
                auto same = std::find_if(clusters.begin(), clusters.end(), [&](const Cluster& c) {
                    return c.first == g.utf8_cluster;
                });
                if (same != clusters.end()) {
                    const float right = std::max(same->x + same->advance, g.x + g.advance);
                    same->x = std::min(same->x, g.x);
                    same->advance = right - same->x;
                    continue;
                }
                clusters.push_back({g.utf8_cluster, g.x, g.advance, g.rtl});
            }
            std::sort(clusters.begin(), clusters.end(), [](const Cluster& a, const Cluster& b) {
                return a.first < b.first;
            });
            return clusters;
        }

        // The characters of a cluster split its advance: each one's offset, left and right x and direction, in byte
        // order.
        template <typename F>
        void characters(const std::vector<Cluster>& clusters, std::string_view text, F&& each) {
            for (std::size_t k = 0; k < clusters.size(); ++k) {
                const auto& c = clusters[k];
                const auto last = k + 1 < clusters.size() ? clusters[k + 1].first : std::uint32_t(text.size());
                std::vector<std::uint32_t> boundaries;
                for (auto i = c.first; i < last && i < text.size();
                     i += static_cast<std::uint32_t>(sequence(static_cast<unsigned char>(text[i])))) {
                    boundaries.push_back(i);
                }
                const float share = c.advance / float(std::max<std::size_t>(boundaries.size(), 1));
                for (std::size_t b = 0; b < boundaries.size(); ++b) {
                    // A right-to-left cluster's first character is on its right.
                    const float left = c.rtl ? c.x + c.advance - share * float(b + 1) : c.x + share * float(b);
                    each(boundaries[b], left, left + share, c.rtl);
                }
            }
        }
    } // namespace

    std::vector<std::pair<std::uint32_t, float>> scene_field_stops(const SceneTextRun& run, std::string_view text,
                                                                   std::uint32_t start) {
        std::vector<std::pair<std::uint32_t, float>> stops;
        float trailing = 0;
        characters(clusters_of(run), text, [&](std::uint32_t offset, float left, float right, bool rtl) {
            stops.push_back({start + offset, rtl ? right : left});
            trailing = rtl ? left : right;
        });
        if (stops.empty() || stops.front().first != start) {
            stops.insert(stops.begin(), {start, 0.f});
        }
        stops.push_back({start + static_cast<std::uint32_t>(text.size()), trailing});
        return stops;
    }

    std::vector<std::pair<std::uint32_t, std::array<float, 2>>>
    scene_field_extents(const SceneTextRun& run, std::string_view text, std::uint32_t start) {
        std::vector<std::pair<std::uint32_t, std::array<float, 2>>> extents;
        if (std::none_of(run.glyphs.begin(), run.glyphs.end(), [](const SceneGlyph& g) {
                return g.rtl;
            })) {
            return extents;
        }
        characters(clusters_of(run), text, [&](std::uint32_t offset, float left, float right, bool) {
            extents.push_back({start + offset, {left, right}});
        });
        return extents;
    }

    std::vector<std::array<float, 2>> scene_field_ranges(const SceneLine& line, std::uint32_t begin,
                                                         std::uint32_t end) {
        std::vector<std::array<float, 2>> out;
        if (line.extents.empty()) {
            const auto at = [&](std::uint32_t offset) {
                float x = 0;
                for (const auto& [o, stop] : line.stops) {
                    if (o > offset) {
                        break;
                    }
                    x = stop;
                }
                return x;
            };
            out.push_back({at(begin), at(end)});
            return out;
        }
        for (const auto& [offset, x] : line.extents) {
            if (offset >= begin && offset < end) {
                out.push_back(x);
            }
        }
        std::sort(out.begin(), out.end());
        std::vector<std::array<float, 2>> merged;
        for (const auto& r : out) {
            if (!merged.empty() && r[0] <= merged.back()[1] + 0.01f) {
                merged.back()[1] = std::max(merged.back()[1], r[1]);
            } else {
                merged.push_back(r);
            }
        }
        return merged;
    }

    std::optional<SceneFieldCaret> scene_field_caret(const SceneNode& n, std::uint32_t shown_offset) {
        if (n.lines.empty()) {
            return std::nullopt;
        }
        std::size_t line = 0;
        for (std::size_t k = 1; k < n.lines.size(); ++k) {
            if (n.lines[k].start <= shown_offset) {
                line = k;
            }
        }
        const auto& l = n.lines[line];
        float x = 0;
        for (const auto& [offset, stop] : l.stops) {
            if (offset > shown_offset) {
                break;
            }
            x = stop;
        }
        x += l.rect[0];
        // Bidirectional text: a field's line is a run of one direction (a piece of its visual line). Where runs of two
        // levels meet, the caret stands on the lower one's side, as Blink resolves a caret at a bidi boundary: a run
        // of the field's direction is of the lower level; the paragraph's start and end are of the field's level, at
        // the visual line's start and end edges.
        if (l.bidi) {
            const bool base_rtl = n.style.text(PropertyId::Direction, "ltr") == "rtl";
            const auto lower = [&](const SceneLine& s) {
                return s.rtl == base_rtl;
            };
            const auto same_line = [&](const SceneLine& s) {
                return std::abs(s.rect[1] - l.rect[1]) < 0.5f;
            };
            float left = l.rect[0];
            float right = l.rect[0] + l.rect[2];
            for (const auto& s : n.lines) {
                if (same_line(s) && !s.text.empty()) {
                    left = std::min(left, s.rect[0]);
                    right = std::max(right, s.rect[0] + s.rect[2]);
                }
            }
            const auto end_of = [](const SceneLine& s) {
                return s.start + std::uint32_t(s.text.size());
            };
            if (!lower(l) && shown_offset == l.start) {
                const SceneLine* before = nullptr;
                for (const auto& s : n.lines) {
                    if (&s != &l && same_line(s) && end_of(s) == shown_offset && !s.text.empty()) {
                        before = &s;
                    }
                }
                if (!before) {
                    x = base_rtl ? right : left;
                } else if (lower(*before) && !before->stops.empty()) {
                    x = before->rect[0] + before->stops.back().second;
                }
            } else if (!lower(l) && shown_offset == end_of(l)) {
                const bool after = std::any_of(n.lines.begin(), n.lines.end(), [&](const SceneLine& s) {
                    return &s != &l && same_line(s) && s.start == shown_offset && !s.text.empty();
                });
                if (!after) {
                    x = base_rtl ? left : right;
                }
            }
        }
        return SceneFieldCaret{line, x};
    }

    std::uint32_t scene_field_offset_at(const SceneNode& n, float x, float y) {
        if (n.lines.empty()) {
            return 0;
        }
        std::size_t line = 0;
        float nearest = std::numeric_limits<float>::infinity();
        for (std::size_t k = 0; k < n.lines.size(); ++k) {
            const auto& r = n.lines[k].rect;
            const float d = y < r[1] ? r[1] - y : y > r[1] + r[3] ? y - r[1] - r[3] : 0;
            if (d < nearest) {
                nearest = d;
                line = k;
            }
        }
        const auto& l = n.lines[line];
        std::uint32_t best = l.start;
        float best_distance = std::numeric_limits<float>::infinity();
        for (const auto& [offset, stop] : l.stops) {
            const float d = std::abs(l.rect[0] + stop - x);
            if (d < best_distance) {
                best_distance = d;
                best = offset;
            }
        }
        return best;
    }
} // namespace weft
