#include "archives.hpp"

#include "text.hpp"

#include <archive.h>
#include <archive_entry.h>

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <memory>
#include <sys/stat.h>
#include <unordered_set>

namespace archives {
namespace {

constexpr size_t BlockSize = 256 * 1024;
// How often a damaged tar may ask to look for the next header in a row.
constexpr int MaxRetries = 100;

// ZIP methods libarchive reads and 3dsDL's own reader does not: bzip2, LZMA,
// Zstandard and XZ.
bool libarchiveZipMethod(uint16_t method)
{
    return method == 12 || method == 14 || method == 93 || method == 95;
}

struct Closer {
    void operator()(struct archive *handle) const
    {
        archive_read_free(handle);
    }
};
using Handle = std::unique_ptr<struct archive, Closer>;

// libarchive's last error as a sentence.
std::string describe(struct archive *handle)
{
    if (archive_errno(handle) == ENOMEM) {
        return "Not enough memory to read this archive.";
    }
    const char *message = archive_error_string(handle);
    if (message == nullptr || *message == '\0') {
        return "This archive cannot be read.";
    }
    std::string text = message;
    if (text.find("Unrecognized archive format") != std::string::npos) {
        return "This is not an archive 3dsDL can read.";
    }
    if (text.back() != '.') {
        text += '.';
    }
    return text;
}

Handle open(const std::string &path, std::string &error)
{
    Handle handle(archive_read_new());
    struct archive *raw = handle.get();
    archive_read_support_format_7zip(raw);
    archive_read_support_format_rar(raw);
    archive_read_support_format_rar5(raw);
    archive_read_support_format_tar(raw);
    archive_read_support_format_zip_seekable(raw);
    archive_read_support_filter_gzip(raw);
    archive_read_support_filter_bzip2(raw);
    archive_read_support_filter_xz(raw);
    archive_read_support_filter_lzma(raw);
    archive_read_support_filter_zstd(raw);
    if (archive_read_open_filename(raw, path.c_str(), BlockSize) !=
        ARCHIVE_OK) {
        error = describe(raw);
        return nullptr;
    }
    return handle;
}

std::string formatName(struct archive *handle)
{
    std::string name;
    switch (archive_format(handle) & ARCHIVE_FORMAT_BASE_MASK) {
    case ARCHIVE_FORMAT_7ZIP:
        name = "7z";
        break;
    case ARCHIVE_FORMAT_RAR:
    case ARCHIVE_FORMAT_RAR_V5:
        name = "RAR";
        break;
    case ARCHIVE_FORMAT_TAR:
        name = "tar";
        break;
    case ARCHIVE_FORMAT_ZIP:
        name = "ZIP";
        break;
    default:
        name = "Archive";
        break;
    }
    switch (archive_filter_code(handle, 0)) {
    case ARCHIVE_FILTER_GZIP:
        return name + ".gz";
    case ARCHIVE_FILTER_BZIP2:
        return name + ".bz2";
    case ARCHIVE_FILTER_XZ:
        return name + ".xz";
    case ARCHIVE_FILTER_LZMA:
        return name + ".lzma";
    case ARCHIVE_FILTER_ZSTD:
        return name + ".zst";
    default:
        return name;
    }
}

Entry entryOf(struct archive_entry *header)
{
    const char *name = archive_entry_pathname_utf8(header);
    if (name == nullptr) {
        name = archive_entry_pathname(header);
    }
    const std::string raw = name != nullptr ? name : "";
    const mode_t type = archive_entry_filetype(header);

    Entry entry;
    entry.name = text::sanitizeUtf8(raw);
    entry.path = safeRelativePath(entry.name);
    entry.directory = type == AE_IFDIR || (!raw.empty() && raw.back() == '/');
    if (archive_entry_size_is_set(header) && archive_entry_size(header) > 0) {
        entry.size = static_cast<uint64_t>(archive_entry_size(header));
    }
    // Links have nothing to point at on FAT.
    entry.supported = entry.directory ||
                      (type == AE_IFREG &&
                       archive_entry_hardlink(header) == nullptr &&
                       !archive_entry_is_data_encrypted(header));
    return entry;
}

void add(Listing &listing, Entry entry)
{
    if (macMetadata(entry.path)) {
        return;
    }
    if (!entry.directory) {
        ++listing.fileCount;
        listing.totalSize += entry.size;
    }
    listing.entries.push_back(std::move(entry));
}

void findTop(Listing &listing)
{
    std::string top;
    bool folder = false;
    for (const Entry &entry : listing.entries) {
        if (entry.path.empty()) {
            continue;
        }
        const size_t slash = entry.path.find('/');
        const std::string head = entry.path.substr(0, slash);
        if (top.empty()) {
            top = head;
        } else if (text::lower(head) != text::lower(top)) {
            return;
        }
        folder = folder || slash != std::string::npos || entry.directory;
    }
    listing.top = top;
    listing.topIsFolder = folder;
}

bool startsWith(const std::string &path, const char *magic, size_t size)
{
    FILE *file = std::fopen(path.c_str(), "rb");
    if (file == nullptr) {
        return false;
    }
    unsigned char head[8] = {};
    const size_t got = std::fread(head, 1, size, file);
    std::fclose(file);
    return got == size && std::memcmp(head, magic, size) == 0;
}

bool looksLikeZip(const std::string &path)
{
    return text::endsWithNoCase(path, ".zip") ||
           startsWith(path, "PK\x03\x04", 4) ||
           startsWith(path, "PK\x05\x06", 4);
}

bool solidRarFile(const std::string &path)
{
    FILE *file = std::fopen(path.c_str(), "rb");
    if (file == nullptr) {
        return false;
    }
    unsigned char head[64] = {};
    const size_t got = std::fread(head, 1, sizeof(head), file);
    std::fclose(file);
    return solidRar(head, got);
}

bool readZip(const std::string &path, Listing &listing, std::string &error)
{
    zip::Archive zipArchive;
    if (!zip::readArchive(path, zipArchive, error)) {
        return false;
    }
    listing.format = "ZIP";
    listing.complete = true;
    bool needsLibarchive = false;
    for (zip::Entry &source : zipArchive.entries) {
        Entry entry;
        // Only the listing shows names; the ZIP reader needs paths.
        entry.name = std::move(source.name);
        entry.path = source.path;
        entry.size = source.size;
        entry.directory = source.directory;
        if (!source.directory && !zip::readable(source)) {
            if (!zip::encrypted(source) && libarchiveZipMethod(source.method)) {
                needsLibarchive = true;
            } else {
                entry.supported = false;
            }
        }
        add(listing, std::move(entry));
    }
    listing.ownZip = !needsLibarchive;
    if (listing.ownZip) {
        listing.zip = std::move(zipArchive);
    }
    findTop(listing);
    return true;
}

// Reads past an entry's data in blocks, so a scan that has to decompress
// can be stopped and shows how far it got.
bool skipData(struct archive *handle, Progress &progress, Observer *observer)
{
    while (true) {
        const void *block = nullptr;
        size_t size = 0;
        la_int64_t offset = 0;
        const int status =
            archive_read_data_block(handle, &block, &size, &offset);
        if (status == ARCHIVE_EOF || status == ARCHIVE_FAILED) {
            return true;
        }
        if (status != ARCHIVE_OK && status != ARCHIVE_WARN) {
            return true; // the next header reports it
        }
        if (observer != nullptr) {
            progress.archiveBytes =
                static_cast<uint64_t>(archive_filter_bytes(handle, -1));
            observer->onProgress(progress);
            if (observer->cancelRequested()) {
                return false;
            }
        }
    }
}

bool readOther(const std::string &path, Scan scan, Listing &listing,
               std::string &error, Observer *observer)
{
    Handle handle = open(path, error);
    if (!handle) {
        return false;
    }
    struct archive *raw = handle.get();
    bool streamed = false;
    int retries = 0;
    Progress progress;
    while (true) {
        if (observer != nullptr && observer->cancelRequested()) {
            error.clear();
            return false;
        }
        struct archive_entry *header = nullptr;
        const int status = archive_read_next_header(raw, &header);
        if (status == ARCHIVE_EOF) {
            break;
        }
        // libarchive is looking for the next good header. A header that
        // failed leaves its data unread, so nothing after it can be
        // trusted.
        if (status == ARCHIVE_RETRY && ++retries <= MaxRetries) {
            continue;
        }
        if (status < ARCHIVE_WARN || status == ARCHIVE_RETRY) {
            error = describe(raw);
            return false;
        }
        retries = 0;
        if (listing.format.empty()) {
            listing.format = formatName(raw);
            // Past a compression filter or inside a solid RAR, getting to
            // the next header means decompressing this entry's data.
            const int base = archive_format(raw) & ARCHIVE_FORMAT_BASE_MASK;
            streamed = archive_filter_count(raw) > 1 ||
                       ((base == ARCHIVE_FORMAT_RAR ||
                         base == ARCHIVE_FORMAT_RAR_V5) &&
                        solidRarFile(path));
            if (streamed && scan == Scan::Quick) {
                return true;
            }
        }
        add(listing, entryOf(header));
        if (streamed && !skipData(raw, progress, observer)) {
            error.clear();
            return false;
        }
        if (observer != nullptr) {
            progress.archiveBytes =
                static_cast<uint64_t>(archive_filter_bytes(raw, -1));
            observer->onProgress(progress);
        }
    }
    if (listing.format.empty()) {
        listing.format = formatName(raw);
    }
    listing.complete = true;
    findTop(listing);
    return true;
}

Result extractOther(const std::string &path, const std::string &folder,
                    const Options &options, Observer &observer)
{
    Result result;
    Handle handle = open(path, result.message);
    if (!handle) {
        return result;
    }
    struct archive *raw = handle.get();
    Writer writer(folder, options.existing);
    if (!writer.makeRoot()) {
        result.message = writer.error();
        return result;
    }
    const std::unordered_set<std::string> only(options.only.begin(),
                                               options.only.end());
    const auto stop = [&]() {
        writer.abandon();
        result.cancelled = true;
        result.message = "Extraction cancelled.";
        return result;
    };

    Progress progress;
    int retries = 0;
    while (true) {
        if (observer.cancelRequested()) {
            return stop();
        }
        struct archive_entry *header = nullptr;
        int status = archive_read_next_header(raw, &header);
        if (status == ARCHIVE_EOF) {
            break;
        }
        if (status == ARCHIVE_RETRY && ++retries <= MaxRetries) {
            continue;
        }
        if (status < ARCHIVE_WARN || status == ARCHIVE_RETRY) {
            result.message = describe(raw);
            return result;
        }
        retries = 0;
        const Entry entry = entryOf(header);
        if (macMetadata(entry.path)) {
            continue;
        }
        if (entry.path.empty()) {
            if (!entry.directory) {
                ++result.skippedUnsafe;
            }
            continue;
        }
        if (!only.empty() && only.count(entry.path) == 0) {
            if (options.place && !entry.directory) {
                ++result.skippedPlaced;
            }
            continue;
        }
        std::string moved;
        if (options.place) {
            moved = options.place(entry.path, entry.directory);
            if (moved.empty()) {
                if (!entry.directory) {
                    ++result.skippedPlaced;
                }
                continue;
            }
        }
        const std::string &written = options.place ? moved : entry.path;
        if (entry.directory) {
            if (!writer.makeFolder(written)) {
                result.message = writer.error();
                return result;
            }
            continue;
        }
        progress.current = written;
        if (!entry.supported) {
            ++result.skippedUnsupported;
            progress.bytes += entry.size;
            ++progress.files;
            continue;
        }

        const int64_t size = archive_entry_size_is_set(header)
                                 ? archive_entry_size(header)
                                 : -1;
        const Writer::Start start = writer.start(written, size);
        if (start == Writer::Start::Failed) {
            result.message = writer.error();
            return result;
        }
        if (start == Writer::Start::Skip) {
            result.skippedExisting.push_back(written);
            progress.bytes += entry.size;
            ++progress.files;
            observer.onProgress(progress);
            continue;
        }

        bool unsupported = false;
        while (true) {
            const void *block = nullptr;
            size_t length = 0;
            la_int64_t offset = 0;
            status = archive_read_data_block(raw, &block, &length, &offset);
            if (status == ARCHIVE_EOF) {
                break;
            }
            if (status != ARCHIVE_OK) {
                writer.abandon();
                // A format refuses an entry it cannot decode before giving
                // any of it, and the next one can still be read.
                if (status == ARCHIVE_FAILED && writer.written() == 0) {
                    unsupported = true;
                    break;
                }
                result.message = entry.path + " is damaged. " + describe(raw);
                return result;
            }
            const uint64_t at = static_cast<uint64_t>(offset);
            if (at < writer.written() ||
                (size >= 0 && at + length > static_cast<uint64_t>(size))) {
                writer.abandon();
                result.message = entry.path + " is damaged.";
                return result;
            }
            const uint64_t hole = at - writer.written();
            if ((hole > 0 && !writer.writeZeros(hole)) ||
                !writer.write(block, length)) {
                writer.abandon();
                result.message = writer.error();
                return result;
            }
            progress.bytes += hole + length;
            progress.archiveBytes =
                static_cast<uint64_t>(archive_filter_bytes(raw, -1));
            observer.onProgress(progress);
            if (observer.cancelRequested()) {
                return stop();
            }
        }
        if (unsupported) {
            ++result.skippedUnsupported;
            progress.bytes += entry.size;
            ++progress.files;
            continue;
        }
        // Sparse files can end in a hole.
        if (size > 0 && archive_entry_sparse_count(header) > 0 &&
            writer.written() < static_cast<uint64_t>(size)) {
            const uint64_t hole = static_cast<uint64_t>(size) - writer.written();
            if (!writer.writeZeros(hole)) {
                writer.abandon();
                result.message = writer.error();
                return result;
            }
            progress.bytes += hole;
        }
        if (!writer.finish()) {
            writer.abandon();
            result.message = writer.error();
            return result;
        }
        ++result.extracted;
        ++progress.files;
        observer.onProgress(progress);
    }
    result.ok = true;
    return result;
}

std::string parentOf(const std::string &path)
{
    const size_t slash = path.find_last_of('/');
    return slash == std::string::npos ? "" : path.substr(0, slash);
}

} // namespace

bool solidRar(const unsigned char *data, size_t size)
{
    static const unsigned char Rar4[] = {'R', 'a', 'r', '!', 0x1a, 0x07, 0x00};
    static const unsigned char Rar5[] = {'R', 'a', 'r', '!',
                                         0x1a, 0x07, 0x01, 0x00};
    if (size >= 12 && std::memcmp(data, Rar4, sizeof(Rar4)) == 0) {
        // Marker, then the main header: CRC (2), type 0x73, flags (2).
        return data[9] == 0x73 && (data[10] & 0x08) != 0;
    }
    if (size < sizeof(Rar5) + 4 ||
        std::memcmp(data, Rar5, sizeof(Rar5)) != 0) {
        return false;
    }
    // Signature, CRC32, then variable-length numbers: header size, type
    // (1 for the main header), header flags, an extra area size and a data
    // size when the flags say so, and the archive flags (4 is solid).
    size_t position = sizeof(Rar5) + 4;
    const auto number = [&](uint64_t &value) {
        value = 0;
        for (int shift = 0; shift < 64 && position < size; shift += 7) {
            const unsigned char byte = data[position++];
            value |= static_cast<uint64_t>(byte & 0x7f) << shift;
            if ((byte & 0x80) == 0) {
                return true;
            }
        }
        return false;
    };
    uint64_t headerSize = 0;
    uint64_t type = 0;
    uint64_t flags = 0;
    uint64_t skipped = 0;
    uint64_t archiveFlags = 0;
    if (!number(headerSize) || !number(type) || type != 1 || !number(flags)) {
        return false;
    }
    if ((flags & 0x1) != 0 && !number(skipped)) {
        return false;
    }
    if ((flags & 0x2) != 0 && !number(skipped)) {
        return false;
    }
    return number(archiveFlags) && (archiveFlags & 0x4) != 0;
}

bool read(const std::string &path, Scan scan, Listing &listing,
          std::string &error, Observer *observer)
{
    listing = Listing();
    struct stat info {};
    if (stat(path.c_str(), &info) != 0) {
        error = "Could not open the archive.";
        return false;
    }
    listing.archiveSize = static_cast<uint64_t>(info.st_size);
    const bool read = looksLikeZip(path)
                          ? readZip(path, listing, error)
                          : readOther(path, scan, listing, error, observer);
    // The ZIP reader cannot be stopped partway, but a stop still counts.
    if (read && observer != nullptr && observer->cancelRequested()) {
        listing = Listing();
        error.clear();
        return false;
    }
    return read;
}

bool existingFiles(const Listing &listing, const std::string &folder,
                   std::vector<std::string> &existing, Observer *observer)
{
    existing.clear();
    FolderNames names(folder);
    Progress progress;
    for (const Entry &entry : listing.entries) {
        if (entry.directory || !entry.supported || entry.path.empty()) {
            continue;
        }
        if (names.contains(entry.path)) {
            existing.push_back(entry.path);
        }
        if (observer != nullptr && ++progress.files % 64 == 0) {
            observer->onProgress(progress);
            if (observer->cancelRequested()) {
                return false;
            }
        }
    }
    return true;
}

uint64_t spaceNeeded(const Listing &listing, uint64_t clusterSize)
{
    if (clusterSize == 0) {
        clusterSize = 1;
    }
    uint64_t total = 0;
    std::unordered_set<std::string> folders;
    for (const Entry &entry : listing.entries) {
        if (entry.path.empty() || !entry.supported) {
            continue;
        }
        if (!entry.directory) {
            // Nothing over 4 GiB fits in a FAT file, so that is the most
            // one can need, and the sum cannot overflow.
            const uint64_t size = std::min<uint64_t>(entry.size, 1ull << 32);
            total += (size / clusterSize + (size % clusterSize != 0)) *
                     clusterSize;
        }
        for (std::string folder = entry.directory ? entry.path
                                                  : parentOf(entry.path);
             !folder.empty() && folders.insert(folder).second;
             folder = parentOf(folder)) {
            total += clusterSize;
        }
    }
    return total;
}

Listing placed(const Listing &listing,
               const std::function<std::string(const std::string &path,
                                               bool directory)> &place)
{
    // Only what is shown and checked for clashes: not the ZIP reader's own
    // entries, which extracting takes from the archive's listing.
    Listing out;
    out.format = listing.format;
    out.archiveSize = listing.archiveSize;
    out.complete = listing.complete;
    for (const Entry &entry : listing.entries) {
        if (entry.path.empty() || macMetadata(entry.path)) {
            continue;
        }
        std::string path = place(entry.path, entry.directory);
        if (path.empty()) {
            continue;
        }
        Entry moved;
        moved.name = entry.name;
        moved.path = std::move(path);
        moved.size = entry.size;
        moved.directory = entry.directory;
        moved.supported = entry.supported;
        add(out, std::move(moved));
    }
    findTop(out);
    return out;
}

Result extract(const std::string &path, const Listing &listing,
               const std::string &folder, const Options &options,
               Observer &observer)
{
    if (listing.ownZip) {
        return zip::extract(path, listing.zip, folder, options, observer);
    }
    return extractOther(path, folder, options, observer);
}

} // namespace archives
