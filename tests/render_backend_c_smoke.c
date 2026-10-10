#include "caseelephant/render_backend.h"
#include "caseelephant/render_backend_d3d11.h"
int main(void) {
    ce_render_backend backend = {0};
    ce_renderer* renderer = (ce_renderer*)1;
    backend.struct_size = sizeof(backend);
    backend.abi_version = CE_RENDER_ABI_VERSION + 1;
    if (ce_renderer_create(&backend, 0, &renderer) != CE_RENDER_UNSUPPORTED_ABI || renderer != 0) return 1;
    ce_render_external_backend extension = {0};
    ce_render_external_texture texture = {0};
    ce_render_external_frame frame = {0};
    ce_render_external_info info = {0};
    ce_render_handle handle = {0};
    ce_render_d3d11_texture native = {0};
    texture.native_texture = &native;
    if (ce_renderer_install_external_backend(0, &extension) != CE_RENDER_INVALID_ARGUMENT ||
        ce_renderer_import_texture(0, &texture, &handle) != CE_RENDER_INVALID_ARGUMENT ||
        ce_renderer_prepare_external(0, &frame, &handle) != CE_RENDER_INVALID_ARGUMENT ||
        ce_renderer_release_texture(0, handle) != CE_RENDER_INVALID_ARGUMENT ||
        ce_renderer_get_external_info(0, &info) != CE_RENDER_INVALID_ARGUMENT) return 3;
    return ce_renderer_destroy(0) == CE_RENDER_INVALID_ARGUMENT ? 0 : 2;
}
