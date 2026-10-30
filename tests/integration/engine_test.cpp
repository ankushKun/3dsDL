#define CHECK_MAIN
#include "../check.hpp"
#include "../support.hpp"

#include "download_engine.hpp"
#include "fs_util.hpp"
#include "records.hpp"

#include <curl/curl.h>
#include <dirent.h>

#include <cerrno>
#include <cstdlib>
#include <limits>
#include <vector>

// Runs the real download engine against tests/fixtures/server.py. The
// environment comes from run_integration.sh.

namespace {

std::string env(const char *name)
{
    const char *value = std::getenv(name);
    return value != nullptr ? value : "";
}

std::string pattern(size_t size)
{
    std::string data(size, '\0');
    for (size_t index = 0; index < size; ++index) {
        data[index] = static_cast<char>((index * 7 + index / 256) % 251);
    }
    return data;
}

// Files in folder, including hidden ones, so leftovers are caught.
std::vector<std::string> listAll(const std::string &folder)
{
    std::vector<std::string> names;
    if (DIR *directory = opendir(folder.c_str())) {
        while (dirent *entry = readdir(directory)) {
            const std::string name = entry->d_name;
            if (name != "." && name != "..") {
                names.push_back(name);
            }
        }
        closedir(directory);
    }
    return names;
}

struct Recorder : net::Observer {
    uint64_t cancelAfter = std::numeric_limits<uint64_t>::max();
    bool keepPartial = false;
    uint64_t received = 0;
    uint64_t total = 0;
    uint64_t resumedAt = 0;
    std::string filename;
    std::vector<net::Phase> phases;

    bool cancelRequested() override { return received >= cancelAfter; }
    void onPhase(net::Phase phase) override { phases.push_back(phase); }
    void onFilename(const std::string &name) override { filename = name; }
    void onProgress(uint64_t now, uint64_t expected) override
    {
        received = now;
        if (expected > 0) {
            total = expected;
        }
    }
    void onResumed(uint64_t offset) override { resumedAt = offset; }
    bool keepPartialOnCancel() override { return keepPartial; }

    bool saw(net::Phase phase) const
    {
        for (const net::Phase seen : phases) {
            if (seen == phase) {
                return true;
            }
        }
        return false;
    }
};

net::Request request(const std::string &path, const TempDir &folder)
{
    net::Request value;
    value.url = env("FIXTURE_HTTP") + path;
    value.folder = folder.path();
    value.userAgent = "3dsDL-tests";
    value.connectTimeoutSeconds = 5;
    value.stallTimeoutSeconds = 2;
    return value;
}

bool contains(const std::string &haystack, const std::string &needle)
{
    return haystack.find(needle) != std::string::npos;
}

struct CurlGlobal {
    CurlGlobal() { curl_global_init(CURL_GLOBAL_DEFAULT); }
    ~CurlGlobal() { curl_global_cleanup(); }
} curlGlobal;

} // namespace

TEST("engine: downloads a file and reports progress")
{
    TempDir folder;
    Recorder recorder;
    const net::Result result =
        net::download(request("/file/300000", folder), recorder);
    CHECK(result.outcome == net::Outcome::Completed);
    CHECK_EQ(result.filename, std::string("300000"));
    CHECK_EQ(result.bytes, static_cast<uint64_t>(300000));
    CHECK_EQ(readText(result.partPath), pattern(300000));
    CHECK_EQ(recorder.total, static_cast<uint64_t>(300000));
    CHECK_EQ(recorder.received, static_cast<uint64_t>(300000));
    CHECK(recorder.saw(net::Phase::Connecting));
    CHECK(recorder.saw(net::Phase::Downloading));
    // Only the finished data file is left; its resume sidecar is gone.
    CHECK_EQ(listAll(folder.path()).size(), static_cast<size_t>(1));
}

TEST("engine: downloads with explicit socket buffer sizes")
{
    TempDir folder;
    Recorder recorder;
    net::Request settings = request("/file/300000", folder);
    settings.socketBufferSize = 48 * 1024;
    const net::Result result = net::download(settings, recorder);
    CHECK(result.outcome == net::Outcome::Completed);
    CHECK_EQ(readText(result.partPath), pattern(300000));
}

