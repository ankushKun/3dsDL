#include "../check.hpp"

#include "info_led.hpp"

#include <algorithm>

namespace {

using infoled::Show;

constexpr size_t Red = 4;
constexpr size_t Green = 36;
constexpr size_t Blue = 68;

int level(const infoled::Pattern &bytes, size_t channel, size_t frame)
{
    return bytes[channel + frame];
}

int peak(const infoled::Pattern &bytes, size_t channel)
{
    return *std::max_element(bytes.begin() + channel, bytes.begin() + channel + 32);
}

int lowest(const infoled::Pattern &bytes, size_t channel)
{
    return *std::min_element(bytes.begin() + channel, bytes.begin() + channel + 32);
}

} // namespace

TEST("info led: milliseconds become the MCU's 512 Hz ticks, never 0")
{
    CHECK_EQ(static_cast<int>(infoled::ticks(0)), 1);
    CHECK_EQ(static_cast<int>(infoled::ticks(2)), 1);
    CHECK_EQ(static_cast<int>(infoled::ticks(94)), 47);
    CHECK_EQ(static_cast<int>(infoled::ticks(63)), 31);
    CHECK_EQ(static_cast<int>(infoled::ticks(1000)), 255);
}

TEST("info led: each state has a pattern of its own colour and rhythm")
{
    for (const Show show : {Show::Off, Show::Working, Show::Done, Show::Failed,
                            Show::Asking}) {
        // The MCU does not take a frame time of 0.
        CHECK(infoled::pattern(show)[0] != 0);
        CHECK_EQ(static_cast<int>(infoled::pattern(show)[3]), 0);
    }

    const infoled::Pattern off = infoled::pattern(Show::Off);
    CHECK_EQ(static_cast<int>(off[2]), 0xFF);
    CHECK(std::all_of(off.begin() + Red, off.end(), [](uint8_t value) { return value == 0; }));

    // Working breathes blue, faded frame to frame, and never goes dark.
    const infoled::Pattern working = infoled::pattern(Show::Working);
    CHECK_EQ(peak(working, Red), 0);
    CHECK_EQ(peak(working, Blue), 200);
    CHECK(lowest(working, Blue) > 0);
    CHECK(peak(working, Green) < peak(working, Blue));
    CHECK_EQ(static_cast<int>(working[1]), static_cast<int>(working[0]));
    CHECK_EQ(static_cast<int>(working[2]), 0);

    // Done pulses green twice, Failed blinks red three times; both end dark
    // and wait on that dark frame before starting again.
    const infoled::Pattern done = infoled::pattern(Show::Done);
    CHECK_EQ(peak(done, Red), 0);
    CHECK_EQ(peak(done, Blue), 0);
    CHECK(peak(done, Green) > 0);
    CHECK_EQ(level(done, Green, 31), 0);
    CHECK(done[2] > 0 && done[2] != 0xFF);
    int greenRuns = 0;
    for (size_t frame = 0; frame < 32; ++frame) {
        greenRuns += level(done, Green, frame) > 0 &&
                     (frame == 0 || level(done, Green, frame - 1) == 0);
    }
    CHECK_EQ(greenRuns, 2);

    const infoled::Pattern failed = infoled::pattern(Show::Failed);
    CHECK_EQ(peak(failed, Green), 0);
    CHECK_EQ(peak(failed, Blue), 0);
    CHECK_EQ(level(failed, Red, 31), 0);
    CHECK(failed[2] > 0 && failed[2] != 0xFF);
    int redRuns = 0;
    for (size_t frame = 0; frame < 32; ++frame) {
        redRuns += level(failed, Red, frame) > 0 &&
                   (frame == 0 || level(failed, Red, frame - 1) == 0);
    }
    CHECK_EQ(redRuns, 3);

    // Asking blinks amber, lit half the time, with no pause between.
    const infoled::Pattern asking = infoled::pattern(Show::Asking);
    CHECK(peak(asking, Red) > peak(asking, Green));
    CHECK(peak(asking, Green) > 0);
    CHECK_EQ(peak(asking, Blue), 0);
    CHECK_EQ(static_cast<int>(asking[2]), 0);
    int lit = 0;
    for (size_t frame = 0; frame < 32; ++frame) {
        lit += level(asking, Red, frame) > 0;
    }
    CHECK_EQ(lit, 16);
}

TEST("info led: shows only with the lid closed, a question first, then work")
{
    infoled::Indicator indicator;
    // Lid open: nothing, whatever is going on.
    CHECK(indicator.update(true, true, 0) == Show::Off);

    indicator.setLid(true);
    CHECK(indicator.update(false, false, 10) == Show::Working); // the linger
    CHECK(indicator.update(false, false, 5000) == Show::Off);
    CHECK(indicator.update(false, true, 6000) == Show::Working);
    CHECK(indicator.update(true, true, 6100) == Show::Asking);
    // Work lingers a moment after it ends, so quick requests do not flicker.
    CHECK(indicator.update(false, false, 6100 + infoled::WorkLingerMs - 1) ==
          Show::Working);
    CHECK(indicator.update(false, false, 6100 + infoled::WorkLingerMs) == Show::Off);
}

TEST("info led: how jobs went shows while the lid stays closed, worst first")
{
    infoled::Indicator indicator;
    // A job that ends with the lid open was seen on the screens.
    indicator.finished(false);
    indicator.setLid(true);
    CHECK(indicator.update(false, false, 10000) == Show::Off);

    indicator.finished(true);
    CHECK(indicator.update(false, false, 10001) == Show::Done);
    indicator.finished(false);
    CHECK(indicator.update(false, false, 10002) == Show::Failed);
    // A later success does not hide the failure.
    indicator.finished(true);
    CHECK(indicator.update(false, false, 10003) == Show::Failed);
    // More work, and a question, still come first.
    CHECK(indicator.update(false, true, 10004) == Show::Working);
    CHECK(indicator.update(true, false, 10005) == Show::Asking);
    CHECK(indicator.update(false, false, 20000) == Show::Failed);

    // Opening the lid clears it; closing it again starts afresh.
    indicator.setLid(false);
    CHECK(indicator.update(false, false, 20001) == Show::Off);
    indicator.setLid(true);
    CHECK(indicator.update(false, false, 20002) == Show::Off);
    indicator.finished(true);
    CHECK(indicator.update(false, false, 20003) == Show::Done);
}
