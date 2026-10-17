#include "layout/grid.hpp"
#include "base/css_scalar.hpp"
#include "constants/layout.hpp"
#include <algorithm>
#include <cmath>
#include <limits>
#include <map>
#include <optional>
#include <stdexcept>
#include <string>

namespace weft::grid {
    namespace {
        using Kind = Breadth::Kind;
        constexpr float infinite = std::numeric_limits<float>::infinity();

        [[noreturn]] void refuse(std::string_view property, std::string_view value, const std::string& why) {
            throw std::invalid_argument("WEFT_UNSUPPORTED_FORMAT: " + std::string(property) + ' ' + std::string(value) +
                                        ": " + why);
        }

        // The value trimmed, each run of whitespace one space, in ASCII lower case (keywords and units are
        // case-insensitive; the names that are not are outside the profile).
        std::string normalized(std::string_view value) {
            auto text = css::collapse_spaces(css::trim(value));
            std::transform(text.begin(), text.end(), text.begin(), [](unsigned char c) {
                return char(c >= 'A' && c <= 'Z' ? c - 'A' + 'a' : c);
            });
            return text;
        }

        // The arguments of name(...) when that is the whole text, each trimmed; nothing for any other text.
        std::optional<std::vector<std::string>> arguments(std::string_view text, std::string_view name) {
            if (text.size() < name.size() + 2 || !text.starts_with(name) || text[name.size()] != '(' ||
                !text.ends_with(')')) {
                return std::nullopt;
            }
            const auto inside = text.substr(name.size() + 1, text.size() - name.size() - 2);
            auto split = css::split(inside, ',', {.quotes = true, .brackets = false, .trim_parts = false});
            if (split.nesting != css::Nesting::Balanced) {
                return std::nullopt;
            }
            std::vector<std::string> out;
            for (const auto& part : split.parts) {
                out.push_back(css::trim(part));
            }
            return out;
        }

        // A number of the decimal syntax, not negative, within the layout bound.
        std::optional<float> amount(std::string_view digits) {
            if (!css::decimal_syntax(digits) || digits.starts_with('-')) {
                return std::nullopt;
            }
            const auto n = css::parse_number<float>(std::string(digits),
                                                    static_cast<float>(constants::layout::max_flex_number_magnitude));
            if (!n) {
                return std::nullopt;
            }
            return *n;
        }

        // An <integer>: an optional sign and decimal digits, its magnitude clamped to the layout bound.
        std::optional<long long> integer(std::string_view text) {
            const std::size_t first = text.starts_with('+') || text.starts_with('-') ? 1 : 0;
            if (first == text.size()) {
                return std::nullopt;
            }
            long long value{};
            for (std::size_t i = first; i < text.size(); ++i) {
                if (text[i] < '0' || text[i] > '9') {
                    return std::nullopt;
                }
                value = std::min(value * 10 + (text[i] - '0'),
                                 static_cast<long long>(constants::layout::max_flex_number_magnitude));
            }
            return text[0] == '-' ? -value : value;
        }

        // A <track-breadth> (§7.2.2): px, %, fr, auto, min-content or max-content; a unitless 0 is 0px.
        std::optional<Breadth> breadth(std::string_view text) {
            if (text == "auto") {
                return Breadth{Kind::Auto};
            }
            if (text == "min-content") {
                return Breadth{Kind::MinContent};
            }
            if (text == "max-content") {
                return Breadth{Kind::MaxContent};
            }
            if (text == "0") {
                return Breadth{Kind::Length};
            }
            for (const auto& [suffix, kind] :
                 {std::pair<std::string_view, Kind>{"fr", Kind::Flex}, {"px", Kind::Length}, {"%", Kind::Percent}}) {
                if (text.size() > suffix.size() && text.ends_with(suffix)) {
                    if (const auto n = amount(text.substr(0, text.size() - suffix.size()))) {
                        return Breadth{kind, *n};
                    }
                    return std::nullopt;
                }
            }
            return std::nullopt;
        }

        // A <track-size> (§7.2.3): a breadth, minmax(<inflexible-breadth>, <track-breadth>) or fit-content().
        std::optional<TrackSize> track_size(std::string_view text) {
            if (const auto args = arguments(text, "minmax")) {
                if (args->size() != 2) {
                    return std::nullopt;
                }
                const auto min = breadth((*args)[0]);
                const auto max = breadth((*args)[1]);
                if (!min || !max || min->kind == Kind::Flex) {
                    return std::nullopt;
                }
                return TrackSize{*min, *max};
            }
            if (const auto args = arguments(text, "fit-content")) {
                if (args->size() != 1) {
                    return std::nullopt;
                }
                const auto limit = breadth((*args)[0]);
                if (!limit || (limit->kind != Kind::Length && limit->kind != Kind::Percent)) {
                    return std::nullopt;
                }
                return TrackSize{Breadth{Kind::Auto},
                                 Breadth{Kind::FitContent, limit->value, limit->kind == Kind::Percent}};
            }
            const auto single = breadth(text);
            if (!single) {
                return std::nullopt;
            }
            if (single->kind == Kind::Flex) {
                return TrackSize{Breadth{Kind::Auto}, *single};
            }
            return TrackSize{*single, *single};
        }

