#pragma once

#include <iomanip>
#include <limits>
#include <locale>
#include <sstream>
#include <string>

namespace caseelephant::testing {
    // Owner timestamp text handed to JS hosts. Timer checkpoints and animation opportunities must
    // round-trip the SAME timestamp; std::to_string rounds to six decimals and can move timers ahead
    // of the following full-precision rAF opportunity.
    inline std::string format_host_time(double time) {
        std::ostringstream out;
        out.imbue(std::locale::classic());
        out << std::setprecision(std::numeric_limits<double>::max_digits10) << time;
        return out.str();
    }
} // namespace caseelephant::testing
