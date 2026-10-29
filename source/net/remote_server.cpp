#include "remote_server.hpp"

#include "file_ops.hpp"
#include "file_types.hpp"
#include "fs_util.hpp"
#include "remote.hpp"
#include "socket_wait.hpp"
#include "title_meta.hpp"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>
#include <zlib.h>

#include <cerrno>
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <cinttypes>

namespace net {

namespace {

// How long a sender gets to finish sending once it has connected.
constexpr int ReadTimeoutMs = 5000;
// A file transfer may take minutes, but gives up after this long without a
// byte going either way.
constexpr int IdleTimeoutMs = 30000;
// Browsers open spare connections that may never send anything. One that is
// still silent after this long gives way to the next one waiting.
constexpr int SpareConnectionMs = 300;
// Bytes gathered before each write to the SD card, which is much faster at
// a few large writes than at many small ones.
constexpr size_t WriteChunk = 256 * 1024;
// How often a wait asks whether a socket is ready: often while a request
// is being served, less often while nobody is connected.
constexpr int ActiveStepMs = 1;
constexpr int IdleStepMs = 5;
constexpr size_t ReadChunk = 64 * 1024;
constexpr int ProgressEveryMs = 200;
// How long a wait for data goes before asking whether to stop.
constexpr int StopCheckMs = 250;
// Finding when a file changed makes the SD card search its folder, which in
// a folder of hundreds of files takes about a tenth of a second each. Bigger
// folders are listed without times, and the page asks for them in batches.
constexpr size_t TimedEntries = 64;
constexpr size_t TimesPerRequest = 50;
// Checksums for the page to tell files apart: a request stops after the
// file that takes it past this many bytes, so each one is quick.
constexpr uint64_t HashBytesPerRequest = 16 * 1024 * 1024;
// A replaced file up to this size is copied over the old one in place,
// which costs one search of its folder; bigger ones are renamed instead,
// as copying them again would take longer than the searches.
constexpr uint64_t OverwriteBytes = 512 * 1024;
// How long a finished connection waits for the other side to close first.
constexpr int LingerMs = 2000;

void closeSocket(int &socketFd)
{
    if (socketFd >= 0) {
        ::close(socketFd);
        socketFd = -1;
    }
}

void quietPipe(int socketFd)
{
#ifdef SO_NOSIGPIPE
    const int on = 1;
    setsockopt(socketFd, SOL_SOCKET, SO_NOSIGPIPE, &on, sizeof(on));
#else
    (void)socketFd;
#endif
}

int sendFlags()
{
#ifdef MSG_NOSIGNAL
    return MSG_NOSIGNAL;
#else
    return 0;
#endif
}

bool listenOn(unsigned short port, int &socketFd, unsigned short &bound,
              std::string &error)
{
    socketFd = socket(AF_INET, SOCK_STREAM, 0);
    if (socketFd < 0) {
        error = "Could not open a network socket.";
        return false;
    }
    const int on = 1;
    setsockopt(socketFd, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on));
    sockaddr_in address {};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_ANY);
    address.sin_port = htons(port);
    if (bind(socketFd, reinterpret_cast<sockaddr *>(&address),
             sizeof(address)) != 0 ||
        listen(socketFd, 4) != 0) {
        error = "Port " + std::to_string(port) + " is in use.";
        closeSocket(socketFd);
        return false;
    }
    socklen_t length = sizeof(address);
    if (getsockname(socketFd, reinterpret_cast<sockaddr *>(&address),
                    &length) == 0) {
        bound = ntohs(address.sin_port);
    } else {
        bound = port;
    }
    return true;
}

using Clock = std::chrono::steady_clock;

// Reads up to size bytes, waiting no later than deadline. Returns bytes
// read, 0 at the end of the stream, -1 on error or timeout. It tries the
// read first, so a busy connection costs one call per read, not two.
// Bytes read, 0 when the other side closed, -1 on an error and -2 when
// nothing came before deadline.
ssize_t readSome(int socketFd, void *buffer, size_t size, Clock::time_point deadline)
{
    while (true) {
        const ssize_t got = recv(socketFd, buffer, size, MSG_DONTWAIT);
        if (got >= 0) {
            return got;
        }
        if (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) {
            return -1;
        }
        const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(
                              deadline - Clock::now())
                              .count();
        if (left <= 0) {
            return -2;
        }
        pollfd waiting {socketFd, POLLIN, 0};
        if (waitSockets(&waiting, 1, static_cast<int>(left), ActiveStepMs) < 0) {
            return -1;
        }
    }
}

bool readExactly(int socketFd, void *buffer, size_t size,
                 Clock::time_point deadline)
{
    unsigned char *bytes = static_cast<unsigned char *>(buffer);
    size_t done = 0;
    while (done < size) {
        const ssize_t got = readSome(socketFd, bytes + done, size - done,
                                     deadline);
        if (got <= 0) {
            return false;
        }
        done += static_cast<size_t>(got);
    }
    return true;
}

