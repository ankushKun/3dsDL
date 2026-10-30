#include "remote.hpp"

#include "archives.hpp"
#include "cia.hpp"
#include "file_ops.hpp"
#include "file_types.hpp"
#include "finalize.hpp"
#include "naming.hpp"
#include "text.hpp"
#include "title_meta.hpp"
#include "url.hpp"

#include <arpa/inet.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <set>

namespace remote {

namespace {

int hexValue(char c)
{
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'a' && c <= 'f') {
        return c - 'a' + 10;
    }
    if (c >= 'A' && c <= 'F') {
        return c - 'A' + 10;
    }
    return -1;
}

std::string formDecode(const std::string &value)
{
    std::string out;
    out.reserve(value.size());
    for (size_t index = 0; index < value.size(); ++index) {
        const char c = value[index];
        if (c == '+') {
            out += ' ';
        } else if (c == '%' && index + 2 < value.size() &&
                   hexValue(value[index + 1]) >= 0 &&
                   hexValue(value[index + 2]) >= 0) {
            out += static_cast<char>(hexValue(value[index + 1]) * 16 +
                                     hexValue(value[index + 2]));
            index += 2;
        } else {
            out += c;
        }
    }
    return out;
}

const char *statusText(int status)
{
    switch (status) {
    case 200:
        return "OK";
    case 303:
        return "See Other";
    case 400:
        return "Bad Request";
    case 403:
        return "Forbidden";
    case 404:
        return "Not Found";
    case 405:
        return "Method Not Allowed";
    case 409:
        return "Conflict";
    case 411:
        return "Length Required";
    case 413:
        return "Payload Too Large";
    case 500:
        return "Internal Server Error";
    case 507:
        return "Insufficient Storage";
    default:
        return "Error";
    }
}

// A name a sender may not use: "~" and a digit anywhere, which on FAT can
// be another file's or folder's short name (BOOT~1.FIR for boot.firm), or
// anything past ASCII, whose case FAT may fold in ways text::lower does not.
bool unsafeSentName(const std::string &name)
{
    for (size_t at = 0; at < name.size(); ++at) {
        const unsigned char c = static_cast<unsigned char>(name[at]);
        if (c >= 0x80 || (c == '~' && at + 1 < name.size() &&
                          std::isdigit(static_cast<unsigned char>(name[at + 1])))) {
            return true;
        }
    }
    return false;
}

} // namespace

std::vector<std::string> extractLinks(const std::string &value)
{
    std::vector<std::string> links;
    std::set<std::string> seen;
    size_t start = 0;
    while (start < value.size()) {
        while (start < value.size() &&
               std::isspace(static_cast<unsigned char>(value[start]))) {
            ++start;
        }
        size_t end = start;
        while (end < value.size() &&
               !std::isspace(static_cast<unsigned char>(value[end]))) {
            ++end;
        }
        std::string word = value.substr(start, end - start);
        start = end;
        // A link at the end of a sentence or in brackets.
        while (!word.empty() && std::string(".,;)>]'\"").find(word.back()) !=
                                    std::string::npos) {
            word.pop_back();
        }
        while (!word.empty() && std::string("(<['\"").find(word.front()) !=
                                    std::string::npos) {
            word.erase(0, 1);
        }
        if (url::isHttpUrl(word) && seen.insert(word).second) {
            links.push_back(word);
        }
    }
    return links;
}

size_t payloadLength(const unsigned char *bytes)
{
    return static_cast<size_t>(bytes[0]) << 24 |
           static_cast<size_t>(bytes[1]) << 16 |
           static_cast<size_t>(bytes[2]) << 8 | static_cast<size_t>(bytes[3]);
}

