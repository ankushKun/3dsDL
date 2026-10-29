#include "records.hpp"

#include "text.hpp"
#include "timezone.hpp"

#include <cstdio>
#include <cstdlib>
#include <sstream>

namespace {

constexpr const char *HistoryHeader = "3dsDL-history 1";
constexpr const char *ResumeHeader = "3dsDL-resume 1";
// Split downloads use a new version so that older 3dsDL releases, which
// would take the whole full-size file as downloaded, refuse them instead.
constexpr const char *SplitResumeHeader = "3dsDL-resume 2";

std::vector<std::string> lines(const std::string &contents)
{
    std::vector<std::string> output;
    std::istringstream stream(contents);
    std::string line;
    while (std::getline(stream, line)) {
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        output.push_back(line);
    }
    return output;
}

std::vector<std::string> split(const std::string &line, char separator)
{
    std::vector<std::string> fields;
    size_t start = 0;
    while (true) {
        const size_t end = line.find(separator, start);
        fields.push_back(line.substr(start, end - start));
        if (end == std::string::npos) {
            return fields;
        }
        start = end + 1;
    }
}

bool parseBool(const std::string &value, bool fallback)
{
    const std::string lowered = text::lower(text::trim(value));
    if (lowered == "1" || lowered == "true" || lowered == "on" ||
        lowered == "yes") {
        return true;
    }
    if (lowered == "0" || lowered == "false" || lowered == "off" ||
        lowered == "no") {
        return false;
    }
    return fallback;
}

bool parseUnsigned(const std::string &value, uint64_t &output)
{
    if (value.empty() || value.find_first_not_of("0123456789") !=
                             std::string::npos) {
        return false;
    }
    output = std::strtoull(value.c_str(), nullptr, 10);
    return true;
}

// key=value pairs, one per line; blank lines and '#' comments are skipped.
template <typename Callback>
void forEachPair(const std::string &contents, Callback callback)
{
    for (const std::string &line : lines(contents)) {
        const std::string trimmed = text::trim(line);
        if (trimmed.empty() || trimmed[0] == '#') {
            continue;
        }
        const size_t equals = trimmed.find('=');
        if (equals == std::string::npos) {
            continue;
        }
        callback(text::lower(text::trim(trimmed.substr(0, equals))),
                 text::trim(trimmed.substr(equals + 1)));
    }
}

const char *statusName(HistoryStatus status)
{
    switch (status) {
    case HistoryStatus::Saved:
        return "saved";
    case HistoryStatus::Cancelled:
        return "cancelled";
    case HistoryStatus::Installed:
        return "installed";
    case HistoryStatus::Failed:
        break;
    }
    return "failed";
}

bool parseStatus(const std::string &value, HistoryStatus &status)
{
    if (value == "saved") {
        status = HistoryStatus::Saved;
    } else if (value == "failed") {
        status = HistoryStatus::Failed;
    } else if (value == "cancelled") {
        status = HistoryStatus::Cancelled;
    } else if (value == "installed") {
        status = HistoryStatus::Installed;
    } else {
        return false;
    }
    return true;
}

const char *ciaActionName(CiaAction action)
{
    switch (action) {
    case CiaAction::Install:
        return "install";
    case CiaAction::InstallAndDelete:
        return "install_delete";
    case CiaAction::Keep:
        return "keep";
    case CiaAction::Ask:
        break;
    }
    return "ask";
}

const char *sortName(fs::SortOrder order)
{
    switch (order) {
    case fs::SortOrder::Size:
        return "size";
    case fs::SortOrder::Date:
        return "date";
    case fs::SortOrder::Type:
        return "type";
    case fs::SortOrder::Name:
        break;
    }
    return "name";
}

uint64_t fnv1a64(const std::string &value)
{
    uint64_t hash = 0xcbf29ce484222325ULL;
    for (const char character : value) {
        hash ^= static_cast<unsigned char>(character);
        hash *= 0x100000001b3ULL;
    }
    return hash;
}

} // namespace

