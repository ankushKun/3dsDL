#pragma once

#include "remote.hpp"

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
    // What the sender is, from its browser ("iPhone"); "" for FBI's tools.
    std::string device;
    // FBI's tools wait on this connection for one byte, sent by
    // finishReceived once the links are done. -1 for the web page.
    int replySocket = -1;
    // What a sender to /send asked for besides the links.
    remote::SendOptions options;
    // Sent by 3dsDL's own page (remote::fromOwnPage) rather than by another
    // site, which may have sent them without the person asking.
    bool ownPage = false;
};

// Tells an FBI tool the links are done, so it can stop serving its files,
// and closes the connection.
void finishReceived(Received &received);

// A file going to or coming from the SD card through the page. Files the
// page only shows, such as pictures, are not reported.
struct Transfer {
    std::string name;
    std::string path; // on the SD card ("sdmc:/cias/a.cia")
    std::string from;
    std::string device; // as in Received
    bool upload = true; // false when the page is saving it from the card
    uint64_t done = 0;
    uint64_t total = 0;
    bool finished = false; // the last report for this file
    bool ok = false;       // with finished: the whole file went through
    // With finished and not ok: what went wrong on the console, or "" when
    // the sender stopped or the connection was lost.
    std::string message;
    // Where the page's batch stands, with its folder mapped onto the SD card
    // ("sdmc:/3ds/Checkpoint"). An upload from anything else, such as curl,
    // has none: an empty id.
    remote::UploadBatch batch;
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
// A folder or file the page lists beside the SD card itself.
struct Place {
    std::string name;
    std::string path; // on the card: "sdmc:/3ds", without a trailing '/'
};

struct FileSharing {
    std::string root; // "sdmc:"
    // Folders the page offers in its sidebar, and Files' favourites. Those
    // not there when the page asks are left out.
    std::vector<Place> places;
    std::vector<Place> favorites;
    // Whether the device at an address may use the files. When ask is set
    // and the device is unknown, the console asks and the answer is Pending
    // until it is given; without ask an unknown device is Unknown. device
    // names it for that question, as in Received. Unset allows everyone.
    std::function<Access(const std::string &from, const std::string &device,
                         bool ask)>
        access;
    // Whether links from an address are turned away before they reach the
    // console, such as a device the console said Don't Allow to. A list of
    // its own: answering the file question does not touch it. Unset takes
    // links from everyone.
    std::function<bool(const std::string &from)> refuseLinks;
    std::function<int64_t()> freeSpace;  // bytes, or -1 when unknown
    std::function<int64_t()> totalSpace; // bytes, or -1 when unknown
    // Unset reports "ready" with no text.
    std::function<ConsoleStatus()> status;
    // The running 3dsDL's version, to name its own CIAs (cia::versionText).
    std::string appVersion;
    // The page's code editor, gzipped (romfs/web/editor.js.gz), served at
    // /editor.js. Empty, or unreadable, leaves the page its plain text box.
    std::string editorScript;
    // Called a few times a second during a transfer, on the server's thread.
    std::function<void(const Transfer &)> progress;
    // Called on the server's thread with each path the page has written,
    // made, moved (both ends) or removed, once it has.
    std::function<void(const std::string &path)> changed;
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