// Sends everything, giving up when the other side takes nothing for
// idleMs.
bool sendBytes(int socketFd, const char *data, size_t size, int idleMs)
{
    size_t sent = 0;
    while (sent < size) {
        const ssize_t wrote = send(socketFd, data + sent, size - sent,
                                   sendFlags() | MSG_DONTWAIT);
        if (wrote > 0) {
            sent += static_cast<size_t>(wrote);
            continue;
        }
        if (wrote < 0 && errno != EAGAIN && errno != EWOULDBLOCK &&
            errno != EINTR) {
            return false;
        }
        pollfd waiting {socketFd, POLLOUT, 0};
        if (waitSockets(&waiting, 1, idleMs, ActiveStepMs) <= 0) {
            return false;
        }
    }
    return true;
}

void sendAll(int socketFd, const std::string &data)
{
    sendBytes(socketFd, data.data(), data.size(), ReadTimeoutMs);
}

// Closes a connection that was sent an answer, without losing its end.
// The 3DS network service resets a non-blocking socket that is closed with
// data still waiting to go out, and drops that data: the last kilobyte of
// a page or a download never arrived. Shutting down the sending side
// queues the end of the stream behind the data instead, and the other side
// closes once it has read everything.
void closeClient(int &socketFd)
{
    if (socketFd < 0) {
        return;
    }
    shutdown(socketFd, SHUT_WR);
    const Clock::time_point deadline =
        Clock::now() + std::chrono::milliseconds(LingerMs);
    char rest[512];
    while (readSome(socketFd, rest, sizeof(rest), deadline) > 0) {
    }
    ::close(socketFd);
    socketFd = -1;
}

// Waits for a connection's first bytes. False when nothing comes before
// deadline, or when it stays silent while another connection is waiting.
bool firstBytes(int client, int listener, Clock::time_point deadline)
{
    const Clock::time_point started = Clock::now();
    while (true) {
        const Clock::time_point now = Clock::now();
        if (now >= deadline) {
            return false;
        }
        const int left = static_cast<int>(
            std::chrono::duration_cast<std::chrono::milliseconds>(deadline -
                                                                  now)
                .count());
        pollfd waiting[2] = {{client, POLLIN, 0}, {listener, POLLIN, 0}};
        if (waitSockets(waiting, 2, std::min(left, 100), ActiveStepMs) < 0) {
            return false;
        }
        if (waiting[0].revents != 0) {
            return true;
        }
        if ((waiting[1].revents & POLLIN) != 0 &&
            Clock::now() - started >=
                std::chrono::milliseconds(SpareConnectionMs)) {
            return false;
        }
    }
}

void sendJson(int client, const std::string &json)
{
    sendAll(client, remote::httpResponse(200, "application/json", json));
}

// Reports a transfer to FileSharing::progress a few times a second.
class Reporter {
public:
    Reporter(const FileSharing &sharing, const std::string &name,
             const std::string &from, bool upload, uint64_t total)
        : sharing_(sharing)
    {
        transfer_.name = name;
        transfer_.from = from;
        transfer_.upload = upload;
        transfer_.total = total;
        report();
    }

    void advance(uint64_t done)
    {
        transfer_.done = done;
        if (Clock::now() - last_ >=
            std::chrono::milliseconds(ProgressEveryMs)) {
            report();
        }
    }

    void finish(bool ok)
    {
        transfer_.finished = true;
        transfer_.ok = ok;
        report();
    }

private:
    void report()
    {
        last_ = Clock::now();
        if (sharing_.progress) {
            sharing_.progress(transfer_);
        }
    }

    const FileSharing &sharing_;
    Transfer transfer_;
    Clock::time_point last_;
};

bool stopping(const FileSharing &sharing)
{
    return sharing.cancelled && sharing.cancelled();
}

// Gives up a delete when the listener is stopping.
class StopWith : public fileops::Observer {
public:
    explicit StopWith(const FileSharing &sharing) : sharing_(sharing) {}
    bool cancelRequested() override { return stopping(sharing_); }

private:
    const FileSharing &sharing_;
};

// The folder that holds path: "sdmc:/a/b" -> "sdmc:/a", "sdmc:/a" ->
// "sdmc:/".
std::string folderOf(const std::string &path)
{
    const size_t slash = path.rfind('/');
    const std::string folder = path.substr(0, slash);
    return folder.find('/') == std::string::npos ? folder + "/" : folder;
}

bool isRoot(const FileSharing &sharing, const std::string &path)
{
    return path == sharing.root + "/";
}

// Reads the rest of a small body into request.body.
bool readBody(int client, std::string &data, size_t bodyStart,
              remote::HttpRequest &request, Clock::time_point deadline)
{
    if (request.contentLength > 64 * 1024) {
        sendAll(client, remote::jsonError(413, "That request is too large."));
        return false;
    }
    const size_t end = bodyStart + static_cast<size_t>(request.contentLength);
    char buffer[4096];
    while (data.size() < end) {
        const ssize_t got = readSome(client, buffer, sizeof(buffer), deadline);
        if (got <= 0) {
            return false;
        }
        data.append(buffer, static_cast<size_t>(got));
    }
    request.body = data.substr(bodyStart, end - bodyStart);
    return true;
}

