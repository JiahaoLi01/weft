#pragma once

// Windows Imaging Component PNG I/O for fixtures and evidence images. COM must already be
// initialized on the calling thread. Pixels are straight (non-premultiplied) RGBA8, rows top-down.

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <wincodec.h>
#include <wrl/client.h>

#include <cstdint>
#include <filesystem>
#include <span>
#include <sstream>
#include <stdexcept>
#include <utility>
#include <vector>

namespace weft::testing {
    struct RgbaImage {
        std::uint32_t width{};
        std::uint32_t height{};
        std::vector<std::uint8_t> rgba;
    };

    inline constexpr std::uint32_t png_bytes_per_pixel = 4;

    inline void check_hresult(HRESULT result, const char* operation) {
        if (FAILED(result)) {
            std::ostringstream message;
            message << operation << " HRESULT=0x" << std::hex << static_cast<unsigned long>(result);
            throw std::runtime_error(message.str());
        }
    }

    inline Microsoft::WRL::ComPtr<IWICImagingFactory> wic_factory() {
        Microsoft::WRL::ComPtr<IWICImagingFactory> factory;
        check_hresult(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory)),
                      "WIC factory");
        return factory;
    }

    inline RgbaImage read_png(const std::filesystem::path& path) {
        const auto factory = wic_factory();
        Microsoft::WRL::ComPtr<IWICBitmapDecoder> decoder;
        check_hresult(factory->CreateDecoderFromFilename(path.c_str(), nullptr, GENERIC_READ,
                                                         WICDecodeMetadataCacheOnLoad, &decoder),
                      "PNG decoder");
        Microsoft::WRL::ComPtr<IWICBitmapFrameDecode> frame;
        check_hresult(decoder->GetFrame(0, &frame), "PNG frame");
        RgbaImage image;
        check_hresult(frame->GetSize(&image.width, &image.height), "PNG size");
        Microsoft::WRL::ComPtr<IWICFormatConverter> converter;
        check_hresult(factory->CreateFormatConverter(&converter), "PNG converter");
        check_hresult(converter->Initialize(frame.Get(), GUID_WICPixelFormat32bppRGBA, WICBitmapDitherTypeNone, nullptr,
                                            0, WICBitmapPaletteTypeCustom),
                      "PNG convert RGBA");
        image.rgba.resize(std::size_t(image.width) * image.height * png_bytes_per_pixel);
        check_hresult(converter->CopyPixels(nullptr, image.width * png_bytes_per_pixel, UINT(image.rgba.size()),
                                            image.rgba.data()),
                      "PNG copy pixels");
        return image;
    }

    inline void write_png(const std::filesystem::path& path, std::uint32_t width, std::uint32_t height,
                          std::span<const std::uint8_t> rgba) {
        if (rgba.size() != std::size_t(width) * height * png_bytes_per_pixel) {
            throw std::runtime_error("PNG buffer dimensions");
        }
        const auto factory = wic_factory();
        Microsoft::WRL::ComPtr<IWICStream> stream;
        check_hresult(factory->CreateStream(&stream), "WIC stream");
        check_hresult(stream->InitializeFromFilename(path.c_str(), GENERIC_WRITE), "PNG output");
        Microsoft::WRL::ComPtr<IWICBitmapEncoder> encoder;
        check_hresult(factory->CreateEncoder(GUID_ContainerFormatPng, nullptr, &encoder), "PNG encoder");
        check_hresult(encoder->Initialize(stream.Get(), WICBitmapEncoderNoCache), "PNG encoder init");
        Microsoft::WRL::ComPtr<IWICBitmapFrameEncode> frame;
        check_hresult(encoder->CreateNewFrame(&frame, nullptr), "PNG frame");
        check_hresult(frame->Initialize(nullptr), "PNG frame init");
        check_hresult(frame->SetSize(width, height), "PNG size");
        WICPixelFormatGUID format = GUID_WICPixelFormat32bppBGRA;
        check_hresult(frame->SetPixelFormat(&format), "PNG format");
        if (!IsEqualGUID(format, GUID_WICPixelFormat32bppBGRA)) {
            throw std::runtime_error("WIC changed BGRA layout");
        }
        std::vector<std::uint8_t> bgra(rgba.begin(), rgba.end());
        for (std::size_t i = 0; i < bgra.size(); i += png_bytes_per_pixel) {
            std::swap(bgra[i], bgra[i + 2]);
        }
        check_hresult(frame->WritePixels(height, width * png_bytes_per_pixel, UINT(bgra.size()), bgra.data()),
                      "PNG pixels");
        check_hresult(frame->Commit(), "PNG frame commit");
        check_hresult(encoder->Commit(), "PNG commit");
    }
} // namespace weft::testing
