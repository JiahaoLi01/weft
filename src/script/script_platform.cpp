#include "script/script_platform.hpp"
#include "script/script_dom.hpp"

#include <cstddef>

namespace weft {
    namespace {
        // Generated from src/script/js/*.js by cmake/embed_bytes.cmake.
        constexpr unsigned char web_host[] = {
#include "script/js/web_host_bootstrap.js.inc"
        };
        constexpr unsigned char animation_frame[] = {
#include "script/js/animation_frame_bootstrap.js.inc"
        };
        constexpr unsigned char scene_platform[] = {
#include "script/js/scene_platform_bootstrap.js.inc"
        };
        constexpr unsigned char engine[] = {
#include "script/js/engine_bootstrap.js.inc"
        };
        constexpr unsigned char crpg_bridge[] = {
#include "script/js/crpg_bridge_bootstrap.js.inc"
        };
        constexpr unsigned char web_platform[] = {
#include "script/js/web_platform_bootstrap.js.inc"
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
            case ScriptPlatformModule::Engine:
                return text(engine);
            case ScriptPlatformModule::CrpgBridge:
                return text(crpg_bridge);
            case ScriptPlatformModule::WebPlatform:
                return text(web_platform);
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
            case ScriptPlatformModule::Engine:
                return "engine_bootstrap.js";
            case ScriptPlatformModule::CrpgBridge:
                return "crpg_bridge_bootstrap.js";
            case ScriptPlatformModule::WebPlatform:
                return "web_platform_bootstrap.js";
        }
        return {};
    }

    std::string install_script_platform(ScriptDom& vm, ScriptPlatformModule module) {
        return vm.eval(script_platform_source(module), script_platform_file(module));
    }
} // namespace weft
