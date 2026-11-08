#include "view/view_image_codecs.hpp"

#include "constants/common.hpp"

// libjpeg's header needs size_t and FILE declared before it.
#include <cstddef>
#include <cstdio>

#include <jpeglib.h>
#include <webp/decode.h>
#include <webp/demux.h>

#include <csetjmp>
#include <cstdlib>
#include <cstring>
#include <utility>
#include <vector>

namespace weft::view_codecs {
    namespace {
        // libjpeg reports a fatal error through error_exit, which must not return: it jumps back to decode_jpeg's
        // setjmp with the message. Warnings (a corrupt but decodable stream) are not reported, as Blink ignores them.
        struct JpegError {
            jpeg_error_mgr manager;
            std::jmp_buf jump;
            char message[JMSG_LENGTH_MAX];
        };

        extern "C" void jpeg_fail(j_common_ptr info) {
            auto* error = reinterpret_cast<JpegError*>(info->err);
            (*info->err->format_message)(info, error->message);
            std::longjmp(error->jump, 1);
        }

        extern "C" void jpeg_quiet(j_common_ptr, int) {
        }

        // What decode_jpeg's libjpeg part gives: the pixels (malloc'd, the caller frees them), their size, the Exif
        // orientation, or why there are none. Plain data only: a longjmp crosses the function that fills it.
        struct JpegResult {
            unsigned char* pixels{};
            std::uint32_t width{};
            std::uint32_t height{};
            int orientation{1};
            bool bounded{};
            char message[JMSG_LENGTH_MAX]{};
        };

        void run_jpeg(const unsigned char* data, std::size_t size, std::uint32_t side, std::size_t limit,
                      JpegResult& result) {
            jpeg_decompress_struct info{};
            JpegError error{};
            info.err = jpeg_std_error(&error.manager);
            error.manager.error_exit = jpeg_fail;
            error.manager.emit_message = jpeg_quiet;
            // Read after a longjmp: volatile.
            unsigned char* volatile pixels = nullptr;
            unsigned char* volatile row = nullptr;
            if (setjmp(error.jump)) {
                jpeg_destroy_decompress(&info);
                std::free(pixels);
                std::free(row);
                std::memcpy(result.message, error.message, sizeof(result.message));
                return;
            }
            // Known from its header on: a JPEG whose data does not decode keeps its size (decode_jpeg).
            jpeg_create_decompress(&info);
            jpeg_mem_src(&info, data, static_cast<unsigned long>(size));
            jpeg_save_markers(&info, JPEG_APP0 + 1, 0xFFFF);
            jpeg_read_header(&info, TRUE);
            result.width = info.image_width;
            result.height = info.image_height;
            if (!info.image_width || !info.image_height || info.image_width > side || info.image_height > side ||
                std::size_t(info.image_width) * info.image_height > limit) {
                result.bounded = true;
                jpeg_destroy_decompress(&info);
                return;
            }
            bool cmyk{};
            switch (info.jpeg_color_space) {
                case JCS_YCbCr:
                case JCS_GRAYSCALE:
                case JCS_RGB:
                    info.out_color_space = JCS_EXT_RGBA;
                    break;
                case JCS_CMYK:
                case JCS_YCCK:
                    info.out_color_space = JCS_CMYK;
                    cmyk = true;
                    break;
                default:
                    std::snprintf(error.message, sizeof(error.message), "a JPEG color space it cannot convert");
                    std::longjmp(error.jump, 1);
            }
            // As Blink's JPEGImageReader sets them.
            info.dct_method = JDCT_ISLOW;
            info.dither_mode = JDITHER_FS;
            info.do_fancy_upsampling = TRUE;
            info.do_block_smoothing = TRUE;
            info.enable_2pass_quant = FALSE;
            info.enable_external_quant = FALSE;
            info.enable_1pass_quant = FALSE;
            info.quantize_colors = FALSE;
            for (auto* marker = info.marker_list; marker; marker = marker->next) {
                if (marker->marker == JPEG_APP0 + 1) {
                    const int o = exif_orientation({marker->data, marker->data_length});
                    if (o != 1) {
                        result.orientation = o;
                        break;
                    }
                }
            }
            jpeg_start_decompress(&info);
            const std::uint32_t w = info.output_width;
            const std::uint32_t h = info.output_height;
            pixels = static_cast<unsigned char*>(std::malloc(std::size_t(w) * h * constants::rgba8_bytes_per_pixel));
            if (cmyk) {
                row = static_cast<unsigned char*>(std::malloc(std::size_t(w) * 4));
            }
            if (!pixels || (cmyk && !row)) {
                std::snprintf(error.message, sizeof(error.message), "no memory for the pixels");
                std::longjmp(error.jump, 1);
            }
            while (info.output_scanline < h) {
                const auto y = info.output_scanline;
                unsigned char* line = pixels + std::size_t(y) * w * constants::rgba8_bytes_per_pixel;
                JSAMPROW target = cmyk ? row : line;
                if (jpeg_read_scanlines(&info, &target, 1) != 1) {
                    std::snprintf(error.message, sizeof(error.message), "a truncated JPEG");
                    std::longjmp(error.jump, 1);
                }
                if (cmyk) {
                    // Adobe's inverted CMYK to RGB as Blink converts it: each channel times K over 255.
                    for (std::uint32_t x = 0; x < w; ++x) {
                        const unsigned k = row[x * 4 + 3];
                        line[x * 4] = static_cast<unsigned char>(row[x * 4] * k / 255);
                        line[x * 4 + 1] = static_cast<unsigned char>(row[x * 4 + 1] * k / 255);
                        line[x * 4 + 2] = static_cast<unsigned char>(row[x * 4 + 2] * k / 255);
                        line[x * 4 + 3] = 255;
                    }
                }
            }
            jpeg_finish_decompress(&info);
            jpeg_destroy_decompress(&info);
            std::free(row);
            result.pixels = pixels;
            result.width = w;
            result.height = h;
        }

