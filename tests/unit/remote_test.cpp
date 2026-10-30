#include "../check.hpp"

#include "archives.hpp"
#include "remote.hpp"

#include <string>
#include <vector>

TEST("remote: finds links in pasted text and FBI payloads")
{
    const std::vector<std::string> links = remote::extractLinks(
        "Get it here: https://example.com/a.cia, and (http://h/b.3dsx).\n"
        "https://example.com/a.cia\r\nftp://nope/c.zip  not-a-link\n"
        "<https://x.org/d%20e.zip>");
    CHECK_EQ(links.size(), static_cast<size_t>(3));
    CHECK_EQ(links[0], std::string("https://example.com/a.cia"));
    CHECK_EQ(links[1], std::string("http://h/b.3dsx"));
    CHECK_EQ(links[2], std::string("https://x.org/d%20e.zip"));
    CHECK(remote::extractLinks("   \n").empty());

    const unsigned char length[4] = {0x00, 0x01, 0x02, 0x03};
    CHECK_EQ(remote::payloadLength(length), static_cast<size_t>(0x010203));
}

TEST("remote: parses web requests as they arrive")
{
    remote::HttpRequest request;
    const std::string body = "links=https%3A%2F%2Fa.b%2Fc.cia+https%3A%2F%2Fd.e%2Ff";
    const std::string full = "POST /send?x=1 HTTP/1.1\r\nHost: 3ds\r\n"
                             "Content-Type: application/x-www-form-urlencoded\r\n"
                             "content-length: " +
                             std::to_string(body.size()) + "\r\n\r\n" + body;
    CHECK(remote::parseRequest(full.substr(0, 20), request) ==
          remote::Parse::Incomplete);
    CHECK(remote::parseRequest(full.substr(0, full.size() - 3), request) ==
          remote::Parse::Incomplete);
    CHECK(remote::parseRequest(full, request) == remote::Parse::Done);
    CHECK_EQ(request.method, std::string("POST"));
    CHECK_EQ(request.path, std::string("/send"));
    CHECK_EQ(request.query, std::string("x=1"));
    CHECK_EQ(request.headers["host"], std::string("3ds"));
    CHECK_EQ(remote::formValue(request.body, "links"),
             std::string("https://a.b/c.cia https://d.e/f"));
    CHECK_EQ(remote::formValue(request.body, "missing"), std::string(""));

    CHECK(remote::parseRequest("GET / HTTP/1.1\r\n\r\n", request) ==
          remote::Parse::Done);
    CHECK(remote::parseRequest("nonsense\r\n\r\n", request) ==
          remote::Parse::Bad);
    CHECK(remote::parseRequest("GET / HTTP/1.1\r\nContent-Length: x\r\n\r\n",
                               request) == remote::Parse::Bad);
    CHECK(remote::parseRequest("POST / HTTP/1.1\r\nContent-Length: 99999\r\n\r\n",
                               request, 100) == remote::Parse::Bad);
}

TEST("remote: responses and the page")
{
    const std::string response = remote::httpResponse(404, "text/plain", "no");
    CHECK(response.rfind("HTTP/1.1 404 Not Found\r\n", 0) == 0);
    CHECK(response.find("Content-Length: 2\r\n") != std::string::npos);
    CHECK(response.find("\r\n\r\nno") != std::string::npos);

    const std::string html = remote::page("Sent <2> links", true);
    CHECK(html.find("action=\"/send\"") != std::string::npos);
    CHECK(html.find("Sent &lt;2&gt; links") != std::string::npos);
    CHECK(html.find("class=\"note good\"") != std::string::npos);
    CHECK(remote::page().find("class=\"note") == std::string::npos);
}