        bool fixed(const Breadth& b) {
            return b.kind == Kind::Length || b.kind == Kind::Percent;
        }

        // A <fixed-size> (§7.2): a fixed minimum, or a fixed maximum (the minimum is never flexible).
        bool fixed_size(const TrackSize& t) {
            return fixed(t.min) || fixed(t.max);
        }

        bool intrinsic(const Breadth& b) {
            return b.kind == Kind::Auto || b.kind == Kind::MinContent || b.kind == Kind::MaxContent ||
                   b.kind == Kind::FitContent;
        }

        // An item's lines in one axis (§8.3.1): definite when a line number fixes them (start, a line index from the
        // explicit grid's first line, may be negative), else only a span to place.
        struct Lines {
            bool definite{};
            int start{};
            int span{1};
        };

        // The 0-based index of line `number` of an explicit grid of `tracks` tracks; negative numbers count from its
        // last line.
        int line_index(int number, int tracks) {
            return number > 0 ? number - 1 : tracks + 1 + number;
        }

        Lines lines(const Line& start, const Line& end, int tracks) {
            using L = Line::Kind;
            if (start.kind == L::Number && end.kind == L::Number) {
                int a = line_index(start.value, tracks);
                int b = line_index(end.value, tracks);
                if (a > b) {
                    std::swap(a, b);
                }
                return {true, a, std::max(1, b - a)};
            }
            if (start.kind == L::Number) {
                return {true, line_index(start.value, tracks), end.kind == L::Span ? end.value : 1};
            }
            if (end.kind == L::Number) {
                const int span = start.kind == L::Span ? start.value : 1;
                return {true, line_index(end.value, tracks) - span, span};
            }
            // Two spans: the start's (§8.3.1).
            return {false, 0, start.kind == L::Span ? start.value : end.kind == L::Span ? end.value : 1};
        }

        // Which sizes a distribution grows (§11.5.1), and the contribution its items bring.
        enum class Grow : std::uint8_t {
            Base,
            Limit,
        };

        enum class Brings : std::uint8_t {
            Minimum,
            MinContent,
            MaxContent,
        };

        struct Sizing {
            std::span<Track> tracks;
            Space space{};
            float gap{};
            // Tracks whose growth limit became finite in the step for intrinsic maximums (§11.5.3).
            std::vector<bool> growable;

            bool flexible(std::size_t t) const {
                return tracks[t].max.kind == Kind::Flex;
            }

            bool crosses_flexible(const Contribution& c) const {
                for (std::size_t t = c.start; t < c.start + c.span; ++t) {
                    if (flexible(t)) {
                        return true;
                    }
                }
                return false;
            }

            float gutters(const Contribution& c) const {
                return gap * float(c.span - 1);
            }

            // A contribution limited by the fixed maximums of the tracks the item spans when all of them have one,
            // and floored by the item's minimum contribution (§11.5).
            float limited(const Contribution& c, float contribution) const {
                float cap = gutters(c);
                for (std::size_t t = c.start; t < c.start + c.span; ++t) {
                    const auto& max = tracks[t].max;
                    if (max.kind != Kind::Length && max.kind != Kind::FitContent) {
                        return std::max(contribution, c.minimum);
                    }
                    cap += max.value;
                }
                return std::max(std::min(contribution, cap), c.minimum);
            }

            float contribution(const Contribution& c, Brings brings) const {
                switch (brings) {
                    case Brings::Minimum:
                        return space == Space::Definite ? c.minimum : limited(c, c.min_content);
                    case Brings::MinContent:
                        return c.min_content;
                    case Brings::MaxContent:
                        return space == Space::MaxContent ? limited(c, c.max_content) : c.max_content;
                }
                return 0;
            }

            float size(std::size_t t, Grow grow) const {
                const auto& track = tracks[t];
                return grow == Grow::Base || std::isinf(track.limit) ? track.base : track.limit;
            }

            // How far a distribution may grow a track before it freezes: a base size up to its growth limit (and its
            // fit-content() argument), a growth limit up to itself unless it is infinitely growable.
            float cap(std::size_t t, Grow grow) const {
                const auto& track = tracks[t];
                if (grow == Grow::Base) {
                    return track.max.kind == Kind::FitContent ? std::min(track.limit, track.max.value) : track.limit;
                }
                if (growable[t] || std::isinf(track.limit)) {
                    return track.max.kind == Kind::FitContent ? track.max.value : infinite;
                }
                return track.limit;
            }

