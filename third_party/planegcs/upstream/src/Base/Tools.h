// Compatibility shim for the extracted PlaneGCS target.
#pragma once

#include <chrono>

namespace Base {

class TimeElapsed {
public:
    TimeElapsed() = default;

    static float diffTimeF(const TimeElapsed& start, const TimeElapsed& end = TimeElapsed()) {
        return std::chrono::duration<float>(end.time_ - start.time_).count();
    }

private:
    std::chrono::steady_clock::time_point time_{std::chrono::steady_clock::now()};
};

}  // namespace Base
