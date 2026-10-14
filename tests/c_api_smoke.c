#include <weft/weft.h>
#include <stddef.h>
#include <string.h>

_Static_assert(sizeof(weft_view_handle) == 16, "portable view handle layout");
_Static_assert(offsetof(weft_view_handle, generation) == 12, "portable generation offset");
_Static_assert(sizeof(weft_anchor) == 32, "anchors are 32 bytes");
_Static_assert(offsetof(weft_anchor, flags) == 24, "portable anchor layout");
_Static_assert(sizeof(weft_hit) == 40, "portable hit layout");

static int messages;

static void on_message(void* user, weft_view_handle view, const weft_message* message) {
    (void)user;
    (void)view;
    if (message->kind == WEFT_MESSAGE_SEND && message->name_size == 5 && memcmp(message->name, "ready", 5) == 0) {
        ++messages;
    }
}

static void on_diagnostic(void* user, weft_view_handle view, const weft_diagnostic* diagnostic) {
    (void)user;
    (void)view;
    (void)diagnostic;
}

static weft_result answer(void* user, const void* argument, size_t size, weft_value_writer* result) {
    (void)user;
    (void)argument;
    (void)size;
    return weft_value_writer_set(result, WEFT_VALUE_JSON, "7", 1);
}

int main(void) {
    weft_runtime* runtime = NULL;
    weft_view_handle view = {0};
    weft_runtime_info info = {0};
    weft_runtime_desc desc = {sizeof(weft_runtime_desc), WEFT_ABI_VERSION, 4, 0};
    static const char source[] = "weft.engine.send('ready', weft.engine.invoke('answer'))";
    weft_script script = {"smoke.js", 8, source, sizeof source - 1};
    weft_page_desc page = {sizeof(weft_page_desc), 0, &script, 1, 0, NULL, 0, NULL, 0, NULL, 0};
    weft_pointer_event move = {sizeof(weft_pointer_event), WEFT_POINTER_MOVE, 1, 1, 0, 0, 0, 0};
    weft_hit hit = {sizeof(weft_hit), 0, {0}};
    weft_anchor anchor = {1, 2, 3, 1, 0, 0, 0};
    weft_view_frame* frame = NULL;
    weft_view_frame_info frame_info = {sizeof(weft_view_frame_info), 0, 0, 0, 0, 0, 0, 0};
    uint32_t prevented = 0;
    uint32_t dropped = 0;
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
    if (weft_runtime_destroy(runtime) != WEFT_OK) {
        return 5;
    }
    /* A runtime without system fonts, a page through every entry point. */
    if (weft_runtime_create(&desc, &runtime) != WEFT_OK || weft_view_create(runtime, NULL, &view) != WEFT_OK) {
        return 6;
    }
    if (weft_view_register_function(runtime, view, "answer", 6, answer, NULL) != WEFT_OK ||
        weft_view_load(runtime, view, &page) != WEFT_OK || weft_view_advance(runtime, view, 0.016) != WEFT_OK) {
        return 7;
    }
    if (weft_view_drain_messages(runtime, view, on_message, NULL) != WEFT_OK || messages != 1) {
        return 8;
    }
    if (weft_view_pointer(runtime, view, &move, &prevented) != WEFT_OK ||
        weft_view_hit_test(runtime, view, 1, 1, &hit) != WEFT_OK || hit.element != 0) {
        return 9;
    }
    if (weft_view_write_anchors(runtime, view, &anchor, 1) != WEFT_OK ||
        weft_view_emit(runtime, view, "e", 1, WEFT_VALUE_JSON, "1", 1, WEFT_EMIT_LATEST) != WEFT_OK ||
        weft_view_set_model(runtime, view, "m", 1, WEFT_VALUE_JSON, "{}", 2) != WEFT_OK ||
        weft_view_complete_call(runtime, view, 1, 1, WEFT_VALUE_JSON, "1", 1) != WEFT_STALE_HANDLE) {
        return 10;
    }
    if (weft_view_acquire_frame(runtime, view, &frame) != WEFT_OK ||
        weft_view_frame_get_info(frame, &frame_info) != WEFT_OK || frame_info.logical_width != 640) {
        return 11;
    }
    weft_view_frame_release(frame);
    if (weft_view_resize(runtime, view, 320, 240, 2) != WEFT_OK ||
        weft_view_drain_diagnostics(runtime, view, on_diagnostic, NULL, &dropped) != WEFT_OK) {
        return 12;
    }
    if (weft_view_destroy(runtime, view) != WEFT_OK) {
        return 13;
    }
    return weft_runtime_destroy(runtime) == WEFT_OK ? 0 : 14;
}