net::Request splitRequest(const std::string &path, const TempDir &folder)
{
    net::Request value = request(path, folder);
    value.connections = 4;
    value.minPieceBytes = 64 * 1024;
    return value;
}

TEST("engine: large files are split across connections")
{
    TempDir folder;
    Recorder recorder;
    const net::Result result =
        net::download(splitRequest("/file/1000000", folder), recorder);
    CHECK(result.outcome == net::Outcome::Completed);
    CHECK(result.connections > 1);
    CHECK_EQ(result.bytes, static_cast<uint64_t>(1000000));
    CHECK_EQ(readText(result.partPath), pattern(1000000));
    CHECK_EQ(recorder.received, static_cast<uint64_t>(1000000));
    CHECK_EQ(recorder.total, static_cast<uint64_t>(1000000));
    CHECK_EQ(listAll(folder.path()).size(), static_cast<size_t>(1));
}

TEST("engine: split downloads work over HTTPS")
{
    TempDir folder;
    Recorder recorder;
    net::Request settings = splitRequest("", folder);
    settings.url = env("FIXTURE_HTTPS") + "/file/1000000";
    settings.caFile = env("FIXTURE_CERT");
    const net::Result result = net::download(settings, recorder);
    CHECK(result.outcome == net::Outcome::Completed);
    CHECK(result.connections > 1);
    CHECK_EQ(readText(result.partPath), pattern(1000000));
}

TEST("engine: small files and files without a validator use one connection")
{
    TempDir folder;
    Recorder recorder;
    net::Result result =
        net::download(splitRequest("/file/100000", folder), recorder);
    CHECK(result.outcome == net::Outcome::Completed);
    CHECK_EQ(result.connections, 1);

    TempDir other;
    result = net::download(
        splitRequest("/file/1000000?etag=none&lm=0", other), recorder);
    CHECK(result.outcome == net::Outcome::Completed);
    CHECK_EQ(result.connections, 1);
    CHECK_EQ(readText(result.partPath), pattern(1000000));
}

TEST("engine: a dropped connection of a split download is reopened")
{
    TempDir folder;
    Recorder recorder;
    const net::Result result = net::download(
        splitRequest("/file/1000000?drop_at=100000&key=split-drop", folder),
        recorder);
    CHECK(result.outcome == net::Outcome::Completed);
    CHECK(result.connections > 1);
    CHECK_EQ(readText(result.partPath), pattern(1000000));
}

TEST("engine: servers that turn away extra connections still finish")
{
    TempDir folder;
    Recorder recorder;
    const net::Result result = net::download(
        splitRequest("/file/1000000?refuse_ranged=3&key=refuse", folder),
        recorder);
    CHECK(result.outcome == net::Outcome::Completed);
    CHECK_EQ(readText(result.partPath), pattern(1000000));
}

TEST("engine: servers that mishandle ranges fall back to one connection")
{
    TempDir folder;
    Recorder ignoring;
    net::Result result = net::download(
        splitRequest("/file/1000000?range_ignore=1", folder), ignoring);
    CHECK(result.outcome == net::Outcome::Completed);
    CHECK(ignoring.saw(net::Phase::Restarting));
    CHECK_EQ(result.connections, 1);
    CHECK_EQ(readText(result.partPath), pattern(1000000));

    // An ETag that changes with every request makes every range a mismatch.
    TempDir other;
    Recorder changing;
    result = net::download(
        splitRequest("/file/1000000?etag=changing&key=split-changing", other),
        changing);
    CHECK(result.outcome == net::Outcome::Completed);
    CHECK(changing.saw(net::Phase::Restarting));
    CHECK_EQ(readText(result.partPath), pattern(1000000));
}