Settings parseSettings(const std::string &contents)
{
    Settings settings;
    forEachPair(contents, [&](const std::string &key,
                              const std::string &value) {
        if (key == "destination" && !value.empty()) {
            settings.destination = value;
        } else if (key == "resume_downloads") {
            settings.resumeDownloads =
                parseBool(value, settings.resumeDownloads);
        } else if (key == "offer_zip_extraction") {
            settings.offerZipExtraction =
                parseBool(value, settings.offerZipExtraction);
        } else if (key == "after_cia_download") {
            for (const CiaAction action :
                 {CiaAction::Ask, CiaAction::Install,
                  CiaAction::InstallAndDelete, CiaAction::Keep}) {
                if (text::lower(value) == ciaActionName(action)) {
                    settings.afterCiaDownload = action;
                }
            }
        } else if (key == "max_connections") {
            char *end = nullptr;
            const long count = std::strtol(value.c_str(), &end, 10);
            if (!value.empty() && end != nullptr && *end == '\0' &&
                count >= 1 && count <= MaxDownloadConnections) {
                settings.maxConnections = static_cast<int>(count);
            }
        } else if (key == "sounds") {
            settings.sounds = parseBool(value, settings.sounds);
        } else if (key == "check_for_updates") {
            settings.checkForUpdates =
                parseBool(value, settings.checkForUpdates);
        } else if (key == "show_hidden_files") {
            settings.showHiddenFiles =
                parseBool(value, settings.showHiddenFiles);
        } else if (key == "file_sort_reverse") {
            settings.fileSortReverse =
                parseBool(value, settings.fileSortReverse);
        } else if (key == "file_sort") {
            for (const fs::SortOrder order :
                 {fs::SortOrder::Name, fs::SortOrder::Size,
                  fs::SortOrder::Date, fs::SortOrder::Type}) {
                if (text::lower(value) == sortName(order)) {
                    settings.fileSort = order;
                }
            }
        } else if (key == "title_sort_reverse") {
            settings.titleSortReverse =
                parseBool(value, settings.titleSortReverse);
        } else if (key == "title_sort") {
            for (const fs::SortOrder order :
                 {fs::SortOrder::Name, fs::SortOrder::Size}) {
                if (text::lower(value) == sortName(order)) {
                    settings.titleSort = order;
                }
            }
        } else if (key == "time_zone_minutes") {
            char *end = nullptr;
            const long minutes = std::strtol(value.c_str(), &end, 10);
            if (!value.empty() && end != nullptr && *end == '\0' &&
                tz::isValidOffset(static_cast<int>(minutes))) {
                settings.timeZoneSet = true;
                settings.timeZoneMinutes = static_cast<int>(minutes);
            }
        }
    });
    return settings;
}

std::string serializeSettings(const Settings &settings)
{
    std::string output = "# 3dsDL settings\n";
    output += "destination=" + text::singleLine(settings.destination) + "\n";
    output += std::string("resume_downloads=") +
              (settings.resumeDownloads ? "1" : "0") + "\n";
    output += std::string("offer_zip_extraction=") +
              (settings.offerZipExtraction ? "1" : "0") + "\n";
    output += std::string("after_cia_download=") +
              ciaActionName(settings.afterCiaDownload) + "\n";
    output += "max_connections=" + std::to_string(settings.maxConnections) +
              "\n";
    output += std::string("sounds=") + (settings.sounds ? "1" : "0") + "\n";
    output += std::string("check_for_updates=") +
              (settings.checkForUpdates ? "1" : "0") + "\n";
    output += std::string("show_hidden_files=") +
              (settings.showHiddenFiles ? "1" : "0") + "\n";
    output += std::string("file_sort=") + sortName(settings.fileSort) + "\n";
    output += std::string("file_sort_reverse=") +
              (settings.fileSortReverse ? "1" : "0") + "\n";
    output += std::string("title_sort=") + sortName(settings.titleSort) + "\n";
    output += std::string("title_sort_reverse=") +
              (settings.titleSortReverse ? "1" : "0") + "\n";
    if (settings.timeZoneSet) {
        output += "time_zone_minutes=" +
                  std::to_string(settings.timeZoneMinutes) + "\n";
    }
    return output;
}

std::string parseLegacyDestination(const std::string &contents)
{
    const std::vector<std::string> all = lines(contents);
    return all.empty() ? "" : text::trim(all.front());
}

std::vector<HistoryEntry> parseHistory(const std::string &contents)
{
    std::vector<HistoryEntry> entries;
    const std::vector<std::string> all = lines(contents);
    if (all.empty() || all.front() != HistoryHeader) {
        return entries;
    }

    for (size_t index = 1; index < all.size(); ++index) {
        const std::vector<std::string> fields = split(all[index], '\t');
        // The ninth field, the file's name, was added in 0.7.
        if (fields.size() != 8 && fields.size() != 9) {
            continue;
        }

        HistoryEntry entry;
        uint64_t time = 0;
        if (!parseUnsigned(fields[0], time) ||
            !parseStatus(fields[1], entry.status) ||
            !parseUnsigned(fields[2], entry.bytes) || fields[4].empty()) {
            continue;
        }
        entry.time = static_cast<int64_t>(time);
        entry.resumable = fields[3] == "1";
        entry.url = fields[4];
        entry.folder = fields[5];
        entry.path = fields[6];
        entry.message = fields[7];
        if (fields.size() == 9) {
            entry.name = fields[8];
        }
        entries.push_back(entry);
    }
    return entries;
}

