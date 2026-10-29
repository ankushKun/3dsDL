#include "zip.hpp"

#include "text.hpp"

#include <zlib.h>

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <memory>
#include <sys/types.h>
#include <unordered_set>

namespace zip {
namespace {

constexpr uint32_t LocalHeaderSignature = 0x04034b50;
constexpr uint32_t CentralHeaderSignature = 0x02014b50;
constexpr uint32_t EndSignature = 0x06054b50;
constexpr uint32_t Zip64LocatorSignature = 0x07064b50;
constexpr uint32_t Zip64EndSignature = 0x06064b50;

constexpr size_t EndRecordSize = 22;
constexpr size_t MaxCommentSize = 0xffff;
constexpr size_t CentralHeaderSize = 46;
constexpr size_t LocalHeaderSize = 30;
constexpr uint64_t MaxCentralDirectorySize = 16 * 1024 * 1024;
constexpr size_t MaxEntries = 100000;
constexpr size_t ChunkSize = 256 * 1024;

constexpr uint16_t FlagEncrypted = 1u << 0;
constexpr uint16_t MethodStored = 0;
constexpr uint16_t MethodDeflated = 8;

struct FileCloser {
    void operator()(FILE *file) const
    {
        if (file != nullptr) {
            std::fclose(file);
        }
    }
};
using File = std::unique_ptr<FILE, FileCloser>;

uint16_t read16(const unsigned char *data)
{
    return static_cast<uint16_t>(data[0] | (data[1] << 8));
}

uint32_t read32(const unsigned char *data)
{
    return static_cast<uint32_t>(data[0]) |
           (static_cast<uint32_t>(data[1]) << 8) |
           (static_cast<uint32_t>(data[2]) << 16) |
           (static_cast<uint32_t>(data[3]) << 24);
}

uint64_t read64(const unsigned char *data)
{
    return static_cast<uint64_t>(read32(data)) |
           (static_cast<uint64_t>(read32(data + 4)) << 32);
}

bool seek(FILE *file, uint64_t offset)
{
    if (offset > static_cast<uint64_t>(INT64_MAX) ||
        static_cast<uint64_t>(static_cast<off_t>(offset)) != offset) {
        return false;
    }
    return fseeko(file, static_cast<off_t>(offset), SEEK_SET) == 0;
}

bool readAt(FILE *file, uint64_t offset, unsigned char *buffer, size_t size)
{
    return seek(file, offset) && std::fread(buffer, 1, size, file) == size;
}

bool fileLength(FILE *file, uint64_t &length)
{
    if (fseeko(file, 0, SEEK_END) != 0) {
        return false;
    }
    const off_t end = ftello(file);
    if (end < 0) {
        return false;
    }
    length = static_cast<uint64_t>(end);
    return true;
}

struct Directory {
    uint64_t entries = 0;
    uint64_t size = 0;
    uint64_t offset = 0;
};

bool findDirectory(FILE *file, Directory &directory, std::string &error)
{
    uint64_t length = 0;
    if (!fileLength(file, length) || length < EndRecordSize) {
        error = "The file is too small to be a ZIP archive.";
        return false;
    }

    const uint64_t window = std::min<uint64_t>(length,
                                               EndRecordSize + MaxCommentSize);
    std::vector<unsigned char> tail(static_cast<size_t>(window));
    if (!readAt(file, length - window, tail.data(), tail.size())) {
        error = "Could not read the end of the archive.";
        return false;
    }

    size_t found = std::string::npos;
    for (size_t index = tail.size() - EndRecordSize + 1; index-- > 0;) {
        if (read32(&tail[index]) == EndSignature &&
            index + EndRecordSize + read16(&tail[index + 20]) <= tail.size()) {
            found = index;
            break;
        }
    }
    if (found == std::string::npos) {
        error = "This is not a ZIP archive, or it is incomplete.";
        return false;
    }

    const unsigned char *end = &tail[found];
    const uint64_t endOffset = length - window + found;
    directory.entries = read16(end + 10);
    directory.size = read32(end + 12);
    directory.offset = read32(end + 16);

    if (directory.entries == 0xffff || directory.size == 0xffffffff ||
        directory.offset == 0xffffffff) {
        unsigned char locator[20];
        if (endOffset < sizeof(locator) ||
            !readAt(file, endOffset - sizeof(locator), locator,
                    sizeof(locator)) ||
            read32(locator) != Zip64LocatorSignature) {
            error = "The archive's ZIP64 directory is missing.";
            return false;
        }
        unsigned char record[56];
        if (!readAt(file, read64(locator + 8), record, sizeof(record)) ||
            read32(record) != Zip64EndSignature) {
            error = "The archive's ZIP64 directory is damaged.";
            return false;
        }
        directory.entries = read64(record + 32);
        directory.size = read64(record + 40);
        directory.offset = read64(record + 48);
    }

    if (directory.entries > MaxEntries ||
        directory.size > MaxCentralDirectorySize ||
        directory.offset + directory.size > length) {
        error = "The archive's directory is damaged or too large.";
        return false;
    }
    return true;
}

// Applies the ZIP64 extended information extra field (id 0x0001), which holds
// 64-bit values for whichever 32-bit fields were set to 0xffffffff.
void applyZip64(Entry &entry, const unsigned char *extra, size_t length,
                bool sizeMissing, bool compressedMissing, bool offsetMissing)
{
    size_t position = 0;
    while (position + 4 <= length) {
        const uint16_t id = read16(extra + position);
        const uint16_t size = read16(extra + position + 2);
        const unsigned char *data = extra + position + 4;
        if (position + 4 + size > length) {
            return;
        }
        if (id == 0x0001) {
            size_t field = 0;
            if (sizeMissing && field + 8 <= size) {
                entry.size = read64(data + field);
                field += 8;
            }
            if (compressedMissing && field + 8 <= size) {
                entry.compressedSize = read64(data + field);
                field += 8;
            }
            if (offsetMissing && field + 8 <= size) {
                entry.localHeaderOffset = read64(data + field);
            }
            return;
        }
        position += 4 + size;
    }
}

// Reads the archive front to back through one buffer. Entries are taken in
// the order they are stored, so a small file's header and data usually come
// out of a read that has already been done.
class Reader {
public:
    explicit Reader(FILE *file)
        : file_(file), buffer_(new unsigned char[ChunkSize])
    {
    }

