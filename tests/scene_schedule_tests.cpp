#include "scene/scene_schedule.hpp"
#include "support/check.hpp"
#include "test_constants/render.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <limits>
#include <map>
#include <memory>
#include <optional>
#include <random>
#include <stdexcept>
#include <string>
#include <variant>
#include <vector>

// The commands that record a prepared scene pass (scene/scene_schedule.hpp), which Vulkan and D3D12 run and Metal is to
// run: on passes built by hand, the exact commands of plain draws, nested groups, backdrops over a target and beneath
// a group, a target the pass may not read, effects, particles and host textures, and the copies effects that read their
// backdrop sample; on random nestings of all of them, the rules a backend with one target at a time relies on
// (render_scene_* and render_*_view_contract draw what they record).
namespace {
#define CHECK(v) WEFT_REQUIRE(v)

    using weft::SceneSchedule;
    using weft::testing::require;
    using Step = weft::render::ScenePass::Step;
    using Kind = SceneSchedule::Target::Kind;
    namespace limits = weft::test_constants::render;

    constexpr unsigned width = 64;
    constexpr unsigned height = 48;

    weft::ScenePrepared pass_of(std::size_t records) {
        weft::ScenePrepared p;
        p.width = width;
        p.height = height;
        p.pass.records.resize(records);
        p.images.resize(records);
        p.planes.resize(records);
        p.layers.resize(records);
        p.filters.resize(records);
        p.textures.resize(records);
        p.pass.clear = {0, 0, 0, 1};
        return p;
    }

    std::shared_ptr<const weft::SceneImage> image() {
        return std::make_shared<weft::SceneImage>();
    }

    weft::render::ScenePass::Backdrop backdrop(std::array<std::int32_t, 4> region, bool under = false,
                                               std::array<unsigned, 2> size = {}, std::array<float, 2> sigma = {},
                                               bool matrix = false) {
        weft::render::ScenePass::Backdrop b;
        b.region = region;
        b.size =
            size[0] ? size : std::array<unsigned, 2>{unsigned(region[2] - region[0]), unsigned(region[3] - region[1])};
        b.sigma = sigma;
        b.under = under;
        if (matrix) {
            b.pre[0] = .5f;
        }
        return b;
    }

    std::string target(const SceneSchedule::Target& t) {
        switch (t.kind) {
            case Kind::Group:
                return "group " + std::to_string(t.index);
            case Kind::Chain:
                return "chain " + std::to_string(t.index) + "." + std::to_string(t.image);
            case Kind::Out:
                break;
        }
        return "out";
    }

    // The commands as text, "; " between them.
    std::string text(const SceneSchedule& s) {
        std::string out;
        for (const auto& command : s.commands) {
            if (!out.empty()) {
                out += "; ";
            }
            if (const auto* b = std::get_if<SceneSchedule::Begin>(&command)) {
                out += "begin " + target(b->target) + (b->clear ? " clear" : "");
            } else if (const auto* e = std::get_if<SceneSchedule::End>(&command)) {
                out += "end " + target(e->target) + (e->sampled ? " sampled" : "");
            } else if (const auto* d = std::get_if<SceneSchedule::Draw>(&command)) {
                static const char* const pipelines[]{"scene", "under", "particles", "effect", "blend"};
                static const char* const images[]{"blank", "item", "page", "group", "backdrop"};
                out += "draw " + std::to_string(d->first) + " " + std::to_string(d->count) + "x" +
                       std::to_string(d->vertices) + " " + pipelines[int(d->pipeline)] + " " +
                       images[int(d->image.kind)];
                if (d->image.kind != SceneSchedule::Image::Kind::Blank) {
                    out += " " + std::to_string(d->image.index);
                }
                if (d->extra == SceneSchedule::Extra::Effect) {
                    out += " effect " + std::to_string(d->source);
                } else if (d->extra == SceneSchedule::Extra::Sprite) {
                    out += " sprite " + std::to_string(d->source);
                }
                if (d->backdrop) {
                    out += " over " + std::to_string(*d->backdrop);
                }
            } else if (const auto* c = std::get_if<SceneSchedule::Copy>(&command)) {
                out += "copy " + target(c->from) + " " + std::to_string(c->region[0]) + "," +
                       std::to_string(c->region[1]) + "," + std::to_string(c->region[2]) + "," +
                       std::to_string(c->region[3]) + " chain " + std::to_string(c->chain);
            } else if (const auto* f = std::get_if<SceneSchedule::Filter>(&command)) {
                out += "filter " + std::to_string(f->chain) + "." + std::to_string(f->pass);
            }
        }
        return out;
    }

    void expect(const SceneSchedule& s, const std::string& want, const std::string& what) {
        if (text(s) != want) {
            std::cerr << what << ":\n  got  " << text(s) << "\n  want " << want << '\n';
            require(false, what);
        }
    }

    // Boxes, an image, a path with nothing in its clip, a plane, a layer, a filtered element and an atlas run, over a
    // target the pass replaces or draws over.
    void PlainDraws() {
        auto p = pass_of(9);
        p.images[2] = image();
        p.planes[4] = std::make_unique<weft::ScenePrepared>();
        p.layers[5] = std::make_shared<weft::SceneLayerImage>();
        p.filters[8] = std::make_shared<weft::SceneFilterImage>();
        p.pages.push_back({weft::SceneImage::Format::Data, 0});
        p.pass.batches = {{Step::Boxes, 0, 2, 0}, {Step::Image, 2, 1, 0}, {Step::Image, 3, 1, 0},
                          {Step::Image, 4, 1, 0}, {Step::Image, 5, 1, 0}, {Step::Atlas, 6, 2, 0},
                          {Step::Image, 8, 1, 0}};
        const std::string draws = "draw 0 2x6 scene blank; draw 2 1x6 scene item 2; draw 4 1x6 scene item 4; "
                                  "draw 5 1x6 scene item 5; draw 6 2x6 scene page 0; draw 8 1x6 scene item 8; end out";
        expect(weft::schedule_scene_pass(p), "begin out clear; " + draws, "a pass that replaces its target");
        const auto replaced = weft::schedule_scene_pass(p);
        CHECK(std::get<SceneSchedule::Begin>(replaced.commands.front()).clear == p.pass.clear);
        p.over = true;
        expect(weft::schedule_scene_pass(p), "begin out; " + draws, "a pass over its target, kept");
        const weft::paint::Color host{.25f, 0, 0, .5f};
        const auto cleared = weft::schedule_scene_pass(p, {}, &host);
        expect(cleared, "begin out clear; " + draws, "a pass over its target, cleared by the host");
        CHECK(std::get<SceneSchedule::Begin>(cleared.commands.front()).clear == host);
        CHECK(cleared.groups == 0 && cleared.chains.empty() && cleared.backdrops == 0);
    }

