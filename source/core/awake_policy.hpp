#pragma once

#include <cstdint>

// When the console may sleep. Work holds it awake, so closing the lid leaves
// Wi-Fi on and the work going, and it stays awake a while after the last
// hold ends: the gap between one file of a batch and the next, one download
// of a queue and the next, or a page's retry never lets it fall asleep
// halfway. Times are milliseconds on a clock that only goes forward.
namespace awake {

constexpr uint64_t LingerMs = 30 * 1000;

class Policy {
public:
    void hold();
    void release(uint64_t now);
    bool sleepAllowed(uint64_t now) const;
    bool held() const { return holds_ > 0; }

private:
    int holds_ = 0;
    bool lingering_ = false;
    uint64_t releasedAt_ = 0;
};

} // namespace awake