void listFolder(int client, const FileSharing &sharing,
                const std::string &webPath, const std::string &local,
                bool hidden, bool times)
{
    bool ok = false;
    std::vector<fs::Entry> entries =
        fs::listEntries(local, &ok, hidden, false);
    if (!ok) {
        sendAll(client, remote::jsonError(404, "That folder does not exist."));
        return;
    }
    times = times && entries.size() <= TimedEntries;
    if (times) {
        const std::string base = local.back() == '/' ? local : local + "/";
        for (fs::Entry &entry : entries) {
            entry.modified = fs::modifiedTime(base + entry.name);
        }
    }
    const int64_t free = sharing.freeSpace ? sharing.freeSpace() : -1;
    const int64_t total = sharing.totalSpace ? sharing.totalSpace() : -1;
    std::string json = "{\"path\":" + remote::jsonString(webPath) +
                       ",\"free\":" + std::to_string(free) +
                       ",\"total\":" + std::to_string(total) +
                       ",\"times\":" + (times ? "true" : "false") +
                       ",\"entries\":[";
    bool first = true;
    for (const fs::Entry &entry : entries) {
        if (entry.name == ".3dsdl-upload.part") {
            continue; // an upload in progress, or one cut off
        }
        json += std::string(first ? "" : ",") +
                "{\"name\":" + remote::jsonString(entry.name) +
                ",\"dir\":" + (entry.folder ? "true" : "false") +
                ",\"size\":" + std::to_string(entry.size) +
                ",\"time\":" + std::to_string(entry.modified) + "}";
        first = false;
    }
    sendJson(client, json + "]}");
}

// When each of names (one per line) in local last changed, for a folder
// listed without times.
void sendTimes(int client, const FileSharing &sharing, const std::string &local,
               const std::string &names)
{
    const std::string base = local.back() == '/' ? local : local + "/";
    std::string json = "{\"times\":{";
    size_t start = 0;
    size_t count = 0;
    while (start < names.size() && count < TimesPerRequest && !stopping(sharing)) {
        size_t end = names.find('\n', start);
        if (end == std::string::npos) {
            end = names.size();
        }
        const std::string name = names.substr(start, end - start);
        start = end + 1;
        if (name.empty() || name.find('/') != std::string::npos ||
            name == "." || name == "..") {
            continue;
        }
        json += std::string(count == 0 ? "" : ",") + remote::jsonString(name) +
                ":" + std::to_string(fs::modifiedTime(base + name));
        ++count;
    }
    sendJson(client, json + "}}");
}

// The CRC-32 of each of names (one per line) in local, as 8 hex digits, or
// "" for one that cannot be read. Answers for the first names only when
// they add up to many bytes; the page asks again for the rest.
void sendHashes(int client, const FileSharing &sharing, const std::string &local,
                const std::string &names)
{
    const std::string base = local.back() == '/' ? local : local + "/";
    std::string json = "{\"hashes\":{";
    std::vector<unsigned char> buffer(ReadChunk);
    uint64_t bytes = 0;
    size_t start = 0;
    bool first = true;
    while (start < names.size() && bytes < HashBytesPerRequest &&
           !stopping(sharing)) {
        size_t end = names.find('\n', start);
        if (end == std::string::npos) {
            end = names.size();
        }
        const std::string name = names.substr(start, end - start);
        start = end + 1;
        if (name.empty() || name.find('/') != std::string::npos ||
            name == "." || name == "..") {
            continue;
        }
        std::string hex;
        if (FILE *file = std::fopen((base + name).c_str(), "rb")) {
            // Buffered, each read would go to the card a kilobyte at a time.
            std::setvbuf(file, nullptr, _IONBF, 0);
            uLong crc = crc32(0L, Z_NULL, 0);
            size_t got = 0;
            while (!stopping(sharing) &&
                   (got = std::fread(buffer.data(), 1, buffer.size(), file)) > 0) {
                crc = crc32(crc, buffer.data(), static_cast<uInt>(got));
                bytes += got;
            }
            if (!std::ferror(file) && !stopping(sharing)) {
                char text[9];
                std::snprintf(text, sizeof(text), "%08lx",
                              static_cast<unsigned long>(crc & 0xffffffffUL));
                hex = text;
            }
            std::fclose(file);
        }
        json += std::string(first ? "" : ",") + remote::jsonString(name) +
                ":" + remote::jsonString(hex);
        first = false;
    }
    sendJson(client, json + "}}");
}

// The name, publisher and icon inside a CIA, a 3DSX or a game image.
struct Title {
    std::string name;
    std::string publisher;
    std::string product;
    uint64_t titleId = 0;
    bool hasTitleId = false;
    int version = -1;
    std::string note;
    std::vector<uint16_t> icon;
};

bool readTitle(const std::string &local, Title &title, std::string &error)
{
    const filetype::Kind kind = filetype::fromName(local);
    if (kind == filetype::Kind::Cia) {
        // Quick, as a .zcia's end could keep the server busy for seconds.
        cia::Info info;
        if (!cia::readInfo(local, info, error, true)) {
            return false;
        }
        title.name = !info.smdh.englishShortName.empty()
                         ? info.smdh.englishShortName
                         : info.smdh.shortName;
        title.publisher = info.smdh.publisher;
        title.titleId = info.titleId;
        title.hasTitleId = true;
        title.version = info.version;
        title.icon = info.smdh.icon;
        return true;
    }
    if (!filetype::hasTitleMeta(kind)) {
        error = "This kind of file has no title.";
        return false;
    }
    titlemeta::Meta meta;
    if (!titlemeta::read(local, kind, meta, error)) {
        return false;
    }
    title.name = meta.name;
    title.publisher = meta.publisher;
    title.product = meta.productCode;
    title.titleId = meta.titleId;
    title.hasTitleId = meta.hasTitleId;
    title.note = meta.note;
    title.icon = meta.icon;
    return true;
}

