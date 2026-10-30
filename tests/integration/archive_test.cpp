#include "../check.hpp"
#include "../support.hpp"

#include "archives.hpp"
#include "fs_util.hpp"
#include "remote.hpp"

#include <archive.h>
#include <archive_entry.h>

#include <cstdlib>
#include <fstream>
#include <iterator>

namespace {

std::string fixture(const std::string &name)
{
    const char *folder = std::getenv("FIXTURE_ZIPS");
    return std::string(folder != nullptr ? folder : ".") + "/" + name;
}

// The RAR files are from libarchive's own tests (BSD licence): 3dsDL cannot
// make RARs, only read them.
std::string rar(const std::string &name)
{
    const char *folder = std::getenv("FIXTURE_RARS");
    return std::string(folder != nullptr ? folder : ".") + "/" + name;
}

std::string binary()
{
    std::string data;
    for (int repeat = 0; repeat < 40; ++repeat) {
        for (int value = 0; value < 256; ++value) {
            data += static_cast<char>(value);
        }
    }
    return data;
}

std::string readAll(const std::string &path)
{
    std::ifstream file(path, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(file), {});
}

struct Quiet : archives::Observer {
    int progressCalls = 0;
    int cancelAfterCalls = -1;
    archives::Progress last;
    bool cancelRequested() override
    {
        return cancelAfterCalls >= 0 && progressCalls >= cancelAfterCalls;
    }
    void onProgress(const archives::Progress &progress) override
    {
        ++progressCalls;
        last = progress;
    }
};

struct Item {
    std::string name;
    std::string data;
    bool folder = false;
    std::string link; // a symlink to this, when set
};

// Writes an archive with libarchive, in format with filter (an
// ARCHIVE_FILTER_ code, or ARCHIVE_FILTER_NONE).
bool make(const std::string &path, int format, int filter,
          const std::vector<Item> &items)
{
    struct archive *writer = archive_write_new();
    archive_write_set_format(writer, format);
    if (filter != ARCHIVE_FILTER_NONE) {
        archive_write_add_filter(writer, filter);
    }
    bool ok = archive_write_open_filename(writer, path.c_str()) == ARCHIVE_OK;
    for (const Item &item : items) {
        struct archive_entry *entry = archive_entry_new();
        archive_entry_set_pathname(entry, item.name.c_str());
        if (item.folder) {
            archive_entry_set_filetype(entry, AE_IFDIR);
            archive_entry_set_perm(entry, 0755);
        } else if (!item.link.empty()) {
            archive_entry_set_filetype(entry, AE_IFLNK);
            archive_entry_set_symlink(entry, item.link.c_str());
            archive_entry_set_perm(entry, 0777);
        } else {
            archive_entry_set_filetype(entry, AE_IFREG);
            archive_entry_set_perm(entry, 0644);
            archive_entry_set_size(entry,
                                   static_cast<la_int64_t>(item.data.size()));
        }
        ok = ok && archive_write_header(writer, entry) == ARCHIVE_OK;
        if (!item.data.empty()) {
            ok = ok && archive_write_data(writer, item.data.data(),
                                          item.data.size()) ==
                           static_cast<la_ssize_t>(item.data.size());
        }
        archive_entry_free(entry);
    }
    ok = archive_write_close(writer) == ARCHIVE_OK && ok;
    archive_write_free(writer);
    return ok;
}

std::vector<Item> pack()
{
    return {
        {"pack/", "", true, ""},
        {"pack/readme.txt", "hello from an archive\n", false, ""},
        {"pack/data.bin", binary(), false, ""},
        {"pack/empty/", "", true, ""},
        {"pack/none.txt", "", false, ""},
    };
}

void checkPack(const std::string &folder)
{
    CHECK_EQ(readText(folder + "pack/readme.txt"),
             std::string("hello from an archive\n"));
    CHECK_EQ(readAll(folder + "pack/data.bin"), binary());
    CHECK(fs::directoryExists(folder + "pack/empty"));
    CHECK(fs::fileExists(folder + "pack/none.txt"));
}

archives::Result run(const std::string &path, const archives::Listing &listing,
                     const std::string &folder,
                     archives::Existing existing = archives::Existing::Skip,
                     std::vector<std::string> only = {})
{
    archives::Options options;
    options.existing = existing;
    options.only = std::move(only);
    Quiet observer;
    return archives::extract(path, listing, folder, options, observer);
}

// Only what is under top/ is extracted, without the top/.
archives::Result runUnder(const std::string &path,
                          const archives::Listing &listing,
                          const std::string &folder, const std::string &top)
{
    archives::Options options;
    options.place = [top](const std::string &entry, bool) {
        return entry.rfind(top, 0) == 0 ? entry.substr(top.size())
                                        : std::string();
    };
    Quiet observer;
    return archives::extract(path, listing, folder, options, observer);
}

} // namespace

