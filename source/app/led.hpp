#pragma once

// The notification LED says how 3dsDL's work is going while the lid is
// closed (core/info_led has what it shows and when). It writes a pattern
// only when what it shows changes, so it never clears one the system set,
// and clears its own when the HOME Menu takes over or 3dsDL closes. Without
// access to mcu::HWC it does nothing.
namespace led {

void init();
void exit();

// Once a frame, on the main thread, with the lid as awake last read it.
void update(bool lidClosed);

// A job is running for as long as one exists. Any thread.
class Job {
public:
    Job();
    ~Job();
    Job(const Job &) = delete;
    Job &operator=(const Job &) = delete;
};
// The same, for work that starts and ends on calls of its own (the Receive
// server's requests). Any thread.
void working(bool starting);

// A job ended, well or not. Main thread.
void finished(bool ok);

// A question waits on the console for as long as one exists. Main thread.
class Asking {
public:
    Asking();
    ~Asking();
    Asking(const Asking &) = delete;
    Asking &operator=(const Asking &) = delete;
};

} // namespace led
