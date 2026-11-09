#include "app.hpp"

#include "app_util.hpp"
#include "fs_util.hpp"
#include "installer.hpp"
#include "library.hpp"
#include "saves.hpp"
#include "storage.hpp"
#include "tasks.hpp"
#include "ui.hpp"
#include "text.hpp"
#include "units.hpp"

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <map>
#include <set>

// Titles: one list of installed software, each with its update, DLC, save
// data, extra data and tickets, and what is left of software that is gone;
// a page for each, and the jobs for all of them under Options.

namespace {

using screens::plural;

// Where a seeddb.bin is usually left: by hand, by GodMode9, or for 3dsDL.
const char *const SeedDbPlaces[] = {
    "sdmc:/seeddb.bin",
    "sdmc:/3ds/seeddb.bin",
    "sdmc:/3ds/3dsDL/seeddb.bin",
    "sdmc:/gm9/out/seeddb.bin",
};

const char *placeName(titles::Place place)
{
    switch (place) {
    case titles::Place::SystemMemory:
        return "system memory";
    case titles::Place::GameCard:
        return "game card";
    case titles::Place::SdCard:
        break;
    }
    return "SD card";
}

ui::Icon iconFor(uint64_t titleId)
{
    switch (cia::kind(titleId)) {
    case cia::Kind::Update:
        return ui::Icon::Download;
    case cia::Kind::Dlc:
        return ui::Icon::Package;
    case cia::Kind::DsiWare:
        return ui::Icon::App;
    default:
        return ui::Icon::GameCard;
    }
}

// No body or icon, so the top screen keeps showing what is being deleted.
bool confirmDanger(const std::string &header, const std::string &title,
                   const std::string &button)
{
    ui::Dialog dialog;
    dialog.header = header;
    dialog.title = title;
    dialog.tone = ui::Tone::Warning;
    dialog.buttons = {button, "Cancel"};
    dialog.dangerous = 0;
    dialog.initial = 1;
    dialog.cancelButton = 1;
    return ui::choose(dialog) == 0;
}

} // namespace

void App::startLibrary()
{
    libraryLoader_.stop();
    library_ = library::Snapshot();
    libraryLoader_.start();
    ++libraryGeneration_;
}

void App::pollLibrary()
{
    if (libraryLoader_.take(library_)) {
        ++libraryGeneration_;
        // Kept for when the software is gone and only a ticket or extra
        // data is left to name.
        if (library_.complete && titles::rememberNames(knownNames_, library_.titles) &&
            storage::saveNames(knownNames_)) {
            sdChanged(storage::DataFolder);
        }
    }
}

bool App::reloadLibrary()
{
    startLibrary();
    return waitForLibrary();
}

namespace {

// Checkpoint's backup folder names, which name a title by its unique ID.
std::vector<std::string> checkpointFolders()
{
    std::vector<std::string> folders;
    for (const char *root : {saves::SavesRoot, saves::ExtDataRoot}) {
        for (const fs::Entry &entry : fs::listEntries(root, nullptr, true, false)) {
            if (entry.folder) {
                folders.push_back(entry.name);
            }
        }
    }
    return folders;
}

// A CIA on the SD card that would bring a title's ticket back, where its
// files are usually kept: its path and the name inside it.
struct FoundCia {
    std::string path;
    std::string name;
};

std::map<uint64_t, FoundCia> ciasOnCard(const std::string &downloadFolder,
                                        const std::atomic<bool> &stop)
{
    std::map<uint64_t, FoundCia> found;
    std::set<std::string> seen;
    for (const std::string &folder : {std::string("sdmc:/cias/"), downloadFolder}) {
        if (!seen.insert(text::lower(folder)).second) {
            continue;
        }
        for (const fs::Entry &entry : fs::listEntries(folder, nullptr, false, false)) {
            if (stop.load()) {
                return found;
            }
            if (entry.folder || !text::endsWithNoCase(entry.name, ".cia")) {
                continue;
            }
            cia::Info info;
            std::string error;
            const std::string path = folder + entry.name;
            if (cia::readInfo(path, info, error, true) && found.count(info.titleId) == 0) {
                found[info.titleId] = {path, info.smdh.shortName};
            }
        }
    }
    return found;
}

} // namespace

std::string App::softwareName(uint64_t titleId, const titles::Index &index,
                              const std::vector<std::string> &folders) const
{
    const std::string shown = titles::displayName(titleId, index);
    if (shown != cia::titleIdText(titleId)) {
        return shown;
    }
    const std::string remembered = titles::rememberedName(titleId, knownNames_);
    if (!remembered.empty()) {
        return remembered;
    }
    for (const std::string &folder : folders) {
        const std::string name = saves::nameFromFolder(cia::baseTitle(titleId), folder);
        if (!name.empty()) {
            return name;
        }
    }
    return "";
}

