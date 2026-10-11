#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <vector>

// DOM values shared by the script DOM, style, layout and scenes: node identities, owned subtree snapshots and change
// journals. No Node pointer or VM value.
namespace weft {
    // A node's identity: its slot, the slot's generation and the view epoch. Weak: it keeps nothing alive.
    struct NodeIdentity {
        std::uint32_t slot{};
        std::uint32_t generation{};
        std::uint64_t view_epoch{};
        friend bool operator==(const NodeIdentity&, const NodeIdentity&) = default;
    };

    // Owner-produced value copy of a subtree.
    struct DomSnapshot {
        NodeIdentity identity;
        int type{};
        std::string local_name;
        std::string namespace_uri;
        std::string data;
        std::map<std::string, std::string> attributes;
        std::vector<DomSnapshot> children;
        double scroll_left{};
        double scroll_top{};
        bool hovered{};
        bool active{};
        friend bool operator==(const DomSnapshot&, const DomSnapshot&) = default;
    };

    // Nodes changed since a revision of the DOM change journal.
    struct DomChanges {
        std::uint64_t epoch{};
        std::uint64_t revision{};
        bool full_required{};
        std::vector<NodeIdentity> nodes; // Weak identities; no extra GC roots.
    };
} // namespace weft
