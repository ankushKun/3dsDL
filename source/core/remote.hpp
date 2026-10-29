#pragma once

#include <cstddef>
#include <cstdint>
#include <map>
#include <set>
#include <string>
#include <vector>

// Receiving links over the local network. Two ways in:
//
// - FBI's remote install protocol on port 5000, which servefiles.py,
//   sendurls.py and Boop speak: a 4-byte big-endian length, then that many
//   bytes of URLs separated by newlines. The sender keeps the connection
//   open and waits for one byte back, sent once everything has downloaded;
//   servefiles.py stops serving its files when it arrives.
// - A small web page, so a phone or computer on the same Wi-Fi can paste
//   links into a form, and browse, upload and download files on the SD card
//   through a little JSON API under /api/.
//
// Only parsing and page text live here; net/remote_server has the sockets.
namespace remote {

constexpr unsigned short FbiPort = 5000;
// The FBI tools send a few URLs; anything bigger is not them.
constexpr size_t MaxPayload = 256 * 1024;

// The http(s) links in text, in order and without repeats. Anything
// separated by whitespace is considered, so a pasted list, FBI's newline
// payload and a sentence with a link in it all work.
std::vector<std::string> extractLinks(const std::string &text);

// The length FBI's protocol puts first, from its 4 bytes.
size_t payloadLength(const unsigned char *bytes);

// FAT32 cannot hold a file of 4 GiB or more.
constexpr uint64_t MaxFileSize = 0xFFFFFFFFull;

struct HttpRequest {
    std::string method; // "GET"
    std::string path;   // "/send", without the query
    std::string query;  // after '?', still encoded
    std::map<std::string, std::string> headers; // names in lower case
    uint64_t contentLength = 0;
    std::string body;
};

enum class Parse { Incomplete, Done, Bad };

// Reads a request's head from the bytes received so far. Done once the
// blank line after the headers has arrived; bodyStart is then where the body
// begins in data. Bad for anything malformed, or a Content-Length over
// MaxFileSize.
Parse parseHead(const std::string &data, HttpRequest &out, size_t &bodyStart);

// Reads a request from the bytes received so far. Done once the head and a
// body of Content-Length bytes have arrived; Bad for anything malformed or
// over maxBody.
Parse parseRequest(const std::string &data, HttpRequest &out,
                   size_t maxBody = 64 * 1024);

// The value of field in an application/x-www-form-urlencoded string, decoded
// ('+' is a space, %XX a byte), or "" when it is missing.
std::string formValue(const std::string &form, const std::string &field);

// What a sender is, from its browser's User-Agent: "iPhone", "Mac",
// "Windows PC" and so on, or "" when it does not say.
std::string deviceName(const std::string &userAgent);

// Where an upload stands among the files the page sends in one go, from its
// X-3dsDL-Batch header: "<batch> <position> <count> <before> <bytes>", then,
// for a file in a folder that was dropped whole, "<files> <folder>" with the
// folder as a percent-encoded page path.
struct UploadBatch {
    std::string id;        // tells one drop from the next; "" for none
    uint32_t position = 0; // from 1
    uint32_t count = 0;
    uint64_t before = 0;   // bytes of the files sent before this one
    uint64_t bytes = 0;    // bytes of the whole batch
    std::string folder;    // "/3ds/Checkpoint", "" for a file on its own
    uint32_t folderFiles = 0; // how many of the batch go into folder
};
// False when the header is missing or malformed.
bool uploadBatch(const HttpRequest &request, UploadBatch &out);

// A whole response with Connection: close.
std::string httpResponse(int status, const std::string &contentType,
                         const std::string &body);

// The file API answers in JSON: {"error":"message"} for failures.
std::string jsonString(const std::string &value);
std::string jsonError(int status, const std::string &message);

// Whether a request may use the file API. Host must be an IPv4 address or a
// .local name, so another website cannot reach the API through a DNS name
// that points at the console. Anything that changes files must carry
// X-Requested-With: 3dsDL, which a page on another site cannot send without
// asking first, and 3dsDL never says yes. why says what was wrong.
bool apiRequestAllowed(const HttpRequest &request, std::string &why);

// Maps a path from the page ("/", "/3ds/app") onto root ("sdmc:"), giving
// "sdmc:/" or "sdmc:/3ds/app". Every part must be a valid FAT name, so ".."
// and the like cannot leave root.
bool localPath(const std::string &root, const std::string &webPath,
               std::string &out);

// What a link sender may ask for besides the links: folder=/3ds/app (a page
// path, as the file API takes) saves there instead of the Download Folder,
// and replace=1, with a folder, asks whether a link the queue kept beside a
// file of the same name should replace it. Any site can open /send, so a
// sender can name only the folders in sendFolderAllowed, and nothing is
// replaced without a choice on the console. False, with why, for any other
// folder.
struct SendOptions {
    std::string folder; // "sdmc:/3ds/app/", or "" for the Download Folder
    bool replace = false;
};
bool sendOptions(const std::string &form, SendOptions &out, std::string &why);

// Folders a sender may name ("sdmc:/3ds/app"): /3ds/ and /cias/ and what
// is in them, except 3dsDL's own folder. Never the root, /luma/ or anything
// else the system or Luma3DS loads from. Every part must be a plain ASCII
// name: no "." or "..", and no "~1", which on FAT can be another folder's
// short name.
bool sendFolderAllowed(const std::string &path);

// Whether a sender's file may have this name: plain ASCII, never 3dsDL.3dsx
// or boot.3dsx, the Homebrew Launcher's own name, and never a name like
// "APP~1.CIA", which on FAT can be another file's short name.
bool sentNameAllowed(const std::string &filename);

// Whether a file a sender's link saved may replace one of the same name:
// only a .3dsx or a .cia, which is what an update is, never another app's
// settings or data.
bool sentMayReplace(const std::string &filename);

// The older file to ask about replacing, for a file a sender's link saved
// at path under the server's filename, in the folder the sender named: one
// of that name the queue kept it beside (finalize::keptBeside), when
// sentMayReplace allows it and it is not in saved, the lower-case paths
// this same send wrote. "" when there is nothing to ask about.
std::string replaceCandidate(const std::string &path, const std::string &filename,
                             const std::string &folder,
                             const std::set<std::string> &saved);

// The Content-Type to send a file as. Pages and scripts are sent as plain
// text, so a file opened from the SD card can never run in the page.
const char *mimeType(const std::string &name);

// A 48x48 icon (RGB565, row by row, as smdh and titlemeta give it) as a
// 24-bit BMP, which every browser shows and needs no compressor to make.
// Empty when icon is not 48x48.
std::string iconBmp(const std::vector<uint16_t> &icon);

// A Content-Disposition header value for name, with an ASCII fallback and
// the UTF-8 name for browsers that read it.
std::string contentDisposition(const std::string &name, bool asAttachment);

// The page a phone sees: a box to paste links into. message, if any, shows
// above it, e.g. after links were sent.
std::string page(const std::string &message = "", bool good = true);

} // namespace remote
