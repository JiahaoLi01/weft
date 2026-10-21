#include "bridge/bridge_value.hpp"
#include "bridge/engine_channel.hpp"
#include "constants/bridge.hpp"
#include "constants/script.hpp"
#include "script/script_dom.hpp"
#include "script/script_platform.hpp"
#include "../support/check.hpp"

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstdint>
#include <iostream>
#include <limits>
#include <memory>
#include <random>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

// weft.engine over a ScriptDom (docs/engine-communication.md, 6): values both ways, events, calls, sync functions,
// view models, resets, and CRPG Framework's bridge over it.
namespace {
    namespace bridge = weft::bridge;
    using bridge::ChannelStatus;
    using weft::ScriptPlatformModule;
    using weft::testing::require;
    using Bytes = std::vector<std::uint8_t>;

    struct Page {
        weft::ScriptDom vm{weft::ScriptConfig{weft::constants::script::probe_vm_stack_bytes}};
        std::shared_ptr<bridge::EngineChannel> channel = std::make_shared<bridge::EngineChannel>();

        explicit Page(bool engine = true) {
            if (engine) {
                vm.install_engine(channel);
            }
            install();
        }

        void install() {
            weft::install_script_platform(vm, ScriptPlatformModule::WebHost);
            weft::install_script_platform(vm, ScriptPlatformModule::Engine);
        }

        std::string eval(std::string_view source) {
            return vm.eval(source);
        }
    };

    std::string json(std::span<const std::uint8_t> value) {
        return bridge::value_to_json(value);
    }

    template <class Action>
    std::string thrown(Action&& action) {
        try {
            action();
        } catch (const std::exception& e) {
            return e.what();
        }
        return {};
    }

    bridge::ValueReader::Type type_of(const Bytes& value, std::string_view key) {
        bridge::ValueReader reader(value);
        const auto map = reader.next();
        for (std::uint32_t i = 0; i < map.count; ++i) {
            const auto name = reader.next();
            if (name.text == key) {
                return reader.next().type;
            }
            reader.skip();
        }
        throw std::runtime_error("no member " + std::string(key));
    }