    // Groups two deep: each begun cleared, ended sampled, and composited into the target below it.
    void NestedGroups() {
        auto p = pass_of(5);
        p.pass.group_depth = 2;
        p.pass.batches = {{Step::BeginGroup},     {Step::Boxes, 0, 1, 0},    {Step::BeginGroup},
                          {Step::Boxes, 1, 2, 0}, {Step::EndGroup, 3, 1, 0}, {Step::EndGroup, 4, 1, 0}};
        const auto s = weft::schedule_scene_pass(p);
        expect(s,
               "begin out clear; end out; begin group 0 clear; draw 0 1x6 scene blank; end group 0; "
               "begin group 1 clear; draw 1 2x6 scene blank; end group 1 sampled; begin group 0; "
               "draw 3 1x6 scene group 1; end group 0 sampled; begin out; draw 4 1x6 scene group 0; end out",
               "nested groups");
        CHECK(s.groups == 2);
        CHECK(std::get<SceneSchedule::Begin>(s.commands[2]).clear == weft::paint::Color{});
    }

    // A backdrop over the target, filtered (a matrix, an area downsample on both axes, then the blur), and backdrops
    // beneath a group's content: reading the target outside the group, or the group enclosing it.
    void Backdrops() {
        auto p = pass_of(6);
        p.pass.group_depth = 2;
        p.pass.backdrops = {backdrop({10, 8, 50, 40}, false, {20, 16}, {4, 4}, true), backdrop({0, 0, 30, 20}, true),
                            backdrop({4, 4, 20, 12}, true, {}, {2, 1})};
        p.pass.batches = {{Step::Backdrop, 0, 1, 0}, {Step::BeginGroup},        {Step::Backdrop, 1, 1, 1},
                          {Step::BeginGroup},        {Step::Backdrop, 2, 1, 2}, {Step::EndGroup, 3, 1, 0},
                          {Step::EndGroup, 4, 1, 0}};
        const auto s = weft::schedule_scene_pass(p);
        expect(s,
               "begin out clear; end out; copy out 10,8,50,40 chain 0; filter 0.0; filter 0.1; filter 0.2; "
               "filter 0.3; filter 0.4; begin out; draw 0 1x6 scene backdrop 0; end out; begin group 0 clear; "
               "end group 0; copy out 0,0,30,20 chain 1; begin group 0; draw 1 1x6 under backdrop 1; end group 0; "
               "begin group 1 clear; end group 1; copy group 0 4,4,20,12 chain 2; filter 2.0; filter 2.1; "
               "begin group 1; draw 2 1x6 under backdrop 2; end group 1 sampled; begin group 0; "
               "draw 3 1x6 scene group 1; end group 0 sampled; begin out; draw 4 1x6 scene group 0; end out",
               "backdrops over the target and beneath groups");
        require(s.chains.size() == 3 && s.backdrops == 3 && s.unreadable_backdrops == 0, "three chains");
        // The first: the copy, then the matrix at its size, the area passes across then down, and the blur's two at
        // the downsampled size; it reads the pass's own target.
        const auto& first = s.chains[0];
        using Size = std::array<std::uint32_t, 2>;
        CHECK((first.images == std::vector<Size>{{40, 32}, {40, 32}, {20, 32}, {20, 16}, {20, 16}, {20, 16}}));
        CHECK(first.passes.size() == 5 && first.passes[0].first == weft::render::BackdropFilter::Matrix &&
              first.passes[1].first == weft::render::BackdropFilter::Area &&
              first.passes[2].first == weft::render::BackdropFilter::Area &&
              first.passes[3].first == weft::render::BackdropFilter::Blur &&
              first.passes[4].first == weft::render::BackdropFilter::Blur);
        CHECK(first.from_out && !first.unreadable);
        // Unfiltered, the copy is what the backdrop draws; inside two groups the copy reads a group.
        CHECK(s.chains[1].images.size() == 1 && s.chains[1].passes.empty() && s.chains[1].from_out);
        CHECK(s.chains[2].images.size() == 3 && !s.chains[2].from_out);
        // The constants are the shared ones.
        CHECK(first.passes[3].second.texel == weft::render::backdrop_blur_constants(4, true, 20, 16).texel);
        // A backdrop beneath a group outside every group is refused.
        auto outside = pass_of(1);
        outside.pass.backdrops = {backdrop({0, 0, 8, 8}, true)};
        outside.pass.batches = {{Step::Backdrop, 0, 1, 0}};
        bool refused = false;
        try {
            (void)weft::schedule_scene_pass(outside);
        } catch (const std::runtime_error&) {
            refused = true;
        }
        require(refused, "a backdrop beneath a group outside every group");
    }

    // A target the pass may not read (a host's without an image to copy): a backdrop reading it is a cleared image,
    // one that reads a group copies the group.
    void UnreadableTarget() {
        auto p = pass_of(3);
        p.pass.group_depth = 1;
        p.pass.backdrops = {backdrop({0, 0, 16, 8}, false, {}, {3, 3}), backdrop({0, 0, 16, 8})};
        p.pass.batches = {
            {Step::Backdrop, 0, 1, 0}, {Step::BeginGroup}, {Step::Backdrop, 1, 1, 1}, {Step::EndGroup, 2, 1, 0}};
        const auto s = weft::schedule_scene_pass(p, {}, nullptr, false);
        expect(s,
               "begin out clear; end out; begin chain 0.2 clear; end chain 0.2 sampled; begin out; "
               "draw 0 1x6 scene backdrop 0; end out; begin group 0 clear; end group 0; copy group 0 0,0,16,8 chain 1; "
               "begin group 0; draw 1 1x6 scene backdrop 1; end group 0 sampled; begin out; "
               "draw 2 1x6 scene group 0; end out",
               "a target the pass may not read");
        CHECK(s.unreadable_backdrops == 1 && s.chains[0].unreadable && !s.chains[1].unreadable);
        CHECK(std::get<SceneSchedule::Begin>(s.commands[2]).clear == weft::paint::Color{});
    }

