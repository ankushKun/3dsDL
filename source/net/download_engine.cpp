#include "download_engine.hpp"

#include "fs_util.hpp"
#include "naming.hpp"
#include "records.hpp"
#include "text.hpp"
#include "tls_clock.hpp"
#include "units.hpp"
#include "url.hpp"

#include <curl/curl.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <iterator>
#include <limits>
#include <list>
#include <memory>
#include <vector>

namespace net {
namespace {

constexpr size_t FileBufferSize = 256 * 1024;
constexpr long CurlBufferSize = 128 * 1024;
// A download starts over at most twice: once when the file changed since the
// partial download, and once more on one connection when the server
// mishandles ranges.
constexpr int MaxAttempts = 3;
// Failed connections of a split download that are reopened before giving up.
constexpr int MaxReconnects = 8;
// Pieces are split on this boundary so SD card writes stay aligned.
constexpr uint64_t SplitAlignment = 64 * 1024;
// New data a split download writes between saves of its progress.
constexpr uint64_t SaveInterval = 4 * 1024 * 1024;
constexpr int PollMilliseconds = 100;
constexpr uint64_t Unknown = std::numeric_limits<uint64_t>::max();

struct Connection;

// A run [start, end) of the file. Bytes [start, next) have arrived and
// [start, flushed) are on the SD card. end is Unknown while the size is.
struct Piece {
    uint64_t start = 0;
    uint64_t next = 0;
    uint64_t flushed = 0;
    uint64_t end = Unknown;
    Connection *connection = nullptr;

    bool complete() const { return next == end; }
};

struct Transfer;

// One HTTP request, filling one piece from its next byte on.
struct Connection {
    Transfer *transfer = nullptr;
    Piece *piece = nullptr;
    CURL *curl = nullptr;
    curl_slist *headers = nullptr;
    // The attempt's first request. Its response decides the file name, the
    // size and whether the download is split.
    bool lead = false;
    bool started = false; // the response's headers have been handled
    bool cutOff = false;  // stopped on purpose at the end of its piece
    std::vector<char> buffer;
    uint64_t bufferStart = 0; // file position of buffer[0]
    char errorBuffer[CURL_ERROR_SIZE] = {0};

    // Headers of the response currently being received. A redirect starts a
    // new response, which clears them.
    std::string contentDisposition;
    std::string etag;
    std::string lastModified;
    std::string acceptRanges;
    std::string contentRange;
};

struct Transfer {
    const Request *request = nullptr;
    Observer *observer = nullptr;
    CURLM *multi = nullptr;
    CURLSH *share = nullptr;
    FILE *file = nullptr;
    std::string url;
    PartialPaths paths;
    uint64_t fileBytes = 0; // size of the file when this attempt began

    std::list<Piece> pieces; // in file order, covering it without gaps
    std::vector<std::unique_ptr<Connection>> connections;
    bool allowSplit = true;
    bool split = false;
    int maxConnections = 1;
    int peakConnections = 0;
    int reconnectsLeft = MaxReconnects;
    // Where the lead ended up after redirects. Later connections go there
    // directly, until it stops working.
    std::string directUrl;

    ResumeInfo info;       // what the sidecar file says, or will say
    std::string validator; // If-Range value for every ranged request
    bool resuming = false;
    bool keepInfo = false; // the sidecar file describes this download
    uint64_t unsaved = 0;  // bytes written since the sidecar was saved

    bool started = false; // the lead's response has been accepted
    bool cancelled = false;
    bool stopped = false;       // a failure ended the attempt
    bool restart = false;       // start again from the beginning
    bool restartSingle = false; // ... on one connection
    uint64_t total = 0;
    std::string filename;
    std::string effectiveUrl;
    std::string failure; // set by our own checks, overrides curl's message
    bool noSpace = false; // the SD card is too full for the file

