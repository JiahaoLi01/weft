#include "bridge/bridge_value.hpp"
#include "bridge/engine_channel.hpp"
#include "constants/script.hpp"
#include "script/script_dom.hpp"
#include "script/script_platform.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

// What an engine's events cost the page's owner (docs/engine-communication.md, 13.2). CRPG Framework's browser host
// splices each event's JSON into a script and evaluates it: window.__ue_dispatch(fn, json). The bridge queues the
// event's value, JSON converted where the engine gives it or MessagePack the engine writes, and the owner delivers
// what waits as JS values once a frame. The page is the same for all three: CRPG's ue-bridge dispatcher over the CRPG
// bridge module, with handlers that read every value. Payloads: a health change (about 10 bytes), and markets of about
// 1 KB and 10 KB (CRPG's loadMarket). Rates: one event a frame, and 17 a frame (1,000 a second at 60 frames a second).
// Timed per frame: the engine's side (writing the payload, and the script text, the JSON conversion or nothing), and
// the owner's (the evaluations, or the delivery). The page's checksums must agree between the three. Also timed alone,
// per payload: JSON to MessagePack and back. And a call's round trip: the page calls during a frame, the host takes the
// messages after it and completes the call at once; the frames until the promise settles.
namespace {
    namespace bridge = weft::bridge;
    using Clock = std::chrono::steady_clock;

    enum class Way {
        Eval,
        Json,
        Pack
    };

    constexpr std::string_view way_names[]{"eval", "bridge-json", "bridge-msgpack"};

    // A payload: its event, and the market's items (0 for the health change).
    struct Payload {
        std::string name;
        std::string event;
        unsigned items{};
    };

    // Event i's payload as JSON, as CRPG writes it (FJsonSerializer's condensed form).
    std::string json_of(const Payload& p, std::uint64_t i) {
        if (!p.items) {
            return "{\"v\":" + std::to_string(i % 1000) + "}";
        }
        std::string out = "{\"gold\":" + std::to_string(i) + ",\"page\":2,\"items\":[";
        for (unsigned k = 0; k < p.items; ++k) {
            if (k) {
                out += ',';
            }
            out += "{\"id\":\"item_" + std::to_string(k) + "\",\"name\":\"Longsword of the North " + std::to_string(k) +
                   "\",\"price\":" + std::to_string(100 + k * 7) + ",\"count\":" + std::to_string(k % 5 + 1) +
                   ",\"rarity\":\"rare\",\"icon\":\"/Game/UI/Icons/Weapons/T_Sword_" + std::to_string(k % 12) + "\"}";
        }
        return out + "]}";
    }

    // The same payload as the engine writes it in MessagePack.
    std::vector<std::uint8_t> pack_of(const Payload& p, std::uint64_t i) {
        bridge::ValueWriter out;
        if (!p.items) {
            out.map(1);
            out.string("v");
            out.integer(std::int64_t(i % 1000));
            return out.take();
        }
        out.map(3);
        out.string("gold");
        out.integer(std::int64_t(i));
        out.string("page");
        out.integer(2);
        out.string("items");
        out.array(p.items);
        std::string text;
        for (unsigned k = 0; k < p.items; ++k) {
            out.map(6);
            out.string("id");
            text = "item_" + std::to_string(k);
            out.string(text);
            out.string("name");
            text = "Longsword of the North " + std::to_string(k);
            out.string(text);
            out.string("price");
            out.integer(100 + k * 7);
            out.string("count");
            out.integer(k % 5 + 1);
            out.string("rarity");
            out.string("rare");
            out.string("icon");
            text = "/Game/UI/Icons/Weapons/T_Sword_" + std::to_string(k % 12);
            out.string(text);
        }
        return out.take();
    }