TEST("engine: a split download resumes only the missing pieces")
{
    TempDir folder;
    Recorder first;
    first.cancelAfter = 150000;
    first.keepPartial = true;
    const std::string path = "/file/600000?slow=1&key=split-resume";
    net::Result result =
        net::download(splitRequest(path, folder), first);
    CHECK(result.outcome == net::Outcome::Cancelled);
    CHECK(result.resumable);
    CHECK(result.connections > 1);
    CHECK(result.bytes >= 150000);
    CHECK(result.bytes < 600000);
    const uint64_t kept = result.bytes;
    CHECK_EQ(net::partialSize(folder.path(), env("FIXTURE_HTTP") + path),
             kept);

    std::string stored;
    ResumeInfo info;
    CHECK(fs::readFile(
        partialPaths(folder.path(), env("FIXTURE_HTTP") + path).info,
        stored));
    CHECK(parseResumeInfo(stored, info));
    CHECK(info.split);

    Recorder second;
    result = net::download(splitRequest(path, folder), second);
    CHECK(result.outcome == net::Outcome::Completed);
    CHECK(result.resumed);
    CHECK_EQ(second.resumedAt, kept);
    CHECK_EQ(readText(result.partPath), pattern(600000));
    CHECK_EQ(listAll(folder.path()).size(), static_cast<size_t>(1));
}

TEST("engine: Content-Disposition names win over the URL")
{
    TempDir folder;
    Recorder recorder;
    const net::Result result = net::download(
        request("/file/10?cd=attachment%3B%20filename%2A%3DUTF-8%27%27"
                "na%25C3%25AFve.zip",
                folder),
        recorder);
    CHECK(result.outcome == net::Outcome::Completed);
    CHECK_EQ(result.filename, std::string("na\xc3\xafve.zip"));
    CHECK_EQ(recorder.filename, result.filename);
}

TEST("engine: chunked responses without a length")
{
    TempDir folder;
    Recorder recorder;
    const net::Result result =
        net::download(request("/chunked/50000", folder), recorder);
    CHECK(result.outcome == net::Outcome::Completed);
    CHECK_EQ(readText(result.partPath), pattern(50000));
    CHECK_EQ(recorder.total, static_cast<uint64_t>(0));
}

TEST("engine: follows redirects and names the file after the final URL")
{
    TempDir folder;
    Recorder recorder;
    const net::Result result =
        net::download(request("/redirect/3", folder), recorder);
    CHECK(result.outcome == net::Outcome::Completed);
    CHECK_EQ(result.filename, std::string("1000"));
    CHECK(contains(result.effectiveUrl, "/file/1000"));
    CHECK_EQ(readText(result.partPath), pattern(1000));
}

TEST("engine: stops redirect loops and non-HTTP redirects")
{
    TempDir folder;
    Recorder recorder;
    net::Result result =
        net::download(request("/redirect/15", folder), recorder);
    CHECK(result.outcome == net::Outcome::Failed);
    CHECK_EQ(result.curlCode, static_cast<int>(CURLE_TOO_MANY_REDIRECTS));
    CHECK(contains(result.message, "redirected too many"));

    result = net::download(request("/redirect-ftp", folder), recorder);
    CHECK(result.outcome == net::Outcome::Failed);
    CHECK_EQ(result.curlCode, static_cast<int>(CURLE_UNSUPPORTED_PROTOCOL));
    CHECK(listAll(folder.path()).empty());
}

TEST("engine: HTTP errors are explained and nothing is saved")
{
    TempDir folder;
    Recorder recorder;
    net::Result result =
        net::download(request("/status/404", folder), recorder);
    CHECK(result.outcome == net::Outcome::Failed);
    CHECK_EQ(result.httpStatus, 404L);
    CHECK(contains(result.message, "not found"));
    CHECK(!result.resumable);

    result = net::download(request("/status/503", folder), recorder);
    CHECK_EQ(result.httpStatus, 503L);
    CHECK(contains(result.message, "server had an error"));
    CHECK(listAll(folder.path()).empty());
}

TEST("engine: a dropped connection keeps a resumable partial file")
{
    TempDir folder;
    Recorder first;
    const std::string path = "/file/200000?drop_at=50000&key=drop-resume";
    net::Result result = net::download(request(path, folder), first);
    CHECK(result.outcome == net::Outcome::Failed);
    CHECK(result.resumable);
    CHECK_EQ(result.bytes, static_cast<uint64_t>(50000));
    CHECK(contains(result.message, "connection dropped"));
    CHECK_EQ(net::partialSize(folder.path(), env("FIXTURE_HTTP") + path),
             static_cast<uint64_t>(50000));

    Recorder second;
    result = net::download(request(path, folder), second);
    CHECK(result.outcome == net::Outcome::Completed);
    CHECK(result.resumed);
    CHECK_EQ(second.resumedAt, static_cast<uint64_t>(50000));
    CHECK_EQ(second.total, static_cast<uint64_t>(200000));
    CHECK_EQ(readText(result.partPath), pattern(200000));
    CHECK_EQ(net::partialSize(folder.path(), env("FIXTURE_HTTP") + path),
             static_cast<uint64_t>(0));
}

