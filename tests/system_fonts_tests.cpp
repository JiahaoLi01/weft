#include "platform/win32/system_fonts.hpp"
#include "support/check.hpp"

#include <algorithm>
#include <chrono>
#include <iostream>
#include <stdexcept>
#include <string>

// Windows font discovery on this machine's fonts: the installed faces listed from the families' font sets (what
// system_font_catalog does on Windows 10 1903 and later) against a font face made for each font (the only way before),
// entry by entry and in order, since match_font_faces takes the first of equal faces.
namespace {
#define CHECK(v) WEFT_REQUIRE(v)

    using weft::platform::win32::system_font_catalog;
    using weft::platform::win32::SystemFontListing;

    double ms_since(std::chrono::steady_clock::time_point start) {
        return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
    }

    bool same(const weft::FontFaceEntry& a, const weft::FontFaceEntry& b) {
        return a.family == b.family && a.weight == b.weight && a.italic == b.italic && a.source == b.source &&
               a.unicode_range == b.unicode_range;
    }

    std::string describe(const weft::FontFaceEntry& e) {
        std::string out = "'" + e.family + "' " + std::to_string(e.weight) + (e.italic ? " italic " : " ") +
                          e.source.path.string() + " #" + std::to_string(e.source.face_index);
        for (const auto& v : e.source.variations) {
            out += " " + std::to_string(v.tag) + "=" + std::to_string(v.value);
        }
        return out;
    }

    void same_catalogs(const weft::FontCatalog& a, const weft::FontCatalog& b, const char* what) {
        for (std::size_t i = 0; i < std::max(a.faces.size(), b.faces.size()); ++i) {
            if (i >= a.faces.size() || i >= b.faces.size() || !same(a.faces[i], b.faces[i])) {
                throw std::runtime_error(std::string(what) + ": entry " + std::to_string(i) + " is " +
                                         (i < a.faces.size() ? describe(a.faces[i]) : "missing") + " and " +
                                         (i < b.faces.size() ? describe(b.faces[i]) : "missing"));
            }
        }
        CHECK(a.generic_families == b.generic_families);
        CHECK(a.fallback_families == b.fallback_families);
    }

    void SameEntries() {
        auto start = std::chrono::steady_clock::now();
        const auto sets = system_font_catalog(SystemFontListing::FontSets);
        const double sets_ms = ms_since(start);
        start = std::chrono::steady_clock::now();
        const auto faces = system_font_catalog(SystemFontListing::FontFaces);
        const double faces_ms = ms_since(start);
        CHECK(!faces.faces.empty());
        same_catalogs(sets, faces, "font sets and font faces");
        // Named instances of variable fonts keep their axis values; Windows ships variable fonts (Bahnschrift, Segoe UI
        // Variable).
        const auto instances = std::count_if(faces.faces.begin(), faces.faces.end(), [](const weft::FontFaceEntry& e) {
            return !e.source.variations.empty();
        });
        CHECK(instances > 0);
        // A system with font sets lists from them.
        same_catalogs(system_font_catalog(), sets, "automatic and font sets");
        std::cout << "  " << faces.faces.size() << " entries, " << instances
                  << " of named instances; listed from font sets in " << sets_ms << " ms, from font faces in "
                  << faces_ms << " ms\n";
    }
} // namespace

int main() {
    try {
        SameEntries();
        std::cout << "PASS system fonts: font sets list the entries of font faces in their order\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
}
