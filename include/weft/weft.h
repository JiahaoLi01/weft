#ifndef CASEELEPHANT_CE_H
#define CASEELEPHANT_CE_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Experimental M0 ABI. Every runtime function requires its owner thread unless
 * explicitly stated. A runtime pointer is invalid after successful destroy. */
#define WEFT_ABI_VERSION 1u

typedef struct weft_runtime weft_runtime;

typedef enum weft_result {
    WEFT_OK = 0,
    WEFT_INVALID_ARGUMENT = 1,
    WEFT_UNSUPPORTED_ABI = 2,
    WEFT_OUT_OF_MEMORY = 3,
    WEFT_WRONG_THREAD = 4,
    WEFT_BUSY = 5,
    WEFT_STALE_HANDLE = 6,
    WEFT_CAPACITY_EXCEEDED = 7,
    WEFT_TIME_REGRESSION = 8,
    WEFT_INTERNAL_ERROR = 9
} weft_result;

typedef struct weft_runtime_desc {
    uint32_t struct_size;
    uint32_t abi_version;
    uint32_t max_views;
    uint32_t reserved;
} weft_runtime_desc;

typedef struct weft_view_handle {
    uint64_t runtime_id;
    uint32_t slot;
    uint32_t generation;
} weft_view_handle;

typedef struct weft_view_desc {
    uint32_t struct_size;
    uint32_t reserved;
    float logical_width;
    float logical_height;
    float device_scale;
    uint32_t reserved2;
} weft_view_desc;

typedef struct weft_runtime_info {
    uint32_t struct_size;
    uint32_t abi_version;
    uint64_t runtime_id;
    uint32_t live_views;
    uint32_t max_views;
} weft_runtime_info;

typedef struct weft_view_info {
    uint32_t struct_size;
    uint32_t reserved;
    float logical_width;
    float logical_height;
    float device_scale;
    uint32_t reserved2;
    uint64_t tick_count;
    double last_time_seconds;
} weft_view_info;

/* NULL desc selects defaults. Output handles are cleared on failure. */
weft_result weft_runtime_create(const weft_runtime_desc* desc, weft_runtime** out_runtime);
/* WEFT_BUSY leaves the runtime intact while live Views remain. */
weft_result weft_runtime_destroy(weft_runtime* runtime);
weft_result weft_runtime_get_info(weft_runtime* runtime, weft_runtime_info* out_info);
weft_result weft_view_create(weft_runtime* runtime, const weft_view_desc* desc, weft_view_handle* out_view);
weft_result weft_view_destroy(weft_runtime* runtime, weft_view_handle view);
weft_result weft_view_get_info(weft_runtime* runtime, weft_view_handle view, weft_view_info* out_info);
/* M0 only validates/stores host monotonic time. This does not render or run JS. */
weft_result weft_view_record_tick(weft_runtime* runtime, weft_view_handle view, double time_seconds);
/* Stateless; callable from any thread. Borrowed static text, do not free. */
const char* weft_result_string(weft_result result);

#ifdef __cplusplus
}
#endif

#endif