TEST("remote: heads of large uploads, without their bodies")
{
    remote::HttpRequest request;
    size_t bodyStart = 0;
    const std::string head = "PUT /api/file?path=%2Fa.cia HTTP/1.1\r\n"
                             "Content-Length: 3000000000\r\n\r\nabc";
    CHECK(remote::parseHead(head, request, bodyStart) == remote::Parse::Done);
    CHECK_EQ(request.contentLength, static_cast<uint64_t>(3000000000ull));
    CHECK_EQ(head.substr(bodyStart), std::string("abc"));
    // A whole body that large is never buffered.
    CHECK(remote::parseRequest(head, request) == remote::Parse::Bad);
    // 4 GiB does not fit on FAT32.
    CHECK(remote::parseHead("PUT / HTTP/1.1\r\nContent-Length: 4294967296\r\n\r\n",
                            request, bodyStart) == remote::Parse::Bad);
}

TEST("remote: paths from the page stay on the SD card")
{
    std::string path;
    CHECK(remote::localPath("sdmc:", "/", path));
    CHECK_EQ(path, std::string("sdmc:/"));
    CHECK(remote::localPath("sdmc:", "/3ds/3dsDL", path));
    CHECK_EQ(path, std::string("sdmc:/3ds/3dsDL"));
    CHECK(remote::localPath("sdmc:", "/cia/", path));
    CHECK_EQ(path, std::string("sdmc:/cia"));
    CHECK(remote::localPath("sdmc:", "/Nintendo 3DS/a b.cia", path));
    CHECK_EQ(path, std::string("sdmc:/Nintendo 3DS/a b.cia"));
    for (const char *bad : {"", "cia", "/..", "/a/../..", "/a/./b", "/a//b",
                            "/a\\b", "/c:", "/ a", "/a ", "/a."}) {
        CHECK(!remote::localPath("sdmc:", bad, path));
    }
}

TEST("remote: a sender can name a folder for apps or CIAs, and ask to replace")
{
    remote::SendOptions options;
    std::string why;
    CHECK(remote::sendOptions("links=https%3A%2F%2Fa.b%2Fc.3dsx", options, why));
    CHECK_EQ(options.folder, std::string(""));
    CHECK(!options.replace);
    // An empty folder is the same as none.
    CHECK(remote::sendOptions("folder=&links=x", options, why));
    CHECK_EQ(options.folder, std::string(""));

    CHECK(remote::sendOptions("links=x&folder=%2F3ds%2F&replace=1", options, why));
    CHECK_EQ(options.folder, std::string("sdmc:/3ds/"));
    CHECK(options.replace);
    CHECK(!options.extract);
    CHECK(remote::sendOptions("links=x&extract=1", options, why));
    CHECK(options.extract);
    CHECK(remote::sendOptions("links=x&extract=yes", options, why));
    CHECK(!options.extract);
    CHECK(remote::sendOptions("folder=%2F3DS%2FChecker+Board", options, why));
    CHECK_EQ(options.folder, std::string("sdmc:/3DS/Checker Board/"));
    CHECK(remote::sendOptions("folder=%2Fcias", options, why));
    CHECK_EQ(options.folder, std::string("sdmc:/cias/"));
    // Only "1" turns it on.
    CHECK(remote::sendOptions("replace=yes", options, why));
    CHECK(!options.replace);

    for (const char *bad :
         {"folder=%2F", "folder=3ds", "folder=sdmc%3A%2F3ds", "folder=%2F..%2F",
          "folder=%2F3ds%2F..%2Fluma", "folder=%2F3ds%2Fa%5Cb",
          "folder=%2F3ds%2F.%2F3dsDL", "folder=%2F3ds%2F3dsDL.",
          "folder=%2F3ds%2F3dsDL%20", "folder=%2F3ds%2F+3dsDL",
          "folder=%2F3ds%2Fa%00b", "folder=%2Fluma%2Fplugins",
          "folder=%2Fluma%2Fpayloads", "folder=%2FNintendo+3DS",
          "folder=%2F3dsx", "folder=%2F3ds%2F3dsDL", "folder=%2F3DS%2F3DSDL%2Fx",
          "folder=%2F3ds%2F3DSDL~1", "folder=%2F3ds%2F3d%C5%BFDL",
          "folder=%2Fciasx"}) {
        why.clear();
        CHECK(!remote::sendOptions(std::string("links=x&replace=1&") + bad,
                                   options, why));
        CHECK(!why.empty());
        CHECK_EQ(options.folder, std::string(""));
        CHECK(!options.replace);
    }

    CHECK(remote::sendFolderAllowed("sdmc:/3ds"));
    CHECK(remote::sendFolderAllowed("sdmc:/3ds/3dsDLx/"));
    CHECK(remote::sendFolderAllowed("sdmc:/cias/a~b"));
    CHECK(remote::sendFolderAllowed("sdmc:/cias/Games/"));
    for (const char *never : {"", "sdmc:/", "sdmc:/3ds/../luma", "sdmc:/3ds/./x",
                              "sdmc:/3ds//x", "sdmc:/cias/A~1"}) {
        CHECK(!remote::sendFolderAllowed(never));
    }
}