void sendTitle(int client, const FileSharing &sharing, const std::string &local,
               bool iconOnly)
{
    Title title;
    std::string error;
    if (!readTitle(local, title, error)) {
        sendAll(client, remote::jsonError(404, error));
        return;
    }
    if (iconOnly) {
        const std::string bmp = remote::iconBmp(title.icon);
        if (bmp.empty()) {
            sendAll(client, remote::jsonError(404, "It has no icon."));
            return;
        }
        // The page asks with the file's time in the address, so a changed
        // file gets a new icon and an unchanged one comes from the cache.
        sendAll(client, "HTTP/1.1 200 OK\r\nContent-Type: image/bmp\r\n"
                        "Content-Length: " +
                            std::to_string(bmp.size()) +
                            "\r\nCache-Control: max-age=86400\r\n"
                            "X-Content-Type-Options: nosniff\r\n"
                            "Connection: close\r\n\r\n" +
                            bmp);
        return;
    }
    char titleId[17] = "";
    if (title.hasTitleId) {
        std::snprintf(titleId, sizeof(titleId), "%016" PRIX64, title.titleId);
    }
    sendJson(client, "{\"name\":" + remote::jsonString(title.name) +
                         ",\"publisher\":" + remote::jsonString(title.publisher) +
                         ",\"product\":" + remote::jsonString(title.product) +
                         ",\"titleId\":" + remote::jsonString(titleId) +
                         ",\"version\":" + std::to_string(title.version) +
                         ",\"versionText\":" +
                         remote::jsonString(
                             title.version < 0
                                 ? ""
                                 : cia::versionText(title.titleId,
                                                    static_cast<uint16_t>(title.version),
                                                    sharing.appVersion)) +
                         ",\"note\":" + remote::jsonString(title.note) +
                         ",\"icon\":" + (title.icon.size() == 48 * 48 ? "true" : "false") +
                         "}");
}

const char *accessName(Access access)
{
    switch (access) {
    case Access::Allowed:
        return "allowed";
    case Access::Pending:
        return "pending";
    case Access::Denied:
        return "denied";
    default:
        return "none";
    }
}

// What the console is doing. Anyone may ask, so the links page can follow
// along too; only an allowed device hears the details, which can name files.
void sendStatus(int client, const FileSharing &sharing, const std::string &from)
{
    const Access access = sharing.root.empty() ? Access::Unknown
                          : sharing.access     ? sharing.access(from, false)
                                               : Access::Allowed;
    const ConsoleStatus status = sharing.status ? sharing.status() : ConsoleStatus();
    const bool allowed = access == Access::Allowed || status.open;
    sendJson(client, "{\"you\":" + remote::jsonString(from) +
                         ",\"access\":\"" + accessName(access) +
                         "\",\"files\":" + (sharing.root.empty() ? "false" : "true") +
                         ",\"state\":" + remote::jsonString(status.state) +
                         ",\"text\":" + remote::jsonString(allowed ? status.text : "") +
                         ",\"result\":" + remote::jsonString(allowed ? status.result : "") +
                         ",\"resultOk\":" + (status.resultOk ? "true" : "false") +
                         ",\"resultSeq\":" + std::to_string(status.resultSeq) + "}");
}

void sendFile(int client, const FileSharing &sharing, const std::string &from,
              const std::string &local, bool asAttachment)
{
    const int64_t size = fs::fileSize(local);
    FILE *file = size < 0 || fileops::isFolder(local)
                     ? nullptr
                     : std::fopen(local.c_str(), "rb");
    if (file == nullptr) {
        sendAll(client, remote::jsonError(404, "That file does not exist."));
        return;
    }
    const std::string name = fileops::name(local);
    // A sandboxed, never-sniffed response cannot run script with the page's
    // access to the API, whatever is in the file.
    const std::string head =
        "HTTP/1.1 200 OK\r\nContent-Type: " + std::string(remote::mimeType(name)) +
        "\r\nContent-Length: " + std::to_string(size) +
        "\r\nContent-Disposition: " +
        remote::contentDisposition(name, asAttachment) +
        "\r\nX-Content-Type-Options: nosniff"
        "\r\nContent-Security-Policy: sandbox"
        "\r\nCache-Control: no-store\r\nConnection: close\r\n\r\n";
    Reporter reporter(sharing, name, from, false, static_cast<uint64_t>(size));
    bool ok = sendBytes(client, head.data(), head.size(), IdleTimeoutMs);
    std::vector<char> buffer(ReadChunk);
    uint64_t done = 0;
    while (ok && done < static_cast<uint64_t>(size)) {
        const size_t got =
            stopping(sharing) ? 0 : std::fread(buffer.data(), 1, buffer.size(), file);
        if (got == 0) {
            ok = false;
            break;
        }
        ok = sendBytes(client, buffer.data(), got, IdleTimeoutMs);
        done += got;
        reporter.advance(done);
    }
    std::fclose(file);
    reporter.finish(ok);
}