    // Effects with their program and textures, particles with their sprite, and host textures bound by name.
    void EffectsParticlesTextures() {
        auto p = pass_of(7);
        p.effects.resize(2);
        p.sprites = {nullptr, image()};
        p.texture_names = {"minimap", "missing", "blank"};
        p.textures[3] = 1;
        p.textures[4] = 2;
        p.textures[6] = 3;
        p.pass.batches = {{Step::Effect, 0, 2, 1}, {Step::Particles, 2, 40, 1}, {Step::Image, 3, 1, 0},
                          {Step::Image, 4, 1, 0},  {Step::Particles, 5, 3, 0},  {Step::Image, 6, 1, 0}};
        // A texture bound without a view counts as bound and draws nothing.
        const std::vector<weft::SceneTexture> host{{"minimap", 7, false}, {"blank", 0, false}};
        const auto s = weft::schedule_scene_pass(p, host);
        expect(s,
               "begin out clear; draw 0 2x6 effect blank effect 1; draw 2 1x240 particles blank sprite 1; "
               "draw 3 1x6 scene item 3; draw 5 1x18 particles blank sprite 0; end out",
               "effects, particles and host textures");
        require(s.textures.size() == 7 && s.textures[3] == &host[0] && !s.textures[4] && s.textures[6] == &host[1],
                "the bound host textures");
        CHECK(s.bound_textures == 2 && s.unbound_textures == 1);
    }

    // Effects that read their backdrop: each samples the copy its Copy step took of the current target (the pass's own,
    // or a group's) as its image, a chain of the copy alone; of a target the pass may not read, a cleared image. An
    // effect reading a copy no earlier step took is refused.
    void EffectCopies() {
        auto p = pass_of(4);
        p.pass.group_depth = 1;
        p.effects.resize(2);
        p.pass.copies = {{{2, 3, 18, 11}}, {{0, 0, 8, 8}}};
        weft::render::ScenePass::Batch reads_first{Step::Effect, 1, 1, 1};
        reads_first.copy = 1;
        weft::render::ScenePass::Batch reads_second{Step::Effect, 2, 1, 0};
        reads_second.copy = 2;
        p.pass.batches = {{Step::Boxes, 0, 1, 0}, {Step::Copy, 1, 0, 0}, reads_first, {Step::BeginGroup},
                          {Step::Copy, 2, 0, 1},  reads_second,          {Step::EndGroup, 3, 1, 0}};
        const std::string inside = "begin group 0 clear; end group 0; copy group 0 0,0,8,8 chain 1; begin group 0; "
                                   "draw 2 1x6 effect backdrop 1 effect 0; end group 0 sampled; begin out; "
                                   "draw 3 1x6 scene group 0; end out";
        const auto s = weft::schedule_scene_pass(p);
        expect(s,
               "begin out clear; draw 0 1x6 scene blank; end out; copy out 2,3,18,11 chain 0; begin out; "
               "draw 1 1x6 effect backdrop 0 effect 1; end out; " +
                   inside,
               "effects that read copies of the target and of a group");
        using Size = std::array<std::uint32_t, 2>;
        require(s.chains.size() == 2 && s.copies == 2 && s.unreadable_copies == 0 && s.backdrops == 0,
                "a chain per copy, no backdrops");
        CHECK((s.chains[0].images == std::vector<Size>{{16, 8}}) && s.chains[0].passes.empty() &&
              s.chains[0].from_out && !s.chains[0].unreadable);
        CHECK((s.chains[1].images == std::vector<Size>{{8, 8}}) && !s.chains[1].from_out);
        // The pass's own target unreadable: the first copy is a cleared image, the group's is copied.
        const auto unreadable = weft::schedule_scene_pass(p, {}, nullptr, false);
        expect(unreadable,
               "begin out clear; draw 0 1x6 scene blank; end out; begin chain 0.0 clear; end chain 0.0 sampled; "
               "begin out; draw 1 1x6 effect backdrop 0 effect 1; end out; " +
                   inside,
               "an effect reading a copy of a target the pass may not read");
        CHECK(unreadable.copies == 2 && unreadable.unreadable_copies == 1 && unreadable.unreadable_backdrops == 0);
        // An effect before the Copy step it reads.
        auto early = pass_of(2);
        early.effects.resize(1);
        early.pass.copies = {{{0, 0, 4, 4}}};
        weft::render::ScenePass::Batch reads{Step::Effect, 0, 1, 0};
        reads.copy = 1;
        early.pass.batches = {reads, {Step::Copy, 1, 0, 0}};
        bool refused = false;
        try {
            (void)weft::schedule_scene_pass(early);
        } catch (const std::runtime_error&) {
            refused = true;
        }
        require(refused, "an effect reading a copy no earlier step took");
    }

    bool samples(const weft::ScenePrepared& p, const SceneSchedule& s, std::size_t i) {
        return p.planes[i] || p.layers[i] || p.filters[i] || p.images[i] || (s.textures[i] && s.textures[i]->view);
    }

