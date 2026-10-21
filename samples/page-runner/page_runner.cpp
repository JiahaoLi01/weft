// A page of a game's UI run without the game: a View through the public C headers only (include/weft), driven by a
// timeline of what the engine would do (events, view models, anchors, pointer input, resizes and replies to the page's
// calls), with every diagnostic and message of the page printed as it comes and frames drawn on D3D11 into PNG files.
// It is how a port of a game's UI onto weft finds what the page needs, frame by frame, before the engine runs it.
//
//   weft_page_runner --page <dir> [--timeline <file.json>] [--fonts <file.json>] [--size 1920x1080] [--scale 1]
//                    [--fps 60] [--frames N] [--out <dir>] [--warp] [--background #rrggbb] [--render-every N]
//                    [--storage <file.json>] [--slow 100] [--effects <file.json>]
//
// --slow prints each frame whose advance, or whose draw (the renderer's prepare and encode on the CPU, with
// --render-every or a shot), took longer than that many ms.
//
// The page is a folder as hosts load it: page.json ({"scripts": [...], "stylesheet": "...", "keyframes": "...", "root":
// "...", "crpgBridge": false, "forgivingCss": false, "webPlatform": false}, paths relative to the folder), or page.js
// with page.css and keyframes.css. Its base (for url() images) is the stylesheet's path; the runner reads those files
// from disk. --storage names a JSON object of strings: the View's localStorage before the page loads, written back
// when the page changed it.
//
// The timeline ({"frames": N, "replies": {...}, "steps": [...]}) says what happens at which frame (frames are 1/fps s):
//   {"frame": 0, "emit": "name", "value": <json>, "latest": false}     an engine event
//   {"frame": 0, "model": "name", "fields": {...}}                      a view model's fields
//   {"frame": 0, "anchors": [{"key": 1, "x": 0, "y": 0, "scale": 1, "depth": 0, "flags": 0}]}
//   {"frame": 0, "pointer": "move|down|up|leave", "x": 0, "y": 0, "buttons": 0}
//   {"frame": 0, "wheel": [dx, dy], "x": 0, "y": 0}
//   {"frame": 0, "key": "KeyA", "action": "press", "modifiers": "S", "value": "A", "text": "A", "repeat": false,
//    "ime": false}                                                     a key by its code: down, up, or press (down, its
//                                                                      text, up); modifiers of S(hift), C(ontrol), A(lt),
//                                                                      M(eta), G (AltGraph); the key value weft_keyboard_key
//                                                                      gives unless "value" says; the text the key's value
//                                                                      when one character, unless "text" says
//   {"frame": 0, "text": "你好"}                                         text typed without a key
//   {"frame": 0, "focus": false}                                        the host's keyboard focus leaves (true: comes back)
//   {"frame": 0, "hit": [x, y]}                                         prints what is under a point
//   {"frame": 0, "resize": [width, height, scale]}
//   {"frame": 0, "shot": "name.png"}                                    draws the frame into <out>/name.png
//   {"frame": 0, "reload": true}                                        loads the page again
//   {"frame": 0, "eval": "document.title"}                               prints String() of a script's value
//   {"frame": 0, "storage": true}                                       prints the View's localStorage
// "replies" answers the page's weft.engine.call by name: {"name": {"ok": true, "value": <json>}}; an unanswered call
// is printed and left open.
//
// The fonts file lists faces the runtime gets before the page loads, paths relative to the file:
//   {"faces": [{"family": "Noto Serif SC", "path": "fonts/NotoSerifSC.ttf", "weight": 400, "italic": false,
//               "unicodeRange": "U+0-10FFFF",
//               "face": 0}], "fallback": ["Noto Serif SC"]}
//
// The effects file lists the UI shader effects the runtime gets (cmake/compile_effect.cmake compiled their bytecode to
// <bytecode>.encoded.dxbc, .linear.dxbc and the .spv ones), paths relative to the file:
//   {"effects": [{"name": "crpg-smoke", "bytecode": "fx/crpg-smoke", "animated": true,
//                 "parameters": [{"name": "smoke-scales", "type": "vec4", "initial": "7.2 2.8 0.1 0.3"}],
//                 "textures": [{"name": "cloud", "path": "cloud.png", "format": "data"}]}]}
// (type number, vec2, vec3, vec4 or color; a texture's format data, its PNG's red channel, or srgb). An effect that
// reads what lies behind its element says "backdrop": true, and "backdropMargin": the CSS px past its box it reads
// (docs/effects.md).
//
// Prints [load], [diag:<kind>], [send], [call], [hit], [shot] and [render] lines, then the advance times. Exits 0 when
// the page loaded, 1 when it did not (or an argument was wrong), 77 without a D3D11 device for a shot.
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <d3d11.h>

