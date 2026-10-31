#include "file_ops.hpp"

#include "fs_util.hpp"
#include "text.hpp"

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <dirent.h>
#include <map>
#include <memory>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

namespace fileops {
namespace {

// Large enough that the SD card sees few requests, small enough to fit
// comfortably next to everything else on an Old 3DS.
constexpr size_t CopyChunk = 256 * 1024;

std::string join(const std::string &folder, const std::string &entry)
{
    return folder.empty() || folder.back() == '/' ? folder + entry
                                                  : folder + "/" + entry;
}

// Names in folder, without "." and "..".
bool children(const std::string &folder, std::vector<std::string> &names)
{
    DIR *directory = opendir(folder.c_str());
    if (directory == nullptr) {
        return false;
    }
    while (dirent *item = readdir(directory)) {
        const std::string entry = item->d_name;
        if (entry != "." && entry != "..") {
            names.push_back(entry);
        }
    }
    closedir(directory);
    return true;
}

std::string failure(const std::string &what, const std::string &path)
{
    const int code = errno;
    std::string message = what + " " + name(path) + ".";
    if (code == ENOSPC) {
        message += " The SD card is full.";
    } else if (code != 0) {
        message += std::string(" ") + std::strerror(code) + ".";
    }
    return message;
}

Result copyFile(const std::string &source, const std::string &destination,
                Observer &observer, uint64_t &done, uint64_t total)
{
    Result result;
    std::FILE *input = std::fopen(source.c_str(), "rb");
    if (input == nullptr) {
        result.status = Status::Failed;
        result.message = failure("Could not open", source);
        return result;
    }
    std::FILE *output = std::fopen(destination.c_str(), "wb");
    if (output == nullptr) {
        result.status = Status::Failed;
        result.message = failure("Could not create", destination);
        std::fclose(input);
        return result;
    }

    std::unique_ptr<char[]> buffer(new char[CopyChunk]);
    const std::string current = name(source);
    observer.onProgress(done, total, current);
    while (true) {
        if (observer.cancelRequested()) {
            result.status = Status::Cancelled;
            break;
        }
        const size_t read = std::fread(buffer.get(), 1, CopyChunk, input);
        if (read == 0) {
            if (std::ferror(input)) {
                result.status = Status::Failed;
                result.message = failure("Could not read", source);
            }
            break;
        }
        if (std::fwrite(buffer.get(), 1, read, output) != read) {
            result.status = Status::Failed;
            result.message = failure("Could not write", destination);
            break;
        }
        done += read;
        observer.onProgress(done, total, current);
    }

    std::fclose(input);
    if (std::fclose(output) != 0 && result.status == Status::Ok) {
        result.status = Status::Failed;
        result.message = failure("Could not finish writing", destination);
    }
    if (result.status != Status::Ok) {
        std::remove(destination.c_str());
    } else {
        result.files = 1;
    }
    return result;
}

// From the folder listings alone, which carry each file's size: asking the
// SD card about every entry one at a time took a minute for a folder of
// songs on the console.
void measureFolder(const std::string &folder, Totals &totals,
                   const std::atomic<bool> *stop, std::atomic<uint64_t> *bytes,
                   const MeasureHooks *hooks)
{
    Totals known;
    if (hooks != nullptr && hooks->known && hooks->known(folder, known)) {
        totals.files += known.files;
        totals.folders += known.folders;
        totals.bytes += known.bytes;
        if (bytes != nullptr) {
            *bytes += known.bytes;
        }
        return;
    }
    const Totals before = totals;
    uint64_t print = 0;
    std::vector<std::string> folders;
    const bool listed = fs::readEntries(
        folder, true, false,
        [&](std::vector<fs::Entry> &&batch) {
            uint64_t counted = 0;
            for (const fs::Entry &entry : batch) {
                fs::addToFingerprint(print, entry);
                if (entry.folder) {
                    ++totals.folders;
                    folders.push_back(entry.name);
                } else {
                    ++totals.files;
                    counted += entry.size > 0 ? entry.size : 0;
                }
            }
            totals.bytes += counted;
            if (bytes != nullptr) {
                *bytes += counted;
            }
        },
        stop);
    for (const std::string &name : folders) {
        if (stop != nullptr && stop->load()) {
            return;
        }
        measureFolder(join(folder, name), totals, stop, bytes, hooks);
    }
    if (listed && hooks != nullptr && hooks->measured &&
        (stop == nullptr || !stop->load())) {
        Totals own;
        own.files = totals.files - before.files;
        own.folders = totals.folders - before.folders;
        own.bytes = totals.bytes - before.bytes;
        hooks->measured(folder, own, print);
    }
}

} // namespace

Totals measure(const std::string &path, const std::atomic<bool> *stop,
               std::atomic<uint64_t> *bytes, const MeasureHooks *hooks,
               Lookup *lookup)
{
    Totals totals;
    bool folder = false;
    int64_t size = 0;
    if (lookup != nullptr) {
        if (!lookup->exists(path)) {
            return totals;
        }
        folder = lookup->isFolder(path);
        size = std::max<int64_t>(0, lookup->size(path));
    } else {
        struct stat info {};
        if (stat(path.c_str(), &info) != 0) {
            return totals;
        }
        folder = S_ISDIR(info.st_mode);
        size = static_cast<int64_t>(info.st_size);
    }
    if (folder) {
        measureFolder(path, totals, stop, bytes, hooks);
    } else {
        totals.files = 1;
        totals.bytes = static_cast<uint64_t>(size);
        if (bytes != nullptr) {
            *bytes += totals.bytes;
        }
    }
    return totals;
}

Result copy(const std::string &source, const std::string &destination,
            Observer &observer, uint64_t &done, uint64_t total)
{
    Result result;
    if (exists(destination)) {
        result.status = Status::Failed;
        result.message = name(destination) + " already exists.";
        return result;
    }
    if (!isFolder(source)) {
        return copyFile(source, destination, observer, done, total);
    }
    if (isInside(destination, source)) {
        result.status = Status::Failed;
        result.message = "A folder cannot be copied into itself.";
        return result;
    }
    if (mkdir(destination.c_str(), 0777) != 0) {
        result.status = Status::Failed;
        result.message = failure("Could not create the folder", destination);
        return result;
    }
    std::vector<std::string> names;
    if (!children(source, names)) {
        result.status = Status::Failed;
        result.message = failure("Could not open the folder", source);
        return result;
    }
    for (const std::string &entry : names) {
        const Result part = copy(join(source, entry),
                                 join(destination, entry), observer, done,
                                 total);
        result.files += part.files;
        if (part.status != Status::Ok) {
            result.status = part.status;
            result.message = part.message;
            return result;
        }
    }
    return result;
}

Result remove(const std::string &path, Observer &observer, uint64_t &done,
              uint64_t total)
{
    Result result;
    if (observer.cancelRequested()) {
        result.status = Status::Cancelled;
        return result;
    }
    if (isFolder(path)) {
        std::vector<std::string> names;
        if (!children(path, names)) {
            result.status = Status::Failed;
            result.message = failure("Could not open the folder", path);
            return result;
        }
        for (const std::string &entry : names) {
            const Result part =
                remove(join(path, entry), observer, done, total);
            result.files += part.files;
            if (part.status != Status::Ok) {
                result.status = part.status;
                result.message = part.message;
                return result;
            }
        }
        if (rmdir(path.c_str()) != 0) {
            result.status = Status::Failed;
            result.message = failure("Could not delete the folder", path);
        }
        return result;
    }
    observer.onProgress(done, total, name(path));
    if (std::remove(path.c_str()) != 0) {
        result.status = Status::Failed;
        result.message = failure("Could not delete", path);
        return result;
    }
    result.files = 1;
    ++done;
    observer.onProgress(done, total, name(path));
    return result;
}

bool move(const std::string &source, const std::string &destination,
          std::string &error)
{
    // FAT ignores case, so a name that changes only in case is the entry
    // itself: it goes through a spare name, and back if the second step
    // fails.
    std::string from = source;
    std::string to = destination;
    while (from.size() > 1 && from.back() == '/') {
        from.pop_back();
    }
    while (to.size() > 1 && to.back() == '/') {
        to.pop_back();
    }
    if (text::lower(from) == text::lower(to)) {
        const std::string folder = from.substr(0, from.find_last_of('/') + 1);
        const std::string target = folder + name(to);
        if (target == from) {
            return true;
        }
        std::string spare = folder + ".3dsdl-rename";
        for (int count = 1; exists(spare); ++count) {
            spare = folder + ".3dsdl-rename-" + std::to_string(count);
        }
        if (std::rename(from.c_str(), spare.c_str()) != 0) {
            error = failure("Could not rename", source);
            return false;
        }
        if (std::rename(spare.c_str(), target.c_str()) != 0) {
            error = failure("Could not rename", source);
            std::rename(spare.c_str(), from.c_str());
            return false;
        }
        return true;
    }
    if (exists(destination)) {
        error = name(destination) + " already exists.";
        return false;
    }
    if (isFolder(source) && isInside(destination, source)) {
        error = "A folder cannot be moved into itself.";
        return false;
    }
    if (std::rename(source.c_str(), destination.c_str()) != 0) {
        error = failure("Could not move", source);
        return false;
    }
    return true;
}

Result merge(const std::string &source, const std::string &destination,
             bool moving, Observer &observer, uint64_t &done, uint64_t total)
{
    Result result;
    if (isInside(destination, source)) {
        result.status = Status::Failed;
        result.message = "A folder cannot be merged into itself.";
        return result;
    }
    std::vector<std::string> names;
    if (!children(source, names)) {
        result.status = Status::Failed;
        result.message = failure("Could not open the folder", source);
        return result;
    }
    for (const std::string &entry : names) {
        if (observer.cancelRequested()) {
            result.status = Status::Cancelled;
            return result;
        }
        const std::string from = join(source, entry);
        const std::string to = join(destination, entry);
        Result part;
        if (exists(to) && isFolder(from) && isFolder(to)) {
            part = merge(from, to, moving, observer, done, total);
        } else {
            if (exists(to)) {
                uint64_t removed = 0;
                const Result cleared = remove(to, observer, removed, 0);
                if (cleared.status != Status::Ok) {
                    return cleared;
                }
            }
            if (moving) {
                if (!move(from, to, part.message)) {
                    part.status = Status::Failed;
                } else {
                    part.files = 1;
                }
            } else {
                part = copy(from, to, observer, done, total);
            }
        }
        result.files += part.files;
        if (part.status != Status::Ok) {
            result.status = part.status;
            result.message = part.message;
            return result;
        }
    }
    if (moving && rmdir(source.c_str()) != 0) {
        result.status = Status::Failed;
        result.message = failure("Could not delete the folder", source);
    }
    return result;
}

bool isFolder(const std::string &path)
{
    struct stat info {};
    return stat(path.c_str(), &info) == 0 && S_ISDIR(info.st_mode);
}

bool exists(const std::string &path)
{
    struct stat info {};
    return stat(path.c_str(), &info) == 0;
}

std::map<std::string, Lookup::Known> &Lookup::listing(const std::string &folder)
{
    const auto found = folders_.find(folder);
    if (found != folders_.end()) {
        return found->second;
    }
    std::map<std::string, Known> &names = folders_[folder];
    for (const fs::Entry &entry : fs::listEntries(folder, nullptr, true, false)) {
        names[text::lower(entry.name)] = {entry.folder,
                                          entry.folder ? -1 : entry.size};
    }
    return names;
}

const Lookup::Known *Lookup::find(const std::string &path)
{
    const size_t slash = path.find_last_of('/');
    if (slash == std::string::npos || slash + 1 == path.size()) {
        return nullptr;
    }
    const std::map<std::string, Known> &names = listing(path.substr(0, slash + 1));
    const auto found = names.find(text::lower(path.substr(slash + 1)));
    return found == names.end() ? nullptr : &found->second;
}

bool Lookup::exists(const std::string &path)
{
    return find(path) != nullptr;
}

bool Lookup::isFolder(const std::string &path)
{
    const Known *known = find(path);
    return known != nullptr && known->folder;
}

int64_t Lookup::size(const std::string &path)
{
    const Known *known = find(path);
    return known != nullptr ? known->size : -1;
}

void Lookup::claim(const std::string &path, bool folder)
{
    const size_t slash = path.find_last_of('/');
    if (slash != std::string::npos && slash + 1 < path.size()) {
        listing(path.substr(0, slash + 1))[text::lower(path.substr(slash + 1))] =
            {folder, -1};
    }
}

bool isInside(const std::string &path, const std::string &folder)
{
    std::string base = folder;
    while (base.size() > 1 && base.back() == '/') {
        base.pop_back();
    }
    return path == base ||
           (path.size() > base.size() && path.compare(0, base.size(), base) == 0 &&
            path[base.size()] == '/');
}

std::string name(const std::string &path)
{
    std::string trimmed = path;
    while (trimmed.size() > 1 && trimmed.back() == '/') {
        trimmed.pop_back();
    }
    return trimmed.substr(trimmed.find_last_of('/') + 1);
}

bool validName(const std::string &value, std::string &error)
{
    const std::string trimmed = text::trim(value);
    if (trimmed.empty()) {
        error = "The name is empty.";
        return false;
    }
    if (trimmed == "." || trimmed == "..") {
        error = "\".\" and \"..\" cannot be used as names.";
        return false;
    }
    if (trimmed.find_first_of("\\/:*?\"<>|") != std::string::npos) {
        error = "Names cannot contain \\ / : * ? \" < > or |.";
        return false;
    }
    for (const char character : trimmed) {
        if (static_cast<unsigned char>(character) < 0x20) {
            error = "Names cannot contain control characters.";
            return false;
        }
    }
    if (trimmed.size() > 255) {
        error = "The name is too long.";
        return false;
    }
    if (trimmed.back() == '.') {
        error = "Names cannot end with a dot.";
        return false;
    }
    return true;
}

int64_t modifiedTime(const std::string &path)
{
    struct stat info {};
    if (stat(path.c_str(), &info) != 0) {
        return 0;
    }
    return fs::modifiedTime(path, static_cast<int64_t>(info.st_mtime));
}

} // namespace fileops