    void values_out() {
        Page page;
        page.eval(R"JS(
          weft.engine.send('kinds', {n: null, u: undefined, t: true, f: false, i: 42, neg: -7, big: 2 ** 53,
            safe: 2 ** 53 - 1, frac: 1.5, huge: 1e300, nz: -0, s: 'é😀', lone: 'a\ud800b', arr: [1, , 3],
            nested: {a: [{b: 1}]}, bi: 12345678901234567890n, nbi: -5n, bytes: new Uint8Array([1, 2, 3]),
            buf: new Uint8Array([9, 8, 7, 6]).buffer, view: new Uint8Array([0, 1, 2, 3, 4]).subarray(1, 3),
            f32: new Float32Array([1.5]), ['__proto__']: 1});
          const shared = {v: 1};
          weft.engine.send('shared', {a: shared, b: [shared, shared]});
          weft.engine.send('bare');
          // As in JSON.stringify: a proxy of an array is an array, and toJSON is what crosses (once).
          const proxied = new Proxy(new Proxy([1, {x: 2}], {}), {});
          const stamp = {toJSON: () => ({at: new Date(0), toJSON: 1})};
          weft.engine.send('as-json', [proxied, new Proxy({k: 'v'}, {}), stamp, {toJSON: () => new Date(0)}]);
        )JS");
        auto messages = page.vm.take_engine_messages();
        require(messages.size() == 4 && messages[0].call == 0 && messages[0].name == "kinds", "a send");
        require(
            json(messages[0].value) ==
                "{\"n\":null,\"t\":true,\"f\":false,\"i\":42,\"neg\":-7,\"big\":9007199254740992,"
                "\"safe\":9007199254740991,\"frac\":1.5,\"huge\":1e+300,\"nz\":-0,\"s\":\"\xc3\xa9\xf0\x9f\x98\x80\","
                "\"lone\":\"a\xef\xbf\xbd"
                "b\",\"arr\":[1,null,3],\"nested\":{\"a\":[{\"b\":1}]},\"bi\":12345678901234567890,\"nbi\":-5,"
                "\"bytes\":[1,2,3],\"buf\":[9,8,7,6],\"view\":[1,2],\"f32\":[0,0,192,63],\"__proto__\":1}",
            "values as the engine gets them: " + json(messages[0].value));
        using Type = bridge::ValueReader::Type;
        const auto& kinds = messages[0].value;
        require(type_of(kinds, "i") == Type::Int && type_of(kinds, "safe") == Type::Int &&
                    type_of(kinds, "big") == Type::Float && type_of(kinds, "nz") == Type::Float &&
                    type_of(kinds, "bi") == Type::Unsigned && type_of(kinds, "nbi") == Type::Int &&
                    type_of(kinds, "bytes") == Type::Bytes,
                "integers within 2^53, floats past them, BigInts as integers, bytes");
        require(json(messages[1].value) == R"({"a":{"v":1},"b":[{"v":1},{"v":1}]})", "a value met twice is no cycle");
        require(json(messages[2].value) == "null", "a send without a value");
        require(json(messages[3].value) == R"([[1,{"x":2}],{"k":"v"},{"at":"1970-01-01T00:00:00.000Z","toJSON":1},{}])",
                "proxies and toJSON: " + json(messages[3].value));
        require(page.vm.take_engine_messages().empty(), "a take takes all");

        // What cannot cross throws where it was sent, and nothing is sent.
        page.eval(R"JS(
          globalThis.refused = [];
          const deep = n => { let v = 0; for (let i = 0; i < n; ++i) v = [v]; return v; };
          const cyclic = {}; cyclic.self = {back: cyclic};
          const loop = []; loop.push(loop);
          const revoked = Proxy.revocable([], {}); revoked.revoke();
          for (const [name, value] of [['fn', () => 1], ['symbol', Symbol('s')], ['member', {s: Symbol()}],
              ['cycle', cyclic], ['loop', loop], ['nan', NaN], ['inf', -Infinity], ['bigint', 2n ** 64n],
              ['negative', -(2n ** 63n) - 1n], ['deep', deep(65)], ['size', 'x'.repeat(1 << 20)],
              ['revoked', revoked.proxy], ['tofn', {toJSON: () => () => 1}]]) {
            try { weft.engine.send(name, value); refused.push(name + ':sent'); }
            catch (e) { refused.push(name + ':' + e.message); }
          }
          for (const name of [1, '', 'n'.repeat(257), 'a\ud800']) {
            try { weft.engine.send(name); refused.push('name:sent'); }
            catch (e) { refused.push(e.name + ':' + e.message); }
          }
          weft.engine.send('deepest', deep(64));
        )JS");
        require(page.eval("refused.join('|')") ==
                    "fn:WEFT_ENGINE_VALUE: a function cannot cross|symbol:WEFT_ENGINE_VALUE: a symbol cannot cross|"
                    "member:WEFT_ENGINE_VALUE: a symbol cannot cross|cycle:WEFT_ENGINE_VALUE: a cycle cannot cross|"
                    "loop:WEFT_ENGINE_VALUE: a cycle cannot cross|nan:WEFT_ENGINE_VALUE: a number that is not finite|"
                    "inf:WEFT_ENGINE_VALUE: a number that is not finite|"
                    "bigint:WEFT_ENGINE_VALUE: a BigInt past int64 and uint64|"
                    "negative:WEFT_ENGINE_VALUE: a BigInt past int64 and uint64|"
                    "deep:WEFT_ENGINE_VALUE: nested past the depth bound|"
                    "size:WEFT_ENGINE_VALUE: a value past the size bound|revoked:revoked proxy|"
                    "tofn:WEFT_ENGINE_VALUE: a function cannot cross|TypeError:weft.engine.send: a name|"
                    "InternalError:WEFT_ENGINE_NAME: a name of 1 to 256 bytes of UTF-8|"
                    "InternalError:WEFT_ENGINE_NAME: a name of 1 to 256 bytes of UTF-8|"
                    "InternalError:WEFT_ENGINE_NAME: a name of 1 to 256 bytes of UTF-8",
                "refusals: " + page.eval("refused.join('|')"));
        messages = page.vm.take_engine_messages();
        require(messages.size() == 1 && messages[0].name == "deepest" && bridge::valid_value(messages[0].value),
                "64 arrays deep crosses, and only it");

        // The page's messages are bounded until the host takes them.
        page.eval(R"JS(
          globalThis.full = '';
          for (let i = 0; i < 4096; ++i) weft.engine.send('m', i);
          try { weft.engine.send('m', 4096); } catch (e) { full = e.message; }
        )JS");
        require(page.eval("full") == "WEFT_ENGINE_FULL: the host has not taken the page's messages",
                "past max_pending_messages");
        messages = page.vm.take_engine_messages();
        require(messages.size() == weft::constants::bridge::max_pending_messages &&
                    json(messages.back().value) == "4095",
                "the messages in order");
        page.eval("weft.engine.send('m', 0)");
        require(page.vm.take_engine_messages().size() == 1, "a take makes room");
    }

