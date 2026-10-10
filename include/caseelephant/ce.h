#ifndef CASEELEPHANT_CE_H
#define CASEELEPHANT_CE_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Experimental M0 ABI. Every runtime function requires its owner thread unless
 * explicitly stated. A runtime pointer is invalid after successful destroy. */
#define CE_ABI_VERSION 1u

typedef struct ce_runtime ce_runtime;

typedef enum ce_result {
    CE_OK = 0,
    CE_INVALID_ARGUMENT = 1,
    CE_UNSUPPORTED_ABI = 2,
    CE_OUT_OF_MEMORY = 3,
    CE_WRONG_THREAD = 4,
    CE_BUSY = 5,
    CE_STALE_HANDLE = 6,
    CE_CAPACITY_EXCEEDED = 7,
    CE_TIME_REGRESSION = 8,
    CE_INTERNAL_ERROR = 9
} ce_result;

typedef struct ce_runtime_desc {
    uint32_t struct_size;
    uint32_t abi_version;
    uint32_t max_views;
    uint32_t reserved;
} ce_runtime_desc;

typedef struct ce_view_handle {
    uint64_t runtime_id;
    uint32_t slot;
    uint32_t generation;
} ce_view_handle;

typedef struct ce_view_desc {
    uint32_t struct_size;
    uint32_t reserved;
    float logical_width;
    float logical_height;
    float device_scale;
    uint32_t reserved2;
} ce_view_desc;

typedef struct ce_runtime_info {
    uint32_t struct_size;
    uint32_t abi_version;
    uint64_t runtime_id;
    uint32_t live_views;
    uint32_t max_views;
} ce_runtime_info;

typedef struct ce_view_info {
    uint32_t struct_size;
    uint32_t reserved;
    float logical_width;
    float logical_height;
    float device_scale;
    uint32_t reserved2;
    uint64_t tick_count;
    double last_time_seconds;
} ce_view_info;

/* NULL desc selects defaults. Output handles are cleared on failure. */
ce_result ce_runtime_create(const ce_runtime_desc* desc, ce_runtime** out_runtime);
/* CE_BUSY leaves the runtime intact while live Views remain. */
ce_result ce_runtime_destroy(ce_runtime* runtime);
ce_result ce_runtime_get_info(ce_runtime* runtime, ce_runtime_info* out_info);
ce_result ce_view_create(ce_runtime* runtime, const ce_view_desc* desc, ce_view_handle* out_view);
ce_result ce_view_destroy(ce_runtime* runtime, ce_view_handle view);
ce_result ce_view_get_info(ce_runtime* runtime, ce_view_handle view, ce_view_info* out_info);
/* M0 only validates/stores host monotonic time. This does not render or run JS. */
ce_result ce_view_record_tick(ce_runtime* runtime, ce_view_handle view, double time_seconds);
/* Stateless; callable from any thread. Borrowed static text, do not free. */
const char* ce_result_string(ce_result result);

#ifdef __cplusplus
}
#endif

#endif
