#include "app.hpp"

#include "app_util.hpp"
#include "cia.hpp"
#include "crash.hpp"
#include "finalize.hpp"
#include "fs_util.hpp"
#include "installer.hpp"
#include "logging.hpp"
#include "mdns.hpp"
#include "mdns_responder.hpp"
#include "naming.hpp"
#include "platform.hpp"
#include "qr_code.hpp"
#include "remote.hpp"
#include "remote_server.hpp"
#include "storage.hpp"
#include "tasks.hpp"
#include "text.hpp"
#include "ui.hpp"
#include "units.hpp"
#include "url.hpp"

#include <arpa/inet.h>

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <deque>
#include <set>

// The download queue, receiving links and files from other devices on the
// network (FBI's remote install protocol and a web page with a file
// manager), and installing a CIA without saving it.

namespace {

using screens::basename;
using screens::plural;

constexpr unsigned short WebPort = 80;
constexpr unsigned short FallbackWebPort = 8080;

std::string nameOf(const std::string &link)
{
    const std::string name = naming::chooseFilename("", link);
    return name == naming::DefaultFilename ? url::host(link) : name;
}

HistoryEntry historyEntry(const std::string &link, const std::string &folder,
                          HistoryStatus status, const std::string &message)
{
    HistoryEntry entry;
    entry.url = link;
    entry.folder = folder;
    entry.status = status;
    entry.message = message;
    return entry;
}

// The console's address on the local network, or "" when it has none.
std::string localAddress()
{
    const long id = gethostid();
    if (id == 0 || id == -1) {
        return "";
    }
    in_addr address {};
    address.s_addr = static_cast<in_addr_t>(id);
    return inet_ntoa(address);
}

// The name the console answers to on the network, as in http://3ds.local/.
constexpr const char *NetworkName = "3ds";

// Listens on a worker thread while the Receive screen is open, and answers
// for 3ds.local on another.
struct ReceiveJob {
    net::Receiver receiver;
    net::MdnsResponder mdns;
    std::atomic<bool> stop {false};
    // Set once mdns has settled on its name; it does not change after.
    std::atomic<bool> named {false};
    LightLock lock;
    std::deque<net::Received> arrived;
    // Devices that asked to use the SD card: those waiting for an answer
    // on the console, and the answers. They last while Receive is open.
    std::deque<std::string> asking;
    std::set<std::string> allowed;
    std::set<std::string> denied;
    net::Transfer transfer;
    bool transferring = false;
    std::string lastFile;
    net::ConsoleStatus status;

    ReceiveJob() { LightLock_Init(&lock); }

    bool take(net::Received &out)
    {
        LightLock_Lock(&lock);
        const bool any = !arrived.empty();
        if (any) {
            out = arrived.front();
            arrived.pop_front();
        }
        LightLock_Unlock(&lock);
        return any;
    }

    bool takeAsking(std::string &out)
    {
        LightLock_Lock(&lock);
        const bool any = !asking.empty();
        if (any) {
            out = asking.front();
            asking.pop_front();
        }
        LightLock_Unlock(&lock);
        return any;
    }

    void answer(const std::string &from, bool allow)
    {
        LightLock_Lock(&lock);
        (allow ? allowed : denied).insert(from);
        LightLock_Unlock(&lock);
    }

    // Runs on the listener's thread.
    net::Access access(const std::string &from, bool ask)
    {
        LightLock_Lock(&lock);
        net::Access result = net::Access::Pending;
        const bool waiting =
            std::find(asking.begin(), asking.end(), from) != asking.end();
        if (allowed.count(from) != 0) {
            result = net::Access::Allowed;
        } else if (denied.count(from) != 0) {
            result = net::Access::Denied;
        } else if (!waiting && !ask) {
            result = net::Access::Unknown;
        } else if (!waiting) {
            asking.push_back(from);
        }
        LightLock_Unlock(&lock);
        return result;
    }

    // What the page shows the console doing; see net::ConsoleStatus.
    // open says the text names nothing private; see net::ConsoleStatus.
    void setState(const std::string &state, const std::string &text = "",
                  bool open = false)
    {
        LightLock_Lock(&lock);
        status.state = state;
        status.text = text;
        status.open = open;
        LightLock_Unlock(&lock);
    }

    void finish(const std::string &result, bool ok, bool open = false)
    {
        LightLock_Lock(&lock);
        status.state = "ready";
        status.text.clear();
        status.result = result;
        status.resultOk = ok;
        status.open = open;
        ++status.resultSeq;
        LightLock_Unlock(&lock);
    }

