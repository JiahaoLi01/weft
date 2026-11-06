#pragma once

// Helpers the Metal scene renderer's translation units share (Objective-C++, ARC).
#import <Metal/Metal.h>

#include "constants/render.hpp"

#include <cstdint>
#include <string>

namespace weft::render::metal::detail {
    namespace limits = weft::constants::render::metal;

    // Encoded premultiplied BGRA, as the D3D11 and Vulkan scene targets.
    inline constexpr MTLPixelFormat target_format = MTLPixelFormatBGRA8Unorm;

    inline std::string describe(NSError* error) {
        return error ? std::string(error.localizedDescription.UTF8String) : std::string("unknown error");
    }

    // The host texture a scene texture's view names: an id<MTLTexture> the backend retains while it is imported.
    inline id<MTLTexture> host_texture(std::uint64_t view) {
        return (__bridge id<MTLTexture>)reinterpret_cast<void*>(static_cast<std::uintptr_t>(view));
    }

    // A private 2D texture of one mip level.
    id<MTLTexture> make_texture(id<MTLDevice> device, MTLPixelFormat format, std::uint32_t width,
                                std::uint32_t height, MTLTextureUsage usage);
} // namespace weft::render::metal::detail
