#include "caseelephant/canvas_probe.hpp"
#include "caseelephant/base/css_scalar.hpp"
#include "caseelephant/constants/canvas.hpp"
#include "caseelephant/constants/common.hpp"
#include <algorithm>
#include <cctype>
#include <cmath>
#include <numbers>
#include <regex>
#include <stdexcept>
#include <utility>
#include <limits>

namespace caseelephant {
    namespace {
        [[noreturn]] void fail(Diag code, std::string_view detail = {}) {
            throw_diagnostic(code, detail);
        }

        std::string trim(std::string_view s) {
            return css::trim(s, css::syntax_whitespace);
        }

        struct Named {
            const char* name;
            std::uint32_t rgb;
        };

        constexpr Named named[] = {
#include "css_named_colors.inc"
        };

        std::array<float, 4> rgb(std::uint32_t n) {
            return {float((n >> 16) & 255) / constants::color_channel_max,
                    float((n >> 8) & 255) / constants::color_channel_max, float(n & 255) / constants::color_channel_max,
                    1};
        }
    } // namespace

    std::optional<std::array<float, 4>> parse_canvas_color_probe(std::string_view input) {
        if (input.size() > constants::canvas::max_color_length) {
            fail(Diag::CanvasColorLimit, "512 characters");
        }
        auto text = trim(std::string(input));
        for (auto& c : text) {
            c = char(std::tolower(static_cast<unsigned char>(c)));
        }
        if (text == "currentcolor") {
            fail(Diag::CanvasColorProfile, "currentColor requires computed style");
        }
        if (text == "transparent") {
            return std::array<float, 4>{0, 0, 0, 0};
        }
        for (const auto& n : named) {
            if (text == n.name) {
                return rgb(n.rgb);
            }
        }
        if (text.starts_with('#')) {
            auto hex = text.substr(1);
            if (hex.size() != 3 && hex.size() != 4 && hex.size() != 6 && hex.size() != 8) {
                return {};
            }
            if (!std::all_of(hex.begin(), hex.end(), [](unsigned char c) {
                    return std::isxdigit(c);
                })) {
                return {};
            }
            if (hex.size() < 5) {
                std::string full;
                for (auto c : hex) {
                    full += c;
                    full += c;
                }
                hex = full;
            }
            auto n = std::stoul(hex, nullptr, 16);
            if (hex.size() == 6) {
                return rgb(std::uint32_t(n));
            }
            auto result = rgb(std::uint32_t(n >> 8));
            result[3] = float(n & 255) / constants::color_channel_max;
            return result;
        }
        const auto open = text.find('(');
        if (open == text.npos || !text.ends_with(')')) {
            return {};
        }
        auto name = text.substr(0, open);
        if (name != "rgb" && name != "rgba" && name != "hsl" && name != "hsla") {
            fail(Diag::CanvasColorProfile, "color function unsupported");
        }
        auto body = text.substr(open + 1, text.size() - open - 2);
        if (body.find(',') == body.npos) {
            fail(Diag::CanvasColorProfile, "comma syntax required");
        }
        std::vector<std::pair<double, bool>> values;
        static const std::regex number("[+-]?(?:[0-9]+(?:\\.[0-9]*)?|\\.[0-9]+)(?:e[+-]?[0-9]+)?");
        for (std::size_t start = 0;;) {
            const auto end = body.find(',', start);
            auto part = trim(body.substr(start, end == body.npos ? end : end - start));
            const bool percent = part.ends_with('%');
            if (percent) {
                part.pop_back();
            }
            if (!std::regex_match(part, number)) {
                return {};
            }
            double v{};
            try {
                v = std::stod(part);
            } catch (...) {
                return {};
            }
            if (!std::isfinite(v)) {
                return {};
            }
            values.push_back({v, percent});
            if (end == body.npos) {
                break;
            }
            start = end + 1;
        }
        if (values.size() != 3 && values.size() != 4) {
            return {};
        }
        const bool hsl = name.starts_with("hsl");
        std::array<float, 4> result{0, 0, 0, 1};
        if (values.size() == 4) {
            result[3] = float(std::clamp(values[3].first / (values[3].second ? constants::percent_scale : 1), 0., 1.));
        }
        if (hsl) {
            if (values[0].second || !values[1].second || !values[2].second) {
                return {};
            }
            double h = std::fmod(values[0].first, static_cast<double>(constants::degrees_per_turn));
            if (h < 0) {
                h += constants::degrees_per_turn;
            }
            const double s = std::clamp(values[1].first / constants::percent_scale, 0., 1.);
            const double l = std::clamp(values[2].first / constants::percent_scale, 0., 1.);
            const double a = s * std::min(l, 1 - l);
            for (unsigned i = 0; i < 3; ++i) {
                const double k = std::fmod((i == 0 ? 0 : i == 1 ? 8 : 4) + h / 30, 12.);
                result[i] = float(l - a * std::max(-1., std::min({k - 3, 9 - k, 1.})));
            }
        } else {
            if (values[0].second != values[1].second || values[1].second != values[2].second) {
                return {};
            }
            for (unsigned i = 0; i < 3; ++i) {
                result[i] = float(std::clamp(
                    values[i].first / (values[i].second ? constants::percent_scale : constants::color_channel_max), 0.,
                    1.));
            }
        }
        return result;
    }

    std::size_t ProbeCanvasState::validate_size(std::uint32_t w, std::uint32_t h) {
        if (w > constants::max_canvas_dimension || h > constants::max_canvas_dimension) {
            fail(Diag::CanvasSizeLimit, "backing dimensions <=2048");
        }
        return std::size_t(w) * h * constants::rgba8_bytes_per_pixel;
    }

