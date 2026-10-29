#include "../check.hpp"

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
