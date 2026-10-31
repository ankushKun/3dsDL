#include "../check.hpp"
#include "../support.hpp"

#include "file_ops.hpp"
#include "file_types.hpp"
#include "fs_util.hpp"
#include "image.hpp"
#include "picture.hpp"
#include "records.hpp"
#include "remote.hpp"
#include "sha256.hpp"
#include "smdh.hpp"
#include "text_doc.hpp"
#include "title_meta.hpp"

#include <atomic>
#include <cstring>
#include <string>
#include <vector>

namespace {

class Quiet : public fileops::Observer {
public:
    bool cancelRequested() override { return cancelAfter >= 0 && calls++ >= cancelAfter; }
    void onProgress(uint64_t done, uint64_t total,
                    const std::string &) override
    {
        lastDone = done;
        lastTotal = total;
    }
    int cancelAfter = -1;
    int calls = 0;
    uint64_t lastDone = 0;
    uint64_t lastTotal = 0;
};

void putLe32(std::string &data, size_t offset, uint32_t value)
{
    for (int index = 0; index < 4; ++index) {
        data[offset + index] = static_cast<char>(value >> (8 * index));
    }
}

void putUtf16(std::string &data, size_t offset, const std::string &ascii)
{
    for (size_t index = 0; index < ascii.size(); ++index) {
        data[offset + index * 2] = ascii[index];
        data[offset + index * 2 + 1] = 0;
    }
}

std::string makeSmdh(const std::string &name, const std::string &publisher)
{
    std::string block(smdh::Size, '\0');
    std::memcpy(&block[0], "SMDH", 4);
    const size_t english = 0x8 + 0x200;
    putUtf16(block, english, name);
    putUtf16(block, english + 0x180, publisher);
    block[0x24c0] = 0x1f; // one blue pixel at (0, 0)
    return block;
}

} // namespace

TEST("filetype: classifies by extension")
{
    using filetype::Kind;
    CHECK(filetype::fromName("Game.CIA") == Kind::Cia);
    CHECK(filetype::fromName("Game.zcia") == Kind::Cia);
    CHECK(filetype::fromName("app.3dsx") == Kind::Homebrew);
    CHECK(filetype::fromName("rom.3ds") == Kind::CartridgeImage);
    CHECK(filetype::fromName("rom.nds") == Kind::Nds);
    CHECK(filetype::fromName("photo.JPEG") == Kind::Image);
    CHECK(filetype::fromName("config.ini") == Kind::Text);
    CHECK(filetype::fromName("README.md") == Kind::Text);
    CHECK(filetype::fromName("boot.firm") == Kind::Firm);
    CHECK(filetype::fromName("song.mp3") == Kind::Audio);
    CHECK(filetype::fromName("pack.zip") == Kind::Archive);
    CHECK(filetype::fromName("pack.7z") == Kind::Archive);
    CHECK(filetype::fromName("Pack.RAR") == Kind::Archive);
    CHECK(filetype::fromName("pack.tar.gz") == Kind::Archive);
    CHECK(filetype::fromName("pack.tar.XZ") == Kind::Archive);
    CHECK(filetype::fromName("pack.tgz") == Kind::Archive);
    CHECK(filetype::fromName("notes.gz") == Kind::Other);
    CHECK(filetype::fromName("noextension") == Kind::Other);
    CHECK(filetype::fromName(".hidden") == Kind::Other);
    CHECK(filetype::hasTitleMeta(Kind::Nds));
    CHECK(!filetype::hasTitleMeta(Kind::Image));
}

