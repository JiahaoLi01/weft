#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <span>

// Blink's spatial navigation (third_party/blink/renderer/core/page/spatial_navigation.cc and
// spatial_navigation_controller.cc, Chrome's --enable-spatial-navigation) over a scene's elements: which element an
// arrow key moves the focus to, or which scroll container it scrolls a line instead.
namespace weft {
    enum class NavDirection : std::uint8_t {
        None,
        Up,
        Down,
        Left,
        Right
    };

    // A rectangle of the View in its px (Blink's PhysicalRect, without its 1/64 px units).
    struct NavRect {
        double x{};
        double y{};
        double width{};
        double height{};

        double right() const {
            return x + width;
        }
        double bottom() const {
            return y + height;
        }
        bool empty() const {
            return width <= 0 || height <= 0;
        }
        bool contains(const NavRect& o) const {
            return x <= o.x && right() >= o.right() && y <= o.y && bottom() >= o.bottom();
        }
        // Whether they meet, edges included.
        bool intersects_inclusively(const NavRect& o) const {
            return x <= o.right() && o.x <= right() && y <= o.bottom() && o.y <= bottom();
        }
    };

    NavRect intersection(const NavRect& a, const NavRect& b);

    // A scene element as spatial navigation sees it, in tree order (an element's subtree follows it up to end).
    struct NavElement {
        // The parent's index; npos for the scene's root, whose parent is the document.
        std::size_t parent{};
        std::size_t end{};
        // Its box in the View (Blink's NodeRectInRootFrame: the padding box), clipped by its ancestors' overflow.
        NavRect rect;
        // A focus candidate: rendered, focusable by the keyboard, not the scene's root.
        bool candidate{};
        // An element spatial navigation may pass over for a focusable descendant inside it (IsSkippableCandidate): no
        // explicit tabindex of 0 or more, no editing host.
        bool skippable{true};
        // Whether the user can scroll it horizontally and vertically (overflow auto or scroll, with something to
        // scroll), its scroll offset and the largest.
        bool scrolls_x{};
        bool scrolls_y{};
        double scroll_left{};
        double scroll_top{};
        double max_left{};
        double max_top{};
    };

    inline constexpr std::size_t nav_document = static_cast<std::size_t>(-1);

    // What an arrow key does: the element (an index) that takes the focus, or the container (an index) that scrolls a
    // line its way, or nothing.
    struct NavResult {
        enum class Kind : std::uint8_t {
            None,
            Focus,
            Scroll
        } kind{};
        std::size_t element{};
    };

    // SpatialNavigationController::Advance from the focused element (none: the document) in the viewport. unobscured
    // answers Blink's IsUnobscured of a candidate (a hit test of its visible part finds it or a descendant).
    NavResult spatial_navigation(std::span<const NavElement> elements, std::optional<std::size_t> focus,
                                 const NavRect& viewport, NavDirection direction,
                                 const std::function<bool(std::size_t)>& unobscured);
} // namespace weft
