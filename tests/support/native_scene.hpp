#pragma once

#include "constants/common.hpp"
#include "dom/tree.hpp"

#include <algorithm>
#include <string>

namespace weft::testing {
    // A scene DOM without a script engine: document > div, the scene root.
    struct NativeScene {
        dom::Tree tree{1};
        dom::Node* document{};
        dom::Node* root{};

        NativeScene() {
            document = &tree.create(constants::document_node_type, "#document", "", {});
            root = &html("div");
            tree.insert(*document, *root, nullptr);
        }

        dom::Node& html(std::string local) {
            return tree.create(constants::element_node_type, std::move(local), constants::xhtml_namespace_uri,
                               document->identity);
        }

        dom::Node& svg(std::string local) {
            return tree.create(constants::element_node_type, std::move(local), constants::svg_namespace_uri,
                               document->identity);
        }

        void set(dom::Node& n, const std::string& name, std::string value) {
            auto it = std::find_if(n.attributes.begin(), n.attributes.end(), [&](const dom::Attribute& a) {
                return a.name == name;
            });
            if (it == n.attributes.end()) {
                n.attributes.push_back({name, name, "", std::move(value)});
            } else {
                it->value = std::move(value);
            }
            tree.changed(n);
        }

        void toggle_class(dom::Node& n, const std::string& name) {
            auto it = std::find_if(n.attributes.begin(), n.attributes.end(), [](const dom::Attribute& a) {
                return a.name == "class";
            });
            if (it != n.attributes.end() && it->value == name) {
                n.attributes.erase(it);
                tree.changed(n);
            } else {
                set(n, "class", name);
            }
        }
    };
} // namespace weft::testing
