#include "scene/scene_schedule.hpp"

#include "constants/render.hpp"

#include <algorithm>
#include <cmath>
#include <functional>
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
                                      const paint::Color* clear, bool out_readable, SceneScheduleOptions options) {
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
        // A draw of an item whose chain holds a mask with an image binds that image.
        const auto masked = [&](std::uint32_t i) {
            return i < prepared.mask_images.size() && prepared.mask_images[i] ? Schedule::Extra::Mask
                                                                              : Schedule::Extra::None;
        };
        const auto quad = std::uint32_t(constants::render::quad_vertex_count);
        const auto draw = [&](std::uint32_t first, std::uint32_t count, std::uint32_t vertices,
                              Schedule::Pipeline pipeline, Image image, Schedule::Extra extra = Schedule::Extra::None,
                              std::uint32_t source = 0, std::optional<std::uint32_t> backdrop = std::nullopt) {
            s.commands.push_back(Schedule::Draw{first, count, vertices, pipeline, image, extra, source, backdrop});
        };
        const Target out{Target::Kind::Out};
        const paint::Color transparent{};

        // The groups the batches nest, in the order they begin: each one's BeginGroup and EndGroup batches, its depth
        // (1 outside every other group), its children, whether anything in it reads a target (a backdrop, a copy, a
        // group in it that blends with a copy, which reads it or a group around it as it composites), and the hoisted
        // image it is drawn into ahead of the target it composites into, if any.
        struct Group {
            std::size_t begin{};
            std::size_t end{};
            std::size_t depth{};
            std::vector<std::size_t> children;
            bool reads{};
            std::optional<std::uint32_t> image;
        };
        std::vector<Group> groups;
        std::vector<std::size_t> outermost;
        constexpr auto none = std::numeric_limits<std::size_t>::max();
        std::vector<std::size_t> group_at(pass.batches.size(), none);
        {
            std::vector<std::size_t> open;
            for (std::size_t b = 0; b < pass.batches.size(); ++b) {
                const auto& batch = pass.batches[b];
                if (batch.step == Step::BeginGroup) {
                    (open.empty() ? outermost : groups[open.back()].children).push_back(groups.size());
                    group_at[b] = groups.size();
                    open.push_back(groups.size());
                    groups.push_back({b, b, open.size()});
                } else if (batch.step == Step::EndGroup) {
                    if (open.empty()) {
                        throw std::runtime_error("a group ends that did not begin");
                    }
                    groups[open.back()].end = b;
                    group_at[b] = open.back();
                    open.pop_back();
                    if (batch.copy && !options.fetch_blends) {
                        for (const auto g : open) {
                            groups[g].reads = true;
                        }
                    }
                } else if (batch.step == Step::Backdrop || batch.step == Step::Copy) {
                    for (const auto g : open) {
                        groups[g].reads = true;
                    }
                }
            }
            if (!open.empty()) {
                throw std::runtime_error("a group that does not end");
            }
        }
        // Hoisting: a group nothing in which reads a target is drawn, before the target it composites into begins,
        // into an image of the device pixels its composite covers (its EndGroup record's outline), its sides rounded up
        // to group_image_granularity but no longer than the pass's; outer groups first while their images fit in a
        // budget of hoisted_group_budget passes' area. The target it composites into then keeps drawing without a break.
        if (options.hoist) {
            const auto granularity = std::uint32_t(constants::render::scene::group_image_granularity);
            const auto up = [&](float v, unsigned limit) {
                const auto side = std::max<std::uint32_t>(std::uint32_t(std::ceil(v)), 1);
                return std::min<std::uint32_t>((side + granularity - 1) / granularity * granularity,
                                               std::max<std::uint32_t>(limit, side));
            };
            auto budget = std::uint64_t(prepared.width) * prepared.height *
                          std::uint64_t(constants::render::scene::hoisted_group_budget);
            for (auto& g : groups) {
                const auto& region = pass.records.at(pass.batches[g.end].first).outline;
                if (g.reads || !(region[2] > 0 && region[3] > 0)) {
                    continue;
                }
                const std::array<std::uint32_t, 2> size{up(region[2], prepared.width), up(region[3], prepared.height)};
                const auto area = std::uint64_t(size[0]) * size[1];
                if (area > budget) {
                    continue;
                }
                budget -= area;
                g.image = std::uint32_t(s.hoisted.size());
                s.hoisted.push_back({size, {std::int32_t(region[0]), std::int32_t(region[1])}});
            }
        }
        const auto hoisted_target = [&](const Group& g) {
            return Target{Target::Kind::Group, std::uint32_t(s.groups + *g.image)};
        };

        // The targets the batches being drawn nest: the pass's own, then each group's that is drawing.
        std::vector<Target> targets{out};
        // Ends drawing into the current target, takes chain's copy of region of target from and filters it with the
        // chain's passes (or, when from may not be read, clears the chain's last image), and resumes drawing into the
        // current target, keeping what it holds. The chain's index, and whether from could not be read.
        const auto take = [&](Target from, const std::array<std::int32_t, 4>& region, Schedule::Chain chain) {
            chain.from_out = from == out;
            chain.unreadable = chain.from_out && !out_readable;
            const auto k = std::uint32_t(s.chains.size());
            const auto last = std::uint32_t(chain.images.size() - 1);
            const bool unreadable = chain.unreadable;
            s.commands.push_back(Schedule::End{targets.back()});
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
            s.commands.push_back(Schedule::Begin{targets.back()});
            return std::pair{k, unreadable};
        };
        // A group's composite into the current target, EndGroup batch b's record over image: a group that blends
        // first copies what the current target holds under it (a chain of the copy alone; a target that cannot be
        // read leaves the copy transparent), the current target ended for the copy when open, then resumed.
        const auto composite = [&](std::size_t b, Image image, bool open) {
            const auto& batch = pass.batches[b];
            std::optional<std::uint32_t> backdrop;
            if (batch.copy && options.fetch_blends) {
                // It reads the target where it draws.
                ++s.blends;
                if (!open) {
                    s.commands.push_back(Schedule::Begin{targets.back()});
                }
                draw(batch.first, 1, quad, Schedule::Pipeline::Blend, image, masked(batch.first), batch.first);
                return;
            }
            if (batch.copy) {
                const auto& region = pass.copies.at(batch.copy - 1).region;
                Schedule::Chain chain;
                chain.images.push_back({std::uint32_t(region[2] - region[0]), std::uint32_t(region[3] - region[1])});
                chain.from_out = targets.back() == out;
                chain.unreadable = chain.from_out && !out_readable;
                const auto k = std::uint32_t(s.chains.size());
                if (open) {
                    s.commands.push_back(Schedule::End{targets.back()});
                }
                if (chain.unreadable) {
                    const Target copy{Target::Kind::Chain, k, 0};
                    s.commands.push_back(Schedule::Begin{copy, transparent});
                    s.commands.push_back(Schedule::End{copy, true});
                    ++s.unreadable_blends;
                } else {
                    s.commands.push_back(Schedule::Copy{targets.back(), region, k});
                }
                s.chains.push_back(std::move(chain));
                backdrop = k;
                ++s.blends;
                open = false;
            }
            if (!open) {
                s.commands.push_back(Schedule::Begin{targets.back()});
            }
            draw(batch.first, 1, quad, Schedule::Pipeline::Scene, image, masked(batch.first), batch.first, backdrop);
        };
        // Per ScenePass::copies entry, the chain of its copy once its Copy step took it.
        constexpr auto untaken = std::numeric_limits<std::uint32_t>::max();
        std::vector<std::uint32_t> copy_chains(pass.copies.size(), untaken);
        // Draws batches [from, to) into the current target; and, before a target begins, the hoisted groups among
        // its children (theirs first), each into its image.
        std::function<void(std::size_t, std::size_t)> content;
        std::function<void(const std::vector<std::size_t>&)> ahead;
        ahead = [&](const std::vector<std::size_t>& children) {
            for (const auto c : children) {
                const auto& g = groups[c];
                if (!g.image) {
                    continue;
                }
                ahead(g.children);
                const auto image = hoisted_target(g);
                s.commands.push_back(Schedule::Begin{image, transparent});
                targets.push_back(image);
                content(g.begin + 1, g.end);
                targets.pop_back();
                s.commands.push_back(Schedule::End{image, true});
            }
        };
        content = [&](std::size_t from, std::size_t to) {
            for (std::size_t b = from; b < to; ++b) {
                const auto& batch = pass.batches[b];
                switch (batch.step) {
                    case Step::Boxes:
                        draw(batch.first, batch.count, quad, Schedule::Pipeline::Scene, {});
                        break;
                    case Step::Atlas:
                        draw(batch.first, batch.count, quad, Schedule::Pipeline::Scene,
                             {Image::Kind::Page, batch.source});
                        break;
                    case Step::Image:
                        if (samples(batch.first)) {
                            draw(batch.first, 1, quad, Schedule::Pipeline::Scene, {Image::Kind::Item, batch.first},
                                 masked(batch.first), batch.first);
                        }
                        break;
                    case Step::BeginGroup: {
                        const auto& g = groups[group_at[b]];
                        if (g.image) {
                            // Drawn ahead into its image: here, its composite alone.
                            composite(g.end, {Image::Kind::Group, hoisted_target(g).index}, true);
                        } else {
                            s.commands.push_back(Schedule::End{targets.back()});
                            const Target group{Target::Kind::Group, std::uint32_t(g.depth - 1)};
                            ahead(g.children);
                            s.commands.push_back(Schedule::Begin{group, transparent});
                            targets.push_back(group);
                            content(b + 1, g.end);
                            targets.pop_back();
                            s.commands.push_back(Schedule::End{group, true});
                            composite(g.end, {Image::Kind::Group, group.index}, false);
                        }
                        b = g.end;
                        break;
                    }
                    case Step::EndGroup:
                        throw std::runtime_error("a group ends that did not begin");
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
                        draw(batch.first, batch.count, quad, Schedule::Pipeline::Effect, image,
                             Schedule::Extra::Effect, batch.source);
                        break;
                    }
                    case Step::Copy: {
                        // What the current target holds over the copy's region, for the effect that reads it later:
                        // a chain of the copy alone.
                        const auto& region = pass.copies.at(batch.source).region;
                        Schedule::Chain chain;
                        chain.images.push_back(
                            {std::uint32_t(region[2] - region[0]), std::uint32_t(region[3] - region[1])});
                        const auto [k, unreadable] = take(targets.back(), region, std::move(chain));
                        copy_chains.at(batch.source) = k;
                        ++s.copies;
                        if (unreadable) {
                            ++s.unreadable_copies;
                        }
                        break;
                    }
                    case Step::Particles:
                        draw(batch.first, 1, quad * batch.count, Schedule::Pipeline::Particles, {},
                             Schedule::Extra::Sprite, batch.source);
                        break;
                    case Step::Backdrop: {
                        // Copy the region of the target the backdrop reads (the current one, or the one enclosing
                        // the current group), filter the copy, and draw it over the current target or beneath its
                        // content.
                        const auto& d = pass.backdrops.at(batch.source);
                        if (d.under && targets.size() < 2) {
                            throw std::runtime_error("a backdrop beneath a group inside one");
                        }
                        const auto [k, unreadable] =
                            take(targets[targets.size() - (d.under ? 2 : 1)], d.region, backdrop_chain(d));
                        if (unreadable) {
                            ++s.unreadable_backdrops;
                        }
                        draw(batch.first, 1, quad,
                             d.under ? Schedule::Pipeline::Under : Schedule::Pipeline::Scene,
                             {Image::Kind::Backdrop, k});
                        ++s.backdrops;
                        break;
                    }
                }
            }
        };
        ahead(outermost);
        if (!prepared.over) {
            s.commands.push_back(Schedule::Begin{out, pass.clear});
        } else {
            s.commands.push_back(Schedule::Begin{out, clear ? std::optional<paint::Color>(*clear) : std::nullopt});
        }
        content(0, pass.batches.size());
        s.commands.push_back(Schedule::End{out});
        return s;
    }
} // namespace weft