TEST("fileops: copy, move and delete files and folders")
{
    TempDir temp;
    const std::string root = temp.path();
    CHECK(fs::makeDirectories(root + "src/inner/"));
    CHECK(writeText(root + "src/a.txt", "hello"));
    CHECK(writeText(root + "src/inner/b.bin", std::string(300000, 'x')));

    const fileops::Totals totals = fileops::measure(root + "src");
    CHECK_EQ(totals.files, static_cast<uint64_t>(2));
    CHECK_EQ(totals.folders, static_cast<uint64_t>(1));
    CHECK_EQ(totals.bytes, static_cast<uint64_t>(300005));
    // The running total adds up across calls.
    std::atomic<uint64_t> running(0);
    fileops::measure(root + "src", nullptr, &running);
    fileops::measure(root + "src/a.txt", nullptr, &running);
    CHECK_EQ(running.load(), static_cast<uint64_t>(300010));

    // Stopped before it starts, the count is empty.
    const std::atomic<bool> stopped(true);
    CHECK_EQ(fileops::measure(root + "src", &stopped).files,
             static_cast<uint64_t>(0));
    // The checksum counts what it reads, and gives up when stopped.
    std::atomic<uint64_t> read(0);
    CHECK(!sha256::ofFile(root + "src/inner/b.bin", nullptr, &read).empty());
    CHECK_EQ(read.load(), static_cast<uint64_t>(300000));
    CHECK_EQ(sha256::ofFile(root + "src/inner/b.bin", &stopped), std::string(""));

    Quiet observer;
    uint64_t done = 0;
    fileops::Result result = fileops::copy(root + "src", root + "copy",
                                           observer, done, totals.bytes);
    CHECK(result.status == fileops::Status::Ok);
    CHECK_EQ(result.files, static_cast<uint64_t>(2));
    CHECK_EQ(done, totals.bytes);
    CHECK_EQ(readText(root + "copy/inner/b.bin").size(),
             static_cast<size_t>(300000));
    CHECK_EQ(readText(root + "copy/a.txt"), std::string("hello"));

    // Never over an existing entry, never into itself.
    result = fileops::copy(root + "src", root + "copy", observer, done, 0);
    CHECK(result.status == fileops::Status::Failed);
    result = fileops::copy(root + "src", root + "src/inner/again", observer,
                           done, 0);
    CHECK(result.status == fileops::Status::Failed);
    CHECK(!fileops::exists(root + "src/inner/again"));

    std::string error;
    CHECK(fileops::move(root + "copy/a.txt", root + "moved.txt", error));
    CHECK(!fileops::exists(root + "copy/a.txt"));
    CHECK(!fileops::move(root + "src/a.txt", root + "moved.txt", error));
    CHECK(!fileops::move(root + "src", root + "src/inner/x", error));

    // A name that changes only in case is the entry itself, not a clash.
    CHECK(fileops::move(root + "moved.txt", root + "Moved.TXT", error));
    CHECK(fileops::move(root + "src/", root + "SRC", error));
    CHECK(fileops::move(root + "SRC", root + "SRC", error));
    std::string names;
    for (const fs::Entry &entry : fs::listEntries(root, nullptr, true, false)) {
        names += entry.name + ";";
    }
    CHECK(names.find("Moved.TXT;") != std::string::npos);
    CHECK(names.find("SRC;") != std::string::npos);
    CHECK(names.find("moved.txt;") == std::string::npos);
    CHECK(names.find("src;") == std::string::npos);
    CHECK(names.find(".3dsdl-rename") == std::string::npos);
    CHECK_EQ(readText(root + "SRC/a.txt"), std::string("hello"));

    uint64_t deleted = 0;
    result = fileops::remove(root + "copy", observer, deleted, 1);
    CHECK(result.status == fileops::Status::Ok);
    CHECK_EQ(deleted, static_cast<uint64_t>(1));
    CHECK(!fileops::exists(root + "copy"));
}

TEST("fileops: a folder merges into one of the same name")
{
    TempDir temp;
    const std::string root = temp.path();
    CHECK(fs::makeDirectories(root + "new/data/deep/"));
    CHECK(writeText(root + "new/app.3dsx", "new app"));
    CHECK(writeText(root + "new/data/deep/level.bin", "new level"));
    CHECK(writeText(root + "new/swap", "a file now"));
    CHECK(fs::makeDirectories(root + "old/DATA/"));
    CHECK(fs::makeDirectories(root + "old/swap/"));
    CHECK(writeText(root + "old/APP.3dsx", "old app"));
    CHECK(writeText(root + "old/config.ini", "kept"));
    CHECK(writeText(root + "old/DATA/save.bin", "kept too"));
    CHECK(writeText(root + "old/swap/inside", "a folder before"));

    // Copied: same names are replaced whatever their case, folders meet,
    // and what only the destination had stays.
    Quiet observer;
    uint64_t done = 0;
    fileops::Result result = fileops::merge(root + "new", root + "old", false,
                                            observer, done, 0);
    CHECK(result.status == fileops::Status::Ok);
    CHECK_EQ(result.files, static_cast<uint64_t>(3));
    CHECK_EQ(readText(root + "old/app.3dsx"), std::string("new app"));
    CHECK_EQ(readText(root + "old/data/deep/level.bin"), std::string("new level"));
    CHECK_EQ(readText(root + "old/DATA/save.bin"), std::string("kept too"));
    CHECK_EQ(readText(root + "old/config.ini"), std::string("kept"));
    CHECK(!fileops::isFolder(root + "old/swap"));
    CHECK_EQ(readText(root + "old/swap"), std::string("a file now"));
    CHECK(fileops::exists(root + "new/app.3dsx"));

    // Moved: the same, and the source is gone after.
    CHECK(writeText(root + "new/app.3dsx", "newer app"));
    result = fileops::merge(root + "new", root + "old", true, observer, done, 0);
    CHECK(result.status == fileops::Status::Ok);
    CHECK(!fileops::exists(root + "new"));
    CHECK_EQ(readText(root + "old/app.3dsx"), std::string("newer app"));
    CHECK_EQ(readText(root + "old/data/deep/level.bin"), std::string("new level"));
    CHECK_EQ(readText(root + "old/config.ini"), std::string("kept"));

    // Never into itself.
    CHECK(fs::makeDirectories(root + "old/sub/"));
    result = fileops::merge(root + "old", root + "old/sub", true, observer, done, 0);
    CHECK(result.status == fileops::Status::Failed);
    CHECK(fileops::exists(root + "old/config.ini"));
}