TEST("archives: ZIP stored, deflated and empty entries")
{
    archives::Listing listing;
    std::string error;
    CHECK(archives::read(fixture("basic.zip"), archives::Scan::Quick, listing,
                         error));
    CHECK(listing.complete);
    CHECK(listing.ownZip);
    CHECK_EQ(listing.format, std::string("ZIP"));
    CHECK_EQ(listing.fileCount, 3u);
    CHECK_EQ(listing.totalSize, static_cast<uint64_t>(binary().size() + 17));
    CHECK_EQ(listing.top, std::string(""));

    TempDir folder;
    const archives::Result result =
        run(fixture("basic.zip"), listing, folder.path());
    CHECK(result.ok);
    CHECK_EQ(result.extracted, 3u);
    CHECK_EQ(readText(folder.path() + "readme.txt"),
             std::string("hello from a zip\n"));
    CHECK_EQ(readAll(folder.path() + "3ds/App/App.3dsx"), binary());
    CHECK(fs::fileExists(folder.path() + "empty.txt"));
}

TEST("archives: existing files are skipped, or replaced whole")
{
    TempDir folder;
    if (!foldsCase(folder.path())) {
        check::fail(__FILE__, __LINE__, "TMPDIR must ignore case, as FAT does");
        return;
    }
    CHECK(writeText(folder.path() + "README.TXT", "mine"));
    archives::Listing listing;
    std::string error;
    CHECK(archives::read(fixture("basic.zip"), archives::Scan::Quick, listing,
                         error));

    // FAT ignores case, so README.TXT is readme.txt.
    std::vector<std::string> existing;
    CHECK(archives::existingFiles(listing, folder.path(), existing));
    CHECK_EQ(existing.size(), static_cast<size_t>(1));

    archives::Result result = run(fixture("basic.zip"), listing, folder.path());
    CHECK(result.ok);
    CHECK_EQ(result.skippedExisting.size(), static_cast<size_t>(1));
    CHECK_EQ(readText(folder.path() + "readme.txt"), std::string("mine"));

    result = run(fixture("basic.zip"), listing, folder.path(),
                 archives::Existing::Overwrite, result.skippedExisting);
    CHECK(result.ok);
    CHECK_EQ(result.extracted, 1u);
    CHECK_EQ(readText(folder.path() + "readme.txt"),
             std::string("hello from a zip\n"));
    CHECK(!fs::fileExists(folder.path() + "readme.txt.3dsdl-tmp"));
}

TEST("archives: a damaged entry never costs the file it would replace")
{
    TempDir folder;
    CHECK(writeText(folder.path() + "data.bin", "mine"));
    archives::Listing listing;
    std::string error;
    CHECK(archives::read(fixture("corrupt.zip"), archives::Scan::Quick, listing,
                         error));
    const archives::Result result = run(fixture("corrupt.zip"), listing,
                                        folder.path(),
                                        archives::Existing::Overwrite);
    CHECK(!result.ok);
    CHECK(result.message.find("checksum") != std::string::npos);
    CHECK_EQ(readText(folder.path() + "data.bin"), std::string("mine"));
    CHECK(!fs::fileExists(folder.path() + "data.bin.3dsdl-tmp"));

    TempDir fresh;
    CHECK(!run(fixture("corrupt.zip"), listing, fresh.path()).ok);
    CHECK(!fs::fileExists(fresh.path() + "data.bin"));

    CHECK(!archives::read(fixture("truncated.zip"), archives::Scan::Quick,
                          listing, error));
    CHECK(!error.empty());
    CHECK(!archives::read(fixture("missing.zip"), archives::Scan::Quick,
                          listing, error));
}

TEST("archives: paths that try to escape the folder are refused")
{
    CHECK_EQ(archives::safeRelativePath("../x"), std::string(""));
    CHECK_EQ(archives::safeRelativePath("/x"), std::string(""));
    CHECK_EQ(archives::safeRelativePath("C:/x"), std::string(""));
    CHECK_EQ(archives::safeRelativePath("a/../../x"), std::string(""));
    CHECK_EQ(archives::safeRelativePath("a\\b\\c.txt"),
             std::string("a/b/c.txt"));
    CHECK_EQ(archives::safeRelativePath("./a//b/"), std::string("a/b"));
    CHECK_EQ(archives::safeRelativePath("a/b?c"), std::string("a/b_c"));

    TempDir outer;
    const std::string inside = outer.path() + "inside/";
    archives::Listing listing;
    std::string error;
    CHECK(archives::read(fixture("evil.zip"), archives::Scan::Quick, listing,
                         error));
    archives::Result result = run(fixture("evil.zip"), listing, inside);
    CHECK(result.ok);
    CHECK_EQ(result.skippedUnsafe, 4u);
    CHECK_EQ(result.extracted, 2u);
    CHECK(fs::fileExists(inside + "good.txt"));
    CHECK(fs::fileExists(inside + "dir/win.txt"));
    CHECK(!fs::fileExists(outer.path() + "escape.txt"));
    CHECK(!fs::fileExists(outer.path() + "up.txt"));

    const std::string tar = outer.path() + "evil.tar.gz";
    CHECK(make(tar, ARCHIVE_FORMAT_TAR_PAX_RESTRICTED, ARCHIVE_FILTER_GZIP,
               {{"../escape.txt", "x", false, ""},
                {"ok.txt", "ok\n", false, ""},
                {"link", "", false, "../../etc/passwd"}}));
    CHECK(archives::read(tar, archives::Scan::Quick, listing, error));
    result = run(tar, listing, inside);
    CHECK(result.ok);
    CHECK_EQ(result.skippedUnsafe, 1u);
    CHECK_EQ(result.skippedUnsupported, 1u);
    CHECK(fs::fileExists(inside + "ok.txt"));
    CHECK(!fs::fileExists(inside + "link"));
    CHECK(!fs::fileExists(outer.path() + "escape.txt"));
}