            // Shares `space` among the tracks `among`, equally or by flex factor, each up to `limit` less its size and
            // its increase so far; returns the space left.
            template <class Limit>
            float share(float extra, const std::vector<std::size_t>& among, std::vector<float>& incurred, Grow grow,
                        bool by_flex, Limit limit) const {
                std::vector<std::size_t> open = among;
                while (extra > constants::layout::grid_size_epsilon && !open.empty()) {
                    float weights{};
                    for (const auto t : open) {
                        weights += by_flex ? tracks[t].max.value : 1;
                    }
                    const bool equal = weights <= 0;
                    if (equal) {
                        weights = float(open.size());
                    }
                    float used{};
                    std::vector<std::size_t> still;
                    for (const auto t : open) {
                        const float want = extra * (equal || !by_flex ? 1 : tracks[t].max.value) / weights;
                        const float room = std::max(0.f, limit(t) - size(t, grow) - incurred[t]);
                        const float take = std::min(want, room);
                        incurred[t] += take;
                        used += take;
                        if (take >= want) {
                            still.push_back(t);
                        }
                    }
                    extra -= used;
                    if (still.size() == open.size()) {
                        break;
                    }
                    open = std::move(still);
                }
                return std::max(0.f, extra);
            }

            // Distributes the extra space the items' contributions need beyond the sizes of the tracks they span over
            // the affected ones among those (§11.5.1), growing base sizes or growth limits.
            template <class Affected>
            void distribute(const std::vector<const Contribution*>& items, Grow grow, Brings brings, Affected affected,
                            bool by_flex = false, bool mark_growable = false) {
                std::vector<float> planned(tracks.size());
                std::vector<bool> touched(tracks.size());
                std::vector<float> incurred(tracks.size());
                for (const auto* item : items) {
                    std::vector<std::size_t> grown;
                    float sizes = gutters(*item);
                    for (std::size_t t = item->start; t < item->start + item->span; ++t) {
                        sizes += size(t, grow);
                        if (affected(t)) {
                            grown.push_back(t);
                        }
                    }
                    if (grown.empty()) {
                        continue;
                    }
                    for (const auto t : grown) {
                        touched[t] = true;
                        incurred[t] = 0;
                    }
                    float extra = contribution(*item, brings) - sizes;
                    if (extra > 0) {
                        extra = share(extra, grown, incurred, grow, by_flex, [&](std::size_t t) {
                            return cap(t, grow);
                        });
                    }
                    if (extra > constants::layout::grid_size_epsilon) {
                        // Beyond the limits: to the tracks whose maximum suits the contribution, else to them all.
                        std::vector<std::size_t> beyond;
                        for (const auto t : grown) {
                            const auto max = tracks[t].max.kind;
                            if (grow == Grow::Limit ||
                                (brings == Brings::MaxContent
                                     ? max == Kind::MaxContent || max == Kind::Auto || max == Kind::FitContent
                                     : intrinsic(tracks[t].max))) {
                                beyond.push_back(t);
                            }
                        }
                        if (beyond.empty()) {
                            beyond = grown;
                        }
                        // A fit-content() track grows as max-content up to its argument, then as fixed.
                        extra = share(extra, beyond, incurred, grow, by_flex, [&](std::size_t t) {
                            return tracks[t].max.kind == Kind::FitContent ? std::max(tracks[t].max.value, size(t, grow))
                                                                          : infinite;
                        });
                        if (extra > constants::layout::grid_size_epsilon) {
                            share(extra, beyond, incurred, grow, by_flex, [](std::size_t) {
                                return infinite;
                            });
                        }
                    }
                    for (const auto t : grown) {
                        planned[t] = std::max(planned[t], incurred[t]);
                    }
                }
                for (std::size_t t = 0; t < tracks.size(); ++t) {
                    if (!touched[t]) {
                        continue;
                    }
                    auto& track = tracks[t];
                    if (grow == Grow::Base) {
                        track.base += planned[t];
                    } else if (std::isinf(track.limit)) {
                        track.limit = track.base + planned[t];
                        if (mark_growable) {
                            growable[t] = true;
                        }
                    } else {
                        track.limit += planned[t];
                    }
                }
            }

            // The size of an fr (§11.7.1) for the tracks [first, first + count) filling `fill`.
            float fr(std::size_t first, std::size_t count, float fill) const {
                std::size_t visible{};
                for (std::size_t t = first; t < first + count; ++t) {
                    visible += tracks[t].collapsed ? 0 : 1;
                }
                const float leftover = fill - gap * float(visible > 1 ? visible - 1 : 0);
                std::vector<bool> inflexible(count);
                for (;;) {
                    float rest = leftover;
                    float factors{};
                    for (std::size_t i = 0; i < count; ++i) {
                        if (flexible(first + i) && !inflexible[i]) {
                            factors += tracks[first + i].max.value;
                        } else {
                            rest -= tracks[first + i].base;
                        }
                    }
                    const float size = rest / std::max(1.f, factors);
                    bool again{};
                    for (std::size_t i = 0; i < count; ++i) {
                        if (flexible(first + i) && !inflexible[i] &&
                            size * tracks[first + i].max.value < tracks[first + i].base) {
                            inflexible[i] = true;
                            again = true;
                        }
                    }
                    if (!again) {
                        return size;
                    }
                }
            }
        };
    } // namespace

