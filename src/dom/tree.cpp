#include "dom/tree.hpp"

#include "constants/common.hpp"

#include <algorithm>
#include <cassert>
#include <stdexcept>
#include <utility>

namespace weft::dom {
    namespace {
        std::string json_string(std::string_view input) {
            std::string output = "\"";
            constexpr char hex[] = "0123456789abcdef";
            for (const unsigned char c : input) {
                if (c == '"' || c == '\\') {
                    output += '\\';
                    output += static_cast<char>(c);
                } else if (c < 0x20) {
                    output += "\\u00";
                    output += hex[c >> 4];
                    output += hex[c & 15];
                } else {
                    output += static_cast<char>(c);
                }
            }
            return output + '"';
        }
    } // namespace

    Node& Tree::create(int type, std::string local, std::string space, NodeIdentity document) {
        auto node = std::make_unique<Node>();
        node->type = type;
        node->local = std::move(local);
        node->space = std::move(space);
        node->document = document;
        const auto index = slots_.first_free();
        if (index == slots_.size()) {
            slots_.append();
        }
        const auto generation = slots_.occupy(index);
        node->identity = {static_cast<std::uint32_t>(index), generation, view_epoch_};
        if (type == constants::document_node_type && !document_.generation) {
            document_ = node->identity;
        }
        slots_[index] = std::move(node);
        ++size_;
        return *slots_[index];
    }

    void Tree::reset(std::uint64_t view_epoch) noexcept {
        assert(size_ == 0);
        view_epoch_ = view_epoch;
        document_ = {};
        revision_ = 1;
        change_head_ = change_count_ = 0;
    }

    std::unique_ptr<Node> Tree::release(const NodeIdentity& node) noexcept {
        if (!find(node)) {
            return nullptr;
        }
        auto taken = std::move(slots_[node.slot]);
        slots_.vacate(node.slot);
        --size_;
        return taken;
    }

    Node* Tree::find(const NodeIdentity& node) const noexcept {
        if (node.view_epoch != view_epoch_) {
            return nullptr;
        }
        const auto* slot = slots_.find(node.slot, node.generation);
        return slot ? slot->get() : nullptr;
    }

    Node& Tree::at(const NodeIdentity& node) const {
        const auto* slot = slots_.find(node.slot, node.generation);
        if (!slot || !*slot) {
            throw std::logic_error("WEFT_TREE_CORRUPTION: dangling node edge");
        }
        return **slot;
    }

    Tree::Move Tree::insert(Node& parent, Node& child, Node* before) {
        if (parent.type != constants::element_node_type && parent.type != constants::document_node_type) {
            throw std::runtime_error("WEFT_HIERARCHY: parent cannot contain children");
        }
        if (child.type == constants::document_node_type) {
            throw std::runtime_error("WEFT_HIERARCHY: cannot insert Document");
        }
        if (parent.document != child.document && parent.type != constants::document_node_type) {
            throw std::runtime_error("WEFT_UNSUPPORTED_DOM: cross-document adoption");
        }
        std::size_t ancestors{};
        for (Node* p = &parent; p; p = parent_of(*p)) {
            if (p == &child) {
                throw std::runtime_error("WEFT_HIERARCHY: insertion creates a cycle");
            }
            if (++ancestors > constants::dom::max_depth) {
                throw std::runtime_error("WEFT_DOM_DEPTH_LIMIT: M0 native DOM depth limit exceeded");
            }
        }
        // Moving a tall detached subtree must not bypass the native recursion bound.
        std::vector<std::pair<Node*, std::size_t>> pending{{&child, ancestors + 1}};
        while (!pending.empty()) {
            const auto [current, depth] = pending.back();
            pending.pop_back();
            if (depth > constants::dom::max_depth) {
                throw std::runtime_error("WEFT_DOM_DEPTH_LIMIT: M0 native DOM depth limit exceeded");
            }
            for (const auto& id : current->children) {
                pending.emplace_back(&at(id), depth + 1);
            }
        }
        if (before && before->parent != parent.identity) {
            throw std::runtime_error("WEFT_NOT_FOUND: reference child is not in parent");
        }
        if (before == &child) {
            return {};
        }
        // Reserve before changing tree edges, so allocation cannot split the move.
        parent.children.reserve(parent.children.size() + 1);
        const auto former = detach(child);
        auto position = before ? std::find(parent.children.begin(), parent.children.end(), before->identity)
                               : parent.children.end();
        parent.children.insert(position, child.identity);
        child.parent = parent.identity;
        changed(child);
        return {true, former};
    }

