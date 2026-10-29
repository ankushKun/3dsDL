#include "../check.hpp"
#include "../support.hpp"
#include "../z3ds_support.hpp"

#include "cia.hpp"
#include "fs_util.hpp"
#include "image.hpp"
#include "smdh.hpp"

#include <algorithm>
#include <cstring>
#include <string>
#include <vector>

namespace {

constexpr uint64_t TestTitleId = 0x0004000000164800ULL;
constexpr uint16_t TestVersion = 0x0420; // 1.2.0

void putLe32(std::string &data, size_t offset, uint32_t value)
{
    for (int index = 0; index < 4; ++index) {
        data[offset + index] = static_cast<char>(value >> (8 * index));
    }
}

void putBe(std::string &data, size_t offset, uint64_t value, int bytes)
{
    for (int index = 0; index < bytes; ++index) {
        data[offset + index] =
            static_cast<char>(value >> (8 * (bytes - 1 - index)));
    }
}

void putUtf16(std::string &data, size_t offset, const std::string &ascii)
{
    for (size_t index = 0; index < ascii.size(); ++index) {
        data[offset + index * 2] = ascii[index];
        data[offset + index * 2 + 1] = 0;
    }
}

size_t align64(size_t value)
{
    return (value + 63) & ~static_cast<size_t>(63);
}

struct Options {
    bool withMeta = true;
    bool encrypted = false;
    uint8_t ncchFlags = 0x20; // uses a seed
};

// A CIA with the sections 3dsDL reads: an RSA-2048 signed TMD with one
// content, an NCCH header as that content and a meta section with an SMDH.
std::string makeCia(const Options &options = Options())
{
    const size_t tmdSize = 0x140 + 0xc4 + 0x900 + 0x30;
    const size_t contentSize = 0x200;
    const size_t metaSize = options.withMeta ? 0x400 + smdh::Size : 0;
    const size_t ticket = align64(0x2020 + 0xa00);
    const size_t tmd = align64(ticket + 0x350);
    const size_t content = align64(tmd + tmdSize);
    const size_t meta = align64(content + contentSize);

    std::string data(meta + metaSize, '\0');
    putLe32(data, 0x00, 0x2020);
    putLe32(data, 0x08, 0xa00);
    putLe32(data, 0x0c, 0x350);
    putLe32(data, 0x10, static_cast<uint32_t>(tmdSize));
    putLe32(data, 0x14, static_cast<uint32_t>(metaSize));
    putLe32(data, 0x18, static_cast<uint32_t>(contentSize));

    putBe(data, tmd, 0x10004, 4);
    const size_t header = tmd + 0x140;
    putBe(data, header + 0x4c, TestTitleId, 8);
    putBe(data, header + 0x9c, TestVersion, 2);
    putBe(data, header + 0x9e, 1, 2);
    putBe(data, header + 0xc4 + 0x900 + 6, options.encrypted ? 1 : 0, 2);

    std::memcpy(&data[content + 0x100], "NCCH", 4);
    data[content + 0x18f] = static_cast<char>(options.ncchFlags);

    if (options.withMeta) {
        const size_t block = meta + 0x400;
        std::memcpy(&data[block], "SMDH", 4);
        const size_t english = block + 0x8 + 0x200;
        putUtf16(data, english, "Test Game");
        putUtf16(data, english + 0x80, "Test Game: Long Name");
        putUtf16(data, english + 0x180, "3dsDL Tests");
        // One red pixel at (5, 9) of the large icon.
        const size_t pixel = block + 0x24c0 + image::tiledIndex(5, 9, 48) * 2;
        data[pixel] = 0x00;
        data[pixel + 1] = static_cast<char>(0xf8);
    }
    return data;
}

} // namespace

TEST("cia: reads the title ID, version, seed flag, name and icon")
{
    TempDir temp;
    const std::string path = temp.path() + "game.cia";
    CHECK(writeText(path, makeCia()));
    CHECK(cia::isCiaFile(path));

    cia::Info info;
    std::string error;
    CHECK(cia::readInfo(path, info, error));
    CHECK_EQ(info.titleId, TestTitleId);
    CHECK_EQ(info.version, TestVersion);
    CHECK(info.seed == cia::Seed::Needed);
    CHECK(info.hasSmdh);
    CHECK_EQ(info.smdh.shortName, std::string("Test Game"));
    CHECK_EQ(info.smdh.longName, std::string("Test Game: Long Name"));
    CHECK_EQ(info.smdh.publisher, std::string("3dsDL Tests"));
    CHECK_EQ(info.smdh.icon.size(), static_cast<size_t>(48 * 48));
    if (info.smdh.icon.size() == 48 * 48) {
        CHECK_EQ(info.smdh.icon[9 * 48 + 5], static_cast<uint16_t>(0xf800));
        CHECK_EQ(info.smdh.icon[0], static_cast<uint16_t>(0));
    }
    CHECK_EQ(info.fileSize, static_cast<uint64_t>(fs::fileSize(path)));
}