    TrackList template_tracks(std::string_view property, std::string_view value) {
        const auto text = normalized(value);
        TrackList out;
        if (text.empty() || text == "none") {
            return out;
        }
        if (text.size() > std::size_t(constants::layout::max_grid_value_bytes)) {
            refuse(property, text.substr(0, constants::layout::max_style_value_bytes), "longer than the grid bound");
        }
        const auto bound = std::size_t(constants::layout::max_grid_lines);
        const auto parts = css::split(text, ' ');
        if (parts.nesting != css::Nesting::Balanced) {
            refuse(property, text, "unbalanced parentheses");
        }
        for (const auto& part : parts.parts) {
            if (const auto args = arguments(part, "repeat")) {
                if (args->size() != 2 || (*args)[1].empty()) {
                    refuse(property, text, "repeat() takes a count and tracks");
                }
                std::vector<TrackSize> tracks;
                for (const auto& t : css::split((*args)[1], ' ').parts) {
                    const auto size = track_size(t);
                    if (!size) {
                        refuse(property, text, "track size " + t);
                    }
                    tracks.push_back(*size);
                }
                const auto& count = (*args)[0];
                if (count == "auto-fill" || count == "auto-fit") {
                    if (out.repeat != TrackList::Repeat::None) {
                        refuse(property, text, "two automatic repetitions");
                    }
                    if (!std::all_of(tracks.begin(), tracks.end(), fixed_size)) {
                        refuse(property, text, "an automatic repetition repeats fixed sizes");
                    }
                    out.repeat = count == "auto-fill" ? TrackList::Repeat::Fill : TrackList::Repeat::Fit;
                    out.repeat_at = out.tracks.size();
                    out.repeated = std::move(tracks);
                    continue;
                }
                const auto n = integer(count);
                if (!n || *n < 1) {
                    refuse(property, text, "repeat() count " + count);
                }
                if (out.tracks.size() + tracks.size() * std::size_t(*n) > bound) {
                    refuse(property, text, "more tracks than the grid bound");
                }
                for (long long i = 0; i < *n; ++i) {
                    out.tracks.insert(out.tracks.end(), tracks.begin(), tracks.end());
                }
                continue;
            }
            const auto size = track_size(part);
            if (!size) {
                refuse(property, text,
                       part.starts_with('[') ? "line names are outside the profile" : "track size " + part);
            }
            out.tracks.push_back(*size);
            if (out.tracks.size() > bound) {
                refuse(property, text, "more tracks than the grid bound");
            }
        }
        if (out.repeat != TrackList::Repeat::None && !std::all_of(out.tracks.begin(), out.tracks.end(), fixed_size)) {
            refuse(property, text, "an automatic repetition takes fixed sizes beside it");
        }
        return out;
    }

    std::vector<TrackSize> auto_tracks(std::string_view property, std::string_view value) {
        const auto text = normalized(value);
        if (text.size() > std::size_t(constants::layout::max_grid_value_bytes)) {
            refuse(property, text.substr(0, constants::layout::max_style_value_bytes), "longer than the grid bound");
        }
        const auto parts = css::split(text, ' ');
        if (parts.nesting != css::Nesting::Balanced || parts.parts.empty()) {
            refuse(property, text, "not a track size list");
        }
        std::vector<TrackSize> out;
        for (const auto& part : parts.parts) {
            const auto size = track_size(part);
            if (!size) {
                refuse(property, text, "track size " + part);
            }
            out.push_back(*size);
        }
        if (out.size() > std::size_t(constants::layout::max_grid_lines)) {
            refuse(property, text, "more tracks than the grid bound");
        }
        return out;
    }

    Line line(std::string_view property, std::string_view value) {
        const auto text = normalized(value);
        if (text.empty() || text == "auto") {
            return {};
        }
        const auto words = css::split(text, ' ').parts;
        const auto bound = static_cast<long long>(constants::layout::max_grid_lines);
        if (words.size() == 2 && (words[0] == "span" || words[1] == "span")) {
            const auto n = integer(words[0] == "span" ? words[1] : words[0]);
            if (!n || *n < 1) {
                refuse(property, text, "a span is a positive integer");
            }
            return {Line::Kind::Span, int(std::min(*n, bound))};
        }
        if (words.size() == 1) {
            if (const auto n = integer(words[0])) {
                if (*n == 0) {
                    refuse(property, text, "there is no line 0");
                }
                return {Line::Kind::Number, int(std::clamp(*n, -bound, bound))};
            }
        }
        refuse(property, text, "named lines and areas are outside the profile");
    }

