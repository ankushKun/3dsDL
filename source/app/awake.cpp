#include "awake.hpp"

#include "crash.hpp"
#include "logging.hpp"
#include "platform.hpp"

#include <3ds.h>

#include <algorithm>
#include <atomic>
#include <cstdio>

namespace awake {
namespace {

// Reading the lid is a request to PTM, so it is read this often, and only
// while the console is held awake.
constexpr uint64_t LidEveryMs = 250;
// An NDM call taking longer than this is logged once.
constexpr uint64_t NdmStuckMs = 5000;
// How long sleep waits for NDM to let go of Wi-Fi before going ahead.
constexpr uint64_t ReleaseWaitMs = 3000;

Policy policy;
bool sleepBlocked = false; // what libctru was last told
bool ptm = false;
bool ndm = false;
bool lcd = false; // gsp::Lcd, opened the first time the screens go dark
std::atomic<bool> dark {false};
bool lidClosed = false;
uint64_t lidReadAt = 0;
uint64_t closedAt = 0;
aptHookCookie cookie {};

// The system clock moves when the console clock is set; ticks do not.
uint64_t nowMs()
{
    return static_cast<uint64_t>(svcGetSystemTick() / CPU_TICKS_PER_MSEC);
}

void light(bool on)
{
    if (on != dark.load()) {
        return;
    }
    if (!lcd) {
        if (on) {
            return;
        }
        lcd = R_SUCCEEDED(gspLcdInit());
        if (!lcd) {
            return; // no screen control (a .3dsx without access): they stay lit
        }
    }
    if (R_SUCCEEDED(on ? GSPLCD_PowerOnAllBacklights()
                       : GSPLCD_PowerOffAllBacklights())) {
        dark = !on;
    }
}

// Closing the lid makes NDM drop Wi-Fi for StreetPass even while sleep is
// refused. Holding its infrastructure state keeps the connection, as
// Rosalina and ftpd do. Only while connected: there is nothing to keep
// otherwise.
//
// NDM's calls run on a thread of their own and the main thread only says
// which state it wants, so a slow or stuck call never holds up the screen
// or the HOME button. One that takes NdmStuckMs is logged.
std::atomic<bool> wantNetwork {false};
std::atomic<bool> networkHeld {false};
std::atomic<uint64_t> ndmCallSince {0}; // the call under way, or 0
std::atomic<bool> ndmReleasing {false}; // which call that is
std::atomic<bool> ndmQuit {false};
LightEvent ndmWake;
Thread ndmThread = nullptr;
bool ndmStuckLogged = false;
uint64_t releaseAskedAt = 0;

void logNdmFailure(const char *step, Result result)
{
    char code[16];
    std::snprintf(code, sizeof(code), "0x%08lX",
                  static_cast<unsigned long>(result));
    logging::warn("awake.network", {{"step", step}, {"result", code}});
}

// Moves NDM one step toward the state wanted. Each change of mind signals
// ndmWake again, so the next pass catches up.
void settleNetwork()
{
    const bool want = wantNetwork.load();
    if (want == networkHeld.load() ||
        (want && platform::wifiStatus() != platform::Wifi::Connected)) {
        return;
    }
    ndmReleasing = !want;
    ndmCallSince = std::max<uint64_t>(1, nowMs());
    if (want) {
        Result result =
            NDMU_EnterExclusiveState(NDM_EXCLUSIVE_STATE_INFRASTRUCTURE);
        if (R_SUCCEEDED(result)) {
            result = NDMU_LockState();
            if (R_FAILED(result)) {
                NDMU_LeaveExclusiveState();
            }
        }
        networkHeld = R_SUCCEEDED(result);
        if (R_FAILED(result)) {
            logNdmFailure("hold", result);
        }
    } else {
        NDMU_UnlockState();
        const Result result = NDMU_LeaveExclusiveState();
        networkHeld = false;
        if (R_FAILED(result)) {
            logNdmFailure("release", result);
        }
    }
    ndmCallSince = 0;
}

void ndmMain(void *)
{
    crash::ThreadName name("ndm");
    do {
        LightEvent_Wait(&ndmWake);
        settleNetwork();
    } while (!ndmQuit.load());
}

void holdNetwork()
{
    if (!ndm) {
        return;
    }
    if (ndmThread == nullptr) {
        s32 priority = 0x30;
        svcGetThreadPriority(&priority, CUR_THREAD_HANDLE);
        ndmThread = threadCreate(ndmMain, nullptr, 32 * 1024,
                                 std::min(0x3f, static_cast<int>(priority) + 1),
                                 -2, false);
        if (ndmThread == nullptr) {
            return;
        }
    }
    wantNetwork = true;
    LightEvent_Signal(&ndmWake);
}

void releaseNetwork()
{
    if (ndmThread != nullptr && wantNetwork.load()) {
        wantNetwork = false;
        LightEvent_Signal(&ndmWake);
    }
}

void onApt(APT_HookType hook, void *)
{
    if (hook == APTHOOK_ONSUSPEND || hook == APTHOOK_ONSLEEP) {
        light(true); // the HOME Menu and waking up need the screens
    } else if (hook == APTHOOK_ONRESTORE || hook == APTHOOK_ONWAKEUP) {
        lidReadAt = 0; // read it again on the next frame
    }
}

} // namespace

void init()
{
    ptm = R_SUCCEEDED(ptmuInit());
    ndm = R_SUCCEEDED(ndmuInit());
    LightEvent_Init(&ndmWake, RESET_ONESHOT);
    aptHook(&cookie, onApt, nullptr);
}

void exit()
{
    aptUnhook(&cookie);
    light(true);
    if (lcd) {
        gspLcdExit();
        lcd = false;
    }
    releaseNetwork();
    if (ndmThread != nullptr) {
        ndmQuit = true;
        LightEvent_Signal(&ndmWake);
        // A stuck NDM call is left behind rather than holding up closing.
        // A timeout is not 0, though R_SUCCEEDED would take it.
        if (threadJoin(ndmThread, 2000000000ULL) == 0) {
            threadFree(ndmThread);
            ndmThread = nullptr;
        }
    }
    if (sleepBlocked) {
        aptSetSleepAllowed(true);
        sleepBlocked = false;
    }
    if (ndm && ndmThread == nullptr) {
        ndmuExit();
        ndm = false;
    }
    if (ptm) {
        ptmuExit();
        ptm = false;
    }
}

Hold::Hold()
{
    policy.hold();
    if (!sleepBlocked) {
        aptSetSleepAllowed(false);
        sleepBlocked = true;
    }
    holdNetwork();
}

Hold::~Hold()
{
    // Sleep stays blocked through the linger; update() ends it.
    policy.release(nowMs());
}

void update()
{
    const uint64_t now = nowMs();
    const uint64_t since = ndmCallSince.load();
    if (since == 0) {
        ndmStuckLogged = false;
    } else if (!ndmStuckLogged && now - since >= NdmStuckMs) {
        ndmStuckLogged = true;
        logging::warn("awake.network",
                      {{"stuck", ndmReleasing.load() ? "release" : "hold"},
                       {"ms", now - since}});
    }
    if (!sleepBlocked) {
        return;
    }
    if (policy.sleepAllowed(now)) {
        // Lit before it sleeps, so it wakes up lit.
        light(true);
        // NDM lets go of Wi-Fi first, so the console does not sleep holding
        // it, unless NDM takes longer than ReleaseWaitMs.
        if (wantNetwork.load()) {
            releaseNetwork();
            releaseAskedAt = now;
        }
        if (networkHeld.load() && now - releaseAskedAt < ReleaseWaitMs) {
            return;
        }
        aptSetSleepAllowed(true);
        sleepBlocked = false;
        if (lidClosed) {
            logging::info("lid.sleep", {{"ms", now - closedAt}});
        }
        lidClosed = false;
        return;
    }
    if (!ptm || now - lidReadAt < LidEveryMs) {
        return;
    }
    lidReadAt = now;
    u8 shell = 1;
    if (R_FAILED(PTMU_GetShellState(&shell))) {
        return;
    }
    const bool closed = shell == 0;
    if (closed != lidClosed) {
        lidClosed = closed;
        if (closed) {
            closedAt = now;
            logging::info("lid.closed", {{"working", policy.held()},
                                         {"network", networkHeld.load()}});
        } else {
            logging::info("lid.opened", {{"ms", now - closedAt}});
        }
    }
    light(!closed);
}

void lightScreens()
{
    if (dark.load() && lcd) {
        GSPLCD_PowerOnAllBacklights();
    }
}

} // namespace awake
