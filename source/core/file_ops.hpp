#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <map>
#include <string>

// Copying, moving, deleting and measuring files and whole folders for Files.
// Paths name the entry itself, without a trailing '/', even for folders:
// "sdmc:/3ds/app", not "sdmc:/3ds/app/".
namespace fileops {

class Observer {
public:
    virtual ~Observer() = default;
    virtual bool cancelRequested() = 0;
    // done and total count bytes across the whole operation; current is the
    // file being worked on.
    virtual void onProgress(uint64_t done, uint64_t total,
                            const std::string &current)
    {
        (void)done;
        (void)total;
        (void)current;
    }
};

struct Totals {
    uint64_t files = 0;
    uint64_t folders = 0; // below path, not counting path itself
    uint64_t bytes = 0;
};

// Lets measure skip folders added up before and keep the ones it adds up.
struct MeasureHooks {
    // The totals for folder when already known, to use instead of reading it.
    std::function<bool(const std::string &folder, Totals &totals)> known;
    // Each folder read whole, with its totals and fs::fingerprint of its own
    // entries.
    std::function<void(const std::string &folder, const Totals &totals,
                       uint64_t fingerprint)>
        measured;
};

class Lookup;

// Everything at path, which can be a file or a folder. Setting stop ends
// the count early, with what was counted so far. bytes, when given, grows by
// each size as it is counted, for a running total. lookup, when given,
// answers what path itself is, for callers measuring many paths.
Totals measure(const std::string &path,
               const std::atomic<bool> *stop = nullptr,
               std::atomic<uint64_t> *bytes = nullptr,
               const MeasureHooks *hooks = nullptr, Lookup *lookup = nullptr);

enum class Status { Ok, Cancelled, Failed };

struct Result {
    Status status = Status::Ok;
    std::string message; // set when status is not Ok
    uint64_t files = 0;  // files copied or deleted
};

// Copies the file or folder at source to destination, which must not exist.
// done is advanced by the bytes copied, so several copies can share one
// progress bar of total bytes. A cancelled or failed file copy is removed;
// files already copied stay.
Result copy(const std::string &source, const std::string &destination,
            Observer &observer, uint64_t &done, uint64_t total);

// Deletes a file, or a folder with everything in it. done counts deleted
// files against total, like copy does with bytes.
Result remove(const std::string &path, Observer &observer, uint64_t &done,
              uint64_t total);

// Renames or moves within the SD card. Fails when destination exists,
// unless it is source itself named in another case.
bool move(const std::string &source, const std::string &destination,
          std::string &error);

// Puts what is in the folder source into the folder destination, which
// exists. An entry there with the same name is replaced, except that a
// folder meeting a folder is merged the same way. With moving set, entries
// are moved and source is deleted once empty; otherwise they are copied,
// with done and total as for copy.
Result merge(const std::string &source, const std::string &destination,
             bool moving, Observer &observer, uint64_t &done, uint64_t total);

bool isFolder(const std::string &path);
bool exists(const std::string &path);
// True when path is folder itself or anything below it.
bool isInside(const std::string &path, const std::string &folder);
// Last part of a path: "sdmc:/a/b" -> "b".
std::string name(const std::string &path);

// Answers about many paths from their folders' listings, reading each folder
// once. On the 3DS every exists, isFolder or size question searches the
// path's folder on the card, so asking about hundreds of paths in a big
// folder one by one takes minutes. Names match whatever their case, as on
// FAT. A folder that changes after it was read is not read again.
class Lookup {
public:
    bool exists(const std::string &path);
    bool isFolder(const std::string &path);
    // The file's size, or -1 for a folder or a path that is not there.
    int64_t size(const std::string &path);
    // Counts path as there from now on, as when it is about to be made.
    void claim(const std::string &path, bool folder);

private:
    struct Known {
        bool folder = false;
        int64_t size = -1;
    };
    // The entry at path, or nullptr when there is none.
    const Known *find(const std::string &path);
    std::map<std::string, Known> &listing(const std::string &folder);
    // Folder ("sdmc:/a/") to lower-case name to entry.
    std::map<std::string, std::map<std::string, Known>> folders_;
};

// Checks a name typed for a new or renamed file or folder. FAT does not
// allow \ / : * ? " < > | and names cannot be "." or "..".
bool validName(const std::string &name, std::string &error);

// Seconds since 1970 when path was last changed, or 0 when unknown.
int64_t modifiedTime(const std::string &path);

} // namespace fileops