    // The response status, and the failure that ended the attempt.
    long httpStatus = 0;
    CURLcode code = CURLE_OK;
    std::string detail;
};

std::string errnoText()
{
    return std::strerror(errno);
}

uint64_t receivedBytes(const Transfer &transfer)
{
    uint64_t total = 0;
    for (const Piece &piece : transfer.pieces) {
        total += piece.next - piece.start;
    }
    return total;
}

uint64_t savedBytes(const Transfer &transfer)
{
    uint64_t total = 0;
    for (const Piece &piece : transfer.pieces) {
        total += piece.flushed - piece.start;
    }
    return total;
}

bool allComplete(const Transfer &transfer)
{
    for (const Piece &piece : transfer.pieces) {
        if (!piece.complete()) {
            return false;
        }
    }
    return !transfer.pieces.empty();
}

// Writes the sidecar file. A split download saves as it goes, so a crash
// loses little, and its done ranges only ever cover data on the card.
void saveInfo(Transfer &transfer)
{
    if (!transfer.keepInfo) {
        return;
    }
    ResumeInfo info = transfer.info;
    info.split = transfer.split;
    info.done.clear();
    if (transfer.split) {
        for (const Piece &piece : transfer.pieces) {
            if (piece.flushed == piece.start) {
                continue;
            }
            if (!info.done.empty() && info.done.back().second == piece.start) {
                info.done.back().second = piece.flushed;
            } else {
                info.done.push_back({piece.start, piece.flushed});
            }
        }
    }
    if (!fs::writeFile(transfer.paths.info, serializeResumeInfo(info))) {
        transfer.keepInfo = false;
        fs::removeFile(transfer.paths.info);
    }
    transfer.unsaved = 0;
}

// Writes what connection has buffered to its place in the file.
bool flush(Transfer &transfer, Connection &connection)
{
    if (connection.buffer.empty()) {
        return true;
    }
    const size_t size = connection.buffer.size();
    size_t written = 0;
    if (fseeko(transfer.file, static_cast<off_t>(connection.bufferStart),
               SEEK_SET) == 0) {
        written = transfer.request->write
            ? transfer.request->write(connection.buffer.data(), size,
                                      transfer.file)
            : std::fwrite(connection.buffer.data(), 1, size, transfer.file);
    }
    connection.buffer.clear();
    if (written != size) {
        if (transfer.failure.empty()) {
            transfer.noSpace = errno == ENOSPC;
            transfer.failure = errno == ENOSPC
                ? "The SD card is full."
                : "Could not write to the SD card (" + errnoText() + ").";
        }
        return false;
    }
    connection.bufferStart += size;
    connection.piece->flushed = connection.bufferStart;
    transfer.unsaved += size;
    return true;
}

bool store(Transfer &transfer, Connection &connection, const char *data,
           size_t size)
{
    while (size > 0) {
        const size_t chunk =
            std::min(size, FileBufferSize - connection.buffer.size());
        connection.buffer.insert(connection.buffer.end(), data, data + chunk);
        connection.piece->next += chunk;
        data += chunk;
        size -= chunk;
        if (connection.buffer.size() == FileBufferSize &&
            !flush(transfer, connection)) {
            return false;
        }
    }
    return true;
}

// The total size a response describes. requestStart is where the request
// asked to begin.
uint64_t responseTotal(const Connection &connection, long status,
                       uint64_t requestStart)
{
    if (status == 206) {
        const int64_t total = contentRangeTotal(connection.contentRange);
        return total > 0 ? static_cast<uint64_t>(total) : 0;
    }
    curl_off_t length = -1;
    curl_easy_getinfo(connection.curl, CURLINFO_CONTENT_LENGTH_DOWNLOAD_T,
                      &length);
    return length >= 0 ? requestStart + static_cast<uint64_t>(length) : 0;
}

// Runs once the headers of the lead's final response are known, before any
// of its body is written. Returns false to abort the transfer.
bool begin(Transfer &transfer, Connection &lead)
{
    const Request &request = *transfer.request;
    transfer.started = true;

    long status = 0;
    curl_easy_getinfo(lead.curl, CURLINFO_RESPONSE_CODE, &status);
    char *effective = nullptr;
    curl_easy_getinfo(lead.curl, CURLINFO_EFFECTIVE_URL, &effective);
    transfer.httpStatus = status;
    transfer.effectiveUrl = effective != nullptr ? effective : transfer.url;
    transfer.directUrl = transfer.effectiveUrl;

    transfer.filename = naming::chooseFilename(lead.contentDisposition,
                                               transfer.effectiveUrl);

    const uint64_t offset = lead.piece->next;
    const uint64_t total = responseTotal(lead, status, offset);
    // A split download's file was laid out for the size the server gave
    // before, so a different size means a different file.
    if (transfer.split && total != transfer.total) {
        transfer.restart = true;
        return false;
    }
    transfer.total = total;

    transfer.observer->onFilename(transfer.filename);
    transfer.observer->onPhase(Phase::Downloading);

    if (transfer.total > request.maxFileSize) {
        transfer.failure = "The file is " + units::bytes(transfer.total) +
                           ", but the SD card's FAT32 format cannot store "
                           "files of 4 GiB or more.";
        return false;
    }
    if (request.freeSpace && transfer.total > transfer.fileBytes) {
        const int64_t available = request.freeSpace(request.folder);
        const uint64_t needed = transfer.total - transfer.fileBytes;
        if (available >= 0 && static_cast<uint64_t>(available) < needed) {
            transfer.noSpace = true;
            transfer.failure = "Not enough free space on the SD card: " +
                               units::bytes(needed) + " needed, " +
                               units::bytes(static_cast<uint64_t>(available)) +
                               " free.";
            return false;
        }
    }

    ResumeInfo &info = transfer.info;
    info.url = transfer.url;
    // A 206 answered our If-Range, so the stored validators still hold even
    // when the response leaves them out.
    if (status != 206) {
        info.etag = lead.etag;
        info.lastModified = lead.lastModified;
    }
    info.filename = transfer.filename;
    info.total = transfer.total;
    transfer.validator = ifRangeValidator(info);
    const std::string ranges = text::lower(lead.acceptRanges);
    transfer.keepInfo = request.allowResume && !transfer.validator.empty() &&
                        ranges != "none";

    // Splitting needs the size, proof that ranges work, and a validator so
    // every connection is sure to get the same version of the file.
    bool split = !transfer.split && transfer.allowSplit &&
                 request.connections > 1 && !transfer.validator.empty() &&
                 (status == 206 || ranges == "bytes") &&
                 transfer.total > offset &&
                 transfer.total - offset >= 2 * request.minPieceBytes;
    if (split) {
        transfer.split = true;
    }
    if (transfer.keepInfo) {
        saveInfo(transfer);
    } else {
        fs::removeFile(transfer.paths.info);
    }
    if (split && !transfer.keepInfo && fs::fileExists(transfer.paths.info)) {
        // A stale sidecar would make the grown file look downloaded.
        transfer.split = false;
        split = false;
    }
    if (split) {
        // The sidecar already says the file is split, so growing it can
        // never make missing bytes look downloaded. Growing it now claims
        // the space at once; if that fails, writes past the end grow it.
        std::fflush(transfer.file);
        if (ftruncate(fileno(transfer.file),
                      static_cast<off_t>(transfer.total)) != 0) {
            // Writes past the end of the file grow it instead.
        }
        lead.piece->end = transfer.total;
    }
    transfer.maxConnections =
        transfer.split ? std::max(1, request.connections) : 1;
    return true;
}

// Checks a later connection's response against the lead's before any of its
// body is written.
bool verify(Transfer &transfer, Connection &connection)
{
    long status = 0;
    curl_easy_getinfo(connection.curl, CURLINFO_RESPONSE_CODE, &status);
    // Ranged requests carry If-Range, so a 206 is the same version. Only a
    // request from byte 0 can get a 200, and its validators have to match.
    const bool same =
        responseTotal(connection, status, 0) == transfer.total &&
        (status == 206 || (connection.etag == transfer.info.etag &&
                           connection.lastModified ==
                               transfer.info.lastModified));
    if (!same) {
        transfer.restart = true;
        transfer.restartSingle = true;
    }
    return same;
}

size_t writeCallback(char *data, size_t size, size_t count, void *userdata)
{
    Connection &connection = *static_cast<Connection *>(userdata);
    Transfer &transfer = *connection.transfer;
    const size_t bytes = size * count;

    if (!connection.started) {
        connection.started = true;
        if (connection.lead ? !begin(transfer, connection)
                            : !verify(transfer, connection)) {
            return 0;
        }
    }

    Piece &piece = *connection.piece;
    const uint64_t room = piece.end - piece.next;
    const size_t take = room < bytes ? static_cast<size_t>(room) : bytes;
    if (take > 0 && !store(transfer, connection, data, take)) {
        return 0;
    }
    // Requests run to the end of the file, so every piece but the last one
    // stops its connection once it is full.
    if (take < bytes || (piece.complete() && piece.end != transfer.total)) {
        connection.cutOff = true;
        return 0;
    }
    return bytes;
}

size_t headerCallback(char *data, size_t size, size_t count, void *userdata)
{
    Connection &connection = *static_cast<Connection *>(userdata);
    const size_t bytes = size * count;
    const std::string line = text::trim(std::string(data, bytes));

    if (text::startsWithNoCase(line, "HTTP/")) {
        connection.contentDisposition.clear();
        connection.etag.clear();
        connection.lastModified.clear();
        connection.acceptRanges.clear();
        connection.contentRange.clear();
        return bytes;
    }

    const size_t colon = line.find(':');
    if (colon == std::string::npos) {
        return bytes;
    }
    const std::string name = text::lower(text::trim(line.substr(0, colon)));
    const std::string value = text::trim(line.substr(colon + 1));
    if (name == "content-disposition") {
        connection.contentDisposition = value;
    } else if (name == "etag") {
        connection.etag = value;
    } else if (name == "last-modified") {
        connection.lastModified = value;
    } else if (name == "accept-ranges") {
        connection.acceptRanges = value;
    } else if (name == "content-range") {
        connection.contentRange = value;
    }
    return bytes;
}

// Runs on each new socket before it connects, which is when the receive
// buffer has to be set for it to size the TCP window. Errors are ignored
// because the stack clamps sizes it does not like to its own limits.
int sockoptCallback(void *userdata, curl_socket_t socket, curlsocktype)
{
    const Transfer &transfer = *static_cast<Transfer *>(userdata);
    const int size = transfer.request->socketBufferSize;
    setsockopt(socket, SOL_SOCKET, SO_RCVBUF, &size, sizeof(size));
    setsockopt(socket, SOL_SOCKET, SO_SNDBUF, &size, sizeof(size));
    return CURL_SOCKOPT_OK;
}

// Failures after which the bytes received so far are still good to resume.
bool isTransient(CURLcode code, long httpStatus)
{
    switch (code) {
    case CURLE_COULDNT_RESOLVE_HOST:
    case CURLE_COULDNT_CONNECT:
    case CURLE_OPERATION_TIMEDOUT:
    case CURLE_PARTIAL_FILE:
    case CURLE_RECV_ERROR:
    case CURLE_SEND_ERROR:
    case CURLE_GOT_NOTHING:
    case CURLE_SSL_CONNECT_ERROR:
        return true;
    case CURLE_HTTP_RETURNED_ERROR:
        return httpStatus >= 500;
    default:
        return false;
    }
}

void configure(Transfer &transfer, Connection &connection)
{
    const Request &request = *transfer.request;
    const Piece &piece = *connection.piece;
    CURL *curl = connection.curl;
    const std::string &address =
        connection.lead || transfer.directUrl.empty() ? transfer.url
                                                      : transfer.directUrl;

    curl_easy_setopt(curl, CURLOPT_URL, address.c_str());
    if (!request.userAgent.empty()) {
        curl_easy_setopt(curl, CURLOPT_USERAGENT, request.userAgent.c_str());
    }
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_MAXREDIRS, MaxRedirects);
    curl_easy_setopt(curl, CURLOPT_PROTOCOLS_STR, "http,https");
    curl_easy_setopt(curl, CURLOPT_REDIR_PROTOCOLS_STR, "http,https");
    curl_easy_setopt(curl, CURLOPT_FAILONERROR, 1L);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT,
                     request.connectTimeoutSeconds);
    curl_easy_setopt(curl, CURLOPT_LOW_SPEED_LIMIT, 1L);
    curl_easy_setopt(curl, CURLOPT_LOW_SPEED_TIME,
                     request.stallTimeoutSeconds);
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(curl, CURLOPT_HTTP_VERSION, CURL_HTTP_VERSION_1_1);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 1L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 2L);
    useLocalClock(curl);
    if (!request.caFile.empty()) {
        curl_easy_setopt(curl, CURLOPT_CAINFO, request.caFile.c_str());
    }
    // Shares DNS answers and TLS sessions, so later connections skip most
    // of the handshake.
    curl_easy_setopt(curl, CURLOPT_SHARE, transfer.share);
    curl_easy_setopt(curl, CURLOPT_PRIVATE, &connection);
    curl_easy_setopt(curl, CURLOPT_ERRORBUFFER, connection.errorBuffer);
    curl_easy_setopt(curl, CURLOPT_BUFFERSIZE, CurlBufferSize);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, writeCallback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &connection);
    curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, headerCallback);
    curl_easy_setopt(curl, CURLOPT_HEADERDATA, &connection);
    if (request.socketBufferSize > 0) {
        curl_easy_setopt(curl, CURLOPT_SOCKOPTFUNCTION, sockoptCallback);
        curl_easy_setopt(curl, CURLOPT_SOCKOPTDATA, &transfer);
    }

    if (request.maxFileSize > piece.next) {
        curl_easy_setopt(curl, CURLOPT_MAXFILESIZE_LARGE,
                         static_cast<curl_off_t>(request.maxFileSize -
                                                 piece.next));
    }
    // Pieces past the start are only ever fetched with a validator, from a
    // resumed download or a split one.
    if (piece.next > 0) {
        curl_easy_setopt(curl, CURLOPT_RESUME_FROM_LARGE,
                         static_cast<curl_off_t>(piece.next));
        connection.headers = curl_slist_append(
            nullptr, ("If-Range: " + transfer.validator).c_str());
        curl_easy_setopt(curl, CURLOPT_HTTPHEADER, connection.headers);
    }
}