    // Runs on the listener's thread.
    net::ConsoleStatus snapshot()
    {
        LightLock_Lock(&lock);
        const net::ConsoleStatus copy = status;
        LightLock_Unlock(&lock);
        return copy;
    }

    // Runs on the listener's thread.
    void progress(const net::Transfer &update)
    {
        LightLock_Lock(&lock);
        transfer = update;
        transferring = !update.finished;
        if (update.finished && update.ok && update.upload) {
            lastFile = update.name + " from " + update.from;
        }
        LightLock_Unlock(&lock);
    }

    // The transfer in progress, if there is one.
    bool current(net::Transfer &out, std::string &last)
    {
        LightLock_Lock(&lock);
        const bool busy = transferring;
        out = transfer;
        last = lastFile;
        LightLock_Unlock(&lock);
        return busy;
    }
};

void receiveMain(void *argument)
{
    crash::ThreadName name("receive");
    ReceiveJob &job = *static_cast<ReceiveJob *>(argument);
    while (!job.stop.load()) {
        net::Received received;
        if (job.receiver.wait(250, received)) {
            LightLock_Lock(&job.lock);
            job.arrived.push_back(received);
            LightLock_Unlock(&job.lock);
        }
    }
}

void mdnsMain(void *argument)
{
    crash::ThreadName name("mdns");
    ReceiveJob &job = *static_cast<ReceiveJob *>(argument);
    job.mdns.claim();
    job.named.store(true);
    while (!job.stop.load()) {
        job.mdns.serve(250);
    }
}

} // namespace

void App::addToQueue(const std::vector<std::string> &links,
                     const std::string &folder, bool announce)
{
    size_t added = 0;
    for (const std::string &link : links) {
        const bool already = std::any_of(
            queue_.begin(), queue_.end(), [&](const QueuedLink &item) {
                return item.url == link && item.folder == folder;
            });
        if (already) {
            continue;
        }
        QueuedLink item;
        item.id = nextQueueId_++;
        item.url = link;
        item.folder = folder;
        queue_.push_back(item);
        ++added;
    }
    if (announce) {
        ui::Notice notice;
        notice.title = added > 0 ? "Added to the Queue" : "Already in the Queue";
        notice.text = std::to_string(queue_.size()) +
                      " waiting in Queue on Home.";
        notice.icon = ui::Icon::Download;
        notice.iconColor = ui::color::IconGreen;
        ui::notify(notice);
    }
}

