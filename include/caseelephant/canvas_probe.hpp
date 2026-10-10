#pragma once
#include "caseelephant/constants/canvas.hpp"
#include <array>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>
#include <stdexcept>

namespace weft {
    // Owner-only raster service. No DOM, JS, device or window types in this contract.
    struct CanvasCommand {
        enum class Kind {
            FillRect,
            FillCircle,
            ClearRect
        } kind;
        std::array<double, 4> geometry; // rect x/y/w/h; circle x/y/r/unused
        std::array<float, 4> srgb;      // straight, source-over in Canvas's sRGB bitmap
    };

    class CanvasRaster {
    public:
        virtual ~CanvasRaster() = default;
        virtual void draw(std::span<const CanvasCommand>) = 0;
        virtual std::vector<std::uint8_t> rgba_straight() = 0;
    };

    using CanvasFactory = std::function<std::unique_ptr<CanvasRaster>(std::uint32_t, std::uint32_t, bool)>;

    struct CanvasBudget {
        std::size_t bytes{};
        std::size_t contexts{};
        std::size_t peak_bytes{};
    };

    struct CanvasBackendError : std::runtime_error {
        using std::runtime_error::runtime_error;
    };

    // Invalid CSS colors return nullopt; valid syntax outside the declared profile
    // throws a diagnostic. Supports all named colors, hex and comma RGB/HSL forms.
    std::optional<std::array<float, 4>> parse_canvas_color(std::string_view);

    class CanvasState final {
        CanvasFactory factory_;
        std::unique_ptr<CanvasRaster> raster_;
        std::shared_ptr<CanvasBudget> budget_;
        std::vector<CanvasCommand> pending_;
        std::shared_ptr<const std::vector<std::uint8_t>> cached_;
        std::optional<std::array<double, 4>> circle_;
        std::size_t bytes_{};
        void enqueue(CanvasCommand);

    public:
        std::uint32_t width{};
        std::uint32_t height{};
        bool alpha{};
        std::uint64_t version{1};
        std::uint64_t commands{};
        std::uint64_t fills{};
        std::array<float, 4> color{0, 0, 0, 1};
        std::string fill_style{constants::canvas::default_fill_style};
        double global_alpha{1};
        CanvasState(CanvasFactory, std::shared_ptr<CanvasBudget>, std::uint32_t, std::uint32_t, bool);
        ~CanvasState();
        CanvasState(const CanvasState&) = delete;
        void resize(std::uint32_t, std::uint32_t);

        void begin_path() {
            circle_.reset();
        }

        void arc(double, double, double, double, double, bool);
        void fill(std::string_view rule);
        void fill_rect(double, double, double, double, bool clear = false);
        std::shared_ptr<const std::vector<std::uint8_t>> pixels();
        static std::size_t validate_size(std::uint32_t, std::uint32_t);
    };

// Windows software reference adapter. Host must initialize COM on its owner.
#ifdef _WIN32
    CanvasFactory make_d2d_canvas_factory();
#endif
} // namespace weft
