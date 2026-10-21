#include "view/view_keys.hpp"

#include "script/script_dom.hpp"

#include <array>
#include <cstdint>
#include <string>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace weft {
    namespace {
        // A physical key (UI Events' code table, with Chromium's dom_code_data.inc for Windows): its US virtual key,
        // its set 1 scan code (0xE0xx extended), its key value when it types nothing, the characters a US layout
        // types with it (unshifted, shifted), and for a numeric keypad key what it is with Num Lock off.
        struct KeyInfo {
            std::string_view code;
            unsigned vk{};
            unsigned scan{};
            std::string_view name;
            std::string_view us;
            std::string_view us_shifted;
            std::string_view num_off;
            unsigned vk_num_off{};
        };

        constexpr KeyInfo keys[] = {
            {"Backquote", 192, 0x29, "", "`", "~"},
            {"Backslash", 220, 0x2B, "", "\\", "|"},
            {"BracketLeft", 219, 0x1A, "", "[", "{"},
            {"BracketRight", 221, 0x1B, "", "]", "}"},
            {"Comma", 188, 0x33, "", ",", "<"},
            {"Digit0", 48, 0x0B, "", "0", ")"},
            {"Digit1", 49, 0x02, "", "1", "!"},
            {"Digit2", 50, 0x03, "", "2", "@"},
            {"Digit3", 51, 0x04, "", "3", "#"},
            {"Digit4", 52, 0x05, "", "4", "$"},
            {"Digit5", 53, 0x06, "", "5", "%"},
            {"Digit6", 54, 0x07, "", "6", "^"},
            {"Digit7", 55, 0x08, "", "7", "&"},
            {"Digit8", 56, 0x09, "", "8", "*"},
            {"Digit9", 57, 0x0A, "", "9", "("},
            {"Equal", 187, 0x0D, "", "=", "+"},
            {"IntlBackslash", 226, 0x56, "", "\\", "|"},
            {"IntlRo", 193, 0x73, "", "/", "?"},
            {"KeyA", 65, 0x1E, "", "a", "A"},
            {"KeyB", 66, 0x30, "", "b", "B"},
            {"KeyC", 67, 0x2E, "", "c", "C"},
            {"KeyD", 68, 0x20, "", "d", "D"},
            {"KeyE", 69, 0x12, "", "e", "E"},
            {"KeyF", 70, 0x21, "", "f", "F"},
            {"KeyG", 71, 0x22, "", "g", "G"},
            {"KeyH", 72, 0x23, "", "h", "H"},
            {"KeyI", 73, 0x17, "", "i", "I"},
            {"KeyJ", 74, 0x24, "", "j", "J"},
            {"KeyK", 75, 0x25, "", "k", "K"},
            {"KeyL", 76, 0x26, "", "l", "L"},
            {"KeyM", 77, 0x32, "", "m", "M"},
            {"KeyN", 78, 0x31, "", "n", "N"},
            {"KeyO", 79, 0x18, "", "o", "O"},
            {"KeyP", 80, 0x19, "", "p", "P"},
            {"KeyQ", 81, 0x10, "", "q", "Q"},
            {"KeyR", 82, 0x13, "", "r", "R"},
            {"KeyS", 83, 0x1F, "", "s", "S"},
            {"KeyT", 84, 0x14, "", "t", "T"},
            {"KeyU", 85, 0x16, "", "u", "U"},
            {"KeyV", 86, 0x2F, "", "v", "V"},
            {"KeyW", 87, 0x11, "", "w", "W"},
            {"KeyX", 88, 0x2D, "", "x", "X"},
            {"KeyY", 89, 0x15, "", "y", "Y"},
            {"KeyZ", 90, 0x2C, "", "z", "Z"},
            {"Minus", 189, 0x0C, "", "-", "_"},
            {"Period", 190, 0x34, "", ".", ">"},
            {"Quote", 222, 0x28, "", "'", "\""},
            {"Semicolon", 186, 0x27, "", ";", ":"},
            {"Slash", 191, 0x35, "", "/", "?"},
            {"Space", 32, 0x39, "", " ", " "},
            {"Numpad0", 96, 0x52, "", "0", "0", "Insert", 45},
            {"Numpad1", 97, 0x4F, "", "1", "1", "End", 35},
            {"Numpad2", 98, 0x50, "", "2", "2", "ArrowDown", 40},
            {"Numpad3", 99, 0x51, "", "3", "3", "PageDown", 34},
            {"Numpad4", 100, 0x4B, "", "4", "4", "ArrowLeft", 37},
            {"Numpad5", 101, 0x4C, "", "5", "5", "Clear", 12},
            {"Numpad6", 102, 0x4D, "", "6", "6", "ArrowRight", 39},
            {"Numpad7", 103, 0x47, "", "7", "7", "Home", 36},
            {"Numpad8", 104, 0x48, "", "8", "8", "ArrowUp", 38},
            {"Numpad9", 105, 0x49, "", "9", "9", "PageUp", 33},
            {"NumpadDecimal", 110, 0x53, "", ".", ".", "Delete", 46},
            {"NumpadAdd", 107, 0x4E, "", "+", "+"},
            {"NumpadSubtract", 109, 0x4A, "", "-", "-"},
            {"NumpadMultiply", 106, 0x37, "", "*", "*"},
            {"NumpadDivide", 111, 0xE035, "", "/", "/"},
            {"NumpadEnter", 13, 0xE01C, "Enter"},
            {"Enter", 13, 0x1C, "Enter"},
            {"Tab", 9, 0x0F, "Tab"},
            {"Backspace", 8, 0x0E, "Backspace"},
            {"Escape", 27, 0x01, "Escape"},
            {"CapsLock", 20, 0x3A, "CapsLock"},
            {"ShiftLeft", 16, 0x2A, "Shift"},
            {"ShiftRight", 16, 0x36, "Shift"},
            {"ControlLeft", 17, 0x1D, "Control"},
            {"ControlRight", 17, 0xE01D, "Control"},
            {"AltLeft", 18, 0x38, "Alt"},
            {"AltRight", 18, 0xE038, "Alt"},
            {"MetaLeft", 91, 0xE05B, "Meta"},
            {"MetaRight", 92, 0xE05C, "Meta"},
            {"ContextMenu", 93, 0xE05D, "ContextMenu"},
            {"Insert", 45, 0xE052, "Insert"},
            {"Delete", 46, 0xE053, "Delete"},
            {"Home", 36, 0xE047, "Home"},
            {"End", 35, 0xE04F, "End"},
            {"PageUp", 33, 0xE049, "PageUp"},
            {"PageDown", 34, 0xE051, "PageDown"},
            {"ArrowUp", 38, 0xE048, "ArrowUp"},
            {"ArrowDown", 40, 0xE050, "ArrowDown"},
            {"ArrowLeft", 37, 0xE04B, "ArrowLeft"},
            {"ArrowRight", 39, 0xE04D, "ArrowRight"},
            {"NumLock", 144, 0xE045, "NumLock"},
            {"ScrollLock", 145, 0x46, "ScrollLock"},
            {"Pause", 19, 0x45, "Pause"},
            {"PrintScreen", 44, 0xE037, "PrintScreen"},
            {"F1", 112, 0x3B, "F1"},
            {"F2", 113, 0x3C, "F2"},
            {"F3", 114, 0x3D, "F3"},
            {"F4", 115, 0x3E, "F4"},
            {"F5", 116, 0x3F, "F5"},
            {"F6", 117, 0x40, "F6"},
            {"F7", 118, 0x41, "F7"},
            {"F8", 119, 0x42, "F8"},
            {"F9", 120, 0x43, "F9"},
            {"F10", 121, 0x44, "F10"},
            {"F11", 122, 0x57, "F11"},
            {"F12", 123, 0x58, "F12"},
            {"F13", 124, 0x64, "F13"},
            {"F14", 125, 0x65, "F14"},
            {"F15", 126, 0x66, "F15"},
            {"F16", 127, 0x67, "F16"},
            {"F17", 128, 0x68, "F17"},
            {"F18", 129, 0x69, "F18"},
            {"F19", 130, 0x6A, "F19"},
            {"F20", 131, 0x6B, "F20"},
            {"F21", 132, 0x6C, "F21"},
            {"F22", 133, 0x6D, "F22"},
            {"F23", 134, 0x6E, "F23"},
            {"F24", 135, 0x76, "F24"},
            {"Convert", 28, 0x79, "Convert"},
            {"NonConvert", 29, 0x7B, "NonConvert"},
            {"KanaMode", 21, 0x70, "KanaMode"},
            {"Lang1", 21, 0x72, "HangulMode"},
            {"Lang2", 25, 0x71, "HanjaMode"},
            {"AudioVolumeMute", 173, 0xE020, "AudioVolumeMute"},
            {"AudioVolumeDown", 174, 0xE02E, "AudioVolumeDown"},
            {"AudioVolumeUp", 175, 0xE030, "AudioVolumeUp"},
            {"MediaTrackNext", 176, 0xE019, "MediaTrackNext"},
            {"MediaTrackPrevious", 177, 0xE010, "MediaTrackPrevious"},
            {"MediaStop", 178, 0xE024, "MediaStop"},
            {"MediaPlayPause", 179, 0xE022, "MediaPlayPause"},
            {"BrowserBack", 166, 0xE06A, "BrowserBack"},
            {"BrowserForward", 167, 0xE069, "BrowserForward"},
        };

        const KeyInfo* find(std::string_view code) {
            for (const auto& k : keys) {
                if (k.code == code) {
                    return &k;
                }
            }
            return nullptr;
        }

        bool letter(const KeyInfo& k) {
            return k.code.size() == 4 && k.code.starts_with("Key");
        }

        // What a US layout types with the key.
        [[maybe_unused]] std::string us_key(const KeyInfo& k, unsigned modifiers) {
            if (!k.num_off.empty() && !(modifiers & key_modifier::num_lock)) {
                return std::string(k.num_off);
            }
            bool shifted = modifiers & key_modifier::shift;
            if (letter(k) && (modifiers & key_modifier::caps_lock)) {
                shifted = !shifted;
            }
            return std::string(shifted ? k.us_shifted : k.us);
        }

#ifdef _WIN32
        void append_utf8(std::string& out, char32_t c) {
            if (c < 0x80) {
                out += char(c);
            } else if (c < 0x800) {
                out += char(0xC0 | (c >> 6));
                out += char(0x80 | (c & 0x3F));
            } else if (c < 0x10000) {
                out += char(0xE0 | (c >> 12));
                out += char(0x80 | ((c >> 6) & 0x3F));
                out += char(0x80 | (c & 0x3F));
            } else {
                out += char(0xF0 | (c >> 18));
                out += char(0x80 | ((c >> 12) & 0x3F));
                out += char(0x80 | ((c >> 6) & 0x3F));
                out += char(0x80 | (c & 0x3F));
            }
        }

        HKL foreground_layout() {
            return GetKeyboardLayout(GetWindowThreadProcessId(GetForegroundWindow(), nullptr));
        }

        // What the foreground window's keyboard layout types with the key (Chromium's PlatformKeyMap asks ToUnicodeEx
        // the same way); none when the layout types nothing with it.
        std::optional<std::string> layout_text(const KeyInfo& k, unsigned modifiers) {
            const HKL layout = foreground_layout();
            // A numeric keypad key's virtual key depends on Num Lock, not on its scan code.
            const UINT vk = k.code.starts_with("Numpad") ? k.vk : MapVirtualKeyExW(k.scan, MAPVK_VSC_TO_VK_EX, layout);
            if (!vk) {
                return std::nullopt;
            }
            BYTE state[256]{};
            if (modifiers & key_modifier::shift) {
                state[VK_SHIFT] = state[VK_LSHIFT] = 0x80;
            }
            if (modifiers & key_modifier::alt_graph) {
                state[VK_CONTROL] = state[VK_LCONTROL] = state[VK_MENU] = state[VK_RMENU] = 0x80;
            }
            if (modifiers & key_modifier::caps_lock) {
                state[VK_CAPITAL] = 0x01;
            }
            if (modifiers & key_modifier::num_lock) {
                state[VK_NUMLOCK] = 0x01;
            }
            wchar_t text[8]{};
            // Flag 4 (Windows 10 1607 and later): the layout's dead-key state stays as it was.
            const int n = ToUnicodeEx(vk, k.scan & 0xFF, state, text, 8, 4, layout);
            if (n < 0) {
                return std::string("Dead");
            }
            if (n == 0) {
                return std::nullopt;
            }
            std::string out;
            for (int i = 0; i < n; ++i) {
                char32_t c = text[i];
                if (c >= 0xD800 && c <= 0xDBFF && i + 1 < n && text[i + 1] >= 0xDC00 && text[i + 1] <= 0xDFFF) {
                    c = 0x10000 + ((c - 0xD800) << 10) + (char32_t(text[++i]) - 0xDC00);
                }
                if (c < 0x20 || c == 0x7F) {
                    return std::nullopt;
                }
                append_utf8(out, c);
            }
            return out;
        }
#endif
    } // namespace

    bool known_key_code(std::string_view code) {
        return find(code) != nullptr;
    }

    unsigned key_location(std::string_view code) {
        if (code == "ShiftLeft" || code == "ControlLeft" || code == "AltLeft" || code == "MetaLeft") {
            return 1;
        }
        if (code == "ShiftRight" || code == "ControlRight" || code == "AltRight" || code == "MetaRight") {
            return 2;
        }
        return code.starts_with("Numpad") ? 3 : 0;
    }

    unsigned windows_key_code(std::string_view code) {
        const auto* k = find(code);
        return k ? k->vk : 0;
    }

    unsigned layout_key_code(std::string_view code, unsigned modifiers) {
        const auto* k = find(code);
        if (!k) {
            return 0;
        }
        if (k->vk_num_off && !(modifiers & key_modifier::num_lock)) {
            return k->vk_num_off;
        }
#ifdef _WIN32
        if (!k->us.empty() && !k->code.starts_with("Numpad")) {
            if (const UINT vk = MapVirtualKeyExW(k->scan, MAPVK_VSC_TO_VK_EX, foreground_layout())) {
                return vk;
            }
        }
#endif
        return k->vk;
    }

    std::optional<std::string_view> physical_code(unsigned virtual_key) {
        switch (virtual_key) {
            case 0xA0: // VK_LSHIFT
                return "ShiftLeft";
            case 0xA1:
                return "ShiftRight";
            case 0xA2: // VK_LCONTROL
                return "ControlLeft";
            case 0xA3:
                return "ControlRight";
            case 0xA4: // VK_LMENU
                return "AltLeft";
            case 0xA5:
                return "AltRight";
            default:
                break;
        }
#ifdef _WIN32
        // Digits, letters and the OEM keys type characters: they move between layouts.
        const unsigned vk = virtual_key;
        if ((vk >= '0' && vk <= '9') || (vk >= 'A' && vk <= 'Z') || (vk >= 0xBA && vk <= 0xC1) || (vk >= 0xDB && vk <= 0xDF) ||
            vk == 0xE2) {
            if (const UINT scan = MapVirtualKeyExW(vk, MAPVK_VK_TO_VSC_EX, foreground_layout())) {
                for (const auto& k : keys) {
                    if (k.scan == scan) {
                        return k.code;
                    }
                }
            }
        }
#endif
        // A numeric keypad key only for a virtual key of its own (VK_RETURN is Enter, not NumpadEnter).
        const KeyInfo* keypad = nullptr;
        for (const auto& k : keys) {
            if (k.vk != virtual_key) {
                continue;
            }
            if (!k.code.starts_with("Numpad")) {
                return k.code;
            }
            if (!keypad) {
                keypad = &k;
            }
        }
        return keypad ? std::optional<std::string_view>(keypad->code) : std::nullopt;
    }

    std::optional<std::string> layout_key(std::string_view code, unsigned modifiers) {
        const auto* k = find(code);
        if (!k) {
            return std::nullopt;
        }
        if (!k->name.empty()) {
            return std::string(k->name);
        }
        if (!k->num_off.empty() && !(modifiers & key_modifier::num_lock)) {
            return std::string(k->num_off);
        }
#ifdef _WIN32
        if (auto text = layout_text(*k, modifiers)) {
            return text;
        }
        return std::string("Unidentified");
#else
        return us_key(*k, modifiers);
#endif
    }
} // namespace weft
