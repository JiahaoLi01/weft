#include "caseelephant/base/diagnostic.hpp"

namespace caseelephant {
    const char* diag_name(Diag code) noexcept {
        switch (code) {
            case Diag::InvalidNumber:
                return "CE_CSS_VALUE";
            case Diag::StyleProfile:
                return "CE_STYLE_PROFILE";
            case Diag::PositionedProfile:
                return "CE_POSITIONED_PROFILE";
            case Diag::AutoProfile:
                return "CE_AUTO_PROFILE";
            case Diag::FlowProfile:
                return "CE_FLOW_PROFILE";
            case Diag::InlineProfile:
                return "CE_INLINE_PROFILE";
            case Diag::SceneProfile:
                return "CE_SCENE_PROFILE";
            case Diag::SceneRaster:
                return "CE_SCENE_RASTER";
            case Diag::CanvasColorLimit:
                return "CE_CANVAS_COLOR_LIMIT";
            case Diag::CanvasColorProfile:
                return "CE_CANVAS_COLOR_PROFILE";
            case Diag::CanvasSizeLimit:
                return "CE_CANVAS_SIZE_LIMIT";
            case Diag::CanvasContextLimit:
                return "CE_CANVAS_CONTEXT_LIMIT";
            case Diag::CanvasVersionLimit:
                return "CE_CANVAS_VERSION_LIMIT";
            case Diag::CanvasBytesLimit:
                return "CE_CANVAS_BYTES_LIMIT";
            case Diag::CanvasCommandLimit:
                return "CE_CANVAS_COMMAND_LIMIT";
            case Diag::CanvasGeometryLimit:
                return "CE_CANVAS_GEOMETRY_LIMIT";
            case Diag::CanvasPathProfile:
                return "CE_CANVAS_PATH_PROFILE";
            case Diag::CanvasFillRule:
                return "CE_CANVAS_FILL_RULE";
            case Diag::CanvasClearProfile:
                return "CE_CANVAS_CLEAR_PROFILE";
            case Diag::CanvasIndexSize:
                return "IndexSizeError";
            case Diag::WrongThread:
                return "CE_WRONG_THREAD";
            case Diag::StyleMemoOwner:
                return "CE_STYLE_MEMO_OWNER";
            case Diag::SceneOwner:
                return "CE_SCENE_OWNER";
            case Diag::SceneRasterOwner:
                return "CE_SCENE_RASTER_OWNER";
        }
        return "CE_UNKNOWN_DIAGNOSTIC";
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
} // namespace caseelephant