TEST("fileops: a cancelled copy removes the unfinished file")
{
    TempDir temp;
    CHECK(writeText(temp.path() + "big.bin", std::string(600000, 'y')));
    Quiet observer;
    observer.cancelAfter = 1;
    uint64_t done = 0;
    const fileops::Result result = fileops::copy(
        temp.path() + "big.bin", temp.path() + "copy.bin", observer, done,
        600000);
    CHECK(result.status == fileops::Status::Cancelled);
    CHECK(!fileops::exists(temp.path() + "copy.bin"));
}

TEST("fs: big folders are read in batches and merge into order")
{
    TempDir temp;
    const std::string root = temp.path() + "big/";
    CHECK(fs::makeDirectories(root + "zfolder/"));
    CHECK(fs::makeDirectories(root + "Afolder/"));
    for (int index = 0; index < 150; ++index) {
        const std::string name = "file" + std::to_string(149 - index) +
                                 (index % 3 == 0 ? ".txt" : ".bin");
        CHECK(writeText(root + name, std::string(index % 7, 'x')));
    }

    size_t batches = 0;
    std::vector<fs::Entry> merged;
    CHECK(fs::readEntries(root, false, false, [&](std::vector<fs::Entry> &&batch) {
        ++batches;
        fs::mergeEntries(merged, std::move(batch), fs::SortOrder::Name);
    }));
    CHECK(batches >= 2);
    CHECK_EQ(merged.size(), static_cast<size_t>(152));
    // Merging batch by batch gives the order a full sort gives.
    for (const fs::SortOrder order : {fs::SortOrder::Name, fs::SortOrder::Size,
                                      fs::SortOrder::Type}) {
        std::vector<fs::Entry> whole = fs::listEntries(root, nullptr, false, false);
        fs::sortEntries(whole, order);
        std::vector<fs::Entry> pieces;
        fs::readEntries(root, false, false, [&](std::vector<fs::Entry> &&batch) {
            fs::mergeEntries(pieces, std::move(batch), order);
        });
        CHECK_EQ(pieces.size(), whole.size());
        bool same = pieces.size() == whole.size();
        for (size_t index = 0; same && index < whole.size(); ++index) {
            same = pieces[index].name == whole[index].name;
        }
        CHECK(same);
    }
    CHECK(merged.front().folder && merged.front().name == "Afolder");

    // Stopped after the first batch, it reads no further.
    std::atomic<bool> stop(false);
    size_t seen = 0;
    fs::readEntries(root, false, false, [&](std::vector<fs::Entry> &&batch) {
        seen += batch.size();
        stop = true;
    }, &stop);
    CHECK(seen > 0 && seen < 152);
    CHECK(!fs::readEntries(root + "missing/", false, false,
                           [](std::vector<fs::Entry> &&) {}));
}