    Flow auto_flow(std::string_view value) {
        const auto text = normalized(value);
        Flow out;
        bool axis{};
        const auto words = css::split(text, ' ').parts;
        for (const auto& word : words) {
            if ((word == "row" || word == "column") && !axis) {
                out.column = word == "column";
                axis = true;
            } else if (word == "dense" && !out.dense) {
                out.dense = true;
            } else {
                refuse("grid-auto-flow", text, "not row, column and dense");
            }
        }
        if (words.empty()) {
            refuse("grid-auto-flow", text, "not row, column and dense");
        }
        return out;
    }

    Align self_alignment(std::string_view property, std::string_view value) {
        auto text = normalized(value);
        for (const std::string_view prefix : {"safe ", "unsafe ", "legacy "}) {
            if (text.starts_with(prefix)) {
                text = text.substr(prefix.size());
            }
        }
        if (text.empty() || text == "auto") {
            return Align::Auto;
        }
        if (text == "normal" || text == "stretch" || text == "legacy") {
            return Align::Stretch;
        }
        if (text == "start" || text == "self-start" || text == "flex-start" || text == "left") {
            return Align::Start;
        }
        if (text == "end" || text == "self-end" || text == "flex-end" || text == "right") {
            return Align::End;
        }
        if (text == "center") {
            return Align::Center;
        }
        if (text == "baseline" || text == "first baseline") {
            return Align::Baseline;
        }
        throw std::invalid_argument("WEFT_UNSUPPORTED_ALIGN: " + std::string(property) + ' ' + text +
                                    ": not stretch, start, center, end or baseline");
    }

    std::size_t repetitions(const TrackList& list, float basis, float available, float gap, bool fill) {
        if (list.repeat == TrackList::Repeat::None || list.repeated.empty()) {
            return 0;
        }
        // Each track as its max track sizing function when that is definite, else as its minimum; a repeated
        // track at least 1px, so that some number of them overflows (§7.2.3.2).
        const auto px = [&](const Breadth& b) -> std::optional<float> {
            if (b.kind == Kind::Length) {
                return b.value;
            }
            if (b.kind == Kind::Percent && std::isfinite(basis)) {
                return basis * b.value / float(constants::percent_scale);
            }
            return std::nullopt;
        };
        const auto size = [&](const TrackSize& t) {
            const auto max = px(t.max);
            const auto min = px(t.min);
            return max ? std::max(*max, min.value_or(0.f)) : min.value_or(0.f);
        };
        float fixed_size_total{};
        for (const auto& t : list.tracks) {
            fixed_size_total += size(t);
        }
        float repeated{};
        for (const auto& t : list.repeated) {
            repeated += std::max(1.f, size(t));
        }
        // n repetitions take first + n * each.
        const auto count = float(list.repeated.size());
        const float first = fixed_size_total + gap * (float(list.tracks.size()) - 1);
        const float each = repeated + gap * count;
        const auto most =
            std::max<std::size_t>(1, (std::size_t(constants::layout::max_grid_lines) -
                                      std::min(list.tracks.size(), std::size_t(constants::layout::max_grid_lines))) /
                                         list.repeated.size());
        const float n = (available - first) / each;
        if (!std::isfinite(n)) {
            return 1;
        }
        const double whole = fill ? std::ceil(n - constants::layout::grid_size_epsilon)
                                  : std::floor(n + constants::layout::grid_size_epsilon);
        return std::clamp<std::size_t>(std::size_t(std::max(1.0, whole)), 1, most);
    }

    std::vector<TrackSize> expand(const TrackList& list, std::size_t count) {
        auto out = list.tracks;
        if (list.repeat != TrackList::Repeat::None) {
            std::vector<TrackSize> repeated;
            for (std::size_t i = 0; i < count; ++i) {
                repeated.insert(repeated.end(), list.repeated.begin(), list.repeated.end());
            }
            out.insert(out.begin() + std::ptrdiff_t(list.repeat_at), repeated.begin(), repeated.end());
        }
        return out;
    }