TEST("archives: ZIP64 archives and UTF-8 names")
{
    TempDir folder;
    archives::Listing listing;
    std::string error;
    CHECK(archives::read(fixture("zip64.zip"), archives::Scan::Quick, listing,
                         error));
    CHECK(run(fixture("zip64.zip"), listing, folder.path()).ok);
    CHECK_EQ(readAll(folder.path() + "big.bin"), binary());

    CHECK(archives::read(fixture("utf8.zip"), archives::Scan::Quick, listing,
                         error));
    CHECK(run(fixture("utf8.zip"), listing, folder.path()).ok);
    CHECK(fs::fileExists(folder.path() + "na\xc3\xafve.txt"));
}

TEST("archives: ZIPs with bzip2 go through libarchive, unknown methods skip")
{
    TempDir folder;
    archives::Listing listing;
    std::string error;
    CHECK(archives::read(fixture("mixed.zip"), archives::Scan::Quick, listing,
                         error));
    CHECK(!listing.ownZip);
    CHECK_EQ(listing.fileCount, 2u);
    archives::Result result = run(fixture("mixed.zip"), listing, folder.path());
    CHECK(result.ok);
    CHECK_EQ(result.extracted, 2u);
    CHECK_EQ(readText(folder.path() + "bz.txt"),
             std::string("packed with bzip2\n"));
    CHECK_EQ(readText(folder.path() + "plain.txt"), std::string("plain\n"));

    CHECK(archives::read(fixture("odd.zip"), archives::Scan::Quick, listing,
                         error));
    CHECK(listing.ownZip);
    result = run(fixture("odd.zip"), listing, folder.path());
    CHECK(result.ok);
    CHECK_EQ(result.skippedUnsupported, 1u);
    CHECK(!fs::fileExists(folder.path() + "odd.bin"));
}

TEST("archives: a name written twice ends up as the last copy")
{
    TempDir folder;
    archives::Listing listing;
    std::string error;
    CHECK(archives::read(fixture("dupes.zip"), archives::Scan::Quick, listing,
                         error));
    const archives::Result result =
        run(fixture("dupes.zip"), listing, folder.path());
    CHECK(result.ok);
    CHECK_EQ(readText(folder.path() + "same.txt"), std::string("second\n"));
}

TEST("archives: a name twice never writes over a file that was there")
{
    TempDir folder;
    CHECK(writeText(folder.path() + "same.txt", "mine"));
    archives::Listing listing;
    std::string error;
    CHECK(archives::read(fixture("dupes.zip"), archives::Scan::Quick, listing,
                         error));
    const archives::Result result =
        run(fixture("dupes.zip"), listing, folder.path());
    CHECK(result.ok);
    CHECK_EQ(result.skippedExisting.size(), static_cast<size_t>(2));
    CHECK_EQ(readText(folder.path() + "same.txt"), std::string("mine"));
}

TEST("archives: a large name twice ends up as the last copy")
{
    TempDir folder;
    archives::Listing listing;
    std::string error;
    CHECK(archives::read(fixture("dupes-big.zip"), archives::Scan::Quick,
                         listing, error));
    const archives::Result result =
        run(fixture("dupes-big.zip"), listing, folder.path());
    CHECK(result.ok);
    CHECK_EQ(readAll(folder.path() + "big.bin"), std::string(300 * 1024, '2'));
    CHECK(!fs::fileExists(folder.path() + "big.bin.3dsdl-tmp"));
    CHECK(!fs::fileExists(folder.path() + "big.bin.3dsdl-old"));
}

TEST("archives: one folder at the top is found")
{
    archives::Listing listing;
    std::string error;
    CHECK(archives::read(fixture("rooted.zip"), archives::Scan::Quick, listing,
                         error));
    CHECK_EQ(listing.top, std::string("Game"));
    CHECK(listing.topIsFolder);
}

