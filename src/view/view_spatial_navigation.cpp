#include "view/view_spatial_navigation.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <utility>

namespace weft {
    namespace {
        constexpr double max_distance = std::numeric_limits<double>::max();
        // Blink's kMinDistance / 4 (kPriorityClassB): candidates that overlap the search origin come first.
        constexpr double priority_overlap = static_cast<double>(std::numeric_limits<int>::lowest()) / 4;

        bool horizontal(NavDirection d) {
            return d == NavDirection::Left || d == NavDirection::Right;
        }

        // Whether a is below, above, right of or left of b; overlapping rects by the edge on that side (Below, Above,
        // RightOf, LeftOf).
        bool below(const NavRect& a, const NavRect& b) {
            return a.y >= b.bottom() || (a.y > b.y && a.intersects_inclusively(b));
        }
        bool above(const NavRect& a, const NavRect& b) {
            return a.bottom() <= b.y || (a.bottom() < b.bottom() && a.intersects_inclusively(b));
        }
        bool right_of(const NavRect& a, const NavRect& b) {
            return a.x >= b.right() || (a.x > b.x && a.intersects_inclusively(b));
        }
        bool left_of(const NavRect& a, const NavRect& b) {
            return a.right() <= b.x || (a.right() < b.right() && a.intersects_inclusively(b));
        }

        // IsRectInDirection: an "insider" of the current rect is its way, its container never.
        bool in_direction(NavDirection d, const NavRect& current, const NavRect& target) {
            if (target.contains(current)) {
                return false;
            }
            if (current.contains(target)) {
                return true;
            }
            switch (d) {
            case NavDirection::Left:
                return left_of(target, current);
            case NavDirection::Right:
                return right_of(target, current);
            case NavDirection::Up:
                return above(target, current);
            case NavDirection::Down:
                return below(target, current);
            default:
                return false;
            }
        }

        bool intersect_on_orthogonal_axis(NavDirection d, const NavRect& a, const NavRect& b) {
            return horizontal(d) ? a.bottom() > b.y && a.y < b.bottom() : a.right() > b.x && a.x < b.right();
        }

        // OppositeEdge: the box's edge a search towards side starts from, a zero-thick slice just outside it.
        NavRect opposite_edge(NavDirection side, const NavRect& box) {
            NavRect thin = box;
            switch (side) {
            case NavDirection::Left:
                thin.x = thin.right();
                thin.width = 0;
                thin.x += 1;
                break;
            case NavDirection::Right:
                thin.width = 0;
                thin.x -= 1;
                break;
            case NavDirection::Down:
                thin.height = 0;
                thin.y -= 1;
                break;
            case NavDirection::Up:
                thin.y = thin.bottom();
                thin.height = 0;
                thin.y += 1;
                break;
            default:
                break;
            }
            return thin;
        }

        struct Point {
            double x{};
            double y{};
        };

        // EntryAndExitPointsForDirection: the closest points of the two rects, the line between them their distance.
        std::pair<Point, Point> entry_and_exit(NavDirection d, const NavRect& start, const NavRect& potential) {
            Point exit;
            Point entry;
            switch (d) {
            case NavDirection::Left:
                exit.x = start.x;
                entry.x = std::min(potential.right(), start.x);
                break;
            case NavDirection::Up:
                exit.y = start.y;
                entry.y = std::min(potential.bottom(), start.y);
                break;
            case NavDirection::Right:
                exit.x = start.right();
                entry.x = std::max(potential.x, start.right());
                break;
            case NavDirection::Down:
                exit.y = start.bottom();
                entry.y = std::max(potential.y, start.bottom());
                break;
            default:
                break;
            }
            if (horizontal(d)) {
                if (below(start, potential)) {
                    exit.y = start.y;
                    entry.y = std::min(potential.bottom(), start.y);
                } else if (below(potential, start)) {
                    exit.y = start.bottom();
                    entry.y = std::max(potential.y, start.bottom());
                } else {
                    exit.y = std::max(start.y, potential.y);
                    entry.y = exit.y;
                }
            } else {
                if (right_of(start, potential)) {
                    exit.x = start.x;
                    entry.x = std::min(potential.right(), start.x);
                } else if (right_of(potential, start)) {
                    exit.x = start.right();
                    entry.x = std::max(potential.x, start.right());
                } else {
                    exit.x = std::max(start.x, potential.x);
                    entry.x = exit.x;
                }
            }
            return {entry, exit};
        }