void App::runQueue(const std::vector<uint64_t> *ids)
{
    std::vector<uint64_t> order;
    for (const QueuedLink &item : queue_) {
        if (ids == nullptr ||
            std::find(ids->begin(), ids->end(), item.id) != ids->end()) {
            order.push_back(item.id);
        }
    }
    if (order.empty()) {
        return;
    }
    if (platform::wifiStatus() == platform::Wifi::Disconnected) {
        ui::message("Not Connected",
                    "Turn wireless communication on, then download the queue "
                    "again.",
                    ui::Tone::Warning);
        return;
    }

    // What to do with CIAs is asked once per run, when the first one lands.
    CiaAction ciaAction = settings_.afterCiaDownload;
    size_t saved = 0;
    std::vector<std::string> savedNames;
    size_t installed = 0;
    size_t failed = 0;
    bool stopped = false;
    for (size_t position = 0; position < order.size() && !stopped; ++position) {
        const auto found = std::find_if(
            queue_.begin(), queue_.end(),
            [&](const QueuedLink &item) { return item.id == order[position]; });
        if (found == queue_.end()) {
            continue;
        }
        QueuedLink item = *found;
        if (!fs::makeDirectories(item.folder)) {
            found->state = QueuedLink::State::Failed;
            found->message = "Could not create " + item.folder + ".";
            ++failed;
            continue;
        }

        net::Request request;
        request.url = item.url;
        request.folder = item.folder;
        request.caFile = http::CaBundle;
        request.userAgent = http::UserAgent;
        request.allowResume = settings_.resumeDownloads;
        request.socketBufferSize = http::SocketBufferSize;
        request.connections = settings_.maxConnections;
        request.freeSpace = [](const std::string &) {
            return platform::sdFreeBytes();
        };
        const std::string step = std::to_string(position + 1) + " of " +
                                 std::to_string(order.size());
        const net::Result result =
            tasks::download(request, settings_.keepAwake, step);
        if (!ui::keepRunning()) {
            return;
        }
        if (result.outcome == net::Outcome::Cancelled) {
            found->state = QueuedLink::State::Stopped;
            found->message = "Stopped.";
            HistoryEntry stoppedEntry = historyEntry(
                item.url, item.folder, HistoryStatus::Cancelled, result.message);
            stoppedEntry.name = result.filename;
            record(stoppedEntry);
            stopped = true;
            break;
        }
        if (result.outcome != net::Outcome::Completed) {
            found->state = QueuedLink::State::Failed;
            found->message = result.message;
            HistoryEntry entry = historyEntry(item.url, item.folder,
                                              HistoryStatus::Failed,
                                              result.message);
            entry.resumable = result.resumable;
            entry.bytes = result.bytes;
            entry.name = result.filename;
            record(entry);
            ++failed;
            continue;
        }

        // A queue never stops to ask about names: a clash keeps both.
        const finalize::Result placed =
            finalize::commit(result.partPath, item.folder, result.filename,
                             false, platform::now());
        HistoryEntry entry = historyEntry(
            item.url, item.folder,
            placed.ok ? HistoryStatus::Saved : HistoryStatus::Failed,
            placed.ok ? "Saved from the queue" : placed.message);
        entry.bytes = result.bytes;
        entry.path = placed.path;
        entry.name = result.filename;
        record(entry);
        if (!placed.ok) {
            found->state = QueuedLink::State::Failed;
            found->message = placed.message;
            ++failed;
            continue;
        }
        queue_.erase(found);
        ++saved;
        savedNames.push_back(basename(placed.path));

        if (!cia::isCiaFile(placed.path) || !installer::available()) {
            continue;
        }
        if (ciaAction == CiaAction::Ask) {
            ui::Dialog dialog;
            dialog.header = "Queue";
            dialog.title = "Install the CIAs from this queue?";
            dialog.tone = ui::Tone::Info;
            dialog.buttons = {"Install", "Install, Delete", "Keep Files"};
            dialog.cancelButton = 2;
            const int choice = ui::choose(dialog);
            ciaAction = choice == 0   ? CiaAction::Install
                        : choice == 1 ? CiaAction::InstallAndDelete
                                      : CiaAction::Keep;
        }
        if (ciaAction == CiaAction::Keep) {
            continue;
        }
        cia::Info info;
        std::string error;
        if (cia::readInfo(placed.path, info, error) &&
            installCia(placed.path, info,
                       ciaAction == CiaAction::InstallAndDelete, item.url,
                       true)) {
            ++installed;
        }
    }

    std::string summary = plural(saved, "file") + " downloaded";
    if (installed > 0) {
        summary += ", " + std::to_string(installed) + " installed";
    }
    if (failed > 0) {
        summary += ", " + std::to_string(failed) + " failed";
    }
    summary += ".";
    if (stopped) {
        summary += " The queue was stopped; the rest are still in it.";
    } else if (failed > 0) {
        summary += " What failed stays in the queue to try again.";
    }
    // The names go on the top screen, which would otherwise show the last
    // progress frame behind the dialog.
    std::string names;
    for (size_t index = 0; index < savedNames.size() && index < 6; ++index) {
        names += (index > 0 ? ", " : "") + savedNames[index];
    }
    if (savedNames.size() > 6) {
        names += " and " + std::to_string(savedNames.size() - 6) + " more";
    }
    ui::message(failed == 0 && !stopped ? "Queue Done" : "Queue Finished",
                summary,
                failed == 0 && !stopped ? ui::Tone::Success
                                        : ui::Tone::Warning,
                names.empty() ? "" : "Saved: " + names + ".");
}

