#include "text/bidi.hpp"

#include <SheenBidi/SheenBidi.h>

#include <algorithm>
#include <memory>
#include <new>

namespace weft {
    namespace {
        SBCodepointSequence sequence(std::string_view utf8) {
            return {SBStringEncodingUTF8, utf8.data(), SBUInteger(utf8.size())};
        }

        template <typename T, void (*Release)(T)>
        struct Releaser {
            void operator()(std::remove_pointer_t<T>* p) const {
                Release(p);
            }
        };

        using Algorithm =
            std::unique_ptr<std::remove_pointer_t<SBAlgorithmRef>, Releaser<SBAlgorithmRef, SBAlgorithmRelease>>;
        using Paragraph =
            std::unique_ptr<std::remove_pointer_t<SBParagraphRef>, Releaser<SBParagraphRef, SBParagraphRelease>>;
        using Locator = std::unique_ptr<std::remove_pointer_t<SBScriptLocatorRef>,
                                        Releaser<SBScriptLocatorRef, SBScriptLocatorRelease>>;
    } // namespace

    bool bidi_left_to_right(std::string_view utf8) {
        const auto* bytes = reinterpret_cast<const SBUInt8*>(utf8.data());
        SBUInteger at = 0;
        while (at < utf8.size()) {
            // Bytes below U+0590's lead byte are code points no right-to-left text has.
            if (bytes[at] < 0xD6) {
                ++at;
                continue;
            }
            const auto c = SBCodepointDecodeNextFromUTF8(bytes, SBUInteger(utf8.size()), &at);
            switch (SBCodepointGetBidiType(c)) {
                case SBBidiTypeR:
                case SBBidiTypeAL:
                case SBBidiTypeAN:
                case SBBidiTypeLRI:
                case SBBidiTypeRLI:
                case SBBidiTypeFSI:
                case SBBidiTypePDI:
                case SBBidiTypeLRE:
                case SBBidiTypeRLE:
                case SBBidiTypeLRO:
                case SBBidiTypeRLO:
                case SBBidiTypePDF:
                    return false;
                default:
                    break;
            }
        }
        return true;
    }

    std::optional<bool> bidi_first_strong_rtl(std::string_view utf8) {
        const auto* bytes = reinterpret_cast<const SBUInt8*>(utf8.data());
        for (SBUInteger at = 0; at < utf8.size();) {
            const auto type =
                SBCodepointGetBidiType(SBCodepointDecodeNextFromUTF8(bytes, SBUInteger(utf8.size()), &at));
            if (type == SBBidiTypeL) {
                return false;
            }
            if (type == SBBidiTypeR || type == SBBidiTypeAL) {
                return true;
            }
        }
        return std::nullopt;
    }

    namespace {
        // The levels as ICU's ubidi_setPara leaves them, from which Blink lays out (adjustWSLevels, its UAX #9 L1 on
        // the paragraph rather than on lines): a run of whitespace, isolate initiators and terminators, boundary
        // neutrals and explicit embeddings at the end of the text or before a segment or paragraph separator takes its
        // paragraph's level, and so does that separator; every other boundary neutral and explicit embedding (which
        // X9 removes) takes the level of the character after it. On code points: the bidi type and level of each.
        void adjust_like_icu(std::vector<SBBidiType>& types, std::vector<std::uint8_t>& levels,
                             const std::vector<std::uint8_t>& bases) {
            const auto whitespace_like = [](SBBidiType t) {
                return t == SBBidiTypeWS || t == SBBidiTypeB || t == SBBidiTypeS || t == SBBidiTypeBN ||
                       (t >= SBBidiTypeLRI && t <= SBBidiTypePDF);
            };
            const auto removed = [](SBBidiType t) {
                return t == SBBidiTypeBN || (t >= SBBidiTypeLRE && t <= SBBidiTypePDF);
            };
            auto i = types.size();
            while (i > 0) {
                while (i > 0 && whitespace_like(types[i - 1])) {
                    --i;
                    levels[i] = bases[i];
                }
                while (i > 0) {
                    --i;
                    if (removed(types[i])) {
                        levels[i] = levels[i + 1];
                    } else if (types[i] == SBBidiTypeB || types[i] == SBBidiTypeS) {
                        levels[i] = bases[i];
                        break;
                    }
                }
            }
        }
    } // namespace

