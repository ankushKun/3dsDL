#pragma once

#include "awake_policy.hpp"

// Keeps the console out of sleep and its Wi-Fi connected while 3dsDL works,
// so closing the lid leaves the work going. The screens go dark while the lid
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

// Lights the screens again if 3dsDL turned them off, for an error screen.
// Safe from any thread.
void lightScreens();

} // namespace awake
