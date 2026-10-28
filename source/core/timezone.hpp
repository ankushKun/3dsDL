#pragma once

#include <cstdint>
#include <string>
#include <vector>

// The 3DS clock has no notion of time zones: it simply holds local time. To
// set it from the internet, 3dsDL adds the UTC offset the user picked.
namespace tz {

constexpr int MinOffsetMinutes = -12 * 60;
constexpr int MaxOffsetMinutes = 14 * 60;
// Milliseconds between 1970-01-01 and 2000-01-01, the 3DS clock's epoch.
constexpr int64_t UnixToY2kMs = 946684800000LL;

struct Zone {
    int offsetMinutes;
    const char *places;
};

// Standard UTC offsets in use around the world, west to east.
const std::vector<Zone> &zones();

// "UTC+05:30", "UTC-03:30", "UTC+00:00"
std::string formatOffset(int offsetMinutes);
bool isValidOffset(int offsetMinutes);
// Index of the zone with this offset in zones(), or -1.
int zoneIndex(int offsetMinutes);

// The UTC offset the console clock seems to be set for, judging by how far it
// is from real UTC, rounded to the nearest listed zone. Returns false when the
// difference is not a plausible offset, i.e. the clock is simply wrong.
bool guessOffset(int64_t consoleUnixSeconds, int64_t utcUnixSeconds,
                 int &offsetMinutes);

// Human description of how far the console clock is off, e.g.
// "about 365 days behind" or "correct to within a minute".
std::string describeDrift(int64_t consoleUnixSeconds,
                          int64_t correctUnixSeconds);

// Seconds since 1970 for a UTC calendar time; month and day count from 1.
int64_t unixSeconds(int year, int month, int day, int hour, int minute,
                    int second);

// TLS reads the console clock as UTC, but it holds local time, so real UTC
// is anywhere from 14 hours behind it to 12 hours ahead. True when a
// certificate valid from notBefore to notAfter (UTC) is valid at some moment
// in that range.
bool coversConsoleTime(int64_t notBefore, int64_t notAfter,
                       int64_t consoleUnixSeconds);

} // namespace tz
