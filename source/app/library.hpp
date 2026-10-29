#pragma once

#include "smdh.hpp"
#include "titles.hpp"

#include <3ds.h>

#include <atomic>
#include <cstdint>
#include <set>
#include <string>
#include <vector>

// Reads and changes what is installed on the console, through the AM and FS
// services, for the Titles screens. Only user software is ever listed or
// touched (see titles::isShown).
namespace library {

// Every ticket 3dsDL deletes is saved here first, with a log of what was
// checked and done, so deleting can be undone.
constexpr const char *DeletedTicketsFolder = "sdmc:/3ds/3dsDL/tickets/deleted/";
// A ticket replaced by installing a .tik is saved here first.
constexpr const char *ReplacedTicketsFolder = "sdmc:/3ds/3dsDL/tickets/replaced/";

struct ExtData {
    uint64_t id = 0;
    // From the extra data's own icon, which stays when the game is gone.
    std::string name;
    std::string englishShortName; // for Checkpoint's folder name
    std::string publisher;
    std::vector<uint16_t> icon;
};

struct PendingTitle {
    uint64_t id = 0;
    FS_MediaType media = MEDIATYPE_SD;
};

struct Snapshot {
    std::vector<titles::Title> titles; // SD card and system memory
    std::vector<uint64_t> tickets;     // user tickets only
    std::vector<ExtData> extData;      // on the SD card
    std::vector<PendingTitle> pending; // unfinished user installs
    // Titles with a folder on the SD card, read from the card itself; they
    // count as installed for deciding what is unused.
    std::set<uint64_t> onSdCard;
    // False when the console's list of SD card software was not available,
    // as on a card that has never held any. Nothing is then called unused.
    bool sdListed = false;
    // True once everything is read. Until then only titles are filled in,
    // a few at a time, and nothing may be decided from the rest.
    bool complete = false;
};

// Tickets no installed software needs, by this snapshot, for showing; none
// when the SD card's software was not listed. Deleting goes through
// checkUnusedTickets, which reads everything again.
std::vector<uint64_t> unusedTickets(const Snapshot &snapshot);
// Whether extra data belongs to installed software; always true when the SD
// card's software was not listed.
bool extDataInUse(const Snapshot &snapshot, uint64_t extDataId);

// Reads a Snapshot on a worker thread and hands it over a piece at a time:
// each title as soon as its name and icon are read, then tickets, extra
// data and unfinished installs at the end, which completes it. Screens show
// titles while the rest loads.
class Loader {
public:
    Loader();
    ~Loader();
    Loader(const Loader &) = delete;
    Loader &operator=(const Loader &) = delete;

    // Reads everything again from the start, stopping a read under way.
    void start();
    // Stops reading and waits for the worker; what was read stays.
    void stop();
    // Moves what has arrived into snapshot, which must start out empty
    // after each start(); returns true when anything changed. Titles are
    // only ever added, into room reserved up front, so pointers to them
    // stay valid while it fills.
    bool take(Snapshot &snapshot);
    bool running() const { return running_; }
    // Title IDs looked at so far, of all there are.
    uint64_t done() const { return done_; }
    uint64_t total() const { return total_; }
    // True, with why, when a part could not be read; the snapshot then
    // never completes.
    bool failed(std::string &error) const;

private:
    static void run(void *argument);
    void work();

    Thread thread_ = nullptr;
    std::atomic<bool> stop_ {false};
    std::atomic<bool> running_ {false};
    std::atomic<uint64_t> done_ {0};
    std::atomic<uint64_t> total_ {0};
    mutable LightLock lock_;
    // Guarded by lock_.
    std::vector<titles::Title> arrived_;
    size_t room_ = 0;
    Snapshot rest_;
    bool restReady_ = false;
    bool failed_ = false;
    std::string error_;
};

FS_MediaType mediaOf(const titles::Title &title);
// The installed title 3dsDL runs as, or 0 for the .3dsx.
uint64_t runningTitle();
// The name and icon of installed software. False when it has none, as for
// DSiWare.
bool titleSmdh(uint64_t titleId, FS_MediaType media, smdh::Info &info);

bool deleteTitle(const titles::Title &title, bool withTicket,
                 std::string &error);
// Takes a deleted title out of a complete snapshot, so the lists keep their
// place instead of reading everything again.
void forgetTitle(Snapshot &snapshot, const titles::Title &title,
                 bool withTicket);
// True when AM describes titleId as installed on the SD card or in system
// memory, asked for that one title.
bool installedAnywhere(uint64_t titleId);
// Reads everything again and returns the tickets no installed software
// needs, for deleting them. Fails, and returns none, when any part cannot
// be read or the sources disagree, rather than guessing.
bool checkUnusedTickets(std::vector<uint64_t> &unused, std::string &error);
// Saves the ticket to DeletedTicketsFolder, then deletes it. Nothing is
// deleted when it cannot be saved.
bool deleteTicket(uint64_t titleId, std::string &error);
// How many deleted tickets are saved, and putting them all back; restored
// copies are removed. Returns how many were restored.
size_t deletedTicketCount();
size_t restoreDeletedTickets(std::string &error);
bool deleteExtData(uint64_t extDataId, std::string &error);
bool deletePending(const PendingTitle &pending, std::string &error);

// Installs the ticket file at path, replacing one already there, as FBI
// does. titleId is the ticket's title.
bool installTicket(const std::string &path, uint64_t &titleId,
                   std::string &error);
// Writes the console's ticket for titleId into folder as <Title ID>.tik and
// returns its path.
bool exportTicket(uint64_t titleId, const std::string &folder,
                  std::string &path, std::string &error);

} // namespace library
