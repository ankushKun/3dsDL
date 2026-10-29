#pragma once

#include <cstdint>
#include <string>

namespace units {

// "512 B", "1.5 KiB", "12.3 MiB", "1.25 GiB"
std::string bytes(uint64_t count);
// The same in at most four characters before the unit, for tight spaces:
// three significant digits, and the next unit up from 1000 ("8.00 GiB",
// "99.5 GiB", "238 GiB", "0.98 GiB" rather than "1023.9 MiB").
std::string shortBytes(uint64_t count);
// "3,210,180,450"
std::string grouped(uint64_t count);
// "850 KiB/s"
std::string rate(double bytesPerSecond);
// "42s", "3m 05s", "1h 02m"
std::string duration(uint64_t seconds);
// An estimate of the time left, to show while something is on its way:
// "8 s left", "45 s left", "3 min left", "1 h 20 min left". Rounded, so it
// holds still from one frame to the next.
std::string timeLeft(uint64_t seconds);
// "2026-10-12 14:03". The 3DS clock has no time zone, so the value is shown
// exactly as stored.
std::string dateTime(int64_t unixSeconds);

// Smoothed transfer rate from (time, byte count) samples.
class RateMeter {
public:
    void reset();
    void sample(uint64_t milliseconds, uint64_t totalBytes);
    bool hasRate() const { return hasRate_; }
    double bytesPerSecond() const { return rate_; }
    // Seconds left for remaining bytes, or -1 when there is no estimate yet.
    int64_t secondsLeft(uint64_t remainingBytes) const;

private:
    static constexpr uint64_t MinimumIntervalMs = 500;
    static constexpr double Smoothing = 0.3;

    bool started_ = false;
    bool hasRate_ = false;
    uint64_t lastTime_ = 0;
    uint64_t lastBytes_ = 0;
    double rate_ = 0.0;
};

} // namespace units
