#include "../check.hpp"
#include "../support.hpp"

#include "finalize.hpp"
#include "fs_util.hpp"
#include "remote.hpp"
#include "text.hpp"

#include <sys/stat.h>

TEST("fs: directories, sizes and atomic writes")
{
    TempDir temp;
    CHECK(fs::directoryExists(temp.path()));
    CHECK(fs::makeDirectories(temp.path() + "a/b/c/"));
    CHECK(fs::directoryExists(temp.path() + "a/b/c"));
    CHECK(fs::makeDirectories(temp.path() + "a/b/c/")); // already there

    CHECK(fs::writeFile(temp.path() + "x.txt", "hello"));
    CHECK(fs::writeFile(temp.path() + "x.txt", "replaced"));
    std::string contents;
    CHECK(fs::readFile(temp.path() + "x.txt", contents));
    CHECK_EQ(contents, std::string("replaced"));
    CHECK_EQ(fs::fileSize(temp.path() + "x.txt"), static_cast<int64_t>(8));
    CHECK(!fs::fileExists(temp.path() + "x.txt.tmp"));
    CHECK(fs::fileExists(temp.path() + "x.txt"));
    CHECK(!fs::fileExists(temp.path() + "a"));
    CHECK_EQ(fs::fileSize(temp.path() + "missing"), static_cast<int64_t>(-1));
}

TEST("fs: folder listing is sorted and skips files and hidden entries")
{
    TempDir temp;
    CHECK(fs::makeDirectories(temp.path() + "beta/"));
    CHECK(fs::makeDirectories(temp.path() + "Alpha/"));
    CHECK(fs::makeDirectories(temp.path() + ".hidden/"));
    CHECK(writeText(temp.path() + "file.txt", "x"));
    const std::vector<std::string> folders = fs::listFolders(temp.path());
    CHECK_EQ(folders.size(), static_cast<size_t>(2));
    if (folders.size() == 2) {
        CHECK_EQ(folders[0], std::string("Alpha"));
        CHECK_EQ(folders[1], std::string("beta"));
    }
}

TEST("finalize: new file, rename on conflict and overwrite")
{
    TempDir temp;
    const std::string part = temp.path() + ".3dsdl-x.part";

    CHECK(writeText(part, "one"));
    finalize::Result result =
        finalize::commit(part, temp.path(), "f.zip", false, 1);
    CHECK(result.ok);
    CHECK_EQ(result.path, temp.path() + "f.zip");

    CHECK(writeText(part, "two"));
    result = finalize::commit(part, temp.path(), "f.zip", false, 1);
    CHECK(result.ok);
    CHECK_EQ(result.path, temp.path() + "f (1).zip");
    CHECK_EQ(readText(temp.path() + "f.zip"), std::string("one"));

    CHECK(writeText(part, "three"));
    result = finalize::commit(part, temp.path(), "f.zip", true, 1);
    CHECK(result.ok);
    CHECK_EQ(readText(temp.path() + "f.zip"), std::string("three"));
    CHECK(!fs::fileExists(temp.path() + "f.zip.3dsdl-backup"));
    CHECK(!fs::fileExists(part));
}

TEST("finalize: a download kept beside an older copy can replace it later")
{
    TempDir temp;
    CHECK(writeText(temp.path() + "f.3dsx", "old"));
    CHECK(writeText(temp.path() + "f (1).3dsx", "new"));
    CHECK_EQ(finalize::keptBeside(temp.path() + "f (1).3dsx", "f.3dsx"),
             temp.path() + "f.3dsx");
    // No clash, or nothing there any more: nothing to ask about.
    CHECK_EQ(finalize::keptBeside(temp.path() + "f.3dsx", "f.3dsx"),
             std::string(""));
    CHECK_EQ(finalize::keptBeside(temp.path() + "g (1).3dsx", "g.3dsx"),
             std::string(""));
    CHECK_EQ(finalize::keptBeside(temp.path() + "f (1).3dsx", ""),
             std::string(""));
    // FAT ignores case, and so does the test folder.
    CHECK_EQ(finalize::keptBeside(temp.path() + "f (1).3dsx", "F.3DSX"),
             temp.path() + "F.3DSX");

    // What a sender's replace=1 may ask about.
    const std::string at = temp.path();
    CHECK(writeText(at + "config.json", "old"));
    CHECK(writeText(at + "config (1).json", "new"));
    CHECK_EQ(remote::replaceCandidate(at + "f (1).3dsx", "f.3dsx", at, {}),
             at + "f.3dsx");
    // Not in the folder the sender named, not an app, no clash, or a file
    // this same send saved.
    CHECK_EQ(remote::replaceCandidate(at + "f (1).3dsx", "f.3dsx", at + "x/", {}),
             std::string(""));
    CHECK_EQ(remote::replaceCandidate(at + "config (1).json", "config.json", at,
                                      {}),
             std::string(""));
    CHECK_EQ(remote::replaceCandidate(at + "f.3dsx", "f.3dsx", at, {}),
             std::string(""));
    CHECK_EQ(remote::replaceCandidate(at + "f (1).3dsx", "f.3dsx", at,
                                      {text::lower(at + "F.3DSX")}),
             std::string(""));

    CHECK(finalize::replace(temp.path() + "f (1).3dsx", temp.path() + "f.3dsx"));
    CHECK_EQ(readText(temp.path() + "f.3dsx"), std::string("new"));
    CHECK(!fs::fileExists(temp.path() + "f (1).3dsx"));
    CHECK(!fs::fileExists(temp.path() + "f.3dsx.3dsdl-backup"));

    // Nothing to move: the old file stays.
    CHECK(!finalize::replace(temp.path() + "gone.3dsx", temp.path() + "f.3dsx"));
    CHECK_EQ(readText(temp.path() + "f.3dsx"), std::string("new"));

    // No older file any more: a plain move.
    CHECK(writeText(temp.path() + "g (1).3dsx", "only"));
    CHECK(finalize::replace(temp.path() + "g (1).3dsx", temp.path() + "g.3dsx"));
    CHECK_EQ(readText(temp.path() + "g.3dsx"), std::string("only"));
    CHECK(!fs::fileExists(temp.path() + "g (1).3dsx"));
}

TEST("finalize: a rejected name falls back instead of losing the data")
{
    TempDir temp;
    const std::string part = temp.path() + ".3dsdl-y.part";
    CHECK(writeText(part, "payload"));

    // A name longer than any host file system allows stands in for a name
    // the SD card refuses.
    // The first fallback is the sanitised ASCII name, which is also short.
    const std::string tooLong(300, 'n');
    const finalize::Result result =
        finalize::commit(part, temp.path(), tooLong + ".zip", false, 77);
    CHECK(result.ok);
    CHECK(result.usedFallbackName);
    CHECK(result.path.size() < temp.path().size() + 200);
    CHECK(result.path.compare(result.path.size() - 4, 4, ".zip") == 0);
    CHECK_EQ(readText(result.path), std::string("payload"));
    CHECK(!fs::fileExists(part));
}

TEST("finalize: data stays in the part file when nothing works")
{
    TempDir temp;
    const std::string part = temp.path() + ".3dsdl-z.part";
    CHECK(writeText(part, "kept"));
    const finalize::Result result = finalize::commit(
        part, temp.path() + "missing-folder/", "f.zip", false, 1);
    CHECK(!result.ok);
    CHECK_EQ(result.path, part);
    CHECK_EQ(readText(part), std::string("kept"));
}
