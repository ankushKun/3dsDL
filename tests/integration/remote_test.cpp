#include "../check.hpp"

#include "file_ops.hpp"
#include "fs_util.hpp"
#include "remote_server.hpp"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <future>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>

// Runs the link receiver on loopback against FBI-style and browser-style
// clients.

namespace {

int connectTo(unsigned short port)
{
    const int socketFd = socket(AF_INET, SOCK_STREAM, 0);
    sockaddr_in address {};
    address.sin_family = AF_INET;
    address.sin_port = htons(port);
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (connect(socketFd, reinterpret_cast<sockaddr *>(&address),
                sizeof(address)) != 0) {
        ::close(socketFd);
        return -1;
    }
    return socketFd;
}

void sendText(int socketFd, const std::string &data)
{
    size_t sent = 0;
    while (sent < data.size()) {
        const ssize_t wrote = send(socketFd, data.data() + sent, data.size() - sent, 0);
        if (wrote <= 0) {
            return;
        }
        sent += static_cast<size_t>(wrote);
    }
}

std::string readAll(int socketFd, int timeoutMs = 3000)
{
    std::string data;
    char buffer[4096];
    while (true) {
        pollfd waiting {socketFd, POLLIN, 0};
        if (poll(&waiting, 1, timeoutMs) <= 0) {
            break;
        }
        const ssize_t got = recv(socketFd, buffer, sizeof(buffer), 0);
        if (got <= 0) {
            break;
        }
        data.append(buffer, static_cast<size_t>(got));
    }
    return data;
}

// Serves until a batch arrives or a few seconds pass.
std::future<net::Received> receiveOne(net::Receiver &receiver)
{
    return std::async(std::launch::async, [&receiver]() {
        net::Received received;
        for (int round = 0; round < 40; ++round) {
            if (receiver.wait(100, received)) {
                break;
            }
        }
        return received;
    });
}

} // namespace

TEST("remote server: FBI's protocol, with the reply byte after the work")
{
    net::Receiver receiver;
    std::string error;
    CHECK(receiver.open(0, 0, 0, error));
    CHECK(receiver.fbiPort() != 0 && receiver.webPort() != 0);

    std::future<net::Received> pending = receiveOne(receiver);
    const int client = connectTo(receiver.fbiPort());
    CHECK(client >= 0);
    // What servefiles.py sends: a big-endian length and newline-separated URLs.
    const std::string payload =
        "http://192.168.1.2:8080/a.cia\nhttp://192.168.1.2:8080/b%20c.cia";
    std::string message(4, '\0');
    const uint32_t length = htonl(static_cast<uint32_t>(payload.size()));
    std::memcpy(&message[0], &length, 4);
    sendText(client, message + payload);

    net::Received received = pending.get();
    CHECK_EQ(received.links.size(), static_cast<size_t>(2));
    CHECK_EQ(received.links[1], std::string("http://192.168.1.2:8080/b%20c.cia"));
    CHECK_EQ(received.from, std::string("127.0.0.1"));
    CHECK(received.replySocket >= 0);

    // The sender is still waiting: nothing has come back yet.
    pollfd waiting {client, POLLIN, 0};
    CHECK_EQ(poll(&waiting, 1, 200), 0);
    net::finishReceived(received);
    CHECK_EQ(received.replySocket, -1);
    const std::string reply = readAll(client, 2000);
    CHECK_EQ(reply.size(), static_cast<size_t>(1));
    ::close(client);
}

TEST("remote server: an FBI payload without links is answered at once")
{
    net::Receiver receiver;
    std::string error;
    CHECK(receiver.open(0, 0, 0, error));
    std::future<net::Received> pending = receiveOne(receiver);
    const int client = connectTo(receiver.fbiPort());
    const std::string payload = "not a link";
    std::string message(4, '\0');
    const uint32_t length = htonl(static_cast<uint32_t>(payload.size()));
    std::memcpy(&message[0], &length, 4);
    sendText(client, message + payload);
    CHECK_EQ(readAll(client, 2000).size(), static_cast<size_t>(1));
    CHECK(pending.get().links.empty());
    ::close(client);
}

