#pragma once

#include <condition_variable>
#include <cstddef>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

// Drops the values a render thread is done with on a thread of its own (step 12.1 of docs/refactoring-plan.md):
// releasing the last reference to a scene frame destroys its whole tree, which would take the render thread's time.
// Only immutable values that no other thread writes, such as published scene frames, may be given to it. At most
// max_pending wait; past that the caller drops in place.
namespace probes {
    class FrameReclaimer {
    public:
        explicit FrameReclaimer(std::size_t max_pending = 64) : max_pending_(max_pending) {
            worker_ = std::thread([this] {
                run();
            });
        }

        FrameReclaimer(const FrameReclaimer&) = delete;
        FrameReclaimer& operator=(const FrameReclaimer&) = delete;

        // Drops what is left of the queue, then stops.
        ~FrameReclaimer() {
            {
                std::lock_guard lock(mutex_);
                stopping_ = true;
            }
            wake_.notify_one();
            worker_.join();
        }

        void drop(std::shared_ptr<const void> value) {
            if (!value) {
                return;
            }
            {
                std::lock_guard lock(mutex_);
                if (queue_.size() >= max_pending_) {
                    return;
                }
                queue_.push_back(std::move(value));
            }
            wake_.notify_one();
        }

    private:
        void run() {
            std::unique_lock lock(mutex_);
            for (;;) {
                wake_.wait(lock, [&] {
                    return stopping_ || !queue_.empty();
                });
                auto batch = std::move(queue_);
                queue_.clear();
                lock.unlock();
                batch.clear();
                lock.lock();
                if (stopping_ && queue_.empty()) {
                    return;
                }
            }
        }

        std::size_t max_pending_{};
        std::mutex mutex_;
        std::condition_variable wake_;
        std::vector<std::shared_ptr<const void>> queue_;
        bool stopping_{};
        std::thread worker_;
    };
} // namespace probes
