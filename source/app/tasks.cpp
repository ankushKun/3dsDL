#include "tasks.hpp"

#include "app_util.hpp"
#include "awake.hpp"
#include "cia.hpp"
#include "crash.hpp"
#include "logging.hpp"
#include "sound.hpp"
#include "ui.hpp"
#include "units.hpp"
#include "url.hpp"

#include <3ds.h>

#include <algorithm>
#include <atomic>
#include <unordered_set>
#include <utility>

namespace tasks {
namespace {

// curl and mbedTLS need a generous stack for the TLS handshake.
constexpr size_t WorkerStackSize = 256 * 1024;

class Guard {
public:
    explicit Guard(LightLock &lock) : lock_(lock) { LightLock_Lock(&lock_); }
    ~Guard() { LightLock_Unlock(&lock_); }
    Guard(const Guard &) = delete;
    Guard &operator=(const Guard &) = delete;

private:
    LightLock &lock_;
};

// The worker runs one step below the main thread's priority so drawing and
// the cancel button stay responsive even while TLS keeps the CPU busy.
int workerPriority()
{
    s32 priority = 0x30;
    svcGetThreadPriority(&priority, CUR_THREAD_HANDLE);
    return std::min(0x3f, static_cast<int>(priority) + 1);
}

// On a New 3DS the worker gets the third core, which nothing else uses, so
// TLS and inflating never wait for drawing. Elsewhere, or when the system
// refuses, it shares the app core with the main thread.
struct Named {
    ThreadFunc entry;
    void *argument;
    const char *name;
};

void runNamed(void *argument)
{
    const Named named = *static_cast<Named *>(argument);
    delete static_cast<Named *>(argument);
    crash::ThreadName name(named.name);
    named.entry(named.argument);
}

Thread startWorkerThread(ThreadFunc entry, void *argument, const char *name)
{
    Named *named = new Named {entry, argument, name};
    bool isNew3ds = false;
    if (R_SUCCEEDED(APT_CheckNew3DS(&isNew3ds)) && isNew3ds) {
        if (Thread thread = threadCreate(runNamed, named, WorkerStackSize,
                                         workerPriority(), 2, false)) {
            return thread;
        }
    }
    Thread thread = threadCreate(runNamed, named, WorkerStackSize,
                                 workerPriority(), -2, false);
    if (thread == nullptr) {
        delete named;
    }
    return thread;
}

bool confirmStop(const std::string &what)
{
    ui::Dialog dialog;
    dialog.header = "cancel";
    dialog.title = "Stop " + what + "?";
    dialog.tone = ui::Tone::Warning;
    dialog.buttons = {"Keep Going", "Stop"};
    dialog.dangerous = 1;
    dialog.cancelButton = 0;
    return ui::choose(dialog) == 1;
}

bool wantsCancel(const ui::Input &input)
{
    return (input.down & KEY_B) != 0 ||
           input.tapped(ui::ProgressCancelButton);
}

// "done", "stopped" or "failed" after an event's area, failures as warnings
// (the error the user sees is logged on its own).
void logOutcome(const std::string &area, bool ok, bool cancelled,
                eventlog::Fields fields)
{
    logging::log(ok || cancelled ? eventlog::Level::Info : eventlog::Level::Warn,
                 area + (ok ? ".done" : cancelled ? ".stopped" : ".failed"),
                 fields);
}

std::string capitalised(std::string value)
{
    if (!value.empty() && value[0] >= 'a' && value[0] <= 'z') {
        value[0] = static_cast<char>(value[0] - 'a' + 'A');
    }
    return value;
}

struct DownloadJob : net::Observer {
    const net::Request *request = nullptr;
    net::Result result;
    std::atomic<bool> cancel {false};
    std::atomic<bool> closing {false};
    std::atomic<bool> finished {false};

    LightLock lock;
    net::Phase phase = net::Phase::Connecting;
    std::string filename;
    uint64_t received = 0;
    uint64_t total = 0;
    uint64_t resumedFrom = 0;
    int connections = 0;
    // Set for a direct install: true once AM has the data.
    std::function<bool()> installStarted;

