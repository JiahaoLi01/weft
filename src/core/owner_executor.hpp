#pragma once
#include <chrono>
#include <algorithm>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <exception>
#include <functional>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <thread>
#include <variant>

#include "constants/script.hpp"

namespace weft {
    // Private C++ host adapter, not the external C ABI. Request/Input/Frame must
    // contain owned values; never put a Node*, JSValue or device handle in them.
    // Factory, hooks and their captures are created AND destroyed on the worker.
    template <class Request, class Input, class Frame>
    class OwnerExecutor {
    public:
        struct Hooks {
            std::function<void(const Input&)> input;
            std::function<std::shared_ptr<const Frame>(const Request&)> frame;
            std::function<void(std::uint64_t)> reset;
            std::function<void()> close;
        };

        struct Publication {
            std::uint64_t epoch{};
            std::uint64_t sequence{};
            std::uint64_t input_sequence{};
            std::shared_ptr<const Frame> value;
            double active_ms{};
            double queue_ms{};
            std::chrono::steady_clock::time_point published;
        };

        enum Result {
            Accepted,
            Full,
            Stale,
            Closed
        };

        struct Work {
            std::size_t requests{};
            std::size_t coalesced_requests{};
            std::size_t published{};
            std::size_t discarded_frames{};
            std::size_t stale_publications{};
            std::size_t accepted_inputs{};
            std::size_t processed_inputs{};
            std::size_t resets{};
            std::size_t coalesced_inputs{};
            std::size_t peak_pending_inputs{};
        };

    private:
        struct Tick {
            Request value;
            std::uint64_t epoch{};
            std::uint64_t sequence{};
            std::chrono::steady_clock::time_point enqueued;
        };

        struct Reset {
            std::uint64_t epoch{};
        };

        struct Event {
            std::variant<Input, Reset> value;
            std::uint64_t sequence{};
        };

        std::mutex mutex_;
        std::condition_variable cv_;
        std::deque<Event> inputs_;
        std::optional<Tick> pending_;
        std::deque<Publication> frames_;
        std::optional<Publication> last_;
        std::uint64_t epoch_ = 1;
        std::uint64_t sequence_{};
        std::uint64_t input_sequence_{};
        bool ready_{};
        bool closed_{};
        bool stopping_{};
        std::exception_ptr failure_;
        Work work_;
        std::thread worker_;
        const std::size_t capacity_;

        void check() {
            if (failure_) {
                std::rethrow_exception(failure_);
            }
        }

        static double ms(std::chrono::steady_clock::duration d) {
            return std::chrono::duration<double, std::milli>(d).count();
        }