TEST("remote: names a sender's file may have, and what it may replace")
{
    CHECK(remote::sentNameAllowed("2048-port-3ds.3dsx"));
    CHECK(remote::sentNameAllowed("Checkpoint.cia"));
    CHECK(remote::sentNameAllowed("app~beta.3dsx"));
    CHECK(remote::sentNameAllowed("config.json"));
    for (const char *never : {"boot.3dsx", "Boot.3DSX", "3dsDL.3dsx",
                              "APP~1.CIA", "CHECKP~1.3DS", "A~B~C~1.TXT",
                              "3d\xc5\xbf" "DL.3dsx", ""}) {
        CHECK(!remote::sentNameAllowed(never));
    }

    CHECK(remote::sentMayReplace("2048-port-3ds.3dsx"));
    CHECK(remote::sentMayReplace("Checkpoint.CIA"));
    for (const char *never : {"config.json", "x.unistore", "App.smdh",
                              "3dsDL.3dsx", "boot.3dsx", "x.firm"}) {
        CHECK(!remote::sentMayReplace(never));
    }
}

TEST("remote: the file API only answers its own page")
{
    remote::HttpRequest request;
    std::string why;
    request.method = "GET";
    request.headers["host"] = "192.168.1.20";
    CHECK(remote::apiRequestAllowed(request, why));
    request.headers["host"] = "192.168.1.20:8080";
    CHECK(remote::apiRequestAllowed(request, why));
    request.headers["host"] = "3DS.local";
    CHECK(remote::apiRequestAllowed(request, why));
    // A name another site controls, pointed at the console.
    request.headers["host"] = "attacker.example";
    CHECK(!remote::apiRequestAllowed(request, why));
    request.headers["host"] = "1.2.3.4.attacker.example";
    CHECK(!remote::apiRequestAllowed(request, why));
    request.headers.erase("host");
    CHECK(!remote::apiRequestAllowed(request, why));

    request.headers["host"] = "192.168.1.20";
    request.method = "POST";
    CHECK(!remote::apiRequestAllowed(request, why));
    CHECK(!why.empty());
    request.headers["x-requested-with"] = "XMLHttpRequest";
    CHECK(!remote::apiRequestAllowed(request, why));
    request.headers["x-requested-with"] = "3dsDL";
    CHECK(remote::apiRequestAllowed(request, why));
}

TEST("remote: JSON text, file types and download names")
{
    CHECK_EQ(remote::jsonString("a\"b\\c\nd\x01"),
             std::string("\"a\\\"b\\\\c\\nd\\u0001\""));
    CHECK_EQ(remote::jsonString("日本"), std::string("\"日本\""));
    CHECK_EQ(std::string(remote::mimeType("a.PNG")), std::string("image/png"));
    CHECK_EQ(std::string(remote::mimeType("x.cia")),
             std::string("application/octet-stream"));
    // Never run as a page.
    CHECK(std::string(remote::mimeType("evil.html")).rfind("text/plain", 0) == 0);
    CHECK(std::string(remote::mimeType("evil.svg")).rfind("text/plain", 0) == 0);
    CHECK_EQ(remote::contentDisposition("a \"b\".cia", true),
             std::string("attachment; filename=\"a _b_.cia\"; "
                         "filename*=UTF-8''a%20%22b%22.cia"));
    CHECK(remote::contentDisposition("é.txt", false)
              .find("filename*=UTF-8''%C3%A9.txt") != std::string::npos);
    CHECK(remote::page().find("id=\"files\"") != std::string::npos);
}