    void seek(uint64_t offset)
    {
        if (offset >= base_ && offset <= base_ + filled_) {
            cursor_ = static_cast<size_t>(offset - base_);
            return;
        }
        base_ = offset;
        filled_ = cursor_ = 0;
    }

    // Points data at up to max of the bytes that come next. Returns how
    // many, or 0 at the end of the file or on an error.
    size_t take(const unsigned char *&data, size_t max)
    {
        if (cursor_ == filled_ && !refill()) {
            return 0;
        }
        const size_t count = std::min(max, filled_ - cursor_);
        data = buffer_.get() + cursor_;
        cursor_ += count;
        return count;
    }

    bool read(unsigned char *out, size_t size)
    {
        while (size > 0) {
            const unsigned char *data = nullptr;
            const size_t count = take(data, size);
            if (count == 0) {
                return false;
            }
            std::memcpy(out, data, count);
            out += count;
            size -= count;
        }
        return true;
    }

    uint64_t position() const { return base_ + cursor_; }

private:
    bool refill()
    {
        base_ += filled_;
        filled_ = cursor_ = 0;
        if (filePosition_ != base_) {
            if (!zip::seek(file_, base_)) {
                return false;
            }
            filePosition_ = base_;
        }
        filled_ = std::fread(buffer_.get(), 1, ChunkSize, file_);
        filePosition_ += filled_;
        return filled_ > 0;
    }

