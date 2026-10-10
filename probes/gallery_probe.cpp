#include "caseelephant/script_probe.hpp"
#include "caseelephant/constants/script.hpp"
#include "gallery_constants.hpp"
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <map>

namespace {
    std::string read(const std::filesystem::path& path) {
        std::ifstream input(path, std::ios::binary);
        if (!input) {
            throw std::runtime_error("Cannot read: " + path.string());
        }
        std::ostringstream out;
        out << input.rdbuf();
        return out.str();
    }

    std::string quote(const std::string& text) {
        std::string out = "\"";
        constexpr char hex[] = "0123456789abcdef";
        for (unsigned char c : text) {
            if (c == '"' || c == '\\') {
                out += '\\';
                out += c;
            } else if (c < 32) {
                out += "\\u00";
                out += hex[c >> 4];
                out += hex[c & 15];
            } else {
                out += c;
            }
        }
        return out + '"';
    }

    void write(const std::filesystem::path& path, const std::string& text) {
        std::ofstream out(path, std::ios::binary);
        if (!out || !(out << text << '\n')) {
            throw std::runtime_error("Cannot write: " + path.string());
        }
    }

    void install(caseelephant::ScriptDomProbe& vm, const std::filesystem::path& source,
                 const std::filesystem::path& bundles) {
        vm.eval(read(source / "probes/web_host_bootstrap.js"), "web_host_bootstrap.js");
        vm.eval(read(source / "probes/animation_frame_bootstrap.js"), "animation_frame_bootstrap.js");
        vm.eval(read(bundles / "platform.js"), "gallery_platform.js");
    }

    void pump(caseelephant::ScriptDomProbe& vm, unsigned frame) {
        // Exactly representable fractional host time; timers and rAF share the clock.
        const std::string time =
            std::to_string(frame * caseelephant::probe_constants::gallery::native_observation_frame_ms) + ".25";
        for (unsigned turn = 0; turn < caseelephant::constants::script::max_task_pump_turns; ++turn) {
            if (vm.eval("__ceHost.pump(" + time + ")") == "0") {
                return;
            }
        }
        throw std::runtime_error("CE_GALLERY_TASK_LIMIT: ready timer turns");
    }
} // namespace

