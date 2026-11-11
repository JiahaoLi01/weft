#pragma once

#include "devtools/json.hpp"
#include "devtools/server.hpp"
#include "view/view.hpp"

#include <array>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <tuple>
#include <unordered_map>
#include <vector>

// The developer tools' side of one View: the Chrome DevTools Protocol over the Server's connections (docs/devtools.md).
// Everything here runs on the owner thread, between the View's advances.
namespace weft::devtools {
    // What an agent asks of whoever owns its View.
    struct AgentHost {
        // Loads a page into a new View in the place of the agent's, which goes once the page loaded: the new View;
        // or null and why, with the agent's View and its page as they were. Without one the agent loads pages into
        // its View, where a page that does not load leaves none.
        std::function<View*(const ViewPage& page, std::string& error)> reload;
    };

    class Agent {
    public:
        Agent(View& view, std::shared_ptr<Server> server, std::string target, AgentHost host = {});
        // Touches the Server, not the View: the View may be gone.
        ~Agent();
        Agent(const Agent&) = delete;
        Agent& operator=(const Agent&) = delete;

        const std::shared_ptr<Server>& server() const {
            return server_;
        }

        const View* view() const {
            return view_;
        }

        // The page a tool supplies in the place of one the host loads (Weft.setPage, Weft.setStyleSheet): the page it
        // stands for is the host's of the same base. Null: the host's page loads.
        const ViewPage* supplied_page(const ViewPage& host) const {
            return supplied_ && supplied_->base == host.base ? &*supplied_ : nullptr;
        }

        // Takes what came in, and sends what changed on the page.
        void pump();
        // Takes the tools' marks off the page; the View must be alive.
        void detach();

    private:
        struct Session {
            std::uint64_t connection{};
            bool dom{};
            bool css{};
            bool runtime{};
            bool log{};
            // Told of the page the View shows (Weft.enable): Weft.pageChanged.
            bool weft{};
            // The last entry of the View's log each of the two domains was sent.
            std::uint64_t runtime_sent{};
            std::uint64_t log_sent{};
            // The nodes whose children this client has been given.
            std::set<int> children_sent;
            // The element the console's $0 is.
            int inspected{};
        };

        struct Located {
            const DomSnapshot* node{};
            const DomSnapshot* parent{};
            // Its index among its parent's childNodes.
            std::size_t index{};
        };

        struct Boxes {
            std::array<double, 4> margin{};
            std::array<double, 4> border{};
            std::array<double, 4> padding{};
            std::array<double, 4> content{};
        };

        struct Highlight {
            int node{};
            std::array<std::string, 4> colors;
            bool info{};
        };

        using Key = std::tuple<std::uint32_t, std::uint32_t, std::uint64_t>;
        using Handler = Json (Agent::*)(Session&, const Json&);
        // The range of [start, end) of a declaration block's text.
        using Place = std::function<Json(std::size_t, std::size_t)>;

        void dispatch(Session& session, const std::string& text);
        void send(const Session& session, std::string_view method, Json::Object params);
        void forget(std::uint64_t connection);
        Session* find_session(std::uint64_t connection);

        // The document.
        int id_of(const NodeIdentity& identity);
        void reindex();
        void ensure_document();
        void refresh_document();
        void page_changed();
        const Located& located(const Json& params);
        Json node_json(const DomSnapshot& node, int depth, Session& session);
        Json::Array children_json(const DomSnapshot& node, int depth, Session& session);
        void diff(const DomSnapshot& old, const DomSnapshot& now, Session& session);
        void push_path(int id, Session& session);
        void push_children(const DomSnapshot& node, int depth, Session& session);
        void forget_children(const DomSnapshot& node, Session& session);
        std::string node_expression(int id);
        std::vector<int> query(Session& session, const Json& params, bool all);
        std::string run(const std::string& script);
        std::string outer_html(const DomSnapshot& node) const;

        // Styles.
        void read_sheet();
        Json range(std::size_t start, std::size_t end) const;
        std::size_t offset_at(double line, double column) const;
        std::pair<std::size_t, std::size_t> edited_range(const Json& edit) const;
        Json style_json(const std::vector<WrittenDeclaration>& written, const Json* id, std::string_view text,
                        const Place* place);
        Json sheet_style(std::size_t start, std::size_t end);
        Json css_rule(const RuleSource& source, bool user_agent, Json::Array* texts = nullptr);
        Json rule_json(const std::vector<const MatchedRule*>& matches);
        std::optional<std::string> restyle(const std::string& text, const std::string& keyframes, bool quiet);
        void replace_sheet_text(std::size_t start, std::size_t end, std::string_view text);
        std::shared_ptr<const RuleSource> rule_from(std::size_t offset) const;