TEST("engine: a stalled server times out and can be resumed")
{
    TempDir folder;
    Recorder first;
    const std::string path = "/file/100000?stall_at=30000&key=stall-resume";
    net::Result result = net::download(request(path, folder), first);
    CHECK(result.outcome == net::Outcome::Failed);
    CHECK_EQ(result.curlCode, static_cast<int>(CURLE_OPERATION_TIMEDOUT));
    CHECK(result.resumable);

    Recorder second;
    result = net::download(request(path, folder), second);
    CHECK(result.outcome == net::Outcome::Completed);
    CHECK(result.resumed);
    CHECK_EQ(readText(result.partPath), pattern(100000));
}

TEST("engine: resume is skipped when disabled")
{
    TempDir folder;
    Recorder recorder;
    net::Request settings =
        request("/file/80000?drop_at=20000&key=no-resume", folder);
    settings.allowResume = false;
    const net::Result result = net::download(settings, recorder);
    CHECK(result.outcome == net::Outcome::Failed);
    CHECK(!result.resumable);
    CHECK(listAll(folder.path()).empty());
}

TEST("engine: a changed file restarts from the beginning")
{
    TempDir folder;
    Recorder first;
    const std::string path =
        "/file/60000?drop_at=20000&etag=changing&key=changed";
    net::Result result = net::download(request(path, folder), first);
    CHECK(result.resumable);

    Recorder second;
    result = net::download(request(path, folder), second);
    CHECK(result.outcome == net::Outcome::Completed);
    CHECK(!result.resumed);
    CHECK(second.saw(net::Phase::Restarting));
    CHECK_EQ(readText(result.partPath), pattern(60000));
}

TEST("engine: a server without range support leaves nothing to resume")
{
    TempDir folder;
    Recorder first;
    // Accept-Ranges: none means the partial file is not even kept.
    const std::string none = "/file/40000?drop_at=10000&ranges=0&key=noranges";
    net::Result result = net::download(request(none, folder), first);
    CHECK(!result.resumable);
    CHECK(listAll(folder.path()).empty());
}

TEST("engine: without a strong validator nothing is resumed")
{
    TempDir folder;
    Recorder recorder;
    const net::Result result = net::download(
        request("/file/40000?drop_at=10000&etag=weak&lm=0&key=weak", folder),
        recorder);
    CHECK(result.outcome == net::Outcome::Failed);
    CHECK(!result.resumable);
    CHECK(listAll(folder.path()).empty());
}

TEST("engine: cancelling removes the partial file")
{
    TempDir folder;
    Recorder recorder;
    recorder.cancelAfter = 20000;
    const net::Result result =
        net::download(request("/file/2000000?slow=1", folder), recorder);
    CHECK(result.outcome == net::Outcome::Cancelled);
    CHECK(listAll(folder.path()).empty());
}

TEST("engine: closing the app mid-download keeps the partial file")
{
    TempDir folder;
    Recorder first;
    first.cancelAfter = 20000;
    first.keepPartial = true;
    const std::string path = "/file/300000?slow=1&key=closing";
    net::Result result = net::download(request(path, folder), first);
    CHECK(result.outcome == net::Outcome::Cancelled);
    CHECK(result.resumable);
    CHECK(result.bytes >= 20000);

    Recorder second;
    result = net::download(request("/file/300000?key=closing", folder),
                           second);
    CHECK(result.outcome == net::Outcome::Completed);
    // A different URL means a different partial file, so this one starts
    // fresh and the earlier partial file is still there to resume.
    CHECK(!result.resumed);
    CHECK(net::partialSize(folder.path(), env("FIXTURE_HTTP") + path) > 0);

    Recorder third;
    result = net::download(request(path, folder), third);
    CHECK(result.outcome == net::Outcome::Completed);
    CHECK(result.resumed);
    CHECK_EQ(readText(result.partPath), pattern(300000));
}

