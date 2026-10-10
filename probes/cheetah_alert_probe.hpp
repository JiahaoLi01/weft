#pragma once

#include "probe_common.hpp"
#include "probe_constants.hpp"

namespace probes {
    // Original React component, original Zustand store and UE protocol modules.

    inline weft::NodeIdentity alert_button(const weft::DomSnapshot& root, int link = -1) {
        std::vector<weft::NodeIdentity> found;
        auto walk = [&](auto&& self, const auto& n) -> void {
            if (n.type == weft::constants::element_node_type && n.local_name == "button" &&
                (link < 0 ? n.attributes.contains("aria-expanded")
                          : n.attributes.contains("class") && n.attributes.at("class") == "link")) {
                found.push_back(n.identity);
            }
            for (const auto& child : n.children) {
                self(self, child);
            }
        };
        walk(walk, root);
        const auto index = link < 0 ? 0 : link;
        require(std::size_t(index) < found.size(), "original AlertBar button missing");
        return found[index];
    }

    // Style evaluation may start at html while formatting starts at #gallery.
    // Ancestors pass computed values, never become synthetic layout boxes.
    inline std::pair<weft::DomSnapshot, weft::ComputedStyle> alert_style_subtree(const weft::DomSnapshot& dom,
                                                                                 const weft::ComputedStyle& style) {
        if (dom.attributes.contains("id") && dom.attributes.at("id") == "gallery") {
            return {dom, style};
        }
        std::size_t i{};
        for (const auto& child : dom.children) {
            if (child.type == weft::constants::element_node_type) {
                try {
                    return alert_style_subtree(child, style.children[i]);
                } catch (const std::out_of_range&) {
                }
                ++i;
            }
        }
        throw std::out_of_range("gallery style subtree");
    }

    inline std::string alert_style_colors(const weft::ComputedStyle& style) {
        std::ostringstream out;
        out << '[';
        bool first = true;
        auto walk = [&](auto&& self, const auto& n) -> void {
            if (!first) {
                out << ',';
            }
            first = false;
            out << "{\"tag\":" << json_string(n.local_name);
            for (const auto* key : {"color", "background-color"}) {
                const auto value = n.values.contains(key)
                                       ? n.values.at(key)
                                       : (std::string(key) == "color" ? weft::constants::default_color : "transparent");
                const auto c = weft::parse_css_color(value);
                out << ',' << json_string(key) << ":[" << c[0] << ',' << c[1] << ',' << c[2] << ',' << c[3] << ']';
            }
            out << '}';
            for (const auto& c : n.children) {
                self(self, c);
            }
        };
        walk(walk, style);
        out << ']';
        return out.str();
    }
} // namespace probes