TEST("remote server: the web page, a form post and a redirect back")
{
    net::Receiver receiver;
    std::string error;
    CHECK(receiver.open(0, 0, 0, error));

    std::future<net::Received> page = receiveOne(receiver);
    int client = connectTo(receiver.webPort());
    sendText(client, "GET / HTTP/1.1\r\nHost: 3ds\r\n\r\n");
    std::string response = readAll(client);
    ::close(client);
    CHECK(response.rfind("HTTP/1.1 200 OK", 0) == 0);
    CHECK(response.find("<form method=\"post\" action=\"/send\"") !=
          std::string::npos);
    CHECK(page.get().links.empty());

    std::future<net::Received> post = receiveOne(receiver);
    client = connectTo(receiver.webPort());
    const std::string body =
        "links=https%3A%2F%2Fexample.com%2Fa.cia%0D%0Ahttps%3A%2F%2Fexample.com%2Fb.zip";
    // Sent in two parts, as a slow phone might.
    sendText(client, "POST /send HTTP/1.1\r\nHost: 3ds\r\nContent-Length: " +
                         std::to_string(body.size()) + "\r\n\r\n");
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    sendText(client, body);
    response = readAll(client);
    ::close(client);
    CHECK(response.rfind("HTTP/1.1 303 See Other", 0) == 0);
    CHECK(response.find("Location: /?sent=2") != std::string::npos);
    const net::Received received = post.get();
    CHECK_EQ(received.links.size(), static_cast<size_t>(2));
    CHECK_EQ(received.links[0], std::string("https://example.com/a.cia"));
    CHECK_EQ(received.replySocket, -1);

    std::future<net::Received> after = receiveOne(receiver);
    client = connectTo(receiver.webPort());
    sendText(client, "GET /?sent=2 HTTP/1.1\r\n\r\n");
    response = readAll(client);
    ::close(client);
    CHECK(response.find("Sent 2 links") != std::string::npos);
    (void)after.get();

    std::future<net::Received> missing = receiveOne(receiver);
    client = connectTo(receiver.webPort());
    sendText(client, "GET /favicon.ico HTTP/1.1\r\n\r\n");
    response = readAll(client);
    ::close(client);
    CHECK(response.rfind("HTTP/1.1 404", 0) == 0);
    (void)missing.get();
}

TEST("remote server: a taken port falls back, a silent client times out")
{
    net::Receiver first;
    std::string error;
    CHECK(first.open(0, 0, 0, error));

    // A port that was free a moment ago, for the fallback.
    unsigned short spare = 0;
    {
        net::Receiver probe;
        CHECK(probe.open(0, 0, 0, error));
        spare = probe.webPort();
    }
    // FBI's port taken: the web page still works without it.
    net::Receiver noFbi;
    CHECK(noFbi.open(first.fbiPort(), 0, 0, error));
    CHECK_EQ(noFbi.fbiPort(), static_cast<unsigned short>(0));
    CHECK(noFbi.webPort() != 0);
    noFbi.close();

    net::Receiver second;
    CHECK(!second.open(0, first.webPort(), 0, error));
    CHECK(!error.empty());
    CHECK(second.open(0, first.webPort(), spare, error));
    CHECK_EQ(second.webPort(), spare);
    second.close();

    // A sender that connects and says nothing is dropped after five
    // seconds, and the receiver is free again.
    const int client = connectTo(first.fbiPort());
    const auto started = std::chrono::steady_clock::now();
    net::Received received;
    CHECK(!first.wait(1000, received));
    const auto took = std::chrono::steady_clock::now() - started;
    CHECK(took >= std::chrono::milliseconds(4500));
    CHECK(took < std::chrono::seconds(7));
    CHECK(received.links.empty());
    ::close(client);
}

namespace {

// Serves on a thread for the length of a test, keeping what arrives.
class Serving {
public:
    explicit Serving(net::Receiver &receiver) : receiver_(receiver)
    {
        thread_ = std::thread([this]() {
            while (!stop_.load()) {
                net::Received received;
                if (receiver_.wait(50, received)) {
                    std::lock_guard<std::mutex> hold(lock_);
                    arrived_.push_back(received);
                }
            }
        });
    }
    ~Serving()
    {
        stop_.store(true);
        thread_.join();
    }
    std::vector<net::Received> arrived()
    {
        std::lock_guard<std::mutex> hold(lock_);
        return arrived_;
    }

private:
    net::Receiver &receiver_;
    std::atomic<bool> stop_ {false};
    std::mutex lock_;
    std::vector<net::Received> arrived_;
    std::thread thread_;
};

std::string roundTrip(unsigned short port, const std::string &request)
{
    const int client = connectTo(port);
    sendText(client, request);
    const std::string response = readAll(client);
    ::close(client);
    return response;
}

std::string bodyOf(const std::string &response)
{
    const size_t end = response.find("\r\n\r\n");
    return end == std::string::npos ? "" : response.substr(end + 4);
}

std::string post(unsigned short port, const std::string &route,
                 const std::string &form)
{
    return roundTrip(port, "POST " + route +
                              " HTTP/1.1\r\nHost: 127.0.0.1\r\n"
                              "X-Requested-With: 3dsDL\r\nContent-Length: " +
                              std::to_string(form.size()) + "\r\n\r\n" + form);
}

std::string put(unsigned short port, const std::string &target,
                const std::string &body)
{
    return roundTrip(port, "PUT " + target +
                              " HTTP/1.1\r\nHost: 127.0.0.1\r\n"
                              "X-Requested-With: 3dsDL\r\nContent-Length: " +
                              std::to_string(body.size()) + "\r\n\r\n" + body);
}

std::string get(unsigned short port, const std::string &target,
                const std::string &host = "127.0.0.1")
{
    return roundTrip(port, "GET " + target + " HTTP/1.1\r\nHost: " + host +
                              "\r\n\r\n");
}

bool startsWith(const std::string &text, const std::string &prefix)
{
    return text.rfind(prefix, 0) == 0;
}

struct TempRoot {
    std::string path;
    TempRoot()
    {
        char pattern[] = "/tmp/3dsdl-files-XXXXXX";
        path = mkdtemp(pattern);
    }
    ~TempRoot()
    {
        struct Quiet : fileops::Observer {
            bool cancelRequested() override { return false; }
        } observer;
        uint64_t done = 0;
        fileops::remove(path, observer, done, 0);
    }
};

} // namespace