TEST("remote: title icons as BMP pictures")
{
    std::vector<uint16_t> icon(48 * 48, 0);
    icon[0] = 0xF800;           // top left: red
    icon[47 * 48 + 47] = 0x07E0; // bottom right: green
    const std::string bmp = remote::iconBmp(icon);
    CHECK_EQ(bmp.size(), static_cast<size_t>(54 + 48 * 48 * 3));
    CHECK(bmp.compare(0, 2, "BM") == 0);
    // Rows go bottom up, in blue, green, red order.
    const size_t topLeft = 54 + 47 * 48 * 3;
    CHECK_EQ(static_cast<unsigned char>(bmp[topLeft + 2]), 0xFF);
    CHECK_EQ(static_cast<unsigned char>(bmp[topLeft]), 0x00);
    const size_t bottomRight = 54 + 47 * 3;
    CHECK_EQ(static_cast<unsigned char>(bmp[bottomRight + 1]), 0xFF);
    CHECK(remote::iconBmp(std::vector<uint16_t>(10)).empty());
}

TEST("remote: senders are named by their browsers")
{
    CHECK_EQ(remote::deviceName("Mozilla/5.0 (iPhone; CPU iPhone OS 18_0 like Mac "
                                "OS X) AppleWebKit/605.1.15 Mobile/15E148"),
             std::string("iPhone"));
    CHECK_EQ(remote::deviceName("Mozilla/5.0 (Linux; Android 15; Pixel 9) "
                                "AppleWebKit/537.36 Chrome/130.0 Mobile"),
             std::string("Android"));
    CHECK_EQ(remote::deviceName("Mozilla/5.0 (Macintosh; Intel Mac OS X 10_15_7) "
                                "AppleWebKit/605.1.15 Safari/605.1.15"),
             std::string("Mac"));
    CHECK_EQ(remote::deviceName("Mozilla/5.0 (Windows NT 10.0; Win64; x64)"),
             std::string("Windows PC"));
    CHECK_EQ(remote::deviceName("Mozilla/5.0 (X11; CrOS x86_64 14541.0.0)"),
             std::string("Chromebook"));
    CHECK_EQ(remote::deviceName("Mozilla/5.0 (X11; Linux x86_64; rv:131.0)"),
             std::string("Linux PC"));
    // The 3DS's own browser says it is like an iPhone.
    CHECK_EQ(remote::deviceName("Mozilla/5.0 (New Nintendo 3DS like iPhone) "
                                "AppleWebKit/536.30 NintendoBrowser/1.12"),
             std::string("3DS"));
    CHECK_EQ(remote::deviceName("curl/8.7.1"), std::string("curl"));
    CHECK_EQ(remote::deviceName(""), std::string(""));
    CHECK_EQ(remote::deviceName("python-requests/2.32"), std::string(""));
}

TEST("remote: where an upload stands in the page's batch")
{
    remote::HttpRequest request;
    remote::UploadBatch batch;
    CHECK(!remote::uploadBatch(request, batch));

    request.headers["x-3dsdl-batch"] = "k3x9 2 5 100 6000000000";
    CHECK(remote::uploadBatch(request, batch));
    CHECK_EQ(batch.id, std::string("k3x9"));
    CHECK_EQ(batch.position, static_cast<uint32_t>(2));
    CHECK_EQ(batch.count, static_cast<uint32_t>(5));
    CHECK_EQ(batch.before, static_cast<uint64_t>(100));
    CHECK_EQ(batch.bytes, static_cast<uint64_t>(6000000000ULL));
    CHECK(batch.folder.empty());

    request.headers["x-3dsdl-batch"] = "k3x9 3 5 0 9 4 %2F3ds%2FCheck%20point";
    CHECK(remote::uploadBatch(request, batch));
    CHECK_EQ(batch.folder, std::string("/3ds/Check point"));
    CHECK_EQ(batch.folderFiles, static_cast<uint32_t>(4));

    // Anything that does not add up is ignored rather than half read.
    for (const char *bad : {"k3x9 6 5 0 9", "k3x9 0 5 0 9", "k3x9 1 2", "K3X9 1 2 0 9",
                            "a 1 x 0 9", "a 1 2 9 3", "a 1 2 0 x", "a 1 2 0 9 3 %2Fx",
                            "a 1 2 0 9 1 x", "a 1 2 0 9 1", "", "a  1 2 0 9"}) {
        request.headers["x-3dsdl-batch"] = bad;
        CHECK(!remote::uploadBatch(request, batch));
    }
}