// Each look at a path makes the SD card search the folder it is in, about
// a tenth of a second in a folder of hundreds of files, so an upload touches
// its folder as few times as it can.

// Gives part the name local unless something has that name already. The
// 3DS's rename refuses an existing name by itself; elsewhere rename would
// replace it, so this looks first.
bool claimName(const std::string &part, const std::string &local)
{
#ifndef __3DS__
    if (fileops::exists(local)) {
        return false;
    }
#endif
    return fs::renameFile(part, local);
}

enum class Overwrite { Missing, Done, Failed };

// Copies part over the contents of the file at local, which keeps its place
// in the folder: one search to open it, where removing and renaming take
// three. Missing when there is no file at local to open.
Overwrite overwrite(const std::string &part, const std::string &local,
                    uint64_t total, std::vector<char> &buffer)
{
    FILE *out = std::fopen(local.c_str(), "r+b");
    if (out == nullptr) {
        return Overwrite::Missing;
    }
    FILE *in = std::fopen(part.c_str(), "rb");
    std::setvbuf(out, nullptr, _IONBF, 0);
    bool ok = in != nullptr &&
              ftruncate(fileno(out), static_cast<off_t>(total)) == 0;
    if (in != nullptr) {
        std::setvbuf(in, nullptr, _IONBF, 0);
        size_t got = 0;
        while (ok && (got = std::fread(buffer.data(), 1, buffer.size(), in)) > 0) {
            ok = std::fwrite(buffer.data(), 1, got, out) == got;
        }
        ok = ok && !std::ferror(in);
        std::fclose(in);
    }
    ok = std::fclose(out) == 0 && ok;
    return ok ? Overwrite::Done : Overwrite::Failed;
}

// Streams a PUT body into local, through a hidden part file that only
// takes the real name once every byte is on the card.
void receiveFile(int client, const FileSharing &sharing,
                 const std::string &from, const remote::HttpRequest &request,
                 const std::string &data, size_t bodyStart,
                 const std::string &local)
{
    const std::string name = fileops::name(local);
    const std::string folder = folderOf(local);
    const bool replace = remote::formValue(request.query, "replace") == "1";
    if (request.headers.count("transfer-encoding") != 0 ||
        request.headers.count("content-length") == 0) {
        sendAll(client, remote::jsonError(411, "Send the file with its size."));
        return;
    }
    if (!fs::directoryExists(folder)) {
        sendAll(client, remote::jsonError(404, "The folder for " + name +
                                                   " does not exist."));
        return;
    }
    // Whether local is taken is found out at the end, by the rename that
    // refuses it, rather than by searching its folder before and after.
    const uint64_t total = request.contentLength;
    if (sharing.freeSpace) {
        // The new copy sits beside the old one until it is complete.
        const int64_t free = sharing.freeSpace();
        if (free >= 0 && total > static_cast<uint64_t>(free)) {
            sendAll(client, remote::jsonError(
                                507, "The SD card does not have room for " +
                                         name + "."));
            return;
        }
    }

    const std::string staging =
        sharing.stagingFolder.empty() ? folder : sharing.stagingFolder;
    const std::string part = (staging.back() == '/' ? staging : staging + "/") +
                             ".3dsdl-upload.part";
    FILE *file = std::fopen(part.c_str(), "wb");
    if (file == nullptr) {
        sendAll(client, remote::jsonError(500, "Could not create " + name +
                                                   " on the SD card."));
        return;
    }
    std::setvbuf(file, nullptr, _IONBF, 0);
    // Taking the whole size at once spares the SD card from growing the
    // file, and finding its clusters, on every write.
    if (total > 0 && ftruncate(fileno(file), static_cast<off_t>(total)) != 0) {
        std::fclose(file);
        fs::removeFile(part);
        sendAll(client, remote::jsonError(507, "The SD card does not have room for " +
                                                   name + "."));
        return;
    }

    Reporter reporter(sharing, name, from, true, total);
    std::vector<char> buffer(WriteChunk);
    // Whatever of the body came in with the head goes first.
    size_t used = std::min<size_t>(data.size() - bodyStart,
                                   static_cast<size_t>(std::min<uint64_t>(
                                       total, buffer.size())));
    std::memcpy(buffer.data(), data.data() + bodyStart, used);
    uint64_t received = used;
    bool written = true;
    bool connected = true;
    while (true) {
        if (used == buffer.size() || (received == total && used > 0)) {
            written = std::fwrite(buffer.data(), 1, used, file) == used;
            used = 0;
            if (!written) {
                break;
            }
        }
        reporter.advance(received);
        if (received == total) {
            break;
        }
        const size_t want = static_cast<size_t>(
            std::min<uint64_t>(buffer.size() - used, total - received));
        // Waited for in short slices, so a stop is noticed mid-upload.
        const Clock::time_point idle =
            Clock::now() + std::chrono::milliseconds(IdleTimeoutMs);
        ssize_t got = -2;
        while (got == -2 && Clock::now() < idle && !stopping(sharing)) {
            got = readSome(client, buffer.data() + used, want,
                           std::min(idle, Clock::now() + std::chrono::milliseconds(
                                                             StopCheckMs)));
        }
        if (got <= 0) {
            connected = false;
            break;
        }
        used += static_cast<size_t>(got);
        received += static_cast<uint64_t>(got);
    }
    written = std::fclose(file) == 0 && written;

    bool ok = written && connected;
    int status = 500;
    std::string problem = "Could not write " + name + " to the SD card.";
    bool placed = false;
    if (ok && replace && total <= OverwriteBytes) {
        const Overwrite result = overwrite(part, local, total, buffer);
        placed = result == Overwrite::Done;
        if (result == Overwrite::Failed) {
            ok = false;
            problem = "Could not replace " + name + ".";
        }
    } else if (ok && replace) {
        fs::removeFile(local); // when it is there
    }
    if (ok && !placed && !claimName(part, local)) {
        ok = false;
        // Only now is it worth searching the folder to say why.
        if (fileops::isFolder(local)) {
            status = 409;
            problem = "A folder is already called " + name + ".";
        } else if (fileops::exists(local)) {
            status = 409;
            problem = name + " already exists.";
        }
    }
    if (!ok || placed) {
        fs::removeFile(part);
    }
    reporter.finish(ok);
    if (ok) {
        sendJson(client, "{\"name\":" + remote::jsonString(name) +
                             ",\"size\":" + std::to_string(total) + "}");
    } else if (connected) {
        sendAll(client, remote::jsonError(status, problem));
    }
}

