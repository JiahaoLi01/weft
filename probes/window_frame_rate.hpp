#pragma once
#include <cmath>
#include <stdexcept>
#include "caseelephant/constants/common.hpp"

// Sample-host presentation rate. Timestamp is monotonic seconds, taken after
// successful Present; idle composition counts, failed/occluded calls do not.
class WindowFrameRate {
public:
    double fps{};
    double frame_ms{};
    unsigned samples{};
    bool ready{};

    void reset(double now) {
        validate(now);
        started_ = previous_ = now;
        frames_ = 0;
        fps = frame_ms = 0;
        ready = false;
    }

    bool presented(double now) {
        validate(now);
        frame_ms = (now - previous_) * caseelephant::constants::milliseconds_per_second;
        previous_ = now;
        ++frames_;
        const auto interval = now - started_;
        if (interval < 1) {
            return false;
        }
        fps = frames_ / interval;
        started_ = now;
        frames_ = 0;
        ready = true;
        ++samples;
        return true;
    }

private:
    double started_{};
    double previous_{};
    unsigned frames_{};

    void validate(double now) const {
        if (!std::isfinite(now) || now < previous_) {
            throw std::invalid_argument("CE_FPS_CLOCK: monotonic finite time");
        }
    }
};
