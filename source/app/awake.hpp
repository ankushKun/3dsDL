#pragma once

#include "awake_policy.hpp"

// Keeps the console out of sleep and its Wi-Fi connected while held (main
// holds it for as long as 3dsDL runs), so closing the lid leaves work going. The screens go dark while the lid
// is closed and light again when it opens. Main thread only, apart from
// lightScreens.
namespace awake {

void init();
void exit();

// Holds the console awake for as long as it exists, and for LingerMs after
// the last one ends.
class Hold {
public:
    Hold();
    ~Hold();
    Hold(const Hold &) = delete;
    Hold &operator=(const Hold &) = delete;
};

// Once a frame: follows the lid while the console is held awake, and lets it
// sleep once the linger is over (at once if the lid is closed).
void update();

// Whether the lid was closed when last read, for the notification LED.
bool isLidClosed();

// Lights the screens again if 3dsDL turned them off, for an error screen.
// Safe from any thread.
void lightScreens();

} // namespace awake
