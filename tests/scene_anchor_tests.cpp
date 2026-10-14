#include "base/diagnostic.hpp"
#include "constants/scene.hpp"
#include "scene/scene.hpp"
#include "scene/scene_anchor.hpp"
#include "scene/scene_display_list.hpp"
#include "scene/scene_geometry.hpp"
#include "support/check.hpp"
#include "support/native_scene.hpp"
#include "support/scene_fonts.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <functional>
#include <iostream>
#include <limits>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

// Anchors (step 13.1 of docs/refactoring-plan.md, docs/engine-communication.md section 5) on the CPU reference path:
// the data-anchor grammar and its diagnostics in the solver; the tables a writer hands two readers, whole and latest,
// under a writer thread; where an anchor puts an element, hides it or clamps it into the View; the order of anchored
// siblings; and the display list, hits and geometry of anchored elements: placed beyond their ancestors' transforms and
// clips as the same boxes at those places draw, hidden without an anchor, kept in place while their key is missing
// for data-anchor-missing: stay, never baked into a layer image or cut by a group's composite; and the D2D reference
// raster, which has no anchors, neither drawing nor hitting them.
namespace {
    using weft::SceneAnchor;
    using weft::SceneAnchoring;
    using weft::testing::require;
    using Reader = weft::SceneAnchorChannel::Reader;
    using Written = weft::SceneAnchorWrite;

    weft::AttributeMap attributes(std::initializer_list<std::pair<const char*, const char*>> values) {
        weft::AttributeMap out;
        for (const auto& [name, value] : values) {
            out[name] = value;
        }
        return out;
    }

    std::optional<SceneAnchoring> anchoring(std::initializer_list<std::pair<const char*, const char*>> values) {
        auto parsed = weft::scene_anchoring(attributes(values));
        require(bool(parsed), "valid anchoring refused: " + (parsed ? std::string() : parsed.error().message));
        return *parsed;
    }

    bool refused(std::initializer_list<std::pair<const char*, const char*>> values) {
        const auto parsed = weft::scene_anchoring(attributes(values));
        return !parsed && parsed.error().code == weft::Diag::SceneProfile;
    }

    void grammar() {
        require(!anchoring({{"class", "plate"}}), "anchoring without data-anchor");
        require(!anchoring({{"data-anchor-pivot", "0% 0%"}}), "options without data-anchor anchor nothing");
        const auto defaults = anchoring({{"data-anchor", "42"}});
        require(defaults && defaults->key == 42 && defaults->pivot_fraction == std::array<float, 2>{.5f, .5f} &&
                    defaults->pivot_px == std::array<float, 2>{} && defaults->scale &&
                    defaults->offscreen == SceneAnchoring::Offscreen::Hide && defaults->margin == 0 &&
                    defaults->missing == SceneAnchoring::Missing::Hide,
                "anchoring defaults");
        require(anchoring({{"data-anchor", "18446744073709551615"}})->key == std::numeric_limits<std::uint64_t>::max(),
                "the largest key");
        require(anchoring({{"data-anchor", " 7 "}})->key == 7, "a key between spaces");
        for (const auto* key : {"", " ", "-1", "+1", "1.5", "0x10", "1e3", "18446744073709551616", "1 2", "abc",
                                "000000000000000000001"}) {
            require(refused({{"data-anchor", key}}), std::string("key accepted: ") + key);
        }
        const auto pivot = anchoring({{"data-anchor", "1"}, {"data-anchor-pivot", "0% 100%"}});
        require(pivot->pivot_fraction == std::array<float, 2>{0, 1} && pivot->pivot_px == std::array<float, 2>{},
                "percentage pivot");
        const auto px = anchoring({{"data-anchor", "1"}, {"data-anchor-pivot", " 10px  -4px "}});
        require(px->pivot_fraction == std::array<float, 2>{} && px->pivot_px == std::array<float, 2>{10, -4},
                "px pivot");
        const auto mixed = anchoring({{"data-anchor", "1"}, {"data-anchor-pivot", "0 25%"}});
        require(mixed->pivot_fraction == std::array<float, 2>{0, .25f} && mixed->pivot_px == std::array<float, 2>{},
                "zero and percentage pivot");
        for (const auto* value : {"50%", "1em 2em", "a b", "1% 2% 3%", "10 20", ""}) {
            require(refused({{"data-anchor", "1"}, {"data-anchor-pivot", value}}),
                    std::string("pivot accepted: ") + value);
        }
        require(!anchoring({{"data-anchor", "1"}, {"data-anchor-scale", "off"}})->scale &&
                    anchoring({{"data-anchor", "1"}, {"data-anchor-scale", "on"}})->scale &&
                    refused({{"data-anchor", "1"}, {"data-anchor-scale", "yes"}}),
                "data-anchor-scale");
        require(anchoring({{"data-anchor", "1"}, {"data-anchor-offscreen", "show"}})->offscreen ==
                        SceneAnchoring::Offscreen::Show &&
                    anchoring({{"data-anchor", "1"}, {"data-anchor-offscreen", "clamp"}})->offscreen ==
                        SceneAnchoring::Offscreen::Clamp &&
                    anchoring({{"data-anchor", "1"}, {"data-anchor-offscreen", "hide"}})->offscreen ==
                        SceneAnchoring::Offscreen::Hide &&
                    refused({{"data-anchor", "1"}, {"data-anchor-offscreen", "none"}}),
                "data-anchor-offscreen");
        require(anchoring({{"data-anchor", "1"}, {"data-anchor-margin", "8px"}})->margin == 8 &&
                    anchoring({{"data-anchor", "1"}, {"data-anchor-margin", "0"}})->margin == 0,
                "data-anchor-margin");
        for (const auto* value : {"-1px", "5%", "8", "px"}) {
            require(refused({{"data-anchor", "1"}, {"data-anchor-margin", value}}),
                    std::string("margin accepted: ") + value);
        }
        require(anchoring({{"data-anchor", "1"}, {"data-anchor-missing", "stay"}})->missing ==
                        SceneAnchoring::Missing::Stay &&
                    refused({{"data-anchor", "1"}, {"data-anchor-missing", "keep"}}),
                "data-anchor-missing");
        require(refused({{"data-anchor", "1"}, {"data-anchor-colour", "red"}}), "an unknown data-anchor- attribute");
        require(anchoring({{"data-anchor", "1"}, {"data-anchored", "yes"}, {"data-anchor2", "x"}})->key == 1,
                "attributes that only begin like data-anchor are not options");
    }

