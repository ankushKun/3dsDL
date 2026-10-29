#include "app.hpp"

#include "app_util.hpp"
#include "finalize.hpp"
#include "installer.hpp"
#include "fs_util.hpp"
#include "sha256.hpp"
#include "storage.hpp"
#include "tasks.hpp"
#include "ui.hpp"
#include "units.hpp"
#include "updater.hpp"

#include <algorithm>
#include <cmath>

// Settings > Software Update, the notification at startup, and putting a
// new version in place.

namespace {

using screens::basename;

std::string folderOf(const std::string &path)
{
    const size_t slash = path.rfind('/');
    return slash == std::string::npos ? path : path.substr(0, slash + 1);
}

// A rounded square with a white icon, like the notification's.
void drawBadgeIcon(float x, float y, ui::Icon icon, u32 fill)
{
    ui::fillRound({x, y, 48, 48}, 11, fill);
    ui::fillRound({x, y, 48, 24}, 11, ui::color::rgb(0xFFFFFF, 0x38),
                  ui::corner::Top);
    ui::drawIcon(icon, x + 8, y + 8, 32, ui::color::rgb(0xFFFFFF));
}

// The badge and headline over facts about the installed and newest versions.
void drawUpdateTop(const updater::Status &status)
{
    ui::drawTopChrome("Software Update", ui::Icon::Download,
                      ui::color::IconGreen);
    ui::drawPanel();
    const float left = ui::TopPanel.x + 18;
    const float top = ui::TopPanel.y + 12;
    const update::Release &release = status.release;

    ui::Icon icon = ui::Icon::Warning;
    u32 fill = ui::color::IconOrange;
    std::string headline = "Update check failed.";
    std::vector<ui::Fact> facts = {{"Installed", APP_VERSION}};
    switch (status.state) {
    case updater::State::Available: {
        icon = ui::Icon::AppMark;
        fill = ui::color::Accent;
        headline = "An update is available.";
        const update::Asset *asset =
            update::findAsset(release, updater::assetName());
        facts.push_back({"New",
                         release.version +
                             (asset != nullptr && asset->size > 0
                                  ? ", " + units::bytes(asset->size)
                                  : ""),
                         ui::color::Good});
        if (!release.published.empty()) {
            facts.push_back({"Released", release.published});
        }
        break;
    }
    case updater::State::UpToDate:
        icon = ui::Icon::Check;
        fill = ui::color::Good;
        headline = "3dsDL is up to date.";
        facts.push_back({"Newest", release.version});
        if (!release.published.empty()) {
            facts.push_back({"Released", release.published});
        }
        break;
    default:
        facts.push_back({"Error", status.message.empty() ? "Not checked yet."
                                                         : status.message});
        // A wrong clock makes every certificate look invalid.
        if (status.certificateProblem) {
            facts.push_back({"Clock", "Check Settings, Console Clock",
                             ui::color::Badge});
        }
        break;
    }

    drawBadgeIcon(left, top, icon, fill);
    // Level with the badge's middle, to the panel's inner edge.
    ui::drawMarquee(headline, left + 60, top + (48 - ui::lineHeight(0.62f)) / 2,
                    0.62f, ui::color::InkStrong, ui::TopPanel.w - 36 - 60);
    const float below = top + 52;
    ui::drawFacts(facts, {ui::TopPanel.x, below, ui::TopPanel.w,
                          ui::TopPanel.y + ui::TopPanel.h - below});
}

} // namespace

App::~App()
{
    saveIndex();
    aptUnhook(&aptCookie_);
    ui::setFrameHook(nullptr);
    updater::shutdown();
}

void App::pollUpdate()
{
    const uint32_t generation = updater::generation();
    if (generation == updateGeneration_) {
        return;
    }
    updateGeneration_ = generation;
    const updater::Status status = updater::status();
    updateAvailable_ = status.state == updater::State::Available;
    if (updateAvailable_ && !updateNoticeShown_) {
        updateNoticeShown_ = true;
        ui::Notice notice;
        notice.title = "Update Available";
        notice.text = "Get 3dsDL " + status.release.version +
                      " from Settings.";
        ui::notify(notice);
    }
}

