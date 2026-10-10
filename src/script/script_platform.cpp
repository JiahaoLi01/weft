#include "caseelephant/script_platform.hpp"
#include "caseelephant/script_probe.hpp"

#include <cstddef>

namespace caseelephant {
    namespace {
        // Generated from src/script/js/*.js by cmake/embed_bytes.cmake.
        constexpr unsigned char web_host[] = {
#include "caseelephant/script_js/web_host_bootstrap.js.inc"
        };
        constexpr unsigned char animation_frame[] = {
#include "caseelephant/script_js/animation_frame_bootstrap.js.inc"
        };
        constexpr unsigned char scene_platform[] = {
#include "caseelephant/script_js/scene_platform_bootstrap.js.inc"
        };

        // UTF-8 bytes above 0x7f cannot initialize a signed char, so the tables are unsigned.
        template <std::size_t N>
        std::string_view text(const unsigned char (&bytes)[N]) noexcept {
            return {reinterpret_cast<const char*>(bytes), N - 1};
        }
    } // namespace

    std::string_view script_platform_source(ScriptPlatformModule module) noexcept {
        switch (module) {
            case ScriptPlatformModule::WebHost:
                return text(web_host);
            case ScriptPlatformModule::AnimationFrame:
                return text(animation_frame);
            case ScriptPlatformModule::ScenePlatform:
                return text(scene_platform);
        }
        return {};
    }

    std::string_view script_platform_file(ScriptPlatformModule module) noexcept {
        switch (module) {
            case ScriptPlatformModule::WebHost:
                return "web_host_bootstrap.js";
            case ScriptPlatformModule::AnimationFrame:
                return "animation_frame_bootstrap.js";
            case ScriptPlatformModule::ScenePlatform:
                return "scene_platform_bootstrap.js";
        }
        return {};
    }

    std::string install_script_platform(ScriptDomProbe& vm, ScriptPlatformModule module) {
        return vm.eval(script_platform_source(module), script_platform_file(module));
    }
} // namespace caseelephant