    weft::SceneAnchorTable written(const std::vector<SceneAnchor>& anchors) {
        weft::SceneAnchorChannel channel;
        require(channel.write(anchors) == Written::Written, "write");
        return channel.take(Reader::Render);
    }

    void channel() {
        weft::SceneAnchorChannel channel;
        const auto& before = channel.take(Reader::Render);
        require(before.serial() == 0 && before.anchors().empty() && channel.writes() == 0,
                "an empty table before the first write");
        // In key order, the last entry of a key counting.
        require(channel.write(std::vector<SceneAnchor>{{5, 1, 1}, {2, 2, 2}, {5, 3, 3}, {9, 4, 4}}) == Written::Written,
                "write");
        {
            const auto& t = channel.take(Reader::Render);
            require(t.serial() == 1 && t.anchors().size() == 3 && t.anchors()[0].key == 2 && t.anchors()[1].key == 5 &&
                        t.anchors()[2].key == 9 && t.find(5) && t.find(5)->x == 3 && !t.find(7) &&
                        t.anchors()[1].reserved == 0,
                    "a table in key order, the last entry of a key counting");
            // A write leaves a reader's table alone until the reader takes again.
            require(channel.write(std::vector<SceneAnchor>{{1, 10, 10}}) == Written::Written, "second write");
            require(t.serial() == 1 && t.anchors().size() == 3, "a write changed a table its reader holds");
        }
        require(channel.take(Reader::Render).serial() == 2 && channel.take(Reader::Render).find(1)->x == 10,
                "the reader takes the latest table");
        // Each reader takes on its own: the owner had taken nothing and gets the latest.
        require(channel.take(Reader::Owner).serial() == 2, "the owner takes the latest table");
        // Writes that publish nothing.
        const float nan = std::numeric_limits<float>::quiet_NaN();
        const float inf = std::numeric_limits<float>::infinity();
        for (const auto& bad : std::vector<SceneAnchor>{{1, nan, 0},
                                                        {1, 0, inf},
                                                        {1, 0, 0, nan},
                                                        {1, 0, 0, -1},
                                                        {1, 0, 0, 1, inf},
                                                        {1, 0, 0, 1, 0, 8},
                                                        {1, 0, 0, 1, 0, 0, 1}}) {
            require(channel.write(std::vector<SceneAnchor>{{3, 1, 1}, bad}) == Written::Invalid,
                    "an invalid anchor written");
        }
        require(channel.write(std::vector<SceneAnchor>(std::size_t(weft::constants::scene::max_anchors) + 1)) ==
                    Written::TooMany,
                "too many anchors written");
        require(channel.writes() == 2 && channel.take(Reader::Render).serial() == 2 &&
                    channel.take(Reader::Owner).serial() == 2,
                "a refused write published");
        require(channel.write(std::vector<SceneAnchor>{{4, 1, 1, 0}}) == Written::Written,
                "a scale of 0 and the known flags are valid");
        // Many writes between takes: the reader gets the last.
        for (unsigned i = 0; i < 10; ++i) {
            require(channel.write(std::vector<SceneAnchor>{{7, float(i), 0}}) == Written::Written, "write");
        }
        const auto& last = channel.take(Reader::Owner);
        require(last.serial() == 13 && last.anchors().size() == 1 && last.find(7)->x == 9, "the last of many writes");
        require(channel.write({}) == Written::Written && channel.take(Reader::Owner).anchors().empty(),
                "an empty write empties the table");
    }

