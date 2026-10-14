#include "bridge/bridge_value.hpp"
#include "bridge/engine_channel.hpp"
#include "constants/bridge.hpp"
#include "support/check.hpp"

#include <algorithm>
#include <atomic>
#include <bit>
#include <cmath>
#include <cstdint>
#include <functional>
#include <iostream>
#include <limits>
#include <map>
#include <random>
#include <span>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

// The engine bridge's values and channel (docs/engine-communication.md, 6 and 7): random round trips, the smallest
// forms, what is refused, JSON, and the channel's order, replacement, bounds, stale completions and threads.
namespace {
    namespace bridge = weft::bridge;
    namespace bounds = weft::constants::bridge;
    using bridge::ChannelStatus;
    using bridge::EngineInput;
    using bridge::ValueReader;
    using bridge::ValueWriter;
    using Type = ValueReader::Type;
    using weft::testing::require;
    using Bytes = std::vector<std::uint8_t>;

    // A value as a tree.
    struct Tree {
        Type type{};
        bool boolean{};
        std::int64_t integer{};
        std::uint64_t unsigned_integer{};
        double number{};
        std::string text;
        Bytes bytes;
        std::vector<std::string> keys;
        std::vector<Tree> items;
    };

    // Equal, floats by their bits (so -0 is not 0).
    bool same(const Tree& a, const Tree& b) {
        if (a.type != b.type || a.items.size() != b.items.size() || a.keys != b.keys) {
            return false;
        }
        switch (a.type) {
            case Type::Nil:
                break;
            case Type::Bool:
                return a.boolean == b.boolean;
            case Type::Int:
                return a.integer == b.integer;
            case Type::Unsigned:
                return a.unsigned_integer == b.unsigned_integer;
            case Type::Float:
                return std::bit_cast<std::uint64_t>(a.number) == std::bit_cast<std::uint64_t>(b.number);
            case Type::String:
                return a.text == b.text;
            case Type::Bytes:
                return a.bytes == b.bytes;
            case Type::Array:
            case Type::Map:
                for (std::size_t i = 0; i < a.items.size(); ++i) {
                    if (!same(a.items[i], b.items[i])) {
                        return false;
                    }
                }
                break;
        }
        return true;
    }

    void write(ValueWriter& out, const Tree& t, bool patched) {
        switch (t.type) {
            case Type::Nil:
                out.nil();
                break;
            case Type::Bool:
                out.boolean(t.boolean);
                break;
            case Type::Int:
                out.integer(t.integer);
                break;
            case Type::Unsigned:
                out.unsigned_integer(t.unsigned_integer);
                break;
            case Type::Float:
                out.number(t.number);
                break;
            case Type::String:
                out.string(t.text);
                break;
            case Type::Bytes:
                out.bytes(t.bytes);
                break;
            case Type::Array:
            case Type::Map: {
                const bool map = t.type == Type::Map;
                std::size_t header{};
                if (patched) {
                    header = map ? out.open_map() : out.open_array();
                } else if (map) {
                    out.map(std::uint32_t(t.items.size()));
                } else {
                    out.array(std::uint32_t(t.items.size()));
                }
                for (std::size_t i = 0; i < t.items.size(); ++i) {
                    if (map) {
                        out.string(t.keys[i]);
                    }
                    write(out, t.items[i], patched);
                }
                if (patched) {
                    out.close(header, std::uint32_t(t.items.size()));
                }
                break;
            }
        }
    }

    Tree read(ValueReader& in) {
        const auto item = in.next();
        Tree t;
        t.type = item.type;
        t.boolean = item.boolean;
        t.integer = item.type == Type::Int ? item.integer : 0;
        t.unsigned_integer = item.type == Type::Unsigned ? item.unsigned_integer : 0;
        t.number = item.type == Type::Float ? item.number : 0;
        if (item.type == Type::String) {
            t.text = item.text;
        }
        if (item.type == Type::Bytes) {
            t.bytes.assign(item.bytes.begin(), item.bytes.end());
        }
        for (std::uint32_t i = 0; i < item.count; ++i) {
            if (item.type == Type::Map) {
                const auto key = in.next();
                require(key.type == Type::String, "a key that is not a string");
                t.keys.emplace_back(key.text);
            }
            t.items.push_back(read(in));
        }
        return t;
    }

    Tree read(std::span<const std::uint8_t> bytes) {
        ValueReader in(bytes);
        auto t = read(in);
        require(in.done(), "bytes past the value");
        return t;
    }

