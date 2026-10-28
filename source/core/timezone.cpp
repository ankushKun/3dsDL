#include "timezone.hpp"

#include <cstdio>
#include <cstdlib>

namespace tz {

const std::vector<Zone> &zones()
{
    static const std::vector<Zone> all = {
        {-720, "Baker Island"},
        {-660, "American Samoa, Niue"},
        {-600, "Hawaii, Tahiti"},
        {-570, "Marquesas Islands"},
        {-540, "Alaska"},
        {-480, "Pacific Time (US, Canada)"},
        {-420, "Mountain Time (US, Canada)"},
        {-360, "Central Time (US, Canada), Mexico City"},
        {-300, "Eastern Time (US, Canada), Colombia, Peru"},
        {-240, "Atlantic Time, Venezuela, Bolivia"},
        {-210, "Newfoundland"},
        {-180, "Brazil, Argentina, Uruguay"},
        {-120, "South Georgia"},
        {-60, "Azores, Cape Verde"},
        {0, "UK, Ireland, Portugal, Iceland, Ghana"},
        {60, "Central Europe, Nigeria, Algeria"},
        {120, "Eastern Europe, Egypt, South Africa"},
        {180, "Moscow, Turkey, Saudi Arabia, Kenya"},
        {210, "Iran"},
        {240, "UAE, Oman, Azerbaijan"},
        {270, "Afghanistan"},
        {300, "Pakistan, Uzbekistan, Maldives"},
        {330, "India, Sri Lanka"},
        {345, "Nepal"},
        {360, "Bangladesh, Bhutan"},
        {390, "Myanmar"},
        {420, "Thailand, Vietnam, Jakarta"},
        {480, "China, Singapore, Philippines, Perth"},
        {525, "Eucla"},
        {540, "Japan, Korea"},
        {570, "Adelaide, Darwin"},
        {600, "Sydney, Brisbane, Guam"},
        {630, "Lord Howe Island"},
        {660, "Solomon Islands, New Caledonia"},
        {720, "New Zealand, Fiji"},
        {765, "Chatham Islands"},
        {780, "Tonga, Samoa"},
        {840, "Line Islands (Kiribati)"},
    };
    return all;
}

std::string formatOffset(int offsetMinutes)
{
    const int magnitude = std::abs(offsetMinutes);
    char buffer[16];
    std::snprintf(buffer, sizeof(buffer), "UTC%c%02d:%02d",
                  offsetMinutes < 0 ? '-' : '+', magnitude / 60,
                  magnitude % 60);
    return buffer;
}

bool isValidOffset(int offsetMinutes)
{
    return offsetMinutes >= MinOffsetMinutes &&
           offsetMinutes <= MaxOffsetMinutes && offsetMinutes % 15 == 0;
}

int zoneIndex(int offsetMinutes)
{
    const std::vector<Zone> &all = zones();
    for (size_t index = 0; index < all.size(); ++index) {
        if (all[index].offsetMinutes == offsetMinutes) {
            return static_cast<int>(index);
        }
    }
    return -1;
}

bool guessOffset(int64_t consoleUnixSeconds, int64_t utcUnixSeconds,
                 int &offsetMinutes)
{
    const int64_t difference = consoleUnixSeconds - utcUnixSeconds;
    // Round to the nearest quarter hour; real offsets are multiples of 15 min.
    const int64_t quarter = 15 * 60;
    const int64_t rounded =
        (difference >= 0 ? difference + quarter / 2
                         : difference - quarter / 2) / quarter * quarter;
    // A clock that is right except for the zone is within a few minutes of
    // that quarter hour; anything else means the clock itself is wrong.
    if (std::llabs(difference - rounded) > 5 * 60) {
        return false;
    }
    const int minutes = static_cast<int>(rounded / 60);
    if (zoneIndex(minutes) < 0) {
        return false;
    }
    offsetMinutes = minutes;
    return true;
}

std::string describeDrift(int64_t consoleUnixSeconds,
                          int64_t correctUnixSeconds)
{
    const int64_t difference = consoleUnixSeconds - correctUnixSeconds;
    const int64_t magnitude = std::llabs(difference);
    const char *direction = difference < 0 ? "behind" : "ahead";

    if (magnitude < 60) {
        return "correct to within a minute";
    }
    char buffer[64];
    if (magnitude < 3600) {
        std::snprintf(buffer, sizeof(buffer), "%lld minutes %s",
                      static_cast<long long>(magnitude / 60), direction);
    } else if (magnitude < 2 * 86400) {
        std::snprintf(buffer, sizeof(buffer), "%lld hours %lld minutes %s",
                      static_cast<long long>(magnitude / 3600),
                      static_cast<long long>((magnitude / 60) % 60),
                      direction);
    } else {
        std::snprintf(buffer, sizeof(buffer), "about %lld days %s",
                      static_cast<long long>((magnitude + 43200) / 86400),
                      direction);
    }
    return buffer;
}

int64_t unixSeconds(int year, int month, int day, int hour, int minute,
                    int second)
{
    // Days from 1970-01-01, counting years from March so that the leap day
    // comes last (Howard Hinnant's days_from_civil).
    const int64_t y = year - (month <= 2 ? 1 : 0);
    const int64_t era = (y >= 0 ? y : y - 399) / 400;
    const int64_t yearOfEra = y - era * 400;
    const int64_t dayOfYear =
        (153 * (month > 2 ? month - 3 : month + 9) + 2) / 5 + day - 1;
    const int64_t dayOfEra =
        yearOfEra * 365 + yearOfEra / 4 - yearOfEra / 100 + dayOfYear;
    const int64_t days = era * 146097 + dayOfEra - 719468;
    return days * 86400 + hour * 3600 + minute * 60 + second;
}

bool coversConsoleTime(int64_t notBefore, int64_t notAfter,
                       int64_t consoleUnixSeconds)
{
    const int64_t earliest = consoleUnixSeconds - MaxOffsetMinutes * 60LL;
    const int64_t latest = consoleUnixSeconds - MinOffsetMinutes * 60LL;
    return notBefore <= notAfter && notBefore <= latest &&
           notAfter >= earliest;
}

} // namespace tz
