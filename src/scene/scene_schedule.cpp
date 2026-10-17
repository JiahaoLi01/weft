#include "scene/scene_schedule.hpp"

#include "constants/render.hpp"

#include <limits>
#include <stdexcept>

namespace weft {
    namespace {
        using Schedule = SceneSchedule;

        // A backdrop's chain: its copy, then a target per filter pass, each the size the passes before it leave.
        Schedule::Chain backdrop_chain(const render::ScenePass::Backdrop& b) {
            Schedule::Chain chain;
            auto w = std::uint32_t(b.region[2] - b.region[0]);
            auto h = std::uint32_t(b.region[3] - b.region[1]);
            chain.images.push_back({w, h});
            const auto add = [&](render::BackdropFilter filter, const render::BackdropConstants& values) {
                chain.images.push_back({w, h});
                chain.passes.emplace_back(filter, values);
            };
            if (b.pre != paint::identity_color_matrix) {
                add(render::BackdropFilter::Matrix, render::backdrop_matrix_constants(b.pre));
            }
            if (b.size[0] < w) {
                const auto wide = w;
                w = b.size[0];
                add(render::BackdropFilter::Area, render::backdrop_area_constants(wide, w, true));
            }
            if (b.size[1] < h) {
                const auto tall = h;
                h = b.size[1];
                add(render::BackdropFilter::Area, render::backdrop_area_constants(tall, h, false));
            }
            if (b.sigma[0] > 0) {
                add(render::BackdropFilter::Blur, render::backdrop_blur_constants(b.sigma[0], true, w, h));
                add(render::BackdropFilter::Blur, render::backdrop_blur_constants(b.sigma[1], false, w, h));
            }
            return chain;
        }
    } // namespace

