#pragma once

#include "devtools/json.hpp"
#include "check.hpp"

#include <arpa/inet.h>
#include <cstdint>
#include <functional>
#include <netinet/in.h>
#include <optional>
#include <poll.h>
#include <stdexcept>
#include <string>
#include <string_view>
#include <sys/socket.h>
#include <unistd.h>
#include <utility>
#include <vector>

// The developer tools' socket from a tool's side (docs/devtools.md), for the tests of a dev build. POSIX sockets.
namespace weft::testing {
    using weft::devtools::Json;

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
            close(socket_);
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

        static int connect_to(std::uint16_t port) {
            const int s = socket(AF_INET, SOCK_STREAM, 0);
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
                const auto n = ::send(socket_, bytes.data(), bytes.size(), 0);
                ::weft::testing::require(n > 0, "send");
                bytes.remove_prefix(std::size_t(n));
            }
        }

        void write_frame(const std::string& text) {
            std::string frame;
            frame += char(0x81);
            if (text.size() < 126) {
                frame += char(0x80 | text.size());
            } else if (text.size() <= 0xFFFF) {
                frame += char(0x80 | 126);
                frame += char(text.size() >> 8);
                frame += char(text.size() & 0xFF);
            } else {
                frame += char(0x80 | 127);
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
            pollfd entry{socket_, POLLIN, 0};
            poll(&entry, 1, 2);
            if (entry.revents & POLLIN) {
                char chunk[65536];
                const auto n = recv(socket_, chunk, sizeof chunk, 0);
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
        int socket_{-1};
        std::string head_;
        std::string buffer_;
        std::vector<Json> inbox_;
        double next_{};
        bool closed_{};
    };

    // What the socket answers a GET of a path with, head and all.
    inline std::string devtools_http_get(std::uint16_t port, std::string_view path) {
        const int s = DevtoolsClient::connect_to(port);
        const std::string request =
            "GET " + std::string(path) + " HTTP/1.1\r\nHost: 127.0.0.1:" + std::to_string(port) + "\r\n\r\n";
        ::weft::testing::require(::send(s, request.data(), request.size(), 0) == ssize_t(request.size()), "send");
        std::string out;
        char chunk[4096];
        for (;;) {
            const auto n = recv(s, chunk, sizeof chunk, 0);
            if (n <= 0) {
                break;
            }
            out.append(chunk, std::size_t(n));
        }
        close(s);
        return out;
    }
} // namespace weft::testing
