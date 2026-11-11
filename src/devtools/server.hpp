#pragma once

#include "base/result.hpp"
#include "constants/devtools.hpp"

#include <atomic>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

// The developer tools' way in: a socket the tool on the developer's machine connects to, which speaks HTTP for the list of
// pages and WebSocket for a page's messages. Only the dev build has this code (docs/devtools.md). The socket is the
// engine's own, not the host's, so that no host has to write one. A thread of its own does the I/O; the owner thread takes
// what came in and hands in what goes out.
namespace weft::devtools {
    struct Listen {
        // Every network interface instead of this machine's alone.
        bool all_interfaces{};
        // The first port tried, and how many from it.
        std::uint16_t first_port{constants::devtools::default_port};
        std::uint16_t attempts{constants::devtools::port_attempts};
    };

    // A value of WEFT_DEVTOOLS_LISTEN: [address:]port, the address 127.0.0.1 or 0.0.0.0. Empty when it is neither.
    std::optional<Listen> parse_listen(std::string_view text);

    // A page the tools can connect to.
    struct ServerTarget {
        std::string id;
        std::string title;
        std::string url;
    };

    // What happened on a page's connections.
    struct ServerEvent {
        enum class Kind : std::uint8_t {
            Opened,
            Message,
            Closed
        };

        Kind kind{};
        std::uint64_t connection{};
        std::string text;
    };

    class Server {
    public:
        // Listens, and serves on a thread of its own until stopped.
        static Result<std::shared_ptr<Server>> start(const Listen& listen);

        Server(const Server&) = delete;
        Server& operator=(const Server&) = delete;
        ~Server();

        // Closes every connection and the port. Owner thread; again does nothing.
        void stop();

        std::uint16_t port() const noexcept {
            return port_;
        }

        // Owner thread.
        void set_target(ServerTarget target);
        void remove_target(std::string_view id);
        std::vector<ServerEvent> take_events(std::string_view target);
        void send(std::uint64_t connection, std::string text);

    private:
        struct Client;

        Server(std::intptr_t listener, std::uint16_t port);
        void serve();
        void take_requests(Client& client);
        void take_frames(Client& client);
        void answer(Client& client, std::string_view head);
        void post(const std::string& target, ServerEvent event);

        std::intptr_t listener_;
        std::uint16_t port_;
        std::atomic<bool> stopping_{};
        std::thread thread_;
        bool stopped_{};

        std::mutex mutex_;
        std::vector<ServerTarget> targets_;
        std::map<std::string, std::vector<ServerEvent>, std::less<>> events_;
        std::vector<std::pair<std::uint64_t, std::string>> outgoing_;
    };

    // What the environment asks for when a runtime is created: nothing without the variable. A runtime is created either
    // way: what could not be done goes to stderr.
    std::shared_ptr<Server> start_from_environment() noexcept;
} // namespace weft::devtools