TEST("archives: ZIP extraction can be cancelled")
{
    TempDir folder;
    archives::Listing listing;
    std::string error;
    CHECK(archives::read(fixture("basic.zip"), archives::Scan::Quick, listing,
                         error));
    Quiet observer;
    observer.cancelAfterCalls = 1;
    const archives::Result result = archives::extract(
        fixture("basic.zip"), listing, folder.path(), {}, observer);
    CHECK(!result.ok);
    CHECK(result.cancelled);
}

TEST("archives: 7z lists quickly and extracts")
{
    TempDir work;
    const std::string path = work.path() + "pack.7z";
    CHECK(make(path, ARCHIVE_FORMAT_7ZIP, ARCHIVE_FILTER_NONE, pack()));
    archives::Listing listing;
    std::string error;
    CHECK(archives::read(path, archives::Scan::Quick, listing, error));
    CHECK(listing.complete);
    CHECK(!listing.ownZip);
    CHECK_EQ(listing.format, std::string("7z"));
    CHECK_EQ(listing.fileCount, 3u);
    CHECK_EQ(listing.top, std::string("pack"));
    CHECK(listing.topIsFolder);

    const std::string out = work.path() + "out/";
    const archives::Result result = run(path, listing, out);
    CHECK(result.ok);
    CHECK_EQ(result.extracted, 3u);
    checkPack(out);
}

TEST("archives: compressed tars list only when asked, extract either way")
{
    const struct {
        int filter;
        const char *format;
    } kinds[] = {{ARCHIVE_FILTER_GZIP, "tar.gz"},
                 {ARCHIVE_FILTER_BZIP2, "tar.bz2"},
                 {ARCHIVE_FILTER_XZ, "tar.xz"},
                 {ARCHIVE_FILTER_ZSTD, "tar.zst"}};
    for (const auto &kind : kinds) {
        TempDir work;
        const std::string path = work.path() + "pack.tar";
        CHECK(make(path, ARCHIVE_FORMAT_TAR_PAX_RESTRICTED, kind.filter,
                   pack()));
        archives::Listing listing;
        std::string error;
        CHECK(archives::read(path, archives::Scan::Quick, listing, error));
        CHECK(!listing.complete);
        CHECK(listing.entries.empty());
        CHECK_EQ(listing.format, std::string(kind.format));

        // A quick listing is enough to extract; progress follows the file.
        const std::string out = work.path() + "out/";
        archives::Options options;
        Quiet observer;
        const archives::Result result =
            archives::extract(path, listing, out, options, observer);
        CHECK(result.ok);
        CHECK_EQ(result.extracted, 3u);
        CHECK(observer.last.archiveBytes > 0);
        CHECK_EQ(observer.last.files, 3u);
        checkPack(out);

        Quiet scanning;
        CHECK(archives::read(path, archives::Scan::Full, listing, error,
                             &scanning));
        CHECK(listing.complete);
        CHECK_EQ(listing.fileCount, 3u);
        CHECK_EQ(listing.top, std::string("pack"));
        std::vector<std::string> existing;
        CHECK(archives::existingFiles(listing, out, existing));
        CHECK_EQ(existing.size(), static_cast<size_t>(3));
    }
}

TEST("archives: a plain tar lists quickly")
{
    TempDir work;
    const std::string path = work.path() + "pack.tar";
    CHECK(make(path, ARCHIVE_FORMAT_TAR_PAX_RESTRICTED, ARCHIVE_FILTER_NONE,
               pack()));
    archives::Listing listing;
    std::string error;
    CHECK(archives::read(path, archives::Scan::Quick, listing, error));
    CHECK(listing.complete);
    CHECK_EQ(listing.format, std::string("tar"));
    CHECK(run(path, listing, work.path() + "out/").ok);
    checkPack(work.path() + "out/");
}

TEST("archives: RAR4 and RAR5, links skipped")
{
    TempDir work;
    archives::Listing listing;
    std::string error;
    CHECK(archives::read(rar("rar4.rar"), archives::Scan::Quick, listing,
                         error));
    CHECK(listing.complete);
    CHECK_EQ(listing.format, std::string("RAR"));
    archives::Result result = run(rar("rar4.rar"), listing, work.path());
    CHECK(result.ok);
    CHECK_EQ(result.extracted, 2u);
    CHECK_EQ(result.skippedUnsupported, 1u);
    CHECK_EQ(readText(work.path() + "test.txt").substr(0, 18),
             std::string("test text document"));
    CHECK(fs::fileExists(work.path() + "testdir/test.txt"));
    CHECK(fs::directoryExists(work.path() + "testemptydir"));
    CHECK(!fs::fileExists(work.path() + "testlink"));

    CHECK(archives::read(rar("rar5.rar"), archives::Scan::Quick, listing,
                         error));
    CHECK(listing.complete);
    result = run(rar("rar5.rar"), listing, work.path());
    CHECK(result.ok);
    CHECK_EQ(fs::fileSize(work.path() + "test.bin"), 1200);
}