Parse parseHead(const std::string &data, HttpRequest &out, size_t &bodyStart)
{
    const size_t headEnd = data.find("\r\n\r\n");
    if (headEnd == std::string::npos) {
        return data.size() > 16 * 1024 ? Parse::Bad : Parse::Incomplete;
    }
    out = HttpRequest();
    const std::string head = data.substr(0, headEnd);
    size_t lineEnd = head.find("\r\n");
    const std::string requestLine = head.substr(0, lineEnd);
    const size_t firstSpace = requestLine.find(' ');
    const size_t secondSpace = requestLine.find(' ', firstSpace + 1);
    if (firstSpace == std::string::npos || secondSpace == std::string::npos ||
        requestLine.compare(secondSpace + 1, 5, "HTTP/") != 0) {
        return Parse::Bad;
    }
    out.method = requestLine.substr(0, firstSpace);
    const std::string target =
        requestLine.substr(firstSpace + 1, secondSpace - firstSpace - 1);
    const size_t question = target.find('?');
    out.path = target.substr(0, question);
    if (question != std::string::npos) {
        out.query = target.substr(question + 1);
    }
    if (out.path.empty() || out.path[0] != '/') {
        return Parse::Bad;
    }

    while (lineEnd != std::string::npos) {
        const size_t start = lineEnd + 2;
        lineEnd = head.find("\r\n", start);
        const std::string line = head.substr(
            start, lineEnd == std::string::npos ? std::string::npos
                                                : lineEnd - start);
        const size_t colon = line.find(':');
        if (colon == std::string::npos) {
            continue;
        }
        out.headers[text::lower(text::trim(line.substr(0, colon)))] =
            text::trim(line.substr(colon + 1));
    }

    const auto found = out.headers.find("content-length");
    if (found != out.headers.end()) {
        const std::string &value = found->second;
        if (value.empty() ||
            value.find_first_not_of("0123456789") != std::string::npos ||
            value.size() > 10) {
            return Parse::Bad;
        }
        out.contentLength = std::strtoull(value.c_str(), nullptr, 10);
        if (out.contentLength > MaxFileSize) {
            return Parse::Bad;
        }
    }
    bodyStart = headEnd + 4;
    return Parse::Done;
}

Parse parseRequest(const std::string &data, HttpRequest &out, size_t maxBody)
{
    size_t bodyStart = 0;
    const Parse head = parseHead(data, out, bodyStart);
    if (head != Parse::Done) {
        return head;
    }
    if (out.contentLength > maxBody) {
        return Parse::Bad;
    }
    const size_t length = static_cast<size_t>(out.contentLength);
    if (data.size() < bodyStart + length) {
        return Parse::Incomplete;
    }
    out.body = data.substr(bodyStart, length);
    return Parse::Done;
}

std::string formValue(const std::string &form, const std::string &field)
{
    size_t start = 0;
    while (start <= form.size()) {
        size_t end = form.find('&', start);
        if (end == std::string::npos) {
            end = form.size();
        }
        const std::string pair = form.substr(start, end - start);
        const size_t equals = pair.find('=');
        if (formDecode(pair.substr(0, equals)) == field) {
            return equals == std::string::npos
                       ? ""
                       : formDecode(pair.substr(equals + 1));
        }
        start = end + 1;
    }
    return "";
}

std::string deviceName(const std::string &userAgent)
{
    static const struct {
        const char *marker;
        const char *name;
    } devices[] = {
        // Nintendo's browsers say "like iPhone", and phones and tablets name
        // the desktop systems they are built on, so these come first.
        {"Nintendo 3DS", "3DS"},
        {"Nintendo Switch", "Switch"},
        {"iPhone", "iPhone"},
        {"iPad", "iPad"},
        {"Android", "Android"},
        {"CrOS", "Chromebook"},
        {"Windows", "Windows PC"},
        {"Macintosh", "Mac"},
        {"Linux", "Linux PC"},
        {"curl/", "curl"},
        {"Wget/", "Wget"},
    };
    for (const auto &device : devices) {
        if (userAgent.find(device.marker) != std::string::npos) {
            return device.name;
        }
    }
    return "";
}