TEST("engine: refuses files that do not fit")
{
    TempDir folder;
    Recorder recorder;
    net::Request settings = request("/file/100000", folder);
    settings.freeSpace = [](const std::string &) {
        return static_cast<int64_t>(1000);
    };
    net::Result result = net::download(settings, recorder);
    CHECK(result.outcome == net::Outcome::Failed);
    CHECK(contains(result.message, "Not enough free space"));
    CHECK(result.noSpace);
    CHECK(listAll(folder.path()).empty());

    settings = request("/file/100000", folder);
    settings.maxFileSize = 5000;
    result = net::download(settings, recorder);
    CHECK(result.outcome == net::Outcome::Failed);
    CHECK(contains(result.message, "FAT32"));
    CHECK(!result.noSpace);

    settings = request("/huge", folder);
    result = net::download(settings, recorder);
    CHECK(result.outcome == net::Outcome::Failed);
    CHECK(contains(result.message, "FAT32"));
    CHECK(listAll(folder.path()).empty());
}

TEST("engine: SD card write failures are reported and cleaned up")
{
    TempDir folder;
    Recorder recorder;
    net::Request settings = request("/file/100000", folder);
    size_t budget = 30000;
    settings.write = [&budget](const void *data, size_t size, FILE *file) {
        if (size > budget) {
            errno = ENOSPC;
            return static_cast<size_t>(0);
        }
        budget -= size;
        return std::fwrite(data, 1, size, file);
    };
    const net::Result result = net::download(settings, recorder);
    CHECK(result.outcome == net::Outcome::Failed);
    CHECK_EQ(result.message, std::string("The SD card is full."));
    CHECK(result.noSpace);
    CHECK(!result.resumable);
    CHECK(listAll(folder.path()).empty());
}

TEST("engine: the write seam can send the data elsewhere, in order")
{
    // Direct CIA install: one connection, nothing kept, and the data goes
    // to the install service instead of the file, at the offset the engine
    // has seeked to.
    TempDir folder;
    Recorder recorder;
    net::Request settings = request("/file/700000", folder);
    settings.connections = 1;
    settings.allowResume = false;
    std::string received;
    bool inOrder = true;
    settings.write = [&](const void *data, size_t size, FILE *file) {
        const off_t at = ftello(file);
        inOrder = inOrder && at == static_cast<off_t>(received.size());
        received.append(static_cast<const char *>(data), size);
        return size;
    };
    const net::Result result = net::download(settings, recorder);
    CHECK(result.outcome == net::Outcome::Completed);
    CHECK(inOrder);
    CHECK_EQ(received.size(), static_cast<size_t>(700000));
    CHECK(received == pattern(700000));
    CHECK_EQ(fs::fileSize(result.partPath), static_cast<int64_t>(0));
}

TEST("engine: rejects bad input before connecting")
{
    TempDir folder;
    Recorder recorder;
    net::Request settings = request("/file/10", folder);
    settings.folder = folder.path() + "missing/";
    net::Result result = net::download(settings, recorder);
    CHECK(result.outcome == net::Outcome::Failed);
    CHECK(contains(result.message, "does not exist"));

    settings = request("/file/10", folder);
    settings.url = "ftp://example.com/file";
    result = net::download(settings, recorder);
    CHECK(contains(result.message, "http:// and https://"));
    CHECK(recorder.phases.empty());
}

TEST("engine: empty bodies still produce a named file")
{
    TempDir folder;
    Recorder recorder;
    const net::Result result = net::download(
        request("/empty?cd=attachment%3B%20filename%3D%22zero.txt%22", folder),
        recorder);
    CHECK(result.outcome == net::Outcome::Completed);
    CHECK_EQ(result.filename, std::string("zero.txt"));
    CHECK_EQ(result.bytes, static_cast<uint64_t>(0));
}

