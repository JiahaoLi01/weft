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

    // The boxes an element's ::before and ::after generate (CSS Pseudo-Elements 4 §2), and their text, are not DOM nodes:
    // their identities are the element's with a mark in the view epoch's top bits (epochs stay far below them), so a
    // scene keeps their state from frame to frame and a hit on one is its element's.
    enum class GeneratedBox : std::uint8_t {
        Before = 1,
        After = 2
    };

    inline constexpr unsigned generated_epoch_shift = 62;
    inline constexpr std::uint64_t generated_text_bit = std::uint64_t(1) << 61;

    inline NodeIdentity generated_identity(NodeIdentity element, GeneratedBox box, bool text = false) {
        element.view_epoch |= std::uint64_t(box) << generated_epoch_shift | (text ? generated_text_bit : 0);
        return element;
    }

    // Whether an identity is a generated box's (or its text's).
    inline bool is_generated(NodeIdentity id) {
        return id.view_epoch >= generated_text_bit;
    }

    // The element a generated box belongs to; the identity itself for any other.
    inline NodeIdentity originating(NodeIdentity id) {
        id.view_epoch &= generated_text_bit - 1;
        return id;
    }

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
        // The element has the focus (:focus), or it or a descendant has (:focus-within).
        bool focused{};
        bool focus_within{};
        // The node's subtree revision when the DOM copied it, or 0 for a value the DOM did not produce. Copies of one
        // node with the same non-zero revision have equal content. Code that changes a copy afterwards sets revision to
        // 0 on the changed node and its ancestors in that copy.
        std::uint64_t revision{};

        // Content equality; the revision is bookkeeping, not content.
        friend bool operator==(const DomSnapshot& a, const DomSnapshot& b) {
            return a.identity == b.identity && a.type == b.type && a.local_name == b.local_name &&
                   a.namespace_uri == b.namespace_uri && a.data == b.data && a.attributes == b.attributes &&
                   a.children == b.children && a.scroll_left == b.scroll_left && a.scroll_top == b.scroll_top &&
                   a.hovered == b.hovered && a.active == b.active && a.focused == b.focused &&
                   a.focus_within == b.focus_within;
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