TEST("remote server: files go up, come back, move and go away")
{
    TempRoot root;
    std::mutex lock;
    std::vector<net::Transfer> finished;
    std::vector<std::string> changed;
    net::FileSharing sharing;
    sharing.root = root.path;
    sharing.freeSpace = []() { return static_cast<int64_t>(1) << 30; };
    sharing.progress = [&](const net::Transfer &transfer) {
        if (transfer.finished) {
            std::lock_guard<std::mutex> hold(lock);
            finished.push_back(transfer);
        }
    };
    sharing.changed = [&](const std::string &path) {
        std::lock_guard<std::mutex> hold(lock);
        changed.push_back(path);
    };
    net::Receiver receiver;
    std::string error;
    CHECK(receiver.open(0, 0, 0, error));
    receiver.shareFiles(sharing);
    Serving serving(receiver);
    const unsigned short port = receiver.webPort();

    std::string response = get(port, "/api/list?path=%2F");
    CHECK(startsWith(response, "HTTP/1.1 200"));
    CHECK(bodyOf(response).find("\"entries\":[]") != std::string::npos);
    CHECK(bodyOf(response).find("\"free\":1073741824") != std::string::npos);

    // Into a folder that is not there yet, then made.
    CHECK(startsWith(put(port, "/api/file?path=%2Fcia%2Fa.bin", "x"),
                     "HTTP/1.1 404"));
    CHECK(startsWith(post(port, "/api/folder", "path=%2Fcia"), "HTTP/1.1 200"));
    CHECK(fs::directoryExists(root.path + "/cia"));

    // Bigger than one SD card write, sent in pieces as a phone would.
    std::string contents(300 * 1024 + 7, '\0');
    for (size_t index = 0; index < contents.size(); ++index) {
        contents[index] = static_cast<char>(index * 31 + index / 977);
    }
    int client = connectTo(port);
    sendText(client, "PUT /api/file?path=%2Fcia%2Fa%20b.bin HTTP/1.1\r\n"
                     "Host: 127.0.0.1\r\nX-Requested-With: 3dsDL\r\n"
                     "Content-Length: " +
                         std::to_string(contents.size()) + "\r\n\r\n" +
                         contents.substr(0, 1000));
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    sendText(client, contents.substr(1000));
    response = readAll(client);
    ::close(client);
    CHECK(startsWith(response, "HTTP/1.1 200"));
    std::string stored;
    CHECK(fs::readFile(root.path + "/cia/a b.bin", stored));
    CHECK(stored == contents);
    CHECK(!fs::fileExists(root.path + "/cia/.3dsdl-upload.part"));
    {
        std::lock_guard<std::mutex> hold(lock);
        CHECK_EQ(finished.size(), static_cast<size_t>(1));
        CHECK(finished.back().ok && finished.back().upload);
        CHECK_EQ(finished.back().done, static_cast<uint64_t>(contents.size()));
        CHECK_EQ(finished.back().path, root.path + "/cia/a b.bin");
        CHECK(finished.back().batch.id.empty());
        CHECK_EQ(changed.size(), static_cast<size_t>(2));
        CHECK_EQ(changed[0], root.path + "/cia");
        CHECK_EQ(changed[1], root.path + "/cia/a b.bin");
        finished.clear();
        changed.clear();
    }

    // Replacing has to be asked for.
    CHECK(startsWith(put(port, "/api/file?path=%2Fcia%2Fa%20b.bin", "new"),
                     "HTTP/1.1 409"));
    CHECK(startsWith(put(port, "/api/file?path=%2Fcia%2Fa%20b.bin&replace=1", "new"),
                     "HTTP/1.1 200"));
    CHECK(fs::readFile(root.path + "/cia/a b.bin", stored) && stored == "new");

    response = get(port, "/api/file?path=%2Fcia%2Fa%20b.bin&download=1");
    CHECK(startsWith(response, "HTTP/1.1 200"));
    CHECK(bodyOf(response) == "new");
    CHECK(response.find("Content-Security-Policy: sandbox") != std::string::npos);
    CHECK(response.find("attachment; filename=\"a b.bin\"") != std::string::npos);
    // Only a file saved from the page is reported, not one it shows.
    CHECK(bodyOf(get(port, "/api/file?path=%2Fcia%2Fa%20b.bin")) == "new");
    {
        std::lock_guard<std::mutex> hold(lock);
        // The refused upload, the replacing one and the download.
        CHECK_EQ(finished.size(), static_cast<size_t>(3));
        CHECK(!finished[0].ok && finished[0].message == "a b.bin already exists.");
        CHECK(finished[1].ok);
        CHECK(!finished[2].upload && finished[2].ok);
        CHECK_EQ(finished[2].done, static_cast<uint64_t>(3));
    }

    response = get(port, "/api/list?path=%2Fcia");
    CHECK(bodyOf(response).find("{\"name\":\"a b.bin\",\"dir\":false,\"size\":3,") !=
          std::string::npos);

    CHECK(startsWith(post(port, "/api/move", "from=%2Fcia%2Fa+b.bin&to=%2Fgame.cia"),
                     "HTTP/1.1 200"));
    CHECK(fs::fileExists(root.path + "/game.cia"));
    CHECK(startsWith(post(port, "/api/move", "from=%2Fcia&to=%2Fcia%2Finside"),
                     "HTTP/1.1 409"));

    // The page asks the console to install; only real CIAs go through.
    response = post(port, "/api/install", "paths=%2Fgame.cia%0A%2Fmissing.cia%0A%2Fcia");
    CHECK(startsWith(response, "HTTP/1.1 200"));
    CHECK(bodyOf(response) == "{\"count\":1}");
    CHECK(startsWith(post(port, "/api/install", "paths=%2Fcia"), "HTTP/1.1 400"));

    CHECK(startsWith(post(port, "/api/delete", "path=%2Fcia"), "HTTP/1.1 200"));
    CHECK(!fs::directoryExists(root.path + "/cia"));
    CHECK(startsWith(post(port, "/api/delete", "path=%2F"), "HTTP/1.1 400"));
    {
        std::lock_guard<std::mutex> hold(lock);
        // The replaced file, both ends of the move, then the deleted folder.
        const std::vector<std::string> expected = {
            root.path + "/cia/a b.bin", root.path + "/cia/a b.bin",
            root.path + "/game.cia", root.path + "/cia"};
        CHECK(changed == expected);
    }

    const std::vector<net::Received> arrived = serving.arrived();
    CHECK_EQ(arrived.size(), static_cast<size_t>(1));
    if (!arrived.empty()) {
        CHECK_EQ(arrived[0].install.size(), static_cast<size_t>(1));
        CHECK_EQ(arrived[0].install[0], root.path + "/game.cia");
        CHECK(arrived[0].links.empty());
    }
}