    void values_in() {
        Page page;
        page.eval(R"JS(
          globalThis.got = [];
          weft.engine.on('v', v => got.push(v));
        )JS");
        bridge::ValueWriter w;
        w.map(9);
        w.string("max");
        w.unsigned_integer(std::numeric_limits<std::uint64_t>::max());
        w.string("unsafe");
        w.integer((std::int64_t(1) << 53) + 1);
        w.string("safe");
        w.integer((std::int64_t(1) << 53) - 1);
        w.string("low");
        w.integer(std::numeric_limits<std::int64_t>::min());
        w.string("bytes");
        w.bytes(Bytes{1, 2, 250});
        w.string("__proto__");
        w.map(1);
        w.string("polluted");
        w.boolean(true);
        w.string("nz");
        w.number(-0.0);
        w.string("text");
        w.string("\xe2\x82\xac");
        w.string("list");
        w.array(2);
        w.nil();
        w.number(0.25);
        require(page.channel->emit("v", w.data()) == ChannelStatus::Ok, "emit");
        const auto delivery = page.vm.deliver_engine();
        require(delivery.events == 1 && delivery.errors.empty(), "one event");
        require(page.eval(R"JS(
          const v = got[0];
          [typeof v.max, v.max === 18446744073709551615n, typeof v.unsafe, v.unsafe === 9007199254740993n,
           typeof v.safe, v.safe === 9007199254740991, v.low === -9223372036854775808n,
           v.bytes instanceof Uint8Array && v.bytes.join() === '1,2,250', Object.getPrototypeOf(v) === Object.prototype,
           Object.hasOwn(v, '__proto__') && v.__proto__.polluted === true, ({}).polluted === undefined,
           Object.is(v.nz, -0), v.text === '€', v.list[0] === null && v.list[1] === 0.25].join()
        )JS") == "bigint,true,bigint,true,number,true,true,true,true,true,true,true,true,true",
                "values as the page gets them");
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

    std::string random_text(std::mt19937_64& random) {
        std::string out;
        for (auto n = random() % 12; n > 0; --n) {
            switch (random() % 4) {
                case 0:
                    out.push_back(char(random() % 0x80));
                    break;
                case 1:
                    append_code(out, std::uint32_t(0x80 + random() % (0x800 - 0x80)));
                    break;
                case 2:
                    append_code(out, std::uint32_t(0xe000 + random() % 0x2000));
                    break;
                default:
                    append_code(out, std::uint32_t(0x10000 + random() % 0x100000));
                    break;
            }
        }
        return out;
    }

    // A random value whose way back from JS is the identity: no float with an integral value (JS cannot tell it from
    // an integer), no map key that is an array index (JS orders those first), no key twice.
    void random_value(bridge::ValueWriter& out, std::mt19937_64& random, unsigned depth) {
        switch (random() % (depth < 4 ? 11 : 8)) {
            case 0:
                out.nil();
                break;
            case 1:
                out.boolean(random() % 2);
                break;
            case 2:
                out.integer(std::int64_t(random()) >> (random() % 64));
                break;
            case 3:
                out.unsigned_integer(random() | (std::uint64_t(1) << 63));
                break;
            case 4: {
                double d{};
                do {
                    d = std::bit_cast<double>(random());
                } while (!std::isfinite(d) || d == std::floor(d));
                out.number(random() % 8 ? d : -0.0);
                break;
            }
            case 5:
            case 6:
                out.string(random_text(random));
                break;
            case 7: {
                Bytes bytes(random() % 20);
                for (auto& b : bytes) {
                    b = std::uint8_t(random());
                }
                out.bytes(bytes);
                break;
            }
            case 8:
            case 9: {
                const auto n = std::uint32_t(random() % 6);
                out.array(n);
                for (std::uint32_t i = 0; i < n; ++i) {
                    random_value(out, random, depth + 1);
                }
                break;
            }
            default: {
                const auto n = std::uint32_t(random() % 6);
                out.map(n);
                for (std::uint32_t i = 0; i < n; ++i) {
                    out.string("k" + std::to_string(i) + random_text(random));
                    random_value(out, random, depth + 1);
                }
                break;
            }
        }
    }

    // Equal as values: the same items in the same order (forms may differ), floats by their bits.
    bool same_value(bridge::ValueReader& a, bridge::ValueReader& b) {
        const auto x = a.next();
        const auto y = b.next();
        using Type = bridge::ValueReader::Type;
        if (x.type != y.type || x.count != y.count) {
            return false;
        }
        switch (x.type) {
            case Type::Nil:
                return true;
            case Type::Bool:
                return x.boolean == y.boolean;
            case Type::Int:
                return x.integer == y.integer;
            case Type::Unsigned:
                return x.unsigned_integer == y.unsigned_integer;
            case Type::Float:
                return std::bit_cast<std::uint64_t>(x.number) == std::bit_cast<std::uint64_t>(y.number);
            case Type::String:
                return x.text == y.text;
            case Type::Bytes:
                return std::ranges::equal(x.bytes, y.bytes);
            case Type::Array:
            case Type::Map:
                for (std::uint32_t i = 0; i < x.count * (x.type == Type::Map ? 2u : 1u); ++i) {
                    if (!same_value(a, b)) {
                        return false;
                    }
                }
                return true;
        }
        return false;
    }

    void echoes() {
        Page page;
        page.eval("weft.engine.on('echo', v => weft.engine.send('echo', v))");
        std::mt19937_64 random(0xec40);
        for (int round = 0; round < 300; ++round) {
            std::vector<Bytes> sent;
            for (int k = 0; k < 8; ++k) {
                bridge::ValueWriter out;
                random_value(out, random, 0);
                sent.push_back(out.take());
                require(page.channel->emit("echo", sent.back()) == ChannelStatus::Ok, "emit");
            }
            page.vm.deliver_engine();
            const auto back = page.vm.take_engine_messages();
            require(back.size() == sent.size(), "every value echoed");
            for (std::size_t k = 0; k < sent.size(); ++k) {
                bridge::ValueReader a(sent[k]);
                bridge::ValueReader b(back[k].value);
                require(same_value(a, b) && a.done() && b.done(),
                        "a value changed on its way through JS: " + json(sent[k]) + " came back as " +
                            json(back[k].value));
            }
        }
    }

    void events() {
        Page page;
        page.eval(R"JS(
          globalThis.log = [];
          globalThis.offHit = weft.engine.on('hit', v => log.push('on:' + JSON.stringify(v)));
          weft.engine.once('hit', v => log.push('once:' + v.n));
          weft.engine.onAny((name, v) => log.push('any:' + name));
          weft.engine.on('hit', () => { throw new Error('boom'); });
          weft.engine.on('hit', v => { log.push('after'); queueMicrotask(() => log.push('micro:' + v.n)); });
        )JS");
        page.channel->emit_json("hit", R"({"n":1})");
        page.channel->emit_json("hit", R"({"n":2})");
        page.channel->emit_json("other", "null");
        auto delivery = page.vm.deliver_engine();
        require(delivery.events == 3 && delivery.errors.size() == 2 && delivery.errors[0] == "Error: boom",
                "a handler's exception is recorded and the others run");
        require(page.eval("log.join()") ==
                    "on:{\"n\":1},once:1,after,any:hit,micro:1,on:{\"n\":2},after,any:hit,micro:2,any:other",
                "handlers in order, once once, a checkpoint after each event: " + page.eval("log.join()"));

        // A handler taken out by an earlier one in the same delivery is not called; one added is not either.
        page.eval(R"JS(
          log.length = 0;
          offHit();
          let offLate;
          weft.engine.on('x', () => { log.push('first'); offLate(); weft.engine.on('x', () => log.push('added')); });
          offLate = weft.engine.on('x', () => log.push('late'));
        )JS");
        page.channel->emit_json("x", "{}");
        page.channel->emit_json("hit", R"({"n":3})");
        delivery = page.vm.deliver_engine();
        require(page.eval("log.join()") == "first,any:x,after,any:hit,micro:3", "off during a delivery");
        require(delivery.errors.size() == 1, "a delivery reports its own exceptions");
        page.channel->emit_json("x", "{}");
        page.vm.deliver_engine();
        require(page.eval("log.slice(5).join()") == "first,added,any:x", "a handler added during a delivery");
        require(page.vm.deliver_engine().events == 0, "nothing waits");
        require(page.eval(R"JS(
          [() => weft.engine.on(1, () => {}), () => weft.engine.on('a', 1), () => weft.engine.onAny(null)]
            .map(f => { try { f(); return 'taken'; } catch (e) { return e.name; } }).join()
        )JS") == "TypeError,TypeError,TypeError",
                "handlers are functions of names");

        // A handler that sends.
        page.eval("weft.engine.on('echo', v => weft.engine.send('echoed', v))");
        page.channel->emit_json("echo", R"({"k":[1,2]})");
        page.vm.deliver_engine();
        const auto messages = page.vm.take_engine_messages();
        require(messages.size() == 1 && json(messages[0].value) == R"({"k":[1,2]})", "a handler sends");
    }

    void calls() {
        Page page;
        page.eval(R"JS(
          globalThis.results = [];
          weft.engine.call('market.load', {page: 2}).then(v => results.push('ok:' + JSON.stringify(v)));
          weft.engine.call('fail', 1).catch(e => results.push('rejected:' + e.message + ':' + JSON.stringify(e.value)));
          weft.engine.call('fail.text', 1).catch(e => results.push('rejected:' + e.message));
          weft.engine.call('fail.bare', 1).catch(e => results.push('rejected:' + e.message + ':' + e.value));
          weft.engine.call('slow', 0, {timeout: 50}).catch(e => results.push('timeout:' + e.message));
          weft.engine.call('raced', 0, {timeout: 10}).then(() => results.push('raced?'), e => results.push('raced:' + e.message));
          for (const [name, value, options] of [[5], ['x', () => 1], ['x', 1, {timeout: -1}], ['x', 1, {timeout: '9'}]])
            weft.engine.call(name, value, options).catch(e => results.push('refused:' + e.name));
        )JS");
        const auto messages = page.vm.take_engine_messages();
        require(messages.size() == 6 && messages[0].name == "market.load" && json(messages[0].value) == R"({"page":2})",
                "calls are messages with ids");
        for (std::size_t i = 1; i < messages.size(); ++i) {
            require(messages[i].call > messages[i - 1].call, "call ids grow");
        }
        require(page.channel->open_calls() == 6, "six open calls");
        page.channel->complete_json(messages[0].call, true, R"({"items":[1,2]})");
        page.channel->complete_json(messages[1].call, false, R"({"message":"no market","code":7})");
        page.channel->complete_json(messages[2].call, false, R"("not today")");
        page.channel->complete_json(messages[3].call, false, "7");
        // A completion the engine gave before the page's timeout, delivered after it: stale on the page's side.
        require(page.channel->complete_json(messages[5].call, true, "1") == ChannelStatus::Ok, "complete in time");
        page.vm.pump_tasks(20);
        auto delivery = page.vm.deliver_engine();
        require(delivery.completions == 4 && delivery.stale == 1, "completions and a stale one");
        page.vm.pump_tasks(60);
        require(page.channel->complete_json(messages[4].call, true, "1") == ChannelStatus::Stale,
                "a completion after the timeout is stale");
        require(page.channel->open_calls() == 0, "no call left open");
        require(page.eval("results.join('|')") ==
                    "refused:TypeError|refused:InternalError|refused:TypeError|refused:TypeError|"
                    "raced:WEFT_ENGINE_TIMEOUT: raced|ok:{\"items\":[1,2]}|"
                    "rejected:no market:{\"message\":\"no market\",\"code\":7}|rejected:not today|"
                    "rejected:weft.engine.call fail.bare failed:7|timeout:WEFT_ENGINE_TIMEOUT: slow",
                "promises settle: " + page.eval("results.join('|')"));

        // A reset closes the calls; the engine's later completions are stale.
        page.eval("weft.engine.call('after.reset', 1)");
        const auto open = page.vm.take_engine_messages();
        page.channel->emit_json("dropped", "1");
        page.eval("weft.engine.send('lost', 1)");
        page.vm.reset_document();
        page.install();
        require(page.channel->complete_json(open[0].call, true, "1") == ChannelStatus::Stale,
                "a completion after a reset is stale");
        require(page.vm.deliver_engine().events == 0, "a reset drops what waited");
        page.eval("weft.engine.call('next', 1)");
        const auto after = page.vm.take_engine_messages();
        require(after.size() == 1 && after[0].name == "next" && after[0].call > open[0].call,
                "a reset drops the page's messages; ids go on");
    }

    void functions() {
        Page page;
        bool reentered = false;
        page.vm.install_engine_function("loc.get", [](std::span<const std::uint8_t> argument) {
            bridge::ValueWriter out;
            out.string(json(argument) == R"({"key":"ui.ok"})" ? "OK" : "?");
            return out.take();
        });
        page.vm.install_engine_function("void", [](std::span<const std::uint8_t>) {
            return Bytes{};
        });
        page.vm.install_engine_function("throws", [](std::span<const std::uint8_t>) -> Bytes {
            throw std::runtime_error("no such key");
        });
        page.vm.install_engine_function("bad", [](std::span<const std::uint8_t>) {
            return Bytes{0xc1};
        });
        page.vm.install_engine_function("reenter", [&](std::span<const std::uint8_t> argument) -> Bytes {
            const auto what = json(argument);
            if (what == R"("eval")") {
                page.vm.eval("1");
            } else if (what == R"("take")") {
                page.vm.take_engine_messages();
            } else if (what == R"("deliver")") {
                page.vm.deliver_engine();
            } else {
                page.vm.stats();
            }
            reentered = true;
            return {};
        });
        page.vm.install_engine_function("big", [](std::span<const std::uint8_t>) {
            return bridge::value_from_json(R"({"n":18446744073709551615,"list":[1,"two",null]})");
        });
        const auto outcome = page.eval(R"JS(
          const out = [weft.engine.invoke('loc.get', {key: 'ui.ok'}), String(weft.engine.invoke('void'))];
          for (const [name, value] of [['throws'], ['bad'], ['nope'], ['reenter', 'eval'], ['reenter', 'take'],
              ['reenter', 'deliver'], ['reenter', 'stats'], ['loc.get', () => 1]]) {
            try { weft.engine.invoke(name, value); out.push('returned'); } catch (e) { out.push(e.message); }
          }
          const big = weft.engine.invoke('big');
          out.push(typeof big.n, big.list.join('/'));
          out.join('|')
        )JS");
        require(outcome == "OK|undefined|no such key|WEFT_ENGINE: bad returned bridge value: a type outside the subset|"
                           "WEFT_ENGINE: no function nope|"
                           "WEFT_ENGINE_REENTRY: a sync engine function must not enter JS or DOM|"
                           "WEFT_ENGINE_REENTRY: a sync engine function must not enter JS or DOM|"
                           "WEFT_ENGINE_REENTRY: a sync engine function must not enter JS or DOM|"
                           "WEFT_ENGINE_REENTRY: a sync engine function must not enter JS or DOM|"
                           "WEFT_ENGINE_VALUE: a function cannot cross|bigint|1/two/",
                "sync functions: " + outcome);
        require(!reentered && page.eval("1 + 1") == "2", "a refused reentry leaves the View working");
        require(thrown([&] {
                    page.vm.install_engine_function("", {});
                }).find("WEFT_ENGINE_NAME") == 0,
                "a function needs a name");
        // Functions and the channel are the View's: they outlive the page.
        page.vm.reset_document();
        page.install();
        require(page.eval("weft.engine.invoke('loc.get', {key: 'ui.ok'})") == "OK", "a function after a reset");
    }

    void models() {
        Page page;
        page.eval(R"JS(
          globalThis.player = weft.engine.model('player');
          globalThis.seen = [];
          globalThis.unsubscribe = player.subscribe((value, changed) =>
            seen.push(JSON.stringify(value) + '|' + JSON.stringify(changed)));
          player.subscribe(() => { throw new Error('subscriber'); });
          globalThis.first = player.value;
          weft.engine.on('turn', () => seen.push('turn sees hp ' + player.value.hp));
        )JS");
        require(page.eval("String(first) + ',' + (weft.engine.model('player') === player) + ',' + player.name") ==
                    "undefined,true,player",
                "a model before its first batch");
        page.channel->emit_json("turn", "{}");
        page.channel->set_model_json("player", R"({"hp":10,"name":"Ann"})");
        page.channel->set_model_json("player", R"({"hp":9})");
        auto delivery = page.vm.deliver_engine();
        require(delivery.models == 1 && delivery.events == 1 &&
                    delivery.errors == std::vector<std::string>{"Error: subscriber"},
                "one batch for the delivery");
        require(page.eval("seen.join(';')") == R"({"hp":9,"name":"Ann"}|{"hp":9,"name":"Ann"};turn sees hp 9)",
                "fields merge into one batch before the events");
        page.eval("globalThis.second = player.value");
        page.channel->set_model_json("player", R"({"hp":8})");
        page.vm.deliver_engine();
        require(page.eval("seen[2] + ',' + (player.value !== second) + ',' + Object.isFrozen(player.value) + ',' + "
                          "Object.isFrozen(second) + ',' + second.hp") ==
                    R"({"hp":8,"name":"Ann"}|{"hp":8},true,true,true,9)",
                "each batch a new frozen value; a batch passes what changed");
        // A subscriber taken out by an earlier one during a batch is not called.
        page.eval(R"JS(
          globalThis.order = [];
          const sub = weft.engine.model('sub');
          let offLater;
          sub.subscribe(() => { order.push('a'); offLater(); });
          offLater = sub.subscribe(() => order.push('b'));
        )JS");
        page.channel->set_model_json("sub", R"({"x":1})");
        page.vm.deliver_engine();
        require(page.eval("order.join()") == "a", "unsubscribed during a batch");
        page.eval("unsubscribe(); weft.engine.model('enemy')");
        page.channel->set_model_json("player", R"({"hp":7})");
        page.channel->set_model_json("other", R"({"a":1})");
        page.vm.deliver_engine();
        require(page.eval("seen.length + ',' + player.value.hp + ',' + weft.engine.model('other').value.a + ',' + "
                          "weft.engine.model('enemy').value") == "3,7,1,undefined",
                "unsubscribed; a model the page has not asked for yet");
    }

    void without_channel() {
        Page page(false);
        require(page.vm.deliver_engine().events == 0, "nothing to deliver without a channel");
        require(page.eval(R"JS(
          let message = '';
          try { weft.engine.send('a'); } catch (e) { message = e.message; }
          weft.engine.call('a').catch(e => { message += '|' + e.message; });
          message
        )JS") == "WEFT_ENGINE: no engine bridge",
                "a send without a channel");
        require(page.eval("message") == "WEFT_ENGINE: no engine bridge|WEFT_ENGINE: no engine bridge",
                "a call without a channel");
        page.vm.install_engine(page.channel);
        page.eval("weft.engine.send('a')");
        require(page.vm.take_engine_messages().size() == 1, "a channel installed later");
        require(thrown([&] {
                    weft::install_script_platform(page.vm, ScriptPlatformModule::Engine);
                }).find("WEFT_ENGINE_INSTALL") != std::string::npos,
                "the module installs once");
    }

    // CRPG's ue-bridge.ts (tests/fixtures/crpg-gallery/UI/src/lib/ue-bridge.ts) as esbuild compiles it, without the
    // WebSocket and CEF fallbacks the direct bridge skips, and MVVMTest.tsx's __vm_update.
    constexpr std::string_view crpg_page = R"JS(
      var registeredHandlers = new Map();
      function sendToUE(name2, data2) {
        if (typeof window.ue?.sendEvent === "function") {
          window.ue.sendEvent(name2, data2 ?? {});
          return;
        }
        throw new Error("no direct bridge");
      }
      function onUECall(fnName, handler) {
        registeredHandlers.set(fnName, handler);
        if (typeof window.ue !== "object") {
          window.ue = { interface: {} };
        }
        if (typeof window.ue.interface !== "object") {
          window.ue.interface = { broadcast: () => {} };
        }
        window.ue.interface[fnName] = handler;
        return () => {
          registeredHandlers.delete(fnName);
          if (window.ue?.interface?.[fnName] === handler) {
            delete window.ue.interface[fnName];
          }
        };
      }
      window.__ue_dispatch = (fnName, data2) => {
        const handler = registeredHandlers.get(fnName);
        if (handler) {
          handler(data2);
        } else {
          console.warn(`[UE Bridge] No handler for UE→JS call "${fnName}"`);
        }
      };
      globalThis.got = [];
      onUECall("pushUI", (d) => got.push("pushUI:" + d.page));
      onUECall("loadMarket", (d) => got.push("market:" + d.items.length + ":" + d.items[0].name + ":" + d.gold));
      const offLocale = onUECall("setLocale", (d) => {
        got.push("locale:" + d.locale);
        sendToUE("localeApplied", { locale: d.locale });
      });
      window.__vm_update = (delta) => {
        for (const [k, v] of Object.entries(delta)) got.push("vm:" + k + "=" + v);
      };
      sendToUE("ui_ready");
    )JS";

