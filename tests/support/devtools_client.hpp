#pragma once

#include "devtools/json.hpp"
#include "check.hpp"

#include <cstdint>
#include <functional>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

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
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

// The developer tools' socket from a tool's side (docs/devtools.md), for the tests of a dev build. POSIX sockets, or
// Winsock.
namespace weft::testing {
    using weft::devtools::Json;

    // The few socket calls a tool makes, the same on both.
    namespace devtools_socket {
#ifdef _WIN32
        using Socket = SOCKET;
        constexpr Socket none = INVALID_SOCKET;

        // Winsock is started once a process and left for its end, as the engine's server does.
        inline void start() {
            static const bool started = [] {
                WSADATA data;
                return WSAStartup(MAKEWORD(2, 2), &data) == 0;
            }();
            ::weft::testing::require(started, "WSAStartup");
        }

        inline void close(Socket socket) {
            closesocket(socket);
        }

        // Whether a read would not block: bytes came, or the other side closed (Winsock says so apart).
        inline bool readable(Socket socket, int ms) {
            WSAPOLLFD entry{socket, POLLRDNORM, 0};
            return WSAPoll(&entry, 1, ms) > 0 && (entry.revents & (POLLRDNORM | POLLHUP | POLLERR)) != 0;
        }

        inline long long send_some(Socket socket, const char* bytes, std::size_t size) {
            return ::send(socket, bytes, int(size), 0);
        }

        inline long long receive(Socket socket, char* bytes, std::size_t size) {
            return ::recv(socket, bytes, int(size), 0);
        }
#else
        using Socket = int;
        constexpr Socket none = -1;

        inline void start() {
        }

        inline void close(Socket socket) {
            ::close(socket);
        }

        inline bool readable(Socket socket, int ms) {
            pollfd entry{socket, POLLIN, 0};
            return ::poll(&entry, 1, ms) > 0 && (entry.revents & POLLIN) != 0;
        }

        inline long long send_some(Socket socket, const char* bytes, std::size_t size) {
            return ::send(socket, bytes, size, 0);
        }

        inline long long receive(Socket socket, char* bytes, std::size_t size) {
            return ::recv(socket, bytes, size, 0);
        }
#endif
    } // namespace devtools_socket

    // A developer tool's connection to a dev build's socket: the WebSocket handshake and text frames, and the
    // protocol's calls and events. `step` runs the page and the tools' turn between reads: everything happens on the
    // test's one thread.
    class DevtoolsClient {
    public:
        DevtoolsClient(std::function<void()> step_page, std::uint16_t port, std::string_view path,
                       std::string_view extra_header = {})
            : step_page_(std::move(step_page)) {
            socket_ = connect_to(port);
            std::string request = "GET " + std::string(path) +
                                  " HTTP/1.1\r\nHost: 127.0.0.1\r\nUpgrade: websocket\r\n"
                                  "Connection: Upgrade\r\nSec-WebSocket-Version: 13\r\n"
                                  "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n";
            request += extra_header;
            request += "\r\n";
            write_all(request);
            while (head_.find("\r\n\r\n") == std::string::npos && !closed_) {
                step();
            }
        }

        ~DevtoolsClient() {
            devtools_socket::close(socket_);
        }

        const std::string& head() const {
            return head_;
        }

        Json call(std::string_view method, Json::Object params = {}) {
            const double id = ++next_;
            write_frame(Json(Json::Object{{"id", id}, {"method", method}, {"params", std::move(params)}}).dump());
            for (int i = 0; i < 4000; ++i) {
                for (auto it = inbox_.begin(); it != inbox_.end(); ++it) {
                    if (it->number_at("id", -1) == id) {
                        auto out = std::move(*it);
                        inbox_.erase(it);
                        return out;
                    }
                }
                step();
            }
            throw std::runtime_error("no answer to " + std::string(method));
        }

        // A result, which must not be an error.
        Json result(std::string_view method, Json::Object params = {}) {
            auto reply = call(method, std::move(params));
            ::weft::testing::require(!reply.find("error"), std::string(method) + " failed: " + reply.dump());
            return *reply.find("result");
        }

        void raw(const std::string& text) {
            write_frame(text);
        }

        void settle(int frames = 12) {
            for (int i = 0; i < frames; ++i) {
                step();
            }
        }

        // The first event of a method whose text holds something, taken off the list; none after a while.
        std::optional<Json> take(std::string_view method, std::string_view holding = {}) {
            for (int i = 0; i < 1500; ++i) {
                for (auto it = inbox_.begin(); it != inbox_.end(); ++it) {
                    if (it->string_at("method") == method &&
                        (holding.empty() || it->dump().find(holding) != std::string::npos)) {
                        auto out = std::move(*it);
                        inbox_.erase(it);
                        return out;
                    }
                }
                step();
            }
            return std::nullopt;
        }

