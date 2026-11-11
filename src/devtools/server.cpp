#include "devtools/server.hpp"

#include "devtools/json.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <cctype>
#include <string>
#include <utility>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <cerrno>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace weft::devtools {
    namespace {
#ifdef _WIN32
        using Socket = SOCKET;
        using Poll = WSAPOLLFD;
        constexpr Socket no_socket = INVALID_SOCKET;
        constexpr short poll_in = POLLRDNORM;
        constexpr short poll_out = POLLWRNORM;

        void close_socket(Socket socket) {
            closesocket(socket);
        }

        int poll_sockets(Poll* entries, std::size_t count, int ms) {
            return WSAPoll(entries, ULONG(count), ms);
        }

        bool address_in_use() {
            return WSAGetLastError() == WSAEADDRINUSE || WSAGetLastError() == WSAEACCES;
        }

        bool would_block() {
            return WSAGetLastError() == WSAEWOULDBLOCK;
        }

        void make_nonblocking(Socket socket) {
            u_long on = 1;
            ioctlsocket(socket, FIONBIO, &on);
        }

        // Sockets are set up once a process and left for its end.
        bool start_sockets() {
            static const bool started = [] {
                WSADATA data;
                return WSAStartup(MAKEWORD(2, 2), &data) == 0;
            }();
            return started;
        }
#else
        using Socket = int;
        using Poll = pollfd;
        constexpr Socket no_socket = -1;
        constexpr short poll_in = POLLIN;
        constexpr short poll_out = POLLOUT;

        void close_socket(Socket socket) {
            close(socket);
        }

        int poll_sockets(Poll* entries, std::size_t count, int ms) {
            return poll(entries, nfds_t(count), ms);
        }

        bool address_in_use() {
            return errno == EADDRINUSE;
        }

        bool would_block() {
            return errno == EAGAIN || errno == EWOULDBLOCK;
        }

        void make_nonblocking(Socket socket) {
            fcntl(socket, F_SETFL, fcntl(socket, F_GETFL, 0) | O_NONBLOCK);
        }

        bool start_sockets() {
            return true;
        }
#endif

        Socket socket_of(std::intptr_t handle) {
            return static_cast<Socket>(handle);
        }

        // A tool that goes away in mid-answer must not end the game with SIGPIPE.
        void no_sigpipe(Socket socket) {
#ifdef SO_NOSIGPIPE
            const int on = 1;
            setsockopt(socket, SOL_SOCKET, SO_NOSIGPIPE, reinterpret_cast<const char*>(&on), sizeof on);
#else
            (void)socket;
#endif
        }

        constexpr int send_flags() {
#ifdef MSG_NOSIGNAL
            return MSG_NOSIGNAL;
#else
            return 0;
#endif
        }

        constexpr std::string_view websocket_guid = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";

        // SHA-1 (RFC 3174), for the WebSocket handshake alone.
        std::array<std::uint8_t, 20> sha1(std::string_view text) {
            std::array<std::uint32_t, 5> h{0x67452301, 0xEFCDAB89, 0x98BADCFE, 0x10325476, 0xC3D2E1F0};
            std::string data(text);
            data += char(0x80);
            while (data.size() % 64 != 56) {
                data += char(0);
            }
            const std::uint64_t bits = std::uint64_t(text.size()) * 8;
            for (int shift = 56; shift >= 0; shift -= 8) {
                data += char(bits >> shift & 0xFF);
            }
            const auto rotate = [](std::uint32_t v, unsigned n) {
                return v << n | v >> (32 - n);
            };
            for (std::size_t chunk = 0; chunk < data.size(); chunk += 64) {
                std::array<std::uint32_t, 80> w{};
                for (unsigned i = 0; i < 16; ++i) {
                    const auto* b = reinterpret_cast<const unsigned char*>(data.data() + chunk + i * 4);
                    w[i] = std::uint32_t(b[0]) << 24 | std::uint32_t(b[1]) << 16 | std::uint32_t(b[2]) << 8 | b[3];
                }
                for (unsigned i = 16; i < 80; ++i) {
                    w[i] = rotate(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
                }
                auto [a, b, c, d, e] = h;
                for (unsigned i = 0; i < 80; ++i) {
                    const std::uint32_t f = i < 20   ? (b & c) | (~b & d)
                                            : i < 40 ? b ^ c ^ d
                                            : i < 60 ? (b & c) | (b & d) | (c & d)
                                                     : b ^ c ^ d;
                    const std::uint32_t k = i < 20   ? 0x5A827999
                                            : i < 40 ? 0x6ED9EBA1
                                            : i < 60 ? 0x8F1BBCDC
                                                     : 0xCA62C1D6;
                    const std::uint32_t next = rotate(a, 5) + f + e + k + w[i];
                    e = d;
                    d = c;
                    c = rotate(b, 30);
                    b = a;
                    a = next;
                }
                h[0] += a;
                h[1] += b;
                h[2] += c;
                h[3] += d;
                h[4] += e;
            }
            std::array<std::uint8_t, 20> out{};
            for (unsigned i = 0; i < 20; ++i) {
                out[i] = std::uint8_t(h[i / 4] >> (24 - (i % 4) * 8) & 0xFF);
            }
            return out;
        }

        std::string base64(const std::array<std::uint8_t, 20>& bytes) {
            static constexpr std::string_view alphabet =
                "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
            std::string out;
            for (std::size_t i = 0; i < bytes.size(); i += 3) {
                const std::uint32_t v = std::uint32_t(bytes[i]) << 16 |
                                        (i + 1 < bytes.size() ? std::uint32_t(bytes[i + 1]) << 8 : 0) |
                                        (i + 2 < bytes.size() ? bytes[i + 2] : 0);
                out += alphabet[v >> 18 & 63];
                out += alphabet[v >> 12 & 63];
                out += i + 1 < bytes.size() ? alphabet[v >> 6 & 63] : '=';
                out += i + 2 < bytes.size() ? alphabet[v & 63] : '=';
            }
            return out;
        }

        std::string frame(unsigned opcode, std::string_view payload) {
            std::string out;
            out += char(0x80 | opcode);
            if (payload.size() < 126) {
                out += char(payload.size());
            } else if (payload.size() <= 0xFFFF) {
                out += char(126);
                out += char(payload.size() >> 8);
                out += char(payload.size() & 0xFF);
            } else {
                out += char(127);
                for (int shift = 56; shift >= 0; shift -= 8) {
                    out += char(std::uint64_t(payload.size()) >> shift & 0xFF);
                }
            }
            out += payload;
            return out;
        }

        // A header's value in a request's head (names compared without case); empty without one.
        std::string header(std::string_view head, std::string_view name) {
            std::size_t p = head.find("\r\n");
            while (p != std::string_view::npos) {
                p += 2;
                const auto end = std::min(head.find("\r\n", p), head.size());
                const auto line = head.substr(p, end - p);
                if (const auto colon = line.find(':');
                    colon == name.size() && std::equal(name.begin(), name.end(), line.begin(), [](char a, char b) {
                        return std::tolower(static_cast<unsigned char>(a)) ==
                               std::tolower(static_cast<unsigned char>(b));
                    })) {
                    auto value = line.substr(colon + 1);
                    while (!value.empty() && value.front() == ' ') {
                        value.remove_prefix(1);
                    }
                    return std::string(value);
                }
                p = end < head.size() ? end : std::string_view::npos;
            }
            return {};
        }

        // Whether a web page may connect: the tools' own pages may, and a page on this machine. Any other page the
        // developer has open must not reach a socket that runs scripts in the game. A client without an Origin is no page.
        bool allowed_origin(std::string_view origin) {
            if (origin.empty() || origin.starts_with("devtools://")) {
                return true;
            }
            for (const std::string_view scheme : {"http://", "https://"}) {
                if (!origin.starts_with(scheme)) {
                    continue;
                }
                const auto host = origin.substr(scheme.size());
                return host.starts_with("[::1]") || host.substr(0, host.find(':')) == "127.0.0.1" ||
                       host.substr(0, host.find(':')) == "localhost";
            }
            return false;
        }

        // A listening socket on the port; no_socket, with in_use saying whether the port was the reason, otherwise.
        Socket listen_on(bool all_interfaces, std::uint16_t port, bool& in_use) {
            in_use = false;
            // Every interface is those of IPv6 too, on one socket: a device a development machine reaches through a
            // tunnel (an iPad's from Xcode's) has only an IPv6 address there. Without IPv6, those of IPv4.
            Socket socket = all_interfaces ? ::socket(AF_INET6, SOCK_STREAM, IPPROTO_TCP) : no_socket;
            const bool both = socket != no_socket;
            if (!both) {
                socket = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
            }
            if (socket == no_socket) {
                return no_socket;
            }
#ifndef _WIN32
            // A game started again takes the port its last run left in TIME_WAIT. (On Windows this option would let
            // another process take a port in use instead.)
            const int on = 1;
            setsockopt(socket, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&on), sizeof on);
#endif
            bool bound{};
            if (both) {
                const int off = 0;
                setsockopt(socket, IPPROTO_IPV6, IPV6_V6ONLY, reinterpret_cast<const char*>(&off), sizeof off);
                sockaddr_in6 address{};
                address.sin6_family = AF_INET6;
                address.sin6_addr = in6addr_any;
                address.sin6_port = htons(port);
                bound = bind(socket, reinterpret_cast<const sockaddr*>(&address), sizeof address) == 0;
            } else {
                sockaddr_in address{};
                address.sin_family = AF_INET;
                address.sin_addr.s_addr = htonl(all_interfaces ? INADDR_ANY : INADDR_LOOPBACK);
                address.sin_port = htons(port);
                bound = bind(socket, reinterpret_cast<const sockaddr*>(&address), sizeof address) == 0;
            }
            if (!bound || listen(socket, SOMAXCONN) != 0) {
                in_use = address_in_use();
                close_socket(socket);
                return no_socket;
            }
            return socket;
        }
    } // namespace

    std::optional<Listen> parse_listen(std::string_view text) {
        Listen out;
        if (const auto colon = text.find(':'); colon != std::string_view::npos) {
            const auto address = text.substr(0, colon);
            if (address == constants::devtools::all_interfaces_address) {
                out.all_interfaces = true;
            } else if (address != constants::devtools::loopback_address) {
                return std::nullopt;
            }
            text.remove_prefix(colon + 1);
        }
        unsigned port{};
        const auto end = text.data() + text.size();
        const auto parsed = std::from_chars(text.data(), end, port);
        if (text.empty() || parsed.ec != std::errc{} || parsed.ptr != end ||
            port > std::numeric_limits<std::uint16_t>::max()) {
            return std::nullopt;
        }
        if (port != 0) {
            out.first_port = static_cast<std::uint16_t>(port);
        }
        return out;
    }

    struct Server::Client {
        Socket socket{no_socket};
        std::uint64_t id{};
        std::string in;
        std::string out;
        // A WebSocket's page, once the handshake is done.
        bool upgraded{};
        std::string target;
        // A message whose first frames came.
        std::string message;
        // To be closed once what is in out went.
        bool closing{};
        bool gone{};

        ~Client() {
            close_socket(socket);
        }
    };

    Server::Server(std::intptr_t listener, std::uint16_t port) : listener_(listener), port_(port) {
    }

    Result<std::shared_ptr<Server>> Server::start(const Listen& listen) {
        if (!start_sockets()) {
            return make_diagnostic(Diag::DevtoolsSocket, "the socket library did not start");
        }
        Socket socket = no_socket;
        std::uint16_t port = listen.first_port;
        bool in_use = false;
        for (std::uint32_t attempt = 0; attempt < listen.attempts; ++attempt) {
            const std::uint32_t candidate = std::uint32_t{listen.first_port} + attempt;
            if (candidate > std::numeric_limits<std::uint16_t>::max()) {
                break;
            }
            port = static_cast<std::uint16_t>(candidate);
            socket = listen_on(listen.all_interfaces, port, in_use);
            if (socket != no_socket || !in_use) {
                break;
            }
        }
        if (socket == no_socket) {
            return make_diagnostic(in_use ? Diag::DevtoolsPortInUse : Diag::DevtoolsSocket,
                                   "port " + std::to_string(listen.first_port));
        }
        make_nonblocking(socket);
        std::shared_ptr<Server> server(new Server(static_cast<std::intptr_t>(socket), port));
        server->thread_ = std::thread([raw = server.get()] {
            raw->serve();
        });
        return server;
    }

    Server::~Server() {
        stop();
    }

    void Server::stop() {
        if (std::exchange(stopped_, true)) {
            return;
        }
        stopping_.store(true, std::memory_order_relaxed);
        if (thread_.joinable()) {
            thread_.join();
        }
        close_socket(socket_of(listener_));
    }

    void Server::set_target(ServerTarget target) {
        const std::lock_guard lock(mutex_);
        for (auto& existing : targets_) {
            if (existing.id == target.id) {
                existing = std::move(target);
                return;
            }
        }
        targets_.push_back(std::move(target));
    }

    void Server::remove_target(std::string_view id) {
        const std::lock_guard lock(mutex_);
        std::erase_if(targets_, [&](const ServerTarget& target) {
            return target.id == id;
        });
        if (const auto it = events_.find(id); it != events_.end()) {
            events_.erase(it);
        }
    }

    std::vector<ServerEvent> Server::take_events(std::string_view target) {
        const std::lock_guard lock(mutex_);
        const auto it = events_.find(target);
        if (it == events_.end()) {
            return {};
        }
        auto out = std::move(it->second);
        it->second.clear();
        return out;
    }

    void Server::send(std::uint64_t connection, std::string text) {
        const std::lock_guard lock(mutex_);
        outgoing_.emplace_back(connection, std::move(text));
    }

    void Server::post(const std::string& target, ServerEvent event) {
        const std::lock_guard lock(mutex_);
        events_[target].push_back(std::move(event));
    }

    void Server::serve() {
        const Socket listener = socket_of(listener_);
        std::vector<std::unique_ptr<Client>> clients;
        std::uint64_t next_id = 1;
        std::vector<Poll> polls;
        std::array<char, 16384> chunk{};
        while (!stopping_.load(std::memory_order_relaxed)) {
            {
                const std::lock_guard lock(mutex_);
                for (auto& [connection, text] : outgoing_) {
                    for (auto& client : clients) {
                        if (client->id == connection && client->upgraded) {
                            client->out += frame(1, text);
                        }
                    }
                }
                outgoing_.clear();
            }
            polls.assign(1, Poll{listener, poll_in, 0});
            for (const auto& client : clients) {
                polls.push_back(Poll{client->socket, short(poll_in | (client->out.empty() ? 0 : poll_out)), 0});
            }
            poll_sockets(polls.data(), polls.size(),
                         clients.empty() ? constants::devtools::accept_poll_ms : constants::devtools::client_poll_ms);
            if (polls[0].revents & poll_in) {
                for (;;) {
                    const Socket accepted = accept(listener, nullptr, nullptr);
                    if (accepted == no_socket) {
                        break;
                    }
                    make_nonblocking(accepted);
                    no_sigpipe(accepted);
                    auto client = std::make_unique<Client>();
                    client->socket = accepted;
                    client->id = next_id++;
                    clients.push_back(std::move(client));
                }
            }
            // Those just accepted are not in polls: their turn is the next round.
            for (std::size_t i = 0; i + 1 < polls.size(); ++i) {
                auto& client = *clients[i];
                if (polls[i + 1].revents & (poll_in | POLLHUP | POLLERR)) {
                    for (;;) {
                        const auto received = recv(client.socket, chunk.data(), int(chunk.size()), 0);
                        if (received > 0) {
                            client.in.append(chunk.data(), std::size_t(received));
                            continue;
                        }
                        client.gone = received == 0 || !would_block();
                        break;
                    }
                }
                if (client.upgraded) {
                    take_frames(client);
                } else {
                    take_requests(client);
                }
                while (!client.out.empty()) {
                    const auto sent = ::send(client.socket, client.out.data(),
                                             int(std::min(client.out.size(), chunk.size())), send_flags());
                    if (sent > 0) {
                        client.out.erase(0, std::size_t(sent));
                        continue;
                    }
                    client.gone = client.gone || !would_block();
                    break;
                }
                client.gone = client.gone || (client.closing && client.out.empty()) ||
                              client.out.size() > constants::devtools::max_pending_bytes;
            }
            std::erase_if(clients, [&](const std::unique_ptr<Client>& client) {
                if (client->gone && client->upgraded) {
                    post(client->target, {ServerEvent::Kind::Closed, client->id, {}});
                }
                return client->gone;
            });
        }
    }

    void Server::take_requests(Client& client) {
        const auto end = client.in.find("\r\n\r\n");
        if (end == std::string::npos) {
            client.gone = client.gone || client.in.size() > constants::devtools::max_request_bytes;
            return;
        }
        const std::string head = client.in.substr(0, end);
        client.in.erase(0, end + 4);
        answer(client, head);
    }

    void Server::answer(Client& client, std::string_view head) {
        const auto respond = [&](std::string_view status, const std::string& body) {
            client.out +=
                "HTTP/1.1 " + std::string(status) +
                "\r\nContent-Type: application/json; charset=UTF-8\r\nContent-Length: " + std::to_string(body.size()) +
                "\r\nConnection: close\r\n\r\n" + body;
            client.closing = true;
        };
        const auto line_end = head.find("\r\n");
        const auto line = head.substr(0, line_end);
        const auto path_end = line.find(' ', 4);
        if (!line.starts_with("GET ") || path_end == std::string_view::npos) {
            respond("405 Method Not Allowed", "");
            return;
        }
        const auto path = line.substr(4, path_end - 4);
        std::string host = header(head, "host");
        if (host.empty()) {
            host = "127.0.0.1:" + std::to_string(port_);
        }
        constexpr std::string_view page_path = "/devtools/page/";
        if (path.starts_with(page_path)) {
            const std::string target(path.substr(page_path.size()));
            bool known{};
            {
                const std::lock_guard lock(mutex_);
                known = std::any_of(targets_.begin(), targets_.end(), [&](const ServerTarget& t) {
                    return t.id == target;
                });
            }
            const auto key = header(head, "sec-websocket-key");
            if (!allowed_origin(header(head, "origin"))) {
                respond("403 Forbidden", "");
            } else if (!known || key.empty()) {
                respond("404 Not Found", "");
            } else {
                client.out += "HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
                              "Sec-WebSocket-Accept: " +
                              base64(sha1(key + std::string(websocket_guid))) + "\r\n\r\n";
                client.upgraded = true;
                client.target = target;
                post(target, {ServerEvent::Kind::Opened, client.id, {}});
            }
            return;
        }
        if (path == "/json/version") {
            respond("200 OK",
                    Json(Json::Object{{"Browser", std::string("Weft/") + WEFT_VERSION}, {"Protocol-Version", "1.3"}})
                        .dump());
            return;
        }
        if (path == "/json" || path == "/json/" || path == "/json/list") {
            Json::Array list;
            const std::lock_guard lock(mutex_);
            for (const auto& target : targets_) {
                const std::string socket = host + std::string(page_path) + target.id;
                list.push_back(Json::Object{
                    {"id", target.id},
                    {"type", "page"},
                    {"title", target.title},
                    {"url", target.url},
                    {"webSocketDebuggerUrl", "ws://" + socket},
                    {"devtoolsFrontendUrl", "devtools://devtools/bundled/devtools_app.html?ws=" + socket}});
            }
            respond("200 OK", Json(std::move(list)).dump());
            return;
        }
        respond("404 Not Found", "");
    }

    void Server::take_frames(Client& client) {
        std::size_t p{};
        auto& in = client.in;
        const auto byte = [&](std::size_t at) {
            return static_cast<unsigned char>(in[at]);
        };
        while (in.size() - p >= 2) {
            const bool fin = byte(p) & 0x80;
            const unsigned opcode = byte(p) & 0x0F;
            const bool masked = byte(p + 1) & 0x80;
            std::uint64_t length = byte(p + 1) & 0x7F;
            std::size_t header_size = 2;
            if (length == 126) {
                header_size = 4;
            } else if (length == 127) {
                header_size = 10;
            }
            if (in.size() - p < header_size + 4) {
                break;
            }
            if (length == 126) {
                length = std::uint64_t(byte(p + 2)) << 8 | byte(p + 3);
            } else if (length == 127) {
                length = 0;
                for (unsigned i = 0; i < 8; ++i) {
                    length = length << 8 | byte(p + 2 + i);
                }
            }
            // A client masks what it sends (RFC 6455 section 5.1).
            if (!masked || length > constants::devtools::max_message_bytes) {
                client.closing = true;
                client.out += frame(8, "");
                in.clear();
                return;
            }
            if (in.size() - p < header_size + 4 + length) {
                break;
            }
            std::string payload = in.substr(p + header_size + 4, std::size_t(length));
            for (std::size_t i = 0; i < payload.size(); ++i) {
                payload[i] = char(payload[i] ^ in[p + header_size + (i & 3)]);
            }
            p += header_size + 4 + std::size_t(length);
            switch (opcode) {
                case 0:
                case 1:
                case 2:
                    client.message += payload;
                    if (client.message.size() > constants::devtools::max_message_bytes) {
                        client.closing = true;
                        client.message.clear();
                        break;
                    }
                    if (fin) {
                        post(client.target, {ServerEvent::Kind::Message, client.id, std::move(client.message)});
                        client.message.clear();
                    }
                    break;
                case 8:
                    client.out += frame(8, payload);
                    client.closing = true;
                    break;
                case 9:
                    client.out += frame(10, payload);
                    break;
                default:
                    break;
            }
        }
        in.erase(0, p);
    }

    std::shared_ptr<Server> start_from_environment() noexcept {
        try {
            const char* value = std::getenv(constants::devtools::listen_variable);
            if (!value || !*value) {
                return nullptr;
            }
            const auto listen = parse_listen(value);
            if (!listen) {
                std::fprintf(stderr, "[weft devtools] %s=%s is not [127.0.0.1:|0.0.0.0:]port\n",
                             constants::devtools::listen_variable, value);
                return nullptr;
            }
            auto server = Server::start(*listen);
            if (!server) {
                std::fprintf(stderr, "[weft devtools] not listening: %s\n", server.error().message.c_str());
                return nullptr;
            }
            std::fprintf(stderr, "[weft devtools] listening on %s:%u\n",
                         listen->all_interfaces ? constants::devtools::all_interfaces_address
                                                : constants::devtools::loopback_address,
                         unsigned{(*server)->port()});
            return std::move(*server);
        } catch (...) {
            return nullptr;
        }
    }
} // namespace weft::devtools
