#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

// What extracting shares between the ZIP reader and libarchive: progress,
// results, safe names, and the Writer that puts files on the SD card.
namespace archives {

enum class Existing { Overwrite, Skip };

struct Options {
    Existing existing = Existing::Skip;
    // When not empty, only these relative paths are extracted.
    std::vector<std::string> only;
    // When set, where each entry goes instead of its own path, relative to
    // the folder, or "" to leave it out. It sees the name an entry is
    // written under, which for a ZIP read by libarchive can differ from the
    // name its listing gave.
    std::function<std::string(const std::string &path, bool directory)> place;
};

struct Progress {
    uint64_t bytes = 0;        // of the files' sizes, written or skipped
    unsigned files = 0;        // files written or skipped
    uint64_t archiveBytes = 0; // read from the archive file so far
    std::string current;       // the file being written
};

class Observer {
public:
    virtual ~Observer() = default;
    virtual bool cancelRequested() = 0;
    virtual void onProgress(const Progress &progress) = 0;
};

struct Result {
    bool ok = false;
    bool cancelled = false;
    unsigned extracted = 0;
    unsigned skippedUnsafe = 0;      // paths that tried to leave the folder
    unsigned skippedUnsupported = 0; // encrypted, links, unknown compression
    unsigned skippedPlaced = 0;      // files Options::place left out
    // Files left alone because they were already there, as relative paths
    // (where Options::place put them).
    std::vector<std::string> skippedExisting;
    std::string message;
};

// "a\\b/../c" style names become "" (rejected); normal names come back with
// '/' separators and each component made safe for FAT.
std::string safeRelativePath(const std::string &name);

// What a Mac adds to archives it makes: __MACOSX folders, ._ files beside
// each real one, and .DS_Store. They are never extracted.
bool macMetadata(const std::string &path);

// The names in each folder under root, read once per folder, so asking
// whether a file is there costs no lookup on the SD card. Case is ignored,
// as FAT does.
class FolderNames {
public:
    explicit FolderNames(std::string root) : root_(std::move(root)) {}

    bool contains(const std::string &path);
    // Marks a folder this run made, so it is never read.
    void madeEmpty(const std::string &folder);

private:
    const std::unordered_set<std::string> &namesIn(const std::string &folder);

    std::string root_;
    std::unordered_map<std::string, std::unordered_set<std::string>> folders_;
};

// Puts files into a folder with as few calls to the SD card as it can. On
// FAT every lookup reads through the whole folder, which is what made 5,000
// files take hours: a small file is an open that creates it, a size check
// on the open file, one write and a close. A larger one takes its whole size first, is written in 256 KiB
// pieces under a temporary name and renamed once whole. Existing names come
// from FolderNames, and each folder is made once. A file that is already
// there is never opened for writing: it is skipped, or replaced from a
// temporary file, so a damaged entry never costs the old one. A folder is
// never replaced.
class Writer {
public:
    Writer(std::string folder, Existing existing);
    ~Writer();
    Writer(const Writer &) = delete;
    Writer &operator=(const Writer &) = delete;

    // Makes the folder itself. One that was not there is known to be empty.
    bool makeRoot();
    // Makes the folder at relative path, and its parents, once.
    bool makeFolder(const std::string &path);

    enum class Start { Write, Skip, Failed };
    // Starts the file at relative path. size is -1 when the archive does
    // not say.
    Start start(const std::string &path, int64_t size);
    bool write(const void *data, size_t size);
    // Writes count zero bytes, for holes in sparse files.
    bool writeZeros(uint64_t count);
    bool finish();
    // Closes and removes the file being written.
    void abandon();

    uint64_t written() const { return written_; }
    const std::string &error() const { return error_; }

private:
    bool open(bool truncate);
    bool flush();
    bool fail(const std::string &message);

    std::string folder_;
    Existing existing_;
    FolderNames names_;
    std::unordered_set<std::string> made_;
    std::unordered_set<std::string> done_; // files written this run, lower case
    std::unique_ptr<unsigned char[]> buffer_;
    size_t buffered_ = 0;

    int fd_ = -1;
    std::string path_;    // relative, for messages
    std::string target_;  // where the file ends up
    std::string temporary_;
    std::string writing_; // what is open: target_ or temporary_
    bool replacing_ = false; // target_ was there before
    int64_t size_ = -1;
    uint64_t written_ = 0;
    std::string error_;
};

} // namespace archives
