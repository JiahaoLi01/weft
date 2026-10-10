#pragma once

#include "caseelephant/base/diagnostic.hpp"

#include <exception>
#include <stdexcept>
#include <string_view>
#include <thread>

namespace caseelephant {
    // The thread that owns a single-threaded object (DOM, script VM, style memo, layout, text, raster).
    // Constructed on, and bound to, the creating thread.
    class OwnerThread {
    public:
        OwnerThread() noexcept : id_(std::this_thread::get_id()) {
        }

        bool is_current() const noexcept {
            return id_ == std::this_thread::get_id();
        }

        // Throws std::logic_error carrying the diagnostic message when called off the owner thread.
        void require(Diag code, std::string_view detail = {}) const {
            if (!is_current()) {
                throw std::logic_error(diag_message(code, detail));
            }
        }

        // For destructors, which cannot report: terminates when called off the owner thread.
        void require_or_terminate() const noexcept {
            if (!is_current()) {
                std::terminate();
            }
        }

    private:
        std::thread::id id_;
    };
} // namespace caseelephant
