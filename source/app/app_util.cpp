#include "app_util.hpp"

#include "installer.hpp"
#include "tasks.hpp"
#include "units.hpp"

#include <map>

namespace screens {
namespace {

// Asking AM is an IPC round trip, too slow for every frame, so answers are
// kept for a second.
bool installedCached(uint64_t titleId, uint16_t &version)
{
    struct Answer {
        uint64_t at;
        bool installed;
        uint16_t version;
    };
    static std::map<uint64_t, Answer> answers;
    const uint64_t now = osGetTime();
    auto found = answers.find(titleId);
    if (found == answers.end() || now - found->second.at > 1000) {
        Answer answer {now, false, 0};
        answer.installed = installer::installedVersion(titleId, answer.version);
        found = answers.insert_or_assign(titleId, answer).first;
    }
    version = found->second.version;
    return found->second.installed;
}

} // namespace

std::string basename(const std::string &path)
{
    return path.substr(path.find_last_of('/') + 1);
}

KindLook lookOf(filetype::Kind kind)
{
    switch (kind) {
    case filetype::Kind::Folder:
        return {ui::Icon::Folder, ui::color::IconOrange};
    case filetype::Kind::Cia:
        return {ui::Icon::Package, ui::color::IconBlue};
    case filetype::Kind::Ticket:
        return {ui::Icon::Lock, ui::color::IconGrey};
    case filetype::Kind::Homebrew:
        return {ui::Icon::App, ui::color::IconTeal};
    case filetype::Kind::Smdh:
        return {ui::Icon::Picture, ui::color::IconTeal};
    case filetype::Kind::CartridgeImage:
    case filetype::Kind::Nds:
        return {ui::Icon::GameCard, ui::color::IconRed};
    case filetype::Kind::Firm:
        return {ui::Icon::Chip, ui::color::IconGrey};
    case filetype::Kind::Image:
        return {ui::Icon::Picture, ui::color::IconPink};
    case filetype::Kind::Text:
        return {ui::Icon::Text, ui::color::IconGrey};
    case filetype::Kind::Archive:
        return {ui::Icon::Zip, ui::color::IconGold};
    case filetype::Kind::Audio:
        return {ui::Icon::Music, ui::color::IconPink};
    case filetype::Kind::Video:
        return {ui::Icon::Video, ui::color::IconPink};
    case filetype::Kind::Other:
        break;
    }
    return {ui::Icon::File, ui::color::IconGrey};
}

std::string plural(uint64_t count, const std::string &word)
{
    return units::grouped(count) + " " + word + (count == 1 ? "" : "s");
}

bool readArchive(const std::string &path, archives::Scan scan,
                 archives::Listing &listing, std::string &error)
{
    // The bar follows how far into the archive file the read has got.
    struct Reading : archives::Observer {
        tasks::Busy &busy;
        explicit Reading(tasks::Busy &busy) : busy(busy) {}
        bool cancelRequested() override { return busy.cancel.load(); }
        void onProgress(const archives::Progress &progress) override
        {
            busy.done = progress.archiveBytes;
        }
    };
    bool ok = false;
    error.clear();
    const bool finished = tasks::busy(
        "Archive", "Reading " + basename(path) + "...",
        [&](tasks::Busy &busy) {
            const int64_t size = fs::fileSize(path);
            busy.bytes = true;
            busy.total = size > 0 ? static_cast<uint64_t>(size) : 0;
            Reading observer(busy);
            ok = archives::read(path, scan, listing, error, &observer);
        },
        true);
    if (!finished) {
        error.clear();
        return false;
    }
    return ok;
}

std::string sortDescription(fs::SortOrder order, bool reverse)
{
    switch (order) {
    case fs::SortOrder::Size:
        return reverse ? "Smallest First" : "Largest First";
    case fs::SortOrder::Date:
        return reverse ? "Oldest First" : "Newest First";
    case fs::SortOrder::Type:
        return reverse ? "Type, Z to A" : "Type, A to Z";
    case fs::SortOrder::Name:
        break;
    }
    return reverse ? "Name, Z to A" : "Name, A to Z";
}

void drawLabelledPanel(float y, float height, const std::string &label,
                       const std::string &value, u32 valueColor)
{
    ui::drawPanel({18, y, 364, height});
    ui::drawText(label, 34, y + 9, 0.44f, ui::color::Muted);
    ui::drawWrapped(value, 34, y + 27, 0.52f, valueColor, 330,
                    static_cast<size_t>((height - 30) / ui::lineHeight(0.52f)));
}

std::string ciaName(const cia::Info &info)
{
    return info.smdh.shortName.empty()
        ? "title " + cia::titleIdText(info.titleId)
        : info.smdh.shortName;
}

void addCiaParagraphs(ui::Dialog &dialog, const cia::Info &info)
{
    if (!info.smdh.publisher.empty()) {
        dialog.body.push_back({info.smdh.publisher, ui::color::Muted, 0.46f,
                               1});
    }
    dialog.body.push_back(
        {std::string(cia::kindName(cia::kind(info.titleId))) + ", " +
             titleVersion(info.titleId, info.version) + ", " +
             units::bytes(info.fileSize),
         ui::color::Text, 0.46f, 2});
    uint16_t installed = 0;
    if (installer::installedVersion(info.titleId, installed)) {
        dialog.body.push_back({"Installed now: " +
                                   titleVersion(info.titleId, installed),
                               installed > info.version ? ui::color::Warning
                                                        : ui::color::Muted,
                               0.44f, 1});
    }
}

void drawCiaTop(const cia::Info &info, const std::string &title,
                ui::Icon icon, u32 iconColor)
{
    ui::drawTopChrome(title, icon, iconColor);
    ui::drawPanel();
    const float left = ui::TopPanel.x + 18;
    const float top = ui::TopPanel.y + 12;
    ui::drawTitleIcon(info.smdh.icon, left, top);
    ui::drawMarquee(ciaName(info), left + 60, top + 2, 0.62f,
                    ui::color::InkStrong, 280);
    ui::drawMarquee(info.smdh.publisher.empty()
                        ? cia::kindName(cia::kind(info.titleId))
                        : info.smdh.publisher,
                    left + 60, top + 30, ui::type::Small, ui::color::InkSoft,
                    280);
    float y = top + 60;
    y = ui::drawTagRow(left, y, 76, "Title ID", cia::titleIdText(info.titleId));
    uint16_t installed = 0;
    const bool isInstalled = installedCached(info.titleId, installed);
    y = ui::drawTagRow(left, y, 76, "Version",
                       versionRow(info.titleId, info.version, isInstalled,
                                  installed));
    ui::drawTagRow(left, y, 76, "Size",
                   units::bytes(info.fileSize) + ", " +
                       (cia::installsToNand(info.titleId) ? "system memory"
                                                          : "SD card"));
}

std::string titleVersion(uint64_t titleId, uint16_t version)
{
    return cia::versionText(titleId, version, APP_VERSION);
}

std::string versionRow(uint64_t titleId, uint16_t version, bool isInstalled,
                       uint16_t installed)
{
    // "1.2.0 (v1056)" without the raw number, so the state fits beside it.
    const auto shortText = [titleId](uint16_t value) {
        const std::string text = titleVersion(titleId, value);
        return text.substr(0, text.find(' '));
    };
    if (!isInstalled) {
        return shortText(version) + ", not installed";
    }
    if (installed == version) {
        return shortText(version) + ", installed";
    }
    return shortText(version) + ", installed " + shortText(installed);
}

} // namespace screens
