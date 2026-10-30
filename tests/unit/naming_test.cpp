#include "../check.hpp"

#include "naming.hpp"
#include "text.hpp"

#include <set>

TEST("naming: safe filenames strip paths and reserved characters")
{
    CHECK_EQ(naming::safeFilename("../../etc/passwd"), std::string("passwd"));
    CHECK_EQ(naming::safeFilename("C:\\games\\x.cia"), std::string("x.cia"));
    CHECK_EQ(naming::safeFilename("a:b*c?d\"e<f>g|h"),
             std::string("a_b_c_d_e_f_g_h"));
    CHECK_EQ(naming::safeFilename("tab\there"), std::string("tab_here"));
    CHECK_EQ(naming::safeFilename("  name.zip .. "), std::string("name.zip"));
    CHECK_EQ(naming::safeFilename(""), std::string("download.bin"));
    CHECK_EQ(naming::safeFilename(".."), std::string("download.bin"));
    CHECK_EQ(naming::safeFilename("dir/"), std::string("download.bin"));
    CHECK_EQ(naming::safeFilename(".3dsdl-0011.part"),
             std::string("_.3dsdl-0011.part"));
}

TEST("naming: invalid UTF-8 is replaced instead of reaching the SD card")
{
    CHECK_EQ(naming::safeFilename("caf\xe9.txt"), std::string("caf_.txt"));
    CHECK(text::isValidUtf8(naming::safeFilename("\xff\xfe.bin")));
}

TEST("naming: long names are cut on a character boundary, keeping extension")
{
    std::string longName;
    for (int index = 0; index < 100; ++index) {
        longName += "\xe2\x82\xac"; // 300 bytes of euro signs
    }
    const std::string safe = naming::safeFilename(longName + ".zip");
    CHECK(safe.size() <= naming::MaxFilenameBytes);
    CHECK(text::isValidUtf8(safe));
    CHECK(text::endsWithNoCase(safe, ".zip"));

    const std::string noExtension = naming::safeFilename(longName);
    CHECK(noExtension.size() <= naming::MaxFilenameBytes);
    CHECK(text::isValidUtf8(noExtension));
}

TEST("naming: ASCII fallback")
{
    CHECK_EQ(naming::asciiFilename("na\xc3\xafve.zip"), std::string("na_ve.zip"));
    CHECK_EQ(naming::asciiFilename("plain.zip"), std::string("plain.zip"));
}

TEST("naming: Content-Disposition parsing")
{
    using naming::contentDispositionFilename;
    CHECK_EQ(contentDispositionFilename("attachment; filename=\"a b.zip\""),
             std::string("a b.zip"));
    CHECK_EQ(contentDispositionFilename("attachment; filename=plain.zip"),
             std::string("plain.zip"));
    CHECK_EQ(contentDispositionFilename(
                 "attachment; filename=\"fallback.zip\"; "
                 "filename*=UTF-8''na%C3%AFve.zip"),
             std::string("na\xc3\xafve.zip"));
    CHECK_EQ(contentDispositionFilename(
                 "attachment; filename*=iso-8859-1'en'caf%E9.txt"),
             std::string("caf\xc3\xa9.txt"));
    // Unknown charsets fall back to the plain parameter.
    CHECK_EQ(contentDispositionFilename(
                 "attachment; filename*=koi8-r''x; filename=\"y.txt\""),
             std::string("y.txt"));
    // Semicolons and escaped quotes inside quoted strings are kept.
    CHECK_EQ(contentDispositionFilename("attachment; filename=\"a;b.zip\""),
             std::string("a;b.zip"));
    CHECK_EQ(contentDispositionFilename(
                 "attachment; filename=\"say \\\"hi\\\".txt\""),
             std::string("say \"hi\".txt"));
    // Plain filename values are not percent-decoded (RFC 6266).
    CHECK_EQ(contentDispositionFilename("attachment; filename=\"50%25.txt\""),
             std::string("50%25.txt"));
    // A parameter that merely ends in "filename" is not the filename.
    CHECK_EQ(contentDispositionFilename("attachment; xfilename=\"x.txt\""),
             std::string(""));
    CHECK_EQ(contentDispositionFilename("inline"), std::string(""));
    CHECK_EQ(contentDispositionFilename("filename=bare.bin"),
             std::string("bare.bin"));
}