bool App::confirmUnsavedTickets(const std::vector<uint64_t> &tickets,
                                const std::string &why,
                                std::map<uint64_t, library::TicketFacts> &shown)
{
    struct Known {
        uint64_t id = 0;
        std::string name;
        library::TicketFacts facts;
        std::string cia; // a CIA that brings it back, or ""
    };
    std::vector<Known> known;
    shown.clear();
    // Reading which tickets were bought here goes through every ticket, so
    // it is read once for all of them. Only reads, so it can stop.
    const bool looked = tasks::busy(
        "Tickets", "Looking at the tickets...",
        [&](tasks::Busy &busy) {
            const titles::Index index = titles::indexOf(library_.titles);
            const std::vector<std::string> folders = checkpointFolders();
            const std::set<uint64_t> personal = library::personalTickets();
            const std::map<uint64_t, FoundCia> cias =
                ciasOnCard(settings_.destination, busy.cancel);
            // Counted from here: the reads above have no count of their own,
            // and a bar stuck at none of them for their seconds says less
            // than the spinner.
            busy.total = tickets.size();
            for (const uint64_t id : tickets) {
                if (busy.cancel.load()) {
                    return;
                }
                Known ticket;
                ticket.id = id;
                ticket.name = softwareName(id, index, folders);
                ticket.facts = library::ticketFacts(id, personal);
                const auto cia = cias.find(id);
                if (cia != cias.end()) {
                    ticket.cia = cia->second.path;
                    if (ticket.name.empty()) {
                        ticket.name = cia->second.name;
                    }
                }
                known.push_back(ticket);
                busy.done.fetch_add(1);
            }
        },
        true);
    if (!looked || known.size() != tickets.size()) {
        return false;
    }
    size_t bought = 0;
    for (const Known &ticket : known) {
        bought += ticket.facts.personal ? 1 : 0;
    }
    const auto nameOf = [](const Known &ticket) {
        return ticket.name.empty() ? cia::titleIdText(ticket.id) : ticket.name;
    };
    // The console's own reason, without the sentence around it.
    const size_t code = why.find("Error ");
    const std::string reason = code == std::string::npos ? why : why.substr(code);

    ui::Dialog dialog;
    dialog.header = "Tickets";
    dialog.title =
        known.size() == 1
            ? "Delete the ticket for " + nameOf(known[0]) + " without a backup?"
            : "Delete " + plural(known.size(), "ticket") + " without a backup?" +
                  (bought > 0 ? " " + std::to_string(bought) + " of them " +
                                    (bought == 1 ? "was" : "were") +
                                    " bought on this 3DS."
                              : "");
    dialog.tone = ui::Tone::Warning;
    dialog.buttons = {"Delete Anyway", "Keep"};
    dialog.dangerous = 0;
    dialog.initial = 1;
    dialog.cancelButton = 1;
    dialog.drawTop = [&]() {
        ui::drawTopChrome("Tickets", ui::Icon::Lock, ui::color::IconGrey);
        ui::drawPanel();
        std::vector<ui::Fact> facts;
        if (known.size() == 1) {
            const Known &ticket = known[0];
            facts.push_back({"Software", ticket.name.empty() ? "Unknown" : ticket.name});
            facts.push_back({"Title ID", cia::titleIdText(ticket.id)});
            facts.push_back({"Bought here", ticket.facts.personal ? "Yes" : "No",
                             ticket.facts.personal ? ui::color::Bad : ui::color::Tag});
            if (ticket.facts.known) {
                facts.push_back({"Issued for", screens::titleVersion(ticket.id,
                                                                     ticket.facts.version)});
            }
            facts.push_back({"Restore", ticket.cia.empty() ? "No CIA for it found"
                                                           : "Install " + ticket.cia});
            facts.push_back({"Backup", "Not possible: " + reason, ui::color::Neutral});
        } else {
            for (size_t index = 0; index < known.size() && index < 5; ++index) {
                const Known &ticket = known[index];
                facts.push_back({ticket.facts.personal ? "Bought here"
                                 : ticket.cia.empty()  ? "No CIA"
                                                       : "CIA found",
                                 nameOf(ticket),
                                 ticket.facts.personal ? ui::color::Bad : ui::color::Tag});
            }
            if (known.size() > 5) {
                facts.push_back({"And", std::to_string(known.size() - 5) + " more",
                                 ui::color::Neutral});
            }
        }
        ui::drawFacts(facts);
    };
    if (ui::choose(dialog) != 0) {
        return false;
    }
    for (const Known &ticket : known) {
        shown[ticket.id] = ticket.facts;
    }
    return true;
}

void App::deleteUnusedTickets()
{
    // Decided again from scratch, never from what the screens showed: a
    // misread list of installed software makes the tickets games need to
    // start look unused (a user lost 256 that way).
    std::vector<uint64_t> unused;
    std::string error;
    bool checked = false;
    tasks::busy("Tickets", "Checking which tickets are unused...",
                [&]() { checked = library::checkUnusedTickets(unused, error); });
    if (!checked) {
        ui::message("Nothing Deleted", error, ui::Tone::Error);
        reloadLibrary();
        return;
    }
    if (unused.empty()) {
        ui::message("Nothing to Delete", "Every ticket belongs to installed "
                                         "software.");
        reloadLibrary();
        return;
    }
    if (!confirmDanger("Delete",
                       "Delete " + plural(unused.size(), "unused ticket") + "?",
                       "Delete")) {
        return;
    }
    size_t deleted = 0;
    // Those kept only because they could not be saved first, and why.
    std::vector<uint64_t> unsaved;
    std::string unsavedWhy;
    tasks::busy(
        "Delete", "Deleting unused tickets...",
        [&](tasks::Busy &busy) {
            busy.total = unused.size();
            for (const uint64_t ticket : unused) {
                // Once more just before it goes, in case something was
                // installed meanwhile.
                bool couldNotSave = false;
                if (!library::installedAnywhere(ticket) &&
                    library::deleteTicket(ticket, error, &couldNotSave)) {
                    ++deleted;
                } else if (couldNotSave) {
                    unsaved.push_back(ticket);
                    unsavedWhy = error;
                }
                busy.done.fetch_add(1);
            }
        },
        false);
    // Asked once for all of them, with what each was.
    std::map<uint64_t, library::TicketFacts> shown;
    if (!unsaved.empty() && confirmUnsavedTickets(unsaved, unsavedWhy, shown)) {
        tasks::busy(
            "Delete", "Deleting tickets without a backup...",
            [&](tasks::Busy &busy) {
                busy.total = unsaved.size();
                for (const uint64_t ticket : unsaved) {
                    if (!library::installedAnywhere(ticket) &&
                        library::deleteTicket(ticket, error, nullptr,
                                              &shown[ticket])) {
                        ++deleted;
                    }
                    busy.done.fetch_add(1);
                }
            },
            false);
    }
    if (deleted == unused.size()) {
        ui::message("Tickets Deleted", plural(deleted, "unused ticket") +
                                           " deleted.",
                    ui::Tone::Success);
    } else {
        ui::message("Some Tickets Remain",
                    plural(deleted, "ticket") + " of " +
                        std::to_string(unused.size()) + " deleted.",
                    ui::Tone::Warning, error);
    }
    reloadLibrary();
}