TEST("fileops: a lookup answers from each folder's listing")
{
    TempDir temp;
    const std::string root = temp.path();
    fs::makeDirectories(root + "Music/Album/");
    CHECK(writeText(root + "Music/Song.mp3", "12345"));
    CHECK(writeText(root + "Music/.hidden", "x"));

    fileops::Lookup lookup;
    CHECK(lookup.exists(root + "Music/Song.mp3"));
    CHECK(lookup.exists(root + "Music/song.MP3")); // FAT ignores case
    CHECK(lookup.exists(root + "Music/.hidden"));
    CHECK(!lookup.isFolder(root + "Music/Song.mp3"));
    CHECK_EQ(lookup.size(root + "Music/Song.mp3"), int64_t(5));
    CHECK(lookup.isFolder(root + "Music/Album"));
    CHECK_EQ(lookup.size(root + "Music/Album"), int64_t(-1));
    CHECK(!lookup.exists(root + "Music/Other.mp3"));
    CHECK(!lookup.exists(root + "Missing/Song.mp3"));
    CHECK(!lookup.exists(root + "Music/"));

    // Read once: what changes on the card afterwards is not seen, but what
    // is claimed is.
    CHECK(writeText(root + "Music/New.mp3", "1"));
    CHECK(!lookup.exists(root + "Music/New.mp3"));
    lookup.claim(root + "Music/Song (1).mp3", false);
    CHECK(lookup.exists(root + "Music/song (1).mp3"));
    lookup.claim(root + "Missing/Folder", true);
    CHECK(lookup.isFolder(root + "Missing/Folder"));

    // measure asks the lookup what each path is, and reads folders as ever.
    CHECK(writeText(root + "Music/Album/Track.mp3", "123"));
    fileops::Lookup fresh;
    const fileops::Totals song =
        fileops::measure(root + "Music/Song.mp3", nullptr, nullptr, nullptr, &fresh);
    CHECK_EQ(song.files, uint64_t(1));
    CHECK_EQ(song.bytes, uint64_t(5));
    const fileops::Totals album =
        fileops::measure(root + "Music/Album", nullptr, nullptr, nullptr, &fresh);
    CHECK_EQ(album.files, uint64_t(1));
    CHECK_EQ(album.bytes, uint64_t(3));
    CHECK_EQ(fileops::measure(root + "Music/Gone.mp3", nullptr, nullptr, nullptr,
                              &fresh).files,
             uint64_t(0));
}

TEST("fileops: names and paths")
{
    std::string error;
    CHECK(fileops::validName("My Folder", error));
    CHECK(fileops::validName("config.ini", error));
    CHECK(!fileops::validName("", error));
    CHECK(!fileops::validName("..", error));
    CHECK(!fileops::validName("a/b", error));
    CHECK(!fileops::validName("what?", error));
    CHECK(!fileops::validName("trailing.", error));

    CHECK(fileops::isInside("sdmc:/a/b", "sdmc:/a"));
    CHECK(fileops::isInside("sdmc:/a", "sdmc:/a/"));
    CHECK(!fileops::isInside("sdmc:/ab", "sdmc:/a"));
    CHECK_EQ(fileops::name("sdmc:/3ds/app/"), std::string("app"));
    CHECK_EQ(fileops::name("sdmc:/x.txt"), std::string("x.txt"));
}

