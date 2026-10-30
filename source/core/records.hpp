#pragma once

#include "fs_util.hpp"

#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

// The small text formats 3dsDL keeps on the SD card. Everything here only
// converts between structs and strings; reading and writing the files is the
// caller's job.

// What happens after a CIA finishes downloading.
enum class CiaAction { Ask, Install, InstallAndDelete, Keep };

// One connection carries at most one TCP window per round trip, about
// 300 KiB/s from a server 150 ms away, so large files are split across
// several to add those up. Four stays under the six per server that
// browsers open; Settings can lower it for servers or networks that
// dislike it.
constexpr int MaxDownloadConnections = 4;

struct Settings {
    std::string destination = "sdmc:/";
    bool resumeDownloads = true;
    bool offerZipExtraction = false;
    CiaAction afterCiaDownload = CiaAction::Ask;
    // The most connections one download opens, 1 to MaxDownloadConnections.
    int maxConnections = MaxDownloadConnections;
    // Ask GitHub for a newer 3dsDL each time it starts.
    bool checkForUpdates = true;
    // Interface sounds.
    bool sounds = true;
    // Files screen.
    bool showHiddenFiles = false;
    fs::SortOrder fileSort = fs::SortOrder::Name;
    bool fileSortReverse = false;
    // Software and Updates and DLC lists: Name or Size.
    fs::SortOrder titleSort = fs::SortOrder::Name;
    bool titleSortReverse = false;
    // UTC offset used when setting the clock from the internet.
    bool timeZoneSet = false;
    int timeZoneMinutes = 0;
    // Files' favourites, as favorites.hpp keeps them.
    std::vector<std::string> favorites;
};

Settings parseSettings(const std::string &contents);
std::string serializeSettings(const Settings &settings);
// The pre-0.3 sdmc:/3dsDL.cfg file held only the folder on its first line.
std::string parseLegacyDestination(const std::string &contents);

enum class HistoryStatus { Saved, Failed, Cancelled, Installed };

struct HistoryEntry {
    int64_t time = 0;
    HistoryStatus status = HistoryStatus::Failed;
    uint64_t bytes = 0;
    bool resumable = false;
    std::string url;
    std::string folder;
    std::string path;    // where the file was saved, when it was
    std::string message; // result shown to the user
    // The file's name from the server, known once a download has started,
    // even if it never finished. "" when unknown.
    std::string name;
};

constexpr size_t HistoryLimit = 50;

std::vector<HistoryEntry> parseHistory(const std::string &contents);
std::string serializeHistory(const std::vector<HistoryEntry> &entries);
// Adds entry at the front and drops the oldest entries past limit.
void addHistoryEntry(std::vector<HistoryEntry> &entries, HistoryEntry entry,
                     size_t limit = HistoryLimit);

// Byte ranges [first, second) of a file.
using ByteRanges = std::vector<std::pair<uint64_t, uint64_t>>;

// What 3dsDL remembers about an interrupted download so it can continue it
// with an HTTP range request later.
struct ResumeInfo {
    std::string url;
    std::string etag;
    std::string lastModified;
    std::string filename;
    uint64_t total = 0; // 0 when the server did not say
    // A download split across connections is written into a file that
    // already has its full size, so only the done ranges hold real data.
    // Otherwise everything in the file is good.
    bool split = false;
    ByteRanges done;
};

std::string serializeResumeInfo(const ResumeInfo &info);
bool parseResumeInfo(const std::string &contents, ResumeInfo &info);
// The value for an If-Range header: a strong ETag, else Last-Modified, else
// "" which means the download cannot be resumed safely.
std::string ifRangeValidator(const ResumeInfo &info);

struct PartialPaths {
    std::string data; // the bytes received so far
    std::string info; // the ResumeInfo for them
};
// Hidden files in folder that hold an unfinished download of url.
PartialPaths partialPaths(const std::string &folder, const std::string &url);

// Total size from a Content-Range value such as "bytes 100-199/1000", or -1
// when it is missing or "*".
int64_t contentRangeTotal(const std::string &value);
