#pragma once

#include "cia.hpp"

#include <3ds.h>

#include <atomic>
#include <cstdint>
#include <string>

// Installs CIA files through the AM service, the way FBI did, and does the
// small jobs around it: seeds, removing an installed title and launching one.
// Runs on whichever thread calls it; tasks::install puts it on the worker.
namespace installer {

// Opens the AM service. 3dsDL still works without it, but cannot install.
void init();
void exit();
bool available();
// Why installing is unavailable, for the user.
std::string unavailableReason();

class Observer {
public:
    virtual ~Observer() = default;
    virtual bool cancelRequested() = 0;
    virtual void onProgress(uint64_t done, uint64_t total)
    {
        (void)done;
        (void)total;
    }
};

enum class Outcome { Installed, Failed, Cancelled };

struct Result {
    Outcome outcome = Outcome::Failed;
    ::Result code = 0;
    // AM would not take this version over the installed one: an older one,
    // or the same one again. Removing the installed title first gets past it.
    bool versionRefused = false;
    uint64_t bytes = 0;
    double seconds = 0.0;
    std::string message;
};

// path is an sdmc:/ path to a CIA whose title ID is titleId.
Result installCia(const std::string &path, uint64_t titleId,
                  Observer &observer);

// Installs a CIA as it downloads, without saving the file, for SD cards
// too full to hold it. Data must come in order from offset 0. The install
// starts once the title ID is known from the first few kilobytes.
class StreamInstall {
public:
    StreamInstall() = default;
    ~StreamInstall();
    StreamInstall(const StreamInstall &) = delete;
    StreamInstall &operator=(const StreamInstall &) = delete;

    // False when the data is not an installable CIA or the install failed;
    // the reason is in result().
    bool write(uint64_t offset, const void *data, size_t size);
    // Finishes the install after the last write.
    Result finish();
    // Abandons the install; nothing is changed.
    void cancel();
    // True once AM has the install. Any thread may ask.
    bool started() const { return started_; }
    const cia::Info &info() const { return stream_.info(); }
    const Result &result() const { return result_; }

private:
    bool begin();

    cia::Stream stream_;
    Handle target_ = 0;
    std::atomic<bool> started_ {false};
    uint64_t written_ = 0;
    u64 startTime_ = 0;
    Result result_;
};

FS_MediaType destination(uint64_t titleId);
// False when the title is not installed.
bool installedVersion(uint64_t titleId, uint16_t &version);
bool removeTitle(uint64_t titleId, std::string &error);
// Bytes the install will take, or -1 when AM cannot say.
int64_t requiredSpace(const std::string &path, uint64_t titleId);

// Adds the seed a title needs to start. It is looked for in
// sdmc:/3ds/3dsDL/seeds/<title id>.dat and FBI's sdmc:/fbi/seed/ first, then
// fetched from Nintendo. message says what happened. Setting stop ends the
// fetch early.
bool installSeed(uint64_t titleId, std::string &message,
                 const std::atomic<bool> *stop = nullptr);

// Adds a 16-byte seed for titleId, as from a seeddb.bin.
bool addSeed(uint64_t titleId, const uint8_t *seed, std::string &error);

// Leaves 3dsDL and starts an installed title.
bool launch(uint64_t titleId, std::string &error);

// A sentence for the user about an AM or FS failure.
std::string describe(::Result code);

} // namespace installer