    DownloadJob() { LightLock_Init(&lock); }

    bool stoppable() const { return !installStarted || !installStarted(); }
    // A Stop pressed just before the install started is not carried out.
    bool cancelRequested() override
    {
        return cancel.load() && (closing.load() || stoppable());
    }
    bool keepPartialOnCancel() override { return closing.load(); }
    void onPhase(net::Phase value) override
    {
        Guard guard(lock);
        phase = value;
        if (value == net::Phase::Restarting) {
            resumedFrom = 0;
            received = 0;
            total = 0;
        }
    }
    void onFilename(const std::string &name) override
    {
        Guard guard(lock);
        filename = name;
    }
    void onProgress(uint64_t now, uint64_t expected) override
    {
        Guard guard(lock);
        received = now;
        total = expected;
    }
    void onResumed(uint64_t offset) override
    {
        Guard guard(lock);
        resumedFrom = offset;
    }
    void onConnections(int count) override
    {
        Guard guard(lock);
        connections = count;
    }
};

void runDownload(void *argument)
{
    DownloadJob &job = *static_cast<DownloadJob *>(argument);
    job.result = net::download(*job.request, job);
    job.finished = true;
}

struct ExtractJob : archives::Observer {
    std::string path;
    const archives::Listing *listing = nullptr;
    std::string folder;
    archives::Options options;
    archives::Result result;
    std::atomic<bool> cancel {false};
    std::atomic<bool> closing {false};
    std::atomic<bool> finished {false};

    LightLock lock;
    archives::Progress progress;

    ExtractJob() { LightLock_Init(&lock); }

    bool stoppable() const { return true; }
    bool cancelRequested() override { return cancel.load(); }
    void onProgress(const archives::Progress &now) override
    {
        Guard guard(lock);
        progress = now;
    }
};

void runExtract(void *argument)
{
    ExtractJob &job = *static_cast<ExtractJob *>(argument);
    job.result = archives::extract(job.path, *job.listing, job.folder,
                                   job.options, job);
    job.finished = true;
}

struct InstallJob : installer::Observer {
    std::string path;
    uint64_t titleId = 0;
    bool commit = true;
    installer::Result result;
    std::atomic<bool> cancel {false};
    std::atomic<bool> closing {false};
    std::atomic<bool> finished {false};

    LightLock lock;
    uint64_t done = 0;
    uint64_t total = 0;

    InstallJob() { LightLock_Init(&lock); }

    // Only closing 3dsDL cancels an install.
    bool stoppable() const { return false; }
    bool cancelRequested() override { return cancel.load(); }
    void onProgress(uint64_t now, uint64_t expected) override
    {
        Guard guard(lock);
        done = now;
        total = expected;
    }
};

void runInstall(void *argument)
{
    InstallJob &job = *static_cast<InstallJob *>(argument);
    job.result = installer::installCia(job.path, job.titleId, job, job.commit);
    job.finished = true;
}

struct FileJob : fileops::Observer {
    const std::function<fileops::Result(fileops::Observer &)> *work = nullptr;
    fileops::Result result;
    std::atomic<bool> cancel {false};
    std::atomic<bool> closing {false};
    std::atomic<bool> finished {false};

    LightLock lock;
    uint64_t done = 0;
    uint64_t total = 0;
    std::string current;

    FileJob() { LightLock_Init(&lock); }