    NodeIdentity Tree::detach(Node& child) {
        Node* parent = parent_of(child);
        if (!parent) {
            return {};
        }
        auto it = std::find(parent->children.begin(), parent->children.end(), child.identity);
        assert(it != parent->children.end());
        parent->children.erase(it);
        changed(*parent);
        changed(child);
        child.parent = {};
        return parent->identity;
    }

    std::string Tree::text(const Node& n) const {
        if (n.type == constants::text_node_type) {
            return n.data;
        }
        std::string result;
        for (const auto& id : n.children) {
            result += text(at(id));
        }
        return result;
    }

    bool Tree::connected(const Node& n) const {
        const Node* p = &n;
        while (p && p->type != constants::document_node_type) {
            p = parent_of(*p);
        }
        return p && p->identity == document_;
    }

    void Tree::changed(Node& n) noexcept {
        if (exhausted()) {
            return;
        }
        const auto revision = ++revision_;
        ++mutations_;
        changes_[change_head_] = {revision, n.identity};
        change_head_ = (change_head_ + 1) % changes_.size();
        change_count_ = std::min(change_count_ + 1, changes_.size());
        for (auto* p = &n; p; p = p->parent.generation ? slots_[p->parent.slot].get() : nullptr) {
            p->subtree_revision = revision;
        }
    }

    DomChanges Tree::changes_since(std::uint64_t revision) const {
        DomChanges result{view_epoch_, revision_};
        if (revision > result.revision) {
            throw std::invalid_argument("WEFT_DOM_REVISION: future revision");
        }
        const auto start = (change_head_ + changes_.size() - change_count_) % changes_.size();
        result.full_required = revision == 0 || (change_count_ && revision < changes_[start].revision - 1);
        if (!result.full_required) {
            for (std::size_t k = 0; k < change_count_; ++k) {
                const auto& entry = changes_[(start + k) % changes_.size()];
                if (entry.revision > revision && find(entry.identity) &&
                    std::find(result.nodes.begin(), result.nodes.end(), entry.identity) == result.nodes.end()) {
                    result.nodes.push_back(entry.identity);
                }
            }
        }
        return result;
    }

    DomSnapshot Tree::snapshot(const Node& n) const {
        ++snapshot_nodes_;
        DomSnapshot result{n.identity, n.type, n.local, n.space, n.data, {}, {}};
        result.scroll_left = n.scroll_left;
        result.scroll_top = n.scroll_top;
        result.hovered = n.hovered;
        result.active = n.active;
        result.focused = n.focused;
        result.focus_visible = n.focus_visible;
        result.focus_within = n.focus_within;
        result.revision = n.subtree_revision;
        result.attributes = snapshot_attributes(n);
        result.children.reserve(n.children.size());
        for (const auto& id : n.children) {
            result.children.push_back(snapshot(at(id)));
        }
        return result;
    }

    bool Tree::refresh(const Node& n, DomSnapshot& copy) const {
        if (copy.identity != n.identity) {
            copy = snapshot(n);
            return true;
        }
        // Revisions start at 1, so a copy the tree did not produce (revision 0) never matches.
        if (copy.revision == n.subtree_revision) {
            return false;
        }
        ++snapshot_nodes_;
        bool changed{};
        if (copy.data != n.data) {
            copy.data = n.data;
            changed = true;
        }
        if (!same_attributes(copy.attributes, n)) {
            copy.attributes = snapshot_attributes(n);
            changed = true;
        }
        if (copy.scroll_left != n.scroll_left || copy.scroll_top != n.scroll_top || copy.hovered != n.hovered ||
            copy.active != n.active || copy.focused != n.focused || copy.focus_visible != n.focus_visible ||
            copy.focus_within != n.focus_within) {
            copy.scroll_left = n.scroll_left;
            copy.scroll_top = n.scroll_top;
            copy.hovered = n.hovered;
            copy.active = n.active;
            copy.focused = n.focused;
            copy.focus_visible = n.focus_visible;
            copy.focus_within = n.focus_within;
            changed = true;
        }
        const bool same_children = copy.children.size() == n.children.size() &&
                                   std::equal(n.children.begin(), n.children.end(), copy.children.begin(),
                                              [](const NodeIdentity& id, const DomSnapshot& child) {
                                                  return child.identity == id;
                                              });
        if (same_children) {
            for (std::size_t i = 0; i < n.children.size(); ++i) {
                changed |= refresh(at(n.children[i]), copy.children[i]);
            }
        } else {
            // Children keep their copies by identity; a node new to this parent is copied whole.
            std::vector<DomSnapshot> children;
            children.reserve(n.children.size());
            for (std::size_t i = 0; i < n.children.size(); ++i) {
                const auto& id = n.children[i];
                auto found = i < copy.children.size() && copy.children[i].identity == id
                                 ? copy.children.begin() + std::ptrdiff_t(i)
                                 : std::find_if(
                                       copy.children.begin(), copy.children.end(),
                                       [&](const DomSnapshot& child) {
                                           return child.identity == id;
                                       });
                if (found == copy.children.end()) {
                    children.push_back(snapshot(at(id)));
                } else {
                    children.push_back(std::move(*found));
                    found->identity = {};
                    refresh(at(id), children.back());
                }
            }
            copy.children = std::move(children);
            changed = true;
        }
        copy.revision = n.subtree_revision;
        return changed;
    }

