#include "../check.hpp"

#include "ntp.hpp"
#include "timezone.hpp"

#include <cstring>

namespace {

void reply(uint8_t packet[ntp::PacketSize], uint64_t origin, uint32_t seconds,
           uint32_t fraction, uint8_t stratum = 2, uint8_t leap = 0)
{
    std::memset(packet, 0, ntp::PacketSize);
    packet[0] = static_cast<uint8_t>((leap << 6) | (4 << 3) | 4);
    packet[1] = stratum;
    for (int index = 0; index < 8; ++index) {
        packet[24 + index] = static_cast<uint8_t>(origin >> (56 - 8 * index));
    }
    for (int index = 0; index < 4; ++index) {
        packet[40 + index] = static_cast<uint8_t>(seconds >> (24 - 8 * index));
        packet[44 + index] = static_cast<uint8_t>(fraction >> (24 - 8 * index));
    }
}

} // namespace

TEST("ntp: request layout")
{
    uint8_t packet[ntp::PacketSize];
    ntp::buildRequest(packet, 0x0102030405060708ULL);
    CHECK_EQ(static_cast<int>(packet[0]), 0x23); // version 4, client mode
    CHECK_EQ(static_cast<int>(packet[40]), 1);
    CHECK_EQ(static_cast<int>(packet[47]), 8);
}

TEST("ntp: response parsing and validation")
{
    uint8_t packet[ntp::PacketSize];
    int64_t unixMs = 0;
    std::string error;

    // 2026-10-12 06:00:00.25 UTC
    reply(packet, 42, static_cast<uint32_t>(1791784800ULL + 2208988800ULL),
          0x40000000);
    CHECK(ntp::parseResponse(packet, sizeof(packet), 42, unixMs, error));
    CHECK_EQ(unixMs, 1791784800LL * 1000 + 250);

    CHECK(!ntp::parseResponse(packet, sizeof(packet), 43, unixMs, error));
    CHECK(!ntp::parseResponse(packet, 47, 42, unixMs, error));

    reply(packet, 42, 3900000000u, 0, 0);
    CHECK(!ntp::parseResponse(packet, sizeof(packet), 42, unixMs, error));
    reply(packet, 42, 3900000000u, 0, 2, 3);
    CHECK(!ntp::parseResponse(packet, sizeof(packet), 42, unixMs, error));

    // After the 2036 rollover small second counts belong to the next era.
    reply(packet, 42, 100, 0);
    CHECK(ntp::parseResponse(packet, sizeof(packet), 42, unixMs, error));
    CHECK_EQ(unixMs, (4294967296LL + 100 - 2208988800LL) * 1000);
}

TEST("tz: offsets, zones and guesses")
{
    CHECK_EQ(tz::formatOffset(330), std::string("UTC+05:30"));
    CHECK_EQ(tz::formatOffset(-210), std::string("UTC-03:30"));
    CHECK_EQ(tz::formatOffset(0), std::string("UTC+00:00"));
    CHECK(tz::isValidOffset(345));
    CHECK(!tz::isValidOffset(7));
    CHECK(!tz::isValidOffset(900));
    CHECK(tz::zoneIndex(330) >= 0);
    CHECK_EQ(tz::zoneIndex(15), -1);

    const int64_t utc = 1791784800;
    int offset = 0;
    CHECK(tz::guessOffset(utc + 330 * 60 + 40, utc, offset));
    CHECK_EQ(offset, 330);
    CHECK(tz::guessOffset(utc - 300 * 60 - 90, utc, offset));
    CHECK_EQ(offset, -300);
    // A clock that is off by a year, or by an odd number of minutes, is
    // simply wrong rather than in another time zone.
    CHECK(!tz::guessOffset(utc - 365 * 86400, utc, offset));
    CHECK(!tz::guessOffset(utc + 37 * 60, utc, offset));
}

TEST("tz: drift descriptions")
{
    CHECK_EQ(tz::describeDrift(1000, 1030),
             std::string("correct to within a minute"));
    CHECK_EQ(tz::describeDrift(1000, 1000 + 600),
             std::string("10 minutes behind"));
    CHECK_EQ(tz::describeDrift(1000 + 5 * 3600 + 120, 1000),
             std::string("5 hours 2 minutes ahead"));
    CHECK_EQ(tz::describeDrift(0, 365 * 86400LL),
             std::string("about 365 days behind"));
}

TEST("tz: calendar times to seconds")
{
    CHECK_EQ(tz::unixSeconds(1970, 1, 1, 0, 0, 0), 0LL);
    CHECK_EQ(tz::unixSeconds(1969, 12, 31, 23, 59, 59), -1LL);
    CHECK_EQ(tz::unixSeconds(2000, 1, 1, 0, 0, 0), tz::UnixToY2kMs / 1000);
    CHECK_EQ(tz::unixSeconds(2024, 2, 29, 0, 0, 0), 1709164800LL);
    CHECK_EQ(tz::unixSeconds(2026, 10, 12, 6, 0, 0), 1791784800LL);
    CHECK_EQ(tz::unixSeconds(2050, 1, 1, 0, 0, 0), 2524608000LL);
}

TEST("tz: certificates against a local clock")
{
    // api.github.com's certificate from 2026-10-28, checked by a console in
    // US Eastern time at 20:22 the evening before, when it was already in use.
    const int64_t from = tz::unixSeconds(2026, 10, 28, 0, 0, 0);
    const int64_t to = tz::unixSeconds(2027, 1, 25, 23, 59, 59);
    CHECK(tz::coversConsoleTime(from, to,
                                tz::unixSeconds(2026, 10, 27, 20, 22, 0)));
    // UTC-12 is the furthest west; a clock further behind is simply wrong.
    CHECK(tz::coversConsoleTime(from, to, from - 12 * 3600));
    CHECK(!tz::coversConsoleTime(from, to, from - 12 * 3600 - 1));
    // UTC+14 is the furthest east, for a certificate that has just expired.
    CHECK(tz::coversConsoleTime(from, to, to + 14 * 3600));
    CHECK(!tz::coversConsoleTime(from, to, to + 14 * 3600 + 1));
    CHECK(!tz::coversConsoleTime(from, to,
                                 tz::unixSeconds(2010, 1, 1, 0, 0, 0)));
    // Dates the wrong way round are never valid.
    CHECK(!tz::coversConsoleTime(to, from, from));
}