TEST("archives: solid RARs are spotted and listed only when asked")
{
    const auto head = [](const std::string &path) {
        const std::string data = readAll(path).substr(0, 64);
        return archives::solidRar(
            reinterpret_cast<const unsigned char *>(data.data()), data.size());
    };
    CHECK(head(rar("rar5-solid.rar")));
    CHECK(!head(rar("rar5.rar")));
    CHECK(!head(rar("rar4.rar")));
    const unsigned char rar4Solid[] = {'R', 'a', 'r', '!', 0x1a, 0x07, 0x00,
                                       0xcf, 0x90, 0x73, 0x08, 0x00};
    CHECK(archives::solidRar(rar4Solid, sizeof(rar4Solid)));

    archives::Listing listing;
    std::string error;
    CHECK(archives::read(rar("rar5-solid.rar"), archives::Scan::Quick, listing,
                         error));
    CHECK(!listing.complete);
    CHECK(archives::read(rar("rar5-solid.rar"), archives::Scan::Full, listing,
                         error));
    CHECK(listing.complete);
    CHECK_EQ(listing.fileCount, 4u);

    TempDir work;
    const archives::Result result =
        run(rar("rar5-solid.rar"), listing, work.path());
    CHECK(result.ok);
    CHECK_EQ(result.extracted, 4u);
    CHECK_EQ(fs::fileSize(work.path() + "test4.bin"), 4096);
}

TEST("archives: damaged 7z data is caught and the file removed")
{
    TempDir work;
    const std::string path = work.path() + "pack.7z";
    std::vector<Item> items = {{"big.bin", std::string(200000, 'x'), false, ""}};
    for (size_t index = 0; index < items[0].data.size(); index += 7) {
        items[0].data[index] = static_cast<char>(index * 31);
    }
    CHECK(make(path, ARCHIVE_FORMAT_7ZIP, ARCHIVE_FILTER_NONE, items));
    std::string data = readAll(path);
    data[64] = static_cast<char>(data[64] ^ 0x55);
    {
        std::ofstream file(path, std::ios::binary | std::ios::trunc);
        file.write(data.data(), static_cast<std::streamsize>(data.size()));
    }
    archives::Listing listing;
    std::string error;
    CHECK(archives::read(path, archives::Scan::Quick, listing, error));
    const archives::Result result = run(path, listing, work.path() + "out/");
    CHECK(!result.ok);
    CHECK(!result.message.empty());
    CHECK(!fs::fileExists(work.path() + "out/big.bin"));
}

TEST("archives: extracting again skips what is there, Only replaces it")
{
    TempDir work;
    const std::string path = work.path() + "pack.7z";
    CHECK(make(path, ARCHIVE_FORMAT_7ZIP, ARCHIVE_FILTER_NONE, pack()));
    archives::Listing listing;
    std::string error;
    CHECK(archives::read(path, archives::Scan::Quick, listing, error));
    const std::string out = work.path() + "out/";
    CHECK(run(path, listing, out).ok);
    CHECK(writeText(out + "pack/readme.txt", "changed"));

    archives::Result result = run(path, listing, out);
    CHECK(result.ok);
    CHECK_EQ(result.extracted, 0u);
    CHECK_EQ(result.skippedExisting.size(), static_cast<size_t>(3));

    result = run(path, listing, out, archives::Existing::Overwrite,
                 {"pack/readme.txt"});
    CHECK(result.ok);
    CHECK_EQ(result.extracted, 1u);
    CHECK_EQ(readText(out + "pack/readme.txt"),
             std::string("hello from an archive\n"));
}

TEST("archives: a damaged tar header adds no empty entries")
{
    TempDir work;
    const std::string path = work.path() + "damaged.tar";
    CHECK(make(path, ARCHIVE_FORMAT_TAR_PAX_RESTRICTED, ARCHIVE_FILTER_NONE,
               {{"one.txt", std::string(600, '1'), false, ""},
                {"two.txt", std::string(600, '2'), false, ""},
                {"three.txt", std::string(600, '3'), false, ""}}));
    std::string data = readAll(path);
    // Spoil the second header's checksum: one.txt is a header and two
    // blocks of data.
    data[3 * 512 + 148] = 'x';
    {
        std::ofstream file(path, std::ios::binary | std::ios::trunc);
        file.write(data.data(), static_cast<std::streamsize>(data.size()));
    }
    // libarchive answers ARCHIVE_RETRY with no name until it finds
    // three.txt's header.
    archives::Listing listing;
    std::string error;
    CHECK(archives::read(path, archives::Scan::Full, listing, error));
    CHECK_EQ(listing.fileCount, 2u);
    for (const archives::Entry &entry : listing.entries) {
        CHECK(!entry.path.empty());
    }
}