TEST("remote server: uploads wait in the staging folder, then take their place")
{
    TempRoot root;
    TempRoot staging;
    net::FileSharing sharing;
    sharing.root = root.path;
    sharing.stagingFolder = staging.path;
    net::Receiver receiver;
    std::string error;
    CHECK(receiver.open(0, 0, 0, error));
    receiver.shareFiles(sharing);
    Serving serving(receiver);
    const unsigned short port = receiver.webPort();

    CHECK(startsWith(put(port, "/api/file?path=%2Fa.bin", "first"), "HTTP/1.1 200"));
    std::string stored;
    CHECK(fs::readFile(root.path + "/a.bin", stored) && stored == "first");
    CHECK(!fs::fileExists(staging.path + "/.3dsdl-upload.part"));
    CHECK(!fs::fileExists(root.path + "/.3dsdl-upload.part"));

    // Taken names are refused once the body is in, and nothing is left.
    CHECK(startsWith(put(port, "/api/file?path=%2Fa.bin", "again"), "HTTP/1.1 409"));
    CHECK(fs::readFile(root.path + "/a.bin", stored) && stored == "first");
    CHECK(fs::makeDirectories(root.path + "/dir"));
    CHECK(bodyOf(put(port, "/api/file?path=%2Fdir&replace=1", "x"))
              .find("A folder is already called dir.") != std::string::npos);
    CHECK(!fs::fileExists(staging.path + "/.3dsdl-upload.part"));

    // Replacing writes over the old copy, shorter or longer.
    CHECK(startsWith(put(port, "/api/file?path=%2Fa.bin&replace=1", "2nd"),
                     "HTTP/1.1 200"));
    CHECK(fs::readFile(root.path + "/a.bin", stored) && stored == "2nd");
    CHECK(startsWith(put(port, "/api/file?path=%2Fa.bin&replace=1", "the third"),
                     "HTTP/1.1 200"));
    CHECK(fs::readFile(root.path + "/a.bin", stored) && stored == "the third");
    // Replacing a file that is not there yet just adds it.
    CHECK(startsWith(put(port, "/api/file?path=%2Fnew.bin&replace=1", "n"),
                     "HTTP/1.1 200"));
    CHECK(fs::readFile(root.path + "/new.bin", stored) && stored == "n");
    CHECK(!fs::fileExists(staging.path + "/.3dsdl-upload.part"));
}

TEST("remote server: a stopping listener gives up an upload part way")
{
    TempRoot root;
    std::atomic<bool> stop {false};
    net::FileSharing sharing;
    sharing.root = root.path;
    sharing.cancelled = [&stop]() { return stop.load(); };
    net::Receiver receiver;
    std::string error;
    CHECK(receiver.open(0, 0, 0, error));
    receiver.shareFiles(sharing);
    Serving serving(receiver);
    const unsigned short port = receiver.webPort();

    // Half the body, then the listener is told to stop: it lets go well
    // before the 30 seconds it would wait for the rest.
    int client = connectTo(port);
    sendText(client, "PUT /api/file?path=%2Fslow.bin HTTP/1.1\r\nHost: 127.0.0.1\r\n"
                     "X-Requested-With: 3dsDL\r\nContent-Length: 8\r\n\r\nabcd");
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    const auto asked = std::chrono::steady_clock::now();
    stop = true;
    const std::string response = readAll(client);
    ::close(client);
    CHECK(std::chrono::steady_clock::now() - asked < std::chrono::seconds(2));
    CHECK(response.empty());
    CHECK(!fs::fileExists(root.path + "/slow.bin"));
    CHECK(!fs::fileExists(root.path + "/.3dsdl-upload.part"));
}

