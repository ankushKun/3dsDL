#include "../check.hpp"

#include "fetch.hpp"
#include "json.hpp"
#include "update.hpp"

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <vector>

// Runs net::fetch against tests/fixtures/server.py.

namespace {

std::string fixture(const char *name)
{
    const char *value = std::getenv(name);
    return value != nullptr ? value : "";
}

} // namespace

TEST("fetch: reads a release over HTTPS with custom headers")
{
    net::FetchRequest request;
    request.url = fixture("FIXTURE_HTTPS") + "/release/v9.1.0";
    request.caFile = fixture("FIXTURE_CERT");
    request.userAgent = "3dsDL-test";
    const net::FetchResult result = net::fetch(request);
    CHECK(result.ok);
    CHECK_EQ(result.httpStatus, 200L);
    update::Release release;
    std::string error;
    CHECK(update::parseRelease(result.body, release, error));
    CHECK_EQ(release.version, std::string("9.1.0"));
    CHECK(update::findAsset(release, update::CiaAsset) != nullptr);

    request.url = fixture("FIXTURE_HTTPS") + "/headers";
    request.headers = {"Accept: application/vnd.github+json"};
    const net::FetchResult echoed = net::fetch(request);
    json::Value headers;
    CHECK(json::parse(echoed.body, headers, error));
    CHECK_EQ(headers["accept"].text(), std::string("application/vnd.github+json"));
    CHECK_EQ(headers["user-agent"].text(), std::string("3dsDL-test"));
}

TEST("fetch: reports HTTP errors, size limits and untrusted certificates")
{
    net::FetchRequest request;
    request.url = fixture("FIXTURE_HTTP") + "/status/404";
    net::FetchResult result = net::fetch(request);
    CHECK(!result.ok);
    CHECK_EQ(result.httpStatus, 404L);
    CHECK(result.message.find("404") != std::string::npos);
    CHECK(result.body.empty());

    request.url = fixture("FIXTURE_HTTP") + "/file/5000";
    request.maxBytes = 4096;
    result = net::fetch(request);
    CHECK(!result.ok);
    CHECK(result.message.find("larger") != std::string::npos);

    request.maxBytes = 5000;
    result = net::fetch(request);
    CHECK(result.ok);
    CHECK_EQ(result.body.size(), static_cast<size_t>(5000));

    // The fixture's self-signed certificate is not in the Mozilla bundle.
    request.url = fixture("FIXTURE_HTTPS") + "/file/10";
    request.caFile = fixture("MOZILLA_CA");
    result = net::fetch(request);
    CHECK(!result.ok);
    CHECK(result.certificateProblem);
}

TEST("fetch: stops early when cancelled")
{
    std::atomic<bool> cancel(true);
    net::FetchRequest request;
    request.url = fixture("FIXTURE_HTTP") + "/file/400000?slow=1";
    request.cancel = &cancel;
    const auto started = std::chrono::steady_clock::now();
    const net::FetchResult result = net::fetch(request);
    CHECK(!result.ok);
    CHECK(result.cancelled);
    CHECK(std::chrono::steady_clock::now() - started < std::chrono::seconds(3));
}

TEST("fetch: whereTo follows redirects as a download would")
{
    net::FetchRequest request;
    request.url = fixture("FIXTURE_HTTP") + "/redirect/3";
    net::Destination destination = net::whereTo(request);
    CHECK(destination.ok);
    CHECK_EQ(destination.url, fixture("FIXTURE_HTTP") + "/file/1000");

    // Another host, which is what the console names before it asks.
    request.url = fixture("FIXTURE_HTTP") + "/redirect-host";
    destination = net::whereTo(request);
    CHECK(destination.ok);
    CHECK(destination.url.rfind("http://localhost:", 0) == 0);

    // An error page still says where the link ends.
    request.url = fixture("FIXTURE_HTTP") + "/status/404";
    destination = net::whereTo(request);
    CHECK(destination.ok);
    CHECK_EQ(destination.url, request.url);

    // Over HTTPS, and not to anything but HTTP or HTTPS.
    request.url = fixture("FIXTURE_HTTPS") + "/file/5000";
    request.caFile = fixture("FIXTURE_CERT");
    CHECK(net::whereTo(request).ok);
    request.url = fixture("FIXTURE_HTTP") + "/redirect-ftp";
    destination = net::whereTo(request);
    CHECK(!destination.ok);
    CHECK(!destination.message.empty());

    // An address the rule turns away, after a redirect too, is not asked.
    request.url = fixture("FIXTURE_HTTP") + "/redirect/2";
    request.caFile.clear();
    std::vector<std::string> seen;
    request.refuse = [&](const std::string &to, const std::string &from) {
        seen.push_back(to);
        CHECK(!from.empty());
        return to == "127.0.0.1";
    };
    destination = net::whereTo(request);
    CHECK(!destination.ok);
    CHECK(destination.refused);
    CHECK(!destination.cancelled);
    CHECK_EQ(seen.size(), static_cast<size_t>(1));
    request.refuse = [](const std::string &, const std::string &) { return false; };
    CHECK(net::whereTo(request).ok);
    request.refuse = nullptr;

    std::atomic<bool> cancel {true};
    request.url = fixture("FIXTURE_HTTP") + "/file/400000?slow=1";
    request.cancel = &cancel;
    CHECK(net::whereTo(request).cancelled);
}
