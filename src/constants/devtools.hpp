#pragma once

#include <cstddef>
#include <cstdint>

// The developer tools' listening socket (src/devtools, docs/devtools.md).
namespace weft::constants::devtools {
    // The environment variable that starts the tools with no change to the host: [address:]port.
    inline constexpr const char* listen_variable = "WEFT_DEVTOOLS_LISTEN";
    // The address that listens on every interface, as that variable writes it.
    inline constexpr const char* all_interfaces_address = "0.0.0.0";
    inline constexpr const char* loopback_address = "127.0.0.1";

    // The first port tried. Chrome's remote debugging takes 9222 and Node's inspector 9229.
    inline constexpr std::uint16_t default_port = 9230;
    // Ports tried from the first, for several runtimes (or several games) on one machine.
    inline constexpr std::uint16_t port_attempts = 16;

    // How often the socket thread looks whether it is to stop, with no client and with one: what closing a runtime can
    // wait for, and what a message waits to go out.
    inline constexpr int accept_poll_ms = 50;
    inline constexpr int client_poll_ms = 2;
    // The most of a request's head that is read, and of a message that is taken or left waiting for a slow client.
    inline constexpr std::size_t max_request_bytes = 8192;
    inline constexpr std::size_t max_message_bytes = std::size_t(64) << 20;
    inline constexpr std::size_t max_pending_bytes = std::size_t(256) << 20;

    // Diagnostics a View keeps for tools that connect late.
    inline constexpr std::size_t max_log_entries = 1000;
    // How often a changed document is compared with what the tools were sent (ms of the page's clock).
    inline constexpr double mutation_interval_ms = 100;
} // namespace weft::constants::devtools
