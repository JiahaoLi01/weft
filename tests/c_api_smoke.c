#include <caseelephant/ce.h>
#include <stddef.h>

_Static_assert(sizeof(ce_view_handle) == 16, "portable view handle layout");
_Static_assert(offsetof(ce_view_handle, generation) == 12, "portable generation offset");

int main(void) {
    ce_runtime* runtime = NULL;
    ce_view_handle view = {0};
    ce_runtime_info info = {0};
    info.struct_size = sizeof(info);
    if (ce_runtime_create(NULL, &runtime) != CE_OK) return 1;
    if (ce_view_create(runtime, NULL, &view) != CE_OK) return 2;
    if (ce_runtime_get_info(runtime, &info) != CE_OK || info.live_views != 1) return 3;
    if (ce_view_destroy(runtime, view) != CE_OK) return 4;
    return ce_runtime_destroy(runtime) == CE_OK ? 0 : 5;
}