TEST("remote server: every request but the status poll counts as working")
{
    TempRoot root;
    std::atomic<int> started {0};
    std::atomic<int> ended {0};
    net::FileSharing sharing;
    sharing.root = root.path;
    sharing.working = [&](bool on) { ++(on ? started : ended); };
    net::Receiver receiver;
    std::string error;
    CHECK(receiver.open(0, 0, 0, error));
    receiver.shareFiles(sharing);
    Serving serving(receiver);
    const unsigned short port = receiver.webPort();
    // The end is reported once the answer has gone, so it can trail it.
    const auto settled = [&](int count) {
        for (int wait = 0; wait < 100 && ended.load() != count; ++wait) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        return started.load() == count && ended.load() == count;
    };

    // An open page polls its status for as long as it is open.
    CHECK(startsWith(get(port, "/api/status"), "HTTP/1.1 200"));
    CHECK(settled(0));
    CHECK(startsWith(get(port, "/api/list?path=%2F"), "HTTP/1.1 200"));
    CHECK(settled(1));
    CHECK(startsWith(put(port, "/api/file?path=%2Fa.bin", "x"), "HTTP/1.1 200"));
    CHECK(settled(2));
    CHECK(startsWith(get(port, "/"), "HTTP/1.1 200"));
    CHECK(settled(3));
}

TEST("remote server: the file API turns away what it should")
{
    TempRoot root;
    net::Access access = net::Access::Allowed;
    net::FileSharing sharing;
    sharing.root = root.path;
    bool asked = false;
    sharing.access = [&](const std::string &from, const std::string &device, bool ask) {
        CHECK_EQ(from, std::string("127.0.0.1"));
        CHECK(device.empty());
        asked = asked || ask;
        return access;
    };
    sharing.freeSpace = []() { return static_cast<int64_t>(10); };
    net::Receiver receiver;
    std::string error;
    CHECK(receiver.open(0, 0, 0, error));

    // Nothing is shared until the app says so.
    {
        Serving serving(receiver);
        CHECK(startsWith(get(receiver.webPort(), "/api/list?path=%2F"),
                         "HTTP/1.1 404"));
    }
    receiver.shareFiles(sharing);
    Serving serving(receiver);
    const unsigned short port = receiver.webPort();

    CHECK(startsWith(get(port, "/api/list?path=%2F..%2F"), "HTTP/1.1 400"));
    CHECK(startsWith(get(port, "/api/list?path=%2F", "attacker.example"),
                     "HTTP/1.1 403"));
    // A form on another site cannot add the header.
    CHECK(startsWith(roundTrip(port, "POST /api/delete HTTP/1.1\r\nHost: 127.0.0.1\r\n"
                                    "Content-Length: 6\r\n\r\npath=x"),
                     "HTTP/1.1 403"));
    // More than the card can hold is refused before it is sent.
    CHECK(startsWith(put(port, "/api/file?path=%2Fbig.bin", std::string(11, 'x')),
                     "HTTP/1.1 507"));
    CHECK(startsWith(roundTrip(port, "PUT /api/file?path=%2Fa HTTP/1.1\r\n"
                                    "Host: 127.0.0.1\r\nX-Requested-With: 3dsDL\r\n"
                                    "Transfer-Encoding: chunked\r\n\r\n0\r\n\r\n"),
                     "HTTP/1.1 411"));

    // A sender that gives up halfway leaves nothing behind.
    int client = connectTo(port);
    sendText(client, "PUT /api/file?path=%2Fhalf.bin HTTP/1.1\r\nHost: 127.0.0.1\r\n"
                     "X-Requested-With: 3dsDL\r\nContent-Length: 8\r\n\r\nabcd");
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    ::close(client);
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    CHECK(!fs::fileExists(root.path + "/half.bin"));
    CHECK(!fs::fileExists(root.path + "/.3dsdl-upload.part"));

    access = net::Access::Pending;
    std::string response = get(port, "/api/list?path=%2F");
    CHECK(startsWith(response, "HTTP/1.1 403"));
    CHECK(bodyOf(response).find("\"access\":\"pending\"") != std::string::npos);
    access = net::Access::Denied;
    CHECK(bodyOf(get(port, "/api/list?path=%2F")).find("\"access\":\"denied\"") !=
          std::string::npos);
    // Links never need permission.
    CHECK(startsWith(get(port, "/"), "HTTP/1.1 200"));
}