    // CRPG's ue-bridge dispatcher (tests/fixtures/crpg-gallery/UI/src/lib/ue-bridge.ts, the parts its direct bridge
    // uses) and handlers that read every value into a checksum.
    constexpr std::string_view page_source = R"JS(
      var registeredHandlers = new Map();
      window.__ue_dispatch = (fnName, data2) => {
        const handler = registeredHandlers.get(fnName);
        if (handler) handler(data2);
      };
      globalThis.checksum = 0;
      registeredHandlers.set("setHealth", (d) => { checksum = (checksum + d.v) % 1000000007; });
      registeredHandlers.set("loadMarket", (d) => {
        let s = d.gold + d.page;
        for (const item of d.items) s += item.price * item.count + item.name.length + item.id.length +
          item.rarity.length + item.icon.length;
        checksum = (checksum + s) % 1000000007;
      });
    )JS";

    struct Page {
        weft::ScriptDom vm{weft::ScriptConfig{weft::constants::script::probe_vm_stack_bytes}};
        std::shared_ptr<bridge::EngineChannel> channel = std::make_shared<bridge::EngineChannel>();

        Page() {
            vm.install_engine(channel);
            weft::install_script_platform(vm, weft::ScriptPlatformModule::WebHost);
            weft::install_script_platform(vm, weft::ScriptPlatformModule::Engine);
            weft::install_script_platform(vm, weft::ScriptPlatformModule::CrpgBridge);
            vm.eval(page_source);
        }
    };

    struct Case {
        const Payload* payload{};
        unsigned per_frame{};
        Way way{};
        std::vector<double> engine;
        std::vector<double> owner;
        std::string checksum;
    };

    double percentile(std::vector<double> v, double p) {
        std::sort(v.begin(), v.end());
        return v.empty() ? 0 : v[std::min(v.size() - 1, std::size_t(std::ceil(p * double(v.size()))) - 1)];
    }

    std::string stats(const std::vector<double>& v) {
        std::ostringstream out;
        out << std::fixed << std::setprecision(2) << "{\"p50\":" << percentile(v, .5)
            << ",\"p99\":" << percentile(v, .99) << ",\"max\":" << percentile(v, 1) << '}';
        return out.str();
    }

    double since(Clock::time_point start) {
        return std::chrono::duration<double, std::micro>(Clock::now() - start).count();
    }

    // Warmup frames, then sampled frames, each per_frame events of the payload.
    void run(Case& c, unsigned warmup, unsigned samples) {
        Page page;
        std::uint64_t event{};
        for (unsigned frame = 0; frame < warmup + samples; ++frame) {
            double engine{};
            double owner{};
            for (unsigned k = 0; k < c.per_frame; ++k, ++event) {
                const auto start = Clock::now();
                if (c.way == Way::Pack) {
                    const auto value = pack_of(*c.payload, event);
                    if (page.channel->emit(c.payload->event, value) != bridge::ChannelStatus::Ok) {
                        throw std::runtime_error("emit");
                    }
                    engine += since(start);
                    continue;
                }
                const auto json = json_of(*c.payload, event);
                if (c.way == Way::Json) {
                    if (page.channel->emit_json(c.payload->event, json) != bridge::ChannelStatus::Ok) {
                        throw std::runtime_error("emit_json");
                    }
                    engine += since(start);
                    continue;
                }
                // CRPG's WebWidgetInstance::CallUI: the script text, then its evaluation on the owner.
                const auto script = "(function(){if(window.__ue_dispatch){window.__ue_dispatch('" + c.payload->event +
                                    "'," + json + ");}})()";
                engine += since(start);
                const auto evaluated = Clock::now();
                page.vm.eval(script, "CallUI");
                owner += since(evaluated);
            }
            if (c.way != Way::Eval) {
                const auto start = Clock::now();
                const auto delivery = page.vm.deliver_engine();
                owner += since(start);
                if (delivery.events != c.per_frame || !delivery.errors.empty()) {
                    throw std::runtime_error("a delivery lost events");
                }
            }
            if (frame >= warmup) {
                c.engine.push_back(engine);
                c.owner.push_back(owner);
            }
        }
        c.checksum = page.vm.eval("String(checksum)");
    }

    // The conversions of a payload alone: JSON to MessagePack (where the engine gives JSON) and back (diagnostics).
    std::string conversions(const Payload& p, unsigned samples) {
        const auto json = json_of(p, 7);
        std::vector<double> parse;
        std::vector<double> print;
        for (unsigned i = 0; i < samples; ++i) {
            auto start = Clock::now();
            const auto value = bridge::value_from_json(json);
            parse.push_back(since(start));
            start = Clock::now();
            const auto text = bridge::value_to_json(value);
            print.push_back(since(start));
            if (text != json) {
                throw std::runtime_error("JSON changed on its round trip: " + p.name);
            }
        }
        return "{\"payload\":\"" + p.name + "\",\"json_bytes\":" + std::to_string(json.size()) +
               ",\"from_json_us\":" + stats(parse) + ",\"to_json_us\":" + stats(print) + "}";
    }

    // Frames from a call to its settled promise when the host completes each call as soon as it takes it.
    unsigned round_trip() {
        Page page;
        page.vm.eval("globalThis.settled = 0; weft.engine.call('market.load', {page: 2}).then(v => settled = v.ok);");
        for (unsigned frame = 1; frame <= 8; ++frame) {
            for (const auto& m : page.vm.take_engine_messages()) {
                page.channel->complete_json(m.call, true, R"({"ok":1})");
            }
            page.vm.deliver_engine();
            if (page.vm.eval("String(settled)") == "1") {
                return frame;
            }
        }
        throw std::runtime_error("a call did not settle");
    }
} // namespace

