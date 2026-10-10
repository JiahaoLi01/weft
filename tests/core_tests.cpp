#include <caseelephant/ce.h>
#include "support/check.hpp"
#include <iostream>
#include <limits>
#include <stdexcept>
#include <thread>

namespace {
    using caseelephant::testing::require;

} // namespace

int main() {
    try {
        ce_runtime* runtime = reinterpret_cast<ce_runtime*>(1);
        ce_runtime_desc bad{sizeof(ce_runtime_desc), 99, 1, 0};
        require(ce_runtime_create(&bad, &runtime) == CE_UNSUPPORTED_ABI && runtime == nullptr,
                "ABI rejected atomically");
        ce_runtime_desc config{sizeof(ce_runtime_desc), CE_ABI_VERSION, 1, 0};
        require(ce_runtime_create(&config, &runtime) == CE_OK, "runtime creation");
        ce_view_handle first{};
        require(ce_view_create(runtime, nullptr, &first) == CE_OK, "view creation");
        ce_view_handle excess{999, 999, 999};
        require(ce_view_create(runtime, nullptr, &excess) == CE_CAPACITY_EXCEEDED && excess.runtime_id == 0,
                "bounded view slots");
        require(ce_runtime_destroy(runtime) == CE_BUSY, "busy destroy preserves runtime");
        ce_result wrong_thread = CE_OK;
        std::thread outsider([&] {
            wrong_thread = ce_view_record_tick(runtime, first, 1);
        });
        outsider.join();
        require(wrong_thread == CE_WRONG_THREAD, "owner thread enforced");
        require(ce_view_record_tick(runtime, first, 3) == CE_OK, "clock accepted");
        require(ce_view_record_tick(runtime, first, 2) == CE_TIME_REGRESSION, "clock regression rejected");
        require(ce_view_record_tick(runtime, first, std::numeric_limits<double>::quiet_NaN()) == CE_INVALID_ARGUMENT,
                "NaN rejected");
        ce_view_info info{};
        info.struct_size = sizeof(info);
        require(ce_view_get_info(runtime, first, &info) == CE_OK && info.tick_count == 1 && info.last_time_seconds == 3,
                "failed ticks leave state unchanged");
        ce_runtime* other{};
        require(ce_runtime_create(nullptr, &other) == CE_OK, "second runtime");
        require(ce_view_get_info(other, first, &info) == CE_STALE_HANDLE, "cross-runtime handle rejected");
        require(ce_runtime_destroy(other) == CE_OK, "second runtime destroy");
        require(ce_view_destroy(runtime, first) == CE_OK, "view destruction");
        ce_view_handle replacement{};
        require(ce_view_create(runtime, nullptr, &replacement) == CE_OK, "slot reused");
        require(replacement.slot == first.slot && replacement.generation != first.generation, "generation changed");
        require(ce_view_record_tick(runtime, first, 4) == CE_STALE_HANDLE, "stale slot cannot mutate replacement");
        require(ce_view_destroy(runtime, first) == CE_STALE_HANDLE, "stale destroy cannot delete replacement");
        require(ce_view_destroy(runtime, replacement) == CE_OK, "replacement destroy");
        ce_view_desc invalid{sizeof(ce_view_desc), 0, -1, 100, 1, 0};
        require(ce_view_create(runtime, &invalid, &replacement) == CE_INVALID_ARGUMENT, "invalid surface rejected");
        require(ce_runtime_destroy(runtime) == CE_OK, "final runtime destroy");
        std::cout << "core_contract: ABI, owner, generations, capacity and clock passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