    AttributeMap Tree::snapshot_attributes(const Node& n) {
        AttributeMap result;
        for (const auto& a : n.attributes) {
            if (!a.space.empty()) {
                throw std::runtime_error("WEFT_PAGE_PROFILE: namespaced attributes require SVG adapter");
            }
            result.emplace(a.name, a.value);
        }
        return result;
    }

    bool Tree::same_attributes(const AttributeMap& copied, const Node& n) {
        if (copied.size() != n.attributes.size()) {
            return false;
        }
        for (const auto& a : n.attributes) {
            const auto it = copied.find(a.name);
            if (!a.space.empty() || it == copied.end() || it->second != a.value) {
                return false;
            }
        }
        return true;
    }

    std::string Tree::dump(const Node& n) const {
        std::string output = "{\"nodeType\":" + std::to_string(n.type) + ",\"localName\":" + json_string(n.local) +
                             ",\"namespaceURI\":" + json_string(n.space) + ",\"textContent\":" + json_string(text(n)) +
                             ",\"attributes\":{";
        bool comma{};
        for (const Attribute& a : n.attributes) {
            if (comma) {
                output += ',';
            }
            comma = true;
            output += json_string(a.name) + ':' + json_string(a.value);
        }
        output += "},\"children\":[";
        comma = false;
        for (const auto& id : n.children) {
            if (comma) {
                output += ',';
            }
            comma = true;
            output += dump(at(id));
        }
        return output + "]}";
    }

    bool Tree::matches(const Node& n, const std::vector<Selector>& list) const {
        return n.type == constants::element_node_type && dom::matches(list, TreeElement{this, &n});
    }

    void Tree::query(const Node& root, const std::vector<Selector>& list, std::vector<Node*>& result,
                     bool first) const {
        for (const auto& id : root.children) {
            Node& child = at(id);
            if (child.type == constants::element_node_type && dom::matches(list, TreeElement{this, &child})) {
                result.push_back(&child);
                if (first) {
                    return;
                }
            }
            query(child, list, result, first);
            if (first && !result.empty()) {
                return;
            }
        }
    }

    bool TreeElement::html() const {
        return n->space == constants::xhtml_namespace_uri;
    }

    const std::string* TreeElement::attribute(std::string_view name) const {
        for (const auto& a : n->attributes) {
            if (a.name == name) {
                return &a.value;
            }
        }
        return nullptr;
    }

    bool TreeElement::document_root() const {
        const Node* p = tree->parent_of(*n);
        return p && p->type == constants::document_node_type;
    }

    std::optional<TreeElement> TreeElement::parent() const {
        const Node* p = tree->parent_of(*n);
        if (!p || p->type != constants::element_node_type) {
            return std::nullopt;
        }
        return TreeElement{tree, p};
    }

    std::optional<TreeElement> TreeElement::previous() const {
        const Node* p = tree->parent_of(*n);
        if (!p) {
            return std::nullopt;
        }
        const auto at = std::find(p->children.begin(), p->children.end(), n->identity);
        for (auto it = at; it != p->children.begin();) {
            const Node& sibling = tree->at(*--it);
            if (sibling.type == constants::element_node_type) {
                return TreeElement{tree, &sibling};
            }
        }
        return std::nullopt;
    }

    std::optional<TreeElement> TreeElement::next() const {
        const Node* p = tree->parent_of(*n);
        if (!p) {
            return std::nullopt;
        }
        auto it = std::find(p->children.begin(), p->children.end(), n->identity);
        if (it == p->children.end()) {
            return std::nullopt;
        }
        for (++it; it != p->children.end(); ++it) {
            const Node& sibling = tree->at(*it);
            if (sibling.type == constants::element_node_type) {
                return TreeElement{tree, &sibling};
            }
        }
        return std::nullopt;
    }

    bool TreeElement::has_content() const {
        for (const auto& id : n->children) {
            const Node& child = tree->at(id);
            if (child.type == constants::element_node_type ||
                (child.type == constants::text_node_type && !child.data.empty())) {
                return true;
            }
        }
        return false;
    }
} // namespace weft::dom