        void run(std::function<Hooks()> factory) noexcept {
            std::optional<Hooks> hooks;
            std::exception_ptr error;
            std::uint64_t processed{};
            double input_ms{};
            try {
                hooks = factory();
                factory = {};
                if (!hooks->frame || !hooks->input || !hooks->reset || !hooks->close) {
                    throw std::invalid_argument("WEFT_OWNER_HOOKS");
                }
                {
                    std::lock_guard lock(mutex_);
                    ready_ = true;
                }
                cv_.notify_all();
                for (;;) {
                    std::deque<Event> events;
                    std::optional<Tick> tick;
                    {
                        std::unique_lock lock(mutex_);
                        cv_.wait(lock, [&] {
                            return stopping_ || !inputs_.empty() || pending_;
                        });
                        events.swap(inputs_);
                        tick = std::move(pending_);
                        pending_.reset();
                        if (stopping_ && events.empty() && !tick) {
                            break;
                        }
                    }
                    const auto input_start = std::chrono::steady_clock::now();
                    for (const auto& event : events) {
                        if (auto input = std::get_if<Input>(&event.value)) {
                            hooks->input(*input);
                            processed = event.sequence;
                            std::lock_guard lock(mutex_);
                            ++work_.processed_inputs;
                        } else {
                            hooks->reset(std::get<Reset>(event.value).epoch);
                            std::lock_guard lock(mutex_);
                            ++work_.resets;
                        }
                    }
                    input_ms += ms(std::chrono::steady_clock::now() - input_start);
                    if (tick) {
                        const auto start = std::chrono::steady_clock::now();
                        auto value = hooks->frame(tick->value);
                        if (!value) {
                            throw std::runtime_error("WEFT_OWNER_NULL_FRAME");
                        }
                        const auto finish = std::chrono::steady_clock::now();
                        Publication publication{tick->epoch,
                                                tick->sequence,
                                                processed,
                                                std::move(value),
                                                input_ms + ms(finish - start),
                                                ms(start - tick->enqueued),
                                                finish};
                        // Dropped snapshots can own large scene trees. Move them
                        // out while locked; free them after releasing the queue.
                        std::optional<Publication> retired_frame;
                        std::optional<Publication> retired_last;
                        input_ms = 0;
                        {
                            std::lock_guard lock(mutex_);
                            if (tick->epoch == epoch_) {
                                if (frames_.size() == constants::script::max_queued_publications) {
                                    retired_frame = std::move(frames_.front());
                                    frames_.pop_front();
                                    ++work_.discarded_frames;
                                }
                                frames_.push_back(publication);
                                retired_last.swap(last_);
                                last_ = std::move(publication);
                                ++work_.published;
                            } else {
                                ++work_.stale_publications;
                            }
                        }
                        cv_.notify_all();
                    }
                }
            } catch (...) {
                error = std::current_exception();
            }
            if (hooks) {
                try {
                    hooks->close();
                } catch (...) {
                    if (!error) {
                        error = std::current_exception();
                    }
                }
                hooks.reset();
            }
            // Factory captures must also die here if initialization threw.
            factory = {};
            {
                std::lock_guard lock(mutex_);
                failure_ = error;
                ready_ = closed_ = true;
            }
            cv_.notify_all();
        }

    public:
        explicit OwnerExecutor(std::function<Hooks()> factory,
                               std::size_t capacity = constants::script::default_owner_input_capacity)
            : capacity_(capacity) {
            if (!factory || !capacity || capacity > constants::script::max_owner_input_capacity) {
                throw std::invalid_argument("WEFT_OWNER_CAPACITY");
            }
            worker_ = std::thread([this, factory = std::move(factory)]() mutable {
                run(std::move(factory));
            });
            std::unique_lock lock(mutex_);
            cv_.wait(lock, [&] {
                return ready_;
            });
            if (failure_) {
                lock.unlock();
                worker_.join();
                std::rethrow_exception(failure_);
            }
        }

        ~OwnerExecutor() {
            try {
                close();
            } catch (...) { /* A worker failure is rethrown by the explicit host close/read. */
            }
        }

        OwnerExecutor(const OwnerExecutor&) = delete;
        OwnerExecutor& operator=(const OwnerExecutor&) = delete;

        Result input(std::uint64_t epoch, Input value) {
            std::lock_guard lock(mutex_);
            check();
            if (stopping_ || closed_) {
                return Closed;
            }
            if (epoch != epoch_) {
                return Stale;
            }
            if (inputs_.size() == capacity_) {
                return Full;
            }
            if (input_sequence_ == UINT64_MAX) {
                throw std::overflow_error("WEFT_OWNER_INPUT_SERIAL");
            }
            inputs_.push_back({std::move(value), ++input_sequence_});
            ++work_.accepted_inputs;
            cv_.notify_all();
            return Accepted;
        }

        // Explicit host sampling policy, separate from reliable input(). Only the
        // immediately preceding pending value can be replaced; never an in-flight
        // event or an intervening reset/down/up/cancel. Predicate sees values only.
        template <class Compatible>
        Result input_latest(std::uint64_t epoch, Input value, Compatible compatible) {
            std::optional<Event> retired;
            {
                std::lock_guard lock(mutex_);
                check();
                if (stopping_ || closed_) {
                    return Closed;
                }
                if (epoch != epoch_) {
                    return Stale;
                }
                if (input_sequence_ == UINT64_MAX) {
                    throw std::overflow_error("WEFT_OWNER_INPUT_SERIAL");
                }
                bool replace{};
                if (!inputs_.empty()) {
                    if (const auto* previous = std::get_if<Input>(&inputs_.back().value)) {
                        replace = compatible(*previous, value);
                    }
                }
                if (!replace && inputs_.size() == capacity_) {
                    return Full;
                }
                if (replace) {
                    retired = std::move(inputs_.back());
                    inputs_.pop_back();
                    ++work_.coalesced_inputs;
                }
                inputs_.push_back({std::move(value), ++input_sequence_});
                ++work_.accepted_inputs;
                work_.peak_pending_inputs = std::max(work_.peak_pending_inputs, inputs_.size());
                cv_.notify_all();
            }
            return Accepted;
        }

