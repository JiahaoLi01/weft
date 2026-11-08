#include "platform/macos/system_fonts.hpp"

#include "constants/text.hpp"

#include <CoreFoundation/CoreFoundation.h>
#include <CoreText/CoreText.h>
#include <ft2build.h>
#include FT_FREETYPE_H

#include <algorithm>
#include <array>
#include <climits>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <map>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

namespace weft::platform::macos {
    namespace {
        // A CoreFoundation reference the scope owns (a Copy or Create result), released at its end.
        template <class T>
        class Ref {
        public:
            Ref() = default;

            explicit Ref(T ref) : ref_(ref) {}

            ~Ref() {
                if (ref_) {
                    CFRelease(ref_);
                }
            }

            Ref(const Ref&) = delete;
            Ref& operator=(const Ref&) = delete;

            T get() const {
                return ref_;
            }

            explicit operator bool() const {
                return ref_ != nullptr;
            }

        private:
            T ref_{};
        };

        // The size the UI font and the cascade list are asked at: CSS's initial font-size, 16 px. The UI font's optical
        // size axis (opsz) follows it; the catalog has one instance per weight, whatever size the page draws at.
        constexpr CGFloat ui_size = 16;

        // The CSS weights and the NSFontWeight values CoreText reports for them as kCTFontWeightTrait.
        constexpr std::array<std::pair<double, std::uint16_t>, 9> weights{{{-0.8, 100},
                                                                           {-0.6, 200},
                                                                           {-0.4, 300},
                                                                           {0.0, 400},
                                                                           {0.23, 500},
                                                                           {0.3, 600},
                                                                           {0.4, 700},
                                                                           {0.56, 800},
                                                                           {0.62, 900}}};

        // The CSS weight whose NSFontWeight is nearest trait.
        std::uint16_t css_weight(double trait) {
            const auto nearest = std::min_element(weights.begin(), weights.end(), [&](const auto& a, const auto& b) {
                return std::abs(a.first - trait) < std::abs(b.first - trait);
            });
            return nearest->second;
        }

        std::string utf8(CFStringRef text) {
            if (!text) {
                return {};
            }
            const auto length = CFStringGetLength(text);
            std::string out(std::size_t(CFStringGetMaximumSizeForEncoding(length, kCFStringEncodingUTF8)) + 1, '\0');
            if (!CFStringGetCString(text, out.data(), CFIndex(out.size()), kCFStringEncodingUTF8)) {
                return {};
            }
            out.resize(std::char_traits<char>::length(out.c_str()));
            return out;
        }

        // What CoreText says of one face: its family, PostScript name, file, CSS weight, style and variation axes.
        struct Described {
            std::string family;
            std::string postscript;
            std::string path;
            std::uint16_t weight{400};
            bool italic{};
            std::vector<FontVariation> variations;
        };