// The JSON API under /api/. True when it handed something to the app.
bool serveApi(int client, const FileSharing &sharing, const std::string &from,
              remote::HttpRequest &request, std::string &data,
              size_t bodyStart, Clock::time_point deadline, Received &out)
{
    std::string why;
    if (!remote::apiRequestAllowed(request, why)) {
        sendAll(client, remote::jsonError(403, why));
        return false;
    }
    if (request.path == "/api/status" && request.method == "GET") {
        sendStatus(client, sharing, from);
        return false;
    }
    if (sharing.root.empty()) {
        sendAll(client, remote::jsonError(404, "Files are not shared."));
        return false;
    }
    const Access access =
        sharing.access ? sharing.access(from, true) : Access::Allowed;
    if (access != Access::Allowed) {
        const bool pending = access == Access::Pending;
        sendAll(client,
                remote::httpResponse(
                    403, "application/json",
                    std::string("{\"error\":") +
                        remote::jsonString(
                            pending ? "Choose Allow on the 3DS to see its files."
                                    : "The 3DS did not allow this device.") +
                        ",\"access\":\"" + (pending ? "pending" : "denied") +
                        "\"}"));
        return false;
    }

    const std::string &method = request.method;
    const std::string &route = request.path;
    std::string local;
    const auto mapped = [&](const std::string &webPath, std::string &into) {
        if (remote::localPath(sharing.root, webPath, into)) {
            return true;
        }
        sendAll(client,
                remote::jsonError(400, "That is not a path on the SD card."));
        return false;
    };

    if (route == "/api/list" && method == "GET") {
        const std::string path = remote::formValue(request.query, "path");
        if (mapped(path, local)) {
            listFolder(client, sharing, path, local,
                       remote::formValue(request.query, "hidden") == "1",
                       remote::formValue(request.query, "times") != "0");
        }
        return false;
    }
    if ((route == "/api/meta" || route == "/api/icon") && method == "GET") {
        if (mapped(remote::formValue(request.query, "path"), local)) {
            sendTitle(client, sharing, local, route == "/api/icon");
        }
        return false;
    }
    if (route == "/api/file" && (method == "GET" || method == "PUT")) {
        if (!mapped(remote::formValue(request.query, "path"), local)) {
            return false;
        }
        if (method == "GET") {
            sendFile(client, sharing, from, local,
                     remote::formValue(request.query, "download") == "1");
        } else if (isRoot(sharing, local)) {
            sendAll(client, remote::jsonError(400, "Name the file to upload."));
        } else {
            receiveFile(client, sharing, from, request, data, bodyStart, local);
        }
        return false;
    }
    if (method != "POST") {
        sendAll(client, remote::jsonError(404, "There is nothing here."));
        return false;
    }
    if (!readBody(client, data, bodyStart, request, deadline)) {
        return false;
    }
    const std::string &form = request.body;

    if (route == "/api/folder") {
        if (!mapped(remote::formValue(form, "path"), local)) {
            return false;
        }
        if (fileops::exists(local) && !fileops::isFolder(local)) {
            sendAll(client, remote::jsonError(409, "A file is already called " +
                                                       fileops::name(local) + "."));
        } else if (!fs::makeDirectories(local)) {
            sendAll(client, remote::jsonError(500, "Could not create the folder."));
        } else {
            sendJson(client, "{}");
        }
        return false;
    }
    if (route == "/api/times") {
        if (mapped(remote::formValue(form, "path"), local)) {
            sendTimes(client, sharing, local, remote::formValue(form, "names"));
        }
        return false;
    }
    if (route == "/api/hash") {
        if (mapped(remote::formValue(form, "path"), local)) {
            sendHashes(client, sharing, local, remote::formValue(form, "names"));
        }
        return false;
    }
    if (route == "/api/move") {
        std::string destination;
        if (!mapped(remote::formValue(form, "from"), local) ||
            !mapped(remote::formValue(form, "to"), destination)) {
            return false;
        }
        std::string error;
        if (isRoot(sharing, local) || isRoot(sharing, destination)) {
            sendAll(client, remote::jsonError(400, "The SD card itself cannot move."));
        } else if (!fileops::exists(local)) {
            sendAll(client, remote::jsonError(404, fileops::name(local) +
                                                       " is not there any more."));
        } else if (!fs::directoryExists(folderOf(destination))) {
            sendAll(client, remote::jsonError(404, "That folder does not exist."));
        } else if (!fileops::move(local, destination, error)) {
            sendAll(client, remote::jsonError(409, error));
        } else {
            sendJson(client, "{}");
        }
        return false;
    }
    if (route == "/api/delete") {
        if (!mapped(remote::formValue(form, "path"), local)) {
            return false;
        }
        if (isRoot(sharing, local)) {
            sendAll(client, remote::jsonError(400, "The SD card itself cannot be deleted."));
            return false;
        }
        if (!fileops::exists(local)) {
            sendJson(client, "{}"); // already gone
            return false;
        }
        // A folder goes in one call where the card can do it itself, rather
        // than by searching its folder for every entry; the page shows no
        // progress, so nothing is counted first.
        if (fileops::isFolder(local) && fs::removeFolderTree(local)) {
            sendJson(client, "{}");
            return false;
        }
        StopWith observer(sharing);
        uint64_t done = 0;
        const fileops::Result result = fileops::remove(local, observer, done, 0);
        if (result.status == fileops::Status::Ok) {
            sendJson(client, "{}");
        } else {
            sendAll(client, remote::jsonError(500, result.message));
        }
        return false;
    }
    if (route == "/api/install") {
        const std::string paths = remote::formValue(form, "paths");
        size_t start = 0;
        while (start < paths.size()) {
            size_t end = paths.find('\n', start);
            if (end == std::string::npos) {
                end = paths.size();
            }
            std::string path;
            if (remote::localPath(sharing.root, paths.substr(start, end - start),
                                  path) &&
                filetype::fromName(path) == filetype::Kind::Cia &&
                fs::fileExists(path) && !fileops::isFolder(path)) {
                out.install.push_back(path);
            }
            start = end + 1;
        }
        if (out.install.empty()) {
            sendAll(client, remote::jsonError(400, "There are no CIAs to install."));
            return false;
        }
        out.from = from;
        sendJson(client, "{\"count\":" + std::to_string(out.install.size()) + "}");
        return true;
    }
    sendAll(client, remote::jsonError(404, "There is nothing here."));
    return false;
}

} // namespace