TEST("remote: a sender's archive goes into one folder in /3ds/")
{
    remote::SentArchive archive;
    // Laid out from the card's root, as most are: the app in its 3ds folder
    // goes into /3ds/ and the rest stays in the archive.
    CHECK(remote::sentArchive({"3ds", "3ds/App", "3ds/App/App.3dsx",
                               "3ds/App/data/level.bin", "App.cia",
                               "README.md"},
                              "anything.zip", archive));
    CHECK_EQ(archive.top, std::string("app"));
    CHECK_EQ(remote::sentArchivePlace(archive, "3ds/App/App.3dsx", false),
             std::string("App/App.3dsx"));
    CHECK_EQ(remote::sentArchivePlace(archive, "3DS/App", true),
             std::string("App"));
    for (const char *beside : {"3ds", "App.cia", "README.md",
                               "luma/plugins/default.3gx", "3dsx/a.3dsx",
                               "3ds/Other/x.3dsx"}) {
        CHECK_EQ(remote::sentArchivePlace(archive, beside, false),
                 std::string(""));
    }
    for (const char *never : {"3ds/App/payload.firm", "3ds/App/boot.3dsx",
                              "3ds/App/APP~1/x", "3ds/App/BOOT~1.FIR",
                              "3ds/App/3dsDL.3dsx",
                              "3ds/App/3d\xc5\xbf" "DL.3dsx"}) {
        CHECK_EQ(remote::sentArchivePlace(archive, never, false),
                 std::string(""));
    }
    // One file in its 3ds folder is fine too.
    CHECK(remote::sentArchive({"3ds/Top.3dsx", "cias/Top.cia"}, "t.zip",
                              archive));
    CHECK_EQ(remote::sentArchivePlace(archive, "3ds/Top.3dsx", false),
             std::string("Top.3dsx"));
    // Two things in it, nothing in it, or 3dsDL's own: not extracted.
    CHECK(!remote::sentArchive({"3ds/A/a.3dsx", "3ds/B/b.3dsx"}, "t.zip",
                               archive));
    CHECK(!remote::sentArchive({"3ds/A/a.3dsx", "3ds/a.3dsx"}, "t.zip",
                               archive));
    CHECK(!remote::sentArchive({"cias/x.cia", "luma/plugins/x.3gx"}, "t.zip",
                               archive));
    CHECK(!remote::sentArchive({"3ds/3dsDL/seeddb.bin"}, "t.zip", archive));
    CHECK(!remote::sentArchive({"3ds/3DSDL~1/x"}, "t.zip", archive));

    // One folder at its top: that folder, as it is.
    CHECK(remote::sentArchive({"Game", "Game/Game.3dsx", "Game/luma/x"},
                              "whatever.zip", archive));
    CHECK_EQ(remote::sentArchivePlace(archive, "Game/Game.3dsx", false),
             std::string("Game/Game.3dsx"));
    CHECK_EQ(remote::sentArchivePlace(archive, "Game/luma/x", false),
             std::string("Game/luma/x"));
    CHECK_EQ(remote::sentArchivePlace(archive, "Other/x", false),
             std::string(""));
    // Anything else: whole, into a folder of its name, made ASCII.
    CHECK(remote::sentArchive({"Game.3dsx", "data/level.bin"},
                              "Some Game v2.zip", archive));
    CHECK_EQ(remote::sentArchivePlace(archive, "Game.3dsx", false),
             std::string("Some Game v2/Game.3dsx"));
    CHECK_EQ(remote::sentArchivePlace(archive, "data", true),
             std::string("Some Game v2/data"));
    CHECK(remote::sentArchive({"a.bin"}, "Pok\xc3\xa9mon.zip", archive));
    CHECK_EQ(remote::sentArchivePlace(archive, "a.bin", false),
             std::string("Pok_mon/a.bin"));
    CHECK(!remote::sentArchive({"a.bin"}, "3dsDL.zip", archive));
    CHECK(!remote::sentArchive({"a.bin"}, "3DSDL~1.ZIP", archive));
    CHECK(!remote::sentArchive({"a.bin"}, ".zip", archive));
    // A folder of its own past ASCII is not renamed: not extracted.
    CHECK(!remote::sentArchive({"Pok\xc3\xa9mon/a.3dsx"}, "p.zip", archive));
}

