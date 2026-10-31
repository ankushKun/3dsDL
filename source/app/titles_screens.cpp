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
#include <cstdio>
#include <map>
#include <set>

// Titles: installed software, updates and DLC, extra data, tickets, seeds
// and cleaning up after failed installs, in the spirit of System Settings,
// Data Management.

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

// The title's icon, name and publisher over its Title ID, version and size;
// order is how the list it is in is sorted, if it is in one.
void drawTitleTop(const std::string &header, ui::Icon headerIcon,
                  const titles::Title &title, const titles::Index &index,
                  const std::string &order = "")
{
    ui::drawTopChrome(header, headerIcon, ui::color::IconBlue, order);
    ui::drawPanel();
    const float left = ui::TopPanel.x + 18;
    const float top = ui::TopPanel.y + 12;
    const titles::Title *game = &title;
    if (title.icon.empty()) {
        const auto found = index.find(cia::baseTitle(title.id));
        if (found != index.end()) {
            game = found->second;
        }
    }
    ui::drawTitleIcon(game->icon, left, top, iconFor(title.id));
    ui::drawMarquee(titles::displayName(title, index), left + 60, top + 2,
                    0.62f, ui::color::InkStrong, 280);
    std::string second = game->publisher.empty()
                             ? cia::kindName(cia::kind(title.id))
                             : game->publisher;
    if (!title.productCode.empty()) {
        second += "   " + title.productCode;
    }
    ui::drawMarquee(second, left + 60, top + 30, ui::type::Small,
                    ui::color::InkSoft, 280);
    float y = top + 60;
    y = ui::drawTagRow(left, y, 76, "Title ID", cia::titleIdText(title.id));
    y = ui::drawTagRow(left, y, 76, "Version", screens::titleVersion(title.id, title.version));
    ui::drawTagRow(left, y, 76, "Size",
                   units::bytes(title.size) + ", " + placeName(title.place));
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

const library::ExtData *extDataFor(const titles::Title &title,
                                   const library::Snapshot &snapshot)
{
    for (const library::ExtData &extData : snapshot.extData) {
        if (extData.id == titles::uniqueId(title.id)) {
            return &extData;
        }
    }
    return nullptr;
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

void App::titlesScreen()
{
    // Opens at once while the titles are read in the background; the title
    // lists and Save Data fill in as they arrive. Everything that needs the
    // whole picture (tickets, seeds, extra data, clean up) waits for it.
    startLibrary();
    struct StopReading {
        library::Loader &loader;
        ~StopReading() { loader.stop(); }
    } stopReading {libraryLoader_};

    int focus = 3;
    while (ui::keepRunning() && !quit_) {
        size_t software = 0;
        size_t addOns = 0;
        uint64_t sdBytes = 0;
        size_t unused = 0;
        size_t cleanUp = 0;
        std::string error;
        bool failed = false;
        const auto count = [&]() {
            software = addOns = 0;
            sdBytes = 0;
            for (const titles::Title &title : library_.titles) {
                (titles::isSoftware(title.id) ? software : addOns) += 1;
                if (title.place == titles::Place::SdCard) {
                    sdBytes += title.size;
                }
            }
            unused = library::unusedTickets(library_).size();
            cleanUp = unused + library_.pending.size();
            failed = libraryLoader_.failed(error);
        };
        count();

        ui::ScreenSpec spec;
        spec.back = "Back";
        const auto add = [&](ui::Rect rect, const char *label, ui::Icon icon,
                             u32 iconColor, bool tile) {
            ui::Control control;
            control.rect = rect;
            control.label = label;
            control.icon = icon;
            control.iconColor = iconColor;
            control.tile = tile;
            control.scale = ui::type::Small + 0.06f;
            spec.controls.push_back(control);
        };
        // Three narrow bars: words only, so they fit without scrolling.
        add({16, 6, 92, 26}, "Clean Up", ui::Icon::None, 0, false);
        spec.controls.back().scale = ui::type::Small;
        add({114, 6, 92, 26}, "Tickets", ui::Icon::None, 0, false);
        spec.controls.back().scale = ui::type::Small;
        add({212, 6, 92, 26}, "Seeds", ui::Icon::None, 0, false);
        spec.controls.back().scale = ui::type::Small;
        add({16, 42, 140, 78}, "Software", ui::Icon::GameCard,
            ui::color::IconBlue, true);
        add({164, 42, 140, 78}, "Updates and DLC", ui::Icon::Download,
            ui::color::IconGreen, true);
        add({16, 127, 140, 76}, "Save Data", ui::Icon::Chip,
            ui::color::IconTeal, true);
        add({164, 127, 140, 76}, "Extra Data", ui::Icon::Folder,
            ui::color::IconOrange, true);
        // The title lists and Save Data fill in while the titles are read;
        // the rest needs all of them first.
        uint32_t seen = libraryGeneration_ - 1;
        spec.update = [&]() {
            if (seen == libraryGeneration_) {
                return false;
            }
            seen = libraryGeneration_;
            count();
            for (const int needsAll : {0, 1, 2, 6}) {
                spec.controls[needsAll].enabled = library_.complete;
            }
            spec.controls[0].badge =
                library_.complete && cleanUp > 0 ? std::to_string(cleanUp) : "";
            return true;
        };
        spec.update();
        spec.drawTop = [&]() {
            ui::drawTopChrome("Titles", ui::Icon::GameCard, ui::color::IconBlue);
            ui::drawPanel();
            std::vector<ui::Fact> facts;
            if (failed) {
                facts.push_back({"Not read", error, ui::color::Bad});
            } else if (!library_.complete) {
                facts.push_back({"Reading",
                                 std::to_string(libraryLoader_.done()) + " of " +
                                     std::to_string(libraryLoader_.total()),
                                 ui::color::Badge});
            }
            switch (focus) {
            case 0:
                facts.push_back({"Unfinished",
                                 plural(library_.pending.size(), "install")});
                facts.push_back({"Unused", plural(unused, "ticket")});
                break;
            case 1:
                facts.push_back(
                    {"Tickets", std::to_string(library_.tickets.size())});
                facts.push_back({"Unused", std::to_string(unused)});
                break;
            case 2:
                facts.push_back({"Looks in", "sdmc:/seeddb.bin"});
                break;
            case 4:
                facts.push_back({"Installed", plural(addOns, "item")});
                break;
            case 5:
                facts.push_back({"Backups in", "sdmc:/3ds/Checkpoint/saves/"});
                break;
            case 6:
                facts.push_back({"On SD card",
                                 plural(library_.extData.size(), "item")});
                facts.push_back(
                    {"Backups in", "sdmc:/3ds/Checkpoint/extdata/"});
                break;
            default:
                facts.push_back({"Installed", plural(software, "title")});
                facts.push_back(
                    {"On SD card",
                     !library_.complete || library_.sdListed
                         ? units::bytes(sdBytes)
                         : "Not readable",
                     !library_.complete || library_.sdListed
                         ? ui::color::Tag
                         : ui::color::Badge});
                break;
            }
            ui::drawFacts(facts);
        };
        const ui::ScreenResult result = ui::runScreen(spec, focus);
        switch (result.choice) {
        case 0:
            cleanUpScreen();
            break;
        case 1:
            ticketsScreen();
            break;
        case 2:
            importSeeds();
            break;
        case 3:
            titleList(true);
            break;
        case 4:
            titleList(false);
            break;
        case 5:
            saveTitlesScreen();
            break;
        case 6:
            extraDataScreen();
            break;
        default:
            return;
        }
    }
}

namespace {

struct ShowChoice {
    titles::Show show;
    const char *label; // on the sort screen
    const char *title; // the list's, while it shows only these
    const char *empty;
};

const ShowChoice SoftwareShows[] = {
    {titles::Show::All, "All Software", "Software", "No software installed."},
    {titles::Show::SdCard, "SD Card", "Software on SD Card",
     "No software on the SD card."},
    {titles::Show::SystemMemory, "System Memory", "Software in System Memory",
     "No software in system memory."},
};
const ShowChoice AddOnShows[] = {
    {titles::Show::All, "Updates and DLC", "Updates and DLC",
     "No updates or DLC installed."},
    {titles::Show::Updates, "Updates", "Updates", "No updates installed."},
    {titles::Show::Dlc, "DLC", "DLC", "No DLC installed."},
};

const ShowChoice &showChoice(bool software, titles::Show show)
{
    const ShowChoice *choices = software ? SoftwareShows : AddOnShows;
    for (int index = 0; index < 3; ++index) {
        if (choices[index].show == show) {
            return choices[index];
        }
    }
    return choices[0];
}

} // namespace

void App::titleList(bool software)
{
    int cursor = 0;
    uint64_t follow = 0; // a title to land on after the order changes
    while (ui::keepRunning() && !quit_) {
        // Rebuilt whenever more titles arrive, so the list fills in while
        // it is open; rows are keyed by Title ID, so the highlight stays.
        titles::Index index;
        std::vector<const titles::Title *> shown;
        const titles::Show show = software ? softwareShow_ : addOnShow_;
        ui::ListSpec spec;
        spec.title = showChoice(software, show).title;
        spec.emptyText = showChoice(software, show).empty;
        spec.action = "Sort";
        spec.actionKey = "X";
        spec.actionKeys = KEY_X;
        uint32_t seen = libraryGeneration_ - 1;
        spec.update = [&]() {
            if (seen == libraryGeneration_) {
                return false;
            }
            seen = libraryGeneration_;
            index = titles::indexOf(library_.titles);
            shown.clear();
            for (const titles::Title &title : library_.titles) {
                if (titles::isSoftware(title.id) == software &&
                    titles::shows(show, title)) {
                    shown.push_back(&title);
                }
            }
            titles::sortTitles(shown, index, settings_.titleSort,
                               settings_.titleSortReverse);
            spec.items.clear();
            for (const titles::Title *title : shown) {
                ui::ListItem item;
                item.key = cia::titleIdText(title->id);
                item.label = titles::displayName(*title, index);
                item.icon = iconFor(title->id);
                item.iconColor =
                    software ? ui::color::IconBlue : ui::color::IconGreen;
                const cia::Kind kind = cia::kind(title->id);
                if (kind == cia::Kind::Update || kind == cia::Kind::Dlc) {
                    item.badge = kind == cia::Kind::Update ? "Update" : "DLC";
                    item.badgeColor = kind == cia::Kind::Update
                                          ? ui::color::Tag
                                          : ui::color::Good;
                } else {
                    item.detail = units::bytes(title->size);
                }
                spec.items.push_back(item);
            }
            return true;
        };
        spec.update();
        for (size_t row = 0; follow != 0 && row < shown.size(); ++row) {
            if (shown[row]->id == follow) {
                cursor = static_cast<int>(row);
            }
        }
        follow = 0;
        spec.loading = [&]() {
            std::string ignored;
            return !library_.complete && !libraryLoader_.failed(ignored);
        };
        spec.drawTop = [&](int highlighted) {
            const std::string order = screens::sortDescription(
                settings_.titleSort, settings_.titleSortReverse);
            if (highlighted < 0 || highlighted >= static_cast<int>(shown.size())) {
                ui::drawTopChrome(spec.title, ui::Icon::GameCard,
                                  ui::color::IconBlue, order);
                ui::drawPanel();
                ui::drawEmblem(ui::Icon::GameCard, ui::color::IconBlue);
                return;
            }
            drawTitleTop(spec.title, software ? ui::Icon::GameCard
                                              : ui::Icon::Download,
                         *shown[highlighted], index, order);
        };
        const ui::ListResult result = ui::runList(spec, cursor);
        const bool onRow =
            result.index >= 0 && result.index < static_cast<int>(shown.size());
        if (result.key == KEY_X) {
            follow = onRow ? shown[result.index]->id : 0;
            cursor = 0;
            titleSortScreen(software);
            continue;
        }
        if (result.key != KEY_A || !onRow) {
            return;
        }
        titleOptions(*shown[result.index]);
    }
}

void App::titleSortScreen(bool software)
{
    titles::Show &show = software ? softwareShow_ : addOnShow_;
    const ShowChoice *shows = software ? SoftwareShows : AddOnShows;
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
            control.label = shows[index].label;
            control.style =
                shows[index].show == show ? ui::Style::Chosen : ui::Style::Normal;
            spec.controls.push_back(control);
        }
        spec.drawTop = [&]() {
            ui::drawTopChrome("Sort", software ? ui::Icon::GameCard
                                               : ui::Icon::Download,
                              software ? ui::color::IconBlue : ui::color::IconGreen);
            ui::drawPanel();
            ui::drawFacts(
                {{"Order", screens::sortDescription(settings_.titleSort,
                                           settings_.titleSortReverse)},
                 {"Showing", showChoice(software, show).label}});
        };
        const ui::ScreenResult result = ui::runScreen(spec, focus);
        if (result.choice == 0 || result.choice == 1) {
            settings_.titleSort = orders[result.choice];
            persistSettings();
        } else if (result.choice == 2) {
            settings_.titleSortReverse = !settings_.titleSortReverse;
            persistSettings();
        } else if (result.choice >= 3 && result.choice < 6) {
            show = shows[result.choice - 3].show;
        } else {
            return;
        }
    }
}