    // A random pass of up to `batches` steps: boxes, images of every kind (and none), atlas runs, groups up to three
    // deep, effects, particles, backdrops of random regions and filters over the target or beneath their group, and
    // copies of random regions of the current target that an effect then reads.
    // hoisting: groups that composite over random regions of a 640 x 480 pass, some of them blending (a copy of their
    // backdrop).
    weft::ScenePrepared random_pass(std::mt19937& random, bool hoisting = false) {
        const auto below = [&](unsigned n) {
            return unsigned(random() % n);
        };
        std::vector<weft::render::ScenePass::Batch> batches;
        std::vector<weft::render::ScenePass::Backdrop> backdrops;
        std::vector<weft::render::ScenePass::Copy> copies;
        std::vector<char> kinds;
        std::uint32_t first{};
        std::size_t depth{};
        std::size_t deepest{};
        const auto take = [&](std::uint32_t count, char kind = '-') {
            const auto at = first;
            first += count;
            kinds.insert(kinds.end(), count, '-');
            kinds[at] = kind;
            return at;
        };
        const int steps = 1 + int(below(unsigned(limits::schedule_batches)));
        for (int b = 0; b < steps; ++b) {
            switch (below(9)) {
                case 0: {
                    const auto count = 1 + below(3);
                    batches.push_back({Step::Boxes, take(count), count, 0});
                    break;
                }
                case 1: {
                    // An image, a plane, a layer, a filtered element, a bound or an unbound host texture, or a path
                    // with nothing to draw.
                    static const char image_kinds[]{'i', 'p', 'l', 'f', 't', 'u', '-'};
                    batches.push_back({Step::Image, take(1, image_kinds[below(7)]), 1, 0});
                    break;
                }
                case 2: {
                    const auto count = 1 + below(3);
                    batches.push_back({Step::Atlas, take(count), count, below(2)});
                    break;
                }
                case 3:
                    if (depth < 3) {
                        batches.push_back({Step::BeginGroup});
                        deepest = std::max(deepest, ++depth);
                    }
                    break;
                case 4:
                    if (depth) {
                        --depth;
                        batches.push_back({Step::EndGroup, take(1), 1, 0});
                        if (hoisting && below(3) == 0) {
                            copies.push_back({{0, 0, 1, 1}});
                            batches.back().copy = std::uint32_t(copies.size());
                        }
                    }
                    break;
                case 5: {
                    const auto count = 1 + below(2);
                    batches.push_back({Step::Effect, take(count), count, below(3)});
                    break;
                }
                case 6:
                    batches.push_back({Step::Particles, take(1), 1 + below(50), below(2)});
                    break;
                case 7: {
                    const std::int32_t x = std::int32_t(below(width - 8));
                    const std::int32_t y = std::int32_t(below(height - 8));
                    const auto w = 1 + below(width - unsigned(x));
                    const auto h = 1 + below(height - unsigned(y));
                    const bool blur = below(2) != 0;
                    const auto s = 1 + float(below(4));
                    backdrops.push_back(backdrop({x, y, x + std::int32_t(w), y + std::int32_t(h)},
                                                 depth && below(2) != 0, {1 + below(w), 1 + below(h)},
                                                 blur ? std::array<float, 2>{s, s} : std::array<float, 2>{},
                                                 below(3) == 0));
                    batches.push_back({Step::Backdrop, take(1), 1, std::uint32_t(backdrops.size() - 1)});
                    break;
                }
                case 8: {
                    // A copy of the current target, then the effect that reads it.
                    const std::int32_t x = std::int32_t(below(width - 8));
                    const std::int32_t y = std::int32_t(below(height - 8));
                    const auto w = std::int32_t(1 + below(width - unsigned(x)));
                    const auto h = std::int32_t(1 + below(height - unsigned(y)));
                    copies.push_back({{x, y, x + w, y + h}});
                    const auto at = take(1);
                    batches.push_back({Step::Copy, at, 0, std::uint32_t(copies.size() - 1)});
                    weft::render::ScenePass::Batch effect{Step::Effect, at, 1, below(3)};
                    effect.copy = std::uint32_t(copies.size());
                    batches.push_back(effect);
                    break;
                }
            }
        }
        while (depth) {
            --depth;
            batches.push_back({Step::EndGroup, take(1), 1, 0});
        }
        auto p = pass_of(first);
        if (hoisting) {
            // Regions within the pass (none for a quarter of the groups), the copies of blending groups the same.
            p.width = 640;
            p.height = 480;
            for (auto& batch : batches) {
                if (batch.step != Step::EndGroup || below(4) == 0) {
                    continue;
                }
                const auto x = below(600);
                const auto y = below(440);
                const auto w = 1 + below(640 - x);
                const auto h = 1 + below(480 - y);
                p.pass.records[batch.first].outline = {float(x), float(y), float(w), float(h)};
                if (batch.copy) {
                    copies[batch.copy - 1].region = {std::int32_t(x), std::int32_t(y), std::int32_t(x + w),
                                                     std::int32_t(y + h)};
                }
            }
        }
        p.pass.batches = std::move(batches);
        p.pass.backdrops = std::move(backdrops);
        p.pass.copies = std::move(copies);
        p.pass.group_depth = deepest;
        p.pages.resize(2);
        p.effects.resize(3);
        p.sprites = {nullptr, image()};
        p.texture_names = {"bound", "unbound"};
        for (std::size_t i = 0; i < kinds.size(); ++i) {
            switch (kinds[i]) {
                case 'i':
                    p.images[i] = image();
                    break;
                case 'p':
                    p.planes[i] = std::make_unique<weft::ScenePrepared>();
                    break;
                case 'l':
                    p.layers[i] = std::make_shared<weft::SceneLayerImage>();
                    break;
                case 'f':
                    p.filters[i] = std::make_shared<weft::SceneFilterImage>();
                    break;
                case 't':
                    p.textures[i] = 1;
                    break;
                case 'u':
                    p.textures[i] = 2;
                    break;
                default:
                    break;
            }
        }
        p.over = below(2) != 0;
        return p;
    }