void App::softwareUpdateScreen()
{
    updater::Status status = updater::status();
    const auto check = [&]() {
        tasks::busy(
            "Software Update", "Checking for updates...",
            [&](tasks::Busy &busy) { status = updater::checkNow(&busy.cancel); },
            true);
        updateGeneration_ = updater::generation();
        updateAvailable_ = status.state == updater::State::Available;
        // Seen here, so the notification has nothing left to say.
        updateNoticeShown_ = true;
    };
    if (status.state != updater::State::Available &&
        status.state != updater::State::UpToDate) {
        check();
    }

    int focus = 0;
    while (ui::keepRunning() && !quit_) {
        const bool available = status.state == updater::State::Available;
        const bool haveRelease = available ||
                                 status.state == updater::State::UpToDate;
        ui::ScreenSpec spec;
        spec.back = "Back";
        const auto add = [&](ui::Rect rect, const std::string &label,
                             ui::Icon icon, u32 iconColor, bool enabled) {
            ui::Control control;
            control.rect = rect;
            control.label = label;
            control.icon = icon;
            control.iconColor = iconColor;
            control.enabled = enabled;
            control.scale = ui::type::Small + 0.06f;
            spec.controls.push_back(control);
        };
        // Centred between the top and the tabs.
        if (available) {
            add({30, 40, 260, 44}, "Update to " + status.release.version,
                ui::Icon::Download, ui::color::IconGreen, true);
            spec.controls.back().scale = ui::type::Button;
        } else {
            add({30, 40, 260, 44}, "Check for Updates", ui::Icon::Globe,
                ui::color::IconTeal, true);
            spec.controls.back().scale = ui::type::Button;
        }
        add({30, 94, 260, 32},
            haveRelease ? "What's New in " + status.release.version
                        : "What's New",
            ui::Icon::Text, ui::color::IconGrey,
            haveRelease && !status.release.notes.empty());
        add({30, 136, 260, 32},
            std::string("Check at Startup: ") +
                (settings_.checkForUpdates ? "On" : "Off"),
            ui::Icon::Clock, ui::color::IconBlue, true);
        spec.drawTop = [&]() { drawUpdateTop(status); };

        const ui::ScreenResult result = ui::runScreen(spec, focus);
        switch (result.choice) {
        case 0:
            if (available) {
                installUpdate(status.release);
            } else {
                check();
            }
            break;
        case 1:
            showReleaseNotes(status.release);
            break;
        case 2:
            settings_.checkForUpdates = !settings_.checkForUpdates;
            persistSettings();
            break;
        default:
            return;
        }
    }
}