TEST("archives: a stopped read is not a read")
{
    Quiet stopped;
    stopped.cancelAfterCalls = 0;
    archives::Listing listing;
    std::string error = "not cleared";
    CHECK(!archives::read(fixture("basic.zip"), archives::Scan::Quick, listing,
                          error, &stopped));
    CHECK(error.empty());

    TempDir folder;
    CHECK(archives::read(fixture("basic.zip"), archives::Scan::Quick, listing,
                         error));
    std::vector<std::string> existing;
    Quiet checking;
    checking.cancelAfterCalls = 0;
    // Fewer than 64 files: it finishes before the first check.
    CHECK(archives::existingFiles(listing, folder.path(), existing, &checking));
}

TEST("archives: what a Mac adds is left out")
{
    CHECK(archives::macMetadata("__MACOSX/a/._b.txt"));
    CHECK(archives::macMetadata("pack/._readme.txt"));
    CHECK(archives::macMetadata("pack/.DS_Store"));
    CHECK(!archives::macMetadata("pack/readme.txt"));
    CHECK(!archives::macMetadata("pack/__MACOSX.txt"));

    TempDir work;
    const std::string path = work.path() + "mac.tar.gz";
    CHECK(make(path, ARCHIVE_FORMAT_TAR_PAX_RESTRICTED, ARCHIVE_FILTER_GZIP,
               {{"pack/readme.txt", "hello\n", false, ""},
                {"pack/._readme.txt", "junk", false, ""},
                {"pack/.DS_Store", "junk", false, ""},
                {"__MACOSX/pack/._readme.txt", "junk", false, ""}}));
    archives::Listing listing;
    std::string error;
    CHECK(archives::read(path, archives::Scan::Full, listing, error));
    CHECK_EQ(listing.fileCount, 1u);
    CHECK_EQ(listing.top, std::string("pack"));
    const archives::Result result = run(path, listing, work.path() + "out/");
    CHECK(result.ok);
    CHECK_EQ(result.extracted, 1u);
    CHECK(!fs::fileExists(work.path() + "out/pack/._readme.txt"));
    CHECK(!fs::directoryExists(work.path() + "out/__MACOSX"));
}

TEST("archives: a file the listing missed is never written over")
{
    TempDir folder;
    {
        archives::Writer writer(folder.path(), archives::Existing::Skip);
        CHECK(writer.makeRoot());
        // The folder is read here, before late.txt exists.
        CHECK(writer.start("first.txt", 1) == archives::Writer::Start::Write);
        CHECK(writer.write("1", 1) && writer.finish());
        CHECK(writeText(folder.path() + "late.txt", "mine"));
        CHECK(writer.start("late.txt", 6) == archives::Writer::Start::Skip);
    }
    CHECK_EQ(readText(folder.path() + "late.txt"), std::string("mine"));

    {
        archives::Writer writer(folder.path(), archives::Existing::Overwrite);
        CHECK(writer.makeRoot());
        CHECK(writer.start("first.txt", 1) == archives::Writer::Start::Write);
        CHECK(writer.write("2", 1) && writer.finish());
        CHECK(writeText(folder.path() + "later.txt", "mine"));
        CHECK(writer.start("later.txt", 6) == archives::Writer::Start::Write);
        CHECK(writer.write("dam", 3));
        writer.abandon(); // the entry turned out damaged
    }
    CHECK_EQ(readText(folder.path() + "later.txt"), std::string("mine"));
    CHECK(!fs::fileExists(folder.path() + "later.txt.3dsdl-tmp"));

    // Large files go through a temporary name, so they are looked up.
    {
        archives::Writer writer(folder.path(), archives::Existing::Skip);
        CHECK(writer.makeRoot());
        CHECK(writer.start("first.txt", 1) == archives::Writer::Start::Skip);
        CHECK(writeText(folder.path() + "late.bin", "mine"));
        CHECK(writer.start("late.bin", 300 * 1024) ==
              archives::Writer::Start::Skip);
    }
    CHECK_EQ(readText(folder.path() + "late.bin"), std::string("mine"));
}