    // Runs the commands as a backend with one target at a time does, failing where it could not: a draw outside the
    // current target, a copy or filter inside one, a group or chain image sampled before its drawing ended, a group
    // begun out of its nesting, a backdrop reading another target than its batch's, a draw out of batch order.
    void run(const weft::ScenePrepared& p, const SceneSchedule& s, bool out_readable, const std::string& what) {
        const auto fail = [&](const std::string& why) {
            std::cerr << what << ": " << why << "\n  " << text(s) << '\n';
            require(false, what + ": " + why);
        };
        const SceneSchedule::Target out{Kind::Out};
        std::optional<SceneSchedule::Target> open;
        // The targets the batches nest: the pass's own, then each open group's.
        std::vector<SceneSchedule::Target> stack;
        // Groups and chain images whose drawing ended to be sampled, until they are drawn into again.
        std::vector<SceneSchedule::Target> readable;
        const auto is_readable = [&](const SceneSchedule::Target& t) {
            return std::find(readable.begin(), readable.end(), t) != readable.end();
        };
        // Per chain: copied, filters run, cleared instead.
        std::vector<int> copied(s.chains.size());
        std::vector<std::size_t> filtered(s.chains.size());
        std::vector<int> cleared(s.chains.size());
        // The batches that draw, in order; and per chain, the Backdrop or Copy step that took it.
        std::vector<const weft::render::ScenePass::Batch*> drawing;
        std::vector<const weft::render::ScenePass::Batch*> takers;
        // Per ScenePass::copies entry, its chain once taken.
        std::vector<std::optional<std::uint32_t>> copy_chains(p.pass.copies.size());
        std::size_t reads_out{};
        for (const auto& batch : p.pass.batches) {
            if (batch.step == Step::Backdrop || batch.step == Step::Copy) {
                takers.push_back(&batch);
            }
            if (batch.step == Step::BeginGroup || batch.step == Step::Copy ||
                (batch.step == Step::Image && !samples(p, s, batch.first))) {
                continue;
            }
            drawing.push_back(&batch);
        }
        if (takers.size() != s.chains.size()) {
            fail("a chain per backdrop and copy");
        }
        // Chains are taken in step order; a draw sampling one before its copy or clear fails below.
        for (std::uint32_t k = 0; k < takers.size(); ++k) {
            if (takers[k]->step == Step::Copy) {
                copy_chains.at(takers[k]->source) = k;
            }
        }
        std::size_t next_draw{};
        for (std::size_t c = 0; c < s.commands.size(); ++c) {
            const auto& command = s.commands[c];
            if (const auto* b = std::get_if<SceneSchedule::Begin>(&command)) {
                if (open) {
                    fail("a target begun inside another");
                }
                open = b->target;
                std::erase(readable, b->target);
                if (b->target.kind == Kind::Chain) {
                    const auto& chain = s.chains.at(b->target.index);
                    if (!chain.unreadable || b->target.image + 1 != chain.images.size() || !b->clear) {
                        fail("a chain image begun but the cleared last of an unreadable backdrop");
                    }
                } else if (stack.empty()) {
                    if (b->target != out || c != 0) {
                        fail("the pass does not begin on its own target");
                    }
                    stack.push_back(out);
                } else if (b->target.kind == Kind::Group && b->clear) {
                    if (b->target.index + 1 != stack.size() || *b->clear != weft::paint::Color{}) {
                        fail("a group begun out of its nesting or not transparent");
                    }
                    stack.push_back(b->target);
                } else if (b->target != stack.back() || b->clear) {
                    fail("a target resumed that is not the current one, or cleared");
                }
            } else if (const auto* e = std::get_if<SceneSchedule::End>(&command)) {
                if (!open || *open != e->target) {
                    fail("an end of a target not begun");
                }
                open.reset();
                if (e->sampled) {
                    readable.push_back(e->target);
                    if (e->target.kind == Kind::Group) {
                        if (stack.back() != e->target) {
                            fail("a group finished out of its nesting");
                        }
                        stack.pop_back();
                    } else if (e->target.kind == Kind::Chain) {
                        cleared.at(e->target.index) = 1;
                    }
                }
            } else if (const auto* d = std::get_if<SceneSchedule::Draw>(&command)) {
                if (!open || *open != stack.back()) {
                    fail("a draw outside the current target");
                }
                if (next_draw >= drawing.size() || drawing[next_draw]->first != d->first) {
                    fail("a draw out of batch order");
                }
                const auto& batch = *drawing[next_draw++];
                // An effect that reads its backdrop samples its copy's chain; no other draw samples a copy's.
                if (batch.step == Step::Effect && batch.copy) {
                    const auto taken = copy_chains.at(batch.copy - 1);
                    if (!taken || d->image.kind != SceneSchedule::Image::Kind::Backdrop || d->image.index != *taken) {
                        fail("an effect that reads its backdrop samples another image than its copy");
                    }
                } else if (d->image.kind == SceneSchedule::Image::Kind::Backdrop &&
                           takers.at(d->image.index)->step != Step::Backdrop) {
                    fail("a draw other than its effect samples a copy");
                }
                const auto index = d->image.index;
                switch (d->image.kind) {
                    case SceneSchedule::Image::Kind::Item:
                        if (!samples(p, s, index)) {
                            fail("a draw of an item with nothing to sample");
                        }
                        break;
                    case SceneSchedule::Image::Kind::Page:
                        if (index >= p.pages.size()) {
                            fail("a page past the pass's");
                        }
                        break;
                    case SceneSchedule::Image::Kind::Group:
                        if (!is_readable({Kind::Group, index}) || index + 1 != stack.size()) {
                            fail("a group composited before it ended, or into another target than its parent");
                        }
                        break;
                    case SceneSchedule::Image::Kind::Backdrop: {
                        const auto& chain = s.chains.at(index);
                        const bool done = chain.unreadable ? cleared[index] != 0
                                                           : copied[index] && filtered[index] == chain.passes.size();
                        if (!done) {
                            fail("a backdrop drawn before its copy was filtered");
                        }
                        break;
                    }
                    case SceneSchedule::Image::Kind::Blank:
                        break;
                }
                // An effect binds its textures, particles their sprite, as one instance of six vertices a particle.
                const bool effect = d->pipeline == SceneSchedule::Pipeline::Effect;
                const bool particles = d->pipeline == SceneSchedule::Pipeline::Particles;
                const bool vertices = particles ? d->count == 1 && d->vertices % 6 == 0 : d->vertices == 6;
                if (effect != (d->extra == SceneSchedule::Extra::Effect) ||
                    particles != (d->extra == SceneSchedule::Extra::Sprite) ||
                    (effect && d->source >= p.effects.size()) || (particles && d->source >= p.sprites.size()) ||
                    !vertices) {
                    fail("a draw's pipeline, textures or vertices unlike its step");
                }
            } else if (const auto* copy = std::get_if<SceneSchedule::Copy>(&command)) {
                if (open || copied.at(copy->chain) || s.chains[copy->chain].unreadable) {
                    fail("a copy inside a target, twice, or of an unreadable target");
                }
                copied[copy->chain] = 1;
                // What the step that took the chain reads: a backdrop beneath a group, the target enclosing the group;
                // another backdrop or a copy, the current target.
                const auto& taker = *takers.at(copy->chain);
                const bool under = taker.step == Step::Backdrop && p.pass.backdrops.at(taker.source).under;
                if (copy->from != stack.at(stack.size() - (under ? 2 : 1))) {
                    fail("a backdrop or a copy copies another target than the one it reads");
                }
                if (taker.step == Step::Copy &&
                    copy->region != p.pass.copies.at(taker.source).region) {
                    fail("a copy of another region than its step's");
                }
                if (s.chains[copy->chain].from_out != (copy->from == out)) {
                    fail("a chain's from_out unlike its copy");
                }
                reads_out += copy->from == out;
            } else if (const auto* f = std::get_if<SceneSchedule::Filter>(&command)) {
                const auto& chain = s.chains.at(f->chain);
                if (open || !copied[f->chain] || filtered[f->chain] != f->pass || f->pass >= chain.passes.size() ||
                    chain.images.size() != chain.passes.size() + 1) {
                    fail("a filter inside a target, before its copy or out of order");
                }
                ++filtered[f->chain];
            }
        }
        if (open || stack.size() != 1 || next_draw != drawing.size()) {
            fail("the pass ends inside a target or a group, or with batches undrawn");
        }
        if (s.backdrops + s.copies != s.chains.size() || (!out_readable && reads_out) ||
            (out_readable && (s.unreadable_backdrops || s.unreadable_copies))) {
            fail("backdrops and copies counted unlike their chains");
        }
    }