        // None for a face without a file (or family or PostScript name).
        std::optional<Described> describe(CTFontDescriptorRef descriptor) {
            const Ref url(static_cast<CFURLRef>(CTFontDescriptorCopyAttribute(descriptor, kCTFontURLAttribute)));
            const Ref family(
                static_cast<CFStringRef>(CTFontDescriptorCopyAttribute(descriptor, kCTFontFamilyNameAttribute)));
            const Ref name(static_cast<CFStringRef>(CTFontDescriptorCopyAttribute(descriptor, kCTFontNameAttribute)));
            char path[PATH_MAX];
            if (!url || !family || !name ||
                !CFURLGetFileSystemRepresentation(url.get(), true, reinterpret_cast<UInt8*>(path), sizeof(path))) {
                return std::nullopt;
            }
            Described out{utf8(family.get()), utf8(name.get()), path};
            if (out.family.empty() || out.postscript.empty()) {
                return std::nullopt;
            }
            const Ref traits(
                static_cast<CFDictionaryRef>(CTFontDescriptorCopyAttribute(descriptor, kCTFontTraitsAttribute)));
            if (traits) {
                double weight{};
                if (const auto n = static_cast<CFNumberRef>(CFDictionaryGetValue(traits.get(), kCTFontWeightTrait));
                    n && CFNumberGetValue(n, kCFNumberDoubleType, &weight)) {
                    out.weight = css_weight(weight);
                }
                std::int64_t symbolic{};
                if (const auto n = static_cast<CFNumberRef>(CFDictionaryGetValue(traits.get(), kCTFontSymbolicTrait));
                    n && CFNumberGetValue(n, kCFNumberSInt64Type, &symbolic)) {
                    out.italic = (std::uint64_t(symbolic) & kCTFontItalicTrait) != 0;
                }
            }
            // Axis identifiers are the four tag bytes packed big-endian, as FreeType and HarfBuzz pack them.
            const Ref variation(
                static_cast<CFDictionaryRef>(CTFontDescriptorCopyAttribute(descriptor, kCTFontVariationAttribute)));
            if (variation) {
                const auto count = CFDictionaryGetCount(variation.get());
                const auto size = std::size_t(count);
                std::vector<const void*> keys(size);
                std::vector<const void*> values(size);
                CFDictionaryGetKeysAndValues(variation.get(), keys.data(), values.data());
                for (CFIndex i = 0; i < count; ++i) {
                    std::int64_t tag{};
                    double value{};
                    if (CFNumberGetValue(static_cast<CFNumberRef>(keys[std::size_t(i)]), kCFNumberSInt64Type, &tag) &&
                        CFNumberGetValue(static_cast<CFNumberRef>(values[std::size_t(i)]), kCFNumberDoubleType,
                                         &value)) {
                        out.variations.push_back({std::uint32_t(tag), float(value)});
                    }
                }
                std::sort(out.variations.begin(), out.variations.end());
            }
            return out;
        }

        std::optional<Described> describe(CTFontRef font) {
            const Ref descriptor(CTFontCopyFontDescriptor(font));
            return descriptor ? describe(descriptor.get()) : std::nullopt;
        }

        // The face index of a PostScript name in its font file, each file read once with FreeType: 0 in a file of one
        // face; in a collection, the face whose name, or the name of one of whose named instances, it is. Files larger
        // than a font file may be (constants::text::max_font_file_bytes) have none.
        class FaceIndices {
        public:
            FaceIndices() {
                if (FT_Init_FreeType(&library_)) {
                    throw std::runtime_error("WEFT_SYSTEM_FONTS: FT_Init_FreeType failed");
                }
            }

            ~FaceIndices() {
                FT_Done_FreeType(library_);
            }

            FaceIndices(const FaceIndices&) = delete;
            FaceIndices& operator=(const FaceIndices&) = delete;

            // None for a file FreeType cannot open, or a name none of its faces has.
            std::optional<std::uint32_t> find(const std::string& path, const std::string& postscript) {
                auto [file, added] = files_.try_emplace(path);
                if (added) {
                    file->second = read(path);
                }
                if (file->second.single) {
                    return 0;
                }
                const auto at = file->second.names.find(postscript);
                if (at == file->second.names.end()) {
                    return std::nullopt;
                }
                return at->second;
            }

        private:
            struct File {
                bool single{};
                std::map<std::string, std::uint32_t> names;
            };

            File read(const std::string& path) {
                File file;
                std::error_code error;
                const auto size = std::filesystem::file_size(path, error);
                if (error || size > std::uintmax_t(constants::text::max_font_file_bytes)) {
                    return file;
                }
                FT_Face face{};
                if (FT_New_Face(library_, path.c_str(), -1, &face)) {
                    return file;
                }
                const FT_Long faces = face->num_faces;
                FT_Done_Face(face);
                file.single = faces == 1;
                for (FT_Long i = 0; !file.single && i < faces; ++i) {
                    if (FT_New_Face(library_, path.c_str(), i, &face)) {
                        continue;
                    }
                    const FT_Long instances = face->style_flags >> 16;
                    name(file, face, i);
                    FT_Done_Face(face);
                    for (FT_Long n = 1; n <= instances; ++n) {
                        if (!FT_New_Face(library_, path.c_str(), i | (n << 16), &face)) {
                            name(file, face, i);
                            FT_Done_Face(face);
                        }
                    }
                }
                return file;
            }