    bool stoppable() const { return true; }
    bool cancelRequested() override { return cancel.load(); }
    void onProgress(uint64_t now, uint64_t expected,
                    const std::string &name) override
    {
        Guard guard(lock);
        done = now;
        total = expected;
        current = name;
    }
};

void runFileJob(void *argument)
{
    FileJob &job = *static_cast<FileJob *>(argument);
    job.result = (*job.work)(job);
    job.finished = true;
}

// Waits for the worker while drawing frames with draw(). Returns once the
// worker has finished, having asked it to stop if the user cancelled. While
// the job is not stoppable, HOME is held back too, as closing 3dsDL from the
// HOME Menu would stop it.
template <typename Job, typename Draw>
void supervise(Job &job, Thread thread, const std::string &what, Draw draw)
{
    // 20 frames a second is plenty for a progress screen, and on an Old 3DS
    // the time not spent drawing goes to the worker.
    constexpr unsigned FramesPerDraw = 3;
    unsigned frame = 0;
    ui::enterScreen("progress|" + what);
    while (!job.finished) {
        aptSetHomeAllowed(job.stoppable());
        if (!ui::keepRunning()) {
            job.closing = true;
            job.cancel = true;
            break;
        }
        const ui::Input input = ui::readInput();
        if (job.stoppable() && !job.cancel && wantsCancel(input) &&
            confirmStop(what)) {
            job.cancel = true;
        }
        if (frame++ % FramesPerDraw == 0) {
            draw();
        }
        gspWaitForVBlank();
    }
    aptSetHomeAllowed(true);
    threadJoin(thread, U64_MAX);
    threadFree(thread);
}

struct BusyJob {
    const std::function<void(Busy &)> *work = nullptr;
    Busy busy;
    std::atomic<bool> finished {false};
};

void runBusy(void *argument)
{
    BusyJob &job = *static_cast<BusyJob *>(argument);
    (*job.work)(job.busy);
    job.finished = true;
}

} // namespace

Thread startWorker(ThreadFunc entry, void *argument, const char *name)
{
    return startWorkerThread(entry, argument, name);
}

void busy(const std::string &header, const std::string &title,
          const std::function<void()> &work)
{
    busy(header, title, [&](Busy &) { work(); }, false);
}

bool busy(const std::string &header, const std::string &title,
          const std::function<void(Busy &)> &work, bool cancellable)
{
    BusyJob job;
    job.work = &work;
    const uint64_t started = osGetTime();
    units::RateMeter meter;
    const auto logged = [&]() {
        const bool stopped = job.busy.cancel.load();
        logOutcome("task", !stopped, stopped,
                   {{"header", header},
                    {"title", title},
                    {"ms", osGetTime() - started}});
        return !stopped;
    };
    Thread thread = startWorkerThread(runBusy, &job, "busy");
    if (thread == nullptr) {
        work(job.busy);
        return logged();
    }
    bool pressed = false;
    while (!job.finished) {
        if (!ui::keepRunning()) {
            job.busy.cancel = true;
        }
        const ui::Input input = ui::readInput();
        const bool stopping = job.busy.cancel.load();
        if (cancellable && !stopping) {
            bool cancel = (input.down & KEY_B) != 0;
            if (input.down & KEY_TOUCH) {
                pressed = ui::BusyCancelButton.contains(input.touch);
            }
            if ((input.up & KEY_TOUCH) && pressed) {
                pressed = false;
                cancel = cancel || ui::BusyCancelButton.contains(input.touch);
            }
            if (cancel) {
                job.busy.cancel = true;
                sound::play(sound::Effect::Back);
            }
        }

        ui::BusyView view;
        view.header = header;
        view.title = title;
        view.cancellable = cancellable;
        view.stopping = job.busy.cancel.load();
        view.pressed = pressed && !view.stopping &&
                       (input.held & KEY_TOUCH) &&
                       ui::BusyCancelButton.contains(input.touch);
        const uint64_t total = job.busy.total.load();
        if (total > 0) {
            const uint64_t done = std::min(job.busy.done.load(), total);
            view.progress = true;
            view.fraction = static_cast<float>(static_cast<double>(done) /
                                               static_cast<double>(total));
            view.amount = job.busy.bytes.load()
                              ? units::bytes(done) + " of " + units::bytes(total)
                              : std::to_string(done) + " of " +
                                    std::to_string(total);
            meter.sample(osGetTime(), done);
            const int64_t left = meter.secondsLeft(total - done);
            if (left >= 0 && done < total) {
                view.left = units::timeLeft(static_cast<uint64_t>(left));
            }
        } else if (job.busy.done.load() > 0) {
            // A running count, when the end is not known.
            const uint64_t done = job.busy.done.load();
            view.amount = job.busy.bytes.load() ? units::bytes(done)
                                                : std::to_string(done);
        }
        ui::drawBusy(view);
        gspWaitForVBlank();
    }
    threadJoin(thread, U64_MAX);
    threadFree(thread);
    return logged();
}

net::Result download(const net::Request &request,
                     const std::string &step, bool direct,
                     std::function<bool()> installStarted)
{
    DownloadJob job;
    job.request = &request;
    job.installStarted = std::move(installStarted);

    awake::Hold hold;
    Thread thread = startWorkerThread(runDownload, &job, "download");
    if (thread == nullptr) {
        job.result.message = "Could not start the download thread.";
        return job.result;
    }

    const std::string host = url::host(request.url);
    units::RateMeter meter;
    supervise(job, thread, "this download", [&]() {
        ui::ProgressView view;
        {
            Guard guard(job.lock);
            view.name = job.filename.empty() ? host : job.filename;
            if (!job.filename.empty() && !direct) {
                const screens::KindLook look =
                    screens::lookOf(filetype::fromName(job.filename));
                view.nameIcon = look.icon;
                view.nameIconColor = look.color;
            }
            view.title = job.phase == net::Phase::Connecting ? "Connecting"
                       : job.phase == net::Phase::Restarting
                           ? "Starting Over"
                       : direct ? "Installing"
                                : "Downloading";
            if (!step.empty()) {
                view.title += " " + step;
            }
            if (job.connections > 1) {
                view.tag = std::to_string(job.connections) + " connections";
            }
            if (job.resumedFrom > 0) {
                view.note = "Continuing from " +
                            units::bytes(job.resumedFrom) +
                            " downloaded earlier";
            }
            meter.sample(osGetTime(), job.received);
            if (job.total > 0) {
                view.fraction = static_cast<float>(
                    static_cast<double>(job.received) /
                    static_cast<double>(job.total));
                view.amounts = units::bytes(job.received) + " of " +
                               units::bytes(job.total);
            } else {
                view.amounts = units::bytes(job.received);
            }
            if (meter.hasRate()) {
                view.speed = units::rate(meter.bytesPerSecond());
                const int64_t left = job.total > job.received
                    ? meter.secondsLeft(job.total - job.received)
                    : -1;
                if (left >= 0) {
                    view.left = units::timeLeft(static_cast<uint64_t>(left));
                }
            } else if (job.phase == net::Phase::Downloading) {
                view.speed = "Measuring speed...";
            }
        }
        view.icon = ui::Icon::Download;
        view.iconColor = ui::color::IconGreen;
        view.source = "from " + host;
        view.message = direct ? "Installing as it downloads"
                              : "Saving to " + request.folder;
        view.cancellable = job.stoppable();
        view.stopping = job.cancel;
        ui::drawProgress(view);
    });
    const net::Result &result = job.result;
    logOutcome("download", result.outcome == net::Outcome::Completed,
               result.outcome == net::Outcome::Cancelled,
               {{"host", host},
                {"file", result.filename},
                {"bytes", result.bytes},
                {"transferred", result.transferred},
                {"ms", static_cast<uint64_t>(result.seconds * 1000)},
                {"connections", result.connections},
                {"resumed", result.resumed},
                {"install", direct},
                {"http", result.httpStatus},
                {"curl", result.curlCode},
                {"message", result.message}});
    return job.result;
}

archives::Result extract(const std::string &path,
                         const archives::Listing &listing,
                         const std::string &folder,
                         const archives::Options &options)
{
    ExtractJob job;
    job.path = path;
    job.listing = &listing;
    job.folder = folder;
    job.options = options;

    // What the bar fills towards. A file costs about this much writing on
    // top of its bytes, so thousands of small files still move it.
    constexpr uint64_t FileWeight = 128 * 1024;
    uint64_t totalBytes = 0;
    unsigned totalFiles = 0;
    if (listing.complete) {
        const std::unordered_set<std::string> only(options.only.begin(),
                                                   options.only.end());
        for (const archives::Entry &entry : listing.entries) {
            // What the extractors count: every file with a safe path.
            if (!entry.directory && !entry.path.empty() &&
                (only.empty() || only.count(entry.path) > 0) &&
                (!options.place || !options.place(entry.path, false).empty())) {
                totalBytes += entry.size;
                ++totalFiles;
            }
        }
    }
    const uint64_t totalWork = totalBytes + totalFiles * FileWeight;

    awake::Hold hold;
    Thread thread = startWorkerThread(runExtract, &job, "extract");
    if (thread == nullptr) {
        job.result.message = "Could not start the extraction thread.";
        return job.result;
    }

    const std::string name = path.substr(path.find_last_of('/') + 1);
    const uint64_t started = osGetTime();
    units::RateMeter speed;
    units::RateMeter work;
    supervise(job, thread, "extracting", [&]() {
        archives::Progress progress;
        {
            Guard guard(job.lock);
            progress = job.progress;
        }
        ui::ProgressView view;
        view.title = "Extracting";
        view.icon = ui::Icon::Zip;
        view.iconColor = ui::color::IconGold;
        view.name = name;
        view.source = progress.current.empty() ? "Reading the archive..."
                                               : progress.current;
        const uint64_t now = osGetTime();
        speed.sample(now, progress.bytes);
        if (listing.complete) {
            const uint64_t done = progress.bytes + progress.files * FileWeight;
            work.sample(now, done);
            view.fraction = totalWork > 0
                ? static_cast<float>(static_cast<double>(done) /
                                     static_cast<double>(totalWork))
                : -1.0f;
            // Files lead: with thousands of small ones, bytes say little.
            view.amounts = units::grouped(progress.files) + " of " +
                           screens::plural(totalFiles, "file");
            if (work.hasRate()) {
                const int64_t left = work.secondsLeft(
                    totalWork > done ? totalWork - done : 0);
                if (left >= 0) {
                    view.left = units::timeLeft(static_cast<uint64_t>(left));
                }
            }
            if (speed.hasRate()) {
                view.speed = units::rate(speed.bytesPerSecond());
            }
        } else {
            work.sample(now, progress.archiveBytes);
            view.fraction = listing.archiveSize > 0
                ? static_cast<float>(
                      static_cast<double>(progress.archiveBytes) /
                      static_cast<double>(listing.archiveSize))
                : -1.0f;
            view.amounts = units::bytes(progress.archiveBytes) + " of " +
                           units::bytes(listing.archiveSize);
            view.tag = screens::plural(progress.files, "file");
            if (work.hasRate()) {
                const int64_t left = work.secondsLeft(
                    listing.archiveSize > progress.archiveBytes
                        ? listing.archiveSize - progress.archiveBytes
                        : 0);
                if (left >= 0) {
                    view.left = units::timeLeft(static_cast<uint64_t>(left));
                }
            }
            if (speed.hasRate()) {
                view.speed = units::rate(speed.bytesPerSecond());
            }
        }
        view.message = "Extracting into " + folder;
        view.stopping = job.cancel;
        ui::drawProgress(view);
    });
    const archives::Result &result = job.result;
    logOutcome("extract", result.ok, result.cancelled,
               {{"format", listing.format},
                {"files", result.extracted},
                {"bytes", listing.totalSize},
                {"ms", osGetTime() - started},
                {"existing", result.skippedExisting.size()},
                {"unsafe", result.skippedUnsafe},
                {"unsupported", result.skippedUnsupported},
                {"placed", result.skippedPlaced > 0
                               ? std::to_string(result.skippedPlaced)
                               : std::string()},
                {"message", result.message}});
    return job.result;
}

installer::Result install(const std::string &path, uint64_t titleId,
                          const std::string &name,
                          const std::vector<uint16_t> &icon, bool commit)
{
    InstallJob job;
    job.path = path;
    job.titleId = titleId;
    job.commit = commit;

    awake::Hold hold;
    Thread thread = startWorkerThread(runInstall, &job, "install");
    if (thread == nullptr) {
        job.result.message = "Could not start the install thread.";
        return job.result;
    }

    units::RateMeter meter;
    supervise(job, thread, "installing", [&]() {
        ui::ProgressView view;
        {
            Guard guard(job.lock);
            meter.sample(osGetTime(), job.done);
            view.fraction = job.total > 0
                ? static_cast<float>(static_cast<double>(job.done) /
                                     static_cast<double>(job.total))
                : -1.0f;
            view.amounts = units::bytes(job.done) + " of " +
                           units::bytes(job.total);
            if (meter.hasRate()) {
                view.speed = units::rate(meter.bytesPerSecond());
                const int64_t left = job.total > job.done
                    ? meter.secondsLeft(job.total - job.done)
                    : -1;
                if (left >= 0) {
                    view.left = units::timeLeft(static_cast<uint64_t>(left));
                }
            }
        }
        view.title = "Installing";
        view.icon = ui::Icon::Package;
        view.iconColor = ui::color::IconBlue;
        view.name = name;
        view.source = path.substr(path.find_last_of('/') + 1);
        view.titleIcon = icon;
        view.message = "Do not turn off the power or remove the SD card.";
        view.cancellable = false;
        ui::drawProgress(view);
    });
    if (job.closing && job.result.outcome == installer::Outcome::Cancelled) {
        job.result.message = "Interrupted when 3dsDL closed.";
    }
    const installer::Result &result = job.result;
    logOutcome("install", result.outcome == installer::Outcome::Installed,
               result.outcome == installer::Outcome::Cancelled,
               {{"title", cia::titleIdText(titleId)},
                {"bytes", result.bytes},
                {"ms", static_cast<uint64_t>(result.seconds * 1000)},
                {"code", eventlog::hex(static_cast<uint32_t>(result.code))},
                {"olderOrSame", result.versionRefused},
                {"compressed", result.compressed},
                {"message", result.message}});
    return job.result;
}

fileops::Result fileOperation(
    const std::string &header, const std::string &title,
    const std::string &folder, uint64_t total, bool countsFiles,
    const std::function<fileops::Result(fileops::Observer &)> &work)
{
    FileJob job;
    job.work = &work;
    job.total = total;

    awake::Hold hold;
    Thread thread = startWorkerThread(runFileJob, &job, "files");
    if (thread == nullptr) {
        job.result.status = fileops::Status::Failed;
        job.result.message = "Could not start the worker thread.";
        return job.result;
    }

    const uint64_t started = osGetTime();
    units::RateMeter meter;
    supervise(job, thread, header, [&]() {
        ui::ProgressView view;
        {
            Guard guard(job.lock);
            const uint64_t expected = std::max(job.total, job.done);
            view.fraction = expected > 0
                ? static_cast<float>(static_cast<double>(job.done) /
                                     static_cast<double>(expected))
                : -1.0f;
            meter.sample(osGetTime(), job.done);
            if (countsFiles) {
                view.amounts = std::to_string(job.done) + " of " +
                               std::to_string(expected) + " files";
            } else {
                view.amounts = units::bytes(job.done) + " of " +
                               units::bytes(expected);
                if (meter.hasRate()) {
                    view.speed = units::rate(meter.bytesPerSecond());
                }
            }
            const int64_t left = expected > job.done
                ? meter.secondsLeft(expected - job.done)
                : -1;
            if (left >= 0) {
                view.left = units::timeLeft(static_cast<uint64_t>(left));
            }
            view.source = job.current;
        }
        view.title = capitalised(header);
        view.icon = header == "deleting" ? ui::Icon::Trash : ui::Icon::Folder;
        view.iconColor = header == "deleting" ? ui::color::Bad
                                              : ui::color::IconOrange;
        view.name = title;
        view.message = "In " + folder;
        view.stopping = job.cancel;
        ui::drawProgress(view);
    });
    logOutcome("files", job.result.status == fileops::Status::Ok,
               job.result.status == fileops::Status::Cancelled,
               {{"action", header},
                {"files", job.result.files},
                {"bytes", countsFiles ? 0 : job.done},
                {"ms", osGetTime() - started},
                {"message", job.result.message}});
    return job.result;
}

} // namespace tasks