void App::importSeeds()
{
    std::string path;
    for (const char *place : SeedDbPlaces) {
        if (fs::fileExists(place)) {
            path = place;
            break;
        }
    }
    if (path.empty()) {
        ui::message("No seeddb.bin Found",
                    "Put a seeddb.bin in sdmc:/, sdmc:/3ds/ or "
                    "sdmc:/3ds/3dsDL/ and try again.",
                    ui::Tone::Info);
        return;
    }
    std::string data;
    std::vector<titles::SeedEntry> entries;
    std::string error;
    if (!fs::readFile(path, data) || !titles::parseSeedDb(data, entries, error)) {
        ui::message("Seeds Not Imported",
                    error.empty() ? "Could not read " + path + "." : error,
                    ui::Tone::Error, path);
        return;
    }
    const std::vector<titles::SeedEntry> wanted =
        titles::seedsFor(entries, library_.titles);
    if (wanted.empty()) {
        ui::message("No Seeds Needed",
                    "None of the games installed here are in " + path + " (" +
                        plural(entries.size(), "seed") + ").");
        return;
    }
    ui::Dialog dialog;
    dialog.header = "Seeds";
    dialog.title = "Add seeds for " + plural(wanted.size(), "installed game") +
                   " from " + path + "?";
    dialog.tone = ui::Tone::Info;
    dialog.buttons = {"Add", "Cancel"};
    dialog.cancelButton = 1;
    if (ui::choose(dialog) != 0) {
        return;
    }
    size_t added = 0;
    tasks::busy(
        "Seeds", "Adding seeds...",
        [&](tasks::Busy &busy) {
            busy.total = wanted.size();
            for (const titles::SeedEntry &entry : wanted) {
                if (installer::addSeed(entry.titleId, entry.seed.data(), error)) {
                    ++added;
                }
                busy.done.fetch_add(1);
            }
        },
        false);
    ui::message(added == wanted.size() ? "Seeds Added" : "Some Seeds Not Added",
                plural(added, "seed") + " added.",
                added == wanted.size() ? ui::Tone::Success : ui::Tone::Warning,
                error);
}

// --- The Titles list -------------------------------------------------------------

