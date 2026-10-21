#pragma once

#include "constants/common.hpp"

#include <cstddef>

// Pointer and keyboard input limits and steps. Each constant has the type of the literal it replaced.
namespace weft::constants::input {
    // MouseEvent.buttons bit of the primary (usually left) button.
    inline constexpr int primary_button_mask = 1;
    // Largest absolute wheel delta, in logical pixels, accepted by the router.
    inline constexpr int max_wheel_delta_px = constants::max_logical_px_magnitude;
    // Keyboard scrolling as Blink steps it: an arrow key moves a line of 40 CSS px (kPixelsPerLineStep), Page Up,
    // Page Down and Space seven eighths of the scrollport (kMinFractionToStepWhenPaging).
    inline constexpr double keyboard_line_step = 40;
    inline constexpr double keyboard_page_fraction = 0.875;
    // The legacy keyCode of a key an input method takes (Windows' VK_PROCESSKEY), with KeyboardEvent.key "Process".
    inline constexpr unsigned ime_process_key_code = 229;
    // Longest key and code values a host may give, in bytes.
    inline constexpr std::size_t max_key_bytes = 64;
    // Longest text a host may give in one call, in bytes.
    inline constexpr std::size_t max_text_bytes = 4096;
    // Clicks (Windows' defaults, by which Chrome counts them): a press counts on from the last one when it comes within
    // this many ms of it and this many px of it on each axis.
    inline constexpr double double_click_ms = 500;
    inline constexpr float double_click_px = 2;
    // A text field's caret blinks: shown, then hidden, this many ms each (Windows' GetCaretBlinkTime default).
    inline constexpr double caret_blink_ms = 530;
} // namespace weft::constants::input
