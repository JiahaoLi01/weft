#include "sampling_profiler.hpp"
#include "probe_constants.hpp"

#include <dbghelp.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <map>
#include <string>
#include <unordered_map>
#include <utility>

namespace probes {
    namespace {
        using weft::probe_constants::sampler_callers_shown;
        using weft::probe_constants::sampler_frame_capacity;
        using weft::probe_constants::sampler_max_depth;
        using weft::probe_constants::sampler_sample_capacity;

        using Counts = std::unordered_map<std::string, std::size_t>;

        std::vector<std::pair<std::string, std::size_t>> sorted(const Counts& counts) {
            std::vector<std::pair<std::string, std::size_t>> out(counts.begin(), counts.end());
            std::sort(out.begin(), out.end(), [](const auto& a, const auto& b) {
                return a.second != b.second ? a.second > b.second : a.first < b.first;
            });
            return out;
        }

        bool own_code(const std::string& name) {
            return name.starts_with("weft::") || name.starts_with("probes::") || name.starts_with("JS_") ||
                   name.starts_with("js_") || name.starts_with("YG") || name.starts_with("facebook::yoga::") ||
                   name.starts_with("hb_") || name.starts_with("FT_");
        }
    } // namespace

    SamplingProfiler::SamplingProfiler(std::chrono::microseconds period)
        : target_(OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE,
                             GetCurrentThreadId())),
          period_(period) {
        GetCurrentThreadStackLimits(&stack_low_, &stack_high_);
        frames_.reserve(sampler_frame_capacity);
        depths_.reserve(sampler_sample_capacity);
        sampler_ = std::thread([this] {
            run();
        });
    }

    SamplingProfiler::~SamplingProfiler() {
        stop();
        if (target_) {
            CloseHandle(target_);
        }
    }

    void SamplingProfiler::stop() {
        running_.store(false);
        if (sampler_.joinable()) {
            sampler_.join();
        }
    }

    void SamplingProfiler::run() {
        // A high-resolution waitable timer gives sub-millisecond periods without changing the system timer.
        const HANDLE timer =
            CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
        while (running_.load(std::memory_order_relaxed)) {
            if (timer) {
                LARGE_INTEGER due{};
                due.QuadPart = -std::int64_t(period_.count()) * 10;
                SetWaitableTimer(timer, &due, 0, nullptr, nullptr, FALSE);
                WaitForSingleObject(timer, INFINITE);
            } else {
                Sleep(1);
            }
            if (!active_.load(std::memory_order_relaxed) || frames_.size() + sampler_max_depth > frames_.capacity() ||
                depths_.size() == depths_.capacity() || !target_) {
                continue;
            }
            if (SuspendThread(target_) == DWORD(-1)) {
                break;
            }
            // Only stack reads and unwind-table lookups until ResumeThread.
            CONTEXT context{};
            context.ContextFlags = CONTEXT_CONTROL | CONTEXT_INTEGER;
            std::uint32_t depth{};
            if (GetThreadContext(target_, &context)) {
                while (depth < sampler_max_depth && context.Rip && context.Rsp >= stack_low_ &&
                       context.Rsp + sizeof(DWORD64) <= stack_high_) {
                    frames_.push_back(context.Rip);
                    ++depth;
                    DWORD64 image_base{};
                    auto* function = RtlLookupFunctionEntry(context.Rip, &image_base, nullptr);
                    if (!function) {
                        // A leaf without unwind data: the return address is on top of the stack.
                        context.Rip = *reinterpret_cast<const DWORD64*>(context.Rsp);
                        context.Rsp += sizeof(DWORD64);
                        continue;
                    }
                    void* handler_data{};
                    DWORD64 establisher{};
                    RtlVirtualUnwind(UNW_FLAG_NHANDLER, image_base, context.Rip, function, &context, &handler_data,
                                     &establisher, nullptr);
                }
            }
            ResumeThread(target_);
            if (depth) {
                depths_.push_back(depth);
            }
        }
        if (timer) {
            CloseHandle(timer);
        }
    }

    std::size_t SamplingProfiler::write(const std::filesystem::path& path, std::size_t top) {
        stop();
        const HANDLE process = GetCurrentProcess();
        SymSetOptions(SymGetOptions() | SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS);
        const bool symbols = SymInitialize(process, nullptr, TRUE);
        alignas(SYMBOL_INFO) char buffer[sizeof(SYMBOL_INFO) + MAX_SYM_NAME]{};
        auto* symbol = reinterpret_cast<SYMBOL_INFO*>(buffer);
        auto name = [&](DWORD64 address, DWORD inline_context, bool inline_frame) -> std::string {
            std::memset(buffer, 0, sizeof(buffer));
            symbol->SizeOfStruct = sizeof(SYMBOL_INFO);
            symbol->MaxNameLen = MAX_SYM_NAME;
            DWORD64 displacement{};
            if (symbols && (inline_frame ? SymFromInlineContext(process, address, inline_context, &displacement, symbol)
                                         : SymFromAddr(process, address, &displacement, symbol))) {
                return std::string(symbol->Name, symbol->NameLen);
            }
            HMODULE module{};
            char file[MAX_PATH]{};
            if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                                       GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                   reinterpret_cast<LPCSTR>(address), &module) &&
                GetModuleFileNameA(module, file, MAX_PATH)) {
                char offset[32];
                std::snprintf(offset, sizeof(offset), "+0x%llx",
                              static_cast<unsigned long long>(address - reinterpret_cast<std::uintptr_t>(module)));
                return std::filesystem::path(file).filename().string() + offset;
            }
            return "?";
        };
        // Logical frames of one code address, innermost inline frame first.
        std::unordered_map<std::uint64_t, std::vector<std::string>> logical;
        auto frames_of = [&](std::uint64_t address) -> const std::vector<std::string>& {
            auto [it, inserted] = logical.try_emplace(address);
            if (inserted) {
                const DWORD inlined = symbols ? SymAddrIncludeInlineTrace(process, address) : 0;
                DWORD context{};
                DWORD index{};
                if (inlined && SymQueryInlineTrace(process, address, 0, address, address, &context, &index)) {
                    for (DWORD i = 0; i < inlined; ++i) {
                        it->second.push_back(name(address, context + i, true));
                    }
                }
                it->second.push_back(name(address, 0, false));
            }
            return it->second;
        };
        Counts folded;
        Counts inclusive;
        Counts exclusive;
        Counts own;
        std::unordered_map<std::string, Counts> callers;
        std::unordered_map<std::string, Counts> own_callers;
        std::vector<std::string> stack;
        std::size_t offset{};
        for (const auto depth : depths_) {
            stack.clear();
            for (std::uint32_t i = 0; i < depth; ++i) {
                // Return addresses point after the call instruction; name the call itself.
                const auto& names = frames_of(frames_[offset + i] - (i ? 1 : 0));
                stack.insert(stack.end(), names.begin(), names.end());
            }
            offset += depth;
            std::string line;
            for (auto it = stack.rbegin(); it != stack.rend(); ++it) {
                line += (line.empty() ? "" : ";") + *it;
            }
            ++folded[line];
            ++exclusive[stack.front()];
            if (stack.size() > 1) {
                ++callers[stack.front()][stack[1]];
            }
            // The innermost frame of this project or a dependency it drives, so library internals (std::, CRT)
            // count toward the code that called them.
            const auto mine = std::find_if(stack.begin(), stack.end(), own_code);
            ++own[mine == stack.end() ? stack.front() : *mine];
            if (mine != stack.end()) {
                const auto caller = std::find_if(mine + 1, stack.end(), [&](const std::string& n) {
                    return own_code(n) && n != *mine;
                });
                if (caller != stack.end()) {
                    ++own_callers[*mine][*caller];
                }
            }
            std::vector<std::string> seen;
            for (const auto& n : stack) {
                if (std::find(seen.begin(), seen.end(), n) == seen.end()) {
                    seen.push_back(n);
                    ++inclusive[n];
                }
            }
        }
        if (symbols) {
            SymCleanup(process);
        }
        {
            // Folded stacks (outermost first, the flame graph input format) for offline queries.
            std::ofstream stacks(std::filesystem::path(path).replace_extension(".folded"));
            for (const auto& [line, count] : sorted(folded)) {
                stacks << line << ' ' << count << '\n';
            }
        }
        std::ofstream out(path);
        const auto total = depths_.size();
        auto share = [&](std::size_t n) {
            char text[32];
            std::snprintf(text, sizeof(text), "%6.2f%% %7zu  ", total ? 100.0 * double(n) / double(total) : 0.0, n);
            return std::string(text);
        };
        auto table = [&](const char* title, const Counts& counts,
                         const std::unordered_map<std::string, Counts>* with_callers) {
            out << "== " << title << "\n";
            const auto rows = sorted(counts);
            for (std::size_t i = 0; i < rows.size() && i < top; ++i) {
                out << share(rows[i].second) << rows[i].first << '\n';
                if (with_callers && with_callers->contains(rows[i].first)) {
                    const auto from = sorted(with_callers->at(rows[i].first));
                    for (std::size_t j = 0; j < from.size() && j < sampler_callers_shown; ++j) {
                        out << "                    <- " << share(from[j].second) << from[j].first << '\n';
                    }
                }
            }
            out << '\n';
        };
        out << "samples " << total << ", period " << period_.count() << " us, symbols " << (symbols ? "yes" : "no")
            << "\n\n";
        table("innermost project frame (library internals charged to their caller), with project callers", own,
              &own_callers);
        table("exclusive (innermost frame, with callers)", exclusive, &callers);
        table("inclusive", inclusive, nullptr);
        return total;
    }
} // namespace probes