TEST("naming: chooseFilename prefers the header, then the URL")
{
    CHECK_EQ(naming::chooseFilename("attachment; filename=\"h.zip\"",
                                    "https://x/u.zip"),
             std::string("h.zip"));
    CHECK_EQ(naming::chooseFilename("", "https://x/dir/my%20file.zip?x=1"),
             std::string("my file.zip"));
    CHECK_EQ(naming::chooseFilename("", "https://x/a%2F..%2Fb.zip"),
             std::string("b.zip"));
    CHECK_EQ(naming::chooseFilename("", "https://x/"),
             std::string("download.bin"));
    CHECK_EQ(naming::chooseFilename("attachment", "https://x/f.3dsx"),
             std::string("f.3dsx"));
}

TEST("naming: stem, extension and unused paths")
{
    CHECK_EQ(naming::stem("a.tar.gz"), std::string("a.tar"));
    CHECK_EQ(naming::extension("a.tar.gz"), std::string(".gz"));
    CHECK_EQ(naming::extension(".hidden"), std::string(""));
    CHECK_EQ(naming::stem(".hidden"), std::string(".hidden"));

    std::set<std::string> taken = {"d/f.zip", "d/f (1).zip"};
    const auto exists = [&](const std::string &path) {
        return taken.count(path) > 0;
    };
    CHECK_EQ(naming::unusedPath("d/", "g.zip", exists), std::string("d/g.zip"));
    CHECK_EQ(naming::unusedPath("d/", "f.zip", exists),
             std::string("d/f (2).zip"));
    CHECK_EQ(naming::unusedPath("d/", "f.zip", exists, 1), std::string(""));
}

TEST("naming: folders")
{
    CHECK_EQ(naming::normaliseFolder(" sdmc:/a "), std::string("sdmc:/a/"));
    CHECK_EQ(naming::normaliseFolder(""), std::string("sdmc:/"));
    CHECK(naming::isSdPath("SDMC:/x"));
    CHECK(!naming::isSdPath("romfs:/x"));
    CHECK_EQ(naming::parentFolder("sdmc:/a/b/"), std::string("sdmc:/a/"));
    CHECK_EQ(naming::parentFolder("sdmc:/a/"), std::string("sdmc:/"));
    CHECK_EQ(naming::parentFolder("sdmc:/"), std::string("sdmc:/"));
    CHECK_EQ(naming::parentFolder("/tmp/x/"), std::string("/tmp/"));
}

TEST("naming: an archive's folder name drops .tar too")
{
    CHECK_EQ(naming::archiveStem("pack.zip"), std::string("pack"));
    CHECK_EQ(naming::archiveStem("pack.tar.gz"), std::string("pack"));
    CHECK_EQ(naming::archiveStem("Pack.TAR.XZ"), std::string("Pack"));
    CHECK_EQ(naming::archiveStem("pack.tgz"), std::string("pack"));
    CHECK_EQ(naming::archiveStem("v1.2.7z"), std::string("v1.2"));
}

TEST("naming: a long name cut short never ends in a space or dot")
{
    const std::string spaced = "3dsDL.3dsx" + std::string(200, ' ') + "x";
    const std::string safe = naming::safeFilename(spaced);
    CHECK(!safe.empty());
    CHECK(safe.back() != ' ' && safe.back() != '.');
    const std::string dotted = std::string(170, 'a') + std::string(40, '.') + "b";
    const std::string cut = naming::safeFilename(dotted);
    CHECK(cut.back() != ' ' && cut.back() != '.');
}
