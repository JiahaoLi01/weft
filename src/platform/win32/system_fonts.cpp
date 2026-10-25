#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "platform/win32/system_fonts.hpp"

#include <windows.h>

#include <dwrite_3.h>
#include <wrl/client.h>

#include <algorithm>
#include <charconv>
#include <cstdint>
#include <map>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace weft::platform::win32 {
    namespace {
        using Microsoft::WRL::ComPtr;

        void check(HRESULT result, const char* operation) {
            if (FAILED(result)) {
                throw std::runtime_error(std::string("WEFT_SYSTEM_FONTS: ") + operation + " failed");
            }
        }

        std::string utf8(const wchar_t* text, int length = -1) {
            const int size = WideCharToMultiByte(CP_UTF8, 0, text, length, nullptr, 0, nullptr, nullptr);
            std::string out(std::size_t(size > 0 ? size : 0), '\0');
            if (size > 0) {
                WideCharToMultiByte(CP_UTF8, 0, text, length, out.data(), size, nullptr, nullptr);
            }
            if (length < 0 && !out.empty()) {
                out.pop_back();
            }
            return out;
        }

        std::vector<std::string> names(IDWriteLocalizedStrings* strings) {
            std::vector<std::string> out;
            for (UINT32 i = 0; i < strings->GetCount(); ++i) {
                UINT32 length{};
                check(strings->GetStringLength(i, &length), "family name length");
                std::wstring value(length + 1, L'\0');
                check(strings->GetString(i, value.data(), length + 1), "family name");
                auto name = utf8(value.c_str());
                if (std::find(out.begin(), out.end(), name) == out.end()) {
                    out.push_back(std::move(name));
                }
            }
            return out;
        }

        std::vector<std::string> family_names(IDWriteFontFamily* family) {
            ComPtr<IDWriteLocalizedStrings> strings;
            check(family->GetFamilyNames(&strings), "family names");
            return names(strings.Get());
        }

        // The path of a local font file; false for a file of another loader.
        bool local_path(IDWriteFontFile* file, std::filesystem::path& out) {
            const void* key{};
            UINT32 key_size{};
            check(file->GetReferenceKey(&key, &key_size), "font file key");
            ComPtr<IDWriteFontFileLoader> loader;
            check(file->GetLoader(&loader), "font file loader");
            ComPtr<IDWriteLocalFontFileLoader> local;
            if (FAILED(loader.As(&local))) {
                return false;
            }
            UINT32 length{};
            check(local->GetFilePathLengthFromKey(key, key_size, &length), "font path length");
            std::wstring path(length + 1, L'\0');
            check(local->GetFilePathFromKey(key, key_size, path.data(), length + 1), "font path");
            path.resize(length);
            out = path;
            return true;
        }

        // The axes a variable font varies: its own fvar axes.
        std::vector<DWRITE_FONT_AXIS_TAG> variable_axes(IDWriteFontResource* resource) {
            std::vector<DWRITE_FONT_AXIS_VALUE> defaults(resource->GetFontAxisCount());
            check(resource->GetDefaultFontAxisValues(defaults.data(), UINT32(defaults.size())), "default axis values");
            std::vector<DWRITE_FONT_AXIS_TAG> out;
            for (UINT32 a = 0; a < defaults.size(); ++a) {
                if (resource->GetFontAxisAttributes(a) & DWRITE_FONT_AXIS_ATTRIBUTES_VARIABLE) {
                    out.push_back(defaults[a].axisTag);
                }
            }
            return out;
        }

        // A named instance's values on the axes its font varies: DirectWrite also reports the standard axes a font
        // lacks.
        std::vector<FontVariation> instance(const std::vector<DWRITE_FONT_AXIS_VALUE>& values,
                                            const std::vector<DWRITE_FONT_AXIS_TAG>& axes) {
            std::vector<FontVariation> out;
            for (const auto& v : values) {
                if (std::find(axes.begin(), axes.end(), v.axisTag) == axes.end()) {
                    continue;
                }
                // DWRITE_FONT_AXIS_TAG stores the four tag bytes in memory order; FreeType and HarfBuzz tags are
                // big-endian packed.
                const auto bytes = static_cast<std::uint32_t>(v.axisTag);
                const std::uint32_t tag = ((bytes & 0xffu) << 24) | (((bytes >> 8) & 0xffu) << 16) |
                                          (((bytes >> 16) & 0xffu) << 8) | ((bytes >> 24) & 0xffu);
                out.push_back({tag, v.value});
            }
            return out;
        }

        // A face under each name of its family, then under each Win32 family name of its own: CSS also matches the
        // legacy Win32 family of a face, e.g. "Segoe UI Semibold".
        void add_face(FontCatalog& catalog, const std::vector<std::string>& family, std::vector<std::string> win32,
                      std::uint16_t weight, bool italic, const FontFaceSource& source) {
            auto aliases = family;
            for (auto& name : win32) {
                if (std::find(aliases.begin(), aliases.end(), name) == aliases.end()) {
                    aliases.push_back(std::move(name));
                }
            }
            for (const auto& alias : aliases) {
                catalog.faces.push_back({alias, weight, italic, source});
            }
        }

        // The axes each file and face index varies, read once for all its named instances.
        using VariableAxes =
            std::map<std::pair<std::filesystem::path, std::uint32_t>, std::vector<DWRITE_FONT_AXIS_TAG>>;

        // Whether some axis range of a font set's face spans values.
        bool spans(IDWriteFontSet1* set, UINT32 index) {
            UINT32 count{};
            if (const auto result = set->GetFontAxisRanges(index, nullptr, 0, &count);
                FAILED(result) && result != E_NOT_SUFFICIENT_BUFFER) {
                check(result, "axis range count");
            }
            std::vector<DWRITE_FONT_AXIS_RANGE> ranges(count);
            check(set->GetFontAxisRanges(index, ranges.data(), count, &count), "axis ranges");
            return std::any_of(ranges.begin(), ranges.end(), [](const DWRITE_FONT_AXIS_RANGE& r) {
                return r.minValue < r.maxValue;
            });
        }

        // A family's faces from its font set: references, properties and axis values come from the system font
        // cache, with no font or face object per face.
        void font_set_faces(FontCatalog& catalog, IDWriteFactory6* factory, IDWriteFontFamily2* family,
                            VariableAxes& variable) {
            const auto aliases = family_names(family);
            ComPtr<IDWriteFontSet1> set;
            check(family->GetFontSet(&set), "family font set");
            for (UINT32 i = 0; i < set->GetFontCount(); ++i) {
                ComPtr<IDWriteFontFaceReference1> reference;
                check(set->GetFontFaceReference(i, &reference), "font face reference");
                if (reference->GetSimulations() != DWRITE_FONT_SIMULATIONS_NONE) {
                    continue;
                }
                ComPtr<IDWriteFontFile> file;
                check(reference->GetFontFile(&file), "font file");
                FontFaceSource source;
                if (!local_path(file.Get(), source.path)) {
                    continue;
                }
                source.face_index = reference->GetFontFaceIndex();
                // Static faces have axis values too (their weight, width, italic and slant), and ranges around them;
                // a face is a named instance of a variable font only when some range spans values, and then its
                // file's resource says which axes the font varies.
                if (const auto count = reference->GetFontAxisValueCount(); count && spans(set.Get(), i)) {
                    std::vector<DWRITE_FONT_AXIS_VALUE> values(count);
                    check(reference->GetFontAxisValues(values.data(), count), "axis values");
                    auto [axes, fresh] = variable.try_emplace({source.path, source.face_index});
                    if (fresh) {
                        ComPtr<IDWriteFontResource> resource;
                        check(factory->CreateFontResource(file.Get(), source.face_index, &resource), "font resource");
                        axes->second = variable_axes(resource.Get());
                    }
                    source.variations = instance(values, axes->second);
                }
                const auto property = [&](DWRITE_FONT_PROPERTY_ID id) {
                    BOOL exists{};
                    ComPtr<IDWriteLocalizedStrings> values;
                    check(set->GetPropertyValues(i, id, &exists, &values), "font property");
                    return exists ? names(values.Get()) : std::vector<std::string>{};
                };
                // DWRITE_FONT_WEIGHT and DWRITE_FONT_STYLE as decimal text.
                const auto number = [&](DWRITE_FONT_PROPERTY_ID id, int fallback) {
                    const auto text = property(id);
                    int value = fallback;
                    if (!text.empty()) {
                        const auto& s = text.front();
                        if (std::from_chars(s.data(), s.data() + s.size(), value).ec != std::errc{}) {
                            throw std::runtime_error("WEFT_SYSTEM_FONTS: font property " + s);
                        }
                    }
                    return value;
                };
                add_face(catalog, aliases, property(DWRITE_FONT_PROPERTY_ID_WIN32_FAMILY_NAME),
                         std::uint16_t(number(DWRITE_FONT_PROPERTY_ID_WEIGHT, DWRITE_FONT_WEIGHT_NORMAL)),
                         number(DWRITE_FONT_PROPERTY_ID_STYLE, DWRITE_FONT_STYLE_NORMAL) != DWRITE_FONT_STYLE_NORMAL,
                         source);
            }
        }

        // A family's faces from a font face per font.
        void font_faces(FontCatalog& catalog, IDWriteFontFamily* family) {
            const auto aliases = family_names(family);
            for (UINT32 i = 0; i < family->GetFontCount(); ++i) {
                ComPtr<IDWriteFont> font;
                check(family->GetFont(i, &font), "font");
                if (font->GetSimulations() != DWRITE_FONT_SIMULATIONS_NONE) {
                    continue;
                }
                ComPtr<IDWriteFontFace> face;
                if (FAILED(font->CreateFontFace(&face))) {
                    continue;
                }
                UINT32 count{};
                check(face->GetFiles(&count, nullptr), "font file count");
                if (count != 1) {
                    continue;
                }
                ComPtr<IDWriteFontFile> file;
                check(face->GetFiles(&count, &file), "font file");
                FontFaceSource source;
                if (!local_path(file.Get(), source.path)) {
                    continue;
                }
                source.face_index = face->GetIndex();
                ComPtr<IDWriteFontFace5> variable;
                if (SUCCEEDED(face.As(&variable)) && variable->HasVariations()) {
                    std::vector<DWRITE_FONT_AXIS_VALUE> values(variable->GetFontAxisValueCount());
                    check(variable->GetFontAxisValues(values.data(), UINT32(values.size())), "axis values");
                    ComPtr<IDWriteFontResource> resource;
                    check(variable->GetFontResource(&resource), "font resource");
                    source.variations = instance(values, variable_axes(resource.Get()));
                }
                std::vector<std::string> win32;
                ComPtr<IDWriteLocalizedStrings> strings;
                BOOL exists{};
                if (SUCCEEDED(font->GetInformationalStrings(DWRITE_INFORMATIONAL_STRING_WIN32_FAMILY_NAMES, &strings,
                                                            &exists)) &&
                    exists) {
                    win32 = names(strings.Get());
                }
                add_face(catalog, aliases, std::move(win32), std::uint16_t(font->GetWeight()),
                         font->GetStyle() != DWRITE_FONT_STYLE_NORMAL, source);
            }
        }
    } // namespace

    std::string system_ui_family() {
        NONCLIENTMETRICSW metrics{};
        metrics.cbSize = sizeof(metrics);
        if (!SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof(metrics), &metrics, 0)) {
            return "Segoe UI";
        }
        return utf8(metrics.lfMessageFont.lfFaceName);
    }

    FontCatalog system_font_catalog(SystemFontListing listing) {
        ComPtr<IDWriteFactory3> factory;
        check(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory3),
                                  reinterpret_cast<IUnknown**>(factory.GetAddressOf())),
              "DWriteCreateFactory");
        // Families' font sets and font resources came with Windows 10 1903.
        ComPtr<IDWriteFactory6> sets;
        if (listing != SystemFontListing::FontFaces && FAILED(factory.As(&sets)) &&
            listing == SystemFontListing::FontSets) {
            throw std::runtime_error("WEFT_SYSTEM_FONTS: font sets need Windows 10 1903 or later");
        }
        FontCatalog catalog;
        if (sets) {
            ComPtr<IDWriteFontCollection2> collection;
            check(sets->GetSystemFontCollection(FALSE, DWRITE_FONT_FAMILY_MODEL_WEIGHT_STRETCH_STYLE, &collection),
                  "system font collection");
            VariableAxes variable;
            for (UINT32 f = 0; f < collection->GetFontFamilyCount(); ++f) {
                ComPtr<IDWriteFontFamily2> family;
                check(collection->GetFontFamily(f, &family), "font family");
                font_set_faces(catalog, sets.Get(), family.Get(), variable);
            }
        } else {
            ComPtr<IDWriteFontCollection> collection;
            check(factory->GetSystemFontCollection(&collection, FALSE), "system font collection");
            for (UINT32 f = 0; f < collection->GetFontFamilyCount(); ++f) {
                ComPtr<IDWriteFontFamily> family;
                check(collection->GetFontFamily(f, &family), "font family");
                font_faces(catalog, family.Get());
            }
        }
        const auto ui = system_ui_family();
        catalog.generic_families = {
            {"serif", "Times New Roman"}, {"sans-serif", "Arial"}, {"monospace", "Consolas"}, {"system-ui", ui}};
        catalog.fallback_families = {ui, "Segoe UI Emoji", "Segoe UI Symbol"};
        return catalog;
    }
} // namespace weft::platform::win32