TEST("engine: HTTPS with a trusted certificate")
{
    TempDir folder;
    Recorder recorder;
    net::Request settings = request("", folder);
    settings.url = env("FIXTURE_HTTPS") + "/file/5000";
    settings.caFile = env("FIXTURE_CERT");
    const net::Result result = net::download(settings, recorder);
    CHECK(result.outcome == net::Outcome::Completed);
    CHECK_EQ(readText(result.partPath), pattern(5000));
}

TEST("engine: HTTPS rejects untrusted certificates and wrong hosts")
{
    TempDir folder;
    Recorder recorder;
    net::Request settings = request("", folder);
    settings.url = env("FIXTURE_HTTPS") + "/file/5000";
    settings.caFile = env("MOZILLA_CA");
    net::Result result = net::download(settings, recorder);
    CHECK(result.outcome == net::Outcome::Failed);
    CHECK_EQ(result.curlCode, static_cast<int>(CURLE_PEER_FAILED_VERIFICATION));
    CHECK(result.certificateProblem);
    CHECK(contains(result.message, "3dsDL trusts"));

    settings.url = env("FIXTURE_HTTPS_IP") + "/file/5000";
    settings.caFile = env("FIXTURE_CERT");
    result = net::download(settings, recorder);
    CHECK(result.outcome == net::Outcome::Failed);
    CHECK_EQ(result.curlCode, static_cast<int>(CURLE_PEER_FAILED_VERIFICATION));
    CHECK(contains(result.message, "different site"));
    CHECK(listAll(folder.path()).empty());
}

TEST("engine: failure messages")
{
    CHECK(contains(net::describeFailure(CURLE_COULDNT_RESOLVE_HOST, 0),
                   "Wi-Fi"));
    CHECK(contains(net::describeFailure(CURLE_HTTP_RETURNED_ERROR, 403),
                   "HTTP 403"));
    CHECK(contains(net::describeFailure(CURLE_HTTP_RETURNED_ERROR, 429),
                   "limiting"));
    CHECK(contains(net::describeFailure(CURLE_HTTP_RETURNED_ERROR, 418),
                   "HTTP 418"));
    CHECK(contains(net::describeFailure(CURLE_SSL_CACERT_BADFILE, 0),
                   "certificate store"));
    CHECK(!net::describeFailure(CURLE_BAD_FUNCTION_ARGUMENT, 0).empty());

    // The reasons mbedTLS gives on the console.
    const int cert = CURLE_PEER_FAILED_VERIFICATION;
    CHECK(contains(net::describeFailure(cert, 0,
                                        "Cert verify failed: BADCERT_FUTURE"),
                   "not valid yet"));
    CHECK(contains(net::describeFailure(cert, 0,
                                        "Cert verify failed: BADCERT_EXPIRED"),
                   "expired"));
    CHECK(contains(net::describeFailure(cert, 0,
                                        "Cert verify failed: "
                                        "BADCERT_NOT_TRUSTED"),
                   "3dsDL trusts"));
    CHECK(contains(net::describeFailure(cert, 0,
                                        "Cert verify failed: "
                                        "BADCERT_CN_MISMATCH"),
                   "different site"));
    CHECK(contains(net::describeFailure(cert, 0, "something else"),
                   "Check clock"));
}

TEST("engine: discarding a partial download")
{
    TempDir folder;
    Recorder recorder;
    const std::string path = "/file/50000?drop_at=10000&key=discard";
    const net::Result result = net::download(request(path, folder), recorder);
    CHECK(result.resumable);
    net::discardPartial(folder.path(), env("FIXTURE_HTTP") + path);
    CHECK(listAll(folder.path()).empty());
}

TEST("engine: an address the request refuses is never asked")
{
    TempDir folder;
    Recorder recorder;
    net::Request value = request("/redirect/2", folder);
    value.refuse = [](const std::string &to, const std::string &from) {
        CHECK(!from.empty());
        return to == "127.0.0.1";
    };
    const net::Result result = net::download(value, recorder);
    CHECK(result.outcome == net::Outcome::Failed);
    CHECK(contains(result.message, "address on this network"));
    CHECK(!result.resumable);

    value.refuse = [](const std::string &, const std::string &) { return false; };
    CHECK(net::download(value, recorder).outcome == net::Outcome::Completed);
}