namespace {

// Names a row the same way while the list fills in and after it changes.
std::string titleKey(const titles::Group &group)
{
    if (group.id != 0) {
        return cia::titleIdText(group.id);
    }
    char text[24];
    std::snprintf(text, sizeof(text), "X%016llX",
                  static_cast<unsigned long long>(group.extData));
    return text;
}

const library::ExtData *extDataById(const library::Snapshot &snapshot, uint64_t id)
{
    for (const library::ExtData &extData : snapshot.extData) {
        if (extData.id == id) {
            return &extData;
        }
    }
    return nullptr;
}

// What a row that is not installed software shows in place of its size.
struct Leftover {
    const char *badge;
    u32 color;
    ui::Icon icon;
};

Leftover leftoverOf(const titles::Group &group)
{
    if (!group.pending.empty()) {
        return {"Unfinished", ui::color::Badge, ui::Icon::Warning};
    }
    if (group.update != nullptr) {
        return {"Update", ui::color::Tag, ui::Icon::Download};
    }
    if (group.dlc != nullptr) {
        return {"DLC", ui::color::Good, ui::Icon::Package};
    }
    const ui::Icon icon = group.tickets.empty() ? ui::Icon::Folder : ui::Icon::Lock;
    if (group.unused) {
        return {"Unused", ui::color::Neutral, icon};
    }
    return {group.tickets.empty() ? "Extra Data" : "Ticket", ui::color::Neutral, icon};
}

// What is left of software that is gone: "Ticket, extra data".
std::string leftParts(const titles::Group &group)
{
    std::string out;
    const auto add = [&](const std::string &part) {
        out += (out.empty() ? "" : ", ") + part;
    };
    if (group.update != nullptr) {
        add("update");
    }
    if (group.dlc != nullptr) {
        add("DLC");
    }
    if (!group.tickets.empty()) {
        add(group.tickets.size() == 1 ? "ticket" : plural(group.tickets.size(), "ticket"));
    }
    if (group.hasExtData) {
        add("extra data");
    }
    if (!group.pending.empty()) {
        add("unfinished install");
    }
    if (!out.empty() && out[0] >= 'a' && out[0] <= 'z') {
        out[0] = static_cast<char>(out[0] - 'a' + 'A');
    }
    return out;
}

std::string backupsText(size_t count)
{
    return count == 0 ? "" : plural(count, "backup");
}

// A row's icon, name and publisher over its facts; order is the list's,
// when it is in one.
void drawGroupTop(const std::string &header, const titles::Group &group,
                  const library::Snapshot &snapshot, const std::string &order = "")
{
    ui::drawTopChrome(header, ui::Icon::GameCard, ui::color::IconBlue, order);
    ui::drawPanel();
    const float left = ui::TopPanel.x + 18;
    const float top = ui::TopPanel.y + 12;
    const titles::Title *software = group.software;
    const library::ExtData *extData =
        group.hasExtData ? extDataById(snapshot, group.extData) : nullptr;
    static const std::vector<uint16_t> NoIcon;
    const std::vector<uint16_t> &icon =
        software != nullptr && !software->icon.empty() ? software->icon
        : extData != nullptr                           ? extData->icon
                                                       : NoIcon;
    ui::drawTitleIcon(icon, left, top,
                      software != nullptr ? iconFor(software->id)
                                          : leftoverOf(group).icon);
    ui::drawMarquee(group.name, left + 60, top + 2, 0.62f, ui::color::InkStrong, 280);
    std::string second;
    if (software != nullptr) {
        second = software->publisher.empty() ? cia::kindName(cia::kind(software->id))
                                             : software->publisher;
        if (!software->productCode.empty()) {
            second += "   " + software->productCode;
        }
    } else {
        second = "Not installed";
    }
    ui::drawMarquee(second, left + 60, top + 30, ui::type::Small, ui::color::InkSoft, 280);
    float y = top + 60;
    if (group.id != 0) {
        y = ui::drawTagRow(left, y, 76, "Title ID", cia::titleIdText(group.id));
    } else {
        char id[20];
        std::snprintf(id, sizeof(id), "%08llX",
                      static_cast<unsigned long long>(group.extData));
        y = ui::drawTagRow(left, y, 76, "Extra data", id);
    }
    if (software != nullptr) {
        const titles::Title &newest = group.update != nullptr ? *group.update : *software;
        y = ui::drawTagRow(left, y, 76, "Version",
                           screens::titleVersion(newest.id, newest.version));
        ui::drawTagRow(left, y, 76, "Size",
                       units::bytes(group.size()) + ", " + placeName(software->place));
        return;
    }
    y = ui::drawTagRow(left, y, 76, "Left", leftParts(group));
    if (group.size() > 0) {
        ui::drawTagRow(left, y, 76, "Size", units::bytes(group.size()));
    }
}

} // namespace

bool App::waitForLibrary()
{
    std::string error;
    while (ui::keepRunning() && !library_.complete &&
           !libraryLoader_.failed(error)) {
        ui::readInput();
        ui::BusyView view;
        view.header = "Titles";
        view.title = "Reading the installed software...";
        const uint64_t total = libraryLoader_.total();
        if (total > 0) {
            view.progress = true;
            view.fraction = static_cast<float>(libraryLoader_.done()) / total;
            view.amount = std::to_string(libraryLoader_.done()) + " of " +
                          std::to_string(total);
        }
        ui::drawBusy(view); // its frame hook takes what arrived
        gspWaitForVBlank();
    }
    if (!library_.complete && libraryLoader_.failed(error)) {
        ui::message("Titles Unavailable", error, ui::Tone::Error);
    }
    return library_.complete;
}

std::vector<titles::Group> App::titleGroups(const titles::Index &index,
                                            const std::vector<std::string> &folders) const
{
    std::vector<uint64_t> pending;
    for (const library::PendingTitle &title : library_.pending) {
        pending.push_back(title.id);
    }
    std::vector<uint64_t> extData;
    std::set<uint64_t> unusedExtData;
    for (const library::ExtData &each : library_.extData) {
        extData.push_back(each.id);
        if (!library::extDataInUse(library_, each.id)) {
            unusedExtData.insert(each.id);
        }
    }
    const std::vector<uint64_t> unusedList = library::unusedTickets(library_);
    std::vector<titles::Group> groups = titles::groupTitles(
        library_.titles, library_.tickets, pending, extData,
        std::set<uint64_t>(unusedList.begin(), unusedList.end()), unusedExtData);
    for (titles::Group &group : groups) {
        if (group.software != nullptr) {
            group.name = titles::displayName(*group.software, index);
        } else if (group.id != 0) {
            group.name = softwareName(group.id, index, folders);
        }
        if (group.name.empty() && group.hasExtData) {
            const library::ExtData *own = extDataById(library_, group.extData);
            if (own != nullptr) {
                group.name = own->name;
            }
        }
        if (group.name.empty()) {
            char text[24];
            std::snprintf(text, sizeof(text), "Extra data %08llX",
                          static_cast<unsigned long long>(group.extData));
            group.name = group.id != 0 ? cia::titleIdText(group.id) : text;
        }
    }
    return groups;
}

