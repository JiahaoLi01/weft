#include "style/style.hpp"
#include "script/script_dom.hpp"
#include "support/check.hpp"
#include "support/files.hpp"
#include "constants/common.hpp"
#include "constants/style.hpp"
#include "test_constants/style_text.hpp"
#include "layout/document.hpp"
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <stdexcept>
using namespace weft;

namespace {
    using weft::testing::read_file;
    using weft::testing::require;

    void near(float a, float b) {
        require(std::abs(a - b) < weft::test_constants::style_text::float_tolerance, "numeric discrepancy");
    }

    DomSnapshot node(std::string id, std::string style = "", std::string tag = "div") {
        DomSnapshot n;
        n.identity = {unsigned(id.size()), 1, 1};
        n.type = weft::constants::element_node_type;
        n.local_name = tag;
        n.namespace_uri = weft::constants::xhtml_namespace_uri;
        n.attributes = {{"id", id}, {"style", style}};
        return n;
    }

    template <class F>
    void rejects(F f) {
        try {
            f();
        } catch (const std::invalid_argument&) {
            return;
        }
        throw std::runtime_error("unsupported input accepted");
    }

    const ComputedStyle& find(const DomSnapshot& dom, const ComputedStyle& style, const std::string& id) {
        if (dom.attributes.contains("id") && dom.attributes.at("id") == id) {
            return style;
        }
        std::size_t i{};
        for (const auto& c : dom.children) {
            if (c.type == weft::constants::element_node_type) {
                try {
                    return find(c, style.children[i], id);
                } catch (const std::out_of_range&) {
                }
                ++i;
            }
        }
        throw std::out_of_range("style id");
    }
} // namespace

