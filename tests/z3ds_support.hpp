#pragma once

#include <zstd.h>

#include <algorithm>
#include <cstdint>
#include <initializer_list>
#include <string>
#include <utility>

// Builds files the way Azahar compresses one: a Z3DS header, metadata, then
// seekable zstd, the data in frames of frameSize bytes followed by a table
// of their sizes.
struct Z3dsOptions {
    size_t frameSize = 64;
    std::string underlying = std::string("CIA\0", 4);
    std::string metadata;   // from z3dsMetadata, or nothing
    bool checksums = false; // a checksum for each frame in the table
};

inline void appendLe(std::string &out, uint64_t value, int bytes)
{
    for (int index = 0; index < bytes; ++index) {
        out += static_cast<char>(value >> (8 * index));
    }
}

// Azahar's metadata block: a version byte, then for each item its type (1),
// name length and data length, the name and the data, then an end item.
inline std::string
z3dsMetadata(std::initializer_list<std::pair<std::string, std::string>> items)
{
    std::string out(1, '\x01');
    for (const auto &item : items) {
        out += '\x01';
        appendLe(out, item.first.size(), 1);
        appendLe(out, item.second.size(), 2);
        out += item.first + item.second;
    }
    out += std::string(4, '\0');
    return out;
}

inline std::string makeZ3ds(const std::string &data,
                            const Z3dsOptions &options = Z3dsOptions())
{
    std::string frames;
    std::string table;
    uint32_t count = 0;
    for (size_t at = 0; at < data.size(); at += options.frameSize) {
        const size_t size = std::min(options.frameSize, data.size() - at);
        std::string frame(ZSTD_compressBound(size), '\0');
        const size_t written =
            ZSTD_compress(&frame[0], frame.size(), data.data() + at, size, 3);
        frame.resize(ZSTD_isError(written) ? 0 : written);
        frames += frame;
        appendLe(table, frame.size(), 4);
        appendLe(table, size, 4);
        if (options.checksums) {
            appendLe(table, 0x12345678, 4);
        }
        ++count;
    }
    appendLe(table, count, 4);
    table += static_cast<char>(options.checksums ? 0x80 : 0);
    appendLe(table, 0x8F92EAB1, 4);
    std::string seekable = frames;
    appendLe(seekable, 0x184D2A5E, 4);
    appendLe(seekable, table.size(), 4);
    seekable += table;

    std::string metadata = options.metadata;
    metadata.resize((metadata.size() + 15) & ~static_cast<size_t>(15), '\0');
    std::string out = "Z3DS" + options.underlying;
    out += '\x01'; // version
    out += '\0';
    appendLe(out, 0x20, 2);
    appendLe(out, metadata.size(), 4);
    appendLe(out, seekable.size(), 8);
    appendLe(out, data.size(), 8);
    return out + metadata + seekable;
}
