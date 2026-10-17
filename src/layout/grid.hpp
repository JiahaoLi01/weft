#pragma once
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>
#include <vector>

// CSS Grid Layout 1 as the layout adapter (flex.cpp) lays it out, on values and numbers alone: the values of the grid
// properties, item placement (§8.5) and the track sizing algorithm (§11.3–§11.8). A value outside the implemented
// profile throws std::invalid_argument "WEFT_UNSUPPORTED_FORMAT: <property> <value>: <why>"; the scene drops such a
// value on a forgiving page instead of failing the frame (scene.cpp, drop_grid).
namespace weft::grid {
    // A track breadth (§7.2.2).
    struct Breadth {
        enum class Kind : std::uint8_t {
            Length,
            Percent,
            Flex,
            Auto,
            MinContent,
            MaxContent,
            // fit-content(<length-percentage>), a max track sizing function only.
            FitContent,
        };

        Kind kind{Kind::Auto};
        // px of a Length, the percentage of a Percent, the factor of a Flex, the argument of a FitContent (px, or a
        // percentage when `percent`).
        float value{};
        bool percent{};

        friend bool operator==(const Breadth&, const Breadth&) = default;
    };

    // A track sizing function (§7.2.3) as minmax(min, max): a single breadth b is minmax(b, b), a flexible one
    // minmax(auto, b), fit-content(x) minmax(auto, fit-content x).
    struct TrackSize {
        Breadth min;
        Breadth max;

        friend bool operator==(const TrackSize&, const TrackSize&) = default;
    };

    // grid-template-columns or -rows (§7.2): the explicit tracks, integer repeat() expanded, and at most one automatic
    // repetition, whose tracks repeat at `repeat_at` (an index into `tracks`) as many times as fit (§7.2.3.2).
    struct TrackList {
        enum class Repeat : std::uint8_t {
            None,
            Fill,
            Fit,
        };

        std::vector<TrackSize> tracks;
        Repeat repeat{};
        std::size_t repeat_at{};
        std::vector<TrackSize> repeated;
    };

    // none (no explicit tracks) or a track list: px, %, fr, auto, min-content, max-content, minmax(), fit-content() and
    // repeat() with an integer, auto-fill or auto-fit. Line names are outside the profile.
    TrackList template_tracks(std::string_view property, std::string_view value);
    // grid-auto-columns or -rows (§7.6): one or more track sizes, without repeat().
    std::vector<TrackSize> auto_tracks(std::string_view property, std::string_view value);

    // A grid-placement property (§8.3): auto, a line number (negative counts from the end of the explicit grid) or a
    // span. Named lines and areas are outside the profile.
    struct Line {
        enum class Kind : std::uint8_t {
            Auto,
            Number,
            Span,
        };

        Kind kind{};
        int value{};
    };

    Line line(std::string_view property, std::string_view value);

    // grid-auto-flow (§7.7).
    struct Flow {
        bool column{};
        bool dense{};
    };

    Flow auto_flow(std::string_view value);

    // A self-alignment value (justify-self and align-self, or their defaults justify-items and align-items; CSS Box
    // Alignment 3 §6) as the grid aligns an item in its area, left to right and top to bottom. Overflow keywords
    // (safe, unsafe) and legacy are ignored; normal is Stretch. Other values throw WEFT_UNSUPPORTED_ALIGN.
    enum class Align : std::uint8_t {
        Auto,
        Stretch,
        Start,
        Center,
        End,
        Baseline,
    };

    Align self_alignment(std::string_view property, std::string_view value);

    // How many times an automatic repetition of `repeated` repeats beside `fixed` (the other explicit tracks), with
    // percentages of `basis`: as many as fit in `available` (a definite size or maximum, `fill` false), or as few as
    // reach it (a minimum, `fill` true); at least one, bounded by the track limit (§7.2.3.2).
    std::size_t repetitions(const TrackList& list, float basis, float available, float gap, bool fill);
    // The explicit tracks of a list with its automatic repetition repeated `count` times.
    std::vector<TrackSize> expand(const TrackList& list, std::size_t count);

    // One item's placement properties.
    struct Placement {
        Line row_start;
        Line row_end;
        Line column_start;
        Line column_end;
    };

    // An item's grid area: its first track and number of tracks in each axis, as indices into the implicit grid.
    struct Area {
        std::size_t row{};
        std::size_t row_span{1};
        std::size_t column{};
        std::size_t column_span{1};
    };

    struct Placed {
        // One per item, in input order.
        std::vector<Area> areas;
        std::size_t rows{};
        std::size_t columns{};
        // Implicit tracks before the explicit grid's first track.
        std::size_t rows_before{};
        std::size_t columns_before{};
    };

    // The grid item placement algorithm (§8.5), items in order-modified document order. The implicit grid holds at
    // least the explicit one.
    Placed place(std::span<const Placement> items, std::size_t explicit_rows, std::size_t explicit_columns, Flow flow);

    // A track as the sizing algorithm takes it: its sizing functions with percentages resolved, and its sizes.
    struct Track {
        Breadth min;
        Breadth max;
        // An automatically repeated auto-fit track no item occupies: 0px, its gutters collapsed (§7.2.3.2).
        bool collapsed{};
        // Base size and growth limit (§11.4); the base size is the track's size once sized.
        float base{};
        float limit{};
    };

    // A track's sizing functions with percentages of `basis` (the content box size in the axis) resolved, or as auto
    // where the basis is indefinite (NaN, §7.2.1).
    Track resolve(const TrackSize& size, float basis);

    // How an axis is sized (§11.1): within a definite available size, or under a min-content or max-content
    // constraint.
    enum class Space : std::uint8_t {
        Definite,
        MinContent,
        MaxContent,
    };

    // An item's tracks in the axis and its outer size contributions there (§11.5): min-content, max-content and
    // minimum.
    struct Contribution {
        std::size_t start{};
        std::size_t span{1};
        float min_content{};
        float max_content{};
        float minimum{};
    };

    // The grid sizing algorithm in one axis (§11.3–§11.8): sets every track's base size, its size. `available` is the
    // content box size when `space` is Definite; `stretch`, that the axis' content distribution stretches auto tracks
    // (normal or stretch).
    void size_tracks(std::span<Track> tracks, std::span<const Contribution> items, Space space, float available,
                     float gap, bool stretch);

    // The tracks' size with the gutters between those not collapsed.
    float extent(std::span<const Track> tracks, float gap);

    // Content distribution (justify-content, align-content; CSS Box Alignment 3 §5.1): the offset of the first track
    // and the space added to each gutter between tracks not collapsed, for `free` space left over.
    struct Distribution {
        float offset{};
        float between{};
    };

    Distribution distribute(std::string_view value, float free, std::size_t tracks);
} // namespace weft::grid