        // Pages.
        void sync_page();
        std::optional<std::string> load_page(const ViewPage& page);
        bool supplied_shown() const;
        Json::Object page_state() const;
        Json::Array matched_rules_json(const NodeIdentity& identity);
        Json inline_style(const DomSnapshot& node);
        std::string sheet_header_url() const;
        void add_sheet(const Session& session);

        // The log.
        void forward_log(Session& session);
        void send_sheet_problems(Session& session);
        Json report();

        // The overlay.
        std::optional<Boxes> boxes_of(const NodeIdentity& identity);
        void highlight(int node, const Json* config);
        void clear_highlight();
        void update_overlay();
        void update_picking();

        // Domains.
        Json dom_get_document(Session&, const Json&);
        Json dom_request_child_nodes(Session&, const Json&);
        Json dom_describe_node(Session&, const Json&);
        Json dom_push_nodes(Session&, const Json&);
        Json dom_get_box_model(Session&, const Json&);
        Json dom_get_node_for_location(Session&, const Json&);
        Json dom_get_outer_html(Session&, const Json&);
        Json dom_set_attribute_value(Session&, const Json&);
        Json dom_set_attributes_as_text(Session&, const Json&);
        Json dom_remove_attribute(Session&, const Json&);
        Json dom_set_node_value(Session&, const Json&);
        Json dom_remove_node(Session&, const Json&);
        Json dom_set_inspected_node(Session&, const Json&);
        Json dom_query_selector(Session&, const Json&);
        Json dom_query_selector_all(Session&, const Json&);
        Json dom_resolve_node(Session&, const Json&);
        Json css_get_platform_fonts(Session&, const Json&);
        Json css_get_animated_styles(Session&, const Json&);
        Json css_get_environment_variables(Session&, const Json&);
        Json css_take_computed_style_updates(Session&, const Json&);
        Json css_enable(Session&, const Json&);
        Json css_get_matched_styles(Session&, const Json&);
        Json css_get_inline_styles(Session&, const Json&);
        Json css_get_computed_style(Session&, const Json&);
        Json css_get_style_sheet_text(Session&, const Json&);
        Json css_set_style_texts(Session&, const Json&);
        Json css_set_style_sheet_text(Session&, const Json&);
        Json css_set_rule_selector(Session&, const Json&);
        Json css_add_rule(Session&, const Json&);
        Json css_create_style_sheet(Session&, const Json&);
        Json page_reload(Session&, const Json&);
        Json weft_enable(Session&, const Json&);
        Json weft_get_page(Session&, const Json&);
        Json weft_set_page(Session&, const Json&);
        Json weft_set_style_sheet(Session&, const Json&);
        Json weft_set_resources(Session&, const Json&);
        Json weft_get_snapshot(Session&, const Json&);
        Json overlay_highlight_node(Session&, const Json&);
        Json overlay_hide_highlight(Session&, const Json&);
        Json overlay_set_inspect_mode(Session&, const Json&);
        Json runtime_enable(Session&, const Json&);
        Json runtime_evaluate(Session&, const Json&);
        Json runtime_discard_console_entries(Session&, const Json&);
        Json log_enable(Session&, const Json&);
        Json dom_enable(Session&, const Json&);
        Json page_get_resource_tree(Session&, const Json&);
        Json page_get_resource_content(Session&, const Json&);
        Json weft_get_compatibility_report(Session&, const Json&);
        Json debugger_enable(Session&, const Json&);
        Json network_emulate_by_rule(Session&, const Json&);
        Json runtime_get_isolate_id(Session&, const Json&);

        View* view_;
        AgentHost host_;
        std::shared_ptr<Server> server_;
        std::string target_;
        // The page a tool supplies, and the connection that did: it goes with the connection.
        std::optional<ViewPage> supplied_;
        std::uint64_t supplied_by_{};
        std::vector<Session> sessions_;
        std::uint64_t loads_{};
        // The address of the front end goes to stderr once.
        bool announce_{true};

        std::map<Key, int> ids_;
        int next_id_;
        std::optional<DomSnapshot> document_;
        std::uint64_t document_revision_{};
        double last_refresh_{-1};
        std::unordered_map<int, Located> index_;

        std::string sheet_text_;
        std::string sheet_name_;
        std::vector<std::size_t> line_starts_;

        std::optional<Highlight> highlight_;
        std::string overlay_key_;
        bool picking_{};
        std::optional<Highlight> picking_style_;
        NodeIdentity last_hovered_;
    };
} // namespace weft::devtools