bool open(Transfer &transfer, Piece &piece, bool lead)
{
    std::unique_ptr<Connection> connection(new Connection());
    connection->transfer = &transfer;
    connection->piece = &piece;
    connection->lead = lead;
    connection->bufferStart = piece.next;
    connection->curl = curl_easy_init();
    if (connection->curl == nullptr) {
        return false;
    }
    connection->buffer.reserve(FileBufferSize);
    configure(transfer, *connection);
    if (curl_multi_add_handle(transfer.multi, connection->curl) != CURLM_OK) {
        curl_slist_free_all(connection->headers);
        curl_easy_cleanup(connection->curl);
        return false;
    }

    piece.connection = connection.get();
    transfer.connections.push_back(std::move(connection));
    const int count = static_cast<int>(transfer.connections.size());
    transfer.peakConnections = std::max(transfer.peakConnections, count);
    transfer.observer->onConnections(count);
    return true;
}

// Writes out what connection received, detaches it from its piece and frees
// it.
void close(Transfer &transfer, Connection &connection)
{
    flush(transfer, connection);
    connection.piece->connection = nullptr;
    curl_multi_remove_handle(transfer.multi, connection.curl);
    curl_easy_cleanup(connection.curl);
    curl_slist_free_all(connection.headers);

    std::vector<std::unique_ptr<Connection>> &all = transfer.connections;
    all.erase(std::find_if(all.begin(), all.end(),
                           [&](const std::unique_ptr<Connection> &entry) {
                               return entry.get() == &connection;
                           }));
    transfer.observer->onConnections(static_cast<int>(all.size()));
}