void finishReceived(Received &received)
{
    if (received.replySocket >= 0) {
        const char done = 1;
        send(received.replySocket, &done, 1, sendFlags());
        closeClient(received.replySocket);
    }
}

Receiver::~Receiver()
{
    close();
}

bool Receiver::open(unsigned short fbiPort, unsigned short webPort,
                        unsigned short fallbackWebPort, std::string &error)
{
    close();
    // FBI's port is a bonus: without it the web page still works.
    std::string fbiError;
    if (!listenOn(fbiPort, fbiSocket_, fbiPort_, fbiError)) {
        fbiPort_ = 0;
    }
    std::string webError;
    if (!listenOn(webPort, webSocket_, webPort_, webError) &&
        (fallbackWebPort == 0 ||
         !listenOn(fallbackWebPort, webSocket_, webPort_, webError))) {
        error = webError;
        close();
        return false;
    }
    return true;
}

void Receiver::close()
{
    closeSocket(fbiSocket_);
    closeSocket(webSocket_);
    fbiPort_ = 0;
    webPort_ = 0;
}

void Receiver::shareFiles(const FileSharing &sharing)
{
    sharing_ = sharing;
    // Set on the listener, so connections start out with the larger
    // window instead of growing into it.
    if (webSocket_ >= 0 && sharing_.socketBufferSize > 0) {
        const int size = sharing_.socketBufferSize;
        setsockopt(webSocket_, SOL_SOCKET, SO_RCVBUF, &size, sizeof(size));
        setsockopt(webSocket_, SOL_SOCKET, SO_SNDBUF, &size, sizeof(size));
    }
}

