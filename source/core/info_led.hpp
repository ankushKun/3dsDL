#pragma once

#include <array>
#include <cstdint>

// The notification (info) LED: what it shows for 3dsDL's work while the lid
// is closed, and the patterns the MCU plays for each. Portable, so the
// choices and the bytes are host-tested; app/led sends them.
namespace infoled {

// The 100 bytes MCUHWC_SetInfoLedPattern takes. The MCU steps through 32
// frames, each lasting byte 0 ticks of its 512 Hz clock (1 to 255), fading
// into the next over byte 1 ticks (0 jumps). Byte 2 repeats the last frame
// that many more times before the pattern starts again (0xFF holds it for
// good). Byte 3 is padding, then 32 red, 32 green and 32 blue levels.
using Pattern = std::array<uint8_t, 100>;

// Milliseconds as the MCU's ticks, as libctru's mcuTicksFromMs counts them.
uint8_t ticks(uint32_t ms);

enum class Show {
    Off,     // nothing of 3dsDL's
    Working, // 3dsDL blue, breathing slowly: still going
    Done,    // green, two soft pulses then a pause: it all went through
    Failed,  // red, three short blinks then a pause: something did not
    Asking,  // amber, blinking once a second: a question waits on the console
};

Pattern pattern(Show show);

// How long Working outlasts the last job, so the page's quick requests in
// Receive read as one stretch of work rather than flicker.
constexpr uint64_t WorkLingerMs = 1500;

// What the LED should show. Main thread only.
class Indicator {
public:
    // The lid as last read. Opening it clears how the jobs went: the screens
    // say that now.
    void setLid(bool closed);
    // A job ended. Counted only while the lid is closed, when nobody can see
    // the screens, and a failure stays over later successes until the lid
    // opens, so a batch left alone says if any of it went wrong.
    void finished(bool ok);
    // asking: a question waits; working: a job runs. A question comes first,
    // then work, then how the jobs went; with the lid open, nothing.
    Show update(bool asking, bool working, uint64_t nowMs);

private:
    enum class Outcome { None, Done, Failed };
    bool lidClosed_ = false;
    Outcome outcome_ = Outcome::None;
    bool worked_ = false; // working was seen, so lastWorkMs_ means something
    uint64_t lastWorkMs_ = 0;
};

} // namespace infoled