TEST("fs: hidden entries and sort orders")
{
    TempDir temp;
    CHECK(writeText(temp.path() + "b.txt", "12"));
    CHECK(writeText(temp.path() + "a.zip", "1234"));
    CHECK(writeText(temp.path() + ".secret", "1"));
    CHECK(fs::makeDirectories(temp.path() + "folder/"));

    std::vector<fs::Entry> entries = fs::listEntries(temp.path());
    CHECK_EQ(entries.size(), static_cast<size_t>(3));
    entries = fs::listEntries(temp.path(), nullptr, true);
    CHECK_EQ(entries.size(), static_cast<size_t>(4));

    fs::sortEntries(entries, fs::SortOrder::Size);
    CHECK_EQ(entries[0].name, std::string("folder"));
    CHECK_EQ(entries[1].name, std::string("a.zip"));
    CHECK_EQ(entries[3].name, std::string(".secret"));

    fs::sortEntries(entries, fs::SortOrder::Type);
    CHECK_EQ(entries[1].name, std::string(".secret")); // no extension first
    CHECK_EQ(entries[2].name, std::string("b.txt"));
    CHECK_EQ(entries[3].name, std::string("a.zip"));

    // Reversed, both groups turn around and folders still come first.
    CHECK(fs::makeDirectories(temp.path() + "another/"));
    entries = fs::listEntries(temp.path());
    fs::sortEntries(entries, fs::SortOrder::Name, true);
    CHECK_EQ(entries[0].name, std::string("folder"));
    CHECK_EQ(entries[1].name, std::string("another"));
    CHECK_EQ(entries[2].name, std::string("b.txt"));
    CHECK_EQ(entries[3].name, std::string("a.zip"));
    fs::sortEntries(entries, fs::SortOrder::Size, true);
    CHECK_EQ(entries[2].name, std::string("b.txt")); // smallest first
    // Folders sort by size once added up; until then they come last.
    std::vector<fs::Entry> folders(3);
    folders[0].name = "small";
    folders[0].size = 10;
    folders[1].name = "pending";
    folders[2].name = "big";
    folders[2].size = 900;
    for (fs::Entry &folder : folders) {
        folder.folder = true;
    }
    fs::sortEntries(folders, fs::SortOrder::Size);
    CHECK_EQ(folders[0].name, std::string("big"));
    CHECK_EQ(folders[1].name, std::string("small"));
    CHECK_EQ(folders[2].name, std::string("pending"));
    fs::sortEntries(folders, fs::SortOrder::Size, true);
    CHECK_EQ(folders[0].name, std::string("small"));
    CHECK_EQ(folders[2].name, std::string("pending"));
    std::vector<fs::Entry> merged;
    fs::mergeEntries(merged, fs::listEntries(temp.path()), fs::SortOrder::Name,
                     true);
    CHECK_EQ(merged.front().name, std::string("folder"));
    CHECK_EQ(merged.back().name, std::string("a.zip"));

    Settings settings;
    settings.showHiddenFiles = true;
    settings.fileSort = fs::SortOrder::Date;
    settings.fileSortReverse = true;
    const Settings parsed = parseSettings(serializeSettings(settings));
    CHECK(parsed.showHiddenFiles);
    CHECK(parsed.fileSort == fs::SortOrder::Date);
    CHECK(parsed.fileSortReverse);
    CHECK(!parseSettings("file_sort=name\n").fileSortReverse);
    settings.titleSort = fs::SortOrder::Size;
    settings.titleSortReverse = true;
    const Settings titles = parseSettings(serializeSettings(settings));
    CHECK(titles.titleSort == fs::SortOrder::Size);
    CHECK(titles.titleSortReverse);
    // Titles have no dates or types.
    CHECK(parseSettings("title_sort=date\n").titleSort == fs::SortOrder::Name);
}

TEST("textdoc: keeps line endings, BOM and the final newline")
{
    textdoc::Document unix = textdoc::parse("a\nb\n");
    CHECK_EQ(unix.lines.size(), static_cast<size_t>(2));
    CHECK(!unix.crlf);
    CHECK(unix.finalNewline);
    unix.lines[1] = "changed";
    CHECK_EQ(textdoc::serialize(unix), std::string("a\nchanged\n"));

    const std::string windows = "\xef\xbb\xbf[core]\r\nkey=1\r\nlast";
    textdoc::Document document = textdoc::parse(windows);
    CHECK(document.bom);
    CHECK(document.crlf);
    CHECK(!document.finalNewline);
    CHECK_EQ(document.lines[0], std::string("[core]"));
    CHECK_EQ(textdoc::serialize(document), windows);

    const textdoc::Document empty = textdoc::parse("");
    CHECK_EQ(empty.lines.size(), static_cast<size_t>(1));
    CHECK_EQ(textdoc::serialize(empty), std::string(""));

    CHECK(textdoc::looksLikeText("plain text\twith tab\n"));
    CHECK(!textdoc::looksLikeText(std::string("bin\0ary", 7)));
}

TEST("textdoc: load, save and read-only cases")
{
    TempDir temp;
    const std::string path = temp.path() + "config.ini";
    CHECK(writeText(path, "x=1\r\ny=2\r\n"));
    textdoc::Document document;
    CHECK(textdoc::load(path, document) == textdoc::Load::Ok);
    CHECK(!document.readOnly);
    document.lines.insert(document.lines.begin() + 1, "z=3");
    CHECK(textdoc::save(path, document));
    CHECK_EQ(readText(path), std::string("x=1\r\nz=3\r\ny=2\r\n"));

    CHECK(writeText(temp.path() + "big.txt", std::string(100, 'a') + "\n" +
                                                 std::string(100, 'b')));
    CHECK(textdoc::load(temp.path() + "big.txt", document, 150) ==
          textdoc::Load::Ok);
    CHECK(document.readOnly);
    CHECK(document.truncated);
    CHECK_EQ(document.lines.size(), static_cast<size_t>(1));

    CHECK(writeText(temp.path() + "latin.txt", "caf\xe9\n"));
    CHECK(textdoc::load(temp.path() + "latin.txt", document) ==
          textdoc::Load::Ok);
    CHECK(document.readOnly);
    CHECK_EQ(document.lines[0], std::string("caf\xc3\xa9"));

    CHECK(writeText(temp.path() + "bin", std::string("\0\1\2", 3)));
    CHECK(textdoc::load(temp.path() + "bin", document) ==
          textdoc::Load::NotText);
    CHECK(textdoc::head(temp.path() + "bin", 3).empty());
    CHECK_EQ(textdoc::head(path, 2).size(), static_cast<size_t>(2));
}

