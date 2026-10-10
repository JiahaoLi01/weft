#pragma once

#include <string>
#include <string_view>

namespace caseelephant {
    class ScriptDomProbe;

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
    };

    std::string_view script_platform_source(ScriptPlatformModule module) noexcept;

    // File name reported in script stack traces.
    std::string_view script_platform_file(ScriptPlatformModule module) noexcept;

    // Evaluates the module in vm; returns the evaluation result like ScriptDomProbe::eval.
    std::string install_script_platform(ScriptDomProbe& vm, ScriptPlatformModule module);
} // namespace caseelephant
