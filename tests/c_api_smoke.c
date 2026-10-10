#include <caseelephant/ce.h>
#include <stddef.h>

_Static_assert(sizeof(weft_view_handle) == 16, "portable view handle layout");
_Static_assert(offsetof(weft_view_handle, generation) == 12, "portable generation offset");

int main(void) {
    weft_runtime* runtime = NULL;
    weft_view_handle view = {0};
    weft_runtime_info info = {0};
    info.struct_size = sizeof(info);
    if (weft_runtime_create(NULL, &runtime) != WEFT_OK) {
        return 1;
    }
    if (weft_view_create(runtime, NULL, &view) != WEFT_OK) {
        return 2;
    }
    if (weft_runtime_get_info(runtime, &info) != WEFT_OK || info.live_views != 1) {
        return 3;
    }
    if (weft_view_destroy(runtime, view) != WEFT_OK) {
        return 4;
    }
    return weft_runtime_destroy(runtime) == WEFT_OK ? 0 : 5;
}