void App::titlesScreen()
{
    // Opens at once while the titles are read in the background; the list
    // fills in as they arrive, and what is left of software that is gone
    // joins it once everything is read.
    startLibrary();
    struct StopReading {
        library::Loader &loader;
        ~StopReading() { loader.stop(); }
    } stopReading {libraryLoader_};
    const std::vector<std::string> folders = checkpointFolders();

    int cursor = 0;
    std::string follow; // a row to land on once the list is built again
    while (ui::keepRunning() && !quit_) {
        std::vector<titles::Group> shown;
        ui::ListSpec spec;
        spec.title = "Titles";
        spec.emptyText = "No software installed.";
        spec.headingActions = {{"Sort", "X", KEY_X}};
        spec.action = "Options";
        spec.actionKey = "Y";
        spec.actionKeys = KEY_Y;
        uint32_t seen = libraryGeneration_ - 1;
        spec.update = [&]() {
            if (seen == libraryGeneration_) {
                return false;
            }
            seen = libraryGeneration_;
            const titles::Index index = titles::indexOf(library_.titles);
            shown.clear();
            for (titles::Group &group : titleGroups(index, folders)) {
                if (softwareShow_ == titles::Show::All ||
                    (group.software != nullptr &&
                     titles::shows(softwareShow_, *group.software))) {
                    shown.push_back(std::move(group));
                }
            }
            titles::sortGroups(shown, settings_.titleSort, settings_.titleSortReverse);
            spec.items.clear();
            size_t software = 0;
            uint64_t bytes = 0;
            for (const titles::Group &group : shown) {
                ui::ListItem item;
                item.key = titleKey(group);
                item.label = group.name;
                if (group.software != nullptr) {
                    item.icon = iconFor(group.software->id);
                    item.iconColor = ui::color::IconBlue;
                    item.detail = units::bytes(group.size());
                    ++software;
                    bytes += group.size();
                } else {
                    const Leftover leftover = leftoverOf(group);
                    item.icon = leftover.icon;
                    item.iconColor = ui::color::IconGrey;
                    item.badge = leftover.badge;
                    item.badgeColor = leftover.color;
                    item.labelColor = ui::color::InkSoft;
                }
                spec.items.push_back(item);
            }
            spec.heading = plural(software, "title") + ", " + units::bytes(bytes);
            return true;
        };
        spec.update();
        for (size_t row = 0; !follow.empty() && row < shown.size(); ++row) {
            if (titleKey(shown[row]) == follow) {
                cursor = static_cast<int>(row);
            }
        }
        follow.clear();
        spec.loading = [&]() {
            std::string ignored;
            return !library_.complete && !libraryLoader_.failed(ignored);
        };
        spec.drawTop = [&](int highlighted) {
            const std::string order = screens::sortDescription(
                settings_.titleSort, settings_.titleSortReverse);
            if (highlighted < 0 || highlighted >= static_cast<int>(shown.size())) {
                ui::drawTopChrome("Titles", ui::Icon::GameCard, ui::color::IconBlue,
                                  order);
                ui::drawPanel();
                ui::drawEmblem(ui::Icon::GameCard, ui::color::IconBlue);
                return;
            }
            drawGroupTop("Titles", shown[highlighted], library_, order);
        };
        const ui::ListResult result = ui::runList(spec, cursor);
        const bool onRow =
            result.index >= 0 && result.index < static_cast<int>(shown.size());
        if (onRow) {
            follow = titleKey(shown[result.index]);
        }
        if (result.key == KEY_X) {
            titleSortScreen();
            continue;
        }
        if (result.key == KEY_Y) {
            titlesOptions();
            continue;
        }
        if (result.key != KEY_A || !onRow) {
            return;
        }
        titlePage(follow, folders);
    }
}

void App::titleSortScreen()
{
    static const struct {
        titles::Show show;
        const char *label;
    } Shows[] = {{titles::Show::All, "All"},
                 {titles::Show::SdCard, "SD Card"},
                 {titles::Show::SystemMemory, "System Memory"}};
    int focus = settings_.titleSort == fs::SortOrder::Size ? 1 : 0;
    // Picks apply at once and the screen stays, like the files' Sort.
    while (ui::keepRunning() && !quit_) {
        ui::ScreenSpec spec;
        spec.back = "Back";
        const fs::SortOrder orders[] = {fs::SortOrder::Name, fs::SortOrder::Size};
        for (int index = 0; index < 2; ++index) {
            ui::Control control;
            control.rect = {index == 0 ? 26.0f : 164.0f, 29, 130, 40};
            control.label = index == 0 ? "Name" : "Size";
            control.style = settings_.titleSort == orders[index]
                                ? ui::Style::Chosen
                                : ui::Style::Normal;
            spec.controls.push_back(control);
        }
        ui::Control reverse;
        reverse.rect = {26, 81, 268, 40};
        reverse.label = "Reverse Order";
        reverse.style = settings_.titleSortReverse ? ui::Style::Chosen
                                                   : ui::Style::Normal;
        spec.controls.push_back(reverse);
        for (int index = 0; index < 3; ++index) {
            ui::Control control;
            control.rect = {26.0f + index * 92.0f, 139, 84, 40};
            control.label = Shows[index].label;
            control.style = Shows[index].show == softwareShow_ ? ui::Style::Chosen
                                                               : ui::Style::Normal;
            spec.controls.push_back(control);
        }
        spec.drawTop = [&]() {
            ui::drawTopChrome("Sort", ui::Icon::GameCard, ui::color::IconBlue);
            ui::drawPanel();
            std::string showing = "All";
            for (const auto &each : Shows) {
                if (each.show == softwareShow_) {
                    showing = each.label;
                }
            }
            ui::drawFacts({{"Order", screens::sortDescription(settings_.titleSort,
                                                              settings_.titleSortReverse)},
                           {"Showing", showing}});
        };
        const ui::ScreenResult result = ui::runScreen(spec, focus);
        if (result.choice == 0 || result.choice == 1) {
            settings_.titleSort = orders[result.choice];
            persistSettings();
        } else if (result.choice == 2) {
            settings_.titleSortReverse = !settings_.titleSortReverse;
            persistSettings();
        } else if (result.choice >= 3 && result.choice < 6) {
            softwareShow_ = Shows[result.choice - 3].show;
        } else {
            return;
        }
    }
}

