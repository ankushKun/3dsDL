#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

// Listens on the local network: FBI's remote install protocol for links, and
// a web page (see core/remote.hpp) for links and for the files on the SD
// card. One connection at a time, which is all a person sending things
// needs. The 3DS app runs it on a worker thread; the host tests run it
// against real sockets.
namespace net {

// What a sender handed over for the app to act on.
struct Received {
    std::vector<std::string> links;
    // CIAs on the SD card (local paths) the page asked to install.
    std::vector<std::string> install;
    std::string from; // the sender's address
    // FBI's tools wait on this connection for one byte, sent by
    // finishReceived once the links are done. -1 for the web page.
    int replySocket = -1;
};

// Tells an FBI tool the links are done, so it can stop serving its files,
// and closes the connection.
void finishReceived(Received &received);

// A file going to or coming from the SD card through the page.
struct Transfer {
    std::string name;
    std::string from;
    bool upload = true; // false when the page is downloading it
    uint64_t done = 0;
    uint64_t total = 0;
    bool finished = false; // the last report for this file
    bool ok = false;       // with finished: the whole file went through
};

enum class Access { Allowed, Pending, Denied, Unknown };

// What the console is doing, for the page to show: "ready", "asking" (about
// a device), "choosing" (a question on the console), "installing",
// "downloading" or "busy". text says it for people. A finished request
// leaves a result, which seq tells apart from the one before.
struct ConsoleStatus {
    std::string state = "ready";
    std::string text;
    std::string result;
    bool resultOk = true;
    uint32_t resultSeq = 0;
    // The text and result name nothing private (links, not files), so
    // devices that were not allowed to see the files may have them too.
    bool open = false;
};

// How the page reaches the SD card. Without a root, the file API answers
// 404 and only links can be sent.
struct FileSharing {
    std::string root; // "sdmc:"
    // Whether the device at an address may use the files. When ask is set
    // and the device is unknown, the console asks and the answer is Pending
    // until it is given; without ask an unknown device is Unknown. Unset
    // allows everyone.
    std::function<Access(const std::string &from, bool ask)> access;
    std::function<int64_t()> freeSpace;  // bytes, or -1 when unknown
    std::function<int64_t()> totalSpace; // bytes, or -1 when unknown
    // Unset reports "ready" with no text.
    std::function<ConsoleStatus()> status;
    // The running 3dsDL's version, to name its own CIAs (cia::versionText).
    std::string appVersion;
    // Called a few times a second during a transfer, on the server's thread.
    std::function<void(const Transfer &)> progress;
    // Called with true as a request starts and false as it ends, on the
    // server's thread, for every request but the page's status poll, which
    // runs for as long as the page is open. The app keeps the console awake
    // for them.
    std::function<void(bool)> working;
    int socketBufferSize = 0; // asked for on each connection when not 0
    // Where uploads are written until they are complete, then moved into
    // place. Empty uses the destination's own folder. A small folder here
    // saves big folders a slow search for room on every upload.
    std::string stagingFolder;
    // Asked between pieces of long work (an upload, a download, checksums,
    // a delete): true gives it up, so the listener can stop quickly.
    std::function<bool()> cancelled;
};

class Receiver {
public:
    Receiver() = default;
    ~Receiver();
    Receiver(const Receiver &) = delete;
    Receiver &operator=(const Receiver &) = delete;

    // Port 0 picks a free port (for tests). When webPort is taken,
    // fallbackWebPort is tried. Fails, with a reason, only when the web page
    // cannot listen; fbiPort() is 0 when FBI's port was taken.
    bool open(unsigned short fbiPort, unsigned short webPort,
              unsigned short fallbackWebPort, std::string &error);
    void close();
    unsigned short fbiPort() const { return fbiPort_; }
    unsigned short webPort() const { return webPort_; }

    // Set after open() and before wait() is first called.
    void shareFiles(const FileSharing &sharing);

    // Waits up to timeoutMs for a connection and serves it. True when it
    // brought something for the app, which is in out.
    bool wait(int timeoutMs, Received &out);

private:
    bool serveFbi(int client, const std::string &from, Received &out);
    bool serveWeb(int client, const std::string &from, Received &out);

    int fbiSocket_ = -1;
    int webSocket_ = -1;
    unsigned short fbiPort_ = 0;
    unsigned short webPort_ = 0;
    FileSharing sharing_;
};

} // namespace net
