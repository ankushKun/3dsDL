#pragma once

#include <cstdint>
#include <cstdio>
#include <functional>
#include <string>

// The HTTP(S) download engine. It has no user interface and no libctru
// dependency, so the 3DS app runs it on a worker thread and the host tests
// run it against a local fixture server.
namespace net {

// How many redirects a download follows, and whereTo with it.
constexpr long MaxRedirects = 10;

enum class Phase { Connecting, Downloading, Restarting };

// Receives updates from the transfer. Every method is called on the thread
// that runs download().
class Observer {
public:
    virtual ~Observer() = default;
    virtual bool cancelRequested() = 0;
    virtual void onPhase(Phase) {}
    virtual void onFilename(const std::string &) {}
    // received counts bytes already on the SD card, including resumed data.
    // total is 0 while the size is unknown.
    virtual void onProgress(uint64_t received, uint64_t total)
    {
        (void)received;
        (void)total;
    }
    virtual void onResumed(uint64_t offset) { (void)offset; }
    // How many connections are open right now.
    virtual void onConnections(int count) { (void)count; }
    // When a cancelled transfer could be resumed, keep its partial file
    // instead of deleting it. Used when the app is closed mid-download.
    virtual bool keepPartialOnCancel() { return false; }
};

struct Request {
    std::string url;
    std::string folder; // existing folder, ending with '/'
    std::string caFile; // PEM bundle; "" uses libcurl's default
    std::string userAgent;
    bool allowResume = true;
    long connectTimeoutSeconds = 30;
    // Give up when less than one byte per second arrives for this long.
    long stallTimeoutSeconds = 60;
    // Receive and send buffer size requested for each connection's socket,
    // in bytes. 0 keeps the system default.
    int socketBufferSize = 0;
    // A large file from a server that accepts byte ranges is split across up
    // to this many connections. Each one is limited by its TCP window, so
    // on a link with any latency more connections means more speed.
    int connections = 1;
    // The smallest piece worth opening another connection for.
    uint64_t minPieceBytes = 512 * 1024;
    // FAT32 cannot hold files of 4 GiB or more.
    uint64_t maxFileSize = 0xffffffffULL;
    // Free bytes in a folder, or -1 when unknown. Optional.
    std::function<int64_t(const std::string &)> freeSpace;
    // Test seam for simulating SD card failures. Defaults to fwrite.
    std::function<size_t(const void *, size_t, FILE *)> write;
};

enum class Outcome { Completed, Failed, Cancelled };

struct Result {
    Outcome outcome = Outcome::Failed;
    std::string partPath;     // finished data, or the kept partial download
    std::string filename;     // sanitised name from the server or the URL
    std::string effectiveUrl; // after redirects
    uint64_t bytes = 0;       // size of the data in partPath
    long httpStatus = 0;
    int curlCode = 0;
    bool resumable = false; // a failed download was kept for a later retry
    bool resumed = false;   // this run continued an earlier partial download
    int connections = 0;    // the most connections that were open at once
    // Bytes that arrived in this run, and how long the run took, connecting
    // included.
    uint64_t transferred = 0;
    double seconds = 0.0;
    // The server's certificate was rejected; often a wrong console clock.
    bool certificateProblem = false;
    // The SD card has too little room for the file.
    bool noSpace = false;
    std::string message;    // for the user
    std::string detail;     // technical detail, may be empty
};

Result download(const Request &request, Observer &observer);

// Size of a kept partial download of url in folder, or 0 when there is none.
uint64_t partialSize(const std::string &folder, const std::string &url);
void discardPartial(const std::string &folder, const std::string &url);

// A message for the user explaining a libcurl failure. detail is libcurl's
// error text, which says why a certificate was rejected.
std::string describeFailure(int curlCode, long httpStatus,
                            const std::string &detail = "");

} // namespace net