int main(int argc, char** argv) {
    try {
        if (argc != 3 || (std::string_view(argv[1]) != "--quick" && std::string_view(argv[1]) != "--full")) {
            std::cerr << "usage: weft_bridge_benchmark --quick|--full <folder>\n";
            return 2;
        }
        const bool full = std::string_view(argv[1]) == "--full";
        const unsigned warmup = full ? 60 : 2;
        const unsigned samples = full ? 600 : 5;
        const std::vector<Payload> payloads{
            {"health", "setHealth", 0}, {"market-1k", "loadMarket", 8}, {"market-10k", "loadMarket", 77}};
        std::vector<Case> cases;
        for (const auto& payload : payloads) {
            for (const unsigned per_frame : {1u, 17u}) {
                for (const auto way : {Way::Eval, Way::Json, Way::Pack}) {
                    cases.push_back({&payload, per_frame, way, {}, {}, {}});
                }
            }
        }
        std::ostringstream out;
        out << "{\"format\":\"weft-bridge-benchmark-v1\",\"build\":"
#ifdef NDEBUG
            << "\"Release\""
#else
            << "\"Debug\""
#endif
            << ",\"warmup_frames\":" << warmup << ",\"sample_frames\":" << samples << ",\"cases\":[";
        for (std::size_t i = 0; i < cases.size(); ++i) {
            auto& c = cases[i];
            run(c, warmup, samples);
            // The three ways of a payload and rate hand the page the same values.
            if (i % 3 && c.checksum != cases[i - i % 3].checksum) {
                throw std::runtime_error("checksums differ for " + c.payload->name);
            }
            const auto json_bytes = json_of(*c.payload, 0).size();
            const auto pack_bytes = pack_of(*c.payload, 0).size();
            out << (i ? "," : "") << "{\"payload\":\"" << c.payload->name << "\",\"json_bytes\":" << json_bytes
                << ",\"msgpack_bytes\":" << pack_bytes << ",\"events_per_frame\":" << c.per_frame << ",\"way\":\""
                << way_names[int(c.way)] << "\",\"checksum\":\"" << c.checksum << "\",\"engine_us\":" << stats(c.engine)
                << ",\"owner_us\":" << stats(c.owner) << '}';
            std::cout << std::left << std::setw(11) << c.payload->name << std::setw(4) << c.per_frame << std::setw(16)
                      << way_names[int(c.way)] << std::fixed << std::setprecision(2) << "engine p50 "
                      << percentile(c.engine, .5) << " us, owner p50 / p99 / max " << percentile(c.owner, .5) << " / "
                      << percentile(c.owner, .99) << " / " << percentile(c.owner, 1) << " us\n";
        }
        out << "],\"conversions\":[";
        for (std::size_t i = 0; i < payloads.size(); ++i) {
            out << (i ? "," : "") << conversions(payloads[i], full ? 2000 : 20);
        }
        const auto frames = round_trip();
        out << "],\"call_round_trip_frames\":" << frames << "}";
        std::cout << "call round trip: " << frames << " frame\n";
        const std::filesystem::path folder = argv[2];
        std::filesystem::create_directories(folder);
        std::ofstream(folder / "bridge.json", std::ios::binary) << out.str();
        std::cout << "PASS bridge benchmark: " << cases.size() << " cases, " << samples << " frames each\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "bridge benchmark failed: " << e.what() << '\n';
        return 1;
    }
}
