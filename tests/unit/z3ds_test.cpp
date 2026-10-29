#include "../check.hpp"
#include "../z3ds_support.hpp"

#include "z3ds.hpp"

#include <atomic>
#include <cstring>
#include <string>
#include <vector>

namespace {

// Text that compresses, with bytes that make every frame different.
std::string sample(size_t size)
{
    std::string data;
    uint32_t state = 12345;
    while (data.size() < size) {
        state = state * 1103515245u + 12345u;
        data += "frame " + std::to_string(data.size()) + " ";
        data += static_cast<char>(state >> 24);
    }
    data.resize(size);
    return data;
}

z3ds::Source memory(const std::string &file)
{
    return [&file](uint64_t offset, void *buffer, size_t size) {
        if (offset > file.size() || size > file.size() - offset) {
            return false;
        }
        std::memcpy(buffer, file.data() + offset, size);
        return true;
    };
}

std::string readAll(z3ds::Reader &reader, uint64_t offset, size_t size)
{
    std::string out(size, '\0');
    std::string error;
    if (!reader.read(offset, &out[0], size, error)) {
        return "failed: " + error;
    }
    return out;
}

} // namespace

TEST("z3ds: reads any part of a file in many frames")
{
    const std::string data = sample(10000);
    Z3dsOptions options;
    options.frameSize = 1000;
    const std::string file = makeZ3ds(data, options);

    z3ds::Reader reader;
    std::string error;
    CHECK(reader.open(memory(file), file.size(), error));
    CHECK_EQ(reader.size(), static_cast<uint64_t>(data.size()));
    CHECK(z3ds::holdsCia(reader.header()));

    CHECK(readAll(reader, 0, data.size()) == data);
    // In order, in pieces that do and do not line up with the frames.
    for (const size_t piece : {static_cast<size_t>(1), static_cast<size_t>(7),
                               static_cast<size_t>(333), static_cast<size_t>(1000),
                               static_cast<size_t>(4096)}) {
        std::string joined;
        for (size_t at = 0; at < data.size(); at += piece) {
            joined += readAll(reader, at, std::min(piece, data.size() - at));
        }
        CHECK(joined == data);
    }
    // Back and forth, across frame edges, and the very end.
    for (const size_t at : {9990, 0, 4321, 999, 1000, 5000, 1, 9999, 2500}) {
        const size_t size = std::min<size_t>(37, data.size() - at);
        CHECK(readAll(reader, at, size) == data.substr(at, size));
    }
    std::string nothing;
    CHECK(reader.read(data.size(), &nothing[0], 0, error));
    char byte = 0;
    CHECK(!reader.read(data.size(), &byte, 1, error));

    // A table with a checksum column reads the same.
    options.checksums = true;
    const std::string checked = makeZ3ds(data, options);
    z3ds::Reader second;
    CHECK(second.open(memory(checked), checked.size(), error));
    CHECK(readAll(second, 4500, 1000) == data.substr(4500, 1000));
}

TEST("z3ds: header and metadata")
{
    const std::string smdh(0x36c0, 'S');
    Z3dsOptions options;
    options.metadata = z3dsMetadata({{"titleinfo", std::string(24, 'T')},
                                     {"smdh", smdh},
                                     {"compressor", "Azahar"}});
    const std::string file = makeZ3ds(sample(100), options);

    z3ds::Header header;
    CHECK(z3ds::parseHeader(reinterpret_cast<const uint8_t *>(file.data()),
                            file.size(), header));
    CHECK_EQ(header.uncompressedSize, static_cast<uint64_t>(100));
    CHECK_EQ(header.metadataOffset, static_cast<uint32_t>(0x20));
    CHECK_EQ(header.metadataSize % 16, static_cast<uint32_t>(0));

    z3ds::Reader reader;
    std::string error;
    CHECK(reader.open(memory(file), file.size(), error));
    const std::vector<uint8_t> found = z3ds::metadataItem(reader.metadata(), "smdh");
    CHECK(std::string(found.begin(), found.end()) == smdh);
    CHECK(z3ds::metadataItem(reader.metadata(), "titleinfo").size() == 24);
    CHECK(z3ds::metadataItem(reader.metadata(), "missing").empty());
    CHECK(z3ds::metadataItem({}, "smdh").empty());
    // An item longer than the block is not read past the end.
    std::vector<uint8_t> cut = reader.metadata();
    cut.resize(40);
    CHECK(z3ds::metadataItem(cut, "smdh").empty());

    Z3dsOptions rom;
    rom.underlying = "NCSD";
    const std::string cartridge = makeZ3ds(sample(100), rom);
    CHECK(z3ds::parseHeader(reinterpret_cast<const uint8_t *>(cartridge.data()),
                            cartridge.size(), header));
    CHECK(!z3ds::holdsCia(header));

    const std::string plain = "NCSD and other things that are not Z3DS";
    CHECK(!z3ds::parseHeader(reinterpret_cast<const uint8_t *>(plain.data()),
                             plain.size(), header));
}

