#pragma once
#include "layout/output.hpp"
#include "layout/page.hpp"
#include "constants/common.hpp"
#include <optional>

namespace weft {
    struct Viewport {
        float x{};
        float y{};
        float width{};
        float height{};
        float scale{};
        static Viewport fit(float physical_width, float physical_height,
                            float logical_width = constants::default_view_width,
                            float logical_height = constants::default_view_height);
        // Compose logical -> raster surface -> window, including both letterboxes.
        static Viewport through_surface(float physical_width, float physical_height, float surface_width,
                                        float surface_height, float logical_width = constants::default_view_width,
                                        float logical_height = constants::default_view_height);
        // Coordinates outside the page/letterbox remain outside the logical viewport.
        std::array<double, 2> logical(double physical_x, double physical_y) const;
    };

    std::optional<std::size_t> hit_page(const Page& page, double x, double y);
    enum class MouseAction {
        Down,
        Up,
        Move,
        Cancel
    };

    class MouseRouter {
    public:
        MouseRouter(ScriptDom& script, Font& font, std::string root = "#page")
            : script_(script), layout_([&script, &font, root = std::move(root)] {
                  return build_page(script.snapshot_subtree(root), font);
              }) {
        }

        // Owner callback must provide fresh owning values; no retained Node/JSValue.
        MouseRouter(ScriptDom& script, std::function<Page()> layout) : script_(script), layout_(std::move(layout)) {
        }

        // Full owner layout flush before each hit and after mouseup before click.
        bool route(MouseAction action, MouseEvent input);
        DispatchResult wheel(const WheelEvent& input);

        void cancel() {
            (void)script_.epoch();
            pressed_.reset();
        }

    private:
        ScriptDom& script_;
        std::function<Page()> layout_;
        std::optional<NodeIdentity> pressed_;
    };
} // namespace weft