        // ProjectedOverlap and Alignment (css-nav-1's heuristics): how much of the current rect the candidate covers
        // across the way, weighted 5.
        double alignment(NavDirection d, NavRect current, NavRect candidate) {
            constexpr double weight = 5;
            if (horizontal(d)) {
                current.width = 1;
                candidate.x = current.x;
                const auto overlap = intersection(current, candidate).height;
                return current.height > 0 ? weight * overlap / current.height : 0;
            }
            current.height = 1;
            candidate.y = current.y;
            const auto overlap = intersection(current, candidate).width;
            return current.width > 0 ? weight * overlap / current.width : 0;
        }

        // ComputeDistanceDataForNode (no popup class: weft has no separate painting layers to tell them).
        double distance(NavDirection d, NavRect current, const NavRect& candidate) {
            if (!in_direction(d, current, candidate)) {
                return max_distance;
            }
            double total = 0;
            if (current.intersects_inclusively(candidate)) {
                // An overlapping candidate wins over any outside: its distance is from the current rect's far edge.
                total = priority_overlap;
                current = opposite_edge(d, current);
            }
            const auto [entry, exit] = entry_and_exit(d, current, candidate);
            const double x_axis = std::abs(exit.x - entry.x);
            const double y_axis = std::abs(exit.y - entry.y);
            total += std::sqrt(x_axis * x_axis + y_axis * y_axis);
            // Aligned candidates win over partly aligned ones, and those over the others; left and right weigh the
            // orthogonal distance more, for rows of elements.
            constexpr double weight_left_right = 30;
            constexpr double weight_up_down = 2;
            double navigation_axis{};
            double weighted_orthogonal{};
            if (horizontal(d)) {
                navigation_axis = x_axis;
                const double bias =
                    intersect_on_orthogonal_axis(d, current, candidate) ? 0 : std::trunc(current.height / 2);
                weighted_orthogonal = (y_axis + bias) * weight_left_right;
            } else {
                navigation_axis = y_axis;
                const double bias =
                    intersect_on_orthogonal_axis(d, current, candidate) ? 0 : std::trunc(current.width / 2);
                weighted_orthogonal = (x_axis + bias) * weight_up_down;
            }
            total += weighted_orthogonal + navigation_axis;
            return total - alignment(d, current, candidate);
        }

        struct Navigator {
            std::span<const NavElement> elements;
            const NavRect& viewport;
            NavDirection direction;
            const std::function<bool(std::size_t)>& unobscured;

            bool offscreen(std::size_t k) const {
                return k == nav_document ? false : intersection(elements[k].rect, viewport).empty();
            }

            // IsScrollableNode: the document always; an element the user can scroll on the way's axis (on either
            // without a way).
            bool scrollable(std::size_t k, NavDirection d) const {
                if (k == nav_document) {
                    return true;
                }
                const auto& e = elements[k];
                switch (d) {
                case NavDirection::Left:
                case NavDirection::Right:
                    return e.scrolls_x;
                case NavDirection::Up:
                case NavDirection::Down:
                    return e.scrolls_y;
                default:
                    return e.scrolls_x || e.scrolls_y;
                }
            }

            // ScrollableAreaOrDocumentOf: the nearest ancestor that scrolls that way, or the document; none past it.
            std::optional<std::size_t> container_of(std::size_t k, NavDirection d) const {
                std::optional<std::size_t> parent = k;
                do {
                    if (*parent == nav_document) {
                        return std::nullopt;
                    }
                    parent = elements[*parent].parent;
                } while (!scrollable(*parent, d));
                return parent;
            }

            bool descendant_of(std::size_t k, std::size_t ancestor) const {
                return ancestor != nav_document && k > ancestor && k < elements[ancestor].end;
            }

            const NavRect& rect(std::size_t k) const {
                return k == nav_document ? viewport : elements[k].rect;
            }

            // SearchOrigin: the focused element's visible part, or the edge of its first scroller that shows, or the
            // viewport's edge.
            NavRect search_origin(std::optional<std::size_t> focus) const {
                if (!focus || *focus == nav_document) {
                    return opposite_edge(direction, viewport);
                }
                if (!offscreen(*focus)) {
                    return intersection(elements[*focus].rect, viewport);
                }
                auto container = container_of(*focus, NavDirection::None);
                while (container) {
                    if (*container == nav_document) {
                        break;
                    }
                    if (!offscreen(*container)) {
                        return opposite_edge(direction, intersection(elements[*container].rect, viewport));
                    }
                    container = container_of(*container, NavDirection::None);
                }
                return opposite_edge(direction, viewport);
            }