    Placed place(std::span<const Placement> items, std::size_t explicit_rows, std::size_t explicit_columns, Flow flow) {
        // The major axis grows as items are placed (rows in row flow); the minor one is fixed before auto-placement.
        struct Item {
            Lines major;
            Lines minor;
            bool placed{};
            int at_major{};
            int at_minor{};
        };

        std::vector<Item> all;
        all.reserve(items.size());
        for (const auto& p : items) {
            const auto rows = lines(p.row_start, p.row_end, int(explicit_rows));
            const auto columns = lines(p.column_start, p.column_end, int(explicit_columns));
            all.push_back(flow.column ? Item{columns, rows} : Item{rows, columns});
        }
        // Implicit tracks before the explicit grid: as many as definite lines reach before it.
        int major_before{};
        int minor_before{};
        for (const auto& item : all) {
            if (item.major.definite) {
                major_before = std::max(major_before, -item.major.start);
            }
            if (item.minor.definite) {
                minor_before = std::max(minor_before, -item.minor.start);
            }
        }
        for (auto& item : all) {
            item.major.start += item.major.definite ? major_before : 0;
            item.minor.start += item.minor.definite ? minor_before : 0;
        }
        int majors = major_before + int(flow.column ? explicit_columns : explicit_rows);
        int minors = minor_before + int(flow.column ? explicit_rows : explicit_columns);
        // Occupied cells, by major then minor index.
        std::vector<std::vector<bool>> occupied;
        long long work{};
        const auto vacant = [&](int major, int major_span, int minor, int minor_span) {
            for (int r = major; r < major + major_span && r < int(occupied.size()); ++r) {
                const auto& row = occupied[std::size_t(r)];
                for (int c = minor; c < minor + minor_span && c < int(row.size()); ++c) {
                    if (++work > constants::layout::max_grid_placement_work) {
                        throw std::invalid_argument("WEFT_LAYOUT_LIMIT: grid placement work");
                    }
                    if (row[std::size_t(c)]) {
                        return false;
                    }
                }
            }
            return true;
        };
        const auto put = [&](Item& item, int major, int minor) {
            item.placed = true;
            item.at_major = major;
            item.at_minor = minor;
            majors = std::max(majors, major + item.major.span);
            minors = std::max(minors, minor + item.minor.span);
            if (std::int64_t(majors) * minors > constants::layout::max_grid_cells) {
                throw std::invalid_argument("WEFT_LAYOUT_LIMIT: grid cells");
            }
            if (occupied.size() < std::size_t(major + item.major.span)) {
                occupied.resize(std::size_t(major + item.major.span));
            }
            for (int r = major; r < major + item.major.span; ++r) {
                auto& row = occupied[std::size_t(r)];
                if (row.size() < std::size_t(minor + item.minor.span)) {
                    row.resize(std::size_t(minor + item.minor.span));
                }
                for (int c = minor; c < minor + item.minor.span; ++c) {
                    row[std::size_t(c)] = true;
                }
            }
        };
        // 1. Items definite in both axes.
        for (auto& item : all) {
            if (item.major.definite && item.minor.definite) {
                put(item, item.major.start, item.minor.start);
            }
        }
        // 2. Items locked to a major track: sparse, past the items this step placed in it before.
        std::map<int, int> past;
        for (auto& item : all) {
            if (item.placed || !item.major.definite) {
                continue;
            }
            int minor = flow.dense ? 0 : past[item.major.start];
            while (!vacant(item.major.start, item.major.span, minor, item.minor.span)) {
                ++minor;
            }
            put(item, item.major.start, minor);
            past[item.major.start] = minor + item.minor.span;
        }
        // 3. The minor tracks: those definite items reach, and the widest span of the others.
        for (const auto& item : all) {
            if (!item.placed) {
                minors = std::max(minors, item.minor.definite ? item.minor.start + item.minor.span : item.minor.span);
            }
        }
        // 4. The others, at the auto-placement cursor.
        int cursor_major{};
        int cursor_minor{};
        for (auto& item : all) {
            if (item.placed) {
                continue;
            }
            if (item.minor.definite) {
                if (flow.dense) {
                    cursor_major = 0;
                } else if (item.minor.start < cursor_minor) {
                    ++cursor_major;
                }
                cursor_minor = item.minor.start;
                while (!vacant(cursor_major, item.major.span, cursor_minor, item.minor.span)) {
                    ++cursor_major;
                }
            } else {
                if (flow.dense) {
                    cursor_major = 0;
                    cursor_minor = 0;
                }
                for (;;) {
                    while (cursor_minor + item.minor.span <= minors &&
                           !vacant(cursor_major, item.major.span, cursor_minor, item.minor.span)) {
                        ++cursor_minor;
                    }
                    if (cursor_minor + item.minor.span <= minors) {
                        break;
                    }
                    ++cursor_major;
                    cursor_minor = 0;
                }
            }
            put(item, cursor_major, cursor_minor);
        }
        Placed out;
        out.areas.reserve(all.size());
        for (const auto& item : all) {
            const auto major = std::size_t(item.at_major);
            const auto minor = std::size_t(item.at_minor);
            const auto major_span = std::size_t(item.major.span);
            const auto minor_span = std::size_t(item.minor.span);
            out.areas.push_back(flow.column ? Area{minor, minor_span, major, major_span}
                                            : Area{major, major_span, minor, minor_span});
        }
        out.rows = std::size_t(flow.column ? minors : majors);
        out.columns = std::size_t(flow.column ? majors : minors);
        out.rows_before = std::size_t(flow.column ? minor_before : major_before);
        out.columns_before = std::size_t(flow.column ? major_before : minor_before);
        return out;
    }