TEST("remote server: a silent spare connection does not hold up the next")
{
    net::Receiver receiver;
    std::string error;
    CHECK(receiver.open(0, 0, 0, error));
    Serving serving(receiver);
    const int spare = connectTo(receiver.webPort());
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    const auto started = std::chrono::steady_clock::now();
    const std::string response = get(receiver.webPort(), "/");
    CHECK(startsWith(response, "HTTP/1.1 200"));
    CHECK(std::chrono::steady_clock::now() - started < std::chrono::seconds(2));
    ::close(spare);
}

TEST("remote server: answers end cleanly, and a client that stays does not hold it")
{
    net::Receiver receiver;
    std::string error;
    CHECK(receiver.open(0, 0, 0, error));
    Serving serving(receiver);
    const unsigned short port = receiver.webPort();

    // The whole page arrives and the connection ends with a close, not a
    // reset (the 3DS dropped the end of non-blocking sockets on close).
    const int client = connectTo(port);
    sendText(client, "GET / HTTP/1.1\r\nHost: 127.0.0.1\r\n\r\n");
    std::string data;
    char buffer[4096];
    ssize_t got = 0;
    while ((got = recv(client, buffer, sizeof(buffer), 0)) > 0) {
        data.append(buffer, static_cast<size_t>(got));
    }
    CHECK_EQ(got, static_cast<ssize_t>(0));
    const size_t length = data.find("Content-Length: ");
    CHECK(length != std::string::npos);
    CHECK_EQ(bodyOf(data).size(),
             static_cast<size_t>(std::atol(data.c_str() + length + 16)));
    CHECK(data.find("</html>") != std::string::npos);
    ::close(client);

    // This one reads its answer and never closes; the next is still served
    // once the linger runs out.
    const int stays = connectTo(port);
    sendText(stays, "GET /nothing HTTP/1.1\r\nHost: 127.0.0.1\r\n\r\n");
    const auto started = std::chrono::steady_clock::now();
    CHECK(startsWith(get(port, "/nothing"), "HTTP/1.1 404"));
    CHECK(std::chrono::steady_clock::now() - started < std::chrono::seconds(3));
    ::close(stays);
}

TEST("remote server: status, links for the page, hidden files and titles")
{
    TempRoot root;
    std::mutex lock;
    std::set<std::string> asked;
    net::ConsoleStatus status;
    status.state = "choosing";
    status.text = "Waiting for you to choose Install on the 3DS.";
    status.result = "Installed game.cia.";
    status.resultSeq = 3;
    net::FileSharing sharing;
    sharing.root = root.path;
    bool allowed = false;
    sharing.access = [&](const std::string &from, const std::string &, bool ask) {
        std::lock_guard<std::mutex> hold(lock);
        if (allowed) {
            return net::Access::Allowed;
        }
        if (!ask && asked.count(from) == 0) {
            return net::Access::Unknown;
        }
        asked.insert(from);
        return net::Access::Pending;
    };
    sharing.totalSpace = []() { return static_cast<int64_t>(4) << 30; };
    sharing.status = [&]() { return status; };
    net::Receiver receiver;
    std::string error;
    CHECK(receiver.open(0, 0, 0, error));
    receiver.shareFiles(sharing);
    Serving serving(receiver);
    const unsigned short port = receiver.webPort();

    // Asking what the console does is not asking for the files, and a
    // stranger hears the state but not the names in it.
    std::string body = bodyOf(get(port, "/api/status"));
    CHECK(body.find("\"access\":\"none\"") != std::string::npos);
    CHECK(body.find("\"you\":\"127.0.0.1\"") != std::string::npos);
    CHECK(body.find("\"state\":\"choosing\"") != std::string::npos);
    CHECK(body.find("\"text\":\"\"") != std::string::npos);
    CHECK(body.find("\"resultSeq\":3") != std::string::npos);
    CHECK(asked.empty());
    CHECK(startsWith(get(port, "/api/list?path=%2F"), "HTTP/1.1 403"));
    CHECK(bodyOf(get(port, "/api/status")).find("\"access\":\"pending\"") != std::string::npos);
    {
        std::lock_guard<std::mutex> hold(lock);
        allowed = true;
    }
    body = bodyOf(get(port, "/api/status"));
    CHECK(body.find("Waiting for you to choose Install") != std::string::npos);
    CHECK(body.find("Installed game.cia.") != std::string::npos);

    // The page sends links itself and stays put.
    const std::string form = "links=https%3A%2F%2Fexample.com%2Fa.cia";
    std::string response = roundTrip(port, "POST /send HTTP/1.1\r\nHost: 127.0.0.1\r\n"
                                           "X-Requested-With: 3dsDL\r\nContent-Length: " +
                                               std::to_string(form.size()) + "\r\n\r\n" + form);
    CHECK(startsWith(response, "HTTP/1.1 200"));
    CHECK_EQ(bodyOf(response), std::string("{\"count\":1}"));

    CHECK(fs::writeFile(root.path + "/.hidden", "x"));
    CHECK(fs::writeFile(root.path + "/shown.txt", "x"));
    CHECK(fs::writeFile(root.path + "/.3dsdl-upload.part", "x"));
    body = bodyOf(get(port, "/api/list?path=%2F"));
    CHECK(body.find("shown.txt") != std::string::npos);
    CHECK(body.find(".hidden") == std::string::npos);
    CHECK(body.find("\"total\":4294967296") != std::string::npos);
    body = bodyOf(get(port, "/api/list?path=%2F&hidden=1"));
    CHECK(body.find(".hidden") != std::string::npos);
    CHECK(body.find(".3dsdl-upload.part") == std::string::npos);

    // A text file has no title; a broken CIA says why.
    CHECK(startsWith(get(port, "/api/meta?path=%2Fshown.txt"), "HTTP/1.1 404"));
    CHECK(fs::writeFile(root.path + "/broken.cia", std::string(100, 'x')));
    response = get(port, "/api/icon?path=%2Fbroken.cia");
    CHECK(startsWith(response, "HTTP/1.1 404"));
    CHECK(bodyOf(response).find("error") != std::string::npos);
}