TEST("archives: a damaged second copy keeps the first, folders stay put")
{
    TempDir folder;
    if (!foldsCase(folder.path())) {
        check::fail(__FILE__, __LINE__, "TMPDIR must ignore case, as FAT does");
        return;
    }
    archives::Writer writer(folder.path(), archives::Existing::Overwrite);
    CHECK(writer.makeRoot());
    CHECK(writer.start("a.txt", 5) == archives::Writer::Start::Write);
    CHECK(writer.write("first", 5) && writer.finish());
    CHECK(writer.start("a.txt", 6) == archives::Writer::Start::Write);
    CHECK(writer.write("sec", 3));
    writer.abandon();
    CHECK_EQ(readText(folder.path() + "a.txt"), std::string("first"));

    // A file entry named like a folder on the SD card leaves the folder be,
    // whether the listing saw the folder or not.
    CHECK(fs::makeDirectories(folder.path() + "Game/"));
    CHECK(writeText(folder.path() + "Game/save.dat", "save"));
    CHECK(writer.start("game", 3) == archives::Writer::Start::Skip);
    {
        archives::Writer listed(folder.path(), archives::Existing::Overwrite);
        CHECK(listed.makeRoot());
        CHECK(listed.start("game", 3) == archives::Writer::Start::Skip);
        CHECK(listed.start("game", 300 * 1024) ==
              archives::Writer::Start::Skip);
    }
    CHECK_EQ(readText(folder.path() + "Game/save.dat"), std::string("save"));
    CHECK(!fs::fileExists(folder.path() + "game.3dsdl-old"));

    // An old copy left by a replace that was cut short is cleared.
    CHECK(writeText(folder.path() + "a.txt.3dsdl-old", "stale"));
    CHECK(writer.start("a.txt", 5) == archives::Writer::Start::Write);
    CHECK(writer.write("third", 5) && writer.finish());
    CHECK_EQ(readText(folder.path() + "a.txt"), std::string("third"));
    CHECK(!fs::fileExists(folder.path() + "a.txt.3dsdl-old"));
}

TEST("archives: a large file only takes its name once whole")
{
    TempDir folder;
    archives::Writer writer(folder.path(), archives::Existing::Skip);
    CHECK(writer.makeRoot());
    const std::string data(300 * 1024, 'x');
    CHECK(writer.start("big.bin", static_cast<int64_t>(data.size())) ==
          archives::Writer::Start::Write);
    CHECK(writer.write(data.data(), data.size()));
    CHECK(!fs::fileExists(folder.path() + "big.bin"));
    CHECK(writer.finish());
    CHECK_EQ(fs::fileSize(folder.path() + "big.bin"),
             static_cast<int64_t>(data.size()));
    CHECK(!fs::fileExists(folder.path() + "big.bin.3dsdl-tmp"));

    CHECK(writer.start("huge.bin", 5ll << 30) ==
          archives::Writer::Start::Failed);
    CHECK(writer.error().find("4 GiB") != std::string::npos);
}

TEST("archives: space needed counts whole clusters and folders")
{
    archives::Listing listing;
    archives::Entry small;
    small.path = "a/b/one.txt";
    small.size = 1;
    archives::Entry big;
    big.path = "a/two.bin";
    big.size = 5000;
    archives::Entry skipped;
    skipped.path = "";
    skipped.size = 1u << 30;
    listing.entries = {small, big, skipped};
    // 1 cluster + 2 clusters for the files, and folders a and a/b.
    CHECK_EQ(archives::spaceNeeded(listing, 4096),
             static_cast<uint64_t>(5 * 4096));
}

TEST("archives: place moves entries or leaves them out, in both readers")
{
    // 3dsDL's own ZIP reader.
    TempDir zip;
    archives::Listing listing;
    std::string error;
    CHECK(archives::read(fixture("basic.zip"), archives::Scan::Quick, listing,
                         error));
    CHECK(listing.ownZip);
    archives::Result result =
        runUnder(fixture("basic.zip"), listing, zip.path(), "3ds/");
    CHECK(result.ok);
    CHECK_EQ(result.extracted, 1u);
    CHECK_EQ(result.skippedPlaced, 2u); // readme.txt and empty.txt
    CHECK_EQ(readAll(zip.path() + "App/App.3dsx"), binary());
    CHECK(!fs::fileExists(zip.path() + "readme.txt"));
    CHECK(!fs::directoryExists(zip.path() + "3ds"));

    // The listing as it would be extracted, for what the console shows.
    const archives::Listing moved =
        archives::placed(listing, [](const std::string &entry, bool) {
            return entry.rfind("3ds/", 0) == 0 ? entry.substr(4) : std::string();
        });
    CHECK_EQ(moved.fileCount, 1u);
    CHECK_EQ(moved.totalSize, static_cast<uint64_t>(binary().size()));
    CHECK_EQ(moved.top, std::string("App"));
    CHECK(moved.topIsFolder);
    CHECK(moved.complete);
    CHECK(!moved.ownZip); // extracting goes by the archive's own listing

    // libarchive, with a tar.
    TempDir tar;
    const std::string path = tar.path() + "pack.tar";
    CHECK(make(path, ARCHIVE_FORMAT_TAR_USTAR, ARCHIVE_FILTER_NONE, pack()));
    CHECK(archives::read(path, archives::Scan::Full, listing, error));
    TempDir out;
    result = runUnder(path, listing, out.path(), "pack/");
    CHECK(result.ok);
    CHECK_EQ(result.extracted, 3u);
    CHECK_EQ(result.skippedPlaced, 0u);
    CHECK_EQ(readText(out.path() + "readme.txt"),
             std::string("hello from an archive\n"));
    CHECK(fs::directoryExists(out.path() + "empty"));
    CHECK(!fs::directoryExists(out.path() + "pack"));
}