void App::titleOptions(titles::Title title)
{
    const titles::Index index = titles::indexOf(library_.titles);
    const std::string name = titles::displayName(title, index);
    const cia::Kind kind = cia::kind(title.id);
    const bool launchable = kind == cia::Kind::Application ||
                            kind == cia::Kind::Demo ||
                            kind == cia::Kind::DsiWare;
    const library::ExtData *extData = extDataFor(title, library_);
    const bool self = title.id == library::runningTitle();

    enum Choice { Launch, Saves, Extra, Seed, ExtraData, Delete, DeleteWithTicket };
    std::vector<Choice> choices;
    std::vector<ui::MenuItem> items;
    const auto offer = [&](Choice choice, const std::string &label,
                           bool danger = false, bool enabled = true) {
        choices.push_back(choice);
        items.push_back({label, danger, enabled});
    };
    if (launchable) {
        offer(Launch, "Launch", false, !self);
    }
    const savedata::Target saveTarget =
        savedata::saveOf(title, library::mediaOf(title));
    const bool canSave = titles::isSoftware(title.id) &&
                         savedata::unsupportedReason(title).empty() &&
                         savedata::exists(saveTarget);
    if (canSave) {
        offer(Saves, "Save Data");
    }
    if (extData != nullptr) {
        offer(Extra, "Extra Data");
    }
    if (kind == cia::Kind::Application) {
        offer(Seed, "Get Seed");
    }
    if (extData != nullptr) {
        offer(ExtraData, "Delete Extra Data", true);
    }
    const bool removable = !self && title.place != titles::Place::GameCard;
    offer(Delete, "Delete", true, removable);
    offer(DeleteWithTicket, "Delete with Ticket", true, removable);

    const int picked = ui::menu(items, [&](int) {
        drawTitleTop(name, iconFor(title.id), title, index);
    });
    if (picked < 0) {
        return;
    }

    std::string error;
    switch (choices[picked]) {
    case Launch:
        if (!installer::launch(title.id, error)) {
            ui::message("Could Not Launch", error, ui::Tone::Error);
        }
        return;
    case Saves:
        saveDataScreen(saveTarget, title.icon);
        return;
    case Extra:
        saveDataScreen(savedata::extDataOf(title), title.icon);
        return;
    case Seed: {
        std::string message;
        bool added = false;
        if (!tasks::busy("Seeds", "Getting the seed " + name + " needs...",
                         [&](tasks::Busy &busy) {
                             added = installer::installSeed(title.id, message,
                                                            &busy.cancel);
                         },
                         true)) {
            return;
        }
        ui::message(added ? "Seed Added" : "No Seed Added", message,
                    added ? ui::Tone::Success : ui::Tone::Warning);
        return;
    }
    case ExtraData:
        if (confirmDanger("Delete", "Delete the extra data of " + name + "?",
                          "Delete")) {
            // The reader stops first, so nothing is read while it changes;
            // the list it came from fills in again by itself.
            libraryLoader_.stop();
            if (!library::deleteExtData(extData->id, error)) {
                ui::message("Not Deleted", error, ui::Tone::Error);
            }
            startLibrary();
        }
        return;
    case Delete:
    case DeleteWithTicket: {
        const bool withTicket = choices[picked] == DeleteWithTicket;
        // An update or DLC goes by its game's name, so the question says
        // which; a game takes its save with it.
        std::string question;
        if (kind == cia::Kind::Update || kind == cia::Kind::Dlc) {
            question = std::string("Delete the ") +
                       (kind == cia::Kind::Update ? "update" : "DLC") +
                       " for " + name + (withTicket ? " and its ticket?" : "?");
        } else {
            question = "Delete " + name +
                       (withTicket ? ", its save data and its ticket?"
                                   : " and its save data?");
        }
        if (!confirmDanger("Delete", question, "Delete")) {
            return;
        }
        bool deleted = false;
        libraryLoader_.stop();
        tasks::busy("Delete", "Deleting " + name + "...", [&]() {
            deleted = library::deleteTitle(title, withTicket, error);
        });
        if (!deleted) {
            ui::message("Not Deleted", error, ui::Tone::Error);
        }
        if (deleted && library_.complete) {
            library::forgetTitle(library_, title, withTicket);
            ++libraryGeneration_;
        } else {
            startLibrary();
        }
        return;
    }
    }
}

