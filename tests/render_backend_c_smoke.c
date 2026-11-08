#include "weft/render_backend.h"
#include "weft/render_backend_d3d11.h"
#include "weft/render_export_texture.h"
#include "weft/render_backend_metal.h"
#include "weft/render_backend_vulkan.h"

#include <stddef.h>

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
    weft_render_metal_pass metal = {0};
    weft_render_scene_frame scene = {0};
    /* Fixed layout on every platform: 64-bit handles, the clear fields, then the target's image. */
    vulkan.struct_size = sizeof(vulkan);
    if (vulkan.struct_size != 64 || offsetof(weft_render_vulkan_pass, target_image) != 56 ||
        sizeof(vulkan.command_buffer) != 8) {
        return 4;
    }
    /* Metal's objects are pointers of 64-bit Apple platforms, the clear fields after them. */
    metal.struct_size = sizeof(metal);
    if (sizeof(void*) == 8 && (metal.struct_size != 48 || offsetof(weft_render_metal_pass, target) != 16 ||
                               offsetof(weft_render_metal_pass, clear_linear_premultiplied) != 32 ||
                               sizeof(weft_render_metal_texture) != 24 || sizeof(weft_render_metal_desc) != 32)) {
        return 6;
    }
    scene.struct_size = sizeof(scene);
    scene.abi_version = WEFT_RENDER_ABI_VERSION;
    if (weft_renderer_prepare_scene(0, &scene, &handle) != WEFT_RENDER_INVALID_ARGUMENT ||
        weft_renderer_prepare_scene(0, &scene, 0) != WEFT_RENDER_INVALID_ARGUMENT) {
        return 5;
    }
    texture.native_texture = &native;
    if (weft_renderer_install_external_backend(0, &extension) != WEFT_RENDER_INVALID_ARGUMENT ||
        weft_renderer_import_texture(0, &texture, &handle) != WEFT_RENDER_INVALID_ARGUMENT ||
        weft_renderer_prepare_external(0, &frame, &handle) != WEFT_RENDER_INVALID_ARGUMENT ||
        weft_renderer_release_texture(0, handle) != WEFT_RENDER_INVALID_ARGUMENT ||
        weft_renderer_get_external_info(0, &info) != WEFT_RENDER_INVALID_ARGUMENT) {
        return 3;
    }
    {
        /* The export extension: fixed 64-bit layouts, and a null renderer refused by every call. */
        weft_render_export_backend exporter = {0};
        weft_render_export_desc desc = {0};
        weft_render_export_image image = {0};
        weft_render_export_info export_info = {0};
        weft_render_pass pass = {0};
        image.struct_size = sizeof(image);
        if (sizeof(desc) != 64 || sizeof(image) != 72 || offsetof(weft_render_export_image, native_texture) != 64 ||
            sizeof(export_info) != 40 || (sizeof(void*) == 8 && sizeof(exporter) != 56)) {
            return 7;
        }
        if (weft_renderer_install_export_backend(0, &exporter) != WEFT_RENDER_INVALID_ARGUMENT ||
            weft_renderer_create_export(0, &desc, &handle) != WEFT_RENDER_INVALID_ARGUMENT ||
            weft_renderer_destroy_export(0, handle) != WEFT_RENDER_INVALID_ARGUMENT ||
            weft_renderer_encode_export(0, handle, handle, &pass, &handle, &image) != WEFT_RENDER_INVALID_ARGUMENT ||
            weft_renderer_release_export_image(0, handle, 0, 0) != WEFT_RENDER_INVALID_ARGUMENT ||
            weft_renderer_get_export_info(0, &export_info) != WEFT_RENDER_INVALID_ARGUMENT) {
            return 8;
        }
    }
    return weft_renderer_destroy(0) == WEFT_RENDER_INVALID_ARGUMENT ? 0 : 2;
}
