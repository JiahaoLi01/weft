#include "dom/node.hpp"

#include <algorithm>
#include <utility>

namespace weft::dom {
    bool update_copy(DomSnapshot& to, const DomSnapshot& from) {
        if (to.identity != from.identity) {
            to = from;
            return true;
        }
        if (from.revision && to.revision == from.revision) {
            return false;
        }
        bool changed{};
        const auto assign = [&](auto& target, const auto& source) {
            if (target != source) {
                target = source;
                changed = true;
            }
        };
        assign(to.type, from.type);
        assign(to.local_name, from.local_name);
        assign(to.namespace_uri, from.namespace_uri);
        assign(to.data, from.data);
        assign(to.attributes, from.attributes);
        assign(to.scroll_left, from.scroll_left);
        assign(to.scroll_top, from.scroll_top);
        assign(to.hovered, from.hovered);
        assign(to.active, from.active);
        const bool same_children = to.children.size() == from.children.size() &&
                                   std::equal(to.children.begin(), to.children.end(), from.children.begin(),
                                              [](const DomSnapshot& a, const DomSnapshot& b) {
                                                  return a.identity == b.identity;
                                              });
        if (same_children) {
            for (std::size_t i = 0; i < from.children.size(); ++i) {
                changed |= update_copy(to.children[i], from.children[i]);
            }
        } else {
            // Children keep their copies by identity; a node new to this parent is copied whole.
            std::vector<DomSnapshot> children;
            children.reserve(from.children.size());
            for (std::size_t i = 0; i < from.children.size(); ++i) {
                const auto& source = from.children[i];
                auto found = i < to.children.size() && to.children[i].identity == source.identity
                                 ? to.children.begin() + std::ptrdiff_t(i)
                                 : std::find_if(
                                       to.children.begin(), to.children.end(),
                                       [&](const DomSnapshot& child) {
                                           return child.identity == source.identity;
                                       });
                if (found == to.children.end()) {
                    children.push_back(source);
                } else {
                    children.push_back(std::move(*found));
                    found->identity = {};
                    update_copy(children.back(), source);
                }
            }
            to.children = std::move(children);
            changed = true;
        }
        to.revision = from.revision;
        return changed;
    }

    bool same_content(const DomSnapshot& a, const DomSnapshot& b) {
        if (a.identity != b.identity) {
            return false;
        }
        if (a.revision && a.revision == b.revision) {
            return true;
        }
        if (a.type != b.type || a.local_name != b.local_name || a.namespace_uri != b.namespace_uri ||
            a.data != b.data || a.attributes != b.attributes || a.scroll_left != b.scroll_left ||
            a.scroll_top != b.scroll_top || a.hovered != b.hovered || a.active != b.active ||
            a.children.size() != b.children.size()) {
            return false;
        }
        for (std::size_t i = 0; i < a.children.size(); ++i) {
            if (!same_content(a.children[i], b.children[i])) {
                return false;
            }
        }
        return true;
    }
} // namespace weft::dom