    // Groups drawn ahead (SceneScheduleOptions::hoist): before the target they composite into begins, into images of
    // the device px their composites cover, inner ones first; a group with a backdrop inside draws in turn, its
    // hoisted children ahead of it; a blending group copies its backdrop as it composites, or with fetch_blends reads
    // the target where it draws; images past the budget, in turn.
    void HoistedGroups() {
        // One group between boxes.
        {
            auto p = pass_of(4);
            p.width = 512;
            p.height = 256;
            p.pass.group_depth = 1;
            p.pass.records[2].outline = {8, 4, 20, 10};
            p.pass.batches = {{Step::Boxes, 0, 1, 0}, {Step::BeginGroup}, {Step::Boxes, 1, 1, 0},
                              {Step::EndGroup, 2, 1, 0}, {Step::Boxes, 3, 1, 0}};
            expect(weft::schedule_scene_pass(p), "begin out clear; draw 0 1x6 scene blank; end out; begin group 0 clear; "
                   "draw 1 1x6 scene blank; end group 0 sampled; begin out; draw 2 1x6 scene group 0; "
                   "draw 3 1x6 scene blank; end out", "a group in turn");
            const auto s = weft::schedule_scene_pass(p, {}, nullptr, true, {true, false});
            expect(s, "begin group 1 clear; draw 1 1x6 scene blank; end group 1 sampled; begin out clear; "
                   "draw 0 1x6 scene blank; draw 2 1x6 scene group 1; draw 3 1x6 scene blank; end out",
                   "a group drawn ahead");
            CHECK(s.groups == 1 && s.hoisted.size() == 1 && s.hoisted[0].size == (std::array<std::uint32_t, 2>{64, 64}) &&
                  s.hoisted[0].origin == (std::array<std::int32_t, 2>{8, 4}));
            // On a target smaller than the rounding, the image is no larger than the pass.
            p.width = 40;
            p.height = 30;
            const auto small = weft::schedule_scene_pass(p, {}, nullptr, true, {true, false});
            CHECK(small.hoisted.size() == 1 &&
                  small.hoisted[0].size == (std::array<std::uint32_t, 2>{40, 30}));
        }
        // Nested groups drawn ahead, a group with a backdrop inside in turn (its hoisted child ahead of it), and a
        // blending group, copying its backdrop or reading the target.
        auto p = pass_of(13);
        p.width = 512;
        p.height = 256;
        p.pass.group_depth = 2;
        p.pass.records[3].outline = {10, 10, 20, 20};
        p.pass.records[4].outline = {0, 0, 100, 100};
        p.pass.records[7].outline = {200, 0, 30, 30};
        p.pass.records[9].outline = {0, 0, 512, 256};
        p.pass.records[11].outline = {300, 100, 40, 40};
        p.pass.backdrops = {backdrop({4, 4, 12, 12})};
        p.pass.copies = {{{300, 100, 340, 140}}};
        weft::render::ScenePass::Batch blending{Step::EndGroup, 11, 1, 0};
        blending.copy = 1;
        p.pass.batches = {{Step::Boxes, 0, 1, 0},    {Step::BeginGroup},        {Step::Boxes, 1, 1, 0},
                          {Step::BeginGroup},        {Step::Boxes, 2, 1, 0},    {Step::EndGroup, 3, 1, 0},
                          {Step::EndGroup, 4, 1, 0}, {Step::BeginGroup},        {Step::Boxes, 5, 1, 0},
                          {Step::BeginGroup},        {Step::Boxes, 6, 1, 0},    {Step::EndGroup, 7, 1, 0},
                          {Step::Backdrop, 8, 1, 0}, {Step::EndGroup, 9, 1, 0}, {Step::BeginGroup},
                          {Step::Boxes, 10, 1, 0},   blending,                  {Step::Boxes, 12, 1, 0}};
        const std::string ahead = "begin group 3 clear; draw 2 1x6 scene blank; end group 3 sampled; "
                                  "begin group 2 clear; draw 1 1x6 scene blank; draw 3 1x6 scene group 3; "
                                  "end group 2 sampled; begin group 5 clear; draw 10 1x6 scene blank; "
                                  "end group 5 sampled; begin out clear; draw 0 1x6 scene blank; "
                                  "draw 4 1x6 scene group 2; end out; begin group 4 clear; draw 6 1x6 scene blank; "
                                  "end group 4 sampled; begin group 0 clear; draw 5 1x6 scene blank; "
                                  "draw 7 1x6 scene group 4; end group 0; copy group 0 4,4,12,12 chain 0; "
                                  "begin group 0; draw 8 1x6 scene backdrop 0; end group 0 sampled; begin out; "
                                  "draw 9 1x6 scene group 0; ";
        const auto copied = weft::schedule_scene_pass(p, {}, nullptr, true, {true, false});
        expect(copied,
               ahead + "end out; copy out 300,100,340,140 chain 1; begin out; draw 11 1x6 scene group 5 over 1; "
                       "draw 12 1x6 scene blank; end out",
               "groups drawn ahead, a group in turn, a blending group that copies");
        CHECK(copied.hoisted.size() == 4 && copied.hoisted[0].size == (std::array<std::uint32_t, 2>{128, 128}) &&
              copied.blends == 1 && copied.backdrops == 1);
        const auto fetched = weft::schedule_scene_pass(p, {}, nullptr, true, {true, true});
        expect(fetched, ahead + "draw 11 1x6 blend group 5; draw 12 1x6 scene blank; end out",
               "a blending group that reads the target");
        CHECK(fetched.blends == 1 && fetched.chains.size() == 1);
        // A budget for one image of the pass's area: the outer group takes it; the rest draw in turn.
        p.pass.records[4].outline = {0, 0, 512, 256};
        const auto budget = weft::schedule_scene_pass(p, {}, nullptr, true, {true, true});
        CHECK(budget.hoisted.size() == 1 && budget.hoisted[0].size == (std::array<std::uint32_t, 2>{512, 256}));
    }

