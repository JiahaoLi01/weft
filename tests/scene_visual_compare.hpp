#pragma once
#include <cmath>
#include "test_constants/scene.hpp"

// Windows fixture decoder; production never loads reference images.
inline caseelephant::ProbeSceneBitmap scene_reference(const std::filesystem::path& path) {
    ComPtr<IWICImagingFactory> f;
    hr(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&f)));
    ComPtr<IWICBitmapDecoder> decoder;
    hr(f->CreateDecoderFromFilename(path.c_str(), nullptr, GENERIC_READ, WICDecodeMetadataCacheOnLoad, &decoder));
    ComPtr<IWICBitmapFrameDecode> frame;
    hr(decoder->GetFrame(0, &frame));
    UINT w{};
    UINT h{};
    hr(frame->GetSize(&w, &h));
    ComPtr<IWICFormatConverter> converter;
    hr(f->CreateFormatConverter(&converter));
    hr(converter->Initialize(frame.Get(), GUID_WICPixelFormat32bppRGBA, WICBitmapDitherTypeNone, nullptr, 0,
                             WICBitmapPaletteTypeCustom));
    caseelephant::ProbeSceneBitmap out{w, h, std::vector<std::uint8_t>(std::size_t(w) * h * 4)};
    hr(converter->CopyPixels(nullptr, w * 4, UINT(out.rgba.size()), out.rgba.data()));
    return out;
}

struct SceneDifference {
    std::array<double, 4> mean{};
    double soft_iou{};
    std::size_t foreground{};

    bool passes() const {
        return *std::max_element(mean.begin(), mean.end()) <=
                   caseelephant::test_constants::scene::max_channel_mean_difference &&
               soft_iou >= caseelephant::test_constants::scene::min_soft_iou &&
               foreground >= caseelephant::test_constants::scene::min_foreground_pixels;
    }
};

inline SceneDifference scene_difference(const caseelephant::ProbeSceneBitmap& a,
                                        const caseelephant::ProbeSceneBitmap& b) {
    require(a.width == b.width && a.height == b.height, "reference extent");
    const auto pixels = std::size_t(a.width) * a.height;
    SceneDifference d;
    std::vector<unsigned char> am(pixels);
    std::vector<unsigned char> bm(pixels);
    for (std::size_t i = 0; i < pixels; ++i) {
        for (unsigned c = 0; c < 4; ++c) {
            d.mean[c] += std::abs(int(a.rgba[i * 4 + c]) - b.rgba[i * 4 + c]);
        }
        am[i] = std::max({std::abs(int(a.rgba[i * 4]) - caseelephant::test_constants::scene::backdrop_red),
                          std::abs(int(a.rgba[i * 4 + 1]) - caseelephant::test_constants::scene::backdrop_green),
                          std::abs(int(a.rgba[i * 4 + 2]) - caseelephant::test_constants::scene::backdrop_blue)}) >
                        caseelephant::test_constants::scene::foreground_channel_threshold
                    ? caseelephant::constants::color_channel_max
                    : 0;
        bm[i] = std::max({std::abs(int(b.rgba[i * 4]) - caseelephant::test_constants::scene::backdrop_red),
                          std::abs(int(b.rgba[i * 4 + 1]) - caseelephant::test_constants::scene::backdrop_green),
                          std::abs(int(b.rgba[i * 4 + 2]) - caseelephant::test_constants::scene::backdrop_blue)}) >
                        caseelephant::test_constants::scene::foreground_channel_threshold
                    ? caseelephant::constants::color_channel_max
                    : 0;
    }
    for (auto& m : d.mean) {
        m /= pixels;
    }
    auto blur = [&](const auto& mask) {
        std::vector<unsigned char> temp(pixels);
        std::vector<unsigned char> result(pixels);
        for (unsigned y = 0; y < a.height; ++y) {
            for (unsigned x = 0; x < a.width; ++x) {
                auto p = std::size_t(y) * a.width + x;
                temp[p] = static_cast<unsigned char>((mask[std::size_t(y) * a.width + (x ? x - 1 : x)] + 2 * mask[p] +
                                                      mask[std::size_t(y) * a.width + std::min(x + 1, a.width - 1)]) /
                                                     4);
            }
        }
        for (unsigned y = 0; y < a.height; ++y) {
            for (unsigned x = 0; x < a.width; ++x) {
                auto p = std::size_t(y) * a.width + x;
                result[p] = (temp[std::size_t(y ? y - 1 : y) * a.width + x] + 2 * temp[p] +
                             temp[std::size_t(std::min(y + 1, a.height - 1)) * a.width + x]) /
                                4 >
                            caseelephant::test_constants::scene::blurred_foreground_threshold;
            }
        }
        return result;
    };
    auto x = blur(am);
    auto y = blur(bm);
    std::size_t intersection{};
    std::size_t united{};
    for (std::size_t i = 0; i < pixels; ++i) {
        intersection += x[i] && y[i];
        united += x[i] || y[i];
        d.foreground += y[i];
    }
    d.soft_iou = united ? double(intersection) / united : 1;
    return d;
}