TEST("remote: extracting a sender's archive, from its listing")
{
    archives::Listing listing;
    listing.format = "ZIP";
    listing.complete = true;
    for (const char *path : {"3ds/App", "3ds/App/App.3dsx", "App.cia"}) {
        archives::Entry entry;
        entry.path = path;
        entry.directory = std::string(path) == "3ds/App";
        entry.size = entry.directory ? 0 : 10;
        listing.entries.push_back(entry);
    }
    listing.fileCount = 2;
    listing.totalSize = 20;

    // libarchive reads this ZIP: held to the listing's names.
    archives::Options options;
    archives::Listing shown;
    CHECK(remote::sentExtraction(listing, "App.zip", options, shown));
    CHECK_EQ(options.only.size(), static_cast<size_t>(3));
    CHECK_EQ(options.place("3ds/App/App.3dsx", false),
             std::string("App/App.3dsx"));
    CHECK_EQ(shown.fileCount, 1u);
    CHECK_EQ(shown.totalSize, static_cast<uint64_t>(10));
    CHECK_EQ(shown.top, std::string("App"));
    CHECK(shown.topIsFolder);

    // 3dsDL's own reader writes the listing's names, so only stays empty.
    listing.ownZip = true;
    archives::Options own;
    CHECK(remote::sentExtraction(listing, "App.zip", own, shown));
    CHECK(own.only.empty());

    // Its one app is an empty folder: nothing would be extracted.
    listing.entries.resize(2);
    listing.entries[1].path = "cias/App.cia";
    archives::Options none;
    CHECK(!remote::sentExtraction(listing, "App.zip", none, shown));
}

TEST("remote: a sender's folder is /3ds/ or /cias/ or one folder in them")
{
    remote::SendOptions options;
    std::string why;
    for (const char *good : {"folder=%2F3ds", "folder=%2F3ds%2F", "folder=%2F3ds%2FApp",
                             "folder=%2F3ds%2FApp%2F", "folder=%2Fcias%2FGames"}) {
        CHECK(remote::sendOptions(std::string("links=x&") + good, options, why));
    }
    for (const char *deep : {"folder=%2F3ds%2FCheckpoint%2Fsaves",
                             "folder=%2F3ds%2FApp%2Fdata%2F",
                             "folder=%2Fcias%2Fa%2Fb"}) {
        why.clear();
        CHECK(!remote::sendOptions(std::string("links=x&") + deep, options, why));
        CHECK(!why.empty());
        CHECK(options.folder.empty());
    }
}

TEST("remote: a sender's folder keeps only apps and archives")
{
    for (const char *kept : {"App.3dsx", "game.CIA", "game.zcia", "pack.zip",
                             "pack.7z", "pack.tar.gz"}) {
        CHECK(remote::sentFileAllowed(kept));
    }
    for (const char *moved : {"config.ini", "cheats.json", "script.lua",
                              "plugin.3gx", "boot.firm", "save.bin", "boot.3dsx",
                              "3dsDL.3dsx", "APP~1.3DS", "ap\xc3\xa9.3dsx", ""}) {
        CHECK(!remote::sentFileAllowed(moved));
    }
}