void App::titlePage(const std::string &key, const std::vector<std::string> &folders)
{
    enum class Verb {
        Launch,
        Saves,
        Extra,
        Seed,
        Export,
        RemovePending,
        DeleteUpdate,
        DeleteDlc,
        DeleteExtra,
        DeleteTickets,
        DeleteSoftware
    };
    // Whether software has save data is asked once: it opens the save.
    std::map<uint64_t, bool> hasSave;
    int cursor = 0;
    while (ui::keepRunning() && !quit_) {
        titles::Group group;
        bool gone = false;
        std::vector<Verb> verbs;
        savedata::Target saveTarget;
        savedata::Target extTarget;
        std::vector<uint64_t> loose; // tickets of what is not installed
        ui::ListSpec spec;
        spec.emptyText = "Nothing to change.";
        // Built again as the library fills in, so rows for tickets and
        // extra data join once they are read.
        uint32_t seen = libraryGeneration_ - 1;
        spec.update = [&]() {
            if (seen == libraryGeneration_) {
                return false;
            }
            seen = libraryGeneration_;
            const titles::Index index = titles::indexOf(library_.titles);
            std::vector<titles::Group> groups = titleGroups(index, folders);
            const auto found = std::find_if(groups.begin(), groups.end(),
                                            [&](const titles::Group &each) {
                                                return titleKey(each) == key;
                                            });
            if (found == groups.end()) {
                gone = true;
                return false;
            }
            group = std::move(*found);
            spec.title = group.name;
            spec.items.clear();
            verbs.clear();
            loose.clear();
            const auto add = [&](Verb verb, const std::string &label, ui::Icon icon,
                                 u32 iconColor, const std::string &detail = "",
                                 bool danger = false) {
                ui::ListItem item;
                item.key = label;
                item.label = label;
                item.icon = icon;
                item.iconColor = iconColor;
                item.detail = detail;
                if (danger) {
                    item.labelColor = ui::color::Bad;
                }
                spec.items.push_back(item);
                verbs.push_back(verb);
            };
            const titles::Title *software = group.software;
            if (software != nullptr) {
                const cia::Kind kind = cia::kind(software->id);
                const bool self = software->id == library::runningTitle();
                if ((kind == cia::Kind::Application || kind == cia::Kind::Demo ||
                     kind == cia::Kind::DsiWare) &&
                    !self) {
                    add(Verb::Launch, "Launch", ui::Icon::Play, ui::color::IconGreen);
                }
                saveTarget = savedata::saveOf(*software, library::mediaOf(*software));
                if (savedata::unsupportedReason(*software).empty()) {
                    auto known = hasSave.find(software->id);
                    if (known == hasSave.end()) {
                        known = hasSave.emplace(software->id,
                                                savedata::exists(saveTarget)).first;
                    }
                    if (known->second) {
                        add(Verb::Saves, "Save Data", ui::Icon::Chip, ui::color::IconTeal,
                            backupsText(savedata::backups(saveTarget).size()));
                    }
                }
            }
            const library::ExtData *extData =
                group.hasExtData ? extDataById(library_, group.extData) : nullptr;
            if (group.hasExtData && (software != nullptr || extData != nullptr)) {
                extTarget = software != nullptr
                                ? savedata::extDataOf(*software)
                                : savedata::extDataOf(extData->id,
                                                      extData->englishShortName,
                                                      group.name);
                add(Verb::Extra, "Extra Data", ui::Icon::Folder, ui::color::IconOrange,
                    backupsText(savedata::backups(extTarget).size()));
            }
            if (software != nullptr &&
                cia::kind(software->id) == cia::Kind::Application) {
                add(Verb::Seed, "Get Seed", ui::Icon::Seed, ui::color::IconGreen);
            }
            if (!group.tickets.empty()) {
                add(Verb::Export,
                    group.tickets.size() == 1
                        ? "Export Ticket"
                        : "Export " + std::to_string(group.tickets.size()) + " Tickets",
                    ui::Icon::Lock, ui::color::IconGrey);
            }
            // Deleting comes last, in red.
            if (!group.pending.empty()) {
                add(Verb::RemovePending, "Remove Unfinished Install", ui::Icon::Trash,
                    ui::color::IconRed, "", true);
            }
            if (group.update != nullptr) {
                add(Verb::DeleteUpdate, "Delete Update", ui::Icon::Trash,
                    ui::color::IconRed, units::bytes(group.update->size), true);
            }
            if (group.dlc != nullptr) {
                add(Verb::DeleteDlc, "Delete DLC", ui::Icon::Trash, ui::color::IconRed,
                    units::bytes(group.dlc->size), true);
            }
            if (group.hasExtData) {
                add(Verb::DeleteExtra, "Delete Extra Data", ui::Icon::Trash,
                    ui::color::IconRed, "", true);
            }
            // An installed title's own ticket only goes with it.
            for (const uint64_t ticket : group.tickets) {
                bool own = false;
                for (const titles::Title *part : {software, group.update, group.dlc}) {
                    own = own || (part != nullptr && part->id == ticket);
                }
                if (!own) {
                    loose.push_back(ticket);
                }
            }
            if (!loose.empty()) {
                add(Verb::DeleteTickets,
                    loose.size() == 1
                        ? "Delete Ticket"
                        : "Delete " + std::to_string(loose.size()) + " Tickets",
                    ui::Icon::Trash, ui::color::IconRed, "", true);
            }
            if (software != nullptr && software->place != titles::Place::GameCard &&
                software->id != library::runningTitle()) {
                add(Verb::DeleteSoftware, "Delete Software", ui::Icon::Trash,
                    ui::color::IconRed, units::bytes(software->size), true);
            }
            return true;
        };
        spec.update();
        if (gone) {
            return;
        }
        spec.close = [&]() { return gone; };
        spec.drawTop = [&](int) { drawGroupTop(group.name, group, library_); };
        const ui::ListResult result = ui::runList(spec, cursor);
        if (result.key != KEY_A || result.index < 0 ||
            result.index >= static_cast<int>(verbs.size())) {
            return;
        }
        const std::string name = group.name;
        std::string error;
        switch (verbs[result.index]) {
        case Verb::Launch:
            if (!installer::launch(group.software->id, error)) {
                ui::message("Could Not Launch", error, ui::Tone::Error);
            }
            break;
        case Verb::Saves:
            saveDataScreen(saveTarget, group.software->icon);
            break;
        case Verb::Extra: {
            const library::ExtData *extData = extDataById(library_, group.extData);
            saveDataScreen(extTarget,
                           group.software != nullptr && !group.software->icon.empty()
                               ? group.software->icon
                           : extData != nullptr ? extData->icon
                                                : std::vector<uint16_t>());
            break;
        }
        case Verb::Seed: {
            std::string message;
            bool added = false;
            const uint64_t id = group.software->id;
            if (!tasks::busy("Seeds", "Getting the seed " + name + " needs...",
                             [&](tasks::Busy &busy) {
                                 added = installer::installSeed(id, message,
                                                                &busy.cancel);
                             },
                             true)) {
                break;
            }
            ui::message(added ? "Seed Added" : "No Seed Added", message,
                        added ? ui::Tone::Success : ui::Tone::Warning);
            break;
        }
        case Verb::Export: {
            const std::string folder = "sdmc:/3ds/3dsDL/tickets/";
            std::string path;
            size_t exported = 0;
            tasks::busy("Tickets", "Exporting...", [&]() {
                for (const uint64_t ticket : group.tickets) {
                    exported += library::exportTicket(ticket, folder, path, error) ? 1 : 0;
                }
            });
            sdChanged(folder);
            if (exported == group.tickets.size()) {
                ui::message(exported == 1 ? "Ticket Exported" : "Tickets Exported",
                            exported == 1 ? "Saved as " + path + "."
                                          : "Saved in " + folder + ".",
                            ui::Tone::Success);
            } else {
                ui::message("Not Exported", error, ui::Tone::Error);
            }
            break;
        }
        case Verb::RemovePending: {
            if (!confirmDanger("Clean Up", "Remove the unfinished install of " + name + "?",
                               "Remove")) {
                break;
            }
            libraryLoader_.stop();
            bool removed = true;
            for (const library::PendingTitle &pending : library_.pending) {
                if (std::find(group.pending.begin(), group.pending.end(), pending.id) !=
                    group.pending.end()) {
                    removed = library::deletePending(pending, error) && removed;
                }
            }
            if (!removed) {
                ui::message("Not Removed", error, ui::Tone::Error);
            }
            reloadLibrary();
            break;
        }
        case Verb::DeleteUpdate:
        case Verb::DeleteDlc:
        case Verb::DeleteSoftware: {
            const Verb verb = verbs[result.index];
            // A copy: deleting takes it out of the library it points into.
            const titles::Title title = verb == Verb::DeleteUpdate ? *group.update
                                        : verb == Verb::DeleteDlc  ? *group.dlc
                                                                   : *group.software;
            const std::string question =
                verb == Verb::DeleteUpdate ? "Delete the update for " + name + "?"
                : verb == Verb::DeleteDlc  ? "Delete the DLC for " + name + "?"
                                           : "Delete " + name + " and its save data?";
            if (!confirmDanger("Delete", question, "Delete")) {
                break;
            }
            bool deleted = false;
            libraryLoader_.stop();
            tasks::busy("Delete", "Deleting...", [&]() {
                deleted = library::deleteTitle(title, false, error);
            });
            if (!deleted) {
                ui::message("Not Deleted", error, ui::Tone::Error);
            }
            // Its ticket stays, and with it the row, until it goes too.
            if (deleted && library_.complete) {
                library::forgetTitle(library_, title, false);
                ++libraryGeneration_;
            } else {
                reloadLibrary();
            }
            break;
        }
        case Verb::DeleteExtra:
            if (confirmDanger("Delete", "Delete the extra data of " + name + "?",
                              "Delete")) {
                // The reader stops first, so nothing is read while it changes.
                libraryLoader_.stop();
                if (!library::deleteExtData(group.extData, error)) {
                    ui::message("Not Deleted", error, ui::Tone::Error);
                }
                reloadLibrary();
            }
            break;
        case Verb::DeleteTickets:
            deleteTickets(loose, name);
            break;
        }
    }
}