    void crpg() {
        Page page;
        {
            Page bare(false);
            bare.eval("window.ue = {interface: {existing: 1}}");
            weft::install_script_platform(bare.vm, ScriptPlatformModule::CrpgBridge);
            require(bare.eval("window.ue.interface.existing + ',' + typeof window.ue.sendEvent") == "1,function",
                    "the CRPG bridge keeps the page's window.ue");
        }
        weft::install_script_platform(page.vm, ScriptPlatformModule::CrpgBridge);
        require(thrown([&] {
                    weft::install_script_platform(page.vm, ScriptPlatformModule::CrpgBridge);
                }).find("WEFT_CRPG_BRIDGE_INSTALL") != std::string::npos,
                "the CRPG bridge installs once");
        page.eval(crpg_page);
        auto messages = page.vm.take_engine_messages();
        require(messages.size() == 1 && messages[0].name == "ui_ready" && json(messages[0].value) == "{}",
                "ue.sendEvent sends");
        // As CRPG's host: no data is {}, a name that is not a string is ignored, and JSON.stringify's rules when a
        // value cannot cross as it is.
        page.eval(R"JS(
          window.ue.sendEvent('bare');
          window.ue.sendEvent('nul', null);
          window.ue.sendEvent(5, {});
          window.ue.sendEvent('json', {a: 1, f() {}, n: NaN, list: [() => 1, Infinity], when: new Date(0)});
          window.ue.sendEvent('fn', () => 1);
        )JS");
        messages = page.vm.take_engine_messages();
        require(messages.size() == 4 && json(messages[0].value) == "{}" && json(messages[1].value) == "{}" &&
                    messages[2].name == "json" &&
                    json(messages[2].value) ==
                        R"({"a":1,"n":null,"list":[null,null],"when":"1970-01-01T00:00:00.000Z"})" &&
                    json(messages[3].value) == "{}",
                "ue.sendEvent as CRPG's host sends");
        require(page.eval(R"JS(
          const cyclic = {}; cyclic.self = cyclic;
          try { window.ue.sendEvent('cyclic', cyclic); 'sent'; } catch (e) { e.name; }
        )JS") == "TypeError",
                "a cycle throws, as JSON.stringify does");
        require(page.eval("typeof window.ue.sendEvent + ',' + typeof window.ue.interface.pushUI") ==
                    "function,function",
                "the page adds to window.ue");
        // The engine's CallUI samples: what WebUISubsystem, the market cheat and the locale setter send.
        page.channel->emit_json("pushUI", R"({"page":"market"})");
        page.channel->emit_json("loadMarket",
                                R"({"items":[{"name":"Sword","price":12},{"name":"Shield","price":9}],"gold":40})");
        page.channel->emit_json("setLocale", R"({"locale":"zh-CN"})");
        page.channel->emit_json("noHandler", "{}");
        page.channel->set_model_json("vm", R"({"SimTick":5,"NpcCount":12})");
        page.channel->set_model_json("other", R"({"ignored":1})");
        const auto delivery = page.vm.deliver_engine();
        require(delivery.events == 4 && delivery.models == 2 && delivery.errors.empty(), "the samples delivered");
        require(page.eval("got.join()") == "vm:NpcCount=12,vm:SimTick=5,pushUI:market,market:2:Sword:40,locale:zh-CN",
                "the page's handlers: " + page.eval("got.join()"));
        messages = page.vm.take_engine_messages();
        require(messages.size() == 1 && messages[0].name == "localeApplied" &&
                    json(messages[0].value) == R"({"locale":"zh-CN"})",
                "a handler answers through ue.sendEvent");
        page.eval("got.length = 0");
        page.channel->set_model_json("vm", R"({"SimTick":6})");
        page.vm.deliver_engine();
        require(page.eval("got.join()") == "vm:SimTick=6", "__vm_update gets what changed");
        // A page without __vm_update, or after it dropped a handler, loses nothing else.
        page.eval("offLocale(); delete window.__vm_update; got.length = 0");
        page.channel->emit_json("setLocale", R"({"locale":"en"})");
        page.channel->set_model_json("vm", R"({"SimTick":6})");
        page.channel->emit_json("pushUI", R"({"page":"inventory"})");
        require(page.vm.deliver_engine().errors.empty() && page.eval("got.join()") == "pushUI:inventory",
                "missing handlers are skipped");
    }
} // namespace

int main() {
    try {
        values_out();
        values_in();
        echoes();
        events();
        calls();
        functions();
        models();
        without_channel();
        crpg();
        std::cout << "engine bridge tests passed\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "engine bridge tests failed: " << e.what() << '\n';
        return 1;
    }
}
