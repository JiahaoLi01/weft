#pragma once

#include "paint/display_list.hpp"

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

// The JPEG and WebP decoders of a page's images (view_images.cpp): libjpeg-turbo and libwebp as Chrome's Blink uses
// them, into straight sRGB RGBA8. Each returns why the bytes give no image, or none and the pixels in out. An image
// whose header gives its size but whose data does not decode is transparent pixels of that size in out, and why: Chrome
// keeps such an image's size and draws nothing.
namespace weft::view_codecs {
    // A JPEG (baseline, progressive, lossless, 8-bit samples; not arithmetic-coded, as Chrome's; YCbCr, grayscale,
    // RGB, and CMYK or YCCK taken as Adobe's
    // inverted CMYK, as Blink converts it), decoded as Blink's JPEGImageDecoder sets libjpeg-turbo up: the accurate
    // integer IDCT, fancy upsampling and block smoothing. Its Exif orientation (APP1) is applied, as Chrome applies
    // it by default (image-orientation: from-image). Its ICC profile is not: its colors are taken as sRGB. An image
    // wider or taller than side, or of more than pixels pixels, is past the bounds and not decoded.
    std::optional<std::string> decode_jpeg(std::span<const std::uint8_t> bytes, std::uint32_t side, std::size_t pixels,
                                           paint::Asset& out);

    // A WebP (lossy VP8, lossless VP8L, with or without alpha) as libwebp decodes it with its default options (fancy
    // upsampling), straight alpha. Of an animated one, its first frame, and why (it does not play). Bounds as
    // decode_jpeg's.
    std::optional<std::string> decode_webp(std::span<const std::uint8_t> bytes, std::uint32_t side, std::size_t pixels,
                                           paint::Asset& out);

    // The Exif orientation (1 to 8) of a JPEG's APP1 segment payload ("Exif\0\0" and a TIFF structure); 1 without
    // one.
    int exif_orientation(std::span<const std::uint8_t> app1);

    // RGBA8 pixels of w x h turned as Exif orientation o says (2 to 8 mirror, rotate or transpose them; 5 to 8 swap
    // the sides, which w and h then hold).
    void orient(std::vector<std::uint8_t>& rgba, std::uint32_t& w, std::uint32_t& h, int o);
} // namespace weft::view_codecs