    Track resolve(const TrackSize& size, float basis) {
        const auto one = [&](Breadth b) {
            if (b.kind == Kind::Percent) {
                return std::isfinite(basis) ? Breadth{Kind::Length, basis * b.value / float(constants::percent_scale)}
                                            : Breadth{Kind::Auto};
            }
            if (b.kind == Kind::FitContent && b.percent) {
                b.value = std::isfinite(basis) ? basis * b.value / float(constants::percent_scale) : infinite;
                b.percent = false;
            }
            return b;
        };
        Track out;
        out.min = one(size.min);
        out.max = one(size.max);
        return out;
    }

    void size_tracks(std::span<Track> tracks, std::span<const Contribution> items, Space space, float available,
                     float gap, bool stretch) {
        // Initialize the track sizes (§11.4).
        for (auto& t : tracks) {
            if (t.collapsed) {
                t.min = t.max = Breadth{Kind::Length};
            }
            t.base = t.min.kind == Kind::Length ? t.min.value : 0;
            t.limit = t.max.kind == Kind::Length ? t.max.value : infinite;
            t.limit = std::max(t.limit, t.base);
        }
        Sizing s{tracks, space, gap, std::vector<bool>(tracks.size())};
        // Resolve intrinsic track sizes (§11.5). Items of one track, not a flexible one.
        std::vector<std::vector<const Contribution*>> single(tracks.size());
        std::map<std::size_t, std::vector<const Contribution*>> spanning;
        std::vector<const Contribution*> flexible;
        for (const auto& c : items) {
            if (c.span == 0 || c.start + c.span > tracks.size()) {
                continue;
            }
            if (s.crosses_flexible(c)) {
                flexible.push_back(&c);
            } else if (c.span == 1) {
                single[c.start].push_back(&c);
            } else {
                spanning[c.span].push_back(&c);
            }
        }
        for (std::size_t t = 0; t < tracks.size(); ++t) {
            auto& track = tracks[t];
            if (single[t].empty()) {
                continue;
            }
            if (intrinsic(track.min)) {
                float most{};
                for (const auto* c : single[t]) {
                    const float contribution = track.min.kind == Kind::MinContent   ? c->min_content
                                               : track.min.kind == Kind::MaxContent ? c->max_content
                                               : space == Space::MinContent         ? s.limited(*c, c->min_content)
                                               : space == Space::MaxContent         ? s.limited(*c, c->max_content)
                                                                                    : c->minimum;
                    most = std::max(most, contribution);
                }
                track.base = std::max(track.base, most);
            }
            if (intrinsic(track.max)) {
                float most{};
                for (const auto* c : single[t]) {
                    most = std::max(most, track.max.kind == Kind::MinContent ? c->min_content : c->max_content);
                }
                track.limit = track.max.kind == Kind::FitContent ? std::min(most, track.max.value) : most;
            }
            track.limit = std::max(track.limit, track.base);
        }
        const auto min_is = [&](std::size_t t, std::initializer_list<Kind> kinds) {
            return std::find(kinds.begin(), kinds.end(), tracks[t].min.kind) != kinds.end();
        };
        const auto max_is = [&](std::size_t t, std::initializer_list<Kind> kinds) {
            return std::find(kinds.begin(), kinds.end(), tracks[t].max.kind) != kinds.end();
        };
        const bool max_content = space == Space::MaxContent;
        // Items spanning several tracks, none flexible, by span.
        for (const auto& [span, group] : spanning) {
            s.distribute(group, Grow::Base, Brings::Minimum, [&](std::size_t t) {
                return intrinsic(tracks[t].min);
            });
            s.distribute(group, Grow::Base, Brings::MinContent, [&](std::size_t t) {
                return min_is(t, {Kind::MinContent, Kind::MaxContent});
            });
            s.distribute(group, Grow::Base, Brings::MaxContent, [&](std::size_t t) {
                return min_is(t, {Kind::MaxContent}) || (max_content && min_is(t, {Kind::Auto}));
            });
            for (auto& t : tracks) {
                t.limit = std::max(t.limit, t.base);
            }
            s.distribute(
                group, Grow::Limit, Brings::MinContent,
                [&](std::size_t t) {
                    return intrinsic(tracks[t].max);
                },
                false, true);
            s.distribute(group, Grow::Limit, Brings::MaxContent, [&](std::size_t t) {
                return max_is(t, {Kind::MaxContent, Kind::Auto, Kind::FitContent});
            });
            std::fill(s.growable.begin(), s.growable.end(), false);
        }
        // Items crossing flexible tracks, together: only the flexible tracks grow, by flex factor.
        if (!flexible.empty()) {
            s.distribute(
                flexible, Grow::Base, Brings::Minimum,
                [&](std::size_t t) {
                    return s.flexible(t) && intrinsic(tracks[t].min);
                },
                true);
            s.distribute(
                flexible, Grow::Base, Brings::MinContent,
                [&](std::size_t t) {
                    return s.flexible(t) && min_is(t, {Kind::MinContent, Kind::MaxContent});
                },
                true);
            s.distribute(
                flexible, Grow::Base, Brings::MaxContent,
                [&](std::size_t t) {
                    return s.flexible(t) && (min_is(t, {Kind::MaxContent}) || (max_content && min_is(t, {Kind::Auto})));
                },
                true);
        }
        for (auto& t : tracks) {
            if (std::isinf(t.limit)) {
                t.limit = t.base;
            }
            t.limit = std::max(t.limit, t.base);
        }
        const auto free = [&] {
            return available - extent(tracks, gap);
        };
        // Maximize tracks (§11.6).
        if (space == Space::MaxContent) {
            for (auto& t : tracks) {
                t.base = t.limit;
            }
        } else if (space == Space::Definite && free() > 0) {
            std::vector<std::size_t> all(tracks.size());
            for (std::size_t t = 0; t < tracks.size(); ++t) {
                all[t] = t;
            }
            std::vector<float> incurred(tracks.size());
            s.share(free(), all, incurred, Grow::Base, false, [&](std::size_t t) {
                return tracks[t].limit;
            });
            for (std::size_t t = 0; t < tracks.size(); ++t) {
                tracks[t].base += incurred[t];
            }
        }
        // Expand flexible tracks (§11.7).
        bool any_flexible{};
        for (std::size_t t = 0; t < tracks.size(); ++t) {
            any_flexible |= s.flexible(t);
        }
        if (any_flexible && space != Space::MinContent) {
            float fraction{};
            if (space == Space::Definite) {
                if (free() > 0) {
                    fraction = s.fr(0, tracks.size(), available);
                }
            } else {
                for (std::size_t t = 0; t < tracks.size(); ++t) {
                    if (s.flexible(t)) {
                        const float factor = tracks[t].max.value;
                        fraction = std::max(fraction, factor > 1 ? tracks[t].base / factor : tracks[t].base);
                    }
                }
                for (const auto* c : flexible) {
                    fraction = std::max(fraction, s.fr(c->start, c->span, c->max_content));
                }
            }
            for (std::size_t t = 0; t < tracks.size(); ++t) {
                if (s.flexible(t)) {
                    tracks[t].base = std::max(tracks[t].base, fraction * tracks[t].max.value);
                }
            }
        }
        // Stretch auto tracks (§11.8).
        if (stretch && space == Space::Definite && free() > 0) {
            std::size_t automatic{};
            for (const auto& t : tracks) {
                automatic += t.max.kind == Kind::Auto && !t.collapsed ? 1 : 0;
            }
            if (automatic) {
                const float each = free() / float(automatic);
                for (auto& t : tracks) {
                    if (t.max.kind == Kind::Auto && !t.collapsed) {
                        t.base += each;
                    }
                }
            }
        }
    }

