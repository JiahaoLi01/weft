#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "platform/win32/system_fonts.hpp"

#include <windows.h>

#include <dwrite_3.h>
#include <wrl/client.h>

#include <algorithm>
#include <cstdint>
#include <stdexcept>
#include <string>
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

        // File path and collection index of a local font file; false for fonts from other loaders.
        bool local_source(IDWriteFontFace* face, FontFaceSource& source) {
            UINT32 count{};
            check(face->GetFiles(&count, nullptr), "font file count");
            if (count != 1) {
                return false;
            }
            ComPtr<IDWriteFontFile> file;
            check(face->GetFiles(&count, &file), "font file");
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
            source.path = path;
            source.face_index = face->GetIndex();
            return true;
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

    FontCatalog system_font_catalog() {
        ComPtr<IDWriteFactory3> factory;
        check(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory3),
                                  reinterpret_cast<IUnknown**>(factory.GetAddressOf())),
              "DWriteCreateFactory");
        ComPtr<IDWriteFontCollection> collection;
        check(factory->GetSystemFontCollection(&collection, FALSE), "system font collection");
        FontCatalog catalog;
        for (UINT32 f = 0; f < collection->GetFontFamilyCount(); ++f) {
            ComPtr<IDWriteFontFamily> family;
            check(collection->GetFontFamily(f, &family), "font family");
            ComPtr<IDWriteLocalizedStrings> family_names;
            check(family->GetFamilyNames(&family_names), "family names");
            const auto aliases = names(family_names.Get());
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
                FontFaceSource source;
                if (!local_source(face.Get(), source)) {
                    continue;
                }
                ComPtr<IDWriteFontFace5> variable;
                if (SUCCEEDED(face.As(&variable)) && variable->HasVariations()) {
                    std::vector<DWRITE_FONT_AXIS_VALUE> values(variable->GetFontAxisValueCount());
                    check(variable->GetFontAxisValues(values.data(), UINT32(values.size())), "axis values");
                    // DirectWrite also reports the standard axes a font lacks; keep the font's own fvar axes.
                    ComPtr<IDWriteFontResource> resource;
                    check(variable->GetFontResource(&resource), "font resource");
                    std::vector<DWRITE_FONT_AXIS_VALUE> defaults(resource->GetFontAxisCount());
                    check(resource->GetDefaultFontAxisValues(defaults.data(), UINT32(defaults.size())),
                          "default axis values");
                    std::vector<DWRITE_FONT_AXIS_TAG> own;
                    for (UINT32 a = 0; a < defaults.size(); ++a) {
                        if (resource->GetFontAxisAttributes(a) & DWRITE_FONT_AXIS_ATTRIBUTES_VARIABLE) {
                            own.push_back(defaults[a].axisTag);
                        }
                    }
                    std::erase_if(values, [&](const DWRITE_FONT_AXIS_VALUE& v) {
                        return std::find(own.begin(), own.end(), v.axisTag) == own.end();
                    });
                    for (const auto& v : values) {
                        // DWRITE_FONT_AXIS_TAG stores the four tag bytes in memory order; FreeType and HarfBuzz
                        // tags are big-endian packed.
                        const auto bytes = static_cast<std::uint32_t>(v.axisTag);
                        const std::uint32_t tag = ((bytes & 0xffu) << 24) | (((bytes >> 8) & 0xffu) << 16) |
                                                  (((bytes >> 16) & 0xffu) << 8) | ((bytes >> 24) & 0xffu);
                        source.variations.push_back({tag, v.value});
                    }
                }
                // CSS also matches the legacy Win32 family of a face, e.g. "Segoe UI Semibold".
                auto face_aliases = aliases;
                ComPtr<IDWriteLocalizedStrings> win32;
                BOOL exists{};
                if (SUCCEEDED(font->GetInformationalStrings(DWRITE_INFORMATIONAL_STRING_WIN32_FAMILY_NAMES, &win32,
                                                            &exists)) &&
                    exists) {
                    for (auto& name : names(win32.Get())) {
                        if (std::find(face_aliases.begin(), face_aliases.end(), name) == face_aliases.end()) {
                            face_aliases.push_back(std::move(name));
                        }
                    }
                }
                const bool italic = font->GetStyle() != DWRITE_FONT_STYLE_NORMAL;
                for (const auto& alias : face_aliases) {
                    catalog.faces.push_back({alias, std::uint16_t(font->GetWeight()), italic, source});
                }
            }
        }
        const auto ui = system_ui_family();
        catalog.generic_families = {
            {"serif", "Times New Roman"}, {"sans-serif", "Arial"}, {"monospace", "Consolas"}, {"system-ui", ui}};
        catalog.fallback_families = {ui, "Segoe UI Emoji", "Segoe UI Symbol"};
        return catalog;
    }
} // namespace weft::platform::win32