TEST("z3ds: refuses damaged and cut-short files")
{
    const std::string data = sample(5000);
    Z3dsOptions options;
    options.frameSize = 1000;
    const std::string file = makeZ3ds(data, options);
    std::string error;

    {
        z3ds::Reader reader;
        const std::string cut = file.substr(0, file.size() - 10);
        CHECK(!reader.open(memory(cut), cut.size(), error));
        CHECK(error.find("incomplete") != std::string::npos);
    }
    {
        // The table's magic number, at the very end.
        std::string bad = file;
        bad[bad.size() - 1] ^= 0x40;
        z3ds::Reader reader;
        CHECK(!reader.open(memory(bad), bad.size(), error));
        CHECK(error.find("damaged") != std::string::npos);
    }
    {
        // The first frame's original size no longer adds up.
        std::string bad = file;
        const size_t entry = bad.size() - 9 - 5 * 8;
        bad[entry + 4] = static_cast<char>(bad[entry + 4] + 1);
        z3ds::Reader reader;
        CHECK(!reader.open(memory(bad), bad.size(), error));
    }
    {
        // A frame's data scrambled: opening works, reading it does not.
        std::string bad = file;
        z3ds::Header header;
        z3ds::parseHeader(reinterpret_cast<const uint8_t *>(bad.data()),
                          bad.size(), header);
        for (size_t at = header.dataOffset + 6; at < header.dataOffset + 30; ++at) {
            bad[at] = static_cast<char>(~bad[at]);
        }
        z3ds::Reader reader;
        CHECK(reader.open(memory(bad), bad.size(), error));
        std::string out(100, '\0');
        CHECK(!reader.read(0, &out[0], out.size(), error));
        // The other frames still read.
        CHECK(readAll(reader, 3000, 100) == data.substr(3000, 100));
    }
    {
        z3ds::Reader reader;
        CHECK(!reader.open(memory(data), data.size(), error));
        CHECK(error.find("not a compressed") != std::string::npos);
    }
}

TEST("z3ds: jumping back in a frame reads nothing again")
{
    const std::string data = sample(10000);
    Z3dsOptions options;
    options.frameSize = data.size();
    const std::string file = makeZ3ds(data, options);
    size_t reads = 0;
    const z3ds::Source counted = [&](uint64_t offset, void *buffer, size_t size) {
        ++reads;
        return memory(file)(offset, buffer, size);
    };
    z3ds::Reader reader;
    std::string error;
    CHECK(reader.open(counted, file.size(), error));
    const size_t opened = reads;
    CHECK(readAll(reader, 5000, 10) == data.substr(5000, 10));
    CHECK_EQ(reads, opened + 1);
    CHECK(readAll(reader, 0, 10) == data.substr(0, 10));
    CHECK(readAll(reader, 9990, 10) == data.substr(9990, 10));
    CHECK(readAll(reader, 100, 10) == data.substr(100, 10));
    CHECK_EQ(reads, opened + 1);
}

TEST("z3ds: a read stops when asked")
{
    const std::string data = sample(5000);
    const std::string file = makeZ3ds(data);
    std::atomic<bool> stop {false};
    z3ds::Reader reader;
    reader.stopWhen(&stop);
    std::string error;
    CHECK(reader.open(memory(file), file.size(), error));
    CHECK(readAll(reader, 0, 10) == data.substr(0, 10));
    stop = true;
    std::string out(10, '\0');
    CHECK(!reader.read(4000, &out[0], out.size(), error));
    CHECK_EQ(error, std::string("Stopped."));
}

TEST("z3ds: decompresses a file handed over in order")
{
    const std::string data = sample(20000);
    Z3dsOptions options;
    options.frameSize = 3000;
    options.metadata = z3dsMetadata({{"compressor", "Azahar"}});
    const std::string file = makeZ3ds(data, options);

    for (const size_t piece : {static_cast<size_t>(1), static_cast<size_t>(5),
                               static_cast<size_t>(4096), file.size()}) {
        z3ds::Stream stream;
        std::string out;
        std::string error;
        bool ok = true;
        for (size_t at = 0; at < file.size() && ok; at += piece) {
            ok = stream.feed(reinterpret_cast<const uint8_t *>(file.data()) + at,
                             std::min(piece, file.size() - at),
                             [&out](const uint8_t *bytes, size_t size) {
                                 out.append(reinterpret_cast<const char *>(bytes),
                                            size);
                                 return true;
                             },
                             error);
        }
        CHECK(ok);
        CHECK(stream.done());
        CHECK(out == data);
    }

    // Cut short, it is not done.
    z3ds::Stream partial;
    std::string error;
    size_t given = 0;
    CHECK(partial.feed(reinterpret_cast<const uint8_t *>(file.data()),
                       file.size() / 2,
                       [&given](const uint8_t *, size_t size) {
                           given += size;
                           return true;
                       },
                       error));
    CHECK(!partial.done());
    CHECK(given < data.size());

    // Not a Z3DS file, and a receiver that stops the stream.
    z3ds::Stream junk;
    const auto ignore = [](const uint8_t *, size_t) { return true; };
    CHECK(!junk.feed(reinterpret_cast<const uint8_t *>(data.data()), 100,
                     ignore, error));
    z3ds::Stream refused;
    error.clear();
    CHECK(!refused.feed(reinterpret_cast<const uint8_t *>(file.data()),
                        file.size(),
                        [](const uint8_t *, size_t) { return false; }, error));
    CHECK(error.empty());
}
