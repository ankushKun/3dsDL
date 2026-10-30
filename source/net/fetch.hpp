#pragma once

#include <atomic>
#include <cstddef>
#include <string>
#include <vector>

// Small HTTP(S) requests whose answer is kept in memory, such as asking
// GitHub for the latest release. Files go through download_engine instead.
namespace net {

struct FetchRequest {
    std::string url;
    std::string caFile; // PEM bundle; "" uses libcurl's default
    std::string userAgent;
    std::vector<std::string> headers; // "Name: value"
    // Answers larger than this fail instead of filling memory.
    size_t maxBytes = 1024 * 1024;
    long connectTimeoutSeconds = 15;
    long totalTimeoutSeconds = 30;
    // Checked while the request runs; setting it ends the request early.
    const std::atomic<bool> *cancel = nullptr;
};

struct FetchResult {
    bool ok = false; // a 2xx answer arrived in full
    long httpStatus = 0;
    int curlCode = 0;
    bool cancelled = false;
    // The server's certificate was rejected; often a wrong console clock.
    bool certificateProblem = false;
    std::string body;
    std::string message; // for the user when !ok
    std::string detail;  // technical detail, may be empty
};

FetchResult fetch(const FetchRequest &request);

// Where request.url leads once its redirects are followed, as a download of
// it would be (the same request, redirect limit and protocols), taking only
// the first byte of the answer. ok when an answer came, whatever its status;
// url is then the address that gave it.
struct Destination {
    bool ok = false;
    bool cancelled = false;
    std::string url;
    std::string message; // for the user when !ok
};
Destination whereTo(const FetchRequest &request);

} // namespace net