bool uploadBatch(const HttpRequest &request, UploadBatch &out)
{
    const auto header = request.headers.find("x-3dsdl-batch");
    if (header == request.headers.end()) {
        return false;
    }
    std::vector<std::string> parts;
    size_t start = 0;
    while (start <= header->second.size() && parts.size() < 8) {
        size_t end = header->second.find(' ', start);
        if (end == std::string::npos) {
            end = header->second.size();
        }
        parts.push_back(header->second.substr(start, end - start));
        start = end + 1;
    }
    const auto digits = [](const std::string &text, size_t most) {
        return !text.empty() && text.size() <= most &&
               text.find_first_not_of("0123456789") == std::string::npos;
    };
    const auto count = [&](const std::string &text, uint32_t &value) {
        value = digits(text, 7)
                    ? static_cast<uint32_t>(std::strtoul(text.c_str(), nullptr, 10))
                    : 0;
        return value > 0;
    };
    const auto bytes = [&](const std::string &text, uint64_t &value) {
        value = std::strtoull(text.c_str(), nullptr, 10);
        return digits(text, 15);
    };
    UploadBatch batch;
    if ((parts.size() != 5 && parts.size() != 7) || parts[0].empty() ||
        parts[0].size() > 24 ||
        parts[0].find_first_not_of("0123456789abcdefghijklmnopqrstuvwxyz") !=
            std::string::npos ||
        !count(parts[1], batch.position) || !count(parts[2], batch.count) ||
        batch.position > batch.count || !bytes(parts[3], batch.before) ||
        !bytes(parts[4], batch.bytes) || batch.before > batch.bytes) {
        return false;
    }
    batch.id = parts[0];
    if (parts.size() == 7) {
        batch.folder = url::percentDecode(parts[6]);
        if (!count(parts[5], batch.folderFiles) ||
            batch.folderFiles > batch.count || batch.folder.size() < 2 ||
            batch.folder[0] != '/') {
            return false;
        }
    }
    out = batch;
    return true;
}

std::string httpResponse(int status, const std::string &contentType,
                         const std::string &body)
{
    std::string response = "HTTP/1.1 " + std::to_string(status) + " " +
                           statusText(status) + "\r\n";
    response += "Content-Type: " + contentType + "\r\n";
    response += "Content-Length: " + std::to_string(body.size()) + "\r\n";
    response += "Cache-Control: no-store\r\n";
    response += "Connection: close\r\n\r\n";
    return response + body;
}

std::string jsonString(const std::string &value)
{
    std::string out = "\"";
    for (const char c : value) {
        switch (c) {
        case '"':
            out += "\\\"";
            break;
        case '\\':
            out += "\\\\";
            break;
        case '\n':
            out += "\\n";
            break;
        default:
            if (static_cast<unsigned char>(c) < 0x20) {
                char escaped[8];
                std::snprintf(escaped, sizeof(escaped), "\\u%04x", c);
                out += escaped;
            } else {
                out += c;
            }
        }
    }
    return out + "\"";
}

std::string jsonError(int status, const std::string &message)
{
    return httpResponse(status, "application/json",
                        "{\"error\":" + jsonString(message) + "}");
}

namespace {

// Whether the request was made to the console by its address or a .local
// name, rather than by a name another site controls (DNS rebinding).
bool toConsole(const HttpRequest &request)
{
    const auto host = request.headers.find("host");
    std::string name = host == request.headers.end() ? "" : host->second;
    const size_t colon = name.rfind(':');
    if (colon != std::string::npos &&
        name.find_first_not_of("0123456789", colon + 1) == std::string::npos) {
        name.erase(colon);
    }
    name = text::lower(name);
    in_addr parsed {};
    const bool address = !name.empty() &&
                         name.find_first_not_of("0123456789.") ==
                             std::string::npos &&
                         inet_aton(name.c_str(), &parsed) != 0;
    const bool local = name.size() > 6 &&
                       name.compare(name.size() - 6, 6, ".local") == 0;
    return address || local;
}

bool markedByPage(const HttpRequest &request)
{
    const auto marker = request.headers.find("x-requested-with");
    return marker != request.headers.end() && marker->second == "3dsDL";
}

} // namespace

bool apiRequestAllowed(const HttpRequest &request, std::string &why)
{
    if (!toConsole(request)) {
        why = "Open this page by the console's address.";
        return false;
    }
    if (request.method != "GET" && !markedByPage(request)) {
        why = "Changes must come from the 3dsDL page.";
        return false;
    }
    return true;
}