TEST("titlemeta: 3dsx, smdh, game images, DS games and tickets")
{
    TempDir temp;
    titlemeta::Meta meta;
    std::string error;

    // A .3dsx with an extended header pointing at its SMDH.
    std::string homebrew(0x40, '\0');
    std::memcpy(&homebrew[0], "3DSX", 4);
    homebrew[4] = 0x2c;
    putLe32(homebrew, 0x20, 0x40);
    putLe32(homebrew, 0x24, smdh::Size);
    homebrew += makeSmdh("Homebrew", "Someone");
    CHECK(writeText(temp.path() + "app.3dsx", homebrew));
    CHECK(titlemeta::read(temp.path() + "app.3dsx",
                          filetype::Kind::Homebrew, meta, error));
    CHECK_EQ(meta.name, std::string("Homebrew"));
    CHECK_EQ(meta.publisher, std::string("Someone"));
    CHECK_EQ(meta.icon.size(), static_cast<size_t>(48 * 48));

    // A decrypted .3ds whose ExeFS holds an icon.
    std::string cart(0x4000 + smdh::Size, '\0');
    std::memcpy(&cart[0x100], "NCSD", 4);
    putLe32(cart, 0x108, 0x00164800);
    putLe32(cart, 0x10c, 0x00040000);
    putLe32(cart, 0x120, 0x10); // partition 0 at 0x2000
    std::memcpy(&cart[0x2100], "NCCH", 4);
    std::memcpy(&cart[0x2150], "CTR-P-TEST", 10);
    cart[0x218f] = 0x04;        // NoCrypto
    putLe32(cart, 0x21a0, 0x8); // ExeFS at 0x2000 + 0x1000
    std::memcpy(&cart[0x3000], "icon", 4);
    putLe32(cart, 0x3008, 0x800);
    const std::string icon = makeSmdh("Cartridge", "Maker");
    cart.replace(0x3000 + 0x200 + 0x800, icon.size(), icon);
    CHECK(writeText(temp.path() + "game.3ds", cart));
    CHECK(titlemeta::read(temp.path() + "game.3ds",
                          filetype::Kind::CartridgeImage, meta, error));
    CHECK(meta.hasTitleId);
    CHECK_EQ(meta.titleId, static_cast<uint64_t>(0x0004000000164800ULL));
    CHECK_EQ(meta.productCode, std::string("CTR-P-TEST"));
    CHECK_EQ(meta.name, std::string("Cartridge"));

    cart[0x218f] = 0;
    CHECK(writeText(temp.path() + "locked.3ds", cart));
    CHECK(titlemeta::read(temp.path() + "locked.3ds",
                          filetype::Kind::CartridgeImage, meta, error));
    CHECK(meta.name.empty());
    CHECK(!meta.note.empty());

    // A DS game with a banner whose title has a publisher line.
    std::string nds(0x200 + 0x840, '\0');
    std::memcpy(&nds[0], "TESTGAME", 8);
    std::memcpy(&nds[0x0c], "ABCE", 4);
    putLe32(nds, 0x68, 0x200);
    putLe32(nds, 0x200 + 0x220 + 2, 0x7fff); // colour 1 is white
    nds[0x200 + 0x20] = 0x01;                // first pixel uses colour 1
    putUtf16(nds, 0x200 + 0x340, "Test Game\nPublisher");
    CHECK(writeText(temp.path() + "game.nds", nds));
    CHECK(titlemeta::read(temp.path() + "game.nds", filetype::Kind::Nds, meta,
                          error));
    CHECK_EQ(meta.name, std::string("Test Game"));
    CHECK_EQ(meta.publisher, std::string("Publisher"));
    CHECK_EQ(meta.productCode, std::string("ABCE"));
    CHECK_EQ(meta.icon.size(), static_cast<size_t>(48 * 48));
    if (meta.icon.size() == 48 * 48) {
        CHECK_EQ(meta.icon[0], static_cast<uint16_t>(0xffff));
    }

    // A ticket signed with RSA-2048.
    std::string ticket(0x350, '\0');
    ticket[1] = 0x01;
    ticket[3] = 0x04;
    const unsigned char id[8] = {0x00, 0x04, 0x00, 0x00,
                                 0x00, 0x16, 0x48, 0x00};
    std::memcpy(&ticket[0x140 + 0x9c], id, 8);
    CHECK(writeText(temp.path() + "a.tik", ticket));
    CHECK(titlemeta::read(temp.path() + "a.tik", filetype::Kind::Ticket, meta,
                          error));
    CHECK_EQ(meta.titleId, static_cast<uint64_t>(0x0004000000164800ULL));

    CHECK(!titlemeta::read(temp.path() + "a.tik", filetype::Kind::Homebrew,
                           meta, error));
}

