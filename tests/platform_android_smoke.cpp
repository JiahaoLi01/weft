#include "platform/android/system_fonts.hpp"
#include "render/backends/vulkan/swapchain.hpp"

#include <exception>
#include <iostream>

// Android only: links the platform layer and the Vulkan surface of Android native windows. On a device it lists the
// system fonts; the surface needs a window, so it is only referenced here. This repository's Windows machine
// cross-compiles and links it but cannot run it (docs/status.md).
int main() {
    try {
        const auto catalog = weft::platform::android::system_font_catalog();
        auto* const surface = &weft::render::vulkan::android_surface;
        std::cout << "system fonts: " << catalog.faces.size() << " faces, " << catalog.fallback_families.size()
                  << " fallback families\n";
        return catalog.faces.empty() || !surface ? 1 : 0;
    } catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
}