        std::uint64_t request(std::uint64_t epoch, Request value) {
            std::optional<Tick> retired;
            std::uint64_t serial;
            {
                std::lock_guard lock(mutex_);
                check();
                if (stopping_ || closed_) {
                    throw std::logic_error("WEFT_OWNER_CLOSED");
                }
                if (epoch != epoch_) {
                    throw std::invalid_argument("WEFT_OWNER_STALE_EPOCH");
                }
                if (sequence_ == UINT64_MAX) {
                    throw std::overflow_error("WEFT_OWNER_FRAME_SERIAL");
                }
                Tick tick{std::move(value), epoch, ++sequence_, std::chrono::steady_clock::now()};
                serial = sequence_;
                if (pending_) {
                    ++work_.coalesced_requests;
                }
                retired.swap(pending_);
                pending_ = std::move(tick);
                ++work_.requests;
            }
            cv_.notify_all();
            return serial;
        }

        Result reset(std::uint64_t epoch) {
            std::optional<Tick> retired_tick;
            std::deque<Publication> retired_frames;
            std::optional<Publication> retired_last;
            {
                std::lock_guard lock(mutex_);
                check();
                if (stopping_ || closed_) {
                    return Closed;
                }
                if (epoch <= epoch_) {
                    return Stale;
                }
                if (inputs_.size() == capacity_) {
                    return Full;
                }
                inputs_.push_back({Reset{epoch}, input_sequence_});
                epoch_ = epoch;
                retired_tick.swap(pending_);
                retired_frames.swap(frames_);
                retired_last.swap(last_);
            }
            cv_.notify_all();
            return Accepted;
        }

        std::optional<Publication> latest() {
            std::deque<Publication> retired;
            std::optional<Publication> value;
            {
                std::lock_guard lock(mutex_);
                check();
                if (frames_.empty()) {
                    return {};
                }
                value = std::move(frames_.back());
                work_.discarded_frames += frames_.size() - 1;
                retired.swap(frames_);
            }
            return value;
        }

        Publication
        wait(std::uint64_t epoch, std::uint64_t sequence,
             std::chrono::milliseconds timeout = std::chrono::seconds(constants::script::default_owner_wait_seconds)) {
            std::deque<Publication> retired;
            std::unique_lock lock(mutex_);
            if (timeout.count() <= 0 || timeout > std::chrono::seconds(constants::script::max_owner_wait_seconds)) {
                throw std::invalid_argument("WEFT_OWNER_WAIT_BOUND");
            }
            if (!cv_.wait_for(lock, timeout, [&] {
                    return failure_ || closed_ || epoch != epoch_ || (last_ && last_->sequence >= sequence);
                })) {
                throw std::runtime_error("WEFT_OWNER_DEADLINE");
            }
            check();
            if (epoch != epoch_) {
                throw std::invalid_argument("WEFT_OWNER_STALE_EPOCH");
            }
            if (!last_ || last_->sequence < sequence) {
                throw std::logic_error("WEFT_OWNER_CLOSED");
            }
            auto result = *last_;
            if (!frames_.empty()) {
                work_.discarded_frames += frames_.size() - 1;
                retired.swap(frames_);
            }
            lock.unlock();
            return result;
        }

        Work work() {
            std::lock_guard lock(mutex_);
            return work_;
        }

        void close() {
            std::optional<Tick> retired;
            {
                std::lock_guard lock(mutex_);
                stopping_ = true;
                retired.swap(pending_);
            }
            cv_.notify_all();
            if (worker_.joinable()) {
                if (worker_.get_id() == std::this_thread::get_id()) {
                    throw std::logic_error("WEFT_OWNER_JOIN_SELF");
                }
                worker_.join();
            }
            std::lock_guard lock(mutex_);
            check();
        }
    };
} // namespace weft
