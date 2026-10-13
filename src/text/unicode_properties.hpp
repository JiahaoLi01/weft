#pragma once

#include <cstdint>

// Unicode character properties the text layer needs beyond HarfBuzz's, from QuickJS-NG's Unicode tables.
namespace weft {
    // How an emoji character shows by default (Unicode's Emoji and Emoji_Presentation properties): as emoji, as text
    // (an emoji character with text presentation, such as U+1F5E1 DAGGER KNIFE), or not an emoji at all.
    enum class EmojiPresentation : std::uint8_t {
        None,
        Text,
        Emoji,
    };

    EmojiPresentation emoji_presentation(char32_t c);
} // namespace weft