void closeAll(Transfer &transfer)
{
    while (!transfer.connections.empty()) {
        close(transfer, *transfer.connections.back());
    }
}

// Handles a connection whose request has ended.
void finish(Transfer &transfer, Connection &connection, CURLcode code)
{
    long status = 0;
    curl_easy_getinfo(connection.curl, CURLINFO_RESPONSE_CODE, &status);
    if (connection.lead && !transfer.started) {
        transfer.httpStatus = status;
        char *effective = nullptr;
        curl_easy_getinfo(connection.curl, CURLINFO_EFFECTIVE_URL, &effective);
        if (effective != nullptr) {
            transfer.effectiveUrl = effective;
        }
        // An empty body never reaches the write callback.
        if (code == CURLE_OK && !begin(transfer, connection)) {
            code = CURLE_WRITE_ERROR;
        }
    }
    const std::string detail = connection.errorBuffer[0] != '\0'
        ? connection.errorBuffer
        : curl_easy_strerror(code);
    const bool cutOff = connection.cutOff;
    const bool direct = !connection.lead;
    Piece &piece = *connection.piece;
    close(transfer, connection);

    if (transfer.stopped || transfer.cancelled) {
        return;
    }
    const bool ours = !transfer.failure.empty() || transfer.restart;
    if (cutOff && !ours) {
        return; // its piece is full
    }
    if (code == CURLE_OK && !ours) {
        if (piece.end == Unknown) {
            piece.end = piece.next; // the server said where the file ends
        }
        if (piece.complete()) {
            return;
        }
        code = CURLE_PARTIAL_FILE;
    }
    if (code == CURLE_OK) {
        code = CURLE_WRITE_ERROR;
    }

    const auto stop = [&]() {
        transfer.stopped = true;
        transfer.code = code;
        transfer.httpStatus = status;
        transfer.detail = detail;
    };
    if (ours) {
        stop();
        return;
    }
    if (code == CURLE_RANGE_ERROR ||
        (code == CURLE_HTTP_RETURNED_ERROR && status == 416)) {
        // From the lead resuming an earlier download, the file has changed.
        // From any later connection, the server mishandles ranges, so the
        // next attempt makes do with one connection.
        transfer.restart = true;
        transfer.restartSingle = transfer.started;
        stop();
        return;
    }
    if (transfer.split && transfer.started && transfer.reconnectsLeft > 0 &&
        (isTransient(code, status) || code == CURLE_HTTP_RETURNED_ERROR)) {
        --transfer.reconnectsLeft;
        if (code == CURLE_HTTP_RETURNED_ERROR) {
            // Some servers turn extra connections away, and the signed links
            // that downloads often redirect to expire. Use fewer connections
            // and go through the original link again.
            transfer.maxConnections = std::max(
                1, static_cast<int>(transfer.connections.size()));
            if (direct) {
                transfer.directUrl.clear();
            }
        }
        return; // the piece is idle again and schedule() reopens it
    }
    stop();
}