    FILE *file_;
    std::unique_ptr<unsigned char[]> buffer_;
    uint64_t base_ = 0;    // where the buffer starts in the file
    size_t filled_ = 0;
    size_t cursor_ = 0;
    uint64_t filePosition_ = UINT64_MAX;
};

// Streams one entry's data into writer, checking size and CRC.
bool copyEntry(Reader &reader, const Entry &entry, archives::Writer &writer,
               unsigned char *inflated, archives::Progress &progress,
               archives::Observer &observer, bool &cancelled,
               std::string &error)
{
    unsigned char header[LocalHeaderSize];
    reader.seek(entry.localHeaderOffset);
    if (!reader.read(header, sizeof(header)) ||
        read32(header) != LocalHeaderSignature) {
        error = entry.path + " is damaged (bad local header).";
        return false;
    }
    reader.seek(entry.localHeaderOffset + LocalHeaderSize + read16(header + 26) +
                read16(header + 28));

    uint64_t remainingInput = entry.compressedSize;
    uint64_t produced = 0;
    uLong crc = crc32(0L, Z_NULL, 0);

    const auto emit = [&](const unsigned char *data, size_t size) {
        produced += size;
        if (produced > entry.size) {
            error = entry.path + " is larger than the archive says.";
            return false;
        }
        crc = crc32(crc, data, static_cast<uInt>(size));
        if (!writer.write(data, size)) {
            error = writer.error();
            return false;
        }
        progress.bytes += size;
        progress.archiveBytes = reader.position();
        observer.onProgress(progress);
        if (observer.cancelRequested()) {
            cancelled = true;
            return false;
        }
        return true;
    };

    if (entry.method == MethodStored) {
        if (entry.compressedSize != entry.size) {
            error = entry.path + " is damaged (size mismatch).";
            return false;
        }
        while (remainingInput > 0) {
            const unsigned char *data = nullptr;
            const size_t count = reader.take(
                data, static_cast<size_t>(
                          std::min<uint64_t>(remainingInput, ChunkSize)));
            if (count == 0) {
                error = entry.path + " is truncated.";
                return false;
            }
            remainingInput -= count;
            if (!emit(data, count)) {
                return false;
            }
        }
    } else {
        z_stream stream {};
        if (inflateInit2(&stream, -MAX_WBITS) != Z_OK) {
            error = "Not enough memory to decompress " + entry.path + ".";
            return false;
        }
        bool finished = false;
        bool ok = true;
        while (ok && !finished) {
            if (stream.avail_in == 0) {
                const unsigned char *data = nullptr;
                const size_t count =
                    remainingInput == 0
                        ? 0
                        : reader.take(data, static_cast<size_t>(std::min<uint64_t>(
                                                remainingInput, ChunkSize)));
                if (count == 0) {
                    error = entry.path + " is truncated.";
                    ok = false;
                    break;
                }
                remainingInput -= count;
                stream.next_in = const_cast<unsigned char *>(data);
                stream.avail_in = static_cast<uInt>(count);
            }

            stream.next_out = inflated;
            stream.avail_out = static_cast<uInt>(ChunkSize);
            const int status = inflate(&stream, Z_NO_FLUSH);
            if (status != Z_OK && status != Z_STREAM_END &&
                status != Z_BUF_ERROR) {
                error = entry.path + " is damaged (" +
                        (stream.msg != nullptr ? stream.msg : "inflate error") +
                        ").";
                ok = false;
                break;
            }
            const size_t size = ChunkSize - stream.avail_out;
            if (size > 0 && !emit(inflated, size)) {
                ok = false;
                break;
            }
            finished = status == Z_STREAM_END;
        }
        inflateEnd(&stream);
        if (!ok) {
            return false;
        }
    }

    if (produced != entry.size) {
        error = entry.path + " is smaller than the archive says.";
        return false;
    }
    if (static_cast<uint32_t>(crc) != entry.crc) {
        error = entry.path + " is damaged (checksum mismatch).";
        return false;
    }
    return true;
}

} // namespace

bool readArchive(const std::string &zipPath, Archive &archive,
                 std::string &error)
{
    archive = Archive();
    File file(std::fopen(zipPath.c_str(), "rb"));
    if (!file) {
        error = "Could not open the archive.";
        return false;
    }

    Directory directory;
    if (!findDirectory(file.get(), directory, error)) {
        return false;
    }

    std::vector<unsigned char> data(static_cast<size_t>(directory.size));
    if (!data.empty() &&
        !readAt(file.get(), directory.offset, data.data(), data.size())) {
        error = "Could not read the archive's directory.";
        return false;
    }

    size_t position = 0;
    for (uint64_t index = 0; index < directory.entries; ++index) {
        if (position + CentralHeaderSize > data.size() ||
            read32(&data[position]) != CentralHeaderSignature) {
            error = "The archive's directory is damaged.";
            return false;
        }
        const unsigned char *header = &data[position];
        const size_t nameLength = read16(header + 28);
        const size_t extraLength = read16(header + 30);
        const size_t commentLength = read16(header + 32);
        const size_t next = position + CentralHeaderSize + nameLength +
                            extraLength + commentLength;
        if (next > data.size()) {
            error = "The archive's directory is damaged.";
            return false;
        }

        Entry entry;
        entry.flags = read16(header + 8);
        entry.method = read16(header + 10);
        entry.crc = read32(header + 16);
        entry.compressedSize = read32(header + 20);
        entry.size = read32(header + 24);
        entry.localHeaderOffset = read32(header + 42);
        applyZip64(entry, header + CentralHeaderSize + nameLength, extraLength,
                   entry.size == 0xffffffff,
                   entry.compressedSize == 0xffffffff,
                   entry.localHeaderOffset == 0xffffffff);

        const std::string raw(
            reinterpret_cast<const char *>(header + CentralHeaderSize),
            nameLength);
        // Names without the UTF-8 flag are officially CP437, but most tools
        // write UTF-8 anyway, so valid UTF-8 is kept and anything else replaced.
        entry.name = text::sanitizeUtf8(raw);
        entry.directory = !raw.empty() &&
                          (raw.back() == '/' || raw.back() == '\\');
        entry.path = archives::safeRelativePath(entry.name);

        if (!entry.directory) {
            archive.totalSize += entry.size;
            ++archive.fileCount;
        }
        archive.entries.push_back(entry);
        position = next;
    }
    return true;
}

bool encrypted(const Entry &entry)
{
    return (entry.flags & FlagEncrypted) != 0;
}

bool readable(const Entry &entry)
{
    return !encrypted(entry) &&
           (entry.method == MethodStored || entry.method == MethodDeflated);
}

archives::Result extract(const std::string &zipPath, const Archive &archive,
                        const std::string &folder,
                        const archives::Options &options,
                        archives::Observer &observer)
{
    archives::Result result;
    File file(std::fopen(zipPath.c_str(), "rb"));
    if (!file) {
        result.message = "Could not open the archive.";
        return result;
    }
    // Reader keeps its own buffer, so stdio's would only copy twice.
    std::setvbuf(file.get(), nullptr, _IONBF, 0);

    archives::Writer writer(folder, options.existing);
    if (!writer.makeRoot()) {
        result.message = writer.error();
        return result;
    }
    const std::unordered_set<std::string> only(options.only.begin(),
                                               options.only.end());

    std::vector<const Entry *> order;
    order.reserve(archive.entries.size());
    for (const Entry &entry : archive.entries) {
        order.push_back(&entry);
    }
    std::stable_sort(order.begin(), order.end(),
                     [](const Entry *a, const Entry *b) {
                         return a->localHeaderOffset < b->localHeaderOffset;
                     });

    Reader reader(file.get());
    std::unique_ptr<unsigned char[]> inflated(new unsigned char[ChunkSize]);
    archives::Progress progress;
    for (const Entry *entry : order) {
        if (observer.cancelRequested()) {
            result.cancelled = true;
            result.message = "Extraction cancelled.";
            return result;
        }
        if (archives::macMetadata(entry->path)) {
            continue;
        }
        if (entry->path.empty()) {
            if (!entry->directory) {
                ++result.skippedUnsafe;
            }
            continue;
        }
        if (!only.empty() && only.count(entry->path) == 0) {
            if (options.place && !entry->directory) {
                ++result.skippedPlaced;
            }
            continue;
        }
        std::string moved;
        if (options.place) {
            moved = options.place(entry->path, entry->directory);
            if (moved.empty()) {
                if (!entry->directory) {
                    ++result.skippedPlaced;
                }
                continue;
            }
        }
        const std::string &written = options.place ? moved : entry->path;
        if (entry->directory) {
            if (!writer.makeFolder(written)) {
                result.message = writer.error();
                return result;
            }
            continue;
        }
        progress.current = written;
        if (!readable(*entry)) {
            ++result.skippedUnsupported;
            progress.bytes += entry->size;
            ++progress.files;
            continue;
        }

        const archives::Writer::Start start =
            writer.start(written, static_cast<int64_t>(std::min<uint64_t>(
                                     entry->size, INT64_MAX)));
        if (start == archives::Writer::Start::Failed) {
            result.message = writer.error();
            return result;
        }
        if (start == archives::Writer::Start::Skip) {
            result.skippedExisting.push_back(written);
            progress.bytes += entry->size;
            ++progress.files;
            observer.onProgress(progress);
            continue;
        }

        bool cancelled = false;
        std::string error;
        if (!copyEntry(reader, *entry, writer, inflated.get(), progress,
                       observer, cancelled, error) ||
            !writer.finish()) {
            writer.abandon();
            result.cancelled = cancelled;
            result.message = cancelled        ? "Extraction cancelled."
                             : !error.empty() ? error
                                              : writer.error();
            return result;
        }
        ++result.extracted;
        ++progress.files;
        observer.onProgress(progress);
    }

    result.ok = true;
    return result;
}

} // namespace zip