void App::queueScreen()
{
    int cursor = 0;
    while (ui::keepRunning() && !quit_) {
        ui::ListSpec spec;
        spec.title = "Queue";
        spec.emptyText = "Nothing is waiting.";
        for (const QueuedLink &item : queue_) {
            ui::ListItem row;
            row.label = nameOf(item.url);
            row.icon = ui::Icon::Download;
            row.iconColor = ui::color::IconGreen;
            if (item.state == QueuedLink::State::Failed) {
                row.badge = "Failed";
                row.badgeColor = ui::color::Bad;
            } else if (item.state == QueuedLink::State::Stopped) {
                row.badge = "Stopped";
                row.badgeColor = ui::color::Neutral;
            } else {
                row.detail = url::host(item.url);
            }
            spec.items.push_back(row);
        }
        if (!queue_.empty()) {
            spec.action = "Download All";
            spec.actionKey = "X";
            spec.actionKeys = KEY_X;
        }
        spec.drawTop = [&](int highlighted) {
            ui::drawTopChrome("Queue", ui::Icon::Download, ui::color::IconGreen);
            ui::drawPanel();
            if (highlighted < 0 || highlighted >= static_cast<int>(queue_.size())) {
                ui::drawEmblem(ui::Icon::Download, ui::color::IconGreen);
                return;
            }
            const QueuedLink &item = queue_[highlighted];
            std::vector<ui::Fact> facts = {{"URL", item.url},
                                           {"Save to", item.folder}};
            if (!item.message.empty()) {
                const bool failed = item.state == QueuedLink::State::Failed;
                facts.push_back({failed ? "Failed" : "Note", item.message,
                                 failed ? ui::color::Bad : ui::color::Neutral});
            } else {
                facts.push_back({"Position", std::to_string(highlighted + 1) +
                                                 " of " +
                                                 std::to_string(queue_.size())});
            }
            // The name beside its icon, then the three fact rows (20 px,
            // 26 apart), centred in the panel as one block.
            constexpr float Header = 30 + 14;
            constexpr float Rows = 3 * 26 - 6;
            const float top =
                ui::TopPanel.y + (ui::TopPanel.h - Header - Rows) / 2;
            const float left = ui::TopPanel.x + 18;
            ui::drawIcon(ui::Icon::Download, left, top, 30,
                         ui::color::IconGreen);
            ui::drawMarquee(nameOf(item.url), left + 40,
                            top + (30 - ui::lineHeight(0.62f)) / 2, 0.62f,
                            ui::color::InkStrong, 300);
            ui::drawFacts(facts, {ui::TopPanel.x, top + Header,
                                  ui::TopPanel.w, Rows});
        };
        const ui::ListResult result = ui::runList(spec, cursor);
        if (result.key == KEY_X && !queue_.empty()) {
            runQueue();
            continue;
        }
        if (result.key != KEY_A || result.index < 0 ||
            result.index >= static_cast<int>(queue_.size())) {
            return;
        }
        const QueuedLink item = queue_[result.index];
        const int choice = ui::menu(
            {{"Download Now", false, true},
             {"Remove", true, true},
             {"Remove All", true, true}},
            [&](int) {
                ui::drawTopChrome("Queue", ui::Icon::Download,
                                  ui::color::IconGreen);
                ui::drawPanel();
                ui::drawPanelText(nameOf(item.url), ui::TopPanel);
            });
        if (choice == 0) {
            const std::vector<uint64_t> only = {item.id};
            runQueue(&only);
        } else if (choice == 1) {
            queue_.erase(std::remove_if(queue_.begin(), queue_.end(),
                                        [&](const QueuedLink &link) {
                                            return link.id == item.id;
                                        }),
                         queue_.end());
        } else if (choice == 2) {
            ui::Dialog dialog;
            dialog.header = "Queue";
            dialog.title = "Remove " + plural(queue_.size(), "link") +
                           " from the queue?";
            dialog.tone = ui::Tone::Warning;
            dialog.buttons = {"Remove", "Cancel"};
            dialog.dangerous = 0;
            dialog.initial = 1;
            dialog.cancelButton = 1;
            if (ui::choose(dialog) == 0) {
                queue_.clear();
            }
        }
    }
}