// Splits the piece with the most left to fetch, if that is worth another
// connection, and returns the new second half.
Piece *splitLargest(Transfer &transfer)
{
    const uint64_t minimum =
        std::max<uint64_t>(transfer.request->minPieceBytes, 1);
    auto largest = transfer.pieces.end();
    uint64_t left = 0;
    for (auto piece = transfer.pieces.begin(); piece != transfer.pieces.end();
         ++piece) {
        if (piece->connection != nullptr && piece->end != Unknown &&
            piece->end - piece->next > left) {
            largest = piece;
            left = piece->end - piece->next;
        }
    }
    if (largest == transfer.pieces.end() || left < 2 * minimum) {
        return nullptr;
    }
    uint64_t middle = largest->next + left / 2;
    middle -= middle % SplitAlignment;
    if (middle <= largest->next) {
        return nullptr;
    }

    Piece half;
    half.start = half.next = half.flushed = middle;
    half.end = largest->end;
    largest->end = middle;
    return &*transfer.pieces.insert(std::next(largest), half);
}

// Opens connections for idle pieces, and splits pieces in flight when none
// are idle, until maxConnections are open.
void schedule(Transfer &transfer)
{
    if (!transfer.started) {
        return;
    }
    while (static_cast<int>(transfer.connections.size()) <
           transfer.maxConnections) {
        Piece *piece = nullptr;
        for (Piece &candidate : transfer.pieces) {
            if (candidate.connection == nullptr && !candidate.complete()) {
                piece = &candidate;
                break;
            }
        }
        if (piece == nullptr && transfer.split) {
            piece = splitLargest(transfer);
        }
        if (piece == nullptr) {
            return;
        }
        if (!open(transfer, *piece, false)) {
            transfer.failure = "Could not start the HTTPS client.";
            transfer.stopped = true;
            transfer.code = CURLE_FAILED_INIT;
            return;
        }
    }
}