    // Runs the commands of a pass whose groups may draw ahead as a backend with one target at a time does, failing
    // where it could not or where the pass would draw something else: every batch that draws drawn once, into the
    // target of the group it lies in (a group's composite into its parent's), in batch order within each target; a
    // group's image composited after its drawing ended and before it begins again; a copy outside every target, of the
    // target its reader reads; hoisted images over their groups' regions within the budget; a blending composite that
    // copies, or with fetch reads its target.
    void run_hoisted(const weft::ScenePrepared& p, const SceneSchedule& s, bool fetch, const std::string& what) {
        const auto fail = [&](const std::string& why) {
            std::cerr << what << ": " << why << "\n  " << text(s) << '\n';
            require(false, what + ": " + why);
        };
        const auto& batches = p.pass.batches;
        // Per batch, the group it lies in (none outside every group; a group's EndGroup lies in its parent), and per
        // group its parent and EndGroup.
        constexpr auto none = std::numeric_limits<std::size_t>::max();
        std::vector<std::size_t> owner(batches.size(), none);
        std::vector<std::size_t> parent;
        std::vector<std::size_t> end_of;
        std::vector<std::size_t> open;
        for (std::size_t b = 0; b < batches.size(); ++b) {
            if (batches[b].step == Step::BeginGroup) {
                owner[b] = open.empty() ? none : open.back();
                parent.push_back(owner[b]);
                end_of.push_back(none);
                open.push_back(parent.size() - 1);
            } else if (batches[b].step == Step::EndGroup) {
                end_of[open.back()] = b;
                open.pop_back();
                owner[b] = open.empty() ? none : open.back();
            } else {
                owner[b] = open.empty() ? none : open.back();
            }
        }
        // The batch each draw draws, by its first record.
        std::vector<std::size_t> drawn_by(p.pass.records.size(), none);
        for (std::size_t b = 0; b < batches.size(); ++b) {
            if (batches[b].step != Step::BeginGroup && batches[b].step != Step::Copy) {
                drawn_by.at(batches[b].first) = b;
            }
        }
        // Each group's image, from its composite.
        std::vector<std::optional<SceneSchedule::Target>> image(parent.size());
        for (const auto& command : s.commands) {
            if (const auto* d = std::get_if<SceneSchedule::Draw>(&command);
                d && d->image.kind == SceneSchedule::Image::Kind::Group) {
                const auto b = drawn_by.at(d->first);
                if (b == none || batches[b].step != Step::EndGroup) {
                    fail("a group's image sampled by a draw other than its composite");
                }
                for (std::size_t g = 0; g < parent.size(); ++g) {
                    if (end_of[g] == b) {
                        image[g] = SceneSchedule::Target{Kind::Group, d->image.index};
                    }
                }
            }
        }
        const SceneSchedule::Target out{Kind::Out};
        const auto target_of = [&](std::size_t g) {
            return g == none ? out : image.at(g).value_or(SceneSchedule::Target{Kind::Chain, 999});
        };
        // Hoisted images: over their groups' regions, rounded up to the granularity within the pass, in budget.
        std::uint64_t area{};
        for (std::size_t g = 0; g < parent.size(); ++g) {
            if (!image[g] || image[g]->index < s.groups) {
                continue;
            }
            const auto& h = s.hoisted.at(image[g]->index - s.groups);
            const auto& region = p.pass.records.at(batches[end_of[g]].first).outline;
            const auto side = [&](float r, std::uint32_t size, unsigned limit) {
                return size >= r && (size % limits::group_image_granularity == 0 || size == limit);
            };
            if (h.origin != std::array<std::int32_t, 2>{std::int32_t(region[0]), std::int32_t(region[1])} ||
                !side(region[2], h.size[0], p.width) || !side(region[3], h.size[1], p.height)) {
                fail("a hoisted image not over its group's region");
            }
            area += std::uint64_t(h.size[0]) * h.size[1];
        }
        if (area > std::uint64_t(p.width) * p.height * limits::hoisted_group_budget) {
            fail("hoisted images past the budget");
        }
        std::optional<SceneSchedule::Target> current;
        std::vector<SceneSchedule::Target> readable;
        std::map<std::pair<int, std::uint32_t>, std::size_t> last;
        std::vector<int> drawn(batches.size());
        // Each copy's source by its chain, and the batch that reads each chain.
        std::map<std::uint32_t, SceneSchedule::Target> copied_from;
        std::map<std::uint32_t, std::size_t> read_by;
        for (std::size_t c = 0; c < s.commands.size(); ++c) {
            const auto& command = s.commands[c];
            if (const auto* b = std::get_if<SceneSchedule::Begin>(&command)) {
                if (current) {
                    fail("a target begun inside another");
                }
                current = b->target;
                std::erase(readable, b->target);
                if (b->target.kind == Kind::Group && b->clear && *b->clear != weft::paint::Color{}) {
                    fail("a group not begun transparent");
                }
            } else if (const auto* e = std::get_if<SceneSchedule::End>(&command)) {
                if (!current || *current != e->target) {
                    fail("an end of a target not begun");
                }
                current.reset();
                if (e->sampled) {
                    readable.push_back(e->target);
                }
            } else if (const auto* d = std::get_if<SceneSchedule::Draw>(&command)) {
                const auto b = drawn_by.at(d->first);
                if (b == none || drawn[b]++) {
                    fail("a draw of no batch, or of one drawn before");
                }
                const auto expected = target_of(owner[b]);
                if (!current || *current != expected) {
                    fail("a draw outside its group's target");
                }
                const std::pair<int, std::uint32_t> key{int(expected.kind), expected.index};
                if (last.contains(key) && last[key] > b) {
                    fail("draws out of batch order within a target");
                }
                last[key] = b;
                if (d->image.kind == SceneSchedule::Image::Kind::Group &&
                    std::find(readable.begin(), readable.end(),
                              SceneSchedule::Target{Kind::Group, d->image.index}) == readable.end()) {
                    fail("a group composited before its drawing ended");
                }
                if (d->image.kind == SceneSchedule::Image::Kind::Backdrop) {
                    read_by[d->image.index] = b;
                }
                if (d->backdrop) {
                    read_by[*d->backdrop] = b;
                }
                if (batches[b].step == Step::EndGroup) {
                    const bool blends = batches[b].copy != 0;
                    const bool by_fetch = d->pipeline == SceneSchedule::Pipeline::Blend;
                    if (by_fetch != (blends && fetch) || bool(d->backdrop) != (blends && !fetch)) {
                        fail("a blending composite neither copies nor reads its target, or one that does not blend");
                    }
                }
            } else if (const auto* copy = std::get_if<SceneSchedule::Copy>(&command)) {
                if (current) {
                    fail("a copy inside a target");
                }
                copied_from[copy->chain] = copy->from;
            } else if (std::get_if<SceneSchedule::Filter>(&command) && current) {
                fail("a filter inside a target");
            }
        }
        for (std::size_t b = 0; b < batches.size(); ++b) {
            const bool draws = batches[b].step != Step::BeginGroup && batches[b].step != Step::Copy &&
                               !(batches[b].step == Step::Image && !samples(p, s, batches[b].first));
            if (draws != (drawn[b] == 1)) {
                fail("a batch undrawn, or one that draws nothing drawn");
            }
        }
        if (current) {
            fail("the pass ends inside a target");
        }
        // A copy reads the target its reader draws into (a backdrop beneath a group, the group's parent's), never a
        // hoisted image (whose pixels lie offset by its origin).
        for (const auto& [k, from] : copied_from) {
            if (!read_by.contains(k)) {
                fail("a copy nothing reads");
            }
            const auto b = read_by[k];
            const bool under = batches[b].step == Step::Backdrop && p.pass.backdrops.at(batches[b].source).under;
            const auto reader = owner[b];
            const auto expected = target_of(under ? (reader == none ? none : parent.at(reader)) : reader);
            if (from != expected || (from.kind == Kind::Group && from.index >= s.groups)) {
                fail("a copy of another target than its reader's, or of a hoisted image");
            }
        }
    }