    BidiParagraphs bidi_levels(std::string_view utf8, BidiBase base) {
        BidiParagraphs out;
        if (utf8.empty()) {
            return out;
        }
        out.levels.resize(utf8.size());
        const auto text = sequence(utf8);
        Algorithm algorithm(SBAlgorithmCreate(&text));
        if (!algorithm) {
            throw std::bad_alloc();
        }
        const SBLevel level = base == BidiBase::Ltr ? 0 : base == BidiBase::Rtl ? 1 : SBLevelDefaultLTR;
        SBUInteger offset = 0;
        while (offset < utf8.size()) {
            Paragraph paragraph(
                SBAlgorithmCreateParagraph(algorithm.get(), offset, SBUInteger(utf8.size()) - offset, level));
            if (!paragraph) {
                throw std::bad_alloc();
            }
            const auto length = SBParagraphGetLength(paragraph.get());
            const auto* levels = SBParagraphGetLevelsPtr(paragraph.get());
            std::copy_n(levels, length, out.levels.begin() + std::ptrdiff_t(offset));
            out.paragraphs.emplace_back(std::size_t(offset), std::uint8_t(SBParagraphGetBaseLevel(paragraph.get())));
            // A paragraph is never empty: P1 ends it at a separator or the text's end.
            offset += std::max<SBUInteger>(length, 1);
        }
        // Per code point: its first byte, type, level and paragraph's level; adjusted as ICU does, then each byte of a
        // code point at its level.
        std::vector<SBUInteger> starts;
        std::vector<SBBidiType> types;
        std::vector<std::uint8_t> levels;
        std::vector<std::uint8_t> bases;
        const auto* bytes = reinterpret_cast<const SBUInt8*>(utf8.data());
        std::size_t paragraph = 0;
        for (SBUInteger at = 0; at < utf8.size();) {
            while (paragraph + 1 < out.paragraphs.size() && out.paragraphs[paragraph + 1].first <= at) {
                ++paragraph;
            }
            starts.push_back(at);
            levels.push_back(out.levels[at]);
            bases.push_back(out.paragraphs[paragraph].second);
            types.push_back(SBCodepointGetBidiType(SBCodepointDecodeNextFromUTF8(bytes, SBUInteger(utf8.size()), &at)));
        }
        adjust_like_icu(types, levels, bases);
        for (std::size_t k = 0; k < starts.size(); ++k) {
            const auto end = k + 1 < starts.size() ? starts[k + 1] : SBUInteger(utf8.size());
            std::fill(out.levels.begin() + std::ptrdiff_t(starts[k]), out.levels.begin() + std::ptrdiff_t(end),
                      levels[k]);
        }
        return out;
    }

    std::vector<std::size_t> bidi_visual_order(std::span<const std::uint8_t> levels) {
        std::vector<std::size_t> order(levels.size());
        for (std::size_t i = 0; i < order.size(); ++i) {
            order[i] = i;
        }
        if (levels.empty()) {
            return order;
        }
        const auto [low, high] = std::minmax_element(levels.begin(), levels.end());
        const std::uint8_t lowest_odd = std::uint8_t(*low | 1);
        for (int level = *high; level >= lowest_odd; --level) {
            for (std::size_t i = 0; i < levels.size();) {
                if (levels[order[i]] < level) {
                    ++i;
                    continue;
                }
                auto j = i;
                while (j < levels.size() && levels[order[j]] >= level) {
                    ++j;
                }
                std::reverse(order.begin() + std::ptrdiff_t(i), order.begin() + std::ptrdiff_t(j));
                i = j;
            }
        }
        return order;
    }

    std::vector<ScriptRun> script_runs(std::string_view utf8) {
        std::vector<ScriptRun> out;
        if (utf8.empty()) {
            return out;
        }
        const auto text = sequence(utf8);
        Locator locator(SBScriptLocatorCreate());
        if (!locator) {
            throw std::bad_alloc();
        }
        SBScriptLocatorLoadCodepoints(locator.get(), &text);
        const auto* agent = SBScriptLocatorGetAgent(locator.get());
        while (SBScriptLocatorMoveNext(locator.get())) {
            out.push_back({std::size_t(agent->offset), std::size_t(agent->offset + agent->length),
                           std::uint32_t(SBScriptGetUnicodeTag(agent->script))});
        }
        return out;
    }
} // namespace weft
