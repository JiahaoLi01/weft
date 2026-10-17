#pragma once

#include <string>
#include <string_view>

namespace weft {
    class ScriptDom;

    // Owner-hosted JS platform modules. Their sources live in src/script/js/ and are compiled into
    // the script library, so a running engine needs no source tree. Install WebHost first;
    // AnimationFrame and ScenePlatform build on it.
    enum class ScriptPlatformModule {
        // DOM constructors and CSSOM declarations over the native DOM, timers, microtasks, console.
        WebHost,
        // requestAnimationFrame batches driven by the owner.
        AnimationFrame,
        // SVG/Element prototypes, window events, geometry reads, ResizeObserver and matchMedia.
        ScenePlatform,
        // weft.engine, the engine bridge (ScriptDom::install_engine).
        Engine,
        // CRPG Framework's window.ue.sendEvent, __ue_dispatch and __vm_update over weft.engine; after Engine.
        CrpgBridge,
        // The web platform pages written for browsers expect (console, localStorage, dataset, focus, KeyboardEvent,
        // matchMedia as @media, IntersectionObserver); after ScenePlatform and Engine, with ScriptDom's web platform
        // hosts installed.
        WebPlatform,
    };

    std::string_view script_platform_source(ScriptPlatformModule module) noexcept;

    // File name reported in script stack traces.
    std::string_view script_platform_file(ScriptPlatformModule module) noexcept;

    // Evaluates the module in vm; returns the evaluation result like ScriptDom::eval.
    std::string install_script_platform(ScriptDom& vm, ScriptPlatformModule module);
} // namespace weft