    ProbeCanvasState::ProbeCanvasState(ProbeCanvasFactory f, std::shared_ptr<ProbeCanvasBudget> budget, std::uint32_t w,
                                       std::uint32_t h, bool a)
        : factory_(std::move(f)), budget_(std::move(budget)), alpha(a) {
        if (!factory_ || !budget_) {
            throw std::invalid_argument("CE_CANVAS_CONFIG: factory and owner budget required");
        }
        if (budget_->contexts >= constants::canvas::max_live_contexts) {
            fail(Diag::CanvasContextLimit, "8 live contexts");
        }
        resize(w, h);
        ++budget_->contexts;
    }

    ProbeCanvasState::~ProbeCanvasState() {
        budget_->bytes -= bytes_;
        --budget_->contexts;
    }

    void ProbeCanvasState::resize(std::uint32_t w, std::uint32_t h) {
        if (version == std::numeric_limits<std::uint64_t>::max()) {
            fail(Diag::CanvasVersionLimit);
        }
        const auto bytes = validate_size(w, h);
        if (bytes > constants::canvas::max_backing_bytes - budget_->bytes) {
            fail(Diag::CanvasBytesLimit, "32MiB including resize overlap");
        }
        std::unique_ptr<ProbeCanvasRaster> next;
        try {
            next = factory_(w, h, alpha);
        } catch (const std::bad_alloc&) {
            throw;
        } catch (const std::exception& e) {
            throw ProbeCanvasBackendError(std::string("CE_CANVAS_BACKEND: ") + e.what());
        } catch (...) {
            throw ProbeCanvasBackendError("CE_CANVAS_BACKEND: unknown raster failure");
        }
        if (!next) {
            throw ProbeCanvasBackendError("CE_CANVAS_BACKEND: null raster");
        }
        budget_->peak_bytes = std::max(budget_->peak_bytes, budget_->bytes + bytes);
        budget_->bytes = budget_->bytes - bytes_ + bytes;
        bytes_ = bytes;
        raster_ = std::move(next);
        width = w;
        height = h;
        pending_.clear();
        cached_.reset();
        circle_.reset();
        color = {0, 0, 0, 1};
        fill_style = constants::canvas::default_fill_style;
        global_alpha = 1;
        ++version;
    }

    void ProbeCanvasState::enqueue(ProbeCanvasCommand c) {
        if (version == std::numeric_limits<std::uint64_t>::max() ||
            commands == std::numeric_limits<std::uint64_t>::max() ||
            fills == std::numeric_limits<std::uint64_t>::max()) {
            fail(Diag::CanvasVersionLimit);
        }
        if (pending_.size() >= constants::max_canvas_pending_commands) {
            fail(Diag::CanvasCommandLimit, "65536 before snapshot");
        }
        pending_.push_back(c);
        ++version;
        ++commands;
        cached_.reset();
    }

    void ProbeCanvasState::arc(double x, double y, double r, double start, double end, bool ccw) {
        for (double v : {x, y, r, start, end}) {
            if (!std::isfinite(v)) {
                return;
            }
        }
        if (r < 0) {
            fail(Diag::CanvasIndexSize, "negative arc radius");
        }
        if (std::abs(x) > constants::canvas::max_geometry_magnitude ||
            std::abs(y) > constants::canvas::max_geometry_magnitude || r > constants::canvas::max_geometry_magnitude) {
            fail(Diag::CanvasGeometryLimit, "1e6");
        }
        if (circle_ || (ccw ? start - end : end - start) < 2 * std::numbers::pi) {
            fail(Diag::CanvasPathProfile, "one full circle per path");
        }
        circle_ = std::array<double, 4>{x, y, r, 0};
    }

    void ProbeCanvasState::fill(std::string_view rule) {
        if (rule != "nonzero" && rule != "evenodd") {
            fail(Diag::CanvasFillRule, "nonzero/evenodd required");
        }
        if (circle_ && (*circle_)[2] > 0) {
            auto c = color;
            c[3] *= float(global_alpha);
            enqueue({ProbeCanvasCommand::Kind::FillCircle, *circle_, c});
            ++fills;
        }
    }

    void ProbeCanvasState::fill_rect(double x, double y, double w, double h, bool clear) {
        for (double v : {x, y, w, h}) {
            if (!std::isfinite(v)) {
                return;
            }
        }
        for (double v : {x, y, w, h}) {
            if (std::abs(v) > constants::canvas::max_geometry_magnitude) {
                fail(Diag::CanvasGeometryLimit, "1e6");
            }
        }
        if (!w || !h) {
            return;
        }
        if (w < 0) {
            x += w;
            w = -w;
        }
        if (h < 0) {
            y += h;
            h = -h;
        }
        if (clear && (x != std::floor(x) || y != std::floor(y) || w != std::floor(w) || h != std::floor(h))) {
            fail(Diag::CanvasClearProfile, "integer rectangles required");
        }
        auto c = color;
        c[3] *= float(global_alpha);
        enqueue({clear ? ProbeCanvasCommand::Kind::ClearRect : ProbeCanvasCommand::Kind::FillRect, {x, y, w, h}, c});
        ++fills;
    }

    std::shared_ptr<const std::vector<std::uint8_t>> ProbeCanvasState::pixels() {
        if (!cached_) {
            raster_->draw(pending_);
            auto bytes = raster_->rgba_straight();
            if (bytes.size() != bytes_) {
                throw std::runtime_error("CE_CANVAS_BACKEND: incomplete RGBA snapshot");
            }
            cached_ = std::make_shared<const std::vector<std::uint8_t>>(std::move(bytes));
            pending_.clear();
        }
        return cached_;
    }
} // namespace caseelephant
