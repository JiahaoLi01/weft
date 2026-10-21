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

    std::vector<std::pair<std::uint32_t, float>> scene_field_stops(const SceneTextRun& run, std::string_view text,
                                                                   std::uint32_t start) {
        // Each cluster's left x and advance, by its first byte.
        std::vector<std::pair<std::uint32_t, std::pair<float, float>>> clusters;
        for (const auto& g : run.glyphs) {
            if (!clusters.empty() && clusters.back().first == g.utf8_cluster) {
                auto& [x, advance] = clusters.back().second;
                const float right = std::max(x + advance, g.x + g.advance);
                x = std::min(x, g.x);
                advance = right - x;
                continue;
            }
            clusters.push_back({g.utf8_cluster, {g.x, g.advance}});
        }
        std::sort(clusters.begin(), clusters.end(), [](const auto& a, const auto& b) {
            return a.first < b.first;
        });
        std::vector<std::pair<std::uint32_t, float>> stops;
        float end = 0;
        for (std::size_t k = 0; k < clusters.size(); ++k) {
            const auto first = clusters[k].first;
            const auto last = k + 1 < clusters.size() ? clusters[k + 1].first : static_cast<std::uint32_t>(text.size());
            const auto [x, advance] = clusters[k].second;
            // The characters of the cluster split its advance.
            std::vector<std::uint32_t> boundaries;
            for (auto i = first; i < last && i < text.size();
                 i += static_cast<std::uint32_t>(sequence(static_cast<unsigned char>(text[i])))) {
                boundaries.push_back(i);
            }
            for (std::size_t b = 0; b < boundaries.size(); ++b) {
                stops.push_back({start + boundaries[b], x + advance * float(b) / float(boundaries.size())});
            }
            end = std::max(end, x + advance);
        }
        if (stops.empty() || stops.front().first != start) {
            stops.insert(stops.begin(), {start, 0.f});
        }
        stops.push_back({start + static_cast<std::uint32_t>(text.size()), std::max(end, stops.back().second)});
        return stops;
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
        return SceneFieldCaret{line, l.rect[0] + x};
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