int main(int argc, char** argv) {
    try {
        if (argc != 2) {
            throw std::runtime_error("Usage: ce_gallery_probe <output-directory>");
        }
        const std::filesystem::path source = CE_SOURCE_DIR;
        const std::filesystem::path bundles = source / "examples/crpg-gallery/native";
        const std::filesystem::path out = argv[1];
        std::filesystem::create_directories(out);
        caseelephant::ScriptProbeConfig config;
        config.max_stack_bytes = caseelephant::constants::script::probe_vm_stack_bytes;
        config.execution_budget_ms = caseelephant::probe_constants::gallery::host_vm_execution_budget_ms;
        caseelephant::ScriptDomProbe registry(config);
        registry.eval(read(bundles / "case-list.js"));
        const unsigned count = static_cast<unsigned>(std::stoul(registry.eval("__ceGalleryCases.length")));
        std::map<std::string, std::string> scripts;
        std::ostringstream report;
        report << "{\"format\":\"gallery-native-execution-v1\",\"case_count\":" << count
               << ",\"rendering_verified\":false,\"interaction_verified\":false,\"performance_verified\":false,"
                  "\"conditions\":{\"host_opportunities\":12,\"time_step_ms\":16,\"time_fraction_ms\":0.25,"
                  "\"css_loaded\":false,\"input_dispatched\":false,\"mode\":\"isolated original cases\"},\"cases\":[";
        unsigned clean = 0;
        for (unsigned i = 0; i < count; ++i) {
            const std::string lookup = "__ceGalleryCases[" + std::to_string(i) + "]";
            const auto record = registry.eval("JSON.stringify(" + lookup + ")");
            const auto group = registry.eval(lookup + ".group");
            if (!scripts.contains(group)) {
                scripts[group] = read(bundles / (group + ".js"));
            }
            caseelephant::ScriptDomProbe vm(config);
            const auto baseline = vm.stats();
            std::string phase = "module";
            std::string failure;
            std::string state = "null";
            std::string raf_errors = "[]";
            std::string lifecycle_failure;
            std::string first_dom = "null";
            std::string last_dom = "null";
            std::size_t callbacks = 0;
            std::size_t callback_errors = 0;
            std::size_t nodes = 0;
            try {
                install(vm, source, bundles);
                vm.eval(scripts.at(group), group + ".js");
                phase = "mount";
                vm.eval("__ceGallery.mount(" + quote(group) + "," + registry.eval(lookup + ".index") + ")");
                phase = "schedule";
                for (unsigned frame = 0; frame < caseelephant::probe_constants::gallery::observation_frames; ++frame) {
                    pump(vm, frame);
                    auto result =
                        vm.animation_frame(frame * caseelephant::probe_constants::gallery::native_observation_frame_ms +
                                           caseelephant::probe_constants::gallery::native_observation_time_fraction_ms);
                    callbacks += result.callbacks;
                    callback_errors += result.exceptions;
                    if (result.exceptions) {
                        raf_errors = result.exception_messages_json;
                    }
                    pump(vm, frame);
                    if (group == "SPRING_TESTS" &&
                        (frame == 0 || frame == caseelephant::probe_constants::gallery::observation_frames - 1)) {
                        const auto dom = vm.eval(
                            "JSON.stringify(Array.from(document.getElementById('root').querySelectorAll('*'),n=>({tag:n.localName,style:n.getAttribute('style'),text:n.textContent})))");
                        if (frame == 0) {
                            first_dom = dom;
                        } else {
                            last_dom = dom;
                        }
                    }
                }
                state = vm.eval("JSON.stringify(__ceGallery.state())");
                nodes = vm.snapshot_subtree("#root").children.size();
                phase = "completed";
            } catch (const std::exception& e) {
                failure = e.what();
            }
            try {
                if (failure.empty()) {
                    vm.eval("__ceGallery.unmount()");
                }
                vm.collect_garbage();
            } catch (const std::exception& e) {
                lifecycle_failure = e.what();
            }
            const auto before_reset = vm.stats();
            vm.reset_document();
            vm.collect_garbage();
            const auto after_reset = vm.stats();
            if (after_reset.live_nodes != baseline.live_nodes || after_reset.native_roots != baseline.native_roots) {
                throw std::runtime_error("Gallery reset leaked native nodes/roots");
            }
            const bool no_errors = failure.empty() && callback_errors == 0 && lifecycle_failure.empty() &&
                                   state.find("\"errors\":[]") != std::string::npos && nodes > 0;
            clean += no_errors;
            if (i) {
                report << ',';
            }
            report << "{\"registration\":" << record
                   << ",\"execution\":" << quote(no_errors ? "no_error_observed" : "blocked")
                   << ",\"phase\":" << quote(phase) << ",\"failure\":" << quote(failure) << ",\"state\":" << state
                   << ",\"raf_callbacks\":" << callbacks << ",\"raf_exception_count\":" << callback_errors
                   << ",\"last_raf_errors\":" << raf_errors << ",\"unmount_failure\":" << quote(lifecycle_failure)
                   << ",\"root_children\":" << nodes << ",\"nodes_before_reset\":" << before_reset.live_nodes
                   << ",\"peak_vm_bytes\":" << before_reset.peak_vm_allocated_bytes
                   << ",\"reset_nodes\":" << after_reset.live_nodes << ",\"reset_roots\":" << after_reset.native_roots
                   << ",\"first_animation_dom\":" << first_dom << ",\"last_animation_dom\":" << last_dom
                   << ",\"pixels\":\"not_run\",\"interaction\":\"not_run\",\"performance\":\"not_run\"}";
        }
        std::string full_error;
        std::string full_state = "null";
        std::string full_phase = "module";
        {
            caseelephant::ScriptDomProbe vm(config);
            try {
                install(vm, source, bundles);
                vm.eval(read(bundles / "all.js"), "gallery_all.js");
                full_phase = "mount";
                vm.eval("__ceGallery.original()");
                full_phase = "schedule";
                for (unsigned frame = 0; frame < caseelephant::probe_constants::gallery::observation_frames; ++frame) {
                    pump(vm, frame);
                    vm.animation_frame(frame * caseelephant::probe_constants::gallery::native_observation_frame_ms +
                                       caseelephant::probe_constants::gallery::native_observation_time_fraction_ms);
                    pump(vm, frame);
                }
                full_state = vm.eval("JSON.stringify(__ceGallery.state())");
                full_phase = "completed";
            } catch (const std::exception& e) {
                full_error = e.what();
            }
        }
        report << "],\"full_gallery\":{\"phase\":" << quote(full_phase) << ",\"failure\":" << quote(full_error)
               << ",\"state\":" << full_state << "},\"no_error_observed\":" << clean << '}';
        write(out / "native-execution.json", report.str());
        std::cout << "Measured " << count << " original cases: " << clean << " without observed execution error; "
                  << count - clean << " blocked. Rendering/interaction/performance not verified.\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "FAIL: " << e.what() << '\n';
        return 1;
    }
}
