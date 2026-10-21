#pragma once

#include "base/generational_slots.hpp"
#include "constants/dom.hpp"
#include "dom/node.hpp"
#include "dom/selector.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

// The native DOM of one view: nodes by identity, their tree edges and the mutation journal. A Tree decides no node
// lifetimes. Its owner creates nodes and releases them; the script binding releases a node when the collector
// finalizes its wrapper, and keeps one wrapper reference per tree edge so the collector sees the tree.
namespace weft::dom {
    struct Attribute {
        // Qualified name, local name and namespace URI (empty for none).
        std::string name;
        std::string local;
        std::string space;
        std::string value;
    };

    struct Node {
        NodeIdentity identity;
        int type{};
        std::string local;
        std::string space;
        std::string data;
        bool hovered{};
        bool active{};
        // The element has the focus (and shows it, :focus-visible), or it or a descendant has.
        bool focused{};
        bool focus_visible{};
        bool focus_within{};
        // A text field's state (dom/text_field.hpp), replaced whole when it changes: snapshots share it.
        std::shared_ptr<const TextFieldState> field;
        std::vector<Attribute> attributes;
        // Tree edges by identity; a null identity (generation 0) is no node.
        NodeIdentity parent;
        NodeIdentity document;
        std::vector<NodeIdentity> children;
        double scroll_left{};
        double scroll_top{};
        // Journal revision of the latest change in this node's subtree; at least 1.
        std::uint64_t subtree_revision{1};
    };

    class Tree {
    public:
        explicit Tree(std::uint64_t view_epoch) : view_epoch_(view_epoch) {
        }

        std::uint64_t view_epoch() const noexcept {
            return view_epoch_;
        }

        // Starts a new view epoch once every node is released: the journal restarts, while slot generations and the
        // mutation and snapshot counts carry on.
        void reset(std::uint64_t view_epoch) noexcept;

        // A new node of document (a null identity for a Document), with no tree edges. The first Document created
        // is the tree's document.
        Node& create(int type, std::string local, std::string space, NodeIdentity document);
        // Takes a node out of the tree; its identity no longer resolves. Edges of nodes released in the same
        // collection may still name it.
        std::unique_ptr<Node> release(const NodeIdentity& node) noexcept;

        // The node an identity names, or nullptr when it is stale or of another view epoch.
        Node* find(const NodeIdentity& node) const noexcept;
        // The node an edge names; edges name live nodes.
        Node& at(const NodeIdentity& node) const;

        Node* parent_of(const Node& n) const {
            return n.parent.generation ? &at(n.parent) : nullptr;
        }

        NodeIdentity document() const noexcept {
            return document_;
        }

        // Live nodes.
        std::size_t size() const noexcept {
            return size_;
        }

        // Slots ever used; a new node takes a free slot below this or the slot at this index.
        std::size_t slot_count() const noexcept {
            return slots_.size();
        }

        // Calls f(Node&) for each live node in slot order.
        template <class F>
        void for_each(F&& f) const {
            for (std::size_t i = 0; i < slots_.size(); ++i) {
                if (slots_.occupied(i)) {
                    f(*slots_[i]);
                }
            }
        }

        // An insertion that changed the tree, and the parent child had before it.
        struct Move {
            bool moved{};
            NodeIdentity former_parent;
        };

        // Validates inserting child into parent before `before` (last when null) and moves it there. Nothing changes
        // when before is child itself. Throws, changing nothing, for a hierarchy the M0 DOM refuses.
        Move insert(Node& parent, Node& child, Node* before);
        // Removes child from its parent; returns the former parent, a null identity when it had none.
        NodeIdentity detach(Node& child);

        // Node.textContent of an element, text node or subtree.
        std::string text(const Node& n) const;
        // Whether the node is in the tree's document.
        bool connected(const Node& n) const;

        // Records a change of n for the journal and marks its subtree revision up to the root. Nothing is recorded
        // once the revision counter is exhausted. Changing a node's fields outside the tree operations needs this
        // call: snapshots and refresh() rely on the revisions.
        void changed(Node& n) noexcept;

        bool exhausted() const noexcept {
            return revision_ == UINT64_MAX;
        }

        std::uint64_t revision() const noexcept {
            return revision_;
        }

        // Changes recorded over every epoch.
        std::uint64_t mutations() const noexcept {
            return mutations_;
        }

        // Nodes changed after revision, or full_required when the journal no longer covers it.
        DomChanges changes_since(std::uint64_t revision) const;

        // An owned copy of a subtree; namespaced attributes are outside the snapshot profile.
        DomSnapshot snapshot(const Node& n) const;
        // Brings an earlier copy of n's subtree up to date. A subtree whose revision has not moved since it was
        // copied is kept as it is, so the work follows what changed. A copy of another node or epoch is replaced.
        // Returns whether the content changed. After an exception the copy is incomplete; discard it.
        bool refresh(const Node& n, DomSnapshot& copy) const;

        // Nodes copied by snapshot() and nodes revisited by refresh() over every epoch.
        std::uint64_t snapshot_nodes() const noexcept {
            return snapshot_nodes_;
        }

        // The subtree as JSON, for evidence reports.
        std::string dump(const Node& n) const;

        // Whether an element matches a selector list; ancestors continue past any query root.
        bool matches(const Node& n, const std::vector<Selector>& list) const;
        // Elements below root in tree order that match the list; only the first one when first.
        void query(const Node& root, const std::vector<Selector>& list, std::vector<Node*>& result, bool first) const;

    private:
        // A node's attributes as a snapshot holds them; namespaced attributes are outside the snapshot profile.
        static AttributeMap snapshot_attributes(const Node& n);
        static bool same_attributes(const AttributeMap& copied, const Node& n);

        struct Change {
            std::uint64_t revision{};
            NodeIdentity identity;
        };

        std::uint64_t view_epoch_;
        GenerationalSlots<std::unique_ptr<Node>> slots_;
        std::size_t size_{};
        NodeIdentity document_;
        std::uint64_t revision_{1};
        std::uint64_t mutations_{};
        std::array<Change, constants::dom::change_journal_capacity> changes_{};
        std::size_t change_head_{};
        std::size_t change_count_{};
        mutable std::uint64_t snapshot_nodes_{};
    };

    // An element of a Tree for the selector engine (dom::SelectorElement).
    struct TreeElement {
        const Tree* tree{};
        const Node* n{};

        std::string_view local_name() const {
            return n->local;
        }

        bool html() const;
        // An attribute by qualified name.
        const std::string* attribute(std::string_view name) const;

        bool hovered() const {
            return n->hovered;
        }

        bool active() const {
            return n->active;
        }

        bool focused() const {
            return n->focused;
        }

        bool focus_visible() const {
            return n->focus_visible;
        }

        bool focus_within() const {
            return n->focus_within;
        }

        // The element is the Document's child.
        bool document_root() const;
        std::optional<TreeElement> parent() const;
        // The element siblings before and after it.
        std::optional<TreeElement> previous() const;
        std::optional<TreeElement> next() const;
        // It has an element or text child (:empty does not match).
        bool has_content() const;

        const TextFieldState* field() const {
            return n->field.get();
        }
    };
} // namespace weft::dom