void App::extraDataScreen()
{
    int cursor = 0;
    while (ui::keepRunning() && !quit_) {
        const titles::Index index = titles::indexOf(library_.titles);
        const auto nameOf = [&](const library::ExtData &extData) {
            if (!extData.name.empty()) {
                return extData.name;
            }
            const titles::Title *owner =
                titles::ownerOfExtData(extData.id, library_.titles);
            if (owner != nullptr) {
                return titles::displayName(*owner, index);
            }
            char text[24];
            std::snprintf(text, sizeof(text), "Extra data %08llX",
                          static_cast<unsigned long long>(extData.id));
            return std::string(text);
        };
        std::vector<const library::ExtData *> shown;
        for (const library::ExtData &extData : library_.extData) {
            shown.push_back(&extData);
        }
        std::stable_sort(shown.begin(), shown.end(),
                         [&](const library::ExtData *a, const library::ExtData *b) {
                             return text::lower(nameOf(*a)) < text::lower(nameOf(*b));
                         });

        ui::ListSpec spec;
        spec.title = "Extra Data";
        spec.emptyText = "No extra data.";
        for (const library::ExtData *extData : shown) {
            ui::ListItem item;
            item.label = nameOf(*extData);
            item.icon = ui::Icon::Folder;
            item.iconColor = ui::color::IconOrange;
            if (!library::extDataInUse(library_, extData->id)) {
                item.badge = "Unused";
                item.badgeColor = ui::color::Neutral;
            }
            spec.items.push_back(item);
        }
        // The list and the menu for one item show the same facts.
        const auto drawDetails = [&](const library::ExtData &extData) {
            ui::drawTopChrome("Extra Data", ui::Icon::Folder,
                              ui::color::IconOrange);
            ui::drawPanel();
            const titles::Title *owner =
                titles::ownerOfExtData(extData.id, library_.titles);
            const float left = ui::TopPanel.x + 18;
            const float top = ui::TopPanel.y + 12;
            ui::drawTitleIcon(!extData.icon.empty() || owner == nullptr
                                  ? extData.icon
                                  : owner->icon,
                              left, top, ui::Icon::Folder);
            ui::drawMarquee(nameOf(extData), left + 60, top + 2, 0.62f,
                            ui::color::InkStrong, 280);
            ui::drawMarquee(extData.publisher, left + 60, top + 30,
                            ui::type::Small, ui::color::InkSoft, 280);
            char id[16];
            std::snprintf(id, sizeof(id), "%08llX",
                          static_cast<unsigned long long>(extData.id));
            const float y = ui::drawTagRow(left, top + 60, 96, "ID", id);
            const bool inUse = library::extDataInUse(library_, extData.id);
            ui::drawTagRow(left, y, 96, "Software",
                           inUse ? "Installed" : "Not installed",
                           inUse ? ui::color::Tag : ui::color::Neutral);
        };
        spec.drawTop = [&](int highlighted) {
            if (highlighted < 0 || highlighted >= static_cast<int>(shown.size())) {
                ui::drawTopChrome("Extra Data", ui::Icon::Folder,
                                  ui::color::IconOrange);
                ui::drawPanel();
                ui::drawEmblem(ui::Icon::Folder, ui::color::IconOrange);
                return;
            }
            drawDetails(*shown[highlighted]);
        };
        if (!shown.empty()) {
            spec.action = "Delete";
            spec.actionKey = "X";
            spec.actionKeys = KEY_X;
        }
        const ui::ListResult result = ui::runList(spec, cursor);
        if ((result.key != KEY_A && result.key != KEY_X) || result.index < 0 ||
            result.index >= static_cast<int>(shown.size())) {
            return;
        }
        const library::ExtData extData = *shown[result.index];
        const std::string name = nameOf(extData);
        if (result.key == KEY_A) {
            const int choice = ui::menu(
                {{"Back Up and Restore", false, true}, {"Delete", true, true}},
                [&](int) { drawDetails(extData); });
            if (choice == 0) {
                // Named after the game when it is installed, as Checkpoint
                // names it; otherwise after the extra data's own icon.
                const titles::Title *owner =
                    titles::ownerOfExtData(extData.id, library_.titles);
                saveDataScreen(owner != nullptr
                                   ? savedata::extDataOf(*owner)
                                   : savedata::extDataOf(extData.id,
                                                         extData.englishShortName,
                                                         name),
                               extData.icon.empty() && owner != nullptr
                                   ? owner->icon
                                   : extData.icon);
                continue;
            }
            if (choice != 1) {
                continue;
            }
        }
        if (!confirmDanger("Delete", "Delete the extra data of " + name + "?",
                           "Delete")) {
            continue;
        }
        std::string error;
        if (!library::deleteExtData(extData.id, error)) {
            ui::message("Not Deleted", error, ui::Tone::Error);
        }
        reloadLibrary();
    }
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

std::map<uint64_t, FoundCia> ciasOnCard(const std::string &downloadFolder)
{
    std::map<uint64_t, FoundCia> found;
    std::set<std::string> seen;
    for (const std::string &folder : {std::string("sdmc:/cias/"), downloadFolder}) {
        if (!seen.insert(text::lower(folder)).second) {
            continue;
        }
        for (const fs::Entry &entry : fs::listEntries(folder, nullptr, false, false)) {
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
                                const std::string &why)
{
    struct Known {
        uint64_t id = 0;
        std::string name;
        library::TicketFacts facts;
        std::string cia; // a CIA that brings it back, or ""
    };
    std::vector<Known> known;
    tasks::busy("Tickets", "Looking at the tickets...", [&]() {
        const titles::Index index = titles::indexOf(library_.titles);
        const std::vector<std::string> folders = checkpointFolders();
        const std::map<uint64_t, FoundCia> cias = ciasOnCard(settings_.destination);
        for (const uint64_t id : tickets) {
            Known ticket;
            ticket.id = id;
            ticket.name = softwareName(id, index, folders);
            ticket.facts = library::ticketFacts(id);
            const auto cia = cias.find(id);
            if (cia != cias.end()) {
                ticket.cia = cia->second.path;
                if (ticket.name.empty()) {
                    ticket.name = cia->second.name;
                }
            }
            known.push_back(ticket);
        }
    });
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
    return ui::choose(dialog) == 0;
}

void App::ticketsScreen()
{
    int cursor = 0;
    const std::vector<std::string> folders = checkpointFolders();
    while (ui::keepRunning() && !quit_) {
        const titles::Index index = titles::indexOf(library_.titles);
        // The name of software that is gone, when 3dsDL can tell.
        const auto nameFor = [&](uint64_t ticket) {
            const std::string name = softwareName(ticket, index, folders);
            return name.empty() ? cia::titleIdText(ticket) : name;
        };
        const std::vector<uint64_t> unusedList =
            library::unusedTickets(library_);
        const std::set<uint64_t> unused(unusedList.begin(), unusedList.end());
        std::vector<uint64_t> shown = library_.tickets;
        std::stable_sort(shown.begin(), shown.end(), [&](uint64_t a, uint64_t b) {
            const std::string left = text::lower(nameFor(a));
            const std::string right = text::lower(nameFor(b));
            return left != right ? left < right : a < b;
        });

        ui::ListSpec spec;
        spec.title = "Tickets";
        spec.emptyText = "No tickets.";
        for (const uint64_t ticket : shown) {
            ui::ListItem item;
            item.label = nameFor(ticket);
            item.icon = ui::Icon::Lock;
            item.iconColor = ui::color::IconGrey;
            if (unused.count(ticket) != 0) {
                item.badge = "Unused";
                item.badgeColor = ui::color::Neutral;
            } else {
                item.detail = cia::kindName(cia::kind(ticket));
            }
            spec.items.push_back(item);
        }
        // The list and the menu for one ticket show the same facts.
        const auto drawDetails = [&](uint64_t ticket) {
            ui::drawTopChrome("Tickets", ui::Icon::Lock, ui::color::IconGrey);
            ui::drawPanel();
            const auto own = index.find(ticket);
            const auto game = index.find(cia::baseTitle(ticket));
            const std::vector<uint16_t> noIcon;
            const std::vector<uint16_t> &icon =
                own != index.end() && !own->second->icon.empty() ? own->second->icon
                : game != index.end() ? game->second->icon
                                      : noIcon;
            const float left = ui::TopPanel.x + 18;
            const float top = ui::TopPanel.y + 12;
            ui::drawTitleIcon(icon, left, top, ui::Icon::Lock);
            // The name alone, level with the icon's middle; the list says
            // the kind.
            ui::drawMarquee(nameFor(ticket), left + 60,
                            top + (48 - ui::lineHeight(0.62f)) / 2, 0.62f,
                            ui::color::InkStrong, 280);
            const float y = ui::drawTagRow(left, top + 60, 96, "Title ID",
                                           cia::titleIdText(ticket));
            const bool isUnused = unused.count(ticket) != 0;
            // Without the SD card's list, whether it is installed is unknown.
            ui::drawTagRow(left, y, 96, "Software",
                           !library_.sdListed ? "Unknown"
                           : isUnused         ? "Not installed"
                                              : "Installed",
                           isUnused ? ui::color::Neutral : ui::color::Tag);
        };
        spec.drawTop = [&](int highlighted) {
            if (highlighted < 0 || highlighted >= static_cast<int>(shown.size())) {
                ui::drawTopChrome("Tickets", ui::Icon::Lock, ui::color::IconGrey);
                ui::drawPanel();
                ui::drawEmblem(ui::Icon::Lock, ui::color::IconGrey);
                return;
            }
            drawDetails(shown[highlighted]);
        };
        if (!unusedList.empty()) {
            spec.action = "Delete Unused";
            spec.actionKey = "X";
            spec.actionKeys = KEY_X;
        }
        const ui::ListResult result = ui::runList(spec, cursor);
        if (result.key == KEY_X && !unusedList.empty()) {
            deleteUnusedTickets();
            continue;
        }
        if (result.key != KEY_A || result.index < 0 ||
            result.index >= static_cast<int>(shown.size())) {
            return;
        }
        const uint64_t ticket = shown[result.index];
        const std::string name = nameFor(ticket);
        const int choice = ui::menu(
            {{"Export to SD Card", false, true}, {"Delete", true, true}},
            [&](int) { drawDetails(ticket); });
        if (choice == 0) {
            std::string path;
            std::string error;
            bool exported = false;
            tasks::busy("Tickets", "Exporting the ticket...", [&]() {
                exported = library::exportTicket(
                    ticket, "sdmc:/3ds/3dsDL/tickets/", path, error);
            });
            if (exported) {
                ui::message("Ticket Exported", "Saved as " + path + ".",
                            ui::Tone::Success);
            } else {
                ui::message("Not Exported", error, ui::Tone::Error);
            }
            continue;
        }
        if (choice != 1) {
            continue;
        }
        // Installed software does not start without its ticket. Asked
        // fresh, so a misread list never words it as harmless.
        const bool needed = library::installedAnywhere(ticket) ||
                            library_.onSdCard.count(ticket) != 0 ||
                            unused.count(ticket) == 0;
        if (!confirmDanger("Delete",
                           needed ? "Delete the ticket " + name +
                                        " needs to start?"
                                  : "Delete the ticket for " + name + "?",
                           "Delete")) {
            continue;
        }
        std::string error;
        bool unsaved = false;
        if (!library::deleteTicket(ticket, error, &unsaved)) {
            // One the console will not export can still go, once the
            // person has seen what it is.
            if (!unsaved) {
                ui::message("Not Deleted", error, ui::Tone::Error);
            } else if (confirmUnsavedTickets({ticket}, error) &&
                       !library::deleteTicket(ticket, error, nullptr, true)) {
                ui::message("Not Deleted", error, ui::Tone::Error);
            }
        }
        reloadLibrary();
    }
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
    if (!unsaved.empty() && confirmUnsavedTickets(unsaved, unsavedWhy)) {
        tasks::busy(
            "Delete", "Deleting tickets without a backup...",
            [&](tasks::Busy &busy) {
                busy.total = unsaved.size();
                for (const uint64_t ticket : unsaved) {
                    if (!library::installedAnywhere(ticket) &&
                        library::deleteTicket(ticket, error, nullptr, true)) {
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

void App::cleanUpScreen()
{
    int focus = 0;
    while (ui::keepRunning() && !quit_) {
        const size_t pending = library_.pending.size();
        const size_t unused =
            library::unusedTickets(library_).size();
        const size_t saved = library::deletedTicketCount();
        ui::ScreenSpec spec;
        spec.back = "Back";
        const auto add = [&](ui::Rect rect, const std::string &label,
                             bool enabled) {
            ui::Control control;
            control.rect = rect;
            control.label = label;
            control.icon = ui::Icon::Trash;
            control.iconColor = ui::color::IconRed;
            control.enabled = enabled;
            control.scale = ui::type::Small + 0.06f;
            spec.controls.push_back(control);
        };
        // Centred between the top and the tabs.
        add({22, 26, 276, 44}, "Remove Unfinished Installs", pending > 0);
        add({22, 82, 276, 44}, "Delete Unused Tickets", unused > 0);
        add({22, 138, 276, 44}, "Restore Deleted Tickets", saved > 0);
        spec.controls.back().icon = ui::Icon::History;
        spec.controls.back().iconColor = ui::color::IconGreen;
        spec.drawTop = [&]() {
            ui::drawTopChrome("Clean Up", ui::Icon::Trash, ui::color::IconRed);
            ui::drawPanel();
            ui::drawFacts({{"Unfinished", plural(pending, "install"),
                            pending > 0 ? ui::color::Badge : ui::color::Tag},
                           {"Unused", plural(unused, "ticket")},
                           {"Saved", plural(saved, "deleted ticket")}});
        };
        const ui::ScreenResult result = ui::runScreen(spec, focus);
        if (result.choice == 0) {
            if (!confirmDanger("Clean Up",
                               "Remove " + plural(pending, "unfinished install") +
                                   "?",
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
                        removed == pending ? ui::Tone::Success
                                           : ui::Tone::Warning,
                        error);
            reloadLibrary();
        } else if (result.choice == 1) {
            deleteUnusedTickets();
        } else if (result.choice == 2) {
            std::string error;
            size_t restored = 0;
            tasks::busy("Tickets", "Restoring deleted tickets...", [&]() {
                restored = library::restoreDeletedTickets(error);
            });
            ui::message(restored == saved ? "Tickets Restored"
                                          : "Some Tickets Not Restored",
                        plural(restored, "ticket") + " of " +
                            std::to_string(saved) + " restored.",
                        restored == saved ? ui::Tone::Success
                                          : ui::Tone::Warning,
                        error);
            reloadLibrary();
        } else {
            return;
        }
    }
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