    float extent(std::span<const Track> tracks, float gap) {
        float total{};
        std::size_t visible{};
        for (const auto& t : tracks) {
            total += t.base;
            visible += t.collapsed ? 0 : 1;
        }
        return total + gap * float(visible > 1 ? visible - 1 : 0);
    }

    Distribution distribute(std::string_view value, float free, std::size_t tracks) {
        auto text = normalized(value);
        bool safe{};
        if (text.starts_with("safe ")) {
            safe = true;
            text = text.substr(5);
        } else if (text.starts_with("unsafe ")) {
            text = text.substr(7);
        }
        // Overflowing content keeps to the start when the alignment is safe.
        const auto shift = [&](float offset) {
            return Distribution{safe && free < 0 ? 0 : offset, 0};
        };
        if (text.empty() || text == "normal" || text == "stretch" || text == "start" || text == "flex-start" ||
            text == "left" || text == "baseline" || text == "first baseline" || text == "last baseline") {
            return {};
        }
        if (text == "end" || text == "flex-end" || text == "right") {
            return shift(free);
        }
        if (text == "center") {
            return shift(free / 2);
        }
        if (text == "space-between") {
            return free > 0 && tracks > 1 ? Distribution{0, free / float(tracks - 1)} : Distribution{};
        }
        if (text == "space-around") {
            return free > 0 && tracks ? Distribution{free / float(tracks) / 2, free / float(tracks)} : Distribution{};
        }
        if (text == "space-evenly") {
            return free > 0 ? Distribution{free / float(tracks + 1), free / float(tracks + 1)} : Distribution{};
        }
        throw std::invalid_argument("WEFT_UNSUPPORTED_ALIGN: content distribution " + text);
    }
} // namespace weft::grid
