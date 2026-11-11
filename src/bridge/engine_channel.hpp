#pragma once

#include <cstddef>
#include <cstdint>
#include <map>
#include <mutex>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

// What an engine gives a View's page (docs/engine-communication.md, 6.2-6.4 and 7), queued from any thread for the
// View's owner to deliver at its next safe point.
namespace weft::bridge {
    // Whether the channel took an input: Invalid (a name or value outside bridge_value.hpp's bounds), Full (past
    // constants::bridge's queue bounds; nothing is taken), Stale (a completion for a call that is not open).
    enum class ChannelStatus : std::uint8_t {
        Ok,
        Invalid,
        Full,
        Stale
    };

    // An input for the page: an engine event (its name and value), or a call's completion (the call, whether it
    // succeeded, and its value).
    struct EngineInput {
        enum class Kind : std::uint8_t {
            Event,
            Completion
        };

        Kind kind{};
        std::string name;
        std::uint64_t call{};
        bool succeeded{};
        std::vector<std::uint8_t> value;
    };

    // A view model's fields set since the last delivery, each its last value, by field name.
    struct EngineModel {
        std::string name;
        std::vector<std::pair<std::string, std::vector<std::uint8_t>>> fields;
    };

    // What waits for the page: view models' fields, then events and completions in the order given.
    struct EnginePending {
        std::vector<EngineModel> models;
        std::vector<EngineInput> inputs;
    };

    // The engine's side of a View's bridge. Values are MessagePack values of bridge_value.hpp's subset, or JSON text
    // converted on the giving thread. The page's open calls are registered here, so that a completion for a call that
    // is not open is refused at once. Thread-safe: any thread gives; the View's owner opens and closes calls, takes
    // what waits and resets.
    class EngineChannel {
    public:
        // An event for the page's weft.engine handlers, after those given before. latest replaces the value of an
        // undelivered event of the name, in its place, instead of queueing another.
        ChannelStatus emit(std::string_view name, std::span<const std::uint8_t> value, bool latest = false);
        ChannelStatus emit_json(std::string_view name, std::string_view json, bool latest = false);
        // Settles an open call: its promise resolves with value, or rejects with it. A call that is not open (completed,
        // closed, from before a reset, or never opened) is Stale; a full queue keeps the call open.
        ChannelStatus complete(std::uint64_t call, bool succeeded, std::span<const std::uint8_t> value);
        ChannelStatus complete_json(std::uint64_t call, bool succeeded, std::string_view json);
        // Merges fields, a map of field names to values, into the view model's fields that wait.
        ChannelStatus set_model(std::string_view model, std::span<const std::uint8_t> fields);
        ChannelStatus set_model_json(std::string_view model, std::string_view json);

        // A new call's id (never reused, never 0), open until completed or closed; 0 when
        // constants::bridge::max_open_calls are open.
        std::uint64_t open_call();
        // Closes an open call the page settled otherwise (a timeout): its completions are Stale.
        void close_call(std::uint64_t call);
        // What waits, taken whole.
        EnginePending take();
        // The page was reset: open calls close and what waits is dropped.
        void reset();

        std::size_t open_calls() const;

#if WEFT_DEVTOOLS
        // What the channel holds for a page, for the developer tools' reload (docs/devtools.md): a page tried in the
        // place of another resets the channel as any page does, and the other gets it back when the new one does not load.
        struct Checkpoint {
            std::vector<EngineInput> inputs;
            std::map<std::string, std::map<std::string, std::vector<std::uint8_t>, std::less<>>, std::less<>> models;
            std::unordered_set<std::uint64_t> open;
        };

        Checkpoint checkpoint() const;
        // The checkpoint's open calls, and what waited then before what was given since. Calls opened since close.
        void restore(Checkpoint checkpoint);
#endif

    private:
        mutable std::mutex mutex_;
        std::vector<EngineInput> inputs_;
        // The place in inputs_ of the last event of each name, for latest.
        std::unordered_map<std::string, std::size_t> last_event_;
        std::map<std::string, std::map<std::string, std::vector<std::uint8_t>, std::less<>>, std::less<>> models_;
        std::size_t bytes_{};
        std::unordered_set<std::uint64_t> open_;
        std::uint64_t next_call_{1};

        static bool valid_name(std::string_view name);
    };
} // namespace weft::bridge