    void append_code(std::string& out, std::uint32_t code) {
        if (code < 0x80) {
            out.push_back(char(code));
        } else if (code < 0x800) {
            out.push_back(char(0xc0 | (code >> 6)));
            out.push_back(char(0x80 | (code & 0x3f)));
        } else if (code < 0x10000) {
            out.push_back(char(0xe0 | (code >> 12)));
            out.push_back(char(0x80 | ((code >> 6) & 0x3f)));
            out.push_back(char(0x80 | (code & 0x3f)));
        } else {
            out.push_back(char(0xf0 | (code >> 18)));
            out.push_back(char(0x80 | ((code >> 12) & 0x3f)));
            out.push_back(char(0x80 | ((code >> 6) & 0x3f)));
            out.push_back(char(0x80 | (code & 0x3f)));
        }
    }

    // Text of every UTF-8 length, control characters and quotes among it; now and then long enough for str16.
    std::string random_text(std::mt19937_64& random) {
        std::string out;
        const std::size_t n = random() % 16 == 0 ? 200 + random() % 400 : random() % 40;
        while (out.size() < n) {
            switch (random() % 5) {
                case 0:
                    out.push_back(char(random() % 0x80));
                    break;
                case 1:
                    out.push_back("\"\\/\n\t"[random() % 5]);
                    break;
                case 2:
                    append_code(out, std::uint32_t(0x80 + random() % (0x800 - 0x80)));
                    break;
                case 3: {
                    auto code = std::uint32_t(0x800 + random() % (0x10000 - 0x800));
                    if (code >= 0xd800 && code <= 0xdfff) {
                        code -= 0x800;
                    }
                    append_code(out, code);
                    break;
                }
                default:
                    append_code(out, std::uint32_t(0x10000 + random() % (0x110000 - 0x10000)));
                    break;
            }
        }
        return out;
    }

    Tree random_tree(std::mt19937_64& random, unsigned depth) {
        Tree t;
        const auto pick = random() % (depth < 6 ? 12 : 9);
        if (pick >= 9) {
            t.type = pick == 9 ? Type::Map : Type::Array;
            const std::size_t count = random() % 8 == 0 ? 16 + random() % 24 : random() % 6;
            for (std::size_t i = 0; i < count; ++i) {
                if (t.type == Type::Map) {
                    t.keys.push_back(random_text(random));
                }
                t.items.push_back(random_tree(random, depth + 1));
            }
            return t;
        }
        static constexpr std::int64_t edges[]{0,
                                              1,
                                              127,
                                              128,
                                              255,
                                              256,
                                              65535,
                                              65536,
                                              4294967295,
                                              4294967296,
                                              std::numeric_limits<std::int64_t>::max(),
                                              -1,
                                              -32,
                                              -33,
                                              -128,
                                              -129,
                                              -32768,
                                              -32769,
                                              std::numeric_limits<std::int32_t>::min(),
                                              std::int64_t(std::numeric_limits<std::int32_t>::min()) - 1,
                                              std::numeric_limits<std::int64_t>::min()};
        switch (pick) {
            case 0:
                t.type = Type::Nil;
                break;
            case 1:
                t.type = Type::Bool;
                t.boolean = random() % 2;
                break;
            case 2:
                t.type = Type::Int;
                t.integer = edges[random() % std::size(edges)];
                break;
            case 3:
                t.type = Type::Int;
                t.integer = std::int64_t(random()) >> (random() % 64);
                break;
            case 4: {
                const auto u = random() | (std::uint64_t(1) << 63);
                t.type = Type::Unsigned;
                t.unsigned_integer = u;
                break;
            }
            case 5: {
                double d{};
                do {
                    d = std::bit_cast<double>(random());
                } while (!std::isfinite(d));
                static constexpr double specials[]{
                    0.0,    -0.0, 1.5, std::numeric_limits<double>::denorm_min(), std::numeric_limits<double>::max(),
                    -1e-300};
                t.type = Type::Float;
                t.number = random() % 3 == 0 ? specials[random() % std::size(specials)] : d;
                break;
            }
            case 6:
            case 7:
                t.type = Type::String;
                t.text = random_text(random);
                break;
            default: {
                t.type = Type::Bytes;
                const std::size_t n = random() % 8 == 0 ? 256 + random() % 300 : random() % 20;
                for (std::size_t i = 0; i < n; ++i) {
                    t.bytes.push_back(std::uint8_t(random()));
                }
                break;
            }
        }
        return t;
    }