            static void name(File& file, FT_Face face, FT_Long index) {
                if (const char* postscript = FT_Get_Postscript_Name(face)) {
                    file.names.try_emplace(postscript, std::uint32_t(index));
                }
            }

            FT_Library library_{};
            std::map<std::string, File> files_;
        };

        class Builder {
        public:
            // Adds the face under its family, the aliases and the family names of its name table; false when its
            // face index or names cannot be read.
            bool add(const Described& d, const std::vector<std::string>& aliases = {}) {
                const auto index = indices_.find(d.path, d.postscript);
                if (!index) {
                    return false;
                }
                FontFaceSource source{d.path, *index, d.variations};
                auto [read, added] = names_.try_emplace({d.path, *index});
                if (added) {
                    try {
                        read->second = font_family_names(source);
                    } catch (const std::runtime_error&) {
                        read->second = std::nullopt;
                    }
                }
                if (!read->second) {
                    return false;
                }
                std::vector<std::string> families{d.family};
                const std::vector<std::string>& named = *read->second;
                for (const auto* list : {&aliases, &named}) {
                    for (const auto& name : *list) {
                        if (std::find(families.begin(), families.end(), name) == families.end()) {
                            families.push_back(name);
                        }
                    }
                }
                postscript_families_.try_emplace(d.postscript, d.family);
                for (auto& family : families) {
                    if (seen_.emplace(family, d.weight, d.italic, source).second) {
                        catalog.faces.push_back({std::move(family), d.weight, d.italic, source});
                    }
                }
                return true;
            }

            bool has(const std::string& family) const {
                return std::any_of(catalog.faces.begin(), catalog.faces.end(), [&](const FontFaceEntry& f) {
                    return f.family == family;
                });
            }

            // The family of the first face added with this PostScript name; empty when none was.
            std::string family_of(const std::string& postscript) const {
                const auto at = postscript_families_.find(postscript);
                return at == postscript_families_.end() ? std::string() : at->second;
            }

            FontCatalog catalog;

        private:
            FaceIndices indices_;
            std::map<std::pair<std::string, std::uint32_t>, std::optional<std::vector<std::string>>> names_;
            std::set<std::tuple<std::string, std::uint16_t, bool, FontFaceSource>> seen_;
            std::map<std::string, std::string> postscript_families_;
        };

        // Hidden families of the UI font's cascade list that have no font file (CoreText keeps them in memory), and the
        // installed family each is the UI cut of (its display name: ".苹方 UI Text-简" is PingFang SC's).
        constexpr std::array<std::pair<const char*, const char*>, 6> hidden_counterparts{
            {{".AppleSimplifiedChineseFont", "PingFang SC"},
             {".AppleTraditionalChineseFont", "PingFang TC"},
             {".AppleHongKongChineseFont", "PingFang HK"},
             {".AppleMacaoChineseFont", "PingFang MO"},
             {".AppleJapaneseFont", "Hiragino Sans"},
             {".AppleKoreanFont", "Apple SD Gothic Neo"}}};

        // The faces CoreText matches to a family name, described; some system families (Times, Courier) are not among
        // the available fonts' collection but are installed and matched by name.
        std::vector<Described> family_faces(const std::string& family) {
            std::vector<Described> out;
            const Ref name(CFStringCreateWithCString(nullptr, family.c_str(), kCFStringEncodingUTF8));
            const void* keys[]{kCTFontFamilyNameAttribute};
            const void* values[]{name.get()};
            const Ref attributes(CFDictionaryCreate(nullptr, keys, values, 1, &kCFTypeDictionaryKeyCallBacks,
                                                    &kCFTypeDictionaryValueCallBacks));
            const Ref descriptor(CTFontDescriptorCreateWithAttributes(attributes.get()));
            const Ref matches(CTFontDescriptorCreateMatchingFontDescriptors(descriptor.get(), nullptr));
            for (CFIndex i = 0; matches && i < CFArrayGetCount(matches.get()); ++i) {
                auto d = describe(static_cast<CTFontDescriptorRef>(CFArrayGetValueAtIndex(matches.get(), i)));
                if (d && d->family == family) {
                    out.push_back(std::move(*d));
                }
            }
            std::sort(out.begin(), out.end(), [](const Described& a, const Described& b) {
                return std::tie(a.postscript, a.path) < std::tie(b.postscript, b.path);
            });
            return out;
        }

