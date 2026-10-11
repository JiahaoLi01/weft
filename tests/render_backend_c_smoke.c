#include "weft/render_backend.h"
#include "weft/render_backend_d3d11.h"
#include "weft/render_backend_vulkan.h"

int main(void) {
    weft_render_backend backend = {0};
    weft_renderer* renderer = (weft_renderer*)1;
    backend.struct_size = sizeof(backend);
    backend.abi_version = WEFT_RENDER_ABI_VERSION + 1;
    if (weft_renderer_create(&backend, 0, &renderer) != WEFT_RENDER_UNSUPPORTED_ABI || renderer != 0) {
        return 1;
    }
    weft_render_external_backend extension = {0};
    weft_render_external_texture texture = {0};
    weft_render_external_frame frame = {0};
    weft_render_external_info info = {0};
    weft_render_handle handle = {0};
    weft_render_d3d11_texture native = {0};
    weft_render_vulkan_pass vulkan = {0};
    /* Fixed layout on every platform: 64-bit handles, then the clear fields. */
    vulkan.struct_size = sizeof(vulkan);
    if (vulkan.struct_size != 56 || sizeof(vulkan.command_buffer) != 8) {
        return 4;
    }
    texture.native_texture = &native;
    if (weft_renderer_install_external_backend(0, &extension) != WEFT_RENDER_INVALID_ARGUMENT ||
        weft_renderer_import_texture(0, &texture, &handle) != WEFT_RENDER_INVALID_ARGUMENT ||
        weft_renderer_prepare_external(0, &frame, &handle) != WEFT_RENDER_INVALID_ARGUMENT ||
        weft_renderer_release_texture(0, handle) != WEFT_RENDER_INVALID_ARGUMENT ||
        weft_renderer_get_external_info(0, &info) != WEFT_RENDER_INVALID_ARGUMENT) {
        return 3;
    }
    return weft_renderer_destroy(0) == WEFT_RENDER_INVALID_ARGUMENT ? 0 : 2;
}