std::string serializeHistory(const std::vector<HistoryEntry> &entries)
{
    std::string output = std::string(HistoryHeader) + "\n";
    for (const HistoryEntry &entry : entries) {
        output += std::to_string(entry.time < 0 ? 0 : entry.time) + "\t" +
                  statusName(entry.status) + "\t" +
                  std::to_string(entry.bytes) + "\t" +
                  (entry.resumable ? "1" : "0") + "\t" +
                  text::singleLine(entry.url) + "\t" +
                  text::singleLine(entry.folder) + "\t" +
                  text::singleLine(entry.path) + "\t" +
                  text::singleLine(entry.message) + "\t" +
                  text::singleLine(entry.name) + "\n";
    }
    return output;
}

void addHistoryEntry(std::vector<HistoryEntry> &entries, HistoryEntry entry,
                     size_t limit)
{
    entries.insert(entries.begin(), std::move(entry));
    if (entries.size() > limit) {
        entries.resize(limit);
    }
}

std::string serializeResumeInfo(const ResumeInfo &info)
{
    std::string output =
        std::string(info.split ? SplitResumeHeader : ResumeHeader) + "\n" +
        "url=" + text::singleLine(info.url) + "\n" +
        "etag=" + text::singleLine(info.etag) + "\n" +
        "last_modified=" + text::singleLine(info.lastModified) + "\n" +
        "filename=" + text::singleLine(info.filename) + "\n" +
        "total=" + std::to_string(info.total) + "\n";
    if (info.split) {
        output += "done=";
        for (size_t index = 0; index < info.done.size(); ++index) {
            output += (index > 0 ? "," : "") +
                      std::to_string(info.done[index].first) + "-" +
                      std::to_string(info.done[index].second);
        }
        output += "\n";
    }
    return output;
}

bool parseResumeInfo(const std::string &contents, ResumeInfo &info)
{
    const std::vector<std::string> all = lines(contents);
    if (all.empty() || (all.front() != ResumeHeader &&
                        all.front() != SplitResumeHeader)) {
        return false;
    }

    info = ResumeInfo();
    info.split = all.front() == SplitResumeHeader;
    bool valid = true;
    forEachPair(contents, [&](const std::string &key,
                              const std::string &value) {
        if (key == "url") {
            info.url = value;
        } else if (key == "etag") {
            info.etag = value;
        } else if (key == "last_modified") {
            info.lastModified = value;
        } else if (key == "filename") {
            info.filename = value;
        } else if (key == "total") {
            parseUnsigned(value, info.total);
        } else if (key == "done" && !value.empty()) {
            for (const std::string &range : split(value, ',')) {
                const std::vector<std::string> ends = split(range, '-');
                uint64_t first = 0;
                uint64_t second = 0;
                if (ends.size() != 2 || !parseUnsigned(ends[0], first) ||
                    !parseUnsigned(ends[1], second) || second <= first) {
                    valid = false;
                    return;
                }
                info.done.push_back({first, second});
            }
        }
    });
    return valid && !info.url.empty();
}

std::string ifRangeValidator(const ResumeInfo &info)
{
    // Weak ETags ("W/...") are not allowed in If-Range.
    if (!info.etag.empty() && !text::startsWithNoCase(info.etag, "W/")) {
        return info.etag;
    }
    return info.lastModified;
}

PartialPaths partialPaths(const std::string &folder, const std::string &url)
{
    char hash[17];
    std::snprintf(hash, sizeof(hash), "%016llx",
                  static_cast<unsigned long long>(fnv1a64(url)));
    const std::string stem = folder + ".3dsdl-" + hash;
    return {stem + ".part", stem + ".resume"};
}

int64_t contentRangeTotal(const std::string &value)
{
    const size_t slash = value.rfind('/');
    if (slash == std::string::npos) {
        return -1;
    }
    uint64_t total = 0;
    if (!parseUnsigned(text::trim(value.substr(slash + 1)), total)) {
        return -1;
    }
    return static_cast<int64_t>(total);
}