        std::string size_text(std::uint32_t w, std::uint32_t h) {
            return std::to_string(w) + " x " + std::to_string(h);
        }

        std::uint32_t read(std::span<const std::uint8_t> s, std::size_t at, int bytes, bool big) {
            std::uint32_t v{};
            for (int k = 0; k < bytes; ++k) {
                const auto byte = std::uint32_t(s[at + std::size_t(big ? k : bytes - 1 - k)]);
                v = (v << 8) | byte;
            }
            return v;
        }
    } // namespace

    int exif_orientation(std::span<const std::uint8_t> app1) {
        static constexpr std::uint8_t exif[]{'E', 'x', 'i', 'f', 0, 0};
        if (app1.size() < 6 + 8 || std::memcmp(app1.data(), exif, 6) != 0) {
            return 1;
        }
        const auto tiff = app1.subspan(6);
        bool big{};
        if (tiff[0] == 'M' && tiff[1] == 'M') {
            big = true;
        } else if (tiff[0] != 'I' || tiff[1] != 'I') {
            return 1;
        }
        if (read(tiff, 2, 2, big) != 42) {
            return 1;
        }
        const std::size_t ifd = read(tiff, 4, 4, big);
        if (ifd + 2 > tiff.size()) {
            return 1;
        }
        const std::size_t entries = read(tiff, ifd, 2, big);
        for (std::size_t k = 0; k < entries; ++k) {
            const std::size_t at = ifd + 2 + k * 12;
            if (at + 12 > tiff.size()) {
                return 1;
            }
            // Orientation: one SHORT.
            if (read(tiff, at, 2, big) == 0x0112 && read(tiff, at + 2, 2, big) == 3 &&
                read(tiff, at + 4, 4, big) == 1) {
                const auto o = int(read(tiff, at + 8, 2, big));
                return o >= 1 && o <= 8 ? o : 1;
            }
        }
        return 1;
    }

    void orient(std::vector<std::uint8_t>& rgba, std::uint32_t& w, std::uint32_t& h, int o) {
        if (o < 2 || o > 8) {
            return;
        }
        const bool swap = o >= 5;
        const std::uint32_t ow = swap ? h : w;
        const std::uint32_t oh = swap ? w : h;
        std::vector<std::uint8_t> out(rgba.size());
        for (std::uint32_t y = 0; y < oh; ++y) {
            for (std::uint32_t x = 0; x < ow; ++x) {
                // The source pixel of output pixel x, y.
                std::uint32_t sx{};
                std::uint32_t sy{};
                switch (o) {
                    case 2:
                        sx = w - 1 - x;
                        sy = y;
                        break;
                    case 3:
                        sx = w - 1 - x;
                        sy = h - 1 - y;
                        break;
                    case 4:
                        sx = x;
                        sy = h - 1 - y;
                        break;
                    case 5:
                        sx = y;
                        sy = x;
                        break;
                    case 6:
                        sx = y;
                        sy = h - 1 - x;
                        break;
                    case 7:
                        sx = w - 1 - y;
                        sy = h - 1 - x;
                        break;
                    default:
                        sx = w - 1 - y;
                        sy = x;
                        break;
                }
                std::memcpy(&out[(std::size_t(y) * ow + x) * 4], &rgba[(std::size_t(sy) * w + sx) * 4], 4);
            }
        }
        rgba = std::move(out);
        w = ow;
        h = oh;
    }

