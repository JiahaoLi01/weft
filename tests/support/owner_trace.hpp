#pragma once

#include "json.hpp"
#include "../test_constants/support.hpp"

#include <atomic>
#include <chrono>
#include <deque>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <locale>
#include <memory>
#include <string>

namespace weft::testing {
    // Shared switch of one profiling run; run_id keeps files of earlier runs apart.
    struct TraceControl {
        std::atomic<bool> enabled{false};
        const long long run_id =
            std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now().time_since_epoch())
                .count();
    };

    // Chrome trace recorder for one thread. No per-event file writes; a long run retains its latest
    // bounded window, including the user's last interactions, and writes it once on destruction.
    class OwnerTrace {
        struct Event {
            std::string name;
            std::string args;
            double start{};
            double duration{};
        };

        std::filesystem::path path_;
        std::shared_ptr<TraceControl> control_;
        std::deque<Event> events_;
        std::size_t dropped_{};
        std::size_t emitted_{};
        std::size_t inputs_{};
        std::size_t publications_{};
        unsigned tid_;

    public:
        explicit OwnerTrace(std::filesystem::path path, unsigned tid = weft::test_constants::support::owner_trace_tid,
                            std::shared_ptr<TraceControl> control = {})
            : path_(std::move(path)), control_(std::move(control)), tid_(tid) {
        }

        bool enabled() const {
            return !path_.empty() && (!control_ || control_->enabled.load(std::memory_order_relaxed));
        }

        void emit(const std::string& name, std::chrono::steady_clock::time_point start, double duration,
                  const std::string& args = "{}") {
            if (!enabled()) {
                return;
            }
            if (events_.size() == weft::test_constants::support::owner_trace_event_limit) {
                events_.pop_front();
                ++dropped_;
            }
            ++emitted_;
            inputs_ += name.starts_with("input scene=");
            publications_ += name == "owner publication";
            events_.push_back({name, args, std::chrono::duration<double, std::micro>(start.time_since_epoch()).count(),
                               duration * weft::test_constants::support::microseconds_per_millisecond});
        }

        // Emits one complete event covering its own lifetime.
        struct Scope {
            OwnerTrace& trace;
            std::string name;
            std::string args;
            bool active;
            std::chrono::steady_clock::time_point start;

            Scope(OwnerTrace& t, std::string n, std::string a, bool allowed = true)
                : trace(t),
                  name(std::move(n)),
                  args(std::move(a)),
                  active(allowed && t.enabled()),
                  start(active ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{}) {
            }

            ~Scope() {
                if (active) {
                    trace.emit(
                        name, start,
                        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count(),
                        args);
                }
            }
        };

        ~OwnerTrace() {
            if (!emitted_) {
                return;
            }
            std::ofstream stream(path_, std::ios::binary);
            if (!stream) {
                return;
            }
            stream.imbue(std::locale::classic());
            stream << std::setprecision(weft::test_constants::support::trace_timestamp_digits) << "{\"traceEvents\":[";
            bool comma{};
            for (const auto& e : events_) {
                if (comma) {
                    stream << ',';
                }
                comma = true;
                stream << "{\"ph\":\"X\",\"pid\":1,\"tid\":" << tid_ << ",\"name\":" << json_string(e.name)
                       << ",\"ts\":" << e.start << ",\"dur\":" << e.duration << ",\"args\":" << e.args << '}';
            }
            stream << "],\"run_id\":" << (control_ ? control_->run_id : 0)
                   << ",\"clock\":\"steady_clock_us\",\"event_limit\":65536,\"retained_window\":\"latest\","
                      "\"emitted_events\":"
                   << emitted_ << ",\"sampled_inputs\":" << inputs_ << ",\"sampled_publications\":" << publications_
                   << ",\"dropped_events\":" << dropped_ << '}';
        }
    };
} // namespace weft::testing
