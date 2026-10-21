#pragma once

#include <optional>
#include <string>
#include <string_view>

// The values of keyboard events (UI Events: KeyboardEvent.key and code, and the legacy keyCode) for a host's keys.
namespace weft {
    // Whether code names a physical key of UI Events' code table that weft knows ("KeyA", "Enter", "NumpadEnter").
    bool known_key_code(std::string_view code);
    // KeyboardEvent.location of a physical key: left or right for the modifier keys that come in pairs, the numeric
    // keypad for its keys, standard otherwise (UI Events §5.6.1.1).
    unsigned key_location(std::string_view code);
    // KeyboardEvent.keyCode of keydown and keyup as Chrome on Windows gives it: the Windows virtual key of the physical
    // key on a US keyboard (a numeric keypad key's with Num Lock on); 0 for a code weft does not know.
    unsigned windows_key_code(std::string_view code);
    // The same as the system gives it: a numeric keypad key with Num Lock off (key_modifier::num_lock) has the virtual
    // key of what it is then (Numpad1 is End, 35), and on Windows a key that types a character has the virtual key the
    // foreground window's layout maps its scan code to (AZERTY's KeyQ is VK_A, 65), as the keys Chrome is given; the
    // other keys, and other platforms, keep windows_key_code's.
    unsigned layout_key_code(std::string_view code, unsigned modifiers);
    // The key value the system's keyboard layout gives the physical key code with the modifiers (key_modifier: Shift,
    // AltGraph, CapsLock, NumLock): the character it types ("a", "A", "!", "é"), "Dead" for a dead key, or its name for a
    // key that types nothing ("Enter", "ArrowLeft", "Shift", "F5"). Windows asks the layout of the foreground window's
    // thread without changing its dead-key state; other platforms answer for a US layout. None for an unknown code.
    std::optional<std::string> layout_key(std::string_view code, unsigned modifiers);
} // namespace weft