void App::receiveScreen()
{
    if (platform::wifiStatus() == platform::Wifi::Disconnected) {
        ui::message("Not Connected",
                    "Receiving needs the same Wi-Fi network as the phone or "
                    "computer sending. Turn wireless communication on first.",
                    ui::Tone::Warning);
        return;
    }
    const std::string address = localAddress();
    if (address.empty()) {
        ui::message("No Network Address",
                    "The console has no address on the local network yet. "
                    "Check the connection and try again.",
                    ui::Tone::Warning);
        return;
    }

    ReceiveJob job;
    std::string error;
    if (!job.receiver.open(remote::FbiPort, WebPort, FallbackWebPort, error)) {
        ui::message("Cannot Receive", error, ui::Tone::Error);
        return;
    }
    net::FileSharing sharing;
    sharing.root = "sdmc:";
    sharing.access = [&job](const std::string &from, bool ask) {
        return job.access(from, ask);
    };
    sharing.freeSpace = []() { return platform::sdFreeBytes(); };
    sharing.totalSpace = []() { return platform::sdTotalBytes(); };
    sharing.status = [&job]() { return job.snapshot(); };
    sharing.appVersion = APP_VERSION;
    sharing.progress = [&job](const net::Transfer &transfer) {
        job.progress(transfer);
    };
    // Room for the network to keep filling while an upload's last piece is
    // written to the SD card, so the sender is not told to wait.
    sharing.socketBufferSize = 128 * 1024;
    if (fs::makeDirectories(storage::DataFolder)) {
        sharing.stagingFolder = storage::DataFolder;
    }
    // Leaving Receive stops whatever request is being served, rather than
    // waiting for an upload or a big delete to finish.
    sharing.cancelled = [&job]() { return job.stop.load(); };
    job.receiver.shareFiles(sharing);

    s32 priority = 0x30;
    svcGetThreadPriority(&priority, CUR_THREAD_HANDLE);
    const int workerPriority = std::min(0x3f, static_cast<int>(priority) + 1);
    // A New 3DS's third core is free, so receiving a file never waits for
    // drawing; elsewhere the listener shares the app core.
    bool isNew3ds = false;
    Thread thread =
        R_SUCCEEDED(APT_CheckNew3DS(&isNew3ds)) && isNew3ds
            ? threadCreate(receiveMain, &job, 32 * 1024, workerPriority, 2,
                           false)
            : nullptr;
    if (thread == nullptr) {
        thread = threadCreate(receiveMain, &job, 32 * 1024, workerPriority,
                              -2, false);
    }
    if (thread == nullptr) {
        ui::message("Cannot Receive", "The network listener could not start.",
                    ui::Tone::Error);
        return;
    }
    // 3ds.local is a convenience: without multicast the address still works.
    std::string mdnsError;
    Thread mdnsThread =
        job.mdns.open(NetworkName, static_cast<uint32_t>(gethostid()),
                      mdns::Port, true, mdnsError)
            ? threadCreate(mdnsMain, &job, 16 * 1024, workerPriority, -2, false)
            : nullptr;

    const std::string port =
        job.receiver.webPort() == 80
            ? std::string("/")
            : ":" + std::to_string(job.receiver.webPort()) + "/";
    const std::string page = "http://" + address + port;
    std::string namedPage;
    const QrCode code(page);
    std::string last;
    bool backPressed = false;
    bool awake = false;
    units::RateMeter meter;
    std::string meterName;
    const ui::Rect back = ui::tabRect(ui::Tab::Left);

    // Installs what the page sent, telling it how each went.
    const auto installReceived = [this](ReceiveJob &state,
                                        const std::vector<std::string> &paths,
                                        bool deleteAfter) {
        size_t installed = 0;
        std::vector<std::string> failed;
        std::vector<std::string> already;
        for (size_t index = 0; index < paths.size() && ui::keepRunning();
             ++index) {
            const std::string name = basename(paths[index]);
            state.setState("installing",
                           "Installing " + name +
                               (paths.size() > 1
                                    ? " (" + std::to_string(index + 1) +
                                          " of " +
                                          std::to_string(paths.size()) + ")"
                                    : "") +
                               ".");
            cia::Info info;
            std::string error;
            bool kept = false;
            if (!cia::readInfo(paths[index], info, error) ||
                !installCia(paths[index], info, deleteAfter, "", true, &kept)) {
                failed.push_back(name);
            } else if (kept) {
                already.push_back(name);
            } else {
                ++installed;
            }
        }
        const auto listed = [](const std::vector<std::string> &names) {
            std::string list;
            for (const std::string &name : names) {
                list += (list.empty() ? "" : ", ") + name;
            }
            return list;
        };
        std::string summary;
        if (installed > 0) {
            summary = paths.size() == 1
                          ? "Installed " + basename(paths[0]) + "."
                      : installed == paths.size()
                          ? "Installed all " + plural(paths.size(), "CIA") + "."
                          : "Installed " + plural(installed, "CIA") + ".";
        }
        if (!already.empty()) {
            summary += std::string(summary.empty() ? "" : " ") +
                       (paths.size() == 1
                            ? already.front() + " was already installed."
                            : "Already installed: " + listed(already) + ".");
        }
        if (!failed.empty()) {
            summary += std::string(summary.empty() ? "" : " ") +
                       "Not installed: " + listed(failed) + ".";
        }
        state.finish(summary, failed.empty());
        // A single CIA that was already there has had its own message.
        if (installed > 0 || !failed.empty() || paths.size() > 1) {
            ui::message(failed.empty() ? "Installed" : "Some were not installed",
                        summary,
                        failed.empty() ? ui::Tone::Success
                                       : ui::Tone::Warning);
        }
    };

    ui::enterScreen("receive");
    while (ui::keepRunning() && !quit_) {
        if (namedPage.empty() && job.named.load()) {
            namedPage = "http://" + job.mdns.hostname() + port;
        }

        std::string asker;
        if (job.takeAsking(asker)) {
            ui::Dialog dialog;
            dialog.header = "Receive";
            dialog.title = "Let " + asker +
                           " add, change and delete files on the SD card?";
            dialog.tone = ui::Tone::Warning;
            dialog.buttons = {"Allow", "Don't Allow"};
            dialog.cancelButton = 1;
            job.setState("asking", "Asking whether " + asker +
                                       " may use the SD card.");
            job.answer(asker, ui::choose(dialog) == 0);
            job.setState("ready");
            continue;
        }

        net::Received received;
        if (job.take(received)) {
            logging::info("receive.got", {{"links", received.links.size()},
                                          {"cias", received.install.size()},
                                          {"fbi", received.replySocket >= 0}});
            if (!received.install.empty()) {
                ui::Dialog dialog;
                dialog.header = "Receive";
                dialog.title = "Install " +
                               plural(received.install.size(), "CIA") +
                               " sent from " + received.from + "?";
                dialog.tone = ui::Tone::Info;
                std::string names;
                for (size_t index = 0;
                     index < received.install.size() && index < 4; ++index) {
                    names += (index > 0 ? ", " : "") +
                             basename(received.install[index]);
                }
                if (received.install.size() > 4) {
                    names += " and " +
                             std::to_string(received.install.size() - 4) +
                             " more";
                }
                dialog.body.push_back({names, ui::color::Text, 0.5f, 3});
                dialog.buttons = {"Install", "Install and Delete", "Not Now"};
                dialog.cancelButton = 2;
                job.setState("choosing",
                             "Waiting for you to choose Install on the 3DS.");
                const int choice = ui::choose(dialog);
                if (choice == 0 || choice == 1) {
                    installReceived(job, received.install, choice == 1);
                } else {
                    job.finish("Not installed: Not Now was chosen on the 3DS.",
                               false);
                }
                continue;
            }
            const bool fromFbiTool = received.replySocket >= 0;
            ui::Dialog dialog;
            dialog.header = "Receive";
            dialog.title = "Received " + plural(received.links.size(), "link") +
                           " from " + received.from + ".";
            dialog.tone = ui::Tone::Info;
            std::string names;
            for (size_t index = 0; index < received.links.size() && index < 4;
                 ++index) {
                names += (index > 0 ? ", " : "") + nameOf(received.links[index]);
            }
            if (received.links.size() > 4) {
                names += " and " + std::to_string(received.links.size() - 4) +
                         " more";
            }
            dialog.body.push_back({names, ui::color::Text, 0.5f, 3});
            dialog.body.push_back({"They go to " + settings_.destination + ".",
                                   ui::color::Muted, 0.44f, 2});
            if (fromFbiTool) {
                // The sender stops serving its files once told the links are
                // done, so these cannot wait in the queue.
                dialog.buttons = {"Download", "Cancel"};
                dialog.cancelButton = 1;
            } else {
                dialog.buttons = {"Download", "Add to Queue", "Cancel"};
                dialog.cancelButton = 2;
            }
            job.setState("choosing",
                         "Waiting for you to choose Download on the 3DS.", true);
            const int choice = ui::choose(dialog);
            const std::string count = plural(received.links.size(), "link");
            if (choice == 1 && !fromFbiTool) {
                job.finish("Added " + count + " to the Queue on the 3DS.", true,
                           true);
            } else if (choice != 0) {
                job.finish("Not downloaded: Cancel was chosen on the 3DS.",
                           false, true);
            }
            if (choice == 0 || (!fromFbiTool && choice == 1)) {
                const uint64_t first = nextQueueId_;
                addToQueue(received.links, settings_.destination, choice == 1);
                if (choice == 0) {
                    std::vector<uint64_t> ids;
                    for (const QueuedLink &item : queue_) {
                        if (item.id >= first ||
                            std::find(received.links.begin(),
                                      received.links.end(),
                                      item.url) != received.links.end()) {
                            ids.push_back(item.id);
                        }
                    }
                    job.setState("downloading", "Downloading " + count + ".",
                                 true);
                    runQueue(&ids);
                    job.finish("Done with " + count +
                                   " on the 3DS. History has how each went.",
                               true, true);
                }
                last = plural(received.links.size(), "link") + " from " +
                       received.from;
            }
            net::finishReceived(received);
            ui::enterScreen("receive");
            continue;
        }

        net::Transfer transfer;
        std::string lastFile;
        const bool busy = job.current(transfer, lastFile);
        if (!lastFile.empty()) {
            last = lastFile;
        }
        // Sleep turns Wi-Fi off, which would cut the transfer.
        if (busy != awake) {
            awake = busy;
            aptSetSleepAllowed(!awake);
        }
        if (busy && transfer.name != meterName) {
            meter.reset();
            meterName = transfer.name;
        }
        if (busy) {
            meter.sample(osGetTime(), transfer.done);
        }

        const ui::Input input = ui::readInput();
        if (input.down & KEY_B) {
            break;
        }
        if (input.down & KEY_TOUCH) {
            backPressed = back.contains(input.touch);
        }
        if ((input.up & KEY_TOUCH) && backPressed) {
            backPressed = false;
            if (back.contains(input.touch)) {
                break;
            }
        }

        ui::beginFrame();
        ui::drawTopChrome("Receive", ui::Icon::Chip, ui::color::IconPink);
        ui::drawPanel();
        const float px = code.size() <= 25 ? 4 : 3;
        const float qrSide = code.size() * px;
        const float qrX = ui::TopPanel.x + 12 + 2 * px;
        const float qrY = ui::TopPanel.y + (ui::TopPanel.h - qrSide) / 2;
        code.draw(qrX, qrY, px);
        const float left = qrX + qrSide + 2 * px + 12;
        const float width = ui::TopPanel.x + ui::TopPanel.w - 12 - left;
        // The page's addresses are large on the touch screen, so the column
        // beside the code holds the one FBI's tools use, centred in it.
        const std::string tools = "Boop and servefiles.py";
        // Small enough that "192.168.185.97:5000" fits the column.
        constexpr float AddressScale = 0.48f;
        const float middle = left + width / 2;
        const float toolsY =
            ui::TopPanel.y +
            (ui::TopPanel.h - 20 - 8 - ui::lineHeight(AddressScale)) / 2;
        ui::drawTag(middle - (ui::textWidth(tools, 0.4f) + 16) / 2, toolsY,
                    tools);
        const bool fbi = job.receiver.fbiPort() != 0;
        ui::drawMarquee(fbi ? address + ":" +
                                  std::to_string(job.receiver.fbiPort())
                            : "Port " + std::to_string(remote::FbiPort) +
                                  " is busy",
                        middle, toolsY + 28, AddressScale,
                        fbi ? ui::color::Ink : ui::color::InkSoft, width,
                        C2D_AlignCenter);

        ui::beginBottom();
        // Centred above Back.
        const ui::Rect panel = {13, 36, 294, 136};
        ui::drawPanel(panel);
        if (busy) {
            ui::drawText(transfer.upload ? "Receiving" : "Sending", 160,
                         panel.y + 14, ui::type::Small, ui::color::InkSoft,
                         C2D_AlignCenter);
            ui::drawMarquee(transfer.name, 160, panel.y + 34, ui::type::Body,
                            ui::color::Ink, 270, C2D_AlignCenter);
            const float fraction =
                transfer.total > 0
                    ? static_cast<float>(static_cast<double>(transfer.done) /
                                         static_cast<double>(transfer.total))
                    : 1.0f;
            ui::drawProgressBar({33, panel.y + 64, 254, 14}, fraction);
            ui::drawText(units::bytes(transfer.done) + " of " +
                             units::bytes(transfer.total),
                         160, panel.y + 88, ui::type::Small, ui::color::Ink,
                         C2D_AlignCenter);
            ui::drawText(meter.hasRate()
                             ? units::rate(meter.bytesPerSecond()) + ", " +
                                   (transfer.upload ? "from " : "to ") +
                                   transfer.from
                             : (transfer.upload ? "From " : "To ") +
                                   transfer.from,
                         160, panel.y + 110, ui::type::Tiny, ui::color::InkSoft,
                         C2D_AlignCenter);
        } else {
            // Centred with or without the last line. The 3ds.local line
            // keeps its place while mdns settles, so nothing jumps.
            const float height =
                last.empty() ? 80 + ui::lineHeight(ui::type::Small)
                             : 102 + ui::lineHeight(ui::type::Tiny);
            const float y = panel.y + (panel.h - height) / 2;
            ui::drawSpinner(160, y + 12, 12);
            ui::drawText("Waiting at", 160, y + 30, ui::type::Body,
                         ui::color::Ink, C2D_AlignCenter);
            ui::drawMarquee(page, 160, y + 56, ui::type::Body,
                            ui::color::Accent, 280, C2D_AlignCenter);
            if (!namedPage.empty()) {
                ui::drawMarquee("or " + namedPage, 160, y + 80,
                                ui::type::Small, ui::color::InkSoft, 280,
                                C2D_AlignCenter);
            }
            if (!last.empty()) {
                ui::drawMarquee("Last: " + last, 160, y + 102, ui::type::Tiny,
                                ui::color::InkSoft, 280, C2D_AlignCenter);
            }
        }
        ui::drawTab(ui::Tab::Left, "Back", "B",
                    backPressed && (input.held & KEY_TOUCH));
        ui::endFrame();
    }

    job.stop.store(true);
    threadJoin(thread, U64_MAX);
    threadFree(thread);
    if (mdnsThread != nullptr) {
        threadJoin(mdnsThread, U64_MAX);
        threadFree(mdnsThread);
    }
    if (awake) {
        aptSetSleepAllowed(true);
    }
    // Senders still waiting are told the links are done, so they exit.
    net::Received pending;
    while (job.take(pending)) {
        net::finishReceived(pending);
    }
    job.receiver.close();
    job.mdns.close();
}