TEST("cia: reads a CIA as it streams in")
{
    const std::string data = makeCia();
    for (const size_t piece : {static_cast<size_t>(1), static_cast<size_t>(7),
                               static_cast<size_t>(4096), data.size()}) {
        cia::Stream stream;
        std::string error;
        std::vector<uint8_t> prefix;
        bool ok = true;
        for (size_t offset = 0; offset < data.size() && ok; offset += piece) {
            const size_t size = std::min(piece, data.size() - offset);
            const bool wasReady = stream.ready();
            ok = stream.feed(offset,
                             reinterpret_cast<const uint8_t *>(data.data()) + offset,
                             size, error);
            if (!wasReady && stream.ready()) {
                prefix = stream.takePrefix();
                // Everything fed so far, so the installer can start with it.
                CHECK_EQ(prefix.size(), offset + size);
                CHECK(std::memcmp(prefix.data(), data.data(), prefix.size()) == 0);
                CHECK_EQ(stream.info().titleId, TestTitleId);
                // The icon is at the end, so it only arrives later, unless
                // this piece held the whole file.
                CHECK(stream.info().hasSmdh == (offset + size == data.size()));
            }
        }
        CHECK(ok);
        CHECK(stream.ready());
        CHECK_EQ(stream.info().version, TestVersion);
        CHECK(stream.info().seed == cia::Seed::Needed);
        CHECK(stream.info().hasSmdh);
        CHECK_EQ(stream.info().smdh.shortName, std::string("Test Game"));
        CHECK(stream.takePrefix().empty());
    }

    cia::Stream outOfOrder;
    std::string error;
    const uint8_t *bytes = reinterpret_cast<const uint8_t *>(data.data());
    CHECK(outOfOrder.feed(0, bytes, 100, error));
    CHECK(!outOfOrder.feed(200, bytes + 200, 100, error));
    CHECK(error.find("order") != std::string::npos);

    cia::Stream notCia;
    const std::string junk(0x3000, 'x');
    CHECK(!notCia.feed(0, reinterpret_cast<const uint8_t *>(junk.data()),
                       junk.size(), error));
    CHECK(!notCia.ready());
}

TEST("cia: seed and meta are optional")
{
    TempDir temp;
    cia::Info info;
    std::string error;

    Options plain;
    plain.withMeta = false;
    plain.ncchFlags = 0x04; // NoCrypto
    CHECK(writeText(temp.path() + "a.cia", makeCia(plain)));
    CHECK(cia::readInfo(temp.path() + "a.cia", info, error));
    CHECK(!info.hasSmdh);
    CHECK(info.seed == cia::Seed::NotNeeded);
    CHECK_EQ(info.titleId, TestTitleId);

    Options encrypted;
    encrypted.encrypted = true;
    CHECK(writeText(temp.path() + "b.cia", makeCia(encrypted)));
    CHECK(cia::readInfo(temp.path() + "b.cia", info, error));
    CHECK(info.seed == cia::Seed::Unknown);
}

TEST("cia: rejects files that are not complete CIAs")
{
    TempDir temp;
    cia::Info info;
    std::string error;

    CHECK(writeText(temp.path() + "text.cia", "hello, this is not a CIA"));
    CHECK(!cia::isCiaFile(temp.path() + "text.cia"));
    CHECK(!cia::readInfo(temp.path() + "text.cia", info, error));
    CHECK_EQ(error, std::string("This is not a CIA file."));

    const std::string whole = makeCia();
    CHECK(writeText(temp.path() + "cut.cia", whole.substr(0, 0x3000)));
    CHECK(cia::isCiaFile(temp.path() + "cut.cia"));
    CHECK(!cia::readInfo(temp.path() + "cut.cia", info, error));

    CHECK(!cia::readInfo(temp.path() + "missing.cia", info, error));
}

