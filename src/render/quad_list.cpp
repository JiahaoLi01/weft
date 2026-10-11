#include "render/quad_list.hpp"

#include "constants/render.hpp"

#include <cmath>

namespace weft::render {
    paint::DisplayList quad_list(const FramePacket& packet, std::uint32_t external_textures, unsigned width,
                                 unsigned height) {
        using Kind = paint::Item::Kind;
        // The isolated group diagnostic draws an opaque red box and then an opaque blue one over it, so only blue
        // shows through the group's opacity.
        constexpr paint::Color group_red{1, 0, 0, 1};
        constexpr paint::Color group_blue{0, 0, 1, 1};
        constexpr auto inset = float(constants::render::diagnostic_clip_inset);

        paint::DisplayList list;
        list.assets = packet.assets;
        list.external_textures = external_textures;
        list.items.reserve(packet.draws.size());
        const paint::ClipRect target{0, 0, float(width), float(height)};
        auto add = [&](Kind kind, const Draw& d, std::uint32_t payload, float opacity) -> paint::Item& {
            paint::Item item;
            item.kind = kind;
            item.transform = {1, 0, 0, 1, d.rect.x, d.rect.y};
            item.clip = target;
            item.width = d.rect.w;
            item.height = d.rect.h;
            item.opacity = opacity;
            item.payload = payload;
            return list.items.emplace_back(item);
        };
        auto box = [&](const Draw& d, const paint::Color& color, float opacity) -> paint::Item& {
            paint::Box b;
            b.background = color;
            list.boxes.push_back(std::move(b));
            return add(Kind::Box, d, std::uint32_t(list.boxes.size() - 1), opacity);
        };
        for (const auto& d : packet.draws) {
            if (d.group) {
                add(Kind::BeginGroup, d, 0, 1);
                box(d, group_red, 1);
                box(d, group_blue, 1);
                add(Kind::EndGroup, d, 0, d.opacity);
                continue;
            }
            paint::Item* item{};
            if (d.asset < 0) {
                item = &box(d, d.srgb, d.opacity);
            } else {
                list.images.push_back({std::uint32_t(d.asset), d.uv, d.srgb, d.stroke, d.outline});
                item = &add(Kind::Image, d, std::uint32_t(list.images.size() - 1), d.opacity);
            }
            if (d.clipped) {
                // As the integer scissor of the quad renderer it replaces.
                item->clip = {std::trunc(d.rect.x + inset), std::trunc(d.rect.y + inset),
                              std::trunc(d.rect.x + d.rect.w - inset), std::trunc(d.rect.y + d.rect.h - inset)};
            }
        }
        return list;
    }
} // namespace weft::render
