#pragma once

#include "base/diagnostic.hpp"

#include <type_traits>
#include <utility>
#include <variant>

// expected-style result: a value, or the Diagnostic that explains why there is none.
// C++20 has no std::expected; this keeps only what the runtime needs.
namespace weft {
    template <class T>
    class [[nodiscard]] Result {
        static_assert(!std::is_same_v<std::remove_cv_t<T>, Diagnostic>, "Result<Diagnostic> is ambiguous");

    public:
        Result(T value) : state_(std::in_place_index<0>, std::move(value)) {
        }

        Result(Diagnostic error) : state_(std::in_place_index<1>, std::move(error)) {
        }

        bool has_value() const noexcept {
            return state_.index() == 0;
        }

        explicit operator bool() const noexcept {
            return has_value();
        }

        // Precondition: has_value().
        T& operator*() & noexcept {
            return *std::get_if<0>(&state_);
        }

        const T& operator*() const& noexcept {
            return *std::get_if<0>(&state_);
        }

        T* operator->() noexcept {
            return std::get_if<0>(&state_);
        }

        const T* operator->() const noexcept {
            return std::get_if<0>(&state_);
        }

        // Bridge to exception-reporting callers: throws DiagnosticError when there is no value.
        T& value() & {
            require();
            return **this;
        }

        const T& value() const& {
            require();
            return **this;
        }

        T&& value() && {
            require();
            return std::move(**this);
        }

        template <class U>
        T value_or(U&& fallback) const& {
            return has_value() ? **this : static_cast<T>(std::forward<U>(fallback));
        }

        // Precondition: !has_value().
        const Diagnostic& error() const noexcept {
            return *std::get_if<1>(&state_);
        }

    private:
        void require() const {
            if (!has_value()) {
                throw DiagnosticError(error());
            }
        }

        std::variant<T, Diagnostic> state_;
    };

    template <>
    class [[nodiscard]] Result<void> {
    public:
        Result() = default;

        Result(Diagnostic error) : error_(std::move(error)), failed_(true) {
        }

        bool has_value() const noexcept {
            return !failed_;
        }

        explicit operator bool() const noexcept {
            return has_value();
        }

        void value() const {
            if (failed_) {
                throw DiagnosticError(error_);
            }
        }

        const Diagnostic& error() const noexcept {
            return error_;
        }

    private:
        Diagnostic error_;
        bool failed_ = false;
    };

    using Status = Result<void>;
} // namespace weft