    // A writer thread replaces the anchors while both readers take them: each table a reader sees is whole (one
    // write's entries, sorted), serials never go back, and the last take sees the last write. The writer writes until
    // each reader has seen many tables (or a bound of writes).
    void stress() {
        weft::SceneAnchorChannel channel;
        constexpr std::uint64_t min_writes = 20000;
        constexpr std::uint64_t max_writes = 20000000;
        constexpr std::uint64_t min_tables = 1000;
        constexpr unsigned period = 97;
        std::atomic<bool> done{false};
        std::atomic<bool> failed{false};
        std::array<std::atomic<std::uint64_t>, 2> taken{};
        std::uint64_t writes{};
        std::thread writer([&] {
            std::vector<SceneAnchor> anchors;
            for (std::uint64_t w = 1; w <= max_writes; ++w) {
                const unsigned count = 1 + unsigned(w % period);
                anchors.clear();
                // In reverse, so that the write sorts them.
                for (unsigned i = 0; i < count; ++i) {
                    const auto key = count - 1 - i;
                    anchors.push_back({key, float(w), -float(w), 1, float(key) * 2});
                }
                if (channel.write(anchors) != Written::Written) {
                    failed = true;
                }
                writes = w;
                if (w >= min_writes && taken[0] >= min_tables && taken[1] >= min_tables) {
                    break;
                }
            }
            done = true;
        });

        struct Seen {
            std::uint64_t last{};
            std::uint64_t tables{};
            std::string failure;
        };

        const auto read = [&](Reader reader, Seen& seen) {
            for (bool finished = false; !finished;) {
                finished = done.load();
                const auto& t = channel.take(reader);
                const auto serial = t.serial();
                if (serial < seen.last) {
                    seen.failure = "a serial went back";
                }
                if (serial != seen.last) {
                    ++seen.tables;
                    taken[unsigned(reader)] = seen.tables;
                }
                seen.last = serial;
                if (serial == 0) {
                    continue;
                }
                const unsigned count = 1 + unsigned(serial % period);
                if (t.anchors().size() != count) {
                    seen.failure = "a table of another write's size";
                    continue;
                }
                for (unsigned i = 0; i < count; ++i) {
                    const auto& a = t.anchors()[i];
                    if (a.key != i || a.x != float(serial) || a.y != -float(serial) || a.depth != float(i) * 2) {
                        seen.failure = "a torn table";
                    }
                }
            }
        };
        Seen render;
        Seen owner;
        std::thread render_reader(read, Reader::Render, std::ref(render));
        std::thread owner_reader(read, Reader::Owner, std::ref(owner));
        writer.join();
        render_reader.join();
        owner_reader.join();
        require(!failed, "a stress write refused");
        for (const auto* seen : {&render, &owner}) {
            require(seen->failure.empty(), seen->failure);
            require(seen->last == writes, "the last take missed the last write");
            require(seen->tables >= min_tables || writes == max_writes, "the readers saw few tables");
        }
        std::cout << "anchor stress: " << writes << " writes, " << render.tables << " render and " << owner.tables
                  << " owner tables\n";
    }