// Drives every connection until the attempt is over.
void run(Transfer &transfer)
{
    while (true) {
        int running = 0;
        curl_multi_perform(transfer.multi, &running);
        int queued = 0;
        while (CURLMsg *message = curl_multi_info_read(transfer.multi,
                                                       &queued)) {
            if (message->msg != CURLMSG_DONE) {
                continue;
            }
            // message does not outlive the handle, which finish() frees.
            const CURLcode code = message->data.result;
            Connection *connection = nullptr;
            curl_easy_getinfo(message->easy_handle, CURLINFO_PRIVATE,
                              &connection);
            finish(transfer, *connection, code);
        }

        if (!transfer.cancelled && transfer.observer->cancelRequested()) {
            transfer.cancelled = true;
        }
        if (!transfer.cancelled && !transfer.stopped) {
            if (transfer.split && transfer.unsaved >= SaveInterval) {
                saveInfo(transfer);
            }
            schedule(transfer);
        }
        if (transfer.cancelled || transfer.stopped) {
            closeAll(transfer);
            return;
        }
        transfer.observer->onProgress(receivedBytes(transfer),
                                      transfer.started ? transfer.total : 0);
        if (transfer.connections.empty()) {
            return;
        }
        curl_multi_poll(transfer.multi, nullptr, 0, PollMilliseconds,
                        nullptr);
    }
}

// Picks up the earlier partial download that info describes. Leaves the
// transfer alone when there is nothing usable to resume.
void restore(Transfer &transfer, const ResumeInfo &info, uint64_t fileSize)
{
    std::list<Piece> pieces;
    const auto add = [&](uint64_t start, uint64_t done, uint64_t end) {
        Piece piece;
        piece.start = start;
        piece.next = piece.flushed = done;
        piece.end = end;
        pieces.push_back(piece);
    };
    if (info.split) {
        if (info.total == 0) {
            return;
        }
        uint64_t cursor = 0;
        for (const auto &range : info.done) {
            if (range.first < cursor || range.second > info.total) {
                return;
            }
            if (range.first > cursor) {
                add(cursor, cursor, range.first);
            }
            add(range.first, range.second, range.second);
            cursor = range.second;
        }
        if (cursor < info.total) {
            add(cursor, cursor, info.total);
        }
    } else {
        add(0, fileSize, Unknown);
    }

    transfer.pieces = pieces;
    if (receivedBytes(transfer) == 0) {
        transfer.pieces.clear();
        return;
    }
    transfer.split = info.split;
    transfer.total = info.split ? info.total : 0;
    transfer.info = info;
    transfer.validator = ifRangeValidator(info);
    transfer.filename = info.filename;
    transfer.resuming = true;
    transfer.keepInfo = true;
}

} // namespace