    SceneSchedule schedule_scene_pass(const ScenePrepared& prepared, std::span<const SceneTexture> host_textures,
                                      const paint::Color* clear, bool out_readable) {
        using Step = render::ScenePass::Step;
        using Target = Schedule::Target;
        using Image = Schedule::Image;
        const auto& pass = prepared.pass;
        Schedule s;
        s.groups = pass.group_depth;
        // The host texture bound to each texture item's name; none, and nothing drawn, for an unbound one.
        const auto host = scene_texture_bindings(prepared, host_textures);
        s.textures.assign(prepared.textures.size(), nullptr);
        for (std::size_t i = 0; i < prepared.textures.size(); ++i) {
            if (const auto t = prepared.textures[i]) {
                s.textures[i] = host[t - 1];
                if (s.textures[i]) {
                    ++s.bound_textures;
                } else {
                    ++s.unbound_textures;
                }
            }
        }
        // An image item draws what it samples: its image (a path with nothing inside its clip has none), its plane's
        // content, its layer, its filtered image, or its bound host texture when that has a view.
        const auto samples = [&](std::size_t i) {
            return (i < prepared.planes.size() && prepared.planes[i]) ||
                   (i < prepared.layers.size() && prepared.layers[i]) ||
                   (i < prepared.filters.size() && prepared.filters[i]) ||
                   (i < prepared.images.size() && prepared.images[i]) ||
                   (i < s.textures.size() && s.textures[i] && s.textures[i]->view);
        };
        const auto quad = std::uint32_t(constants::render::quad_vertex_count);
        const auto draw = [&](std::uint32_t first, std::uint32_t count, std::uint32_t vertices,
                              Schedule::Pipeline pipeline, Image image, Schedule::Extra extra = Schedule::Extra::None,
                              std::uint32_t source = 0) {
            s.commands.push_back(Schedule::Draw{first, count, vertices, pipeline, image, extra, source});
        };
        const Target out{Target::Kind::Out};
        // The target drawn into at a depth: the group's, or the pass's own outside every group.
        const auto at = [&](std::size_t depth) {
            return depth ? Target{Target::Kind::Group, std::uint32_t(depth - 1)} : out;
        };
        const paint::Color transparent{};
        if (!prepared.over) {
            s.commands.push_back(Schedule::Begin{out, pass.clear});
        } else {
            s.commands.push_back(Schedule::Begin{out, clear ? std::optional<paint::Color>(*clear) : std::nullopt});
        }
        std::size_t depth{};
        // Ends drawing into the current target, takes chain's copy of region of target from and filters it with the
        // chain's passes (or, when from may not be read, clears the chain's last image), and resumes drawing into the
        // current target, keeping what it holds. The chain's index, and whether from could not be read.
        const auto take = [&](Target from, const std::array<std::int32_t, 4>& region, Schedule::Chain chain) {
            chain.from_out = from == out;
            chain.unreadable = chain.from_out && !out_readable;
            const auto k = std::uint32_t(s.chains.size());
            const auto last = std::uint32_t(chain.images.size() - 1);
            const bool unreadable = chain.unreadable;
            s.commands.push_back(Schedule::End{at(depth)});
            if (unreadable) {
                // Nothing the pass may read: the chain's result is transparent.
                const Target filtered{Target::Kind::Chain, k, last};
                s.commands.push_back(Schedule::Begin{filtered, transparent});
                s.commands.push_back(Schedule::End{filtered, true});
            } else {
                s.commands.push_back(Schedule::Copy{from, region, k});
                for (std::uint32_t p = 0; p < chain.passes.size(); ++p) {
                    s.commands.push_back(Schedule::Filter{k, p});
                }
            }
            s.chains.push_back(std::move(chain));
            s.commands.push_back(Schedule::Begin{at(depth)});
            return std::pair{k, unreadable};
        };
        // Per ScenePass::copies entry, the chain of its copy once its Copy step took it.
        constexpr auto untaken = std::numeric_limits<std::uint32_t>::max();
        std::vector<std::uint32_t> copy_chains(pass.copies.size(), untaken);
        for (const auto& batch : pass.batches) {
            switch (batch.step) {
                case Step::Boxes:
                    draw(batch.first, batch.count, quad, Schedule::Pipeline::Scene, {});
                    break;
                case Step::Atlas:
                    draw(batch.first, batch.count, quad, Schedule::Pipeline::Scene, {Image::Kind::Page, batch.source});
                    break;
                case Step::Image:
                    if (samples(batch.first)) {
                        draw(batch.first, 1, quad, Schedule::Pipeline::Scene, {Image::Kind::Item, batch.first});
                    }
                    break;
                case Step::BeginGroup:
                    s.commands.push_back(Schedule::End{at(depth)});
                    s.commands.push_back(Schedule::Begin{{Target::Kind::Group, std::uint32_t(depth)}, transparent});
                    ++depth;
                    break;
                case Step::EndGroup:
                    --depth;
                    s.commands.push_back(Schedule::End{{Target::Kind::Group, std::uint32_t(depth)}, true});
                    s.commands.push_back(Schedule::Begin{at(depth)});
                    draw(batch.first, 1, quad, Schedule::Pipeline::Scene, {Image::Kind::Group, std::uint32_t(depth)});
                    break;
                case Step::Effect: {
                    // An effect that reads its backdrop samples the copy its Copy step took earlier in the pass.
                    Image image;
                    if (batch.copy) {
                        const auto k = copy_chains.at(batch.copy - 1);
                        if (k == untaken) {
                            throw std::runtime_error("an effect reads a copy that no earlier Copy step took");
                        }
                        image = {Image::Kind::Backdrop, k};
                    }
                    draw(batch.first, batch.count, quad, Schedule::Pipeline::Effect, image, Schedule::Extra::Effect,
                         batch.source);
                    break;
                }
                case Step::Copy: {
                    // What the current target holds over the copy's region, for the effect that reads it later: a
                    // chain of the copy alone.
                    const auto& region = pass.copies.at(batch.source).region;
                    Schedule::Chain chain;
                    chain.images.push_back(
                        {std::uint32_t(region[2] - region[0]), std::uint32_t(region[3] - region[1])});
                    const auto [k, unreadable] = take(at(depth), region, std::move(chain));
                    copy_chains.at(batch.source) = k;
                    ++s.copies;
                    if (unreadable) {
                        ++s.unreadable_copies;
                    }
                    break;
                }
                case Step::Particles:
                    draw(batch.first, 1, quad * batch.count, Schedule::Pipeline::Particles, {}, Schedule::Extra::Sprite,
                         batch.source);
                    break;
                case Step::Backdrop: {
                    // Copy the region of the target the backdrop reads (the current one, or the one enclosing the
                    // current group), filter the copy, and draw it over the current target or beneath its content.
                    const auto& b = pass.backdrops.at(batch.source);
                    if (b.under && !depth) {
                        throw std::runtime_error("a backdrop beneath a group inside one");
                    }
                    const auto [k, unreadable] = take(at(b.under ? depth - 1 : depth), b.region, backdrop_chain(b));
                    if (unreadable) {
                        ++s.unreadable_backdrops;
                    }
                    draw(batch.first, 1, quad, b.under ? Schedule::Pipeline::Under : Schedule::Pipeline::Scene,
                         {Image::Kind::Backdrop, k});
                    ++s.backdrops;
                    break;
                }
            }
        }
        s.commands.push_back(Schedule::End{out});
        return s;
    }
} // namespace weft
