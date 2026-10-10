#pragma once

#include <stdexcept>
#include <string>
#include <string_view>

// Assertions shared by the behavior tests and the probes. A failed check throws; each executable's
// main reports what() and returns non-zero.
namespace caseelephant::testing {
    inline void require(bool condition, std::string_view message) {
        if (!condition) {
            throw std::runtime_error(std::string(message));
        }
    }

    inline void require_expression(bool condition, const char* expression, const char* file, int line) {
        if (!condition) {
            throw std::runtime_error(std::string(expression) + " (" + file + ":" + std::to_string(line) + ")");
        }
    }
} // namespace caseelephant::testing

// Checks an expression and reports its source text and location on failure.
#define CE_REQUIRE(expression)                                                                                         \
    ::caseelephant::testing::require_expression((expression), #expression, __FILE__, __LINE__)