Result download(const Request &request, Observer &observer)
{
    Result result;
    const std::string address = text::trim(request.url);
    if (!url::isHttpUrl(address)) {
        result.message = "Only http:// and https:// links can be downloaded.";
        return result;
    }

    Request normalised = request;
    normalised.folder = naming::normaliseFolder(request.folder);
    if (!fs::directoryExists(normalised.folder)) {
        result.message = "The folder " + normalised.folder +
                         " does not exist.";
        return result;
    }
    const PartialPaths paths = partialPaths(normalised.folder, address);

    bool allowSplit = true;
    for (int attempt = 0; attempt < MaxAttempts; ++attempt) {
        Transfer transfer;
        transfer.request = &normalised;
        transfer.observer = &observer;
        transfer.url = address;
        transfer.paths = paths;
        transfer.allowSplit = allowSplit;
        const auto began = std::chrono::steady_clock::now();

        // Continue an earlier download only when it was for the same URL and
        // the server gave a validator we can send in If-Range. Otherwise any
        // leftover data could belong to a different version of the file.
        std::string stored;
        ResumeInfo previous;
        const int64_t existing = fs::fileSize(paths.data);
        if (normalised.allowResume && attempt == 0 && existing > 0 &&
            fs::readFile(paths.info, stored) &&
            parseResumeInfo(stored, previous) && previous.url == address &&
            !ifRangeValidator(previous).empty()) {
            restore(transfer, previous, static_cast<uint64_t>(existing));
        }
        if (!transfer.resuming) {
            fs::removeFile(paths.data);
            fs::removeFile(paths.info);
            transfer.pieces.assign(1, Piece());
        }

        observer.onPhase(attempt == 0 ? Phase::Connecting : Phase::Restarting);

        transfer.file = std::fopen(paths.data.c_str(),
                                   transfer.resuming ? "r+b" : "w+b");
        if (transfer.file == nullptr) {
            result.message = "Could not create a file in " +
                             normalised.folder + " (" + errnoText() + ").";
            return result;
        }
        // Each connection buffers its own writes for its own place in the
        // file, so stdio's buffer would only add a copy.
        std::setvbuf(transfer.file, nullptr, _IONBF, 0);
        transfer.fileBytes = transfer.resuming
            ? static_cast<uint64_t>(existing)
            : 0;

        transfer.multi = curl_multi_init();
        transfer.share = curl_share_init();
        if (transfer.share != nullptr) {
            curl_share_setopt(transfer.share, CURLSHOPT_SHARE,
                              CURL_LOCK_DATA_DNS);
            curl_share_setopt(transfer.share, CURLSHOPT_SHARE,
                              CURL_LOCK_DATA_SSL_SESSION);
        }
        const uint64_t resumedBytes = receivedBytes(transfer);
        if (transfer.resuming) {
            observer.onResumed(resumedBytes);
        }
        Piece *first = nullptr;
        for (Piece &piece : transfer.pieces) {
            if (!piece.complete()) {
                first = &piece;
                break;
            }
        }
        if (transfer.multi == nullptr || transfer.share == nullptr ||
            (first != nullptr && !open(transfer, *first, true))) {
            curl_multi_cleanup(transfer.multi);
            curl_share_cleanup(transfer.share);
            std::fclose(transfer.file);
            if (!transfer.resuming) {
                fs::removeFile(paths.data);
            }
            result.message = "Could not start the HTTPS client.";
            return result;
        }
        if (first != nullptr) {
            run(transfer);
        }
        curl_multi_cleanup(transfer.multi);
        curl_share_cleanup(transfer.share);
        const bool closed = std::fclose(transfer.file) == 0;

        result.httpStatus = transfer.httpStatus;
        result.curlCode = transfer.code;
        result.filename = transfer.filename;
        result.effectiveUrl = transfer.effectiveUrl.empty()
            ? address
            : transfer.effectiveUrl;
        result.resumed = transfer.resuming;
        result.connections = transfer.peakConnections;
        result.transferred = receivedBytes(transfer) - resumedBytes;
        result.seconds = std::chrono::duration<double>(
                             std::chrono::steady_clock::now() - began)
                             .count();

        if (transfer.cancelled) {
            const bool keep = observer.keepPartialOnCancel() &&
                              normalised.allowResume && closed;
            if (keep) {
                saveInfo(transfer);
            }
            const uint64_t kept = savedBytes(transfer);
            if (keep && kept > 0 && transfer.keepInfo &&
                fs::fileExists(paths.info)) {
                result.resumable = true;
                result.partPath = paths.data;
                result.bytes = kept;
            } else {
                fs::removeFile(paths.data);
                fs::removeFile(paths.info);
            }
            result.outcome = Outcome::Cancelled;
            result.message = "Download cancelled.";
            return result;
        }

        if (transfer.restart) {
            fs::removeFile(paths.data);
            fs::removeFile(paths.info);
            if (transfer.restartSingle) {
                allowSplit = false;
            }
            continue;
        }

        if (!transfer.stopped && allComplete(transfer) && closed) {
            fs::removeFile(paths.info);
            result.outcome = Outcome::Completed;
            result.partPath = paths.data;
            const int64_t size = fs::fileSize(paths.data);
            result.bytes = size > 0 ? static_cast<uint64_t>(size) : 0;
            if (result.filename.empty()) {
                result.filename = naming::DefaultFilename;
            }
            return result;
        }

        CURLcode code = transfer.code;
        if (!transfer.stopped && code == CURLE_OK) {
            code = CURLE_PARTIAL_FILE;
        }
        result.outcome = Outcome::Failed;
        result.curlCode = code;
        result.detail = !transfer.detail.empty() ? transfer.detail
                                                 : curl_easy_strerror(code);
        result.certificateProblem = code == CURLE_PEER_FAILED_VERIFICATION;
        result.noSpace = transfer.noSpace;
        if (!transfer.failure.empty()) {
            result.message = transfer.failure;
        } else if (!closed) {
            result.message = "Could not finish writing to the SD card.";
        } else {
            result.message =
                describeFailure(code, result.httpStatus, result.detail);
        }

        const bool resumable = normalised.allowResume && closed &&
                               transfer.failure.empty() &&
                               isTransient(code, result.httpStatus);
        if (resumable) {
            saveInfo(transfer);
        }
        const uint64_t kept = savedBytes(transfer);
        if (resumable && kept > 0 && transfer.keepInfo &&
            fs::fileExists(paths.info)) {
            result.resumable = true;
            result.partPath = paths.data;
            result.bytes = kept;
        } else {
            fs::removeFile(paths.data);
            fs::removeFile(paths.info);
        }
        return result;
    }

    result.outcome = Outcome::Failed;
    result.message = "The server kept rejecting the request to continue the "
                     "download.";
    return result;
}