    Bytes encode(const Tree& t, bool patched = false) {
        ValueWriter out;
        write(out, t, patched);
        return out.take();
    }

    Bytes nested(unsigned depth, bool map, bool scalar) {
        ValueWriter out;
        for (unsigned i = 0; i < depth; ++i) {
            if (map) {
                out.map(1);
                out.string("a");
            } else {
                out.array(1);
            }
        }
        if (scalar) {
            out.integer(7);
        } else {
            out.array(0);
        }
        return out.take();
    }

    std::string why_refused(const Bytes& bytes) {
        std::string why;
        require(!bridge::valid_value(bytes, &why), "a malformed value was taken");
        return why;
    }

    void round_trips() {
        std::mt19937_64 random(0x5eed'b41d);
        std::size_t prefixes{};
        for (int round = 0; round < 3000; ++round) {
            const auto t = random_tree(random, 0);
            for (const bool patched : {false, true}) {
                const auto bytes = encode(t, patched);
                std::string why;
                require(bridge::valid_value(bytes, &why), "a written value refused: " + why);
                require(same(read(bytes), t), "a value changed on its round trip");
                ValueReader in(bytes);
                require(in.skip().size() == bytes.size() && in.done(), "skip did not take the whole value");
                // JSON text is a fixed point after one trip: value -> JSON -> value -> JSON.
                const auto json = bridge::value_to_json(bytes);
                require(bridge::value_to_json(bridge::value_from_json(json)) == json, "JSON changed on its round trip");
                auto longer = bytes;
                longer.push_back(0xc0);
                require(why_refused(longer).find("past the value") != std::string::npos, "a byte past the value");
                // Every strict prefix is refused.
                if (bytes.size() < 160) {
                    for (std::size_t n = 0; n < bytes.size(); ++n) {
                        require(!bridge::valid_value({bytes.data(), n}), "a cut value was taken");
                        ++prefixes;
                    }
                }
            }
        }
        require(prefixes > 10000, "too few prefixes checked");

        // Flipped bytes are taken or refused, never read past their end, and what is taken converts.
        for (int round = 0; round < 20000; ++round) {
            auto bytes = encode(random_tree(random, 2));
            for (int flips = 1 + int(random() % 3); flips > 0; --flips) {
                bytes[random() % bytes.size()] = std::uint8_t(random());
            }
            if (bridge::valid_value(bytes)) {
                (void)bridge::value_to_json(bytes);
                (void)read(bytes);
            }
        }
    }

    void forms() {
        struct Form {
            std::int64_t value;
            std::size_t size;
            std::uint8_t first;
        };

        for (const auto& f : std::vector<Form>{{0, 1, 0x00},
                                               {127, 1, 0x7f},
                                               {128, 2, 0xcc},
                                               {255, 2, 0xcc},
                                               {256, 3, 0xcd},
                                               {65535, 3, 0xcd},
                                               {65536, 5, 0xce},
                                               {4294967295, 5, 0xce},
                                               {4294967296, 9, 0xcf},
                                               {std::numeric_limits<std::int64_t>::max(), 9, 0xcf},
                                               {-1, 1, 0xff},
                                               {-32, 1, 0xe0},
                                               {-33, 2, 0xd0},
                                               {-128, 2, 0xd0},
                                               {-129, 3, 0xd1},
                                               {-32768, 3, 0xd1},
                                               {-32769, 5, 0xd2},
                                               {std::numeric_limits<std::int32_t>::min(), 5, 0xd2},
                                               {std::int64_t(std::numeric_limits<std::int32_t>::min()) - 1, 9, 0xd3},
                                               {std::numeric_limits<std::int64_t>::min(), 9, 0xd3}}) {
            ValueWriter out;
            out.integer(f.value);
            require(out.data().size() == f.size && out.data()[0] == f.first,
                    "integer " + std::to_string(f.value) + " not in its smallest form");
            const auto t = read(out.data());
            require(t.type == Type::Int && t.integer == f.value, "integer read back");
        }
        {
            ValueWriter out;
            out.unsigned_integer(std::numeric_limits<std::uint64_t>::max());
            const auto t = read(out.data());
            require(out.data().size() == 9 && t.type == Type::Unsigned &&
                        t.unsigned_integer == std::numeric_limits<std::uint64_t>::max(),
                    "uint64 max");
        }

        struct Sized {
            std::size_t count;
            std::size_t header;
            std::uint8_t first;
        };

        for (const auto& s : std::vector<Sized>{{0, 1, 0xa0},
                                                {31, 1, 0xbf},
                                                {32, 2, 0xd9},
                                                {255, 2, 0xd9},
                                                {256, 3, 0xda},
                                                {65535, 3, 0xda},
                                                {65536, 5, 0xdb}}) {
            ValueWriter out;
            out.string(std::string(s.count, 'x'));
            require(out.data().size() == s.header + s.count && out.data()[0] == s.first, "string header");
        }
        for (const auto& s : std::vector<Sized>{{0, 2, 0xc4}, {255, 2, 0xc4}, {256, 3, 0xc5}, {65536, 5, 0xc6}}) {
            ValueWriter out;
            out.bytes(Bytes(s.count, 7));
            require(out.data().size() == s.header + s.count && out.data()[0] == s.first, "bytes header");
        }
        for (const auto& s : std::vector<Sized>{{15, 1, 0x9f}, {16, 3, 0xdc}, {65536, 5, 0xdd}}) {
            ValueWriter out;
            out.array(std::uint32_t(s.count));
            for (std::size_t i = 0; i < s.count; ++i) {
                out.nil();
            }
            require(out.data().size() == s.header + s.count && out.data()[0] == s.first, "array header");
            require(bridge::valid_value(out.data()), "array");
        }
        {
            ValueWriter out;
            out.map(16);
            for (int i = 0; i < 16; ++i) {
                out.string("k");
                out.nil();
            }
            require(out.data()[0] == 0xde && bridge::valid_value(out.data()), "map16");
            ValueWriter number;
            number.number(1.5);
            require(number.data().size() == 9 && number.data()[0] == 0xcb, "floats are float64");
        }
        // Forms a writer never makes are still read.
        require(read(Bytes{0xd0, 0x01}).integer == 1 && read(Bytes{0xcd, 0x00, 0x01}).integer == 1,
                "integers in larger forms");
        require(read(Bytes{0xd9, 0x01, 'a'}).text == "a", "str8 of one byte");
        require(read(Bytes{0xdc, 0x00, 0x01, 0x01}).items.size() == 1, "array16 of one item");
        require(read(Bytes{0xca, 0x3f, 0xc0, 0x00, 0x00}).number == 1.5, "float32");
        // Writers refuse what no value holds.
        for (const auto& refused :
             std::vector<std::function<void(ValueWriter&)>>{[](ValueWriter& w) {
                                                                w.number(std::nan(""));
                                                            },
                                                            [](ValueWriter& w) {
                                                                w.number(std::numeric_limits<double>::infinity());
                                                            },
                                                            [](ValueWriter& w) {
                                                                w.string("\xff");
                                                            },
                                                            [](ValueWriter& w) {
                                                                w.string("\xed\xa0\x80");
                                                            }}) {
            ValueWriter w;
            bool threw = false;
            try {
                refused(w);
            } catch (const std::invalid_argument&) {
                threw = true;
            }
            require(threw, "a writer took what no value holds");
        }
    }

    void refusals() {
        struct Case {
            Bytes bytes;
            const char* why;
        };

        for (const auto& c : std::vector<Case>{
                 {{}, "truncated"},
                 {{0xc1}, "outside the subset"},
                 {{0xd4, 0x01, 0x02}, "outside the subset"},
                 {{0xd5, 0x01, 0x02, 0x03}, "outside the subset"},
                 {{0xc7, 0x01, 0x05, 0x00}, "outside the subset"},
                 {{0xc8, 0x00, 0x01, 0x05, 0x00}, "outside the subset"},
                 {{0xc9, 0x00, 0x00, 0x00, 0x01, 0x05, 0x00}, "outside the subset"},
                 {{0xcb, 0x7f, 0xf8, 0, 0, 0, 0, 0, 0}, "not finite"},
                 {{0xcb, 0x7f, 0xf0, 0, 0, 0, 0, 0, 0}, "not finite"},
                 {{0xcb, 0xff, 0xf0, 0, 0, 0, 0, 0, 0}, "not finite"},
                 {{0xca, 0x7f, 0xc0, 0x00, 0x00}, "not finite"},
                 {{0xa2, 0xc0, 0x80}, "not UTF-8"},
                 {{0xa3, 0xed, 0xa0, 0x80}, "not UTF-8"},
                 {{0xa4, 0xf4, 0x90, 0x80, 0x80}, "not UTF-8"},
                 {{0xa1, 0x80}, "not UTF-8"},
                 {{0xa1, 0xe2}, "not UTF-8"},
                 {{0xa2, 0xe2, 0x82}, "not UTF-8"},
                 {{0xcd, 0x01}, "truncated"},
                 {{0xa5, 'a', 'b'}, "truncated"},
                 {{0xc4, 0x05, 0x01}, "truncated"},
                 {{0xdd, 0xff, 0xff, 0xff, 0xff}, "count past the bytes"},
                 {{0x92, 0x01}, "count past the bytes"},
                 {{0x81, 0xa1, 'a'}, "truncated"},
                 {{0x81, 0x01, 0x02}, "not a string"},
                 {{0x82, 0xa1, 'a', 0x01, 0xc3, 0x02}, "not a string"},
                 {{0x01, 0x02}, "past the value"},
             }) {
            const auto why = why_refused(c.bytes);
            require(why.find(c.why) != std::string::npos, "refused for " + why + ", not " + c.why);
        }
        // Depth counts the arrays and maps a value nests: 64 hold a scalar or an empty array, 65 do not.
        for (const bool map : {false, true}) {
            for (const bool scalar : {false, true}) {
                const auto deepest = nested(bounds::max_value_depth - (scalar ? 0 : 1), map, scalar);
                require(bridge::valid_value(deepest), "the deepest value refused");
                require(bridge::value_to_json(deepest).size() > bounds::max_value_depth,
                        "the deepest value has no JSON");
                const auto json = bridge::value_to_json(deepest);
                require(bridge::value_to_json(bridge::value_from_json(json)) == json, "the deepest JSON");
                const auto deeper = nested(bounds::max_value_depth + (scalar ? 1 : 0), map, scalar);
                require(why_refused(deeper).find("depth") != std::string::npos, "nested past the depth bound");
                bool threw = false;
                try {
                    (void)bridge::value_to_json(deeper);
                } catch (const std::invalid_argument&) {
                    threw = true;
                }
                require(threw, "JSON of a value past the depth bound");
            }
        }
        // Size: a value of max_value_bytes is taken, one byte more is not.
        {
            const std::size_t header = 5;
            ValueWriter out;
            out.string(std::string(bounds::max_value_bytes - header, 'x'));
            require(out.data().size() == bounds::max_value_bytes && bridge::valid_value(out.data()), "the largest");
            ValueWriter over;
            over.string(std::string(bounds::max_value_bytes - header + 1, 'x'));
            require(why_refused(over.take()).find("size") != std::string::npos, "past the size bound");
        }
    }

    void json() {
        const auto text = [](std::string_view json) {
            return bridge::value_to_json(bridge::value_from_json(json));
        };
        require(text(" \t\r\n{\"a\" : [1, -2, 3.5, true, false, null, \"x\", {}, []]} \n") ==
                    R"({"a":[1,-2,3.5,true,false,null,"x",{},[]]})",
                "JSON whitespace and literals");
        require(text(R"("\"\\\/\b\f\n\r\t\u00e9\ud83d\ude00\u0001")") ==
                    "\"\\\"\\\\/\\u0008\\u000c\\n\\r\\t\xc3\xa9\xf0\x9f\x98\x80\\u0001\"",
                "JSON escapes and a surrogate pair");

        struct Number {
            const char* json;
            Type type;
        };

        for (const auto& n : std::vector<Number>{{"0", Type::Int},
                                                 {"9223372036854775807", Type::Int},
                                                 {"9223372036854775808", Type::Unsigned},
                                                 {"18446744073709551615", Type::Unsigned},
                                                 {"18446744073709551616", Type::Float},
                                                 {"-9223372036854775808", Type::Int},
                                                 {"-9223372036854775809", Type::Float},
                                                 {"-0", Type::Float},
                                                 {"1e2", Type::Float},
                                                 {"1.0", Type::Float},
                                                 {"-1.5E-3", Type::Float}}) {
            require(read(bridge::value_from_json(n.json)).type == n.type, std::string("JSON number type ") + n.json);
        }
        const auto zero = read(bridge::value_from_json("-0"));
        require(std::signbit(zero.number), "JSON -0 lost its sign");
        require(read(bridge::value_from_json("18446744073709551615")).unsigned_integer ==
                    std::numeric_limits<std::uint64_t>::max(),
                "JSON uint64 max");
        std::string deepest(bounds::max_value_depth, '[');
        deepest += std::string(bounds::max_value_depth, ']');
        require(text(deepest) == deepest, "JSON at the depth bound");
        for (const std::string bad : {"",
                                      " ",
                                      "01",
                                      "-01",
                                      "+1",
                                      ".5",
                                      "1.",
                                      "1e",
                                      "1e+",
                                      "-",
                                      "NaN",
                                      "Infinity",
                                      "-Infinity",
                                      "[1,]",
                                      "{\"a\":1,}",
                                      "{a:1}",
                                      "'x'",
                                      "\"\x01\"",
                                      "\"\\x\"",
                                      "\"\\ud800\"",
                                      "\"\\udc00\"",
                                      "\"\\ud800\\u0041\"",
                                      "\"\\u12\"",
                                      "[1] 2",
                                      "// c\n1",
                                      "[",
                                      "{\"a\"}",
                                      "{\"a\" 1}",
                                      "tru",
                                      "nul",
                                      "\"abc",
                                      "1e400",
                                      "\"\xff\"",
                                      "\"\xc0\x80\"",
                                      "[1 2]",
                                      "{\"a\":1 \"b\":2}",
                                      "\"\\u00\""}) {
            bool threw = false;
            try {
                (void)bridge::value_from_json(bad);
            } catch (const std::invalid_argument&) {
                threw = true;
            }
            require(threw, "JSON taken: " + bad);
        }
        bool threw = false;
        try {
            (void)bridge::value_from_json("[" + deepest + "]");
        } catch (const std::invalid_argument&) {
            threw = true;
        }
        require(threw, "JSON past the depth bound");
        threw = false;
        try {
            (void)bridge::value_from_json("\"" + std::string(bounds::max_value_bytes, 'x') + "\"");
        } catch (const std::invalid_argument&) {
            threw = true;
        }
        require(threw, "JSON past the size bound");
    }

    Bytes integer(std::int64_t value) {
        ValueWriter out;
        out.integer(value);
        return out.take();
    }

    std::int64_t integer_of(const Bytes& value) {
        return read(value).integer;
    }

    void channel_order() {
        bridge::EngineChannel channel;
        const auto call = channel.open_call();
        require(call != 0 && channel.open_calls() == 1, "a call opens");
        require(channel.emit("a", integer(1)) == ChannelStatus::Ok, "emit");
        require(channel.complete(call, true, integer(2)) == ChannelStatus::Ok, "complete");
        require(channel.emit_json("b", "[3]") == ChannelStatus::Ok, "emit JSON");
        auto pending = channel.take();
        require(pending.inputs.size() == 3 && pending.inputs[0].kind == EngineInput::Kind::Event &&
                    pending.inputs[0].name == "a" && pending.inputs[1].kind == EngineInput::Kind::Completion &&
                    pending.inputs[1].call == call && pending.inputs[1].succeeded && pending.inputs[2].name == "b" &&
                    bridge::value_to_json(pending.inputs[2].value) == "[3]",
                "inputs in the order given");
        require(channel.take().inputs.empty(), "a take takes all");

        // latest replaces the last undelivered event of the name in its place; others queue.
        channel.emit("pos", integer(1), true);
        channel.emit("other", integer(2));
        channel.emit("pos", integer(3), true);
        channel.emit("twice", integer(4));
        channel.emit("twice", integer(5));
        channel.emit("twice", integer(6), true);
        pending = channel.take();
        require(pending.inputs.size() == 4 && pending.inputs[0].name == "pos" &&
                    integer_of(pending.inputs[0].value) == 3 && pending.inputs[1].name == "other" &&
                    integer_of(pending.inputs[2].value) == 4 && integer_of(pending.inputs[3].value) == 6,
                "latest replaces in place");
        channel.emit("pos", integer(7), true);
        require(channel.take().inputs.size() == 1, "latest after a take queues");

        // Names: 1 to 256 bytes of UTF-8; values: the subset.
        require(channel.emit("", integer(1)) == ChannelStatus::Invalid, "an empty name");
        require(channel.emit(std::string(bounds::max_name_bytes, 'n'), integer(1)) == ChannelStatus::Ok, "256");
        require(channel.emit(std::string(bounds::max_name_bytes + 1, 'n'), integer(1)) == ChannelStatus::Invalid,
                "257 bytes");
        require(channel.emit("\xff", integer(1)) == ChannelStatus::Invalid, "a name that is not UTF-8");
        require(channel.emit("a", Bytes{0xc1}) == ChannelStatus::Invalid, "a value outside the subset");
        require(channel.emit("a", Bytes{}) == ChannelStatus::Invalid, "no value");
        require(channel.emit_json("a", "{") == ChannelStatus::Invalid, "JSON that is not");
        require(channel.complete_json(channel.open_call(), true, "nope") == ChannelStatus::Invalid,
                "completion JSON that is not");
        channel.take();
        channel.reset();
    }

    void channel_bounds() {
        bridge::EngineChannel channel;
        for (std::size_t i = 0; i < bounds::max_pending_inputs; ++i) {
            require(channel.emit("e", integer(1)) == ChannelStatus::Ok, "emit within the bound");
        }
        require(channel.emit("e", integer(1)) == ChannelStatus::Full, "past max_pending_inputs");
        require(channel.emit("e", integer(9), true) == ChannelStatus::Ok, "latest replaces when full");
        const auto call = channel.open_call();
        require(channel.complete(call, true, integer(1)) == ChannelStatus::Full, "complete when full");
        require(channel.open_calls() == 1, "a full queue keeps the call open");
        auto pending = channel.take();
        require(pending.inputs.size() == bounds::max_pending_inputs && integer_of(pending.inputs.back().value) == 9,
                "nothing taken past the bound");
        require(channel.complete(call, false, integer(1)) == ChannelStatus::Ok, "complete after the take");

        // Bytes: 16 values of nearly 1 MiB fill max_pending_bytes.
        channel.take();
        ValueWriter big;
        big.string(std::string(bounds::max_value_bytes - 1000, 'x'));
        const auto value = big.take();
        std::size_t taken{};
        while (channel.emit("big", value) == ChannelStatus::Ok) {
            ++taken;
        }
        require(taken == bounds::max_pending_bytes / bounds::max_value_bytes, "max_pending_bytes");
        require(channel.emit("small", integer(1)) == ChannelStatus::Ok, "small values fit after");
        require(channel.set_model_json("m", R"({"a":1})") == ChannelStatus::Ok, "a small model fits");
        ValueWriter fields;
        fields.map(1);
        fields.string("f");
        fields.string(std::string(bounds::max_value_bytes - 16, 'y'));
        require(channel.set_model("m", fields.data()) == ChannelStatus::Full, "a model past max_pending_bytes");
        pending = channel.take();
        require(pending.models.size() == 1 && pending.models[0].fields.size() == 1, "a full model merges nothing");

        // Calls: max_open_calls at once; ids never 0 and never reused.
        std::vector<std::uint64_t> ids;
        for (std::size_t i = 0; i < bounds::max_open_calls; ++i) {
            ids.push_back(channel.open_call());
        }
        require(std::ranges::find(ids, 0) == ids.end() && channel.open_call() == 0, "max_open_calls");
        channel.close_call(ids[5]);
        const auto next = channel.open_call();
        require(next > ids.back(), "ids are never reused");
        channel.reset();
        require(channel.open_calls() == 0 && channel.open_call() > next, "a reset closes calls; ids go on");
    }

    void channel_stale() {
        bridge::EngineChannel channel;
        const auto a = channel.open_call();
        const auto b = channel.open_call();
        const auto c = channel.open_call();
        require(channel.complete(a, true, integer(1)) == ChannelStatus::Ok, "complete");
        require(channel.complete(a, true, integer(1)) == ChannelStatus::Stale, "completed twice");
        channel.close_call(b);
        require(channel.complete(b, true, integer(1)) == ChannelStatus::Stale, "completed after a close");
        require(channel.complete(12345, true, integer(1)) == ChannelStatus::Stale, "never opened");
        require(channel.complete(0, true, integer(1)) == ChannelStatus::Stale, "call 0");
        channel.emit("e", integer(1));
        channel.set_model_json("m", R"({"x":1})");
        channel.reset();
        require(channel.complete(c, true, integer(1)) == ChannelStatus::Stale, "completed after a reset");
        const auto pending = channel.take();
        require(pending.inputs.empty() && pending.models.empty(), "a reset drops what waits");
    }

    void channel_models() {
        bridge::EngineChannel channel;
        require(channel.set_model_json("player", R"({"hp":1,"name":"a"})") == ChannelStatus::Ok, "a model");
        require(channel.set_model_json("player", R"({"hp":2})") == ChannelStatus::Ok, "a model again");
        require(channel.set_model_json("enemy", R"({"hp":9})") == ChannelStatus::Ok, "another model");
        require(channel.set_model_json("player", R"({"hp":3,"hp":4})") == ChannelStatus::Ok, "a key twice");
        auto pending = channel.take();
        require(pending.models.size() == 2 && pending.models[0].name == "enemy" && pending.models[1].name == "player",
                "models by name");
        const auto& player = pending.models[1].fields;
        require(player.size() == 2 && player[0].first == "hp" && integer_of(player[0].second) == 4 &&
                    player[1].first == "name" && read(player[1].second).text == "a",
                "fields merge, the last value of each kept");
        require(channel.set_model_json("m", "[1]") == ChannelStatus::Invalid, "a model that is not a map");
        require(channel.set_model_json("m", R"({"":1})") == ChannelStatus::Invalid, "an empty field name");
        require(channel.set_model_json("", R"({"a":1})") == ChannelStatus::Invalid, "an empty model name");
        require(channel.set_model_json("m", "{}") == ChannelStatus::Ok && channel.take().models.size() == 1,
                "an empty batch still names its model");

        // A key given twice counts its bytes once: 30 such batches of 0.9 MiB hold 13.5 MiB, under 16.
        const std::string half(std::size_t(0.45 * double(bounds::max_value_bytes)), 'h');
        for (int i = 0; i < 30; ++i) {
            ValueWriter fields;
            fields.map(2);
            for (int k = 0; k < 2; ++k) {
                fields.string("k" + std::to_string(i));
                fields.string(half);
            }
            require(channel.set_model("m", fields.data()) == ChannelStatus::Ok, "a key twice counted twice");
        }
        require(channel.take().models[0].fields.size() == 30, "30 fields");
    }

    // Producers on threads give while the owner takes: per producer in order and none lost, latest keeps its order,
    // and of two threads completing each call exactly one wins.
    void channel_threads() {
        bridge::EngineChannel channel;
        constexpr int producers = 6;
        constexpr int each = 20000;
        constexpr int calls = 2000;
        std::vector<std::uint64_t> ids;
        for (int i = 0; i < calls; ++i) {
            ids.push_back(channel.open_call());
        }
        std::atomic<int> running{producers + 3};
        std::atomic<int> won{};
        std::vector<std::thread> threads;
        for (int p = 0; p < producers; ++p) {
            threads.emplace_back([&, p] {
                const std::string name = "p" + std::to_string(p);
                for (int i = 0; i < each;) {
                    if (channel.emit(name, integer(i)) == ChannelStatus::Ok) {
                        ++i;
                    } else {
                        std::this_thread::yield();
                    }
                }
                --running;
            });
        }
        threads.emplace_back([&] {
            for (int i = 0; i < each; ++i) {
                while (channel.emit("pos", integer(i), true) != ChannelStatus::Ok) {
                    std::this_thread::yield();
                }
            }
            --running;
        });
        for (int t = 0; t < 2; ++t) {
            threads.emplace_back([&] {
                for (const auto id : ids) {
                    for (;;) {
                        const auto status = channel.complete(id, true, integer(std::int64_t(id)));
                        if (status == ChannelStatus::Ok) {
                            ++won;
                        }
                        if (status != ChannelStatus::Full) {
                            break;
                        }
                        std::this_thread::yield();
                    }
                }
                --running;
            });
        }
        std::vector<std::int64_t> last(producers, -1);
        std::int64_t position = -1;
        std::map<std::uint64_t, int> completed;
        const auto drain = [&] {
            for (const auto& input : channel.take().inputs) {
                if (input.kind == EngineInput::Kind::Completion) {
                    require(integer_of(input.value) == std::int64_t(input.call), "a completion's value");
                    ++completed[input.call];
                    continue;
                }
                const auto value = integer_of(input.value);
                if (input.name == "pos") {
                    require(value > position, "latest went back");
                    position = value;
                    continue;
                }
                auto& seen = last[std::size_t(input.name[1] - '0')];
                require(value == seen + 1, "a producer's events out of order or lost");
                seen = value;
            }
        };
        while (running > 0) {
            drain();
            std::this_thread::yield();
        }
        for (auto& t : threads) {
            t.join();
        }
        drain();
        require(std::ranges::all_of(last,
                                    [](std::int64_t v) {
                                        return v == each - 1;
                                    }),
                "every event delivered");
        require(position == each - 1, "the last position delivered");
        require(won == calls && completed.size() == std::size_t(calls) &&
                    std::ranges::all_of(completed,
                                        [](const auto& c) {
                                            return c.second == 1;
                                        }),
                "each call completed once");
        require(channel.open_calls() == 0, "no call left open");
    }
} // namespace

int main() {
    try {
        round_trips();
        forms();
        refusals();
        json();
        channel_order();
        channel_bounds();
        channel_stale();
        channel_models();
        channel_threads();
        std::cout << "bridge tests passed\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "bridge tests failed: " << e.what() << '\n';
        return 1;
    }
}