    void maps() {
        const std::array<double, 6> identity{1, 0, 0, 1, 0, 0};
        const std::array<double, 2> view{400, 300};
        const std::array<double, 2> box{40, 20};
        SceneAnchoring a;
        a.key = 1;
        SceneAnchor at{1, 100, 50, 2};
        require(weft::scene_anchor_map(a, &at, box, view, identity) == std::array<double, 6>{2, 0, 0, 2, 60, 30},
                "the pivot on the anchor, scaled about it");
        a.scale = false;
        require(weft::scene_anchor_map(a, &at, box, view, identity) == std::array<double, 6>{1, 0, 0, 1, 80, 40},
                "data-anchor-scale: off");
        a.pivot_fraction = {0, 1};
        a.pivot_px = {4, -2};
        require(weft::scene_anchor_map(a, &at, box, view, identity) == std::array<double, 6>{1, 0, 0, 1, 96, 32},
                "a pivot of fractions and px");
        a = {};
        a.key = 1;
        require(!weft::scene_anchor_map(a, nullptr, box, view, identity), "no anchor hides");
        at = {1, 100, 50, 1, 0, weft::scene_anchor_flags::hidden};
        require(!weft::scene_anchor_map(a, &at, box, view, identity), "a hidden anchor hides");

        // Off the View: outside it (its edges are inside), behind the camera, or flagged off screen.
        struct Case {
            float x{};
            float y{};
            std::uint32_t flags{};
            bool off{};
        };

        for (const auto& [x, y, flags, off] : std::vector<Case>{{-1, 50, 0, true},
                                                                {401, 50, 0, true},
                                                                {100, -0.5f, 0, true},
                                                                {100, 301, 0, true},
                                                                {0, 0, 0, false},
                                                                {400, 300, 0, false},
                                                                {100, 50, weft::scene_anchor_flags::behind, true},
                                                                {100, 50, weft::scene_anchor_flags::offscreen, true}}) {
            at = {1, x, y, 1, 0, flags};
            a.offscreen = SceneAnchoring::Offscreen::Hide;
            require(bool(weft::scene_anchor_map(a, &at, box, view, identity)) == !off, "offscreen: hide");
            a.offscreen = SceneAnchoring::Offscreen::Show;
            require(weft::scene_anchor_map(a, &at, box, view, identity) ==
                        std::array<double, 6>{1, 0, 0, 1, x - 20.0, y - 10.0},
                    "offscreen: show");
        }
        // Clamped into the View inset by the margin, behind the camera too; centred where it cannot fit.
        a.offscreen = SceneAnchoring::Offscreen::Clamp;
        at = {1, -50, 150, 1, 0, weft::scene_anchor_flags::behind};
        require(weft::scene_anchor_map(a, &at, box, view, identity) == std::array<double, 6>{1, 0, 0, 1, 0, 140},
                "clamped to the left edge");
        a.margin = 8;
        require(weft::scene_anchor_map(a, &at, box, view, identity) == std::array<double, 6>{1, 0, 0, 1, 8, 140},
                "clamped to the margin");
        at = {1, 500, 400};
        require(weft::scene_anchor_map(a, &at, box, view, identity) == std::array<double, 6>{1, 0, 0, 1, 352, 272},
                "clamped to the right and bottom edges");
        at = {1, 200, 150};
        require(weft::scene_anchor_map(a, &at, box, view, identity) == std::array<double, 6>{1, 0, 0, 1, 180, 140},
                "a clamped element inside the View stays");
        a.margin = 0;
        require(weft::scene_anchor_map(a, &at, {500, 20}, view, identity) ==
                    std::array<double, 6>{1, 0, 0, 1, -50, 140},
                "centred on an axis it cannot fit");
        // The bounds of the box under its own transform: twice as large about its origin.
        at = {1, -50, 150};
        require(weft::scene_anchor_map(a, &at, box, view, {2, 0, 0, 2, 0, 0}) ==
                    std::array<double, 6>{1, 0, 0, 1, 0, 140},
                "clamped by the bounds of its own transform");
        require(weft::scene_anchor_map(a, &at, box, view, {2, 0, 0, 2, -40, 0}) ==
                    std::array<double, 6>{1, 0, 0, 1, 40, 140},
                "clamped by the bounds of a moved transform");
    }

    void order() {
        // Children 0-4 at z-index 0 (1, 2 and 4 anchored), 5 and 6 at z-index 1 (both anchored).
        const std::array<float, 7> z{0, 0, 0, 0, 0, 1, 1};
        const std::array<std::optional<float>, 7> depth{std::nullopt, 5.f, 9.f, std::nullopt, 5.f, 1.f, 7.f};
        std::vector<std::size_t> order{0, 1, 2, 3, 4, 5, 6};
        weft::scene_anchor_order(
            order,
            [&](std::size_t i) {
                return z[i];
            },
            [&](std::size_t i) {
                return depth[i];
            });
        require(order == std::vector<std::size_t>{0, 2, 1, 3, 4, 6, 5},
                "anchored siblings far to near in their places, equal depths in order");
    }