bool Receiver::wait(int timeoutMs, Received &out)
{
    out = Received();
    if (webSocket_ < 0) {
        return false;
    }
    pollfd listeners[2] = {{webSocket_, POLLIN, 0}, {fbiSocket_, POLLIN, 0}};
    const nfds_t count = fbiSocket_ >= 0 ? 2 : 1;
    if (waitSockets(listeners, count, timeoutMs, IdleStepMs) <= 0) {
        return false;
    }
    for (nfds_t index = 0; index < count; ++index) {
        if ((listeners[index].revents & POLLIN) == 0) {
            continue;
        }
        sockaddr_in peer {};
        socklen_t length = sizeof(peer);
        int client = accept(listeners[index].fd,
                            reinterpret_cast<sockaddr *>(&peer), &length);
        if (client < 0) {
            continue;
        }
        quietPipe(client);
        setNonBlocking(client);
        if (sharing_.socketBufferSize > 0) {
            const int size = sharing_.socketBufferSize;
            setsockopt(client, SOL_SOCKET, SO_RCVBUF, &size, sizeof(size));
            setsockopt(client, SOL_SOCKET, SO_SNDBUF, &size, sizeof(size));
        }
        const std::string from = inet_ntoa(peer.sin_addr);
        const bool got = index == 0 ? serveWeb(client, from, out)
                                    : serveFbi(client, from, out);
        if (got) {
            return true;
        }
    }
    return false;
}

bool Receiver::serveFbi(int client, const std::string &from,
                            Received &out)
{
    const Clock::time_point deadline =
        Clock::now() + std::chrono::milliseconds(ReadTimeoutMs);
    unsigned char lengthBytes[4];
    if (!readExactly(client, lengthBytes, sizeof(lengthBytes), deadline)) {
        closeSocket(client);
        return false;
    }
    const size_t length = remote::payloadLength(lengthBytes);
    if (length == 0 || length > remote::MaxPayload) {
        closeSocket(client);
        return false;
    }
    std::string payload(length, '\0');
    if (!readExactly(client, &payload[0], length, deadline)) {
        closeSocket(client);
        return false;
    }
    out.links = remote::extractLinks(payload);
    out.from = from;
    out.replySocket = client;
    if (out.links.empty()) {
        // Nothing to download, so the sender may go at once.
        finishReceived(out);
        return false;
    }
    return true;
}

bool Receiver::serveWeb(int client, const std::string &from, Received &out)
{
    const Clock::time_point deadline =
        Clock::now() + std::chrono::milliseconds(ReadTimeoutMs);
    if (!firstBytes(client, webSocket_, deadline)) {
        closeSocket(client);
        return false;
    }
    std::string data;
    remote::HttpRequest request;
    size_t bodyStart = 0;
    remote::Parse state = remote::Parse::Incomplete;
    char buffer[4096];
    while (state == remote::Parse::Incomplete) {
        const ssize_t got = readSome(client, buffer, sizeof(buffer), deadline);
        if (got <= 0) {
            break;
        }
        data.append(buffer, static_cast<size_t>(got));
        state = remote::parseHead(data, request, bodyStart);
    }
    if (state != remote::Parse::Done) {
        if (state == remote::Parse::Bad) {
            sendAll(client, remote::httpResponse(400, "text/plain",
                                                 "Bad request\n"));
        }
        closeClient(client);
        return false;
    }

    // Everything but the page's status poll is someone using the console.
    const bool counts = sharing_.working && request.path != "/api/status";
    if (counts) {
        sharing_.working(true);
    }
    struct Done {
        const std::function<void(bool)> *report;
        ~Done()
        {
            if (report != nullptr) {
                (*report)(false);
            }
        }
    } done {counts ? &sharing_.working : nullptr};

    const std::string html = "text/html; charset=utf-8";
    bool got = false;
    if (request.path.compare(0, 5, "/api/") == 0) {
        got = serveApi(client, sharing_, from, request, data, bodyStart,
                       deadline, out);
    } else if (request.path == "/" && request.method == "GET") {
        const std::string sent = remote::formValue(request.query, "sent");
        const std::string message =
            sent.empty() ? ""
            : sent == "0" ? "No links were found in what you sent."
            : sent == "1" ? "Sent 1 link. It is downloading on the 3DS."
                          : "Sent " + sent +
                                " links. They are downloading on the 3DS.";
        sendAll(client, remote::httpResponse(200, html,
                                             remote::page(message, sent != "0")));
    } else if (request.path == "/send" &&
               (request.method == "POST" || request.method == "GET")) {
        if (request.method == "POST" &&
            !readBody(client, data, bodyStart, request, deadline)) {
            closeClient(client);
            return false;
        }
        const std::string &form =
            request.method == "POST" ? request.body : request.query;
        out.links = remote::extractLinks(remote::formValue(form, "links"));
        out.from = from;
        got = !out.links.empty();
        const auto marker = request.headers.find("x-requested-with");
        if (marker != request.headers.end() && marker->second == "3dsDL") {
            // The page sent it itself and stays where it is.
            sendJson(client, "{\"count\":" + std::to_string(out.links.size()) + "}");
            closeClient(client);
            return got;
        }
        // Back to the form, so reloading the page does not send again.
        std::string response = "HTTP/1.1 303 See Other\r\nLocation: /?sent=" +
                               std::to_string(out.links.size()) +
                               "\r\nContent-Length: 0\r\nConnection: close\r\n\r\n";
        sendAll(client, response);
    } else if (request.method != "GET" && request.method != "POST") {
        sendAll(client, remote::httpResponse(405, "text/plain",
                                             "Method not allowed\n"));
    } else {
        sendAll(client, remote::httpResponse(404, "text/plain", "Not found\n"));
    }
    closeClient(client);
    return got;
}

} // namespace net