void App::showReleaseNotes(const update::Release &release)
{
    constexpr float Scale = ui::type::Tiny + 0.02f;
    constexpr float Top = 64;
    constexpr float Width = 356;
    const float pitch = ui::lineHeight(Scale) * 0.92f;

    // Wrapped once; list items keep their indent on following lines.
    std::vector<std::string> lines;
    for (const std::string &note : update::noteLines(release.notes)) {
        const size_t indent = note.find_first_not_of(' ');
        const bool item = indent != std::string::npos &&
                          note.compare(indent, 2, "- ") == 0;
        const std::string hang(item ? indent + 2 : 0, ' ');
        const std::vector<std::string> wrapped =
            ui::wrap(note, Scale, Width, 40);
        for (size_t index = 0; index < wrapped.size(); ++index) {
            lines.push_back(index == 0 ? wrapped[index] : hang + wrapped[index]);
        }
        if (wrapped.empty()) {
            lines.push_back("");
        }
    }
    const float viewport = ui::ScreenHeight - Top - 4;
    const float content = lines.size() * pitch;
    const std::string title = "What's New in " + release.version;

    ui::Scroller scroller;
    bool backPressed = false;
    const ui::Rect back = ui::tabRect(ui::Tab::Left);
    ui::enterScreen("whats-new");
    while (ui::keepRunning()) {
        const ui::Input input = ui::readInput();
        if (input.down & KEY_B) {
            return;
        }
        if (input.down & KEY_TOUCH) {
            backPressed = back.contains(input.touch);
        }
        if ((input.up & KEY_TOUCH) && backPressed) {
            if (back.contains(input.touch)) {
                return;
            }
            backPressed = false;
        }
        const float maxOffset = std::max(0.0f, content - viewport);
        if (input.repeat & (KEY_DOWN | KEY_UP)) {
            const float step = (input.repeat & KEY_DOWN) ? pitch * 3 : -pitch * 3;
            scroller.setOffset(std::max(0.0f, std::min(maxOffset,
                                                       scroller.offset() + step)));
        }
        if (!backPressed) {
            scroller.update(input, {0, 0, 320, 205}, content, viewport);
        }

        ui::beginFrame();
        const float offset = scroller.offset();
        for (size_t index = 0; index < lines.size(); ++index) {
            const float y = Top + index * pitch - offset;
            if (y > ui::ScreenHeight) {
                break;
            }
            if (y + pitch < Top - pitch) {
                continue;
            }
            const std::string &line = lines[index];
            const bool heading = !line.empty() && line[0] != ' ' &&
                                 line.compare(0, 2, "- ") != 0 &&
                                 index + 1 < lines.size() &&
                                 (lines[index + 1].empty() ||
                                  lines[index + 1].compare(0, 2, "- ") == 0);
            ui::drawText(line, 22, y, Scale,
                         heading ? ui::color::Accent : ui::color::Ink);
        }
        // Lines scrolled past the top are covered again by the chrome.
        ui::fillRound({0, 0, ui::TopWidth, Top - 4}, 0, ui::color::TopA);
        ui::drawStatusBar();
        const float width = 28 + ui::textWidth(title, ui::type::Title);
        ui::drawIcon(ui::Icon::Text, 200 - width / 2, 26, 22,
                     ui::color::IconGrey);
        ui::drawText(title, 200 - width / 2 + 28,
                     37 - ui::lineHeight(ui::type::Title) / 2 + 1,
                     ui::type::Title, ui::color::Accent);
        ui::drawDots(57);
        if (content > viewport) {
            const float bar = std::max(20.0f, viewport * viewport / content);
            const float at = Top + (viewport - bar) * (offset / maxOffset);
            ui::fillRound({392, std::max(Top, at), 4, bar}, 2,
                          ui::color::rgb(0xB0A66E, 0x90));
        }

        ui::beginBottom();
        ui::drawTab(ui::Tab::Left, "Back", "B",
                    backPressed && (input.held & KEY_TOUCH));
        ui::endFrame();
    }
}