bool fromOwnPage(const HttpRequest &request)
{
    return toConsole(request) && markedByPage(request);
}

bool localPath(const std::string &root, const std::string &webPath,
               std::string &out)
{
    if (webPath.empty() || webPath[0] != '/') {
        return false;
    }
    std::string path = root;
    size_t start = 1;
    while (start < webPath.size()) {
        size_t end = webPath.find('/', start);
        if (end == std::string::npos) {
            end = webPath.size();
        }
        const std::string part = webPath.substr(start, end - start);
        start = end + 1;
        if (part.empty() && end == webPath.size()) {
            break; // a trailing '/'
        }
        std::string error;
        if (part != text::trim(part) || !fileops::validName(part, error)) {
            return false;
        }
        path += "/" + part;
    }
    out = path == root ? root + "/" : path;
    return true;
}

bool sendOptions(const std::string &form, SendOptions &out, std::string &why)
{
    out = SendOptions();
    if (extractLinks(formValue(form, "links")).size() > MaxSentLinks) {
        why = "Send at most " + std::to_string(MaxSentLinks) + " links at once.";
        return false;
    }
    const std::string folder = formValue(form, "folder");
    if (!folder.empty()) {
        std::string path;
        if (!localPath("sdmc:", folder, path) || !sendFolderAllowed(path)) {
            why = "Links can only be saved in /3ds/ or /cias/, or one folder "
                  "in them.";
            return false;
        }
        out.folder = path.back() == '/' ? path : path + "/";
        // Apps live in /3ds/ or one folder in it; what is deeper is their
        // data, which a sender has no business in.
        if (std::count(out.folder.begin() + 6, out.folder.end(), '/') > 2) {
            out = SendOptions();
            why = "Links can only be saved in /3ds/ or /cias/, or one folder "
                  "in them.";
            return false;
        }
    }
    out.replace = formValue(form, "replace") == "1";
    out.extract = formValue(form, "extract") == "1";
    return true;
}

bool sendFolderAllowed(const std::string &path)
{
    const std::string card = "sdmc:/";
    const std::string folder =
        text::lower(path) + (!path.empty() && path.back() == '/' ? "" : "/");
    const auto under = [&](const std::string &top) {
        return folder.compare(0, top.size(), top) == 0;
    };
    if (!(under("sdmc:/3ds/") && !under("sdmc:/3ds/3dsdl/")) &&
        !under("sdmc:/cias/")) {
        return false;
    }
    for (size_t start = card.size(); start < folder.size();) {
        const size_t end = folder.find('/', start);
        const std::string part = folder.substr(start, end - start);
        if (part.empty() || part == "." || part == ".." || unsafeSentName(part)) {
            return false;
        }
        start = end + 1;
    }
    return true;
}

bool sentNameAllowed(const std::string &filename)
{
    const std::string name = text::lower(filename);
    return !name.empty() && !unsafeSentName(name) && name != "boot.3dsx" &&
           name != "3dsdl.3dsx";
}

bool sentFileAllowed(const std::string &filename)
{
    const filetype::Kind kind = filetype::fromName(filename);
    return sentNameAllowed(filename) &&
           (kind == filetype::Kind::Homebrew || kind == filetype::Kind::Cia ||
            kind == filetype::Kind::Archive);
}

bool sameApp(const std::string &older, const std::string &newer,
             std::string *name)
{
    const filetype::Kind kind = filetype::fromName(older);
    if (kind != filetype::fromName(newer)) {
        return false;
    }
    std::string error;
    if (kind == filetype::Kind::Cia) {
        cia::Info was;
        cia::Info now;
        if (!cia::readInfo(older, was, error, true) ||
            !cia::readInfo(newer, now, error, true) || was.titleId != now.titleId) {
            return false;
        }
        if (name != nullptr) {
            *name = !now.smdh.shortName.empty() ? now.smdh.shortName
                                                : cia::titleIdText(now.titleId);
        }
        return true;
    }
    if (kind == filetype::Kind::Homebrew) {
        titlemeta::Meta was;
        titlemeta::Meta now;
        if (!titlemeta::read(older, kind, was, error) ||
            !titlemeta::read(newer, kind, now, error) || was.name.empty() ||
            text::lower(was.name) != text::lower(now.name) ||
            text::lower(was.publisher) != text::lower(now.publisher)) {
            return false;
        }
        if (name != nullptr) {
            *name = now.name;
        }
        return true;
    }
    return false;
}