    void resolution() {
        const auto first = written({{3, 10, 20, 1, 4}});
        const auto second = written({{4, 1, 1}});
        const weft::NodeIdentity element{5, 1, 1};
        SceneAnchoring stay;
        stay.key = 3;
        stay.missing = SceneAnchoring::Missing::Stay;
        std::vector<weft::ScenePlacedAnchor> placed;
        const auto* a = weft::scene_anchor_resolve({&first, nullptr}, element, stay, &placed);
        require(a && a->x == 10 && placed.size() == 1, "the table's anchor taken and recorded");
        weft::scene_anchor_resolve({&first, nullptr}, element, stay, &placed);
        weft::scene_anchor_sort(placed);
        require(placed.size() == 1 && placed[0].element == element && placed[0].anchor.key == 3,
                "one record per element");
        std::vector<weft::ScenePlacedAnchor> next;
        a = weft::scene_anchor_resolve({&second, &placed}, element, stay, &next);
        require(a && a->x == 10 && a->depth == 4 && next.size() == 1, "missing: stay keeps the last anchor");
        require(weft::scene_anchor_resolve({nullptr, &placed}, element, stay), "missing: stay without a table");
        auto hide = stay;
        hide.missing = SceneAnchoring::Missing::Hide;
        require(!weft::scene_anchor_resolve({&second, &placed}, element, hide), "missing: hide");
        auto other = stay;
        other.key = 6;
        require(!weft::scene_anchor_resolve({&second, &placed}, element, other),
                "missing: stay keeps no anchor of another key");
        require(!weft::scene_anchor_resolve({&second, &placed}, {6, 1, 1}, stay),
                "missing: stay keeps no anchor of another element");
    }

    weft::SceneNode node(std::uint32_t slot, std::array<float, 4> box,
                         std::initializer_list<std::pair<const char*, const char*>> style) {
        weft::SceneNode n;
        n.identity = {slot, 1, 1};
        n.tag = "div";
        n.box = box;
        for (const auto& [k, v] : style) {
            n.style[k] = v;
        }
        return n;
    }

    weft::SceneNode anchored(weft::SceneNode n, std::uint64_t key, SceneAnchoring::Missing missing = {}) {
        n.anchoring.emplace();
        n.anchoring->key = key;
        n.anchoring->missing = missing;
        return n;
    }

    constexpr std::array<float, 4> view_clip{0, 0, 400, 300};

    // Plates A (key 1, with a child; missing applies to it) and C (key 3) in a box that moves its content and clips it
    // to rounded corners, and B (key 2), whose anchor is missing; A and C escape the box.
    weft::SceneFrame plates(std::uint64_t c_key = 3, SceneAnchoring::Missing missing = {}, std::uint64_t a_key = 1) {
        weft::SceneFrame f;
        f.root = node(1, {0, 0, 400, 300}, {{"background-color", "#ffffff"}});
        auto box = node(2, {20, 20, 200, 150},
                        {{"background-color", "#eeeeee"},
                         {"overflow", "hidden"},
                         {"border-top-left-radius", "6px"},
                         {"border-top-right-radius", "6px"},
                         {"border-bottom-right-radius", "6px"},
                         {"border-bottom-left-radius", "6px"},
                         {"transform", "translate(5px, 7px)"}});
        auto a = anchored(node(3, {10, 10, 40, 20}, {{"background-color", "#ff0000"}}), a_key, missing);
        a.children.push_back(node(4, {2, 2, 10, 4}, {{"background-color", "#0000ff"}}));
        box.children.push_back(std::move(a));
        box.children.push_back(anchored(node(5, {10, 40, 40, 20}, {{"background-color", "#00ff00"}}), 2));
        box.children.push_back(anchored(node(6, {10, 70, 40, 20}, {{"background-color", "#ffff00"}}), c_key));
        f.root.children.push_back(std::move(box));
        return f;
    }

    // The same boxes where the anchors put them, as CSS places them in the root: C (scale 2 about its corner) first,
    // as it is farther, then A.
    weft::SceneFrame placed_plates(bool c_first = true) {
        weft::SceneFrame f;
        f.root = node(1, {0, 0, 400, 300}, {{"background-color", "#ffffff"}});
        f.root.children.push_back(node(2, {20, 20, 200, 150},
                                       {{"background-color", "#eeeeee"},
                                        {"overflow", "hidden"},
                                        {"border-top-left-radius", "6px"},
                                        {"border-top-right-radius", "6px"},
                                        {"border-bottom-right-radius", "6px"},
                                        {"border-bottom-left-radius", "6px"},
                                        {"transform", "translate(5px, 7px)"}}));
        auto a = node(3, {280, 190, 40, 20}, {{"background-color", "#ff0000"}});
        a.children.push_back(node(4, {2, 2, 10, 4}, {{"background-color", "#0000ff"}}));
        auto c = node(6, {280, 195, 40, 20},
                      {{"background-color", "#ffff00"}, {"transform", "scale(2)"}, {"transform-origin", "0 0"}});
        if (c_first) {
            f.root.children.push_back(std::move(c));
            f.root.children.push_back(std::move(a));
        } else {
            f.root.children.push_back(std::move(a));
            f.root.children.push_back(std::move(c));
        }
        return f;
    }

