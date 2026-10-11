#include "dom/attributes.hpp"
#include "dom/node_table.hpp"
#include "support/check.hpp"
#include <cstdint>
#include <iostream>
#include <memory>
#include <optional>
#include <string>

// src/dom values besides the selector engine: per-node side tables and HTML attribute values.
namespace {
    using weft::testing::require;
    namespace dom = weft::dom;

    std::optional<std::uint32_t> width(const char* text) {
        const std::string attribute(text);
        const auto value = dom::canvas_dimension(&attribute, dom::CanvasSide::Width);
        return value ? std::optional(*value) : std::nullopt;
    }
} // namespace

int main() {
    try {
        // A node's value is found by its slot and generation only; erasing destroys it.
        auto shared = std::make_shared<int>(7);
        dom::NodeTable<std::shared_ptr<int>> table;
        const weft::NodeIdentity node{3, 1, 1};
        table.emplace(node, shared);
        require(table.find(node) && **table.find(node) == 7 && table.size() == 1 && shared.use_count() == 2, "emplace");
        require(!table.find({3, 2, 1}) && !table.find({4, 1, 1}) && !table.find({100000, 1, 1}),
                "a later occupant or another slot found the value");
        table.erase({3, 2, 1});
        require(table.size() == 1 && shared.use_count() == 2, "a stale identity erased the value");
        table.erase(node);
        require(!table.find(node) && table.size() == 0 && shared.use_count() == 1, "erase kept the value");
        // The slot's next occupant starts without a value and may get its own; slots are independent.
        require(!table.find({3, 2, 1}), "the next occupant inherited a value");
        table.emplace({3, 2, 1}, std::make_shared<int>(8));
        table.emplace({0, 5, 1}, std::make_shared<int>(9));
        require(table.size() == 2 && **table.find({3, 2, 1}) == 8 && **table.find({0, 5, 1}) == 9 && !table.find(node),
                "independent slots");

        // Canvas sides: HTML's rules for parsing non-negative integers, or the 300 by 150 default.
        require(*dom::canvas_dimension(nullptr, dom::CanvasSide::Width) == 300 &&
                    *dom::canvas_dimension(nullptr, dom::CanvasSide::Height) == 150,
                "absent attribute");
        const std::string invalid = "x";
        require(*dom::canvas_dimension(&invalid, dom::CanvasSide::Height) == 150, "invalid height");

        struct Case {
            const char* text;
            std::uint32_t expected;
        };

        for (const auto& [text, expected] :
             {Case{"", 300}, Case{" ", 300}, Case{"12", 12}, Case{" \t\n\f\r12", 12}, Case{"+5", 5}, Case{"7px", 7},
              Case{"7.9", 7}, Case{"1e3", 1}, Case{"0002048", 2048}, Case{"0", 0}, Case{"-0", 0}, Case{"-00", 0},
              Case{"-1", 300}, Case{"-99999", 300}, Case{"abc", 300}, Case{"\v12", 300}, Case{"+-1", 300},
              Case{"- 1", 300}}) {
            require(width(text) == expected, std::string("canvas width of '") + text + "'");
        }
        // Above the profile bound, however long the digit run.
        for (const char* text : {"2049", "99999999999999999999999"}) {
            const std::string attribute(text);
            const auto value = dom::canvas_dimension(&attribute, dom::CanvasSide::Width);
            require(!value && value.error().code == weft::Diag::CanvasSizeLimit, std::string("accepted ") + text);
        }
        std::cout << "PASS DOM values: node side tables by slot and generation, canvas sizes by HTML's non-negative "
                     "integers with the profile bound\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "FAIL: " << e.what() << '\n';
        return 1;
    }
}
