#pragma once

#include <cstdint>
#include <functional>
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

    // Element attributes by qualified name. Lookups take a string_view or literal without building a string.
    using AttributeMap = std::map<std::string, std::string, std::less<>>;

    // Owner-produced value copy of a subtree.
    struct DomSnapshot {
        NodeIdentity identity;
        int type{};
        std::string local_name;
        std::string namespace_uri;
        std::string data;
        AttributeMap attributes;
        std::vector<DomSnapshot> children;
        double scroll_left{};
        double scroll_top{};
        bool hovered{};
        bool active{};
        // The node's subtree revision when the DOM copied it, or 0 for a value the DOM did not produce. Copies of one
        // node with the same non-zero revision have equal content. Code that changes a copy afterwards sets revision to
        // 0 on the changed node and its ancestors in that copy.
        std::uint64_t revision{};

        // Content equality; the revision is bookkeeping, not content.
        friend bool operator==(const DomSnapshot& a, const DomSnapshot& b) {
            return a.identity == b.identity && a.type == b.type && a.local_name == b.local_name &&
                   a.namespace_uri == b.namespace_uri && a.data == b.data && a.attributes == b.attributes &&
                   a.children == b.children && a.scroll_left == b.scroll_left && a.scroll_top == b.scroll_top &&
                   a.hovered == b.hovered && a.active == b.active;
        }
    };

    namespace dom {
        // Makes `to` equal in content to `from`. Where both hold copies of one node with the same non-zero revision,
        // the subtree is kept as it is, so the work follows what changed. Returns whether the content changed. After an
        // exception `to` is incomplete; discard it.
        bool update_copy(DomSnapshot& to, const DomSnapshot& from);
        // Content equality (operator==) that takes copies of one node with the same non-zero revision as equal without
        // comparing them, so the work follows what differs. Both values must be plain copies of the DOM.
        bool same_content(const DomSnapshot& a, const DomSnapshot& b);
    } // namespace dom

    // Nodes changed since a revision of the DOM change journal.
    struct DomChanges {
        std::uint64_t epoch{};
        std::uint64_t revision{};
        bool full_required{};
        std::vector<NodeIdentity> nodes; // Weak identities; no extra GC roots.
    };
} // namespace weft