    // A at (300, 200) near; C at (320, 215), twice as large and farther.
    weft::SceneAnchorTable plate_anchors(float a_depth = 1, float c_depth = 9) {
        return written({{1, 300, 200, 1, a_depth}, {3, 320, 215, 2, c_depth}});
    }

    bool same_items(const weft::paint::DisplayList& a, const weft::paint::DisplayList& b) {
        if (a.items.size() != b.items.size() || a.boxes.size() != b.boxes.size()) {
            return false;
        }
        for (std::size_t i = 0; i < a.items.size(); ++i) {
            const auto& x = a.items[i];
            const auto& y = b.items[i];
            if (x.kind != y.kind || x.transform != y.transform || x.clip != y.clip || x.width != y.width ||
                x.height != y.height || x.opacity != y.opacity || x.clips_count != y.clips_count ||
                (x.kind == weft::paint::Item::Kind::Box &&
                 a.boxes[x.payload].background != b.boxes[y.payload].background)) {
                return false;
            }
        }
        return true;
    }

    std::size_t count(const weft::paint::DisplayList& list, weft::paint::Item::Kind kind) {
        return std::size_t(std::count_if(list.items.begin(), list.items.end(), [&](const auto& item) {
            return item.kind == kind;
        }));
    }

    void lists() {
        const auto table = plate_anchors();
        const auto listed = weft::scene_display_list(plates(), {}, nullptr, 1, nullptr, {&table, nullptr});
        require(listed.fallback.empty(), "anchored list fallback: " + listed.fallback);
        const auto reference = weft::scene_display_list(placed_plates());
        require(same_items(listed.list, reference.list),
                "anchored boxes differ from the same boxes placed there, beyond the box that moves and clips");
        require(listed.placed.size() == 2 && listed.placed[0].element.slot == 3 && listed.placed[1].element.slot == 6,
                "the anchors placed, by element");
        // Depths reversed: C draws last.
        const auto swapped = plate_anchors(9, 1);
        require(same_items(weft::scene_display_list(plates(), {}, nullptr, 1, nullptr, {&swapped, nullptr}).list,
                           weft::scene_display_list(placed_plates(false)).list),
                "anchored siblings ordered by depth");
        // Without a table every anchored element hides.
        const auto none = weft::scene_display_list(plates());
        require(none.fallback.empty() && none.list.items.size() == 2 && none.placed.empty(),
                "anchored elements drawn without anchors");
        // Missing: stay keeps the last anchor; missing: hide does not.
        const auto later = written({{3, 320, 215, 2, 9}});
        const auto stays = weft::scene_display_list(plates(3, SceneAnchoring::Missing::Stay), {}, nullptr, 1, nullptr,
                                                    {&table, nullptr});
        const auto kept = weft::scene_display_list(plates(3, SceneAnchoring::Missing::Stay), {}, nullptr, 1, nullptr,
                                                   {&later, &stays.placed});
        require(same_items(kept.list, reference.list) && kept.placed.size() == 2,
                "missing: stay moved or hid the element");
        const auto hidden = weft::scene_display_list(plates(), {}, nullptr, 1, nullptr, {&later, &listed.placed});
        require(hidden.list.items.size() == 3 && hidden.placed.size() == 1, "missing: hide kept the element");
        // A new key under stay has no last anchor.
        const auto rekeyed = weft::scene_display_list(plates(3, SceneAnchoring::Missing::Stay, 5), {}, nullptr, 1,
                                                      nullptr, {&later, &stays.placed});
        require(rekeyed.list.items.size() == 3, "missing: stay kept another key's anchor");
        // A layer of a subtree holding anchored elements draws in the pass; an anchored layer root moves its image.
        const auto box = plates().root.children[0].identity;
        const auto layered = weft::scene_display_list(plates(), {box}, nullptr, 1, nullptr, {&table, nullptr});
        require(count(layered.list, weft::paint::Item::Kind::Layer) == 0 && same_items(layered.list, listed.list),
                "a layer image holds anchored descendants");
        const auto plate = plates().root.children[0].children[0].identity;
        const auto moved = weft::scene_display_list(plates(), {plate}, nullptr, 1, nullptr, {&table, nullptr});
        const auto layer = std::find_if(moved.list.items.begin(), moved.list.items.end(), [](const auto& item) {
            return item.kind == weft::paint::Item::Kind::Layer;
        });
        require(layer != moved.list.items.end() && layer->transform == weft::paint::Affine{1, 0, 0, 1, 280, 190} &&
                    layer->clip == view_clip && !moved.list.rasters[layer->payload].source->anchoring,
                "an anchored layer root");
        // A group holding an anchored element composites over the View, beyond its clipping ancestor.
        auto grouped = plates();
        auto& clipper = grouped.root.children[0];
        auto translucent = node(8, {0, 0, 200, 150}, {{"opacity", "0.5"}});
        translucent.children = std::move(clipper.children);
        clipper.children = {std::move(translucent)};
        const auto group = weft::scene_display_list(grouped, {}, nullptr, 1, nullptr, {&table, nullptr});
        const auto begin = std::find_if(group.list.items.begin(), group.list.items.end(), [](const auto& item) {
            return item.kind == weft::paint::Item::Kind::BeginGroup;
        });
        require(group.fallback.empty() && begin != group.list.items.end() && begin->clip == view_clip &&
                    begin->clips_count == 0,
                "a group's composite cuts its anchored descendants");
        // What the list cannot draw: an anchored inline box, an anchored perspective plane.
        auto inline_box = plates();
        inline_box.root.children[0].children[0].fragments = {{{0, 0, 40, 20}, true, true}};
        require(weft::scene_display_list(inline_box, {}, nullptr, 1, nullptr, {&table, nullptr}).fallback ==
                    "anchored inline box",
                "anchored inline box");
        auto plane = plates();
        plane.root.children[0].children[0].style["transform"] = "perspective(100px) rotateY(10deg)";
        require(weft::scene_display_list(plane, {}, nullptr, 1, nullptr, {&table, nullptr}).fallback ==
                    "anchored element in a perspective plane",
                "anchored perspective plane");
    }

