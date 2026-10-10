#pragma once

#include <cstdint>
#include <stdexcept>
#include <string>
#include <string_view>

// Diagnostics shared by every module (docs/decisions.md, "错误作为值返回").
//
// Control flow branches on Diag codes, never on message text. The message starts with the code's
// stable "WEFT_*" name, which reports, logs and tests read.
namespace weft {
    enum class Diag : std::uint16_t {
        // Shared value grammar (base/css_scalar.hpp).
        InvalidNumber,
        // Profile and module domains whose errors reject a whole document or value.
        StyleProfile,
        PositionedProfile,
        AutoProfile,
        DocumentProfile,
        InlineProfile,
        SceneProfile,
        SceneRaster,
        // Canvas 2D model.
        CanvasColorLimit,
        CanvasColorProfile,
        CanvasSizeLimit,
        CanvasContextLimit,
        CanvasVersionLimit,
        CanvasBytesLimit,
        CanvasCommandLimit,
        CanvasGeometryLimit,
        CanvasPathProfile,
        CanvasFillRule,
        CanvasClearProfile,
        // DOM IndexSizeError raised by canvas geometry arguments.
        CanvasIndexSize,
        // Owner-thread violations (base/owner_thread.hpp).
        WrongThread,
        StyleMemoOwner,
        SceneOwner,
        SceneRasterOwner,
    };

    // Stable name of a code, e.g. "WEFT_STYLE_PROFILE" or "IndexSizeError".
    const char* diag_name(Diag code) noexcept;

    // "<name>: <detail>", or just the name when detail is empty.
    std::string diag_message(Diag code, std::string_view detail);

    struct Diagnostic {
        Diag code{};
        std::string message;
    };

    Diagnostic make_diagnostic(Diag code, std::string_view detail = {});

    // Transitional exception form of a Diagnostic for modules that still report by throwing.
    // Derives from std::invalid_argument, which those modules already throw and catch.
    class DiagnosticError : public std::invalid_argument {
    public:
        explicit DiagnosticError(Diagnostic diagnostic);

        Diag code() const noexcept {
            return code_;
        }

    private:
        Diag code_;
    };

    [[noreturn]] void throw_diagnostic(Diag code, std::string_view detail = {});
} // namespace weft