        void clear() {
            inbox_.clear();
        }

        // Whether an event of a method whose text holds something has come, without waiting for one.
        bool has(std::string_view method, std::string_view holding = {}) const {
            for (const auto& message : inbox_) {
                if (message.string_at("method") == method &&
                    (holding.empty() || message.dump().find(holding) != std::string::npos)) {
                    return true;
                }
            }
            return false;
        }

        static devtools_socket::Socket connect_to(std::uint16_t port) {
            devtools_socket::start();
            const devtools_socket::Socket s = socket(AF_INET, SOCK_STREAM, 0);
            ::weft::testing::require(s != devtools_socket::none, "socket");
            sockaddr_in address{};
            address.sin_family = AF_INET;
            address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
            address.sin_port = htons(port);
            ::weft::testing::require(connect(s, reinterpret_cast<const sockaddr*>(&address), sizeof address) == 0,
                                     "connect");
            return s;
        }

    private:
        void write_all(std::string_view bytes) {
            while (!bytes.empty()) {
                const auto n = devtools_socket::send_some(socket_, bytes.data(), bytes.size());
                ::weft::testing::require(n > 0, "send");
                bytes.remove_prefix(std::size_t(n));
            }
        }

        void write_frame(const std::string& text) {
            std::string frame;
            // FIN and a text frame; then the mask bit with the length (126: 16 bits follow, 127: 64 bits).
            frame += '\x81';
            if (text.size() < 126) {
                frame += char(0x80 | text.size());
            } else if (text.size() <= 0xFFFF) {
                frame += '\xFE';
                frame += char(text.size() >> 8);
                frame += char(text.size() & 0xFF);
            } else {
                frame += '\xFF';
                for (int shift = 56; shift >= 0; shift -= 8) {
                    frame += char(std::uint64_t(text.size()) >> shift & 0xFF);
                }
            }
            const char mask[4] = {0x11, 0x22, 0x33, 0x44};
            frame.append(mask, 4);
            for (std::size_t i = 0; i < text.size(); ++i) {
                frame += char(text[i] ^ mask[i & 3]);
            }
            write_all(frame);
        }

        void step() {
            step_page_();
            if (devtools_socket::readable(socket_, 2)) {
                char chunk[65536];
                const auto n = devtools_socket::receive(socket_, chunk, sizeof chunk);
                if (n <= 0) {
                    closed_ = true;
                    return;
                }
                buffer_.append(chunk, std::size_t(n));
            }
            if (head_.find("\r\n\r\n") == std::string::npos) {
                const auto end = buffer_.find("\r\n\r\n");
                if (end == std::string::npos) {
                    return;
                }
                head_ = buffer_.substr(0, end + 4);
                buffer_.erase(0, end + 4);
            }
            for (;;) {
                if (buffer_.size() < 2) {
                    return;
                }
                const auto byte = [&](std::size_t at) {
                    return static_cast<unsigned char>(buffer_[at]);
                };
                std::uint64_t length = byte(1) & 0x7F;
                std::size_t header = 2;
                if (length == 126) {
                    if (buffer_.size() < 4) {
                        return;
                    }
                    length = std::uint64_t(byte(2)) << 8 | byte(3);
                    header = 4;
                } else if (length == 127) {
                    if (buffer_.size() < 10) {
                        return;
                    }
                    length = 0;
                    for (unsigned i = 0; i < 8; ++i) {
                        length = length << 8 | byte(2 + i);
                    }
                    header = 10;
                }
                if (buffer_.size() < header + length) {
                    return;
                }
                const auto parsed = Json::parse(std::string_view(buffer_).substr(header, std::size_t(length)));
                ::weft::testing::require(parsed.has_value(), "a message that is not JSON");
                inbox_.push_back(*parsed);
                buffer_.erase(0, header + std::size_t(length));
            }
        }

        std::function<void()> step_page_;
        devtools_socket::Socket socket_{devtools_socket::none};
        std::string head_;
        std::string buffer_;
        std::vector<Json> inbox_;
        double next_{};
        bool closed_{};
    };

    // What the socket answers a GET of a path with, head and all.
    inline std::string devtools_http_get(std::uint16_t port, std::string_view path) {
        const auto s = DevtoolsClient::connect_to(port);
        const std::string request =
            "GET " + std::string(path) + " HTTP/1.1\r\nHost: 127.0.0.1:" + std::to_string(port) + "\r\n\r\n";
        ::weft::testing::require(devtools_socket::send_some(s, request.data(), request.size()) ==
                                     static_cast<long long>(request.size()),
                                 "send");
        std::string out;
        char chunk[4096];
        for (;;) {
            const auto n = devtools_socket::receive(s, chunk, sizeof chunk);
            if (n <= 0) {
                break;
            }
            out.append(chunk, std::size_t(n));
        }
        devtools_socket::close(s);
        return out;
    }
} // namespace weft::testing