bool sentMayReplace(const std::string &filename)
{
    return sentNameAllowed(filename) &&
           (text::endsWithNoCase(filename, ".3dsx") ||
            text::endsWithNoCase(filename, ".cia"));
}

std::string replaceCandidate(const std::string &path, const std::string &filename,
                             const std::string &folder,
                             const std::set<std::string> &saved)
{
    if (naming::parentFolder(path) != folder || !sentMayReplace(filename)) {
        return "";
    }
    const std::string older = finalize::keptBeside(path, filename);
    return older.empty() || saved.count(text::lower(older)) > 0 ? "" : older;
}

bool sentArchive(const std::vector<std::string> &paths,
                 const std::string &filename, SentArchive &out)
{
    out = SentArchive();
    const auto head = [](const std::string &path) {
        return text::lower(path.substr(0, path.find('/')));
    };
    bool fromRoot = false;
    bool inFolder = false; // something is inside its one top
    std::set<std::string> tops;
    for (const std::string &path : paths) {
        const std::string top = head(path);
        fromRoot = fromRoot || top == "3ds" || top == "cias" || top == "luma" ||
                   top == "gm9";
        inFolder = inFolder || path.find('/') != std::string::npos;
        tops.insert(top);
    }
    if (fromRoot) {
        out.strip = "3ds/";
        tops.clear();
        for (const std::string &path : paths) {
            if (path.size() > 4 && text::lower(path).rfind("3ds/", 0) == 0) {
                tops.insert(head(path.substr(4)));
            }
        }
        if (tops.size() != 1) {
            return false;
        }
        out.top = *tops.begin();
    } else if (tops.size() == 1 && inFolder) {
        out.top = *tops.begin();
    } else {
        out.prefix = naming::safeFilename(naming::asciiFilename(
                         naming::archiveStem(filename))) +
                     "/";
        out.top = text::lower(out.prefix.substr(0, out.prefix.size() - 1));
    }
    return !out.top.empty() && out.top[0] != '.' &&
           sendFolderAllowed(ArchiveTarget + out.top);
}

std::string sentArchivePlace(const SentArchive &archive,
                             const std::string &path, bool directory)
{
    const size_t strip = archive.strip.size();
    if (path.size() <= strip ||
        text::lower(path.substr(0, strip)) != archive.strip) {
        return ""; // the 3ds folder itself, or what is beside it
    }
    const std::string placed = archive.prefix + path.substr(strip);
    const std::string name = placed.substr(placed.find_last_of('/') + 1);
    const bool allowed =
        text::lower(placed.substr(0, placed.find('/'))) == archive.top &&
        sendFolderAllowed(ArchiveTarget + placed) &&
        (directory ||
         (sentNameAllowed(name) && !text::endsWithNoCase(name, ".firm")));
    return allowed ? placed : "";
}

bool sentExtraction(const archives::Listing &listing,
                    const std::string &filename, archives::Options &options,
                    archives::Listing &shown)
{
    std::vector<std::string> paths;
    paths.reserve(listing.entries.size());
    for (const archives::Entry &entry : listing.entries) {
        if (!entry.path.empty() && !archives::macMetadata(entry.path)) {
            paths.push_back(entry.path);
        }
    }
    SentArchive archive;
    if (!sentArchive(paths, filename, archive)) {
        return false;
    }
    options.place = [archive](const std::string &path, bool directory) {
        return sentArchivePlace(archive, path, directory);
    };
    // libarchive takes a ZIP's names from its local headers, which need not
    // be the ones its listing showed; 3dsDL's own reader writes the listing's.
    if (listing.format == "ZIP" && !listing.ownZip) {
        options.only = std::move(paths);
    }
    shown = archives::placed(listing, options.place);
    return shown.fileCount > 0;
}