    void hits() {
        auto& text = weft::testing::thread_scene_text();
        weft::SceneGeometry geometry(text);
        const auto f = plates();
        const auto table = plate_anchors();
        const auto id = [](std::uint32_t slot) {
            return weft::NodeIdentity{slot, 1, 1};
        };
        // A beyond the box's clip, its child on it; A's place in layout and B's (missing) hit the box.
        require(geometry.hit_test(f, 300, 200, &table) == id(3) && geometry.hit_test(f, 285, 194, &table) == id(4),
                "an anchored element missed where its anchor puts it");
        require(geometry.hit_test(f, 45, 47, &table) == id(2) && geometry.hit_test(f, 45, 77, &table) == id(2),
                "an anchored element hit where layout put it");
        // The overlap: the nearer on top, as drawn.
        require(geometry.hit_test(f, 300, 205, &table) == id(3), "the farther plate hit over the nearer");
        const auto swapped = plate_anchors(9, 1);
        require(geometry.hit_test(f, 300, 205, &swapped) == id(6), "the nearer plate under the farther");
        require(geometry.hit_test(f, 350, 230, &table) == id(6), "a scaled anchored element");
        require(geometry.hit_test(f, 300, 200) == id(1), "an anchored element without anchors hit");
        // Geometry reads: rects where the anchors put them, hidden ones empty at the origin.
        const auto elements = geometry.geometry(f, false, &table);
        const auto element = [&](std::uint32_t slot) {
            return *std::find_if(elements.begin(), elements.end(), [&](const auto& e) {
                return e.identity == id(slot);
            });
        };
        require(element(3).rect == std::array<double, 4>{280, 190, 40, 20} &&
                    element(4).rect == std::array<double, 4>{282, 192, 10, 4} &&
                    element(6).rect == std::array<double, 4>{280, 195, 80, 40} &&
                    element(5).rect == std::array<double, 4>{},
                "anchored rects");
        require(element(3).offset_parent == weft::NodeIdentity{} &&
                    element(3).offset == std::array<double, 4>{280, 190, 40, 20} && element(4).offset_parent == id(3) &&
                    element(4).offset[0] == 2 && element(4).offset[1] == 2,
                "anchored offsets");
        // Missing: stay in hits, from the geometry's last call; (300, 192) is in A alone.
        const auto stay = plates(3, SceneAnchoring::Missing::Stay);
        require(geometry.hit_test(stay, 300, 192, &table) == id(3), "stay hit");
        const auto later = written({{3, 320, 215, 2, 9}});
        require(geometry.hit_test(stay, 300, 192, &later) == id(3), "missing: stay moved the hit");
        require(geometry.hit_test(f, 300, 192, &later) == id(1), "missing: hide still hit");
        // A hit keeps what it placed for the next: A moved to (100, 100) stays there while its key is missing.
        const auto moved = written({{1, 100, 100, 1, 1}, {3, 320, 215, 2, 9}});
        require(geometry.hit_test(stay, 100, 92, &moved) == id(3), "a moved hit");
        require(geometry.hit_test(stay, 100, 92, &later) == id(3), "missing: stay kept an older anchor");
    }

