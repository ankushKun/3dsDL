#include "units.hpp"

#include <algorithm>
#include <cstdio>
#include <ctime>

namespace units {

std::string bytes(uint64_t count)
{
    static const char *const suffixes[] = {"KiB", "MiB", "GiB", "TiB"};
    if (count < 1024) {
        return std::to_string(count) + " B";
    }

    double value = static_cast<double>(count) / 1024.0;
    size_t suffix = 0;
    while (value >= 1024.0 && suffix + 1 < sizeof(suffixes) / sizeof(*suffixes)) {
        value /= 1024.0;
        ++suffix;
    }

    char buffer[32];
    std::snprintf(buffer, sizeof(buffer), value < 10.0 ? "%.2f %s" : "%.1f %s",
                  value, suffixes[suffix]);
    return buffer;
}

std::string grouped(uint64_t count)
{
    std::string digits = std::to_string(count);
    for (int at = static_cast<int>(digits.size()) - 3; at > 0; at -= 3) {
        digits.insert(static_cast<size_t>(at), ",");
    }
    return digits;
}

std::string shortBytes(uint64_t count)
{
    static const char *const suffixes[] = {"B", "KiB", "MiB", "GiB", "TiB"};
    double value = static_cast<double>(count);
    size_t suffix = 0;
    // 999.5 would round to a fourth digit, so it moves up too.
    while (value >= 999.5 && suffix + 1 < sizeof(suffixes) / sizeof(*suffixes)) {
        value /= 1024.0;
        ++suffix;
    }
    char buffer[32];
    if (suffix == 0) {
        std::snprintf(buffer, sizeof(buffer), "%llu B",
                      static_cast<unsigned long long>(count));
    } else {
        std::snprintf(buffer, sizeof(buffer),
                      value < 9.995 ? "%.2f %s" : value < 99.95 ? "%.1f %s"
                                                                : "%.0f %s",
                      value, suffixes[suffix]);
    }
    return buffer;
}

std::string rate(double bytesPerSecond)
{
    if (bytesPerSecond < 0.0) {
        bytesPerSecond = 0.0;
    }
    return bytes(static_cast<uint64_t>(bytesPerSecond)) + "/s";
}

std::string duration(uint64_t seconds)
{
    char buffer[32];
    if (seconds < 60) {
        std::snprintf(buffer, sizeof(buffer), "%us",
                      static_cast<unsigned>(seconds));
    } else if (seconds < 3600) {
        std::snprintf(buffer, sizeof(buffer), "%um %02us",
                      static_cast<unsigned>(seconds / 60),
                      static_cast<unsigned>(seconds % 60));
    } else {
        std::snprintf(buffer, sizeof(buffer), "%uh %02um",
                      static_cast<unsigned>(seconds / 3600),
                      static_cast<unsigned>((seconds / 60) % 60));
    }
    return buffer;
}

std::string dateTime(int64_t unixSeconds)
{
    const time_t value = static_cast<time_t>(unixSeconds);
    struct tm parts {};
    if (gmtime_r(&value, &parts) == nullptr) {
        return "";
    }
    char buffer[32];
    std::strftime(buffer, sizeof(buffer), "%Y-%m-%d %H:%M", &parts);
    return buffer;
}

void RateMeter::reset()
{
    *this = RateMeter();
}

void RateMeter::sample(uint64_t milliseconds, uint64_t totalBytes)
{
    if (!started_ || totalBytes < lastBytes_ || milliseconds < lastTime_) {
        started_ = true;
        lastTime_ = milliseconds;
        lastBytes_ = totalBytes;
        return;
    }

    const uint64_t elapsed = milliseconds - lastTime_;
    if (elapsed < MinimumIntervalMs) {
        return;
    }

    const double instant = static_cast<double>(totalBytes - lastBytes_) *
                           1000.0 / static_cast<double>(elapsed);
    rate_ = hasRate_ ? rate_ + Smoothing * (instant - rate_) : instant;
    hasRate_ = true;
    lastTime_ = milliseconds;
    lastBytes_ = totalBytes;
}

std::string timeLeft(uint64_t seconds)
{
    char buffer[40];
    // Under ten seconds it counts down; then in fives, then in minutes.
    const uint64_t fives = (seconds + 4) / 5 * 5;
    if (seconds < 10) {
        std::snprintf(buffer, sizeof(buffer), "%u s left",
                      static_cast<unsigned>(std::max<uint64_t>(1, seconds)));
        return buffer;
    }
    if (fives < 60) {
        std::snprintf(buffer, sizeof(buffer), "%u s left",
                      static_cast<unsigned>(fives));
        return buffer;
    }
    const uint64_t minutes = std::max<uint64_t>(1, (seconds + 30) / 60);
    if (minutes < 60) {
        std::snprintf(buffer, sizeof(buffer), "%u min left",
                      static_cast<unsigned>(minutes));
    } else if (minutes % 60 == 0) {
        std::snprintf(buffer, sizeof(buffer), "%u h left",
                      static_cast<unsigned>(minutes / 60));
    } else {
        std::snprintf(buffer, sizeof(buffer), "%u h %u min left",
                      static_cast<unsigned>(minutes / 60),
                      static_cast<unsigned>(minutes % 60));
    }
    return buffer;
}

int64_t RateMeter::secondsLeft(uint64_t remainingBytes) const
{
    if (!hasRate_ || rate_ < 1.0) {
        return -1;
    }
    return static_cast<int64_t>(static_cast<double>(remainingBytes) / rate_ +
                                0.5);
}

} // namespace units