        // A font of descriptor with its weight trait set to an NSFontWeight.
        Ref<CTFontRef> with_weight(CTFontRef font, double weight) {
            const Ref base(CTFontCopyFontDescriptor(font));
            const Ref value(CFNumberCreate(nullptr, kCFNumberDoubleType, &weight));
            const void* trait_keys[]{kCTFontWeightTrait};
            const void* trait_values[]{value.get()};
            const Ref traits(CFDictionaryCreate(nullptr, trait_keys, trait_values, 1, &kCFTypeDictionaryKeyCallBacks,
                                                &kCFTypeDictionaryValueCallBacks));
            const void* keys[]{kCTFontTraitsAttribute};
            const void* values[]{traits.get()};
            const Ref attributes(CFDictionaryCreate(nullptr, keys, values, 1, &kCFTypeDictionaryKeyCallBacks,
                                                    &kCFTypeDictionaryValueCallBacks));
            const Ref descriptor(CTFontDescriptorCreateCopyWithAttributes(base.get(), attributes.get()));
            return Ref(CTFontCreateWithFontDescriptor(descriptor.get(), ui_size, nullptr));
        }

        FontCatalog build() {
            Builder builder;
            // The available faces, those of fixed designs before variable fonts' instances of the same family, weight
            // and style (matching takes the first).
            std::vector<Described> available;
            {
                const Ref collection(CTFontCollectionCreateFromAvailableFonts(nullptr));
                const Ref descriptors(collection ? CTFontCollectionCreateMatchingFontDescriptors(collection.get())
                                                 : nullptr);
                if (!descriptors) {
                    throw std::runtime_error("WEFT_SYSTEM_FONTS: CoreText listed no fonts");
                }
                for (CFIndex i = 0; i < CFArrayGetCount(descriptors.get()); ++i) {
                    if (auto d = describe(
                            static_cast<CTFontDescriptorRef>(CFArrayGetValueAtIndex(descriptors.get(), i)))) {
                        available.push_back(std::move(*d));
                    }
                }
            }
            std::stable_sort(available.begin(), available.end(), [](const Described& a, const Described& b) {
                return std::tie(a.family, a.postscript, a.path) < std::tie(b.family, b.postscript, b.path);
            });
            std::stable_partition(available.begin(), available.end(), [](const Described& d) {
                return d.variations.empty();
            });
            for (const auto& d : available) {
                builder.add(d);
            }
            // The generic families' targets, when the collection left them out.
            for (const char* family : {"Times", "Helvetica", "Courier"}) {
                if (!builder.has(family)) {
                    for (const auto& d : family_faces(family)) {
                        builder.add(d);
                    }
                }
            }

            // The UI font at each weight, upright and italic, under its hidden family and Chrome's aliases.
            const Ref ui(CTFontCreateUIFontForLanguage(kCTFontUIFontSystem, ui_size, nullptr));
            if (!ui) {
                throw std::runtime_error("WEFT_SYSTEM_FONTS: no system UI font");
            }
            const std::vector<std::string> ui_aliases{system_ui_family, "-apple-system", "BlinkMacSystemFont"};
            for (const auto& [trait, weight] : weights) {
                const auto upright = with_weight(ui.get(), trait);
                if (!upright) {
                    continue;
                }
                const Ref italic(
                    CTFontCreateCopyWithSymbolicTraits(upright.get(), 0, nullptr, kCTFontItalicTrait, kCTFontItalicTrait));
                for (CTFontRef font : {upright.get(), italic.get()}) {
                    if (auto d = font ? describe(font) : std::nullopt) {
                        d->family = system_ui_family;
                        d->weight = weight;
                        d->italic = font == italic.get();
                        builder.add(*d, ui_aliases);
                    }
                }
            }

            // The UI font's cascade list, its families in order. A hidden family (not among the available faces)
            // stands for its installed counterpart when it has no file (hidden_counterparts), or for a face one of them
            // already lists under its PostScript name; otherwise its face is added regular and bold under the hidden
            // name.
            std::vector<std::string> order{system_ui_family};
            const Ref cascade(CTFontCopyDefaultCascadeListForLanguages(ui.get(), nullptr));
            for (CFIndex i = 0; cascade && i < CFArrayGetCount(cascade.get()); ++i) {
                const auto descriptor = static_cast<CTFontDescriptorRef>(CFArrayGetValueAtIndex(cascade.get(), i));
                const Ref name(
                    static_cast<CFStringRef>(CTFontDescriptorCopyAttribute(descriptor, kCTFontFamilyNameAttribute)));
                auto family = utf8(name.get());
                if (family.empty()) {
                    continue;
                }
                const auto counterpart =
                    std::find_if(hidden_counterparts.begin(), hidden_counterparts.end(), [&](const auto& h) {
                        return family == h.first;
                    });
                if (counterpart != hidden_counterparts.end()) {
                    family = counterpart->second;
                } else if (family.front() == '.' && !builder.has(family)) {
                    const Ref regular(CTFontCreateWithFontDescriptor(descriptor, ui_size, nullptr));
                    const auto described = regular ? describe(regular.get()) : std::nullopt;
                    if (const auto listed = described ? builder.family_of(described->postscript) : std::string();
                        !listed.empty()) {
                        family = listed;
                    } else {
                        const Ref bold(regular ? CTFontCreateCopyWithSymbolicTraits(regular.get(), 0, nullptr,
                                                                                    kCTFontBoldTrait, kCTFontBoldTrait)
                                               : nullptr);
                        for (CTFontRef font : {regular.get(), bold.get()}) {
                            if (auto d = font ? describe(font) : std::nullopt) {
                                d->family = family;
                                builder.add(*d);
                            }
                        }
                    }
                }
                if (builder.has(family) && std::find(order.begin(), order.end(), family) == order.end()) {
                    order.push_back(family);
                }
            }

            // The family CoreText's fallback (CTFontCreateForString, as Chrome on macOS asks it per character) picks
            // from the UI font for a letter of each script that the cascade list may reach too late (the scene shapes
            // with a bounded fallback list): first for that script's text.
            std::vector<std::pair<std::uint32_t, std::vector<std::string>>> script_fallbacks;
            for (const auto& [script, sample] : script_fallback_samples()) {
                const UniChar units[2]{UniChar(sample), 0};
                const Ref text(CFStringCreateWithCharacters(nullptr, units, 1));
                const Ref font(text ? CTFontCreateForString(ui.get(), text.get(), CFRangeMake(0, 1)) : nullptr);
                const auto described = font ? describe(font.get()) : std::nullopt;
                if (!described) {
                    continue;
                }
                auto family = builder.family_of(described->postscript);
                if (family.empty() && !builder.has(described->family)) {
                    // A face of no listed family (a hidden one, as ".SF Devanagari" in SFIndia.ttc): added under
                    // its own name, regular and bold, as the cascade list's hidden families are.
                    const Ref bold(CTFontCreateCopyWithSymbolicTraits(font.get(), 0, nullptr, kCTFontBoldTrait,
                                                                      kCTFontBoldTrait));
                    for (CTFontRef face : {font.get(), bold.get()}) {
                        if (auto d = face ? describe(face) : std::nullopt) {
                            d->family = described->family;
                            builder.add(*d);
                        }
                    }
                }
                if (family.empty() && builder.has(described->family)) {
                    family = described->family;
                }
                if (!family.empty() && family != system_ui_family) {
                    script_fallbacks.push_back({script, {family}});
                }
            }

            auto& catalog = builder.catalog;
            catalog.script_fallbacks = std::move(script_fallbacks);
            catalog.generic_families = {{"serif", "Times"},
                                        {"sans-serif", "Helvetica"},
                                        {"monospace", "Courier"},
                                        {"system-ui", system_ui_family}};
            for (const auto& family : order) {
                if (builder.has(family)) {
                    catalog.fallback_families.push_back(family);
                }
            }
            return std::move(builder.catalog);
        }
    } // namespace

    FontCatalog system_font_catalog() {
        // Listing reads every face's names once; a process sees the fonts installed when it first asks.
        static const FontCatalog catalog = build();
        return catalog;
    }
} // namespace weft::platform::macos
