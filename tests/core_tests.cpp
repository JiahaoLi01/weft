#include <weft/weft.h>
#include "support/check.hpp"
#include <iostream>
#include <limits>
#include <stdexcept>
#include <thread>

namespace {
    using weft::testing::require;

} // namespace

int main() {
    try {
        weft_runtime* runtime = reinterpret_cast<weft_runtime*>(1);
        weft_runtime_desc bad{sizeof(weft_runtime_desc), 99, 1, 0};
        require(weft_runtime_create(&bad, &runtime) == WEFT_UNSUPPORTED_ABI && runtime == nullptr,
                "ABI rejected atomically");
        weft_runtime_desc config{sizeof(weft_runtime_desc), WEFT_ABI_VERSION, 1, 0};
        require(weft_runtime_create(&config, &runtime) == WEFT_OK, "runtime creation");
        weft_view_handle first{};
        require(weft_view_create(runtime, nullptr, &first) == WEFT_OK, "view creation");
        weft_view_handle excess{999, 999, 999};
        require(weft_view_create(runtime, nullptr, &excess) == WEFT_CAPACITY_EXCEEDED && excess.runtime_id == 0,
                "bounded view slots");
        require(weft_runtime_destroy(runtime) == WEFT_BUSY, "busy destroy preserves runtime");
        weft_result wrong_thread = WEFT_OK;
        std::thread outsider([&] {
            wrong_thread = weft_view_record_tick(runtime, first, 1);
        });
        outsider.join();
        require(wrong_thread == WEFT_WRONG_THREAD, "owner thread enforced");
        require(weft_view_record_tick(runtime, first, 3) == WEFT_OK, "clock accepted");
        require(weft_view_record_tick(runtime, first, 2) == WEFT_TIME_REGRESSION, "clock regression rejected");
        require(weft_view_record_tick(runtime, first, std::numeric_limits<double>::quiet_NaN()) ==
                    WEFT_INVALID_ARGUMENT,
                "NaN rejected");
        weft_view_info info{};
        info.struct_size = sizeof(info);
        require(weft_view_get_info(runtime, first, &info) == WEFT_OK && info.tick_count == 1 &&
                    info.last_time_seconds == 3,
                "failed ticks leave state unchanged");
        weft_runtime* other{};
        require(weft_runtime_create(nullptr, &other) == WEFT_OK, "second runtime");
        require(weft_view_get_info(other, first, &info) == WEFT_STALE_HANDLE, "cross-runtime handle rejected");
        require(weft_runtime_destroy(other) == WEFT_OK, "second runtime destroy");
        require(weft_view_destroy(runtime, first) == WEFT_OK, "view destruction");
        weft_view_handle replacement{};
        require(weft_view_create(runtime, nullptr, &replacement) == WEFT_OK, "slot reused");
        require(replacement.slot == first.slot && replacement.generation != first.generation, "generation changed");
        require(weft_view_record_tick(runtime, first, 4) == WEFT_STALE_HANDLE, "stale slot cannot mutate replacement");
        require(weft_view_destroy(runtime, first) == WEFT_STALE_HANDLE, "stale destroy cannot delete replacement");
        require(weft_view_destroy(runtime, replacement) == WEFT_OK, "replacement destroy");
        weft_view_desc invalid{sizeof(weft_view_desc), 0, -1, 100, 1, 0};
        require(weft_view_create(runtime, &invalid, &replacement) == WEFT_INVALID_ARGUMENT, "invalid surface rejected");
        require(weft_runtime_destroy(runtime) == WEFT_OK, "final runtime destroy");
        std::cout << "core_contract: ABI, owner, generations, capacity and clock passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
