#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

struct ZSTD_DCtx_s;

// Azahar's compressed 3DS files (.zcia, .zcci, .zcxi, .z3dsx): a 32-byte
// header, a block of named metadata, then the original file as seekable
// zstd, a run of frames compressed on their own with a table of their sizes
// at the end. 3dsDL reads compressed CIAs through this, so they install like
// plain ones.
namespace z3ds {

constexpr size_t HeaderSize = 0x20;

struct Header {
    char underlying[4] = {}; // the original's kind: "CIA\0", "NCSD", "3DSX"
    uint32_t metadataOffset = 0;
    uint32_t metadataSize = 0;
    uint64_t dataOffset = 0;     // where the frames start
    uint64_t compressedSize = 0; // the frames and their table
    uint64_t uncompressedSize = 0;
};

// True when data starts with a Z3DS header 3dsDL can read.
bool parseHeader(const uint8_t *data, size_t size, Header &header);
// True when the original file is a CIA.
bool holdsCia(const Header &header);
// The metadata item called name, as Azahar adds "smdh" and "titleinfo" to
// the CIAs it compresses. Empty when there is none.
std::vector<uint8_t> metadataItem(const std::vector<uint8_t> &metadata,
                                  const std::string &name);

// Reads size bytes at offset of the compressed file into buffer. False when
// it cannot.
using Source = std::function<bool(uint64_t offset, void *buffer, size_t size)>;

// The original file inside a Z3DS file, read at any offset. Reading on from
// where the last read ended keeps decompressing; anywhere else starts again
// at the frame that holds it.
class Reader {
public:
    Reader();
    ~Reader();
    Reader(const Reader &) = delete;
    Reader &operator=(const Reader &) = delete;

    // Reads the header, the metadata and the frame table. fileSize is the
    // compressed file's size.
    bool open(const Source &source, uint64_t fileSize, std::string &error);
    const Header &header() const { return header_; }
    const std::vector<uint8_t> &metadata() const { return metadata_; }
    // The original file's size.
    uint64_t size() const { return header_.uncompressedSize; }
    bool read(uint64_t offset, void *buffer, size_t size, std::string &error);
    // Once *stop is set, reads fail with "Stopped.".
    void stopWhen(const std::atomic<bool> *stop) { stop_ = stop; }

private:
    void restart(size_t frame);
    bool decode(uint8_t *out, size_t room, size_t &produced,
                std::string &error);
    uint64_t frameEnd(size_t frame) const;

    Source source_;
    Header header_;
    std::vector<uint8_t> metadata_;
    std::vector<uint64_t> starts_; // where each frame starts in the original
    std::vector<uint64_t> places_; // and in the compressed file
    uint64_t end_ = 0;             // where the last frame ends
    ZSTD_DCtx_s *context_ = nullptr;
    std::vector<uint8_t> input_;
    size_t inputPos_ = 0;
    size_t inputSize_ = 0;
    uint64_t next_ = 0;     // the next compressed byte to load
    bool jumped_ = false;   // nothing loaded since a jump
    uint64_t position_ = 0; // the next original byte the decoder gives
    size_t frame_ = 0;      // the frame being decoded
    bool positioned_ = false;
    std::vector<uint8_t> scratch_;
    const std::atomic<bool> *stop_ = nullptr;
};

// Decompresses a Z3DS file handed over in order from its first byte, for
// installing one as it downloads.
class Stream {
public:
    Stream();
    ~Stream();
    Stream(const Stream &) = delete;
    Stream &operator=(const Stream &) = delete;

    // Takes the next bytes of the compressed file and passes on what they
    // decompress to, in order. When out returns false, so does feed, with
    // error left as it was.
    bool feed(const uint8_t *data, size_t size,
              const std::function<bool(const uint8_t *, size_t)> &out,
              std::string &error);
    // The whole original file has come out.
    bool done() const;

private:
    std::vector<uint8_t> head_; // the header, until it is whole
    bool haveHeader_ = false;
    Header header_;
    uint64_t taken_ = 0; // compressed bytes fed
    uint64_t given_ = 0; // original bytes passed on
    ZSTD_DCtx_s *context_ = nullptr;
    std::vector<uint8_t> output_;
};

} // namespace z3ds