    // Random nestings drawn with groups ahead, with and without fetch: run_hoisted's rules.
    void RandomHoisted() {
        std::mt19937 random(limits::schedule_seed + 1);
        std::size_t hoisted{};
        std::size_t in_turn{};
        std::size_t blends{};
        for (int i = 0; i < limits::schedule_passes; ++i) {
            const auto p = random_pass(random, true);
            const bool fetch = random() % 2 != 0;
            const std::vector<weft::SceneTexture> host{{"bound", 1, false}};
            const auto s = weft::schedule_scene_pass(p, host, nullptr, random() % 4 != 0, {true, fetch});
            run_hoisted(p, s, fetch, "random hoisted pass " + std::to_string(i));
            hoisted += s.hoisted.size();
            for (const auto& command : s.commands) {
                if (const auto* b = std::get_if<SceneSchedule::Begin>(&command);
                    b && b->target.kind == Kind::Group && b->clear && b->target.index < s.groups) {
                    ++in_turn;
                }
            }
            blends += s.blends;
        }
        std::cout << limits::schedule_passes << " random passes with groups ahead: " << hoisted << " hoisted, "
                  << in_turn << " in turn, " << blends << " blending\n";
        require(hoisted > 1000 && in_turn > 1000 && blends > 500, "random passes with groups of every kind");
    }

    void RandomPasses() {
        std::mt19937 random(limits::schedule_seed);
        std::size_t commands{};
        std::size_t backdrops{};
        std::size_t unreadable{};
        std::size_t copies{};
        std::size_t unreadable_copies{};
        for (int i = 0; i < limits::schedule_passes; ++i) {
            const auto p = random_pass(random);
            const bool out_readable = random() % 4 != 0;
            const std::vector<weft::SceneTexture> host{{"bound", 1, false}};
            const auto s = weft::schedule_scene_pass(p, host, nullptr, out_readable);
            run(p, s, out_readable, "random pass " + std::to_string(i));
            commands += s.commands.size();
            backdrops += s.backdrops;
            unreadable += s.unreadable_backdrops;
            copies += s.copies;
            unreadable_copies += s.unreadable_copies;
        }
        std::cout << limits::schedule_passes << " random passes, " << commands << " commands, " << backdrops
                  << " backdrops (" << unreadable << " of a target the pass may not read), " << copies
                  << " copies for effects (" << unreadable_copies << ")\n";
        require(backdrops > 1000 && unreadable > 100, "random passes with backdrops of both kinds");
        require(copies > 1000 && unreadable_copies > 100, "random passes with copies of both kinds");
    }
} // namespace

int main() {
    try {
        PlainDraws();
        NestedGroups();
        Backdrops();
        UnreadableTarget();
        EffectsParticlesTextures();
        EffectCopies();
        HoistedGroups();
        RandomPasses();
        RandomHoisted();
        std::cout << "PASS scene pass schedules: draws, groups, backdrops, unreadable targets, effects, particles, "
                     "host textures, copies effects read, random nestings\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
}