TEST("cia: reads a CIA compressed as a .zcia")
{
    TempDir temp;
    const std::string data = makeCia();
    // The SMDH Azahar copies into the metadata, here with another name, to
    // tell which copy was read.
    std::string smdhCopy = data.substr(data.size() - smdh::Size);
    putUtf16(smdhCopy, 0x8 + 0x200, "Up Front!");
    Z3dsOptions options;
    options.frameSize = 0x1000;
    options.metadata = z3dsMetadata({{"titleinfo", std::string(24, '\0')},
                                     {"smdh", smdhCopy}});
    const std::string path = temp.path() + "game.zcia";
    CHECK(writeText(path, makeZ3ds(data, options)));
    CHECK(cia::isCiaFile(path));

    for (const bool quick : {false, true}) {
        cia::Info info;
        std::string error;
        CHECK(cia::readInfo(path, info, error, quick));
        CHECK(info.compressed);
        CHECK_EQ(info.titleId, TestTitleId);
        CHECK_EQ(info.version, TestVersion);
        CHECK(info.seed == cia::Seed::Needed);
        CHECK_EQ(info.fileSize, static_cast<uint64_t>(data.size()));
        CHECK_EQ(info.contentSize, static_cast<uint64_t>(0x200));
        CHECK(info.hasSmdh);
        CHECK(!info.smdhLeft);
        CHECK_EQ(info.smdh.shortName, std::string("Up Front!"));
        CHECK_EQ(info.smdh.publisher, std::string("3dsDL Tests"));
    }

    // Without the copy, the name and icon come from the CIA's end, which a
    // quick read leaves for later.
    options.metadata = z3dsMetadata({{"compressor", "something else"}});
    const std::string other = temp.path() + "other.zcia";
    CHECK(writeText(other, makeZ3ds(data, options)));
    cia::Info info;
    std::string error;
    CHECK(cia::readInfo(other, info, error, true));
    CHECK(info.compressed);
    CHECK_EQ(info.titleId, TestTitleId);
    CHECK(!info.hasSmdh);
    CHECK(info.smdhLeft);
    CHECK(cia::readInfo(other, info, error));
    CHECK(info.hasSmdh);
    CHECK(!info.smdhLeft);
    CHECK_EQ(info.smdh.shortName, std::string("Test Game"));

    std::atomic<bool> stop {true};
    CHECK(!cia::readInfo(other, info, error, false, &stop));
    CHECK_EQ(error, std::string("Stopped."));
}

TEST("cia: a .zcia must hold a whole CIA")
{
    TempDir temp;
    cia::Info info;
    std::string error;

    Z3dsOptions rom;
    rom.underlying = "NCSD";
    CHECK(writeText(temp.path() + "game.zcci", makeZ3ds(makeCia(), rom)));
    CHECK(!cia::isCiaFile(temp.path() + "game.zcci"));
    CHECK(!cia::readInfo(temp.path() + "game.zcci", info, error));
    CHECK_EQ(error, std::string("This compressed file does not hold a CIA."));

    const std::string whole = makeZ3ds(makeCia());
    CHECK(writeText(temp.path() + "cut.zcia", whole.substr(0, whole.size() / 2)));
    CHECK(cia::isCiaFile(temp.path() + "cut.zcia"));
    CHECK(!cia::readInfo(temp.path() + "cut.zcia", info, error));
    CHECK(error.find("incomplete") != std::string::npos);

    // A CIA that was cut short before it was compressed.
    CHECK(writeText(temp.path() + "short.zcia",
                    makeZ3ds(makeCia().substr(0, 0x3000))));
    CHECK(!cia::readInfo(temp.path() + "short.zcia", info, error));

    // Plain CIAs say nothing about compression.
    CHECK(writeText(temp.path() + "plain.cia", makeCia()));
    CHECK(cia::readInfo(temp.path() + "plain.cia", info, error, true));
    CHECK(!info.compressed);
    CHECK(info.hasSmdh);
}

