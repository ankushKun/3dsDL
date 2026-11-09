#include "info_led.hpp"

#include <cmath>

namespace infoled {

namespace {

constexpr size_t Frames = 32;
constexpr size_t Red = 4;
constexpr size_t Green = Red + Frames;
constexpr size_t Blue = Green + Frames;

struct Colour {
    uint8_t red;
    uint8_t green;
    uint8_t blue;
};

// Each colour as the LED shows it, not as the screens do: its blue is far
// stronger than its red, so 3dsDL's blue needs half as much green to read
// as blue, and amber needs little green to stay clear of yellow.
constexpr Colour WorkingBlue = {0, 100, 200};
constexpr Colour DoneGreen = {0, 220, 0};
constexpr Colour FailedRed = {255, 0, 0};
constexpr Colour AskingAmber = {255, 100, 0};

// A pattern of one colour whose frames are lit by level (0 to 1).
template <typename Level>
Pattern make(uint8_t frameTicks, uint8_t fadeTicks, uint8_t lastRepeats,
             Colour colour, Level level)
{
    Pattern bytes {};
    bytes[0] = frameTicks;
    bytes[1] = fadeTicks;
    bytes[2] = lastRepeats;
    for (size_t frame = 0; frame < Frames; ++frame) {
        const double lit = level(frame);
        bytes[Red + frame] = static_cast<uint8_t>(std::lround(colour.red * lit));
        bytes[Green + frame] = static_cast<uint8_t>(std::lround(colour.green * lit));
        bytes[Blue + frame] = static_cast<uint8_t>(std::lround(colour.blue * lit));
    }
    return bytes;
}

} // namespace

uint8_t ticks(uint32_t ms)
{
    // 512 Hz, and never 0, which the MCU does not take.
    const uint32_t count = 512u * ms / 1000u;
    const uint32_t value = count < 2 ? 1 : count - 1;
    return static_cast<uint8_t>(value > 255 ? 255 : value);
}

Pattern pattern(Show show)
{
    switch (show) {
    case Show::Working: {
        // One slow breath every 3 s, never quite dark, faded frame to frame.
        const uint8_t frame = ticks(94);
        return make(frame, frame, 0, WorkingBlue, [](size_t index) {
            const double pi = std::acos(-1.0);
            const double wave = (1 - std::cos(2 * pi * index / Frames)) / 2;
            return 0.06 + 0.94 * wave;
        });
    }
    case Show::Done:
        // Two soft pulses, then dark until the 3.5 s cycle starts again.
        return make(ticks(63), ticks(31), 24, DoneGreen, [](size_t index) {
            return index == 1 || index == 2 || index == 5 || index == 6 ? 1.0 : 0.0;
        });
    case Show::Failed:
        // Three short blinks, then dark until the 2.5 s cycle starts again:
        // not the steady blinking of a battery about to run out, which also
        // turns the power LED red.
        return make(ticks(63), ticks(8), 8, FailedRed, [](size_t index) {
            return index < 12 && index % 4 < 2 ? 1.0 : 0.0;
        });
    case Show::Asking:
        // On half a second, off half a second, for as long as it waits.
        return make(ticks(63), ticks(12), 0, AskingAmber, [](size_t index) {
            return index % 16 < 8 ? 1.0 : 0.0;
        });
    case Show::Off:
        break;
    }
    // Dark, held: the frame time only has to be above 0.
    return make(1, 0, 0xFF, Colour {0, 0, 0}, [](size_t) { return 0.0; });
}

void Indicator::setLid(bool closed)
{
    if (!closed) {
        outcome_ = Outcome::None;
    }
    lidClosed_ = closed;
}

void Indicator::finished(bool ok)
{
    if (!lidClosed_) {
        return;
    }
    if (!ok) {
        outcome_ = Outcome::Failed;
    } else if (outcome_ == Outcome::None) {
        outcome_ = Outcome::Done;
    }
}

Show Indicator::update(bool asking, bool working, uint64_t nowMs)
{
    if (working) {
        worked_ = true;
        lastWorkMs_ = nowMs;
    }
    if (!lidClosed_) {
        return Show::Off;
    }
    if (asking) {
        return Show::Asking;
    }
    if (working || (worked_ && nowMs - lastWorkMs_ < WorkLingerMs)) {
        return Show::Working;
    }
    switch (outcome_) {
    case Outcome::Done:
        return Show::Done;
    case Outcome::Failed:
        return Show::Failed;
    case Outcome::None:
        break;
    }
    return Show::Off;
}

} // namespace infoled