int main(int argc, char** argv) {
    try {
        auto profile = document_style_profile();
        auto solid = node("solid", "background-color:#ff0000;background:var(--ink);--ink:rgba(10,8,6,0.5)");
        const auto solid_color = parse_css_color(compute_style(solid, "", profile).values.at("background-color"));
        near(solid_color[0], 10 / 255.f);
        near(solid_color[1], 8 / 255.f);
        near(solid_color[2], 6 / 255.f);
        near(solid_color[3], .5f);
        solid.attributes["style"] = "background:#0000ff!important;background-color:#ff0000";
        require(compute_style(solid, "", profile).values.at("background-color") == "#0000ff",
                "solid background importance");
        solid.attributes["style"] = "background:url(test.png)";
        rejects([&] {
            compute_style(solid, "", profile);
        });
        solid.attributes["style"] = "--ink:#000000 none;background:var(--ink)";
        rejects([&] {
            compute_style(solid, "", profile);
        });
        auto root = node("root", "--a:#112233;--b:var(--a);color:#123456;width:200px;height:100px");
        auto child = node("child", "--a:#abcdef;color:var(--b);height:24px");
        root.children.push_back(child);
        auto style = compute_style(root, "", profile);
        require(style.children[0].values.at("color") == "#112233", "inherited variable must already be resolved");
        root.attributes["style"] += ";--A:#abcdef;--empty:;--initial:initial";
        root.children[0].attributes["style"] = "color:var(--missing,var(--A));height:24px;--inherit:inherit";
        style = compute_style(root, "", profile);
        require(style.children[0].values.at("color") == "#abcdef", "nested fallback/case");
        require(style.custom_properties.at("--empty") == "" && !style.custom_properties.at("--initial"),
                "empty differs from guaranteed-invalid");
        root.children[0].attributes["style"] =
            "color:#ff0000;color:var(--empty,#ffffff);width:10px;width:var(--number)px;--number:16;height:24px;background-color:var(--missing)";
        style = compute_style(root, "", profile);
        require(style.children[0].values.at("color") == "#123456" && style.children[0].values.at("width") == "auto" &&
                    style.children[0].values.at("background-color") == "transparent",
                "IACVT unsets winner; never recascade or concatenate tokens");
        require(style.children[0].invalid_at_computed_value.size() == 3, "computed invalid diagnostics");
        root.children[0].attributes["style"] =
            "--a:var(--b);--b:var(--c);--c:var(--a);--d:var(--c);--a:var(--b,var(--d));--safe:var(--a,#abcdef);color:var(--safe);height:24px";
        style = compute_style(root, "", profile);
        for (const auto* k : {"--a", "--b", "--c", "--d"}) {
            require(!style.children[0].custom_properties.at(k), "all evaluated cycle members must invalidate");
        }
        require(style.children[0].values.at("color") == "#abcdef", "dependent outside cycle may recover with fallback");
        root.children[0].attributes["style"] = "--ok:#ffffff;--a:var(--ok,var(--a));color:var(--a,#abcdef);height:24px";
        require(compute_style(root, "", profile).children[0].values.at("color") == "#ffffff",
                "unused fallback must not create a cycle");
        root.children[0].attributes["style"] =
            "--a:initial;color:var(--missing,);height:24px;width:inherit;background-color:unset";
        style = compute_style(root, "", profile);
        require(style.children[0].values.at("width") == "200px" && style.children[0].values.at("color") == "#123456",
                "CSS-wide/empty fallback");
        root.children[0].attributes["style"] = "--b:var(--missing,inherit);color:var(--b);height:24px";
        require(compute_style(root, "", profile).children[0].values.at("color") == "#112233",
                "custom CSS-wide after substitution inherits parent variable");
        root.children[0].attributes["style"] = "--n:16px;display:var(--n);height:24px";
        require(compute_style(root, "", profile).children[0].values.at("display") == "inline",
                "keyword type invalid at computed time");
        for (const auto* declaration : {"--x:1em;width:var(--x)", "--x:blue;color:var(--x)", "--x:revert",
                                        "--x:var(--missing,revert-layer)", "--x:10px 20px;inset:var(--x)"}) {
            root.children[0].attributes["style"] = declaration;
            rejects([&] {
                compute_style(root, "", profile);
            });
        }
        root.children[0].attributes["style"] = "--text:'!important; a,b';height:24px";
        require(compute_style(root, "", profile).children[0].custom_properties.at("--text") == "'!important; a,b'",
                "quoted importance and separators remain tokens");
        root.children[0].attributes["style"] = "height:24px;color:#000000";
        root.children[0].attributes["aria-expanded"] = "true";
        style = compute_style(
            root,
            "#child{color:#ff0000!important} [aria-expanded=\"true\"]{color:#ffffff} #child{color:#00ff00!important}",
            profile);
        require(style.children[0].values.at("color") == "#00ff00",
                "important beats inline normal, later equal specificity wins");
        root.children[0].attributes["style"] += ";color:#112233!important";
        require(compute_style(root, "#child{color:#ff0000!important}", profile).children[0].values.at("color") ==
                    "#112233",
                "inline important wins");
        // Descendant search must backtrack if the nearest B fails A > B.
        auto a = node("a");
        a.attributes["class"] = "a";
        auto b = node("b");
        b.attributes["class"] = "b";
        auto bridge = node("bridge");
        auto inner = node("inner");
        inner.attributes["class"] = "b";
        inner.children.push_back(node("leaf"));
        bridge.children.push_back(inner);
        b.children.push_back(bridge);
        a.children.push_back(b);
        require(compute_style(a, ".a > .b #leaf{color:#123456}", profile)
                        .children[0]
                        .children[0]
                        .children[0]
                        .children[0]
                        .values.at("color") == "#123456",
                "ancestor selector backtracking");
        root.children[0].attributes["label"] = "a,b";
        require(
            compute_style(root, "[label=\"a,b\"], [missing]{color:#abcdef}", profile).children[0].values.at("color") ==
                "#112233",
            "inline important retained through quoted selector list");
        root.children[0].attributes["style"] = "height:24px";
        require(compute_style(root, ":root{--theme:#ffffff}#child{color:var(--theme,#abcdef)}", profile)
                        .children[0]
                        .values.at("color") == "#abcdef",
                "scoped root must not impersonate document root");
        auto document = node("document", "", "html");
        document.children.push_back(root);
        style = compute_style(document, ":root{--theme:#ffffff}#child{color:var(--theme,#abcdef)}", profile);
        require(style.children[0].children[0].values.at("color") == "#ffffff", "document root variable inheritance");
        near(parse_css_color("rgba(160,154,130,.22)")[3], .22f);
        near(parse_css_color("rgb(300,-10,0)")[0], 1);
        near(parse_css_color("rgb(300,-10,0)")[1], 0);
        for (const auto* css :
             {"#root:hover{color:#ffffff}", " > #root{color:#ffffff}", ".root*{color:#ffffff}",
              "@media screen{#root{color:#ffffff}}", "#root{opacity:.5}", "#root{--a:var(foo)}",
              "#root{--a:var(--a)} trailing", "#root{color:rgba(1,2,3,.5}", "#root[foo^=a]{color:#ffffff}"}) {
            rejects([&] {
                compute_style(root, css, profile);
            });
        }
        root.attributes["style"] = std::string(weft::constants::style::max_declaration_bytes + 1, ' ');
        rejects([&] {
            compute_style(root, "", profile);
        });
        root.attributes["style"] = "--a:x";
        std::string expansion = "--v0:x;";
        for (int i = 1; i < weft::test_constants::style_text::var_doubling_levels; ++i) {
            expansion += "--v" + std::to_string(i) + ":var(--v" + std::to_string(i - 1) + ") var(--v" +
                         std::to_string(i - 1) + ");";
        }
        rejects([&] {
            compute_style(root, "#root{" + expansion + "}", profile);
        });
        root.attributes["style"].clear();
        for (int i = 0; i < weft::constants::style::max_custom_properties + 1; ++i) {
            root.attributes["style"] += "--v" + std::to_string(i) + ":0;";
        }
        rejects([&] {
            compute_style(root, "", profile);
        });
        {
            const std::string sheet =
                ".paint{color:var(--ink)}.hot .paint{background-color:#334455}.container > .paint{height:12px}[data-tone=\"gold\"]{color:#aabbcc}";
            CompiledStyle compiled(sheet, profile);
            StyleMemo memo;
            auto tree = node("memo-root", "--ink:#112233;color:#123456");
            tree.identity = {1, 1, weft::test_constants::style_text::memo_view_epoch};
            for (unsigned p = 0; p < 2; ++p) {
                auto parent = node("memo-p" + std::to_string(p));
                parent.identity = {p + 2, 1, weft::test_constants::style_text::memo_view_epoch};
                parent.attributes["class"] = "container";
                for (unsigned n = 0; n < weft::test_constants::style_text::memo_leaves_per_parent; ++n) {
                    auto leaf = node(
                        "memo-n" + std::to_string(p * weft::test_constants::style_text::memo_leaves_per_parent + n),
                        "width:10px");
                    leaf.identity = {p * weft::test_constants::style_text::memo_leaves_per_parent + n +
                                         weft::test_constants::style_text::memo_first_leaf_slot,
                                     1, weft::test_constants::style_text::memo_view_epoch};
                    leaf.attributes["class"] = "paint";
                    leaf.attributes.erase("id");
                    parent.children.push_back(leaf);
                }
                tree.children.push_back(parent);
            }
            auto equal = [&](auto&& self, const ComputedStyle& a, const ComputedStyle& b) -> bool {
                if (a.identity != b.identity || a.local_name != b.local_name || a.values != b.values ||
                    a.custom_properties != b.custom_properties ||
                    a.invalid_at_computed_value != b.invalid_at_computed_value ||
                    a.specified_properties != b.specified_properties || a.children.size() != b.children.size()) {
                    return false;
                }
                for (std::size_t n = 0; n < a.children.size(); ++n) {
                    if (!self(self, a.children[n], b.children[n])) {
                        return false;
                    }
                }
                return true;
            };
            ComputedStyle inplace;
            StyleMemo update_memo;
            unsigned seed = weft::test_constants::style_text::memo_mutation_seed;
            for (unsigned i = 0; i < weft::test_constants::style_text::memo_mutation_iterations; ++i) {
                seed = seed * weft::constants::lcg_multiplier + weft::constants::lcg_increment;
                auto p = seed % 2;
                if (tree.children[p].children.empty()) {
                    p = 1 - p;
                }
                auto& children = tree.children[p].children;
                auto n = (seed >> 8) % children.size();
                switch ((seed >> 16) % weft::test_constants::style_text::memo_mutation_kinds) {
                    case 0:
                        tree.attributes["style"] =
                            (seed & 2) ? "--ink:#abcdef;color:#225588" : "--ink:#334455;color:#abcdef";
                        break;
                    case 1:
                        tree.children[p].attributes["class"] = (seed & 2) ? "container hot" : "container";
                        break;
                    case 2:
                        children[n].attributes["data-tone"] = (seed & 2) ? "gold" : "plain";
                        break;
                    case 3:
                        children[n].attributes["style"] = (seed & 2)
                                                              ? "color:var(--missing,#778899);width:inherit"
                                                              : "--ink:var(--ink);color:var(--ink,#998877);width:14px";
                        break;
                    case 4: {
                        auto moved_node = std::move(children[n]);
                        children.erase(children.begin() + n);
                        tree.children[1 - p].children.push_back(std::move(moved_node));
                        break;
                    }
                    case 5:
                        ++children[n].identity.generation;
                        break;
                }
                auto actual = compiled.compute(tree, memo);
                auto expected = compute_style(tree, sheet, profile);
                require(equal(equal, actual, expected), "512 cached/full cascade mutation differentials");
                if (i % weft::test_constants::style_text::memo_clear_period == 0) {
                    memo.clear();
                }
                const auto prior = inplace;
                std::vector<NodeIdentity> changed;
                compiled.update(tree, inplace, update_memo, &changed);
                require(equal(equal, inplace, expected), "512 in-place/full cascade mutation differentials");
                std::vector<std::array<std::uint64_t, 3>> actual_changes;
                std::vector<std::array<std::uint64_t, 3>> expected_changes;
                for (auto id : changed) {
                    actual_changes.push_back({id.view_epoch, id.slot, id.generation});
                }
                // Match retained output within its parent. A reparented subtree is
                // reconstructed, while unchanged reordered siblings retain values.
                auto collect = [&](auto&& self, const ComputedStyle& current, const ComputedStyle* before) -> void {
                    if (!before || before->identity != current.identity || before->local_name != current.local_name ||
                        before->values != current.values || before->custom_properties != current.custom_properties ||
                        before->invalid_at_computed_value != current.invalid_at_computed_value ||
                        before->specified_properties != current.specified_properties) {
                        expected_changes.push_back(
                            {current.identity.view_epoch, current.identity.slot, current.identity.generation});
                    }
                    for (const auto& c : current.children) {
                        const ComputedStyle* old_child{};
                        if (before && before->identity == current.identity) {
                            for (const auto& candidate : before->children) {
                                if (candidate.identity == c.identity) {
                                    old_child = &candidate;
                                    break;
                                }
                            }
                        }
                        self(self, c, old_child);
                    }
                };
                collect(collect, expected, &prior);
                std::sort(actual_changes.begin(), actual_changes.end());
                std::sort(expected_changes.begin(), expected_changes.end());
                require(actual_changes == expected_changes,
                        "changed identities missed a computed update or invalidated retained unchanged values");
                if (i % weft::test_constants::style_text::sibling_reverse_period == 0) {
                    std::reverse(children.begin(), children.end());
                    changed.clear();
                    compiled.update(tree, inplace, update_memo, &changed);
                    require(changed.empty(), "sibling reorder rebuilt unchanged own styles");
                    require(equal(equal, inplace, compute_style(tree, sheet, profile)), "sibling reorder style values");
                }
            }
            CompiledStyle other(".paint{color:#778899}", profile);
            require(equal(equal, other.compute(tree, memo), compute_style(tree, ".paint{color:#778899}", profile)),
                    "style memo crossed stylesheet identity");
            auto wide = node("wide");
            wide.attributes["style"].clear();
            for (unsigned n = 0; n < weft::constants::style::max_custom_properties; ++n) {
                wide.attributes["style"] += "--v" + std::to_string(n) + ":x x x x x x x x;";
            }
            for (unsigned n = 0; n < weft::test_constants::style_text::quota_children; ++n) {
                auto sample_child = node("unused");
                sample_child.attributes.clear();
                sample_child.identity = {n + 2, 1, weft::test_constants::style_text::quota_view_epoch};
                wide.children.push_back(sample_child);
            }
            CompiledStyle bounded("", profile);
            rejects([&] {
                compute_style(wide, "", profile);
            });
            rejects([&] {
                bounded.compute(wide, memo);
            });
            rejects([&] {
                bounded.update(wide, inplace, update_memo);
            });
            inplace = {};
            compiled.update(tree, inplace, update_memo);
            require(equal(equal, inplace, compute_style(tree, sheet, profile)),
                    "in-place cache did not recover after quota failure");
        }
        FontSet fonts({WEFT_SOURCE_DIR "/third_party/harfbuzz/source/perf/fonts/Roboto-Regular.ttf"});
        auto flex = node("flex");
        flex.children.push_back(node("cell"));
        const auto page =
            layout_document(
                flex,
                "#flex{display:flex;width:200px;height:40px;--w:60px}#cell{display:flex;width:var(--w);height:20px;background-color:rgba(16,18,17,.78)}",
                weft::constants::default_view_width, weft::constants::default_view_height, fonts)
                .page;
        near(page.geometry.boxes[1].width, 60);
        near(page.paints[0].paint.srgb[3], .78f);
        auto paired = node("paired", "width:100px;height:30px");
        auto computed = compute_style(paired, "", profile);
        paired.identity.generation++;
        rejects([&] {
            layout_document(paired, computed, 100, 30, fonts);
        });
        // Same DOM-building JS and CSS in Chromium and QuickJS/native DOM.
        if (argc == 2) {
            ScriptDom script;
            script.eval(read_file(WEFT_SOURCE_DIR "/tests/fixtures/style-cascade/fixture.js"));
            std::filesystem::create_directories(std::filesystem::path(argv[1]).parent_path());
            std::ofstream out(argv[1]);
            out << "{\"fixture\":\"style-cascade-v1\",\"frames\":[";
            for (int frame = 0; frame < weft::test_constants::style_text::cascade_fixture_frames; ++frame) {
                if (frame) {
                    script.dispatch_mouse(script.node_identity(frame == 1 ? "#theme" : "#expand"), {MouseType::Click});
                }
                const auto dom = script.snapshot_subtree("html");
                const auto tree =
                    compute_style(dom, read_file(WEFT_SOURCE_DIR "/tests/fixtures/style-cascade/profile.css"), profile);
                if (frame) {
                    out << ',';
                }
                out << '[';
                const std::vector<std::string> ids{"case-a", "case-b", "case-c", "case-d", "case-e", "case-f",
                                                   "case-g", "case-h", "case-i", "case-j", "case-k", "case-l",
                                                   "case-m", "case-n", "case-o", "case-p"};
                for (std::size_t i = 0; i < ids.size(); ++i) {
                    const auto& s = find(dom, tree, ids[i]);
                    if (i) {
                        out << ',';
                    }
                    const auto c = parse_css_color(s.values.at("color"));
                    out << "{\"id\":\"" << ids[i] << "\",\"color\":[" << c[0] << ',' << c[1] << ',' << c[2] << ','
                        << c[3] << "]}";
                }
                out << ']';
            }
            out << "]}";
        }
        std::cout
            << "PASS shared style: token boundaries, inherited resolution, selected-fallback cycles, IACVT, cascade, selectors, snapshot pairing, Flex and resource bounds\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "FAIL " << e.what() << '\n';
        return 1;
    }
}