uint64_t partialSize(const std::string &folder, const std::string &url)
{
    const PartialPaths paths =
        partialPaths(naming::normaliseFolder(folder), text::trim(url));
    const int64_t size = fs::fileSize(paths.data);
    std::string stored;
    ResumeInfo info;
    if (size <= 0 || !fs::readFile(paths.info, stored) ||
        !parseResumeInfo(stored, info)) {
        return 0;
    }
    if (!info.split) {
        return static_cast<uint64_t>(size);
    }
    uint64_t done = 0;
    for (const auto &range : info.done) {
        done += range.second - range.first;
    }
    return done;
}

void discardPartial(const std::string &folder, const std::string &url)
{
    const PartialPaths paths =
        partialPaths(naming::normaliseFolder(folder), text::trim(url));
    fs::removeFile(paths.data);
    fs::removeFile(paths.info);
}

std::string describeFailure(int curlCode, long httpStatus,
                            const std::string &detail)
{
    const auto mentions = [&](const char *needle) {
        return text::lower(detail).find(text::lower(needle)) !=
               std::string::npos;
    };

    switch (static_cast<CURLcode>(curlCode)) {
    case CURLE_OK:
        return "";
    case CURLE_UNSUPPORTED_PROTOCOL:
        return "Only http:// and https:// links are supported, and the "
               "server redirected somewhere else.";
    case CURLE_URL_MALFORMAT:
        return "That link is not a valid URL.";
    case CURLE_COULDNT_RESOLVE_HOST:
        return "Could not find the server. Check the Wi-Fi connection and "
               "the link.";
    case CURLE_COULDNT_CONNECT:
        return "Could not connect to the server. Check the Wi-Fi connection.";
    case CURLE_OPERATION_TIMEDOUT:
        return "The connection timed out because the server stopped "
               "responding.";
    case CURLE_PEER_FAILED_VERIFICATION:
        // mbedTLS (on the console) and OpenSSL (in tests) word these
        // differently, so both spellings are recognised.
        if (mentions("BADCERT_FUTURE") || mentions("not yet valid")) {
            return "The server's certificate is not valid yet according to "
                   "the console's clock, so the console's date is probably "
                   "behind. Check the clock and try again.";
        }
        if (mentions("BADCERT_EXPIRED") || mentions("expired")) {
            return "The server's certificate has expired according to the "
                   "console's clock. Check that the console's date is "
                   "right; if it is, the site's certificate really has "
                   "expired.";
        }
        if (mentions("CN_MISMATCH") || mentions("subject name") ||
            mentions("does not match")) {
            return "The server's certificate is for a different site than "
                   "the link.";
        }
        if (mentions("NOT_TRUSTED") || mentions("self-signed") ||
            mentions("self signed") || mentions("local issuer")) {
            return "The server's certificate is not signed by an authority "
                   "3dsDL trusts.";
        }
        return "The server's certificate could not be verified. If the "
               "console's date and time are wrong, fix them with Check "
               "clock and try again.";
    case CURLE_SSL_CONNECT_ERROR:
        return "The secure connection to the server failed.";
    case CURLE_SSL_CACERT_BADFILE:
        return "3dsDL could not load its certificate store.";
    case CURLE_TOO_MANY_REDIRECTS:
        return "The link redirected too many times.";
    case CURLE_FILESIZE_EXCEEDED:
        return "The file is too large for the SD card's FAT32 format "
               "(4 GiB or more).";
    case CURLE_PARTIAL_FILE:
    case CURLE_RECV_ERROR:
    case CURLE_SEND_ERROR:
    case CURLE_GOT_NOTHING:
        return "The connection dropped before the download finished.";
    case CURLE_WRITE_ERROR:
        return "Could not write to the SD card.";
    case CURLE_RANGE_ERROR:
        return "The server refused to continue the earlier download.";
    case CURLE_HTTP_RETURNED_ERROR:
        switch (httpStatus) {
        case 401:
        case 403:
            return "The server refused access (HTTP " +
                   std::to_string(httpStatus) +
                   "). The link may need a login or may have expired.";
        case 404:
        case 410:
            return "The file was not found on the server (HTTP " +
                   std::to_string(httpStatus) + ").";
        case 429:
            return "The server is limiting requests (HTTP 429). Try again "
                   "later.";
        default:
            break;
        }
        if (httpStatus >= 500) {
            return "The server had an error (HTTP " +
                   std::to_string(httpStatus) + "). Try again later.";
        }
        return "The server answered with HTTP " + std::to_string(httpStatus) +
               ".";
    default:
        break;
    }
    return std::string("The download failed: ") +
           curl_easy_strerror(static_cast<CURLcode>(curlCode)) + ".";
}

} // namespace net