TEST("remote server: uploads say who sent them and where they stand in the batch")
{
    TempRoot root;
    std::mutex lock;
    std::vector<net::Transfer> finished;
    std::string asking;
    net::FileSharing sharing;
    sharing.root = root.path;
    sharing.access = [&](const std::string &, const std::string &device, bool ask) {
        std::lock_guard<std::mutex> hold(lock);
        if (ask) {
            asking = device;
        }
        return net::Access::Allowed;
    };
    sharing.progress = [&](const net::Transfer &transfer) {
        if (transfer.finished) {
            std::lock_guard<std::mutex> hold(lock);
            finished.push_back(transfer);
        }
    };
    net::Receiver receiver;
    std::string error;
    CHECK(receiver.open(0, 0, 0, error));
    receiver.shareFiles(sharing);
    Serving serving(receiver);
    const unsigned short port = receiver.webPort();
    CHECK(fs::makeDirectories(root.path + "/3ds/Check point/saves"));

    const auto send = [&](const std::string &target, const std::string &batch) {
        return roundTrip(port, "PUT " + target +
                                   " HTTP/1.1\r\nHost: 127.0.0.1\r\n"
                                   "X-Requested-With: 3dsDL\r\n"
                                   "User-Agent: Mozilla/5.0 (iPhone; CPU iPhone OS 18_0)\r\n"
                                   "X-3dsDL-Batch: " + batch + "\r\n"
                                   "Content-Length: 2\r\n\r\nok");
    };
    CHECK(startsWith(send("/api/file?path=%2F3ds%2FCheck%20point%2Fsaves%2Fa",
                          "q7 2 3 2 6 2 %2F3ds%2FCheck%20point"),
                     "HTTP/1.1 200"));
    // A folder the file is not in is not taken for its own.
    CHECK(startsWith(send("/api/file?path=%2Fb", "q7 3 3 4 6 2 %2F3ds%2FCheck%20point"),
                     "HTTP/1.1 200"));
    std::lock_guard<std::mutex> hold(lock);
    CHECK_EQ(asking, std::string("iPhone"));
    CHECK_EQ(finished.size(), static_cast<size_t>(2));
    if (finished.size() == 2) {
        const remote::UploadBatch &first = finished[0].batch;
        CHECK_EQ(finished[0].device, std::string("iPhone"));
        CHECK_EQ(first.id, std::string("q7"));
        CHECK_EQ(first.position, static_cast<uint32_t>(2));
        CHECK_EQ(first.count, static_cast<uint32_t>(3));
        CHECK_EQ(first.before, static_cast<uint64_t>(2));
        CHECK_EQ(first.bytes, static_cast<uint64_t>(6));
        CHECK_EQ(first.folder, root.path + "/3ds/Check point");
        CHECK_EQ(first.folderFiles, static_cast<uint32_t>(2));
        CHECK_EQ(finished[1].batch.position, static_cast<uint32_t>(3));
        CHECK(finished[1].batch.folder.empty());
        CHECK_EQ(finished[1].batch.folderFiles, static_cast<uint32_t>(0));
    }
}