    std::optional<std::string> decode_jpeg(std::span<const std::uint8_t> bytes, std::uint32_t side, std::size_t pixels,
                                           paint::Asset& out) {
        JpegResult result;
        run_jpeg(bytes.data(), bytes.size(), side, pixels, result);
        if (result.bounded) {
            return "a JPEG image of " + size_text(result.width, result.height) + " pixels, past the bounds";
        }
        if (!result.pixels) {
            const auto why = std::string("no JPEG image: ") + result.message;
            if (result.width && result.height) {
                // Its header gave its size: Chrome keeps the size and draws nothing (12-bit samples, arithmetic
                // coding, data that does not decode). So does weft: transparent pixels. Its Exif orientation stands.
                out.width = result.width;
                out.height = result.height;
                out.encoding = paint::Encoding::SrgbStraight;
                out.bytes.assign(std::size_t(result.width) * result.height * constants::rgba8_bytes_per_pixel, 0);
                orient(out.bytes, out.width, out.height, result.orientation);
                return why + " (drawn as nothing, at its size)";
            }
            return why;
        }
        out.width = result.width;
        out.height = result.height;
        out.encoding = paint::Encoding::SrgbStraight;
        out.bytes.assign(result.pixels,
                         result.pixels + std::size_t(result.width) * result.height * constants::rgba8_bytes_per_pixel);
        std::free(result.pixels);
        orient(out.bytes, out.width, out.height, result.orientation);
        return std::nullopt;
    }

    std::optional<std::string> decode_webp(std::span<const std::uint8_t> bytes, std::uint32_t side, std::size_t pixels,
                                           paint::Asset& out) {
        WebPBitstreamFeatures features{};
        if (WebPGetFeatures(bytes.data(), bytes.size(), &features) != VP8_STATUS_OK) {
            return std::string("no WebP image");
        }
        const auto w = std::uint32_t(features.width);
        const auto h = std::uint32_t(features.height);
        if (!w || !h || w > side || h > side || std::size_t(w) * h > pixels) {
            return "a WebP image of " + size_text(w, h) + " pixels, past the bounds";
        }
        if (features.has_animation) {
            // Its first frame on its canvas (libwebp's animation decoder blends and disposes frames as the format
            // says), straight alpha: what Chrome draws of it before it plays. It does not play.
            WebPAnimDecoderOptions options;
            if (!WebPAnimDecoderOptionsInit(&options)) {
                return std::string("no WebP image: libwebp's animation decoder");
            }
            options.color_mode = MODE_RGBA;
            options.use_threads = 0;
            const WebPData data{bytes.data(), bytes.size()};
            WebPAnimDecoder* decoder = WebPAnimDecoderNew(&data, &options);
            WebPAnimInfo info{};
            std::uint8_t* frame{};
            int timestamp{};
            const bool decoded = decoder && WebPAnimDecoderGetInfo(decoder, &info) && info.canvas_width == w &&
                                 info.canvas_height == h && WebPAnimDecoderGetNext(decoder, &frame, &timestamp);
            out.width = w;
            out.height = h;
            out.encoding = paint::Encoding::SrgbStraight;
            if (decoded) {
                out.bytes.assign(frame, frame + std::size_t(w) * h * constants::rgba8_bytes_per_pixel);
            } else {
                out.bytes.assign(std::size_t(w) * h * constants::rgba8_bytes_per_pixel, 0);
            }
            WebPAnimDecoderDelete(decoder);
            return decoded ? std::string("an animated WebP image: its first frame (the scene does not play it)")
                           : std::string("no WebP image: its frames do not decode (drawn as nothing, at its size)");
        }
        std::vector<std::uint8_t> rgba(std::size_t(w) * h * constants::rgba8_bytes_per_pixel);
        if (!WebPDecodeRGBAInto(bytes.data(), bytes.size(), rgba.data(), rgba.size(),
                                int(w * constants::rgba8_bytes_per_pixel))) {
            // Its size stands, as with a JPEG: transparent pixels.
            out.width = w;
            out.height = h;
            out.encoding = paint::Encoding::SrgbStraight;
            out.bytes.assign(rgba.size(), 0);
            return std::string("no WebP image: its data does not decode (drawn as nothing, at its size)");
        }
        out.width = w;
        out.height = h;
        out.encoding = paint::Encoding::SrgbStraight;
        out.bytes = std::move(rgba);
        return std::nullopt;
    }
} // namespace weft::view_codecs
