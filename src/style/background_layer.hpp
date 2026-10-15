#pragma once

#include <cstdint>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

// One layer of the background shorthand (CSS Backgrounds 3) as its longhands take it, for the style module's cascade
// and the scene's keyframes alike:
//   [ <bg-image> || <bg-position> [ / <bg-size> ]? || <repeat-style> || <attachment> || <box> || <box> ] and the color
// of the final layer. The parts may come in any order; a part the value omits takes its initial value.
namespace weft {
    enum class BackgroundPart : std::uint8_t {
        Color,
        Image,
        Position,
        // The '/' between the position and the size.
        Slash,
        Size,
        Repeat,
        Attachment,
        // The first <box> sets background-origin, the second background-clip; one alone sets both.
        Origin,
        Clip,
        OriginAndClip
    };

    // A component of the value as the shorthand tells its parts apart: an identifier, a function by its name, a
    // length, percentage or number, a hash, the '/' delimiter, or anything else (a comma between layers).
    struct BackgroundComponent {
        enum class Kind : std::uint8_t {
            Ident,
            Function,
            Length,
            Hash,
            Slash,
            Other
        };
        Kind kind{};
        // An identifier or a function's name, in lower case.
        std::string_view name;
    };

    // Which part each component is; nothing for a value that is not one layer of the shorthand: a part twice, a
    // position of more than four components or a size of more than two, a '/' without a position before it and a size
    // after it, more than two boxes or repeat keywords, or a component no part takes (another layer's comma).
    std::optional<std::vector<BackgroundPart>> background_parts(std::span<const BackgroundComponent> components);
} // namespace weft