            // FindNextCandidateInContainer with ConsiderForBestCandidate: the best candidate in the container's
            // subtree (the subtrees of scroll containers in it left out), from the starting rect.
            std::optional<std::size_t> best_in(std::size_t container, const NavRect& start,
                                               std::optional<std::size_t> interest_child) const {
                const std::size_t first = container == nav_document ? 0 : container + 1;
                const std::size_t last = container == nav_document ? elements.size() : elements[container].end;
                std::optional<std::size_t> best;
                std::optional<std::size_t> previous;
                double best_distance = max_distance;
                double previous_distance = max_distance;
                for (std::size_t k = first; k < last;) {
                    const std::size_t next = scrollable(k, direction) ? elements[k].end : k + 1;
                    const std::size_t at = k;
                    k = next;
                    if ((interest_child && at == *interest_child) || !elements[at].candidate || offscreen(at)) {
                        continue;
                    }
                    const double d = distance(direction, start, elements[at].rect);
                    if (d == max_distance) {
                        continue;
                    }
                    // A skippable best that contains this, its descendant, gives way to the best before it.
                    if (best && descendant_of(at, *best) && elements[*best].skippable &&
                        elements[*best].rect.contains(elements[at].rect)) {
                        best = previous;
                        best_distance = previous_distance;
                        previous_distance = max_distance;
                    }
                    // A tie goes to the container: the focus moves from the outside in.
                    const bool tie = best && std::abs(d - best_distance) < std::numeric_limits<double>::epsilon() &&
                                     elements[at].rect.contains(elements[*best].rect);
                    if ((d < best_distance || tie) && unobscured(at)) {
                        previous = best;
                        previous_distance = best_distance;
                        best = at;
                        best_distance = d;
                    }
                }
                return best;
            }

            // CanScrollInDirection of an element (the View's document does not scroll).
            bool can_scroll(std::size_t k) const {
                if (k == nav_document || !scrollable(k, direction)) {
                    return false;
                }
                const auto& e = elements[k];
                switch (direction) {
                case NavDirection::Left:
                    return e.scroll_left > 0;
                case NavDirection::Up:
                    return e.scroll_top > 0;
                case NavDirection::Right:
                    return e.scroll_left < e.max_left;
                case NavDirection::Down:
                    return e.scroll_top < e.max_top;
                default:
                    return false;
                }
            }

            // AdvanceWithinContainer: its best candidate takes the focus; with none, it scrolls if it can.
            std::optional<NavResult> within(std::size_t container, const NavRect& start,
                                            std::optional<std::size_t> interest_child) const {
                if (const auto best = best_in(container, start, interest_child)) {
                    return NavResult{NavResult::Kind::Focus, *best};
                }
                if (can_scroll(container)) {
                    return NavResult{NavResult::Kind::Scroll, container};
                }
                return std::nullopt;
            }
        };
    } // namespace

    NavRect intersection(const NavRect& a, const NavRect& b) {
        const double x = std::max(a.x, b.x);
        const double y = std::max(a.y, b.y);
        const double right = std::min(a.right(), b.right());
        const double bottom = std::min(a.bottom(), b.bottom());
        if (right <= x || bottom <= y) {
            return {};
        }
        return {x, y, right - x, bottom - y};
    }

    NavResult spatial_navigation(std::span<const NavElement> elements, std::optional<std::size_t> focus,
                                 const NavRect& viewport, NavDirection direction,
                                 const std::function<bool(std::size_t)>& unobscured) {
        if (direction == NavDirection::None) {
            return {};
        }
        const Navigator nav{elements, viewport, direction, unobscured};
        const std::size_t interest = focus.value_or(nav_document);
        const auto start = nav.search_origin(focus);
        // A scroll container that shows and has the focus: search inside it from its edge.
        if (nav.scrollable(interest, direction) && !nav.offscreen(interest)) {
            if (const auto found = nav.within(interest, opposite_edge(direction, start), std::nullopt)) {
                return *found;
            }
        }
        // Then outwards: a container's own element never takes the focus on the way out.
        std::optional<std::size_t> skipped = interest == nav_document ? std::nullopt : std::optional(interest);
        auto container = interest == nav_document ? std::nullopt : nav.container_of(interest, direction);
        while (container) {
            if (const auto found = nav.within(*container, start, skipped)) {
                return *found;
            }
            skipped = *container;
            container = *container == nav_document ? std::nullopt : nav.container_of(*container, direction);
        }
        return {};
    }
} // namespace weft
