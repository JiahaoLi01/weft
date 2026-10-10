#pragma once
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

namespace caseelephant {
// Owner-only raster service. No DOM, JS, device or window types in this contract.
struct ProbeCanvasCommand {
    enum class Kind { FillRect, FillCircle, ClearRect } kind;
    std::array<double,4> geometry; // rect x/y/w/h; circle x/y/r/unused
    std::array<float,4> srgb; // straight, source-over in Canvas's sRGB bitmap
};
class ProbeCanvasRaster {
public:
    virtual ~ProbeCanvasRaster()=default;
    virtual void draw(std::span<const ProbeCanvasCommand>)=0;
    virtual std::vector<std::uint8_t> rgba_straight()=0;
};
using ProbeCanvasFactory=std::function<std::unique_ptr<ProbeCanvasRaster>(std::uint32_t,std::uint32_t,bool)>;
struct ProbeCanvasBudget {std::size_t bytes{},contexts{},peak_bytes{};};
struct ProbeCanvasBackendError:std::runtime_error {using std::runtime_error::runtime_error;};
// Invalid CSS colors return nullopt; valid syntax outside the declared profile
// throws a diagnostic. Supports all named colors, hex and comma RGB/HSL forms.
std::optional<std::array<float,4>> parse_canvas_color_probe(std::string_view);
class ProbeCanvasState final {
    ProbeCanvasFactory factory_;
    std::unique_ptr<ProbeCanvasRaster> raster_;
    std::shared_ptr<ProbeCanvasBudget> budget_;
    std::vector<ProbeCanvasCommand> pending_;
    std::shared_ptr<const std::vector<std::uint8_t>> cached_;
    std::optional<std::array<double,4>> circle_;
    std::size_t bytes_{};
    void enqueue(ProbeCanvasCommand);
public:
    std::uint32_t width{},height{};
    bool alpha{};
    std::uint64_t version{1},commands{},fills{};
    std::array<float,4> color{0,0,0,1};
    std::string fill_style{"#000000"};
    double global_alpha{1};
    ProbeCanvasState(ProbeCanvasFactory,std::shared_ptr<ProbeCanvasBudget>,std::uint32_t,std::uint32_t,bool);
    ~ProbeCanvasState();
    ProbeCanvasState(const ProbeCanvasState&)=delete;
    void resize(std::uint32_t,std::uint32_t);
    void begin_path(){circle_.reset();}
    void arc(double,double,double,double,double,bool);
    void fill(std::string_view rule);
    void fill_rect(double,double,double,double,bool clear=false);
    std::shared_ptr<const std::vector<std::uint8_t>> pixels();
    static std::size_t validate_size(std::uint32_t,std::uint32_t);
};
// Windows software reference adapter. Host must initialize COM on its owner.
#ifdef _WIN32
ProbeCanvasFactory make_d2d_canvas_factory_probe();
#endif
}