TEST("archives: place sees the name a ZIP's entry is written under")
{
    TempDir folder;
    archives::Listing listing;
    std::string error;
    CHECK(archives::read(fixture("renamed.zip"), archives::Scan::Quick,
                         listing, error));
    CHECK(!listing.ownZip);
    // The listing has the central directory's names; libarchive reads the
    // local header's, 3ds/a/zzzz.3dsx, for the first entry.
    CHECK_EQ(listing.entries.size(), static_cast<size_t>(2));
    CHECK_EQ(listing.entries[0].path, std::string("3ds/a/abcd.3dsx"));

    std::vector<std::string> listed;
    for (const archives::Entry &entry : listing.entries) {
        listed.push_back(entry.path);
    }
    archives::Result result = run(fixture("renamed.zip"), listing,
                                  folder.path(), archives::Existing::Skip,
                                  listed);
    CHECK(result.ok);
    CHECK_EQ(result.extracted, 1u);
    CHECK(!fs::fileExists(folder.path() + "3ds/a/zzzz.3dsx"));
    CHECK(!fs::fileExists(folder.path() + "3ds/a/abcd.3dsx"));
    CHECK_EQ(readText(folder.path() + "3ds/a/bz.txt"),
             std::string("packed with bzip2\n"));

    // place alone sees only the local header's name, so it would let one
    // the listing never showed through: why a sender's ZIP also sets only.
    TempDir placed;
    result = runUnder(fixture("renamed.zip"), listing, placed.path(), "3ds/");
    CHECK(result.ok);
    CHECK_EQ(result.extracted, 2u);
    CHECK(fs::fileExists(placed.path() + "a/zzzz.3dsx"));
    CHECK_EQ(readText(placed.path() + "a/bz.txt"),
             std::string("packed with bzip2\n"));
}

TEST("archives: a sender's archive is extracted into its one folder only")
{
    Quiet observer;
    archives::Listing listing;
    archives::Options options;
    archives::Listing shown;
    std::string error;

    // Laid out from the card's root: its 3ds folder's app, nothing else.
    CHECK(archives::read(fixture("basic.zip"), archives::Scan::Full, listing,
                         error));
    CHECK(remote::sentExtraction(listing, "basic.zip", options, shown));
    CHECK(options.only.empty()); // 3dsDL's own reader
    CHECK_EQ(shown.top, std::string("App"));
    CHECK_EQ(shown.fileCount, 1u);
    TempDir root;
    archives::Result result = archives::extract(fixture("basic.zip"), listing,
                                                root.path(), options, observer);
    CHECK(result.ok);
    CHECK_EQ(result.extracted, 1u);
    CHECK_EQ(result.skippedPlaced, 2u);
    CHECK_EQ(readAll(root.path() + "App/App.3dsx"), binary());

    // One folder at its top: that folder.
    CHECK(archives::read(fixture("rooted.zip"), archives::Scan::Full, listing,
                         error));
    options = archives::Options();
    CHECK(remote::sentExtraction(listing, "rooted.zip", options, shown));
    CHECK_EQ(shown.top, std::string("Game"));
    CHECK_EQ(shown.fileCount, 2u);

    // libarchive's names held to the listing's, and placed as well.
    CHECK(archives::read(fixture("renamed.zip"), archives::Scan::Full,
                         listing, error));
    options = archives::Options();
    CHECK(remote::sentExtraction(listing, "renamed.zip", options, shown));
    CHECK_EQ(options.only.size(), static_cast<size_t>(2));
    TempDir renamed;
    result = archives::extract(fixture("renamed.zip"), listing,
                               renamed.path(), options, observer);
    CHECK(result.ok);
    CHECK_EQ(result.extracted, 1u);
    CHECK(!fs::fileExists(renamed.path() + "a/zzzz.3dsx"));
    CHECK(!fs::fileExists(renamed.path() + "a/abcd.3dsx"));
    CHECK_EQ(readText(renamed.path() + "a/bz.txt"),
             std::string("packed with bzip2\n"));

    // Two apps in its 3ds folder: not extracted at all.
    TempDir made;
    const std::string two = made.path() + "two.zip";
    CHECK(make(two, ARCHIVE_FORMAT_ZIP, ARCHIVE_FILTER_NONE,
               {{"3ds/A/a.3dsx", "a", false, ""},
                {"3ds/B/b.3dsx", "b", false, ""}}));
    CHECK(archives::read(two, archives::Scan::Full, listing, error));
    options = archives::Options();
    CHECK(!remote::sentExtraction(listing, "two.zip", options, shown));
}