void App::deleteTickets(const std::vector<uint64_t> &tickets, const std::string &name)
{
    // Installed software does not start without its ticket. Asked fresh,
    // so a misread list never words it as harmless.
    const std::vector<uint64_t> unusedList = library::unusedTickets(library_);
    bool needed = false;
    for (const uint64_t ticket : tickets) {
        needed = needed || library::installedAnywhere(ticket) ||
                 library_.onSdCard.count(ticket) != 0 ||
                 std::find(unusedList.begin(), unusedList.end(), ticket) ==
                     unusedList.end();
    }
    const std::string what = tickets.size() == 1 ? "the ticket"
                                                 : plural(tickets.size(), "ticket");
    if (!confirmDanger("Delete",
                       needed ? "Delete " + what + " " + name + " needs to start?"
                              : "Delete " + what + " for " + name + "?",
                       "Delete")) {
        return;
    }
    std::string error;
    std::string failure;
    std::vector<uint64_t> unsaved;
    std::string unsavedWhy;
    for (const uint64_t ticket : tickets) {
        bool couldNotSave = false;
        if (library::deleteTicket(ticket, error, &couldNotSave)) {
            continue;
        }
        if (couldNotSave) {
            unsaved.push_back(ticket);
            unsavedWhy = error;
        } else {
            failure = error;
        }
    }
    // One the console will not export can still go, once the person has
    // seen what it is.
    std::map<uint64_t, library::TicketFacts> shown;
    if (!unsaved.empty() && confirmUnsavedTickets(unsaved, unsavedWhy, shown)) {
        for (const uint64_t ticket : unsaved) {
            if (!library::deleteTicket(ticket, error, nullptr, &shown[ticket])) {
                failure = error;
            }
        }
    }
    sdChanged(library::DeletedTicketsFolder);
    if (!failure.empty()) {
        ui::message("Not Deleted", failure, ui::Tone::Error);
    }
    reloadLibrary();
}

