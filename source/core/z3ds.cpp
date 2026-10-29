#include "z3ds.hpp"

#include <zstd.h>
#include <zstd_errors.h>

#include <algorithm>
#include <cstring>

namespace z3ds {

namespace {

constexpr uint8_t Version = 1;
// The zstd seekable format's frame table: a skippable frame whose content
// ends in a 9-byte footer.
constexpr uint32_t SkippableMagic = 0x184D2A5E;
constexpr uint32_t SeekableMagic = 0x8F92EAB1;
constexpr size_t FooterSize = 9;
constexpr uint8_t ChecksumFlag = 0x80;
constexpr uint8_t ReservedBits = 0x7C;
// Azahar's own files have a few dozen frames; this many takes 4 MiB.
constexpr uint32_t MaxFrames = 1u << 18;
// Its metadata is a few KiB: names, a date and an icon.
constexpr uint32_t MaxMetadata = 1024 * 1024;
// Azahar compresses at zstd's default level, which needs a 2 MiB window.
// Files made at the highest levels need up to 128 MiB, more than a 3DS has.
constexpr int MaxWindowLog = 25;
// Reads of the compressed file: small after a jump, as the CIA's header and
// title metadata need only its first few KiB, then large.
constexpr size_t FirstInputSize = 32 * 1024;
constexpr size_t InputSize = 256 * 1024;
constexpr size_t ScratchSize = 128 * 1024;

const char *const Damaged = "The compressed file is damaged.";
const char *const Incomplete =
    "The compressed file is incomplete. It may not have finished "
    "downloading.";

uint16_t le16(const uint8_t *data)
{
    return static_cast<uint16_t>(data[0] | data[1] << 8);
}

uint32_t le32(const uint8_t *data)
{
    return static_cast<uint32_t>(data[0]) |
           static_cast<uint32_t>(data[1]) << 8 |
           static_cast<uint32_t>(data[2]) << 16 |
           static_cast<uint32_t>(data[3]) << 24;
}

uint64_t le64(const uint8_t *data)
{
    return static_cast<uint64_t>(le32(data)) |
           static_cast<uint64_t>(le32(data + 4)) << 32;
}

std::string describe(size_t code)
{
    switch (ZSTD_getErrorCode(code)) {
    case ZSTD_error_memory_allocation:
    case ZSTD_error_frameParameter_windowTooLarge:
        return "There is not enough memory to decompress this file.";
    default:
        return Damaged;
    }
}

ZSTD_DCtx *newContext()
{
    ZSTD_DCtx *context = ZSTD_createDCtx();
    if (context != nullptr) {
        ZSTD_DCtx_setParameter(context, ZSTD_d_windowLogMax, MaxWindowLog);
    }
    return context;
}

} // namespace

bool parseHeader(const uint8_t *data, size_t size, Header &header)
{
    if (size < HeaderSize || std::memcmp(data, "Z3DS", 4) != 0 ||
        data[8] != Version) {
        return false;
    }
    const uint16_t headerSize = le16(data + 0x0a);
    if (headerSize < HeaderSize) {
        return false;
    }
    std::memcpy(header.underlying, data + 4, 4);
    header.metadataOffset = headerSize;
    header.metadataSize = le32(data + 0x0c);
    header.dataOffset = static_cast<uint64_t>(headerSize) + header.metadataSize;
    header.compressedSize = le64(data + 0x10);
    header.uncompressedSize = le64(data + 0x18);
    return true;
}

bool holdsCia(const Header &header)
{
    return std::memcmp(header.underlying, "CIA\0", 4) == 0;
}

std::vector<uint8_t> metadataItem(const std::vector<uint8_t> &metadata,
                                  const std::string &name)
{
    // A version byte, then items of a type, a name length and a data
    // length, ending with an item of type 0.
    constexpr uint8_t Binary = 1;
    if (metadata.empty() || metadata[0] != 1) {
        return {};
    }
    size_t at = 1;
    while (at + 4 <= metadata.size()) {
        const uint8_t type = metadata[at];
        const size_t nameSize = metadata[at + 1];
        const size_t dataSize = le16(&metadata[at + 2]);
        at += 4;
        if (type == 0 || at + nameSize + dataSize > metadata.size()) {
            break;
        }
        if (type == Binary && nameSize == name.size() &&
            std::memcmp(&metadata[at], name.data(), nameSize) == 0) {
            const auto first =
                metadata.begin() + static_cast<std::ptrdiff_t>(at + nameSize);
            return std::vector<uint8_t>(
                first, first + static_cast<std::ptrdiff_t>(dataSize));
        }
        at += nameSize + dataSize;
    }
    return {};
}

// --- Reader ------------------------------------------------------------------

Reader::Reader() = default;

Reader::~Reader()
{
    ZSTD_freeDCtx(context_);
}

bool Reader::open(const Source &source, uint64_t fileSize, std::string &error)
{
    source_ = source;
    uint8_t raw[HeaderSize];
    if (fileSize < HeaderSize || !source_(0, raw, sizeof(raw)) ||
        !parseHeader(raw, sizeof(raw), header_)) {
        error = "This is not a compressed 3DS file.";
        return false;
    }
    if (header_.dataOffset > fileSize ||
        header_.compressedSize > fileSize - header_.dataOffset) {
        error = Incomplete;
        return false;
    }
    const uint64_t end = header_.dataOffset + header_.compressedSize;
    if (header_.metadataSize > MaxMetadata ||
        header_.compressedSize < FooterSize + 8) {
        error = Damaged;
        return false;
    }
    metadata_.resize(header_.metadataSize);
    if (!metadata_.empty() &&
        !source_(header_.metadataOffset, metadata_.data(), metadata_.size())) {
        error = "The compressed file could not be read.";
        return false;
    }

    uint8_t footer[FooterSize];
    if (!source_(end - FooterSize, footer, sizeof(footer))) {
        error = "The compressed file could not be read.";
        return false;
    }
    const uint32_t frames = le32(footer);
    const uint8_t descriptor = footer[4];
    if (le32(footer + 5) != SeekableMagic || (descriptor & ReservedBits) != 0 ||
        frames == 0 || frames > MaxFrames) {
        error = Damaged;
        return false;
    }
    const size_t entrySize = (descriptor & ChecksumFlag) != 0 ? 12 : 8;
    const uint64_t tableSize = static_cast<uint64_t>(frames) * entrySize + FooterSize;
    uint8_t skippable[8];
    if (tableSize + sizeof(skippable) > header_.compressedSize ||
        !source_(end - tableSize - sizeof(skippable), skippable,
                 sizeof(skippable)) ||
        le32(skippable) != SkippableMagic || le32(skippable + 4) != tableSize) {
        error = Damaged;
        return false;
    }

    std::vector<uint8_t> table(static_cast<size_t>(tableSize - FooterSize));
    if (!source_(end - tableSize, table.data(), table.size())) {
        error = "The compressed file could not be read.";
        return false;
    }
    starts_.resize(frames);
    places_.resize(frames);
    uint64_t start = 0;
    uint64_t place = header_.dataOffset;
    for (uint32_t index = 0; index < frames; ++index) {
        const uint8_t *entry = &table[index * entrySize];
        starts_[index] = start;
        places_[index] = place;
        place += le32(entry);
        start += le32(entry + 4);
    }
    // The frames and the table fill the compressed part exactly, and the
    // frames add up to the original file.
    end_ = place;
    if (end_ + tableSize + sizeof(skippable) != end ||
        start != header_.uncompressedSize) {
        error = Damaged;
        return false;
    }

    context_ = newContext();
    input_.resize(InputSize);
    if (context_ == nullptr) {
        error = "There is not enough memory to decompress this file.";
        return false;
    }
    positioned_ = false;
    return true;
}

uint64_t Reader::frameEnd(size_t frame) const
{
    return frame + 1 < starts_.size() ? starts_[frame + 1]
                                      : header_.uncompressedSize;
}

void Reader::restart(size_t frame)
{
    ZSTD_DCtx_reset(context_, ZSTD_reset_session_only);
    frame_ = frame;
    position_ = starts_[frame];
    positioned_ = true;
    // The frame may start in what was read last, as when reading jumps
    // back to the start of the only frame.
    const uint64_t loaded = next_ - inputSize_;
    if (places_[frame] >= loaded && places_[frame] < next_) {
        inputPos_ = static_cast<size_t>(places_[frame] - loaded);
        return;
    }
    next_ = places_[frame];
    inputPos_ = 0;
    inputSize_ = 0;
    jumped_ = true;
}

bool Reader::decode(uint8_t *out, size_t room, size_t &produced,
                    std::string &error)
{
    ZSTD_outBuffer output = {out, room, 0};
    for (;;) {
        if (frame_ >= starts_.size()) {
            error = Damaged;
            return false;
        }
        ZSTD_inBuffer input = {input_.data(), inputSize_, inputPos_};
        const size_t before = output.pos;
        const size_t result = ZSTD_decompressStream(context_, &output, &input);
        inputPos_ = input.pos;
        if (ZSTD_isError(result)) {
            error = describe(result);
            return false;
        }
        position_ += output.pos - before;
        if (position_ > frameEnd(frame_)) {
            error = Damaged;
            return false;
        }
        if (result == 0) {
            // A frame ended, where the table says it does.
            if (position_ != frameEnd(frame_)) {
                error = Damaged;
                return false;
            }
            ++frame_;
        }
        if (output.pos > 0) {
            break;
        }
        if (inputPos_ < inputSize_) {
            continue;
        }
        if (next_ >= end_) {
            error = Damaged;
            return false;
        }
        const size_t wanted = static_cast<size_t>(std::min<uint64_t>(
            jumped_ ? FirstInputSize : input_.size(), end_ - next_));
        jumped_ = false;
        if (!source_(next_, input_.data(), wanted)) {
            error = "The compressed file could not be read.";
            return false;
        }
        next_ += wanted;
        inputPos_ = 0;
        inputSize_ = wanted;
    }
    produced = output.pos;
    return true;
}

bool Reader::read(uint64_t offset, void *buffer, size_t size,
                  std::string &error)
{
    if (context_ == nullptr || offset > header_.uncompressedSize ||
        size > header_.uncompressedSize - offset) {
        error = Damaged;
        return false;
    }
    uint8_t *out = static_cast<uint8_t *>(buffer);
    while (size > 0) {
        if (stop_ != nullptr && stop_->load()) {
            error = "Stopped.";
            return false;
        }
        const size_t frame = static_cast<size_t>(
            std::upper_bound(starts_.begin(), starts_.end(), offset) -
            starts_.begin() - 1);
        if (!positioned_ || offset < position_ || frame > frame_) {
            restart(frame);
        }
        const bool skipping = position_ < offset;
        if (skipping && scratch_.empty()) {
            scratch_.resize(ScratchSize);
        }
        uint8_t *target = skipping ? scratch_.data() : out;
        const size_t room = skipping
            ? static_cast<size_t>(std::min<uint64_t>(scratch_.size(),
                                                     offset - position_))
            : size;
        size_t produced = 0;
        if (!decode(target, room, produced, error)) {
            positioned_ = false;
            return false;
        }
        if (!skipping) {
            out += produced;
            offset += produced;
            size -= produced;
        }
    }
    return true;
}

// --- Stream ------------------------------------------------------------------

Stream::Stream() = default;

Stream::~Stream()
{
    ZSTD_freeDCtx(context_);
}

bool Stream::done() const
{
    return haveHeader_ && given_ == header_.uncompressedSize;
}

bool Stream::feed(const uint8_t *data, size_t size,
                  const std::function<bool(const uint8_t *, size_t)> &out,
                  std::string &error)
{
    while (size > 0) {
        if (!haveHeader_) {
            const size_t take = std::min(size, HeaderSize - head_.size());
            head_.insert(head_.end(), data, data + take);
            taken_ += take;
            data += take;
            size -= take;
            if (head_.size() < HeaderSize) {
                return true;
            }
            if (!parseHeader(head_.data(), head_.size(), header_)) {
                error = "This is not a compressed 3DS file.";
                return false;
            }
            haveHeader_ = true;
            context_ = newContext();
            output_.resize(ZSTD_DStreamOutSize());
            if (context_ == nullptr) {
                error = "There is not enough memory to decompress this file.";
                return false;
            }
            continue;
        }
        // The metadata is not needed, and nothing after the original file's
        // last byte is: that is the frame table.
        if (taken_ < header_.dataOffset) {
            const size_t skip = static_cast<size_t>(
                std::min<uint64_t>(size, header_.dataOffset - taken_));
            taken_ += skip;
            data += skip;
            size -= skip;
            continue;
        }
        if (done()) {
            taken_ += size;
            return true;
        }
        ZSTD_inBuffer input = {data, size, 0};
        for (;;) {
            ZSTD_outBuffer output = {output_.data(), output_.size(), 0};
            const size_t result = ZSTD_decompressStream(context_, &output, &input);
            if (ZSTD_isError(result)) {
                error = describe(result);
                return false;
            }
            if (output.pos > header_.uncompressedSize - given_) {
                error = Damaged;
                return false;
            }
            given_ += output.pos;
            if (output.pos > 0 && !out(output_.data(), output.pos)) {
                return false;
            }
            if (done() || (input.pos == input.size && output.pos < output.size)) {
                break;
            }
        }
        taken_ += size;
        return true;
    }
    return true;
}

} // namespace z3ds
