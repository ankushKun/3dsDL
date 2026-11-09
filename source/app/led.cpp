#include "led.hpp"

#include "info_led.hpp"
#include "logging.hpp"

#include <3ds.h>

#include <atomic>
#include <cstring>

namespace led {

namespace {

bool available = false;
infoled::Indicator indicator;
// What the LED shows of 3dsDL's. Off at the start, so a pattern the system
// set (a SpotPass notice) stays until there is something of ours to show.
infoled::Show written = infoled::Show::Off;
std::atomic<int> jobs {0};
int questions = 0;
aptHookCookie cookie;

void show(infoled::Show wanted)
{
    if (!available || wanted == written) {
        return;
    }
    const infoled::Pattern bytes = infoled::pattern(wanted);
    // libctru's struct is the MCU's 100 bytes; its older field names
    // (delay, smoothing, loopDelay) describe them wrongly.
    InfoLedPattern pattern;
    static_assert(sizeof(pattern) == sizeof(bytes), "the MCU takes 100 bytes");
    std::memcpy(&pattern, bytes.data(), bytes.size());
    if (R_SUCCEEDED(MCUHWC_SetInfoLedPattern(&pattern))) {
        written = wanted;
    }
}

// The HOME Menu, sleep and closing take the LED back from 3dsDL.
void onApt(APT_HookType hook, void *)
{
    if (hook == APTHOOK_ONSUSPEND || hook == APTHOOK_ONSLEEP ||
        hook == APTHOOK_ONEXIT) {
        show(infoled::Show::Off);
    }
}

} // namespace

void init()
{
    available = R_SUCCEEDED(mcuHwcInit());
    if (!available) {
        logging::info("led.unavailable");
    }
    aptHook(&cookie, onApt, nullptr);
}

void exit()
{
    aptUnhook(&cookie);
    show(infoled::Show::Off);
    if (available) {
        mcuHwcExit();
        available = false;
    }
}

void update(bool lidClosed)
{
    indicator.setLid(lidClosed);
    show(indicator.update(questions > 0, jobs.load() > 0, osGetTime()));
}

Job::Job()
{
    working(true);
}

Job::~Job()
{
    working(false);
}

void working(bool starting)
{
    jobs.fetch_add(starting ? 1 : -1);
}

void finished(bool ok)
{
    indicator.finished(ok);
}

Asking::Asking()
{
    ++questions;
}

Asking::~Asking()
{
    --questions;
}

} // namespace led
