#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>
#include <vector>

// A PNG file of straight RGBA8 rows (top-down) in memory, with stored (uncompressed) deflate blocks: no codec and no
// COM, for tests that hand pages and effects PNG bytes.
namespace weft::testing {
    inline std::uint32_t png_crc32(const std::uint8_t* data, std::size_t size) {
        static const auto table = [] {
            std::array<std::uint32_t, 256> t{};
            for (std::uint32_t n = 0; n < 256; ++n) {
                std::uint32_t c = n;
                for (int k = 0; k < 8; ++k) {
                    c = c & 1 ? 0xedb88320u ^ (c >> 1) : c >> 1;
                }
                t[n] = c;
            }
            return t;
        }();
        std::uint32_t crc = ~0u;
        for (std::size_t i = 0; i < size; ++i) {
            crc = table[(crc ^ data[i]) & 0xff] ^ (crc >> 8);
        }
        return ~crc;
    }

    inline std::vector<std::uint8_t> encode_png(std::uint32_t width, std::uint32_t height,
                                                const std::vector<std::uint8_t>& rgba) {
        std::vector<std::uint8_t> raw;
        raw.reserve(std::size_t(height) * (std::size_t(width) * 4 + 1));
        for (std::uint32_t y = 0; y < height; ++y) {
            raw.push_back(0);
            const auto* row = rgba.data() + std::size_t(y) * width * 4;
            raw.insert(raw.end(), row, row + std::size_t(width) * 4);
        }
        std::vector<std::uint8_t> z{0x78, 0x01};
        std::uint32_t a = 1;
        std::uint32_t b = 0;
        for (std::size_t at = 0;;) {
            const std::size_t n = std::min<std::size_t>(65535, raw.size() - at);
            const bool last = at + n == raw.size();
            z.push_back(last ? 1 : 0);
            z.push_back(std::uint8_t(n & 0xff));
            z.push_back(std::uint8_t(n >> 8));
            z.push_back(std::uint8_t(~n & 0xff));
            z.push_back(std::uint8_t((~n >> 8) & 0xff));
            z.insert(z.end(), raw.begin() + std::ptrdiff_t(at), raw.begin() + std::ptrdiff_t(at + n));
            for (std::size_t i = at; i < at + n; ++i) {
                a = (a + raw[i]) % 65521;
                b = (b + a) % 65521;
            }
            at += n;
            if (last) {
                break;
            }
        }
        const std::uint32_t adler = (b << 16) | a;
        for (int s = 24; s >= 0; s -= 8) {
            z.push_back(std::uint8_t(adler >> s));
        }
        std::vector<std::uint8_t> out{0x89, 'P', 'N', 'G', '\r', '\n', 0x1a, '\n'};
        const auto put32 = [&](std::uint32_t v) {
            for (int s = 24; s >= 0; s -= 8) {
                out.push_back(std::uint8_t(v >> s));
            }
        };
        const auto chunk = [&](std::string_view type, const std::vector<std::uint8_t>& data) {
            put32(std::uint32_t(data.size()));
            const auto start = out.size();
            out.insert(out.end(), type.begin(), type.end());
            out.insert(out.end(), data.begin(), data.end());
            put32(png_crc32(out.data() + start, out.size() - start));
        };
        std::vector<std::uint8_t> header(13);
        for (int i = 0; i < 4; ++i) {
            header[std::size_t(i)] = std::uint8_t(width >> (24 - 8 * i));
            header[std::size_t(4 + i)] = std::uint8_t(height >> (24 - 8 * i));
        }
        header[8] = 8; // bit depth
        header[9] = 6; // RGBA
        chunk("IHDR", header);
        chunk("IDAT", z);
        chunk("IEND", {});
        return out;
    }
} // namespace weft::testing
