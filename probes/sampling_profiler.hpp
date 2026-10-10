#pragma once

#include <windows.h>

#include "probe_constants.hpp"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <thread>
#include <vector>

// Windows developer tool: samples one thread's call stack at a fixed period and writes inclusive and exclusive
// function shares. Each sample suspends the thread, unwinds it with the x64 unwind tables and resumes it; nothing is
// allocated while the thread is suspended. Names come from DbgHelp after sampling stops, inline frames included, so
// the binary needs PDBs (the windows-profile preset); without them a frame is module+offset. Unwinding takes the
// loader's function-table lock, so do not sample a thread that loads or unloads modules.
namespace probes {
    class SamplingProfiler {
    public:
        // Samples the calling thread while active.
        explicit SamplingProfiler(std::chrono::microseconds period);
        ~SamplingProfiler();
        SamplingProfiler(const SamplingProfiler&) = delete;
        SamplingProfiler& operator=(const SamplingProfiler&) = delete;

        // Samples are taken only while active (for example, inside measured ticks).
        void active(bool on) {
            active_.store(on, std::memory_order_relaxed);
        }

        void stop();
        // Writes the report, and the folded stacks next to it as <name>.folded (stops sampling first); returns the
        // number of samples.
        std::size_t write(const std::filesystem::path& path,
                          std::size_t top = weft::probe_constants::sampler_report_rows);

    private:
        void run();

        HANDLE target_{};
        ULONG_PTR stack_low_{};
        ULONG_PTR stack_high_{};
        std::chrono::microseconds period_;
        std::atomic<bool> running_{true};
        std::atomic<bool> active_{};
        // Return addresses of every sample, innermost first; depths_ splits them per sample.
        std::vector<std::uint64_t> frames_;
        std::vector<std::uint32_t> depths_;
        std::thread sampler_;
    };
} // namespace probes