TEST("remote server: what a sender asks for comes with its links")
{
    net::Receiver receiver;
    std::string error;
    CHECK(receiver.open(0, 0, 0, error));

    std::future<net::Received> sent = receiveOne(receiver);
    int client = connectTo(receiver.webPort());
    sendText(client, "GET /send?links=https%3A%2F%2Fexample.com%2Fa.3dsx"
                     "&folder=%2F3ds%2Fa&replace=1 HTTP/1.1\r\n"
                     "Host: 3ds\r\n\r\n");
    std::string response = readAll(client);
    ::close(client);
    CHECK(response.rfind("HTTP/1.1 303 See Other", 0) == 0);
    net::Received received = sent.get();
    CHECK_EQ(received.links.size(), static_cast<size_t>(1));
    CHECK_EQ(received.options.folder, std::string("sdmc:/3ds/a/"));
    CHECK(received.options.replace);

    // The page's form posts the same fields.
    sent = receiveOne(receiver);
    client = connectTo(receiver.webPort());
    const std::string body = "links=https%3A%2F%2Fexample.com%2Fb.cia&folder=%2Fcias";
    sendText(client, "POST /send HTTP/1.1\r\nHost: 3ds\r\nContent-Length: " +
                         std::to_string(body.size()) + "\r\n\r\n" + body);
    response = readAll(client);
    ::close(client);
    CHECK(response.rfind("HTTP/1.1 303 See Other", 0) == 0);
    received = sent.get();
    CHECK_EQ(received.links.size(), static_cast<size_t>(1));
    CHECK_EQ(received.options.folder, std::string("sdmc:/cias/"));
    CHECK(!received.options.replace);

    // A folder a sender may not name turns the whole send away.
    client = connectTo(receiver.webPort());
    sendText(client, "GET /send?links=https%3A%2F%2Fexample.com%2Fboot.firm"
                     "&folder=%2F&replace=1 HTTP/1.1\r\nHost: 3ds\r\n\r\n");
    net::Received refused;
    CHECK(!receiver.wait(2000, refused));
    CHECK(refused.links.empty());
    response = readAll(client);
    ::close(client);
    CHECK(response.rfind("HTTP/1.1 400", 0) == 0);
    CHECK(response.find("/3ds/ or /cias/") != std::string::npos);

    // A page that sent options and no links, then an FBI send, both waiting
    // when the receiver looks: one wait serves both, and the FBI links come
    // without the page's options.
    const int page = connectTo(receiver.webPort());
    sendText(page, "GET /send?folder=%2F3ds%2F&replace=1 HTTP/1.1\r\n"
                   "Host: 3ds\r\n\r\n");
    const int fbi = connectTo(receiver.fbiPort());
    const std::string payload = "http://192.168.1.2:8080/a.cia";
    std::string message(4, '\0');
    const uint32_t length = htonl(static_cast<uint32_t>(payload.size()));
    std::memcpy(&message[0], &length, 4);
    sendText(fbi, message + payload);
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    net::Received both;
    CHECK(receiver.wait(2000, both));
    CHECK_EQ(both.links.size(), static_cast<size_t>(1));
    CHECK(both.replySocket >= 0);
    CHECK(both.options.folder.empty());
    CHECK(!both.options.replace);
    CHECK(readAll(page, 2000).find("Location: /?sent=0") != std::string::npos);
    ::close(page);
    net::finishReceived(both);
    CHECK_EQ(readAll(fbi, 2000).size(), static_cast<size_t>(1));
    ::close(fbi);
}

TEST("remote server: a send says if 3dsDL's page made it, holds 20 links, and a turned-down device is refused")
{
    net::Access access = net::Access::Allowed;
    net::FileSharing sharing;
    sharing.access = [&](const std::string &, const std::string &, bool) {
        return access;
    };
    net::Receiver receiver;
    std::string error;
    CHECK(receiver.open(0, 0, 0, error));
    receiver.shareFiles(sharing);
    const auto post = [&](const std::string &headers, const std::string &body) {
        const int client = connectTo(receiver.webPort());
        sendText(client, "POST /send HTTP/1.1\r\n" + headers + "Content-Length: " +
                             std::to_string(body.size()) + "\r\n\r\n" + body);
        return client;
    };
    const std::string one = "links=https%3A%2F%2Fexample.com%2Fa.3dsx";

    // The page adds its header, by the console's address.
    std::future<net::Received> sent = receiveOne(receiver);
    int client = post("Host: 127.0.0.1\r\nX-Requested-With: 3dsDL\r\n", one);
    CHECK(sent.get().ownPage);
    readAll(client);
    ::close(client);
    // A form on another site cannot add it, and a name another site
    // controls is not the console's page even with it.
    for (const char *headers : {"Host: 127.0.0.1\r\n",
                                "Host: attacker.example\r\nX-Requested-With: 3dsDL\r\n"}) {
        sent = receiveOne(receiver);
        client = post(headers, one);
        CHECK(!sent.get().ownPage);
        readAll(client);
        ::close(client);
    }

    // Twenty links go through; twenty-one are turned away before the console.
    std::string links = "links=";
    for (int index = 0; index < 20; ++index) {
        links += "https%3A%2F%2Fexample.com%2F" + std::to_string(index) + ".3dsx+";
    }
    sent = receiveOne(receiver);
    client = post("Host: 127.0.0.1\r\n", links);
    CHECK_EQ(sent.get().links.size(), static_cast<size_t>(20));
    readAll(client);
    ::close(client);
    client = post("Host: 127.0.0.1\r\n", links + "https%3A%2F%2Fexample.com%2F20.3dsx");
    net::Received refused;
    CHECK(!receiver.wait(1000, refused));
    std::string response = readAll(client);
    ::close(client);
    CHECK(response.rfind("HTTP/1.1 400", 0) == 0);
    CHECK(response.find("at most 20") != std::string::npos);

    // A device the console turned down sends nothing more.
    access = net::Access::Denied;
    client = post("Host: 127.0.0.1\r\nX-Requested-With: 3dsDL\r\n", one);
    CHECK(!receiver.wait(1000, refused));
    CHECK(refused.links.empty());
    response = readAll(client);
    ::close(client);
    CHECK(response.rfind("HTTP/1.1 403", 0) == 0);
}