TEST("picture: fits, enlarges and blends transparency")
{
    // 4x2 opaque red shrunk into 2x2 keeps the shape: 2x1.
    std::vector<uint8_t> red(4 * 2 * 4, 0);
    for (size_t index = 0; index < red.size(); index += 4) {
        red[index] = 255;
        red[index + 3] = 255;
    }
    picture::Picture result;
    picture::fit(red.data(), 4, 4, 2, 2, 2, 0, result);
    CHECK_EQ(result.width, static_cast<uint32_t>(2));
    CHECK_EQ(result.height, static_cast<uint32_t>(1));
    CHECK_EQ(result.pixels[0], static_cast<uint16_t>(0xf800));

    // A 2x2 picture in a 9x11 box is enlarged by the whole factor 4.
    const uint8_t rgb[12] = {0, 0, 255, 0, 0, 255, 0, 0, 255, 0, 0, 255};
    picture::fit(rgb, 3, 2, 2, 9, 11, 0, result);
    CHECK_EQ(result.width, static_cast<uint32_t>(8));
    CHECK_EQ(result.pixels.size(), static_cast<size_t>(64));
    CHECK_EQ(result.pixels[63], static_cast<uint16_t>(0x001f));

    // The size alone, as the top screen works it out before decoding.
    uint32_t width = 0;
    uint32_t height = 0;
    picture::fittedSize(4032, 3024, 160, 132, width, height);
    CHECK_EQ(width, static_cast<uint32_t>(160));
    CHECK_EQ(height, static_cast<uint32_t>(120));
    picture::fittedSize(48, 48, 160, 132, width, height);
    CHECK_EQ(width, static_cast<uint32_t>(96));
    picture::fittedSize(0, 10, 160, 132, width, height);
    CHECK_EQ(width, static_cast<uint32_t>(0));

    // Fully transparent pixels show the background.
    const uint8_t clear[4] = {255, 255, 255, 0};
    picture::fit(clear, 4, 1, 1, 1, 1, 0x07e0, result);
    CHECK_EQ(result.pixels[0], static_cast<uint16_t>(0x07e0));
}

TEST("picture: decodes a BMP file and refuses non-pictures")
{
    TempDir temp;
    // A 2x1 24-bit BMP: blue then green (rows are BGR, padded to 4 bytes).
    std::string bmp(54 + 8, '\0');
    bmp[0] = 'B';
    bmp[1] = 'M';
    putLe32(bmp, 2, static_cast<uint32_t>(bmp.size()));
    putLe32(bmp, 10, 54);
    putLe32(bmp, 14, 40);
    putLe32(bmp, 18, 2);
    putLe32(bmp, 22, 1);
    bmp[26] = 1;
    bmp[28] = 24;
    putLe32(bmp, 34, 8);
    bmp[54] = static_cast<char>(255); // blue
    bmp[58] = static_cast<char>(255); // green
    CHECK(writeText(temp.path() + "two.bmp", bmp));

    uint32_t width = 0;
    uint32_t height = 0;
    CHECK(picture::size(temp.path() + "two.bmp", width, height));
    CHECK_EQ(width, static_cast<uint32_t>(2));

    picture::Picture result;
    std::string error;
    CHECK(picture::load(temp.path() + "two.bmp", 400, 240, 0, result, error));
    CHECK_EQ(result.sourceWidth, static_cast<uint32_t>(2));
    CHECK_EQ(result.width, static_cast<uint32_t>(400));
    CHECK_EQ(result.height, static_cast<uint32_t>(200));
    CHECK_EQ(result.pixels.front(), static_cast<uint16_t>(0x001f));
    CHECK_EQ(result.pixels.back(), static_cast<uint16_t>(0x07e0));

    CHECK(writeText(temp.path() + "fake.png", "not a picture"));
    CHECK(!picture::load(temp.path() + "fake.png", 400, 240, 0, result,
                         error));
    CHECK(!error.empty());
}

