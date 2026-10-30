#include "fetch.hpp"

#include "download_engine.hpp"
#include "tls_clock.hpp"

#include <curl/curl.h>

namespace net {

namespace {

struct Transfer {
    const FetchRequest *request;
    std::string body;
    bool tooLarge = false;
};

size_t writeCallback(char *data, size_t size, size_t count, void *user)
{
    Transfer &transfer = *static_cast<Transfer *>(user);
    const size_t bytes = size * count;
    if (transfer.body.size() + bytes > transfer.request->maxBytes) {
        transfer.tooLarge = true;
        return 0; // makes libcurl stop with CURLE_WRITE_ERROR
    }
    transfer.body.append(data, bytes);
    return bytes;
}

int progressCallback(void *user, curl_off_t, curl_off_t, curl_off_t,
                     curl_off_t)
{
    const Transfer &transfer = *static_cast<const Transfer *>(user);
    const std::atomic<bool> *cancel = transfer.request->cancel;
    return cancel != nullptr && cancel->load() ? 1 : 0;
}

} // namespace

FetchResult fetch(const FetchRequest &request)
{
    FetchResult result;
    CURL *curl = curl_easy_init();
    if (curl == nullptr) {
        result.message = "The network library could not start a request.";
        return result;
    }

    Transfer transfer;
    transfer.request = &request;
    char errorBuffer[CURL_ERROR_SIZE] = {};
    curl_slist *headers = nullptr;
    for (const std::string &header : request.headers) {
        headers = curl_slist_append(headers, header.c_str());
    }

    curl_easy_setopt(curl, CURLOPT_URL, request.url.c_str());
    if (!request.userAgent.empty()) {
        curl_easy_setopt(curl, CURLOPT_USERAGENT, request.userAgent.c_str());
    }
    if (headers != nullptr) {
        curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    }
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_MAXREDIRS, 5L);
    curl_easy_setopt(curl, CURLOPT_PROTOCOLS_STR, "http,https");
    curl_easy_setopt(curl, CURLOPT_REDIR_PROTOCOLS_STR, "http,https");
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, request.connectTimeoutSeconds);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, request.totalTimeoutSeconds);
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(curl, CURLOPT_HTTP_VERSION, CURL_HTTP_VERSION_1_1);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 1L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 2L);
    useLocalClock(curl);
    if (!request.caFile.empty()) {
        curl_easy_setopt(curl, CURLOPT_CAINFO, request.caFile.c_str());
    }
    curl_easy_setopt(curl, CURLOPT_ERRORBUFFER, errorBuffer);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, writeCallback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &transfer);
    curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
    curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, progressCallback);
    curl_easy_setopt(curl, CURLOPT_XFERINFODATA, &transfer);

    const CURLcode code = curl_easy_perform(curl);
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &result.httpStatus);
    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);

    result.curlCode = static_cast<int>(code);
    result.detail = errorBuffer;
    if (code == CURLE_ABORTED_BY_CALLBACK) {
        result.cancelled = true;
        result.message = "Cancelled.";
        return result;
    }
    if (transfer.tooLarge) {
        result.message = "The server's answer was larger than expected.";
        return result;
    }
    if (code != CURLE_OK) {
        result.certificateProblem = code == CURLE_PEER_FAILED_VERIFICATION;
        result.message = describeFailure(result.curlCode, result.httpStatus,
                                         result.detail);
        return result;
    }
    if (result.httpStatus < 200 || result.httpStatus >= 300) {
        result.message = "The server answered with HTTP status " +
                         std::to_string(result.httpStatus) + ".";
        return result;
    }
    result.ok = true;
    result.body = std::move(transfer.body);
    return result;
}

namespace {

struct Probe {
    const FetchRequest *request;
    bool answered = false;
    bool refused = false;
};

// Turns away a connection request.refuse names, before it is asked anything.
int probePrerequest(void *user, char *to, char *from, int, int)
{
    Probe &probe = *static_cast<Probe *>(user);
    if (probe.request->refuse(to != nullptr ? to : "", from != nullptr ? from : "")) {
        probe.refused = true;
        return CURL_PREREQFUNC_ABORT;
    }
    return CURL_PREREQFUNC_OK;
}

// Stops at the first byte of the answer, once the redirects are behind it.
size_t stopAtFirstByte(char *, size_t, size_t, void *user)
{
    static_cast<Probe *>(user)->answered = true;
    return 0; // makes libcurl stop with CURLE_WRITE_ERROR
}

int probeProgress(void *user, curl_off_t, curl_off_t, curl_off_t, curl_off_t)
{
    const std::atomic<bool> *cancel = static_cast<Probe *>(user)->request->cancel;
    return cancel != nullptr && cancel->load() ? 1 : 0;
}

} // namespace

Destination whereTo(const FetchRequest &request)
{
    Destination result;
    CURL *curl = curl_easy_init();
    if (curl == nullptr) {
        result.message = "The network library could not start a request.";
        return result;
    }
    Probe probe;
    probe.request = &request;
    char errorBuffer[CURL_ERROR_SIZE] = {};
    curl_slist *headers = nullptr;
    for (const std::string &header : request.headers) {
        headers = curl_slist_append(headers, header.c_str());
    }

    curl_easy_setopt(curl, CURLOPT_URL, request.url.c_str());
    if (!request.userAgent.empty()) {
        curl_easy_setopt(curl, CURLOPT_USERAGENT, request.userAgent.c_str());
    }
    if (headers != nullptr) {
        curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    }
    // A GET, as the download will be: some servers redirect only those.
    curl_easy_setopt(curl, CURLOPT_RANGE, "0-0");
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_MAXREDIRS, MaxRedirects);
    curl_easy_setopt(curl, CURLOPT_PROTOCOLS_STR, "http,https");
    curl_easy_setopt(curl, CURLOPT_REDIR_PROTOCOLS_STR, "http,https");
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, request.connectTimeoutSeconds);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, request.totalTimeoutSeconds);
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(curl, CURLOPT_HTTP_VERSION, CURL_HTTP_VERSION_1_1);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 1L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 2L);
    useLocalClock(curl);
    if (!request.caFile.empty()) {
        curl_easy_setopt(curl, CURLOPT_CAINFO, request.caFile.c_str());
    }
    curl_easy_setopt(curl, CURLOPT_ERRORBUFFER, errorBuffer);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, stopAtFirstByte);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &probe);
    curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
    curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, probeProgress);
    curl_easy_setopt(curl, CURLOPT_XFERINFODATA, &probe);
    if (request.refuse) {
        curl_easy_setopt(curl, CURLOPT_PREREQFUNCTION, probePrerequest);
        curl_easy_setopt(curl, CURLOPT_PREREQDATA, &probe);
    }

    const CURLcode code = curl_easy_perform(curl);
    long status = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
    char *effective = nullptr;
    curl_easy_getinfo(curl, CURLINFO_EFFECTIVE_URL, &effective);
    if (effective != nullptr) {
        result.url = effective;
    }
    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);

    if (probe.refused) {
        result.refused = true;
        result.message = "The link leads to an address on this network.";
        return result;
    }
    if (code == CURLE_ABORTED_BY_CALLBACK) {
        result.cancelled = true;
        result.message = "Cancelled.";
        return result;
    }
    // An empty answer ends without a byte; one with a body ends at it.
    result.ok = !result.url.empty() &&
                (code == CURLE_OK || (code == CURLE_WRITE_ERROR && probe.answered));
    if (!result.ok) {
        result.message = describeFailure(static_cast<int>(code), status, errorBuffer);
    }
    return result;
}

} // namespace net
