#pragma once

#include "probe_common.hpp"
#include "probe_constants.hpp"

namespace probes {
    inline const weft::DomSnapshot& flow_find(const weft::DomSnapshot& n, const std::string& id) {
        if (n.attributes.contains("id") && n.attributes.at("id") == id) {
            return n;
        }
        for (const auto& c : n.children) {
            try {
                return flow_find(c, id);
            } catch (const std::out_of_range&) {
            }
        }
        throw std::out_of_range(id);
    }
} // namespace probes