void App::installDirectly(const std::string &address)
{
    const std::string folder = storage::DataFolder;
    if (!fs::makeDirectories(folder)) {
        ui::message("Not Installed", "Could not create " + folder + ".",
                    ui::Tone::Error);
        return;
    }
    installer::StreamInstall sink;
    net::Request request;
    request.url = address;
    request.folder = folder;
    request.caFile = http::CaBundle;
    request.userAgent = http::UserAgent;
    request.allowResume = false;
    request.socketBufferSize = http::SocketBufferSize;
    request.connections = 1; // the install has to receive the data in order
    request.write = [&sink](const void *data, size_t size, FILE *file) {
        const off_t at = ftello(file);
        return at >= 0 && sink.write(static_cast<uint64_t>(at), data, size)
                   ? size
                   : static_cast<size_t>(0);
    };
    const net::Result result =
        tasks::download(request, settings_.keepAwake, "", true,
                        [&sink]() { return sink.started(); });
    if (!result.partPath.empty()) {
        fs::removeFile(result.partPath); // empty: the data went to the install
    }

    installer::Result installed;
    if (result.outcome == net::Outcome::Completed) {
        installed = sink.finish();
    } else {
        sink.cancel();
        installed = sink.result();
        if (installed.message.empty()) {
            installed.message = result.message;
        }
        if (result.outcome == net::Outcome::Cancelled) {
            installed.outcome = installer::Outcome::Cancelled;
        }
    }
    if (!ui::keepRunning()) {
        return;
    }

    const cia::Info &info = sink.info();
    const std::string name = info.hasSmdh ? screens::ciaName(info)
                                          : nameOf(address);
    const auto recordResult = [&](HistoryStatus status,
                                  const std::string &message) {
        HistoryEntry entry = historyEntry(address, folder, status, message);
        entry.name = result.filename;
        entry.bytes = installed.bytes;
        record(entry);
    };
    if (installed.outcome == installer::Outcome::Cancelled) {
        recordResult(HistoryStatus::Cancelled, "Direct install stopped");
        ui::message("Install Stopped", "Nothing was changed.");
        return;
    }
    if (installed.outcome != installer::Outcome::Installed) {
        recordResult(HistoryStatus::Failed,
                     "Direct install failed: " + installed.message);
        ui::message("Install Failed", installed.message, ui::Tone::Error, name);
        return;
    }
    recordResult(HistoryStatus::Installed, "Installed directly, nothing saved");

    std::string seedNote;
    if (info.seed == cia::Seed::Needed) {
        tasks::busy("Installing", "Getting the seed " + name + " needs...",
                    [&](tasks::Busy &busy) {
                        installer::installSeed(info.titleId, seedNote,
                                               &busy.cancel);
                    },
                    true);
    }
    const uint64_t launchId = cia::baseTitle(info.titleId);
    const cia::Kind kind = cia::kind(launchId);
    const bool canLaunch = launchId == info.titleId &&
                           (kind == cia::Kind::Application ||
                            kind == cia::Kind::Demo);
    ui::Dialog dialog;
    dialog.header = "Installation Complete";
    dialog.title = name + " has been installed.";
    dialog.tone = ui::Tone::Success;
    dialog.icon = info.hasSmdh ? ui::showIcon(info.smdh.icon) : nullptr;
    dialog.body.push_back({name, ui::color::Text, 0.5f, 1});
    dialog.body.push_back({std::string(cia::kindName(cia::kind(info.titleId))) +
                               ", " + screens::titleVersion(info.titleId, info.version) + ", " +
                               units::bytes(installed.bytes),
                           ui::color::Muted, 0.46f, 3});
    if (!seedNote.empty()) {
        dialog.body.push_back({seedNote, ui::color::Muted, 0.44f, 2});
    }
    dialog.buttons = canLaunch ? std::vector<std::string>{"Close", "Launch"}
                               : std::vector<std::string>{"Close"};
    dialog.initial = canLaunch ? 1 : 0;
    dialog.cancelButton = 0;
    if (ui::choose(dialog) == 1 && canLaunch) {
        std::string error;
        if (!installer::launch(launchId, error)) {
            ui::message("Could Not Launch", error, ui::Tone::Error);
        }
    }
}