const char *mimeType(const std::string &name)
{
    const size_t dot = name.rfind('.');
    const std::string extension =
        dot == std::string::npos ? "" : text::lower(name.substr(dot + 1));
    static const struct {
        const char *extension;
        const char *type;
    } types[] = {
        {"png", "image/png"},        {"jpg", "image/jpeg"},
        {"jpeg", "image/jpeg"},      {"gif", "image/gif"},
        {"bmp", "image/bmp"},        {"webp", "image/webp"},
        {"mp3", "audio/mpeg"},       {"ogg", "audio/ogg"},
        {"wav", "audio/wav"},        {"mp4", "video/mp4"},
        {"pdf", "application/pdf"},  {"json", "text/plain; charset=utf-8"},
        {"txt", "text/plain; charset=utf-8"},
        {"ini", "text/plain; charset=utf-8"},
        {"cfg", "text/plain; charset=utf-8"},
        {"log", "text/plain; charset=utf-8"},
        {"md", "text/plain; charset=utf-8"},
        {"xml", "text/plain; charset=utf-8"},
        {"html", "text/plain; charset=utf-8"},
        {"htm", "text/plain; charset=utf-8"},
        {"svg", "text/plain; charset=utf-8"},
        {"js", "text/plain; charset=utf-8"},
    };
    for (const auto &entry : types) {
        if (extension == entry.extension) {
            return entry.type;
        }
    }
    return "application/octet-stream";
}

std::string iconBmp(const std::vector<uint16_t> &icon)
{
    constexpr uint32_t Side = 48;
    if (icon.size() != Side * Side) {
        return "";
    }
    const uint32_t pixels = Side * Side * 3; // 144-byte rows need no padding
    std::string out;
    const auto put32 = [&out](uint32_t value) {
        for (int shift = 0; shift < 32; shift += 8) {
            out += static_cast<char>((value >> shift) & 0xFF);
        }
    };
    const auto put16 = [&out](uint16_t value) {
        out += static_cast<char>(value & 0xFF);
        out += static_cast<char>(value >> 8);
    };
    out += "BM";
    put32(54 + pixels);
    put32(0);
    put32(54);
    put32(40);
    put32(Side);
    put32(Side); // positive height: rows go bottom up
    put16(1);
    put16(24);
    put32(0);
    put32(pixels);
    put32(2835);
    put32(2835);
    put32(0);
    put32(0);
    for (uint32_t row = Side; row-- > 0;) {
        for (uint32_t column = 0; column < Side; ++column) {
            const uint16_t colour = icon[row * Side + column];
            const uint8_t red = static_cast<uint8_t>((colour >> 11) & 0x1F);
            const uint8_t green = static_cast<uint8_t>((colour >> 5) & 0x3F);
            const uint8_t blue = static_cast<uint8_t>(colour & 0x1F);
            out += static_cast<char>((blue << 3) | (blue >> 2));
            out += static_cast<char>((green << 2) | (green >> 4));
            out += static_cast<char>((red << 3) | (red >> 2));
        }
    }
    return out;
}

std::string contentDisposition(const std::string &name, bool asAttachment)
{
    std::string fallback;
    std::string encoded;
    for (const char c : name) {
        const unsigned char byte = static_cast<unsigned char>(c);
        fallback += byte >= 0x20 && byte < 0x7f && c != '"' && c != '\\'
                        ? c
                        : '_';
        if (std::isalnum(byte) || std::string("-._~").find(c) !=
                                      std::string::npos) {
            encoded += c;
        } else {
            char escaped[4];
            std::snprintf(escaped, sizeof(escaped), "%%%02X", byte);
            encoded += escaped;
        }
    }
    return std::string(asAttachment ? "attachment" : "inline") +
           "; filename=\"" + fallback + "\"; filename*=UTF-8''" + encoded;
}

} // namespace remote