TEST("picture: decodes from memory and fills a box by cutting the edges")
{
    // A 4x1 24-bit BMP: blue, green, red, white.
    std::string bmp(54 + 12, '\0');
    bmp[0] = 'B';
    bmp[1] = 'M';
    putLe32(bmp, 2, static_cast<uint32_t>(bmp.size()));
    putLe32(bmp, 10, 54);
    putLe32(bmp, 14, 40);
    putLe32(bmp, 18, 4);
    putLe32(bmp, 22, 1);
    bmp[26] = 1;
    bmp[28] = 24;
    putLe32(bmp, 34, 12);
    bmp[54] = static_cast<char>(255);
    bmp[58] = static_cast<char>(255);
    bmp[62] = static_cast<char>(255);
    bmp[63] = bmp[64] = bmp[65] = static_cast<char>(255);
    const std::vector<uint8_t> bytes(bmp.begin(), bmp.end());

    picture::Picture result;
    std::string error;
    CHECK(picture::decode(bytes, 400, 240, 0, result, error));
    CHECK_EQ(result.width, static_cast<uint32_t>(400));
    CHECK_EQ(result.height, static_cast<uint32_t>(100));

    // Filling a square keeps the middle: green, shown 3x3.
    CHECK(picture::decode(bytes, 3, 3, 0, result, error, true));
    CHECK_EQ(result.width, static_cast<uint32_t>(3));
    CHECK_EQ(result.height, static_cast<uint32_t>(3));
    CHECK_EQ(result.sourceWidth, static_cast<uint32_t>(4));
    for (const uint16_t pixel : result.pixels) {
        CHECK_EQ(pixel, static_cast<uint16_t>(0x07e0));
    }
    CHECK(picture::decode(bytes, 2, 1, 0, result, error, true));
    CHECK_EQ(result.pixels[0], static_cast<uint16_t>(0x07e0));
    CHECK_EQ(result.pixels[1], static_cast<uint16_t>(0xf800));

    CHECK(!picture::decode({1, 2, 3}, 48, 48, 0, result, error, true));
    CHECK(!error.empty());
}

TEST("remote: a sender's 3dsx replaces only the same app")
{
    TempDir temp;
    const auto homebrew = [](const std::string &name, const std::string &publisher) {
        std::string data(0x40, '\0');
        std::memcpy(&data[0], "3DSX", 4);
        data[4] = 0x2c;
        putLe32(data, 0x20, 0x40);
        putLe32(data, 0x24, smdh::Size);
        return data + makeSmdh(name, publisher);
    };
    CHECK(writeText(temp.path() + "old.3dsx", homebrew("Checkpoint", "Bernardo")));
    CHECK(writeText(temp.path() + "new.3dsx", homebrew("CHECKPOINT", "bernardo")));
    CHECK(writeText(temp.path() + "other.3dsx", homebrew("Checkpoint", "Someone")));
    CHECK(writeText(temp.path() + "renamed.3dsx", homebrew("Evil", "Bernardo")));
    CHECK(writeText(temp.path() + "bare.3dsx", std::string(0x40, '\0')));
    CHECK(writeText(temp.path() + "same.cia", homebrew("Checkpoint", "Bernardo")));
    std::string name;
    CHECK(remote::sameApp(temp.path() + "old.3dsx", temp.path() + "new.3dsx", &name));
    CHECK_EQ(name, std::string("CHECKPOINT"));
    CHECK(!remote::sameApp(temp.path() + "old.3dsx", temp.path() + "other.3dsx"));
    CHECK(!remote::sameApp(temp.path() + "old.3dsx", temp.path() + "renamed.3dsx"));
    // Files that say nothing about themselves match nothing, not even each
    // other, and a CIA is never a 3dsx's update.
    CHECK(!remote::sameApp(temp.path() + "bare.3dsx", temp.path() + "bare.3dsx"));
    CHECK(!remote::sameApp(temp.path() + "old.3dsx", temp.path() + "same.cia"));
}