    // The D2D reference raster has no anchors: anchored elements neither draw nor take hits there.
    void reference() {
        weft::SceneRaster raster(weft::testing::thread_scene_text());
        const auto f = plates();
        const auto bitmap = raster.raster(f, 1);
        const auto* pixel = bitmap.rgba.data() + (std::size_t(47) * bitmap.width + 45) * 4;
        // A's place in layout shows the box beneath it.
        require(pixel[0] == 0xee && pixel[1] == 0xee && pixel[2] == 0xee,
                "the reference raster drew an anchored element");
        require(raster.hit_test(f, 45, 47) == weft::NodeIdentity{2, 1, 1}, "the reference hit an anchored element");
    }

    void equality() {
        auto a = node(1, {0, 0, 10, 10}, {});
        auto b = a;
        b.anchoring.emplace();
        require(!(a == b) && !weft::scene_geometry_own_equal(a, b), "nodes equal but for anchoring compare equal");
        a.anchoring.emplace();
        b.anchoring->pivot_fraction = {0, 0};
        require(!(a == b) && !weft::scene_geometry_own_equal(a, b), "nodes of other pivots compare equal");
        b.anchoring = a.anchoring;
        require(a == b && weft::scene_geometry_own_equal(a, b), "equal anchoring");
    }

    void solver() {
        weft::testing::NativeScene s;
        s.set(*s.root, "style", "position:relative;width:400px;height:300px");
        auto& plate = s.html("div");
        s.set(plate, "style", "position:absolute;left:10px;top:10px;width:40px;height:20px");
        s.set(plate, "data-anchor", "7");
        s.set(plate, "data-anchor-pivot", "0% 100%");
        s.set(plate, "data-anchor-missing", "stay");
        s.tree.insert(*s.root, plate, nullptr);
        auto& svg = s.svg("svg");
        s.set(svg, "width", "20");
        s.set(svg, "height", "20");
        s.tree.insert(*s.root, svg, nullptr);
        auto& rect = s.svg("rect");
        s.set(rect, "width", "10");
        s.set(rect, "height", "10");
        s.tree.insert(svg, rect, nullptr);
        weft::SceneSolver scene("", "", weft::testing::thread_scene_text());
        const auto f = scene.frame(s.tree.snapshot(*s.root), 400, 300, 0);
        const auto& a = f.root.children[0].anchoring;
        require(a && a->key == 7 && a->pivot_fraction == std::array<float, 2>{0, 1} &&
                    a->missing == SceneAnchoring::Missing::Stay && !f.root.children[1].anchoring,
                "the solver's anchoring");
        // A new pivot changes where the element lies: the update says the geometry changed.
        auto updated = f;
        s.set(plate, "data-anchor-pivot", "50% 50%");
        scene.update(s.tree.snapshot(*s.root), updated, 400, 300, 0);
        require(updated.root.children[0].anchoring->pivot_fraction == std::array<float, 2>{.5f, .5f} &&
                    scene.work().geometry_changed,
                "a pivot edit");
        s.set(plate, "data-anchor-pivot", "0% 100%");
        const auto rejects = [&](const char* what) {
            try {
                scene.reset();
                (void)scene.frame(s.tree.snapshot(*s.root), 400, 300, 0);
            } catch (const weft::DiagnosticError& e) {
                require(e.code() == weft::Diag::SceneProfile, std::string(what) + ": " + e.what());
                return;
            }
            require(false, std::string("solver accepted ") + what);
        };
        s.set(plate, "data-anchor", "seven");
        rejects("a key that is not decimal");
        s.set(plate, "data-anchor", "7");
        s.set(rect, "data-anchor", "8");
        rejects("data-anchor on an SVG element inside svg");
        s.set(svg, "data-anchor", "8");
        auto& attributes = rect.attributes;
        attributes.erase(std::remove_if(attributes.begin(), attributes.end(),
                                        [](const auto& attribute) {
                                            return attribute.name == "data-anchor";
                                        }),
                         attributes.end());
        s.tree.changed(rect);
        scene.reset();
        const auto svg_root = scene.frame(s.tree.snapshot(*s.root), 400, 300, 0);
        require(svg_root.root.children[1].anchoring && svg_root.root.children[1].anchoring->key == 8,
                "an anchored svg root");
    }
} // namespace

int main() {
    try {
        require(SUCCEEDED(CoInitializeEx(nullptr, COINIT_MULTITHREADED)), "COM init");
        grammar();
        channel();
        stress();
        maps();
        order();
        resolution();
        lists();
        hits();
        reference();
        equality();
        solver();
        std::cout << "PASS scene anchors\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "FAIL " << e.what() << '\n';
        return 1;
    }
}