void App::titlesOptions()
{
    // Each of these decides from everything that is installed.
    if (!waitForLibrary()) {
        return;
    }
    while (ui::keepRunning() && !quit_) {
        const size_t pending = library_.pending.size();
        const size_t unused = library::unusedTickets(library_).size();
        const size_t saved = library::deletedTicketCount();
        std::string seeds;
        for (const char *place : SeedDbPlaces) {
            if (seeds.empty() && fs::fileExists(place)) {
                seeds = place;
            }
        }
        const std::vector<ui::MenuItem> items = {
            {"Add Seeds", false, true, ui::Icon::Seed,
             ui::color::IconGreen},
            {"Restore Deleted Tickets", false, saved > 0, ui::Icon::History,
             ui::color::IconGreen},
            {"Remove Unfinished Installs", true, pending > 0, ui::Icon::Trash,
             ui::color::IconRed},
            {"Delete Unused Tickets", true, unused > 0, ui::Icon::Trash,
             ui::color::IconRed}};
        const int choice = ui::menu(items, [&](int) {
            ui::drawTopChrome("Titles", ui::Icon::GameCard, ui::color::IconBlue);
            ui::drawPanel();
            ui::drawFacts({{"Seeds", seeds.empty() ? "No seeddb.bin" : seeds,
                            seeds.empty() ? ui::color::Neutral : ui::color::Tag},
                           {"Saved", plural(saved, "deleted ticket")},
                           {"Unfinished", plural(pending, "install"),
                            pending > 0 ? ui::color::Badge : ui::color::Tag},
                           {"Unused", plural(unused, "ticket")}});
        });
        if (choice == 0) {
            importSeeds();
        } else if (choice == 1) {
            std::string error;
            size_t restored = 0;
            tasks::busy("Tickets", "Restoring deleted tickets...", [&]() {
                restored = library::restoreDeletedTickets(error);
            });
            sdChanged(library::DeletedTicketsFolder);
            ui::message(restored == saved ? "Tickets Restored"
                                          : "Some Tickets Not Restored",
                        plural(restored, "ticket") + " of " +
                            std::to_string(saved) + " restored.",
                        restored == saved ? ui::Tone::Success : ui::Tone::Warning,
                        error);
            reloadLibrary();
        } else if (choice == 2) {
            if (!confirmDanger("Clean Up",
                               "Remove " + plural(pending, "unfinished install") + "?",
                               "Remove")) {
                continue;
            }
            size_t removed = 0;
            std::string error;
            tasks::busy(
                "Clean Up", "Removing unfinished installs...",
                [&](tasks::Busy &busy) {
                    busy.total = library_.pending.size();
                    for (const library::PendingTitle &title : library_.pending) {
                        if (library::deletePending(title, error)) {
                            ++removed;
                        }
                        busy.done.fetch_add(1);
                    }
                },
                false);
            ui::message(removed == pending ? "Cleaned Up" : "Some Remain",
                        plural(removed, "unfinished install") + " removed.",
                        removed == pending ? ui::Tone::Success : ui::Tone::Warning,
                        error);
            reloadLibrary();
        } else if (choice == 3) {
            deleteUnusedTickets();
        } else {
            return;
        }
    }
}