void App::installUpdate(const update::Release &release)
{
    const std::string name = updater::assetName();
    const update::Asset *asset = update::findAsset(release, name);
    if (asset == nullptr) {
        ui::message("Update Not Available",
                    "The " + release.version + " release has no " + name +
                        " file to update from.",
                    ui::Tone::Error);
        return;
    }
    if (platform::wifiStatus() == platform::Wifi::Disconnected) {
        ui::message("Not Connected",
                    "Updating needs the internet. Turn wireless "
                    "communication on and try again.",
                    ui::Tone::Warning);
        return;
    }

    std::string listing;
    std::string error;
    bool listed = false;
    if (!tasks::busy("Software Update", "Getting the release's checksums...",
                     [&](tasks::Busy &busy) {
                         listed = updater::fetchChecksums(release, listing,
                                                          error, &busy.cancel);
                     },
                     true)) {
        return;
    }
    const std::string expected =
        listed ? update::checksumFor(listing, name) : "";
    if (expected.empty()) {
        ui::message("Update Not Installed",
                    listed ? "The release's checksum file does not list " +
                                 name + "."
                           : error,
                    ui::Tone::Error);
        return;
    }

    // The CIA waits in 3dsDL's own folder; the .3dsx downloads next to the
    // one it replaces, so putting it in place is a rename on the same card.
    const bool cia = updater::runningAsCia();
    const std::string target = cia ? std::string(storage::DataFolder) + name
                                   : updater::homebrewPath();
    const std::string folder = folderOf(target);
    if (!fs::makeDirectories(folder)) {
        ui::message("Update Not Installed",
                    "Could not create " + folder + ".", ui::Tone::Error);
        return;
    }

    net::Request request;
    request.url = asset->url;
    request.folder = folder;
    request.caFile = http::CaBundle;
    request.userAgent = http::UserAgent;
    request.allowResume = false;
    request.socketBufferSize = http::SocketBufferSize;
    request.connections = settings_.maxConnections;
    request.freeSpace = [](const std::string &) {
        return platform::sdFreeBytes();
    };
    const net::Result result = tasks::download(request);
    if (result.outcome == net::Outcome::Cancelled || !ui::keepRunning()) {
        return;
    }
    if (result.outcome != net::Outcome::Completed) {
        ui::message("The update could not be downloaded.", result.message,
                    ui::Tone::Error, result.detail);
        return;
    }

    std::string actual;
    tasks::busy("Software Update", "Checking the download...",
                [&]() { actual = sha256::ofFile(result.partPath); });
    if (actual != expected) {
        fs::removeFile(result.partPath);
        ui::message("Update Not Installed",
                    "The download did not match the release's checksum, so "
                    "nothing was changed. Try again.",
                    ui::Tone::Error,
                    "Expected " + expected + ", got " +
                        (actual.empty() ? "an unreadable file" : actual) + ".");
        return;
    }

    if (cia) {
        const finalize::Result placed =
            finalize::commit(result.partPath, folder, name, true, platform::now());
        if (!placed.ok) {
            fs::removeFile(placed.path);
            ui::message("Update Not Installed", placed.message, ui::Tone::Error);
            return;
        }
        cia::Info info;
        u64 running = 0;
        APT_GetProgramID(&running);
        if (!cia::readInfo(placed.path, info, error) ||
            (running != 0 && info.titleId != running)) {
            fs::removeFile(placed.path);
            ui::message("Update Not Installed",
                        "The downloaded CIA is not 3dsDL, so it was not "
                        "installed.",
                        ui::Tone::Error, error);
            return;
        }
        // Installing over the running title is FBI's special case: never
        // remove the installed copy first, and restart straight afterwards,
        // because this copy keeps reading its own files from a title that
        // has just been replaced.
        if (!installer::available()) {
            fs::removeFile(placed.path);
            ui::message("Update Not Installed", installer::unavailableReason(),
                        ui::Tone::Error);
            return;
        }
        const int64_t needed = installer::requiredSpace(placed.path, info);
        const int64_t free = platform::sdFreeBytes();
        if (needed > 0 && free >= 0 && needed > free) {
            fs::removeFile(placed.path);
            ui::message("Update Not Installed",
                        "The update needs " +
                            units::bytes(static_cast<uint64_t>(needed)) +
                            " on the SD card and " +
                            units::bytes(static_cast<uint64_t>(free)) +
                            " is free.",
                        ui::Tone::Error);
            return;
        }
        const installer::Result installed =
            tasks::install(placed.path, info.titleId, "3dsDL " + release.version,
                           info.smdh.icon);
        fs::removeFile(placed.path);
        if (installed.outcome != installer::Outcome::Installed) {
            if (installed.outcome == installer::Outcome::Failed) {
                ui::message("Update Not Installed", installed.message,
                            ui::Tone::Error);
            }
            return;
        }
        updateAvailable_ = false;

        ui::Dialog dialog;
        dialog.header = "Software Update";
        dialog.title = "3dsDL has been updated to " + release.version + ".";
        dialog.tone = ui::Tone::Success;
        dialog.buttons = {"Restart"};
        ui::choose(dialog);
        // Closing with the chainloader set starts the new version.
        aptSetChainloaderToSelf();
        quit_ = true;
        return;
    }

    // The running .3dsx also holds the app's own files (romfs), which stay
    // open while it runs. Let go of them so the file can be swapped, and
    // close afterwards so the Homebrew Launcher loads the new one.
    romfsExit();
    const finalize::Result placed =
        finalize::commit(result.partPath, folder, basename(target), true,
                         platform::now());
    if (!placed.ok || placed.path != target) {
        if (placed.path != target) {
            fs::removeFile(placed.path);
        }
        romfsInit();
        ui::message("Update Not Installed",
                    placed.message.empty()
                        ? "The new version could not be put in place of " +
                              target + "."
                        : placed.message,
                    ui::Tone::Error);
        return;
    }
    ui::Dialog dialog;
    dialog.header = "Software Update";
    dialog.title = "3dsDL has been updated to " + release.version + ".";
    dialog.tone = ui::Tone::Success;
    dialog.buttons = {"Close 3dsDL"};
    ui::choose(dialog);
    quit_ = true;
}