TEST("cia: title ID helpers")
{
    CHECK_EQ(cia::titleIdText(TestTitleId), std::string("0004000000164800"));
    CHECK_EQ(cia::versionText(TestVersion), std::string("1.2.0 (v1056)"));
    CHECK_EQ(cia::versionText(0), std::string("0.0.0 (v0)"));

    // 3dsDL's own build numbers, the same as scripts/version.sh gives.
    CHECK_EQ(cia::buildNumber("0.8.1-beta.1"), 2065);
    CHECK_EQ(cia::buildNumber("0.8.1-beta.2"), 2066);
    CHECK_EQ(cia::buildNumber("0.8.1-rc.3"), 2067);
    CHECK_EQ(cia::buildNumber("0.8.1"), 2079);
    CHECK_EQ(cia::buildNumber("1.0.0"), 16399);
    CHECK_EQ(cia::buildNumber("0.8.1-beta"), -1);
    CHECK_EQ(cia::buildNumber("0.8.1-beta.15"), -1);
    CHECK_EQ(cia::buildNumber("0.8.1-beta.1a"), -1);
    CHECK_EQ(cia::buildNumber("0.7.16"), -1);
    CHECK_EQ(cia::buildNumber("nonsense"), -1);
    CHECK_EQ(cia::versionText(cia::OwnTitleId, 2066, "0.8.1-beta.2"),
             std::string("0.8.1-beta.2 (v2066)"));
    CHECK_EQ(cia::versionText(cia::OwnTitleId, 2066, "0.8.1-beta.3"),
             std::string("0.8.1-pre.2 (v2066)"));
    CHECK_EQ(cia::versionText(cia::OwnTitleId, 2079), std::string("0.8.1 (v2079)"));
    // 0.8.0 and older used major.minor.micro, and other titles always do.
    CHECK_EQ(cia::versionText(cia::OwnTitleId, 128), std::string("0.8.0 (v128)"));
    CHECK_EQ(cia::versionText(0x0004000000055D00ull, 2066), std::string("2.1.2 (v2066)"));

    CHECK(cia::kind(0x0004000000164800ULL) == cia::Kind::Application);
    CHECK(cia::kind(0x0004000E00164800ULL) == cia::Kind::Update);
    CHECK(cia::kind(0x0004008C00164800ULL) == cia::Kind::Dlc);
    CHECK(cia::kind(0x0004800442383841ULL) == cia::Kind::DsiWare);
    CHECK(cia::kind(0x0004013000002802ULL) == cia::Kind::System);
    CHECK(cia::isSystemTitle(0x0004001000021000ULL));
    CHECK(!cia::isSystemTitle(TestTitleId));

    CHECK(!cia::installsToNand(TestTitleId));
    CHECK(cia::installsToNand(0x0004800442383841ULL));
    CHECK(cia::installsToNand(0x0004000100164800ULL));
    CHECK(cia::installsToNand(0x0004013000002802ULL));

    CHECK(cia::isNew3dsOnly(0x0004000020000000ULL));
    CHECK(!cia::isNew3dsOnly(TestTitleId));

    CHECK_EQ(cia::baseTitle(0x0004000E00164800ULL), TestTitleId);
    CHECK_EQ(cia::baseTitle(0x0004008C00164800ULL), TestTitleId);
    CHECK_EQ(cia::baseTitle(TestTitleId), TestTitleId);
}

TEST("smdh: UTF-16 names with surrogates and line breaks")
{
    const uint8_t text[] = {'A', 0, '\n', 0, 'B', 0, 0x3d, 0xd8, 0x00, 0xde,
                            0, 0, 'Z', 0};
    CHECK_EQ(smdh::utf16ToUtf8(text, 7), std::string("A B\xf0\x9f\x98\x80"));
    CHECK_EQ(smdh::utf16ToUtf8(text, 1), std::string("A"));
}

TEST("fs: entries list folders first, then files with sizes")
{
    TempDir temp;
    CHECK(fs::makeDirectories(temp.path() + "zeta/"));
    CHECK(fs::makeDirectories(temp.path() + ".hidden/"));
    CHECK(writeText(temp.path() + "b.cia", "12345"));
    CHECK(writeText(temp.path() + "A.txt", "1"));
    bool ok = false;
    const std::vector<fs::Entry> entries = fs::listEntries(temp.path(), &ok);
    CHECK(ok);
    CHECK_EQ(entries.size(), static_cast<size_t>(3));
    if (entries.size() == 3) {
        CHECK_EQ(entries[0].name, std::string("zeta"));
        CHECK(entries[0].folder);
        CHECK_EQ(entries[1].name, std::string("A.txt"));
        CHECK_EQ(entries[2].name, std::string("b.cia"));
        CHECK_EQ(entries[2].size, static_cast<int64_t>(5));
    }
    fs::listEntries(temp.path() + "missing/", &ok);
    CHECK(!ok);
}
