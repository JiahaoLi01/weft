#include "base/diagnostic.hpp"

namespace weft {
    const char* diag_name(Diag code) noexcept {
        switch (code) {
            case Diag::InvalidNumber:
                return "WEFT_CSS_VALUE";
            case Diag::StyleProfile:
                return "WEFT_STYLE_PROFILE";
            case Diag::DocumentProfile:
                return "WEFT_DOCUMENT_PROFILE";
            case Diag::InlineProfile:
                return "WEFT_INLINE_PROFILE";
            case Diag::SceneProfile:
                return "WEFT_SCENE_PROFILE";
            case Diag::SceneRaster:
                return "WEFT_SCENE_RASTER";
            case Diag::CanvasColorLimit:
                return "WEFT_CANVAS_COLOR_LIMIT";
            case Diag::CanvasColorProfile:
                return "WEFT_CANVAS_COLOR_PROFILE";
            case Diag::CanvasSizeLimit:
                return "WEFT_CANVAS_SIZE_LIMIT";
            case Diag::CanvasContextLimit:
                return "WEFT_CANVAS_CONTEXT_LIMIT";
            case Diag::CanvasVersionLimit:
                return "WEFT_CANVAS_VERSION_LIMIT";
            case Diag::CanvasBytesLimit:
                return "WEFT_CANVAS_BYTES_LIMIT";
            case Diag::CanvasCommandLimit:
                return "WEFT_CANVAS_COMMAND_LIMIT";
            case Diag::CanvasGeometryLimit:
                return "WEFT_CANVAS_GEOMETRY_LIMIT";
            case Diag::CanvasPathProfile:
                return "WEFT_CANVAS_PATH_PROFILE";
            case Diag::CanvasFillRule:
                return "WEFT_CANVAS_FILL_RULE";
            case Diag::CanvasClearProfile:
                return "WEFT_CANVAS_CLEAR_PROFILE";
            case Diag::CanvasIndexSize:
                return "IndexSizeError";
            case Diag::WrongThread:
                return "WEFT_WRONG_THREAD";
            case Diag::StyleMemoOwner:
                return "WEFT_STYLE_MEMO_OWNER";
            case Diag::SceneOwner:
                return "WEFT_SCENE_OWNER";
            case Diag::SceneRasterOwner:
                return "WEFT_SCENE_RASTER_OWNER";
        }
        return "WEFT_UNKNOWN_DIAGNOSTIC";
    }

    std::string diag_message(Diag code, std::string_view detail) {
        std::string message = diag_name(code);
        if (!detail.empty()) {
            message += ": ";
            message += detail;
        }
        return message;
    }

    Diagnostic make_diagnostic(Diag code, std::string_view detail) {
        return {code, diag_message(code, detail)};
    }

    DiagnosticError::DiagnosticError(Diagnostic diagnostic)
        : std::invalid_argument(diagnostic.message), code_(diagnostic.code) {
    }

    void throw_diagnostic(Diag code, std::string_view detail) {
        throw DiagnosticError(make_diagnostic(code, detail));
    }
} // namespace weft