#include <weft/render_backend.h>
#include <weft/render_backend_d3d11.h>
#include <weft/weft.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <deque>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {
    constexpr int skipped = 77;

    struct Failure : std::runtime_error {
        using std::runtime_error::runtime_error;
    };

    // UTF-8 text as a path, and a path as UTF-8 text (C++20 spells UTF-8 char8_t).
    std::filesystem::path utf8_path(std::string_view text) {
        return std::filesystem::path(std::u8string(text.begin(), text.end()));
    }

    std::string utf8(const std::filesystem::path& path) {
        const std::u8string text = path.generic_u8string();
        return std::string(text.begin(), text.end());
    }

    std::string read(const std::filesystem::path& path) {
        std::ifstream in(path, std::ios::binary);
        if (!in) {
            throw Failure("cannot read " + path.string());
        }
        return {std::istreambuf_iterator<char>(in), {}};
    }

    // ---- JSON: what the page folder, the timeline and the fonts file are written in. Each value keeps its own text,
    // which the engine hands weft as it is (WEFT_VALUE_JSON). ----

    struct Json {
        enum class Kind {
            Null,
            Bool,
            Number,
            String,
            Array,
            Object
        } kind = Kind::Null;
        bool boolean{};
        double number{};
        std::string string;
        std::vector<Json> items;
        std::vector<std::pair<std::string, Json>> members;
        std::string text;

        const Json* find(std::string_view key) const {
            for (const auto& [k, v] : members) {
                if (k == key) {
                    return &v;
                }
            }
            return nullptr;
        }

        std::string get_string(std::string_view key, std::string fallback = {}) const {
            const Json* v = find(key);
            return v && v->kind == Kind::String ? v->string : fallback;
        }

        double get_number(std::string_view key, double fallback = 0) const {
            const Json* v = find(key);
            return v && v->kind == Kind::Number ? v->number : fallback;
        }

        bool get_bool(std::string_view key, bool fallback = false) const {
            const Json* v = find(key);
            return v && v->kind == Kind::Bool ? v->boolean : fallback;
        }
    };

    class JsonParser {
    public:
        explicit JsonParser(std::string_view text) : text_(text) {
        }

        Json parse() {
            Json value = parse_value();
            skip();
            if (at_ != text_.size()) {
                fail("trailing text");
            }
            return value;
        }

    private:
        [[noreturn]] void fail(const char* what) const {
            throw Failure("JSON: " + std::string(what) + " at offset " + std::to_string(at_));
        }

        void skip() {
            while (at_ < text_.size() &&
                   (text_[at_] == ' ' || text_[at_] == '\n' || text_[at_] == '\r' || text_[at_] == '\t')) {
                ++at_;
            }
        }

        static void put_utf8(std::string& out, std::uint32_t c) {
            if (c < 0x80) {
                out += char(c);
            } else if (c < 0x800) {
                out += char(0xc0 | (c >> 6));
                out += char(0x80 | (c & 0x3f));
            } else if (c < 0x10000) {
                out += char(0xe0 | (c >> 12));
                out += char(0x80 | ((c >> 6) & 0x3f));
                out += char(0x80 | (c & 0x3f));
            } else {
                out += char(0xf0 | (c >> 18));
                out += char(0x80 | ((c >> 12) & 0x3f));
                out += char(0x80 | ((c >> 6) & 0x3f));
                out += char(0x80 | (c & 0x3f));
            }
        }

        std::uint32_t hex4() {
            if (at_ + 4 > text_.size()) {
                fail("short \\u escape");
            }
            std::uint32_t c = 0;
            for (int i = 0; i < 4; ++i) {
                const char h = text_[at_++];
                c <<= 4;
                if (h >= '0' && h <= '9') {
                    c |= std::uint32_t(h - '0');
                } else if (h >= 'a' && h <= 'f') {
                    c |= std::uint32_t(h - 'a' + 10);
                } else if (h >= 'A' && h <= 'F') {
                    c |= std::uint32_t(h - 'A' + 10);
                } else {
                    fail("bad \\u escape");
                }
            }
            return c;
        }

        std::string parse_string() {
            ++at_;
            std::string out;
            while (true) {
                if (at_ >= text_.size()) {
                    fail("unterminated string");
                }
                const char c = text_[at_++];
                if (c == '"') {
                    return out;
                }
                if (c != '\\') {
                    out += c;
                    continue;
                }
                if (at_ >= text_.size()) {
                    fail("unterminated escape");
                }
                switch (const char e = text_[at_++]) {
                    case '"':
                    case '\\':
                    case '/':
                        out += e;
                        break;
                    case 'b':
                        out += '\b';
                        break;
                    case 'f':
                        out += '\f';
                        break;
                    case 'n':
                        out += '\n';
                        break;
                    case 'r':
                        out += '\r';
                        break;
                    case 't':
                        out += '\t';
                        break;
                    case 'u': {
                        std::uint32_t c1 = hex4();
                        if (c1 >= 0xd800 && c1 < 0xdc00 && at_ + 6 <= text_.size() && text_[at_] == '\\' &&
                            text_[at_ + 1] == 'u') {
                            at_ += 2;
                            const std::uint32_t c2 = hex4();
                            c1 = 0x10000 + ((c1 - 0xd800) << 10) + (c2 - 0xdc00);
                        }
                        put_utf8(out, c1);
                        break;
                    }
                    default:
                        fail("bad escape");
                }
            }
        }

        Json parse_value() {
            skip();
            if (at_ >= text_.size()) {
                fail("value expected");
            }
            const std::size_t start = at_;
            Json v;
            const char c = text_[at_];
            if (c == '{') {
                v.kind = Json::Kind::Object;
                ++at_;
                skip();
                if (at_ < text_.size() && text_[at_] == '}') {
                    ++at_;
                } else {
                    while (true) {
                        skip();
                        if (at_ >= text_.size() || text_[at_] != '"') {
                            fail("member name expected");
                        }
                        std::string key = parse_string();
                        skip();
                        if (at_ >= text_.size() || text_[at_] != ':') {
                            fail("':' expected");
                        }
                        ++at_;
                        v.members.emplace_back(std::move(key), parse_value());
                        skip();
                        if (at_ < text_.size() && text_[at_] == ',') {
                            ++at_;
                            continue;
                        }
                        if (at_ < text_.size() && text_[at_] == '}') {
                            ++at_;
                            break;
                        }
                        fail("',' or '}' expected");
                    }
                }
            } else if (c == '[') {
                v.kind = Json::Kind::Array;
                ++at_;
                skip();
                if (at_ < text_.size() && text_[at_] == ']') {
                    ++at_;
                } else {
                    while (true) {
                        v.items.push_back(parse_value());
                        skip();
                        if (at_ < text_.size() && text_[at_] == ',') {
                            ++at_;
                            continue;
                        }
                        if (at_ < text_.size() && text_[at_] == ']') {
                            ++at_;
                            break;
                        }
                        fail("',' or ']' expected");
                    }
                }
            } else if (c == '"') {
                v.kind = Json::Kind::String;
                v.string = parse_string();
            } else if (text_.substr(at_, 4) == "true") {
                v.kind = Json::Kind::Bool;
                v.boolean = true;
                at_ += 4;
            } else if (text_.substr(at_, 5) == "false") {
                v.kind = Json::Kind::Bool;
                at_ += 5;
            } else if (text_.substr(at_, 4) == "null") {
                at_ += 4;
            } else {
                v.kind = Json::Kind::Number;
                const std::size_t begin = at_;
                while (at_ < text_.size() &&
                       std::string_view("+-0123456789.eE").find(text_[at_]) != std::string_view::npos) {
                    ++at_;
                }
                if (begin == at_) {
                    fail("value expected");
                }
                v.number = std::stod(std::string(text_.substr(begin, at_ - begin)));
            }
            v.text = std::string(text_.substr(start, at_ - start));
            return v;
        }

        std::string_view text_;
        std::size_t at_{};
    };

    Json parse_json(const std::string& text) {
        return JsonParser(text).parse();
    }

    // ---- MessagePack, what weft hands the engine, as JSON text for printing. ----

    class MessagePack {
    public:
        MessagePack(const void* data, std::size_t size) : data_(static_cast<const std::uint8_t*>(data)), size_(size) {
        }

        std::string json() {
            if (!size_) {
                return "undefined";
            }
            std::string out;
            value(out, 0);
            return out;
        }

    private:
        std::uint64_t be(unsigned bytes) {
            if (at_ + bytes > size_) {
                throw Failure("MessagePack: short");
            }
            std::uint64_t v = 0;
            for (unsigned i = 0; i < bytes; ++i) {
                v = (v << 8) | data_[at_++];
            }
            return v;
        }

        static void quote(std::string& out, std::string_view s) {
            out += '"';
            for (const char ch : s) {
                const auto c = static_cast<unsigned char>(ch);
                if (c == '"' || c == '\\') {
                    out += '\\';
                    out += ch;
                } else if (c < 0x20) {
                    char buffer[8];
                    std::snprintf(buffer, sizeof buffer, "\\u%04x", c);
                    out += buffer;
                } else {
                    out += ch;
                }
            }
            out += '"';
        }

        static void number(std::string& out, double d) {
            if (!std::isfinite(d)) {
                out += "null";
                return;
            }
            char buffer[32];
            std::snprintf(buffer, sizeof buffer, "%.17g", d);
            out += buffer;
        }

        void string(std::string& out, std::size_t n) {
            if (at_ + n > size_) {
                throw Failure("MessagePack: short string");
            }
            quote(out, std::string_view(reinterpret_cast<const char*>(data_ + at_), n));
            at_ += n;
        }

        void array(std::string& out, std::size_t n, unsigned depth) {
            out += '[';
            for (std::size_t i = 0; i < n; ++i) {
                if (i) {
                    out += ',';
                }
                value(out, depth + 1);
            }
            out += ']';
        }

        void map(std::string& out, std::size_t n, unsigned depth) {
            out += '{';
            for (std::size_t i = 0; i < n; ++i) {
                if (i) {
                    out += ',';
                }
                std::string key;
                value(key, depth + 1);
                if (key.empty() || key.front() != '"') {
                    key = "\"" + key + "\"";
                }
                out += key;
                out += ':';
                value(out, depth + 1);
            }
            out += '}';
        }

        void value(std::string& out, unsigned depth) {
            if (depth > 64) {
                throw Failure("MessagePack: too deep");
            }
            const std::uint8_t b = std::uint8_t(be(1));
            if (b <= 0x7f) {
                out += std::to_string(b);
            } else if (b >= 0xe0) {
                out += std::to_string(int(std::int8_t(b)));
            } else if ((b & 0xf0) == 0x80) {
                map(out, b & 0x0f, depth);
            } else if ((b & 0xf0) == 0x90) {
                array(out, b & 0x0f, depth);
            } else if ((b & 0xe0) == 0xa0) {
                string(out, b & 0x1f);
            } else {
                switch (b) {
                    case 0xc0:
                        out += "null";
                        break;
                    case 0xc2:
                        out += "false";
                        break;
                    case 0xc3:
                        out += "true";
                        break;
                    case 0xc4:
                    case 0xc5:
                    case 0xc6: {
                        const std::size_t n = std::size_t(be(b == 0xc4 ? 1 : b == 0xc5 ? 2 : 4));
                        at_ += n;
                        out += "\"<" + std::to_string(n) + " bytes>\"";
                        break;
                    }
                    case 0xca: {
                        const auto bits = std::uint32_t(be(4));
                        float f;
                        std::memcpy(&f, &bits, 4);
                        number(out, f);
                        break;
                    }
                    case 0xcb: {
                        const auto bits = be(8);
                        double d;
                        std::memcpy(&d, &bits, 8);
                        number(out, d);
                        break;
                    }
                    case 0xcc:
                    case 0xcd:
                    case 0xce:
                    case 0xcf:
                        out += std::to_string(be(1u << (b - 0xcc)));
                        break;
                    case 0xd0:
                        out += std::to_string(std::int8_t(be(1)));
                        break;
                    case 0xd1:
                        out += std::to_string(std::int16_t(be(2)));
                        break;
                    case 0xd2:
                        out += std::to_string(std::int32_t(be(4)));
                        break;
                    case 0xd3:
                        out += std::to_string(std::int64_t(be(8)));
                        break;
                    case 0xd9:
                    case 0xda:
                    case 0xdb:
                        string(out, std::size_t(be(b == 0xd9 ? 1 : b == 0xda ? 2 : 4)));
                        break;
                    case 0xdc:
                    case 0xdd:
                        array(out, std::size_t(be(b == 0xdc ? 2 : 4)), depth);
                        break;
                    case 0xde:
                    case 0xdf:
                        map(out, std::size_t(be(b == 0xde ? 2 : 4)), depth);
                        break;
                    default:
                        out += "null";
                        break;
                }
            }
        }

        const std::uint8_t* data_;
        std::size_t size_;
        std::size_t at_{};
    };

    // ---- PNG: RGBA rows, stored (uncompressed) deflate blocks, so no codec is needed. ----

    std::uint32_t crc32(const std::uint8_t* data, std::size_t size, std::uint32_t crc = 0) {
        static const auto table = [] {
            std::array<std::uint32_t, 256> t{};
            for (std::uint32_t n = 0; n < 256; ++n) {
                std::uint32_t c = n;
                for (int k = 0; k < 8; ++k) {
                    c = c & 1 ? 0xedb88320u ^ (c >> 1) : c >> 1;
                }
                t[n] = c;
            }
            return t;
        }();
        crc = ~crc;
        for (std::size_t i = 0; i < size; ++i) {
            crc = table[(crc ^ data[i]) & 0xff] ^ (crc >> 8);
        }
        return ~crc;
    }

    void write_png(const std::filesystem::path& path, unsigned width, unsigned height,
                   const std::vector<std::uint8_t>& bgra) {
        if (path.has_parent_path()) {
            std::filesystem::create_directories(path.parent_path());
        }
        std::vector<std::uint8_t> raw;
        raw.reserve(std::size_t(height) * (std::size_t(width) * 4 + 1));
        for (unsigned y = 0; y < height; ++y) {
            raw.push_back(0);
            const std::uint8_t* row = bgra.data() + std::size_t(y) * width * 4;
            for (unsigned x = 0; x < width; ++x) {
                raw.push_back(row[x * 4 + 2]);
                raw.push_back(row[x * 4 + 1]);
                raw.push_back(row[x * 4 + 0]);
                raw.push_back(row[x * 4 + 3]);
            }
        }
        std::vector<std::uint8_t> z{0x78, 0x01};
        std::uint32_t a = 1, b = 0;
        for (std::size_t at = 0; at < raw.size() || at == 0;) {
            const std::size_t n = std::min<std::size_t>(65535, raw.size() - at);
            const bool last = at + n == raw.size();
            z.push_back(last ? 1 : 0);
            z.push_back(std::uint8_t(n & 0xff));
            z.push_back(std::uint8_t(n >> 8));
            z.push_back(std::uint8_t(~n & 0xff));
            z.push_back(std::uint8_t((~n >> 8) & 0xff));
            z.insert(z.end(), raw.begin() + std::ptrdiff_t(at), raw.begin() + std::ptrdiff_t(at + n));
            for (std::size_t i = at; i < at + n; ++i) {
                a = (a + raw[i]) % 65521;
                b = (b + a) % 65521;
            }
            at += n;
            if (last) {
                break;
            }
        }
        const std::uint32_t adler = (b << 16) | a;
        for (int s = 24; s >= 0; s -= 8) {
            z.push_back(std::uint8_t(adler >> s));
        }
        std::ofstream out(path, std::ios::binary);
        if (!out) {
            throw Failure("cannot write " + path.string());
        }
        const auto put32 = [&](std::uint32_t v) {
            const std::uint8_t bytes[4]{std::uint8_t(v >> 24), std::uint8_t(v >> 16), std::uint8_t(v >> 8),
                                        std::uint8_t(v)};
            out.write(reinterpret_cast<const char*>(bytes), 4);
        };
        const auto chunk = [&](const char* type, const std::vector<std::uint8_t>& data) {
            put32(std::uint32_t(data.size()));
            std::vector<std::uint8_t> typed(type, type + 4);
            typed.insert(typed.end(), data.begin(), data.end());
            out.write(reinterpret_cast<const char*>(typed.data()), std::streamsize(typed.size()));
            put32(crc32(typed.data(), typed.size()));
        };
        static const std::uint8_t signature[8]{0x89, 'P', 'N', 'G', '\r', '\n', 0x1a, '\n'};
        out.write(reinterpret_cast<const char*>(signature), 8);
        std::vector<std::uint8_t> header(13);
        for (int i = 0; i < 4; ++i) {
            header[std::size_t(i)] = std::uint8_t(width >> (24 - 8 * i));
            header[std::size_t(4 + i)] = std::uint8_t(height >> (24 - 8 * i));
        }
        header[8] = 8; // bit depth
        header[9] = 6; // RGBA
        chunk("IHDR", header);
        chunk("IDAT", z);
        chunk("IEND", {});
    }

    // ---- The page folder, as the Unreal plugin reads it (FWeftPage::FromDirectory). ----

    struct Page {
        std::vector<std::pair<std::string, std::string>> scripts;
        std::string stylesheet;
        std::string keyframes;
        std::string root;
        std::string base;
        bool crpg_bridge{};
        bool forgiving_css{};
        bool web_platform{};
    };

    Page read_page(const std::filesystem::path& dir) {
        Page page;
        const auto manifest = dir / "page.json";
        std::string stylesheet_name = "page.css";
        if (std::filesystem::exists(manifest)) {
            const Json json = parse_json(read(manifest));
            if (const Json* scripts = json.find("scripts")) {
                for (const Json& s : scripts->items) {
                    page.scripts.emplace_back(s.string, read(dir / utf8_path(s.string)));
                }
            }
            if (const Json* s = json.find("stylesheet"); s && s->kind == Json::Kind::String) {
                stylesheet_name = s->string;
                page.stylesheet = read(dir / utf8_path(s->string));
            }
            if (const Json* k = json.find("keyframes"); k && k->kind == Json::Kind::String) {
                page.keyframes = read(dir / utf8_path(k->string));
            }
            page.root = json.get_string("root");
            page.crpg_bridge = json.get_bool("crpgBridge");
            page.forgiving_css = json.get_bool("forgivingCss");
            page.web_platform = json.get_bool("webPlatform");
        } else {
            page.scripts.emplace_back("page.js", read(dir / "page.js"));
            if (std::filesystem::exists(dir / "page.css")) {
                page.stylesheet = read(dir / "page.css");
            }
            if (std::filesystem::exists(dir / "keyframes.css")) {
                page.keyframes = read(dir / "keyframes.css");
            }
        }
        if (page.scripts.empty()) {
            throw Failure(dir.string() + " names no script");
        }
        page.base = utf8(dir / utf8_path(stylesheet_name));
        return page;
    }

    const char* render_result(weft_render_result r) {
        static const char* names[]{"OK",
                                   "INVALID_ARGUMENT",
                                   "UNSUPPORTED_ABI",
                                   "UNSUPPORTED_FEATURE",
                                   "WRONG_THREAD",
                                   "STALE_HANDLE",
                                   "WRONG_BACKEND",
                                   "WRONG_EPOCH",
                                   "BUSY",
                                   "INVALID_STATE",
                                   "INVALID_SERIAL",
                                   "BACKEND_ERROR",
                                   "OUT_OF_MEMORY",
                                   "SHUTTING_DOWN",
                                   "DEVICE_UNAVAILABLE",
                                   "HOST_CONFIRMATION_REQUIRED"};
        return unsigned(r) < std::size(names) ? names[r] : "?";
    }

    const char* diagnostic_kind(std::uint32_t kind) {
        switch (kind) {
            case WEFT_DIAGNOSTIC_SCRIPT:
                return "script";
            case WEFT_DIAGNOSTIC_HANDLER:
                return "handler";
            case WEFT_DIAGNOSTIC_FRAME:
                return "frame";
            case WEFT_DIAGNOSTIC_SCENE:
                return "scene";
            case WEFT_DIAGNOSTIC_CONSOLE:
                return "console";
            case WEFT_DIAGNOSTIC_CONSOLE_WARNING:
                return "console.warn";
            case WEFT_DIAGNOSTIC_CONSOLE_ERROR:
                return "console.error";
            default:
                return "other";
        }
    }

    // ---- D3D11: draws the View's latest frame over a solid "game" picture, on demand. ----

    class Drawer {
    public:
        Drawer(bool warp, std::array<float, 4> background) : background_(background) {
            const D3D_FEATURE_LEVEL levels[]{D3D_FEATURE_LEVEL_11_0};
            if (FAILED(D3D11CreateDevice(nullptr, warp ? D3D_DRIVER_TYPE_WARP : D3D_DRIVER_TYPE_HARDWARE, nullptr,
                                         D3D11_CREATE_DEVICE_BGRA_SUPPORT, levels, 1, D3D11_SDK_VERSION, &device_,
                                         nullptr, &context_))) {
                return;
            }
            const weft_render_d3d11_desc desc{sizeof(desc), WEFT_RENDER_ABI_VERSION, device_, queue, 0};
            weft_render_backend table{};
            if (weft_render_d3d11_create(&desc, &backend_, &table) != WEFT_RENDER_OK ||
                weft_renderer_create(&table, nullptr, &renderer_) != WEFT_RENDER_OK) {
                throw Failure("no D3D11 renderer");
            }
            info_.struct_size = sizeof(info_);
            weft_renderer_get_info(renderer_, &info_);
            D3D11_QUERY_DESC query{D3D11_QUERY_EVENT, 0};
            device_->CreateQuery(&query, &done_);
        }

        ~Drawer() {
            release_target();
            if (renderer_) {
                weft_renderer_destroy(renderer_);
            }
            if (backend_) {
                weft_render_d3d11_destroy(backend_);
            }
            if (done_) {
                done_->Release();
            }
            if (context_) {
                context_->Release();
            }
            if (device_) {
                device_->Release();
            }
        }

        bool available() const {
            return device_ != nullptr;
        }

        // Draws the frame; the pixels (BGRA rows) when asked. False, printed, when it did not draw.
        bool draw(weft_runtime* runtime, weft_view_handle view, unsigned frame_index, std::vector<std::uint8_t>* pixels,
                  unsigned* out_width, unsigned* out_height) {
            weft_view_frame* frame{};
            if (const weft_result r = weft_view_acquire_frame(runtime, view, &frame); r != WEFT_OK) {
                std::printf("[render] frame %u: no frame yet (%s)\n", frame_index, weft_result_string(r));
                return false;
            }
            weft_view_frame_info frame_info{};
            frame_info.struct_size = sizeof(frame_info);
            weft_view_frame_get_info(frame, &frame_info);
            const unsigned width = unsigned(std::ceil(frame_info.logical_width * frame_info.device_scale));
            const unsigned height = unsigned(std::ceil(frame_info.logical_height * frame_info.device_scale));
            ensure_target(width, height);
            context_->ClearRenderTargetView(rtv_, background_.data());
            const weft_render_scene_frame scene{sizeof(scene), WEFT_RENDER_ABI_VERSION, frame, 0, 0, nullptr};
            weft_render_handle prepared{};
            const auto start = std::chrono::steady_clock::now();
            weft_render_result r = weft_renderer_prepare_scene(renderer_, &scene, &prepared);
            weft_view_frame_release(frame);
            if (r != WEFT_RENDER_OK) {
                std::printf("[render] frame %u: prepare failed (%s)\n", frame_index, render_result(r));
                return false;
            }
            weft_render_d3d11_pass payload{sizeof(payload), WEFT_RENDER_ABI_VERSION, nullptr, rtv_, nullptr, 0, 0, {}};
            const weft_render_pass pass{sizeof(pass),
                                        WEFT_RENDER_ABI_VERSION,
                                        info_.identity,
                                        width,
                                        height,
                                        WEFT_RENDER_TARGET_LINEAR_BGRA8,
                                        1,
                                        &payload,
                                        sizeof(payload)};
            weft_render_handle use{};
            r = weft_renderer_encode(renderer_, prepared, &pass, &use);
            weft_renderer_release_prepared(renderer_, prepared);
            if (r != WEFT_RENDER_OK) {
                std::printf("[render] frame %u: encode failed (%s)\n", frame_index, render_result(r));
                weft_renderer_last_failure(
                    renderer_,
                    [](void*, const char* text, size_t size) {
                        std::printf("[render] because: %.*s\n", int(size), text);
                    },
                    nullptr);
                return false;
            }
            render_ms_.push_back(
                std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count());
            context_->End(done_);
            context_->Flush();
            weft_renderer_notify_submitted(renderer_, use, queue, ++serial_);
            while (context_->GetData(done_, nullptr, 0, 0) == S_FALSE) {
                Sleep(0);
            }
            weft_renderer_notify_completed(renderer_, info_.identity.device_epoch, queue, serial_);
            if (pixels) {
                read_back(*pixels);
                *out_width = width;
                *out_height = height;
            }
            return true;
        }

        const std::vector<double>& render_ms() const {
            return render_ms_;
        }

    private:
        static constexpr std::uint64_t queue = 1;

        void release_target() {
            for (IUnknown* object :
                 {static_cast<IUnknown*>(staging_), static_cast<IUnknown*>(rtv_), static_cast<IUnknown*>(target_)}) {
                if (object) {
                    object->Release();
                }
            }
            staging_ = nullptr;
            rtv_ = nullptr;
            target_ = nullptr;
        }

        void ensure_target(unsigned width, unsigned height) {
            if (target_ && width == width_ && height == height_) {
                return;
            }
            release_target();
            D3D11_TEXTURE2D_DESC desc{};
            desc.Width = width;
            desc.Height = height;
            desc.MipLevels = desc.ArraySize = 1;
            desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
            desc.SampleDesc.Count = 1;
            desc.BindFlags = D3D11_BIND_RENDER_TARGET;
            if (FAILED(device_->CreateTexture2D(&desc, nullptr, &target_)) ||
                FAILED(device_->CreateRenderTargetView(target_, nullptr, &rtv_))) {
                throw Failure("no render target");
            }
            desc.BindFlags = 0;
            desc.Usage = D3D11_USAGE_STAGING;
            desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
            if (FAILED(device_->CreateTexture2D(&desc, nullptr, &staging_))) {
                throw Failure("no staging texture");
            }
            width_ = width;
            height_ = height;
        }

        void read_back(std::vector<std::uint8_t>& pixels) {
            context_->CopyResource(staging_, target_);
            D3D11_MAPPED_SUBRESOURCE mapped{};
            if (FAILED(context_->Map(staging_, 0, D3D11_MAP_READ, 0, &mapped))) {
                throw Failure("cannot map the frame");
            }
            pixels.resize(std::size_t(width_) * height_ * 4);
            for (unsigned y = 0; y < height_; ++y) {
                std::memcpy(pixels.data() + std::size_t(y) * width_ * 4,
                            static_cast<const std::uint8_t*>(mapped.pData) + std::size_t(y) * mapped.RowPitch,
                            std::size_t(width_) * 4);
            }
            context_->Unmap(staging_, 0);
        }

        std::array<float, 4> background_;
        ID3D11Device* device_{};
        ID3D11DeviceContext* context_{};
        ID3D11Query* done_{};
        weft_render_d3d11* backend_{};
        weft_renderer* renderer_{};
        weft_render_info info_{};
        ID3D11Texture2D* target_{};
        ID3D11RenderTargetView* rtv_{};
        ID3D11Texture2D* staging_{};
        unsigned width_{};
        unsigned height_{};
        std::uint64_t serial_{};
        std::vector<double> render_ms_;
    };

    std::string times(std::vector<double> ms) {
        if (ms.empty()) {
            return "none";
        }
        std::sort(ms.begin(), ms.end());
        const auto at = [&](double q) {
            return ms[std::min(ms.size() - 1, std::size_t(q * double(ms.size() - 1) + 0.5))];
        };
        char text[128];
        std::snprintf(text, sizeof text, "p50 %.3f / p99 %.3f / max %.3f ms over %zu", at(0.5), at(0.99), ms.back(),
                      ms.size());
        return text;
    }

    struct Options {
        std::filesystem::path page;
        std::filesystem::path timeline;
        std::filesystem::path fonts;
        std::filesystem::path effects;
        std::filesystem::path out = "page-runner-out";
        float width = 1920;
        float height = 1080;
        float scale = 1;
        double fps = 60;
        long frames = -1;
        bool warp{};
        unsigned render_every{};
        std::array<float, 4> background{0.10f, 0.12f, 0.16f, 1};
        std::filesystem::path storage;
        double slow_ms = 100;
    };

    // The engine of the run: the runtime, the View and what the page sent.
    struct Run {
        weft_runtime* runtime{};
        weft_view_handle view{};
        const Json* replies{};
        unsigned diagnostics{};
        unsigned messages{};

        static void on_message(void* user, weft_view_handle view, const weft_message* m) {
            auto& run = *static_cast<Run*>(user);
            ++run.messages;
            const std::string name(m->name, m->name_size);
            std::string payload;
            try {
                payload = MessagePack(m->payload, m->payload_size).json();
            } catch (const std::exception& e) {
                payload = std::string("<") + e.what() + ">";
            }
            if (m->kind == WEFT_MESSAGE_CALL) {
                std::printf("[call#%llu] %s %s\n", static_cast<unsigned long long>(m->call_id), name.c_str(),
                            payload.c_str());
                const Json* reply = run.replies ? run.replies->find(name) : nullptr;
                if (reply) {
                    const bool ok = reply->get_bool("ok", true);
                    const Json* value = reply->find("value");
                    const std::string text = value ? value->text : "null";
                    const weft_result r = weft_view_complete_call(run.runtime, view, m->call_id, ok ? 1u : 0u,
                                                                  WEFT_VALUE_JSON, text.data(), text.size());
                    std::printf("[reply#%llu] %s %s -> %s\n", static_cast<unsigned long long>(m->call_id),
                                ok ? "resolve" : "reject", text.c_str(), weft_result_string(r));
                }
            } else {
                std::printf("[send] %s %s\n", name.c_str(), payload.c_str());
            }
        }

        void drain() {
            weft_view_drain_messages(runtime, view, &Run::on_message, this);
            std::uint32_t dropped{};
            weft_view_drain_diagnostics(
                runtime, view,
                [](void* user, weft_view_handle, const weft_diagnostic* d) {
                    ++static_cast<Run*>(user)->diagnostics;
                    std::printf("[diag:%s] %.*s\n", diagnostic_kind(d->kind), int(d->message_size), d->message);
                },
                this, &dropped);
            if (dropped) {
                std::printf("[diag] %u dropped\n", dropped);
            }
        }
    };

    std::array<float, 4> parse_color(const std::string& text) {
        if (text.size() != 7 || text[0] != '#') {
            throw Failure("--background wants #rrggbb");
        }
        const auto channel = [&](std::size_t at) {
            return float(std::stoul(text.substr(at, 2), nullptr, 16)) / 255;
        };
        return {channel(1), channel(3), channel(5), 1};
    }

    Options parse(int argc, char** argv) {
        Options o;
        for (int i = 1; i < argc; ++i) {
            const std::string a = argv[i];
            const auto next = [&]() -> std::string {
                if (i + 1 >= argc) {
                    throw Failure(a + " wants a value");
                }
                return argv[++i];
            };
            if (a == "--page") {
                o.page = utf8_path(next());
            } else if (a == "--timeline") {
                o.timeline = utf8_path(next());
            } else if (a == "--fonts") {
                o.fonts = utf8_path(next());
            } else if (a == "--effects") {
                o.effects = utf8_path(next());
            } else if (a == "--out") {
                o.out = utf8_path(next());
            } else if (a == "--size") {
                const std::string s = next();
                const auto x = s.find('x');
                if (x == std::string::npos) {
                    throw Failure("--size wants WxH");
                }
                o.width = std::stof(s.substr(0, x));
                o.height = std::stof(s.substr(x + 1));
            } else if (a == "--scale") {
                o.scale = std::stof(next());
            } else if (a == "--fps") {
                o.fps = std::stod(next());
            } else if (a == "--frames") {
                o.frames = std::stol(next());
            } else if (a == "--warp") {
                o.warp = true;
            } else if (a == "--render-every") {
                o.render_every = unsigned(std::stoul(next()));
            } else if (a == "--background") {
                o.background = parse_color(next());
            } else if (a == "--storage") {
                o.storage = utf8_path(next());
            } else if (a == "--slow") {
                o.slow_ms = std::stod(next());
            } else {
                throw Failure("unknown argument " + a);
            }
        }
        if (o.page.empty()) {
            throw Failure("--page <dir> is required");
        }
        return o;
    }

    void add_fonts(weft_runtime* runtime, const std::filesystem::path& file) {
        const Json json = parse_json(read(file));
        const auto folder = file.parent_path();
        if (const Json* faces = json.find("faces")) {
            for (const Json& f : faces->items) {
                const std::string family = f.get_string("family");
                const std::string path = utf8(folder / utf8_path(f.get_string("path")));
                weft_font_face face{sizeof(face)};
                face.weight = std::uint32_t(f.get_number("weight", 400));
                face.italic = f.get_bool("italic") ? 1 : 0;
                face.face_index = std::uint32_t(f.get_number("face", 0));
                face.family = family.data();
                face.family_size = family.size();
                face.path = path.data();
                face.path_size = path.size();
                const std::string range = f.get_string("unicodeRange");
                if (!range.empty()) {
                    face.unicode_range = range.data();
                    face.unicode_range_size = range.size();
                }
                const weft_result r = weft_runtime_add_font(runtime, &face);
                std::printf("[font] %s %u%s %s: %s\n", family.c_str(), face.weight, face.italic ? " italic" : "",
                            path.c_str(), weft_result_string(r));
            }
        }
        if (const Json* fallback = json.find("fallback")) {
            for (const Json& f : fallback->items) {
                const weft_result r = weft_runtime_add_fallback_family(runtime, f.string.data(), f.string.size());
                std::printf("[font] fallback %s: %s\n", f.string.c_str(), weft_result_string(r));
            }
        }
    }

    // A JSON string literal of s.
    std::string json_quote(std::string_view s) {
        std::string out = "\"";
        for (const char ch : s) {
            const auto c = static_cast<unsigned char>(ch);
            if (c == '"' || c == '\\') {
                out += '\\';
                out += ch;
            } else if (c < 0x20) {
                char buffer[8];
                std::snprintf(buffer, sizeof buffer, "\\u%04x", c);
                out += buffer;
            } else {
                out += ch;
            }
        }
        return out + "\"";
    }

    // The View's localStorage items in key order.
    std::vector<std::pair<std::string, std::string>> storage_items(const Run& run, std::uint64_t* revision) {
        std::vector<std::pair<std::string, std::string>> items;
        weft_view_get_storage(
            run.runtime, run.view, 0,
            [](void* user, const char* key, size_t key_size, const char* value, size_t value_size) {
                static_cast<std::vector<std::pair<std::string, std::string>>*>(user)->emplace_back(
                    std::string(key, key_size), std::string(value, value_size));
            },
            &items, revision);
        return items;
    }

    void add_effects(weft_runtime* runtime, const std::filesystem::path& file) {
        const Json json = parse_json(read(file));
        const auto folder = file.parent_path();
        const Json* effects = json.find("effects");
        if (!effects) {
            throw Failure(utf8(file) + " lists no effects");
        }
        for (const Json& e : effects->items) {
            const std::string name = e.get_string("name");
            // Each value the descriptor points at, kept in place until the call.
            std::deque<std::string> texts;
            const auto keep = [&](std::string text) -> const std::string& {
                return texts.emplace_back(std::move(text));
            };
            std::vector<weft_effect_parameter> parameters;
            if (const Json* list = e.find("parameters")) {
                static const std::map<std::string, std::uint32_t> types{{"number", WEFT_EFFECT_NUMBER},
                                                                        {"vec2", WEFT_EFFECT_VEC2},
                                                                        {"vec3", WEFT_EFFECT_VEC3},
                                                                        {"vec4", WEFT_EFFECT_VEC4},
                                                                        {"color", WEFT_EFFECT_COLOR}};
                for (const Json& p : list->items) {
                    const auto type = types.find(p.get_string("type"));
                    if (type == types.end()) {
                        throw Failure(name + ": a parameter's type is number, vec2, vec3, vec4 or color");
                    }
                    const auto& n = keep(p.get_string("name"));
                    const auto& initial = keep(p.get_string("initial"));
                    parameters.push_back({n.data(), n.size(), initial.data(), initial.size(), type->second, 0});
                }
            }
            std::vector<weft_effect_texture> textures;
            if (const Json* list = e.find("textures")) {
                for (const Json& t : list->items) {
                    const auto& n = keep(t.get_string("name"));
                    const auto& bytes = keep(read(folder / utf8_path(t.get_string("path"))));
                    const auto format =
                        t.get_string("format") == "srgb" ? WEFT_EFFECT_TEXTURE_SRGB : WEFT_EFFECT_TEXTURE_DATA;
                    textures.push_back(
                        {n.data(), n.size(), bytes.data(), bytes.size(), format | WEFT_EFFECT_TEXTURE_PNG, 0, 0, 0});
                }
            }
            const auto prefix = folder / utf8_path(e.get_string("bytecode"));
            const auto code = [&](const char* suffix) -> const std::string& {
                auto path = prefix;
                path += suffix;
                return keep(std::filesystem::exists(path) ? read(path) : std::string());
            };
            const auto& dxbc_encoded = code(".encoded.dxbc");
            const auto& dxbc_linear = code(".linear.dxbc");
            const auto& spirv_encoded = code(".encoded.spv");
            const auto& spirv_linear = code(".linear.spv");
            weft_effect_desc desc{sizeof(desc)};
            desc.animated = e.get_bool("animated") ? 1 : 0;
            desc.backdrop = e.get_bool("backdrop") ? 1 : 0;
            desc.backdrop_margin = float(e.get_number("backdropMargin"));
            desc.name = name.data();
            desc.name_size = name.size();
            desc.parameters = parameters.data();
            desc.parameter_count = std::uint32_t(parameters.size());
            desc.textures = textures.data();
            desc.texture_count = std::uint32_t(textures.size());
            desc.dxbc_encoded = dxbc_encoded.data();
            desc.dxbc_encoded_size = dxbc_encoded.size();
            desc.dxbc_linear = dxbc_linear.data();
            desc.dxbc_linear_size = dxbc_linear.size();
            desc.spirv_encoded = spirv_encoded.data();
            desc.spirv_encoded_size = spirv_encoded.size();
            desc.spirv_linear = spirv_linear.data();
            desc.spirv_linear_size = spirv_linear.size();
            const weft_result r = weft_runtime_add_effect(runtime, &desc);
            std::printf("[effect] %s (%zu parameters, %zu textures%s): %s\n", name.c_str(), parameters.size(),
                        textures.size(), desc.backdrop ? ", backdrop" : "", weft_result_string(r));
        }
    }

    bool load(Run& run, const Page& page) {
        std::vector<weft_script> scripts;
        for (const auto& [name, source] : page.scripts) {
            scripts.push_back({name.data(), name.size(), source.data(), source.size()});
        }
        weft_page_desc desc{sizeof(desc)};
        desc.flags = (page.crpg_bridge ? WEFT_PAGE_CRPG_BRIDGE : 0u) |
                     (page.forgiving_css ? WEFT_PAGE_FORGIVING_CSS : 0u) |
                     (page.web_platform ? WEFT_PAGE_WEB_PLATFORM : 0u);
        desc.scripts = scripts.data();
        desc.script_count = std::uint32_t(scripts.size());
        desc.stylesheet = page.stylesheet.data();
        desc.stylesheet_size = page.stylesheet.size();
        desc.keyframes = page.keyframes.data();
        desc.keyframes_size = page.keyframes.size();
        desc.root = page.root.empty() ? nullptr : page.root.data();
        desc.root_size = page.root.size();
        desc.base = page.base.data();
        desc.base_size = page.base.size();
        const auto start = std::chrono::steady_clock::now();
        const weft_result r = weft_view_load(run.runtime, run.view, &desc);
        const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
        std::printf("[load] %s in %.1f ms\n", weft_result_string(r), ms);
        run.drain();
        return r == WEFT_OK;
    }

    weft_result read_resource(void*, const char* path, std::size_t size, weft_resource_writer* file) {
        std::ifstream in(utf8_path(std::string(path, size)), std::ios::binary);
        if (!in) {
            std::printf("[resource] missing %.*s\n", int(size), path);
            return WEFT_OK;
        }
        const std::string bytes{std::istreambuf_iterator<char>(in), {}};
        return weft_resource_writer_set(file, bytes.data(), bytes.size());
    }

    int run(const Options& o) {
        const Page page = read_page(o.page);
        Json timeline;
        if (!o.timeline.empty()) {
            timeline = parse_json(read(o.timeline));
        }
        const long frames = o.frames >= 0 ? o.frames : long(timeline.get_number("frames", 60));

        Run run;
        run.replies = timeline.find("replies");
        const weft_runtime_desc runtime_desc{sizeof(runtime_desc), WEFT_ABI_VERSION, 8, WEFT_RUNTIME_SYSTEM_FONTS};
        if (const weft_result r = weft_runtime_create(&runtime_desc, &run.runtime); r != WEFT_OK) {
            throw Failure(std::string("runtime: ") + weft_result_string(r));
        }
        if (!o.fonts.empty()) {
            add_fonts(run.runtime, o.fonts);
        }
        if (!o.effects.empty()) {
            add_effects(run.runtime, o.effects);
        }
        const weft_view_desc view_desc{sizeof(view_desc), 0, o.width, o.height, o.scale, 0};
        if (const weft_result r = weft_view_create(run.runtime, &view_desc, &run.view); r != WEFT_OK) {
            throw Failure(std::string("view: ") + weft_result_string(r));
        }
        weft_view_set_resource_reader(run.runtime, run.view, &read_resource, nullptr);
        std::uint64_t restored{};
        if (!o.storage.empty() && std::filesystem::exists(o.storage)) {
            const Json items = parse_json(read(o.storage));
            for (const auto& [key, value] : items.members) {
                const weft_result r = weft_view_set_storage_item(run.runtime, run.view, key.data(), key.size(),
                                                                 value.string.data(), value.string.size());
                if (r != WEFT_OK) {
                    std::printf("[storage] %s: %s\n", key.c_str(), weft_result_string(r));
                }
            }
            storage_items(run, &restored);
            std::printf("[storage] %zu items from %s\n", items.members.size(), utf8(o.storage).c_str());
        }
        bool loaded = load(run, page);

        std::unique_ptr<Drawer> drawer;
        const auto ensure_drawer = [&]() -> Drawer* {
            if (!drawer) {
                drawer = std::make_unique<Drawer>(o.warp, o.background);
            }
            return drawer->available() ? drawer.get() : nullptr;
        };

        std::vector<double> advance_ms;
        // An advance that took longer is printed as it happens.
        const double slow_ms = o.slow_ms;
        const Json* steps = timeline.find("steps");
        int status = loaded ? 0 : 1;
        for (long n = 0; n < frames; ++n) {
            if (steps) {
                for (const Json& step : steps->items) {
                    if (long(step.get_number("frame", -1)) != n) {
                        continue;
                    }
                    if (const Json* emit = step.find("emit")) {
                        const Json* value = step.find("value");
                        const std::string text = value ? value->text : "{}";
                        const weft_result r = weft_view_emit(
                            run.runtime, run.view, emit->string.data(), emit->string.size(), WEFT_VALUE_JSON,
                            text.data(), text.size(), step.get_bool("latest") ? WEFT_EMIT_LATEST : 0);
                        if (r != WEFT_OK) {
                            std::printf("[emit] %s: %s\n", emit->string.c_str(), weft_result_string(r));
                        }
                    }
                    if (const Json* model = step.find("model")) {
                        const Json* fields = step.find("fields");
                        const std::string text = fields ? fields->text : "{}";
                        const weft_result r =
                            weft_view_set_model(run.runtime, run.view, model->string.data(), model->string.size(),
                                                WEFT_VALUE_JSON, text.data(), text.size());
                        if (r != WEFT_OK) {
                            std::printf("[model] %s: %s\n", model->string.c_str(), weft_result_string(r));
                        }
                    }
                    if (const Json* anchors = step.find("anchors")) {
                        std::vector<weft_anchor> table;
                        for (const Json& a : anchors->items) {
                            table.push_back({std::uint64_t(a.get_number("key")), float(a.get_number("x")),
                                             float(a.get_number("y")), float(a.get_number("scale", 1)),
                                             float(a.get_number("depth")), std::uint32_t(a.get_number("flags")), 0});
                        }
                        weft_view_write_anchors(run.runtime, run.view, table.data(), table.size());
                    }
                    if (const Json* resize = step.find("resize"); resize && resize->items.size() == 3) {
                        weft_view_resize(run.runtime, run.view, float(resize->items[0].number),
                                         float(resize->items[1].number), float(resize->items[2].number));
                    }
                    if (const Json* pointer = step.find("pointer")) {
                        static const std::map<std::string, weft_pointer_kind> kinds{{"move", WEFT_POINTER_MOVE},
                                                                                    {"down", WEFT_POINTER_DOWN},
                                                                                    {"up", WEFT_POINTER_UP},
                                                                                    {"leave", WEFT_POINTER_LEAVE}};
                        const auto kind = kinds.find(pointer->string);
                        if (kind == kinds.end()) {
                            throw Failure("pointer wants move, down, up or leave");
                        }
                        const weft_pointer_event e{sizeof(e),
                                                   std::uint32_t(kind->second),
                                                   float(step.get_number("x")),
                                                   float(step.get_number("y")),
                                                   std::uint32_t(step.get_number("buttons")),
                                                   0,
                                                   0,
                                                   0};
                        uint32_t prevented{};
                        weft_view_pointer(run.runtime, run.view, &e, &prevented);
                    }
                    if (const Json* wheel = step.find("wheel"); wheel && wheel->items.size() == 2) {
                        const weft_pointer_event e{sizeof(e),
                                                   WEFT_POINTER_WHEEL,
                                                   float(step.get_number("x")),
                                                   float(step.get_number("y")),
                                                   0,
                                                   0,
                                                   float(wheel->items[0].number),
                                                   float(wheel->items[1].number)};
                        uint32_t prevented{};
                        weft_view_pointer(run.runtime, run.view, &e, &prevented);
                    }
                    if (const Json* key = step.find("key")) {
                        std::uint32_t modifiers{};
                        for (const char m : step.get_string("modifiers")) {
                            modifiers |= m == 'S'   ? WEFT_MODIFIER_SHIFT
                                         : m == 'C' ? WEFT_MODIFIER_CONTROL
                                         : m == 'A' ? WEFT_MODIFIER_ALT
                                         : m == 'M' ? WEFT_MODIFIER_META
                                         : m == 'G' ? WEFT_MODIFIER_ALT_GRAPH
                                                    : 0;
                        }
                        std::string value = step.get_string("value");
                        if (value.empty()) {
                            char layout[32]{};
                            if (weft_keyboard_key(key->string.data(), key->string.size(), modifiers, layout) != WEFT_OK) {
                                throw Failure("key " + key->string + " is no code weft knows; give its value");
                            }
                            value = layout;
                        }
                        const std::string action = step.get_string("action", "press");
                        // The text of a key whose value is one character (not a name such as Enter or Dead).
                        const auto lead = value.empty() ? 0 : static_cast<unsigned char>(value[0]);
                        const std::size_t length = lead < 0x80 ? 1 : lead < 0xE0 ? 2 : lead < 0xF0 ? 3 : 4;
                        const auto typed = step.find("text") ? step.get_string("text")
                                           : value.size() == length ? value
                                                                    : std::string();
                        const auto send = [&](std::uint32_t kind) {
                            const weft_key_event e{sizeof(e),
                                                   kind,
                                                   value.data(),
                                                   value.size(),
                                                   key->string.data(),
                                                   key->string.size(),
                                                   modifiers,
                                                   (step.get_bool("repeat") ? WEFT_KEY_REPEAT : 0u) |
                                                       (step.get_bool("ime") ? WEFT_KEY_IME : 0u),
                                                   0,
                                                   0};
                            std::uint32_t handled{};
                            const weft_result r = weft_view_key(run.runtime, run.view, &e, &handled);
                            std::printf("[key] %s %s %s -> %s%s%s\n", key->string.c_str(),
                                        kind == WEFT_KEY_DOWN ? "down" : "up", value.c_str(), weft_result_string(r),
                                        handled & WEFT_INPUT_PREVENTED ? " prevented" : "",
                                        handled & WEFT_INPUT_DEFAULT ? " default" : "");
                        };
                        if (action == "down" || action == "press") {
                            send(WEFT_KEY_DOWN);
                            if (!typed.empty()) {
                                std::uint32_t handled{};
                                weft_view_text(run.runtime, run.view, typed.data(), typed.size(), &handled);
                            }
                        }
                        if (action == "up" || action == "press") {
                            send(WEFT_KEY_UP);
                        }
                        if (action != "down" && action != "up" && action != "press") {
                            throw Failure("key action wants down, up or press");
                        }
                    }
                    if (const Json* text = step.find("text"); text && !step.find("key")) {
                        std::uint32_t handled{};
                        const weft_result r =
                            weft_view_text(run.runtime, run.view, text->string.data(), text->string.size(), &handled);
                        std::printf("[text] %s -> %s%s\n", text->string.c_str(), weft_result_string(r),
                                    handled & WEFT_INPUT_DEFAULT ? " default" : "");
                    }
                    if (const Json* focus = step.find("focus")) {
                        weft_view_set_focus(run.runtime, run.view, step.get_bool("focus", true) ? 1 : 0);
                    }
                    if (const Json* hit = step.find("hit"); hit && hit->items.size() == 2) {
                        weft_hit h{};
                        h.struct_size = sizeof(h);
                        weft_view_hit_test(run.runtime, run.view, float(hit->items[0].number),
                                           float(hit->items[1].number), &h);
                        std::printf("[hit] %g,%g -> element=%u cursor=%s\n", hit->items[0].number, hit->items[1].number,
                                    h.element, h.cursor);
                    }
                    if (const Json* eval = step.find("eval")) {
                        const weft_result r = weft_view_evaluate(
                            run.runtime, run.view, eval->string.data(), eval->string.size(),
                            [](void*, uint32_t ok, const char* text, size_t size) {
                                std::printf("[eval%s] %.*s\n", ok ? "" : ":error", int(size), text);
                            },
                            nullptr);
                        if (r != WEFT_OK) {
                            std::printf("[eval] %s\n", weft_result_string(r));
                        }
                    }
                    if (step.get_bool("storage")) {
                        for (const auto& [key, value] : storage_items(run, nullptr)) {
                            std::printf("[storage] %s = %s\n", key.c_str(), value.c_str());
                        }
                    }
                    if (step.get_bool("reload")) {
                        loaded = load(run, page);
                        status = loaded ? status : 1;
                    }
                }
            }
            const auto start = std::chrono::steady_clock::now();
            if (const weft_result r = weft_view_advance(run.runtime, run.view, double(n) / o.fps); r != WEFT_OK) {
                std::printf("[advance] frame %ld: %s\n", n, weft_result_string(r));
            }
            advance_ms.push_back(
                std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count());
            if (advance_ms.back() > slow_ms) {
                std::printf("[slow] frame %ld: advance %.1f ms\n", n, advance_ms.back());
            }
            run.drain();
            std::string shot;
            if (steps) {
                for (const Json& step : steps->items) {
                    if (long(step.get_number("frame", -1)) == n && step.find("shot")) {
                        shot = step.get_string("shot");
                    }
                }
            }
            const bool periodic = o.render_every && n % long(o.render_every) == 0;
            if (shot.empty() && !periodic) {
                continue;
            }
            Drawer* d = ensure_drawer();
            if (!d) {
                std::printf("[render] no D3D11 device\n");
                return skipped;
            }
            std::vector<std::uint8_t> pixels;
            unsigned width{}, height{};
            const auto drawn = d->render_ms().size();
            const bool done =
                d->draw(run.runtime, run.view, unsigned(n), shot.empty() ? nullptr : &pixels, &width, &height);
            if (d->render_ms().size() > drawn && d->render_ms().back() > slow_ms) {
                std::printf("[slow] frame %ld: render %.1f ms\n", n, d->render_ms().back());
            }
            if (done && !shot.empty()) {
                const auto path = o.out / utf8_path(shot);
                write_png(path, width, height, pixels);
                std::printf("[shot] frame %ld -> %s (%ux%u)\n", n, utf8(path).c_str(), width, height);
            }
        }
        if (!o.storage.empty()) {
            std::uint64_t revision{};
            const auto items = storage_items(run, &revision);
            if (revision != restored) {
                std::string text = "{";
                for (std::size_t i = 0; i < items.size(); ++i) {
                    text += (i ? ",\n  " : "\n  ") + json_quote(items[i].first) + ": " + json_quote(items[i].second);
                }
                text += items.empty() ? "}\n" : "\n}\n";
                std::ofstream(o.storage, std::ios::binary) << text;
                std::printf("[storage] %zu items to %s\n", items.size(), utf8(o.storage).c_str());
            }
        }
        std::printf("[done] %ld frames, %u diagnostics, %u messages; advance %s", frames, run.diagnostics, run.messages,
                    times(advance_ms).c_str());
        if (drawer && drawer->available()) {
            std::printf("; render CPU %s", times(drawer->render_ms()).c_str());
        }
        std::printf("\n");
        drawer.reset();
        weft_view_destroy(run.runtime, run.view);
        weft_runtime_destroy(run.runtime);
        return status;
    }
} // namespace

int main(int argc, char** argv) {
    SetConsoleOutputCP(CP_UTF8);
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    try {
        return run(parse(argc, argv));
    } catch (const std::exception& e) {
        std::fprintf(stderr, "weft_page_runner: %s\n", e.what());
        return 1;
    }
}
