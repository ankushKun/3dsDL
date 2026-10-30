#include "naming.hpp"

#include "text.hpp"
#include "url.hpp"

#include <utility>
#include <vector>

namespace naming {
namespace {

constexpr size_t MaxKeptExtensionBytes = 16;
constexpr const char *ReservedPrefix = ".3dsdl-";

std::string basename(const std::string &path)
{
    const size_t slash = path.find_last_of("/\\");
    return slash == std::string::npos ? path : path.substr(slash + 1);
}

bool isReservedCharacter(char character)
{
    const unsigned char byte = static_cast<unsigned char>(character);
    return byte < 0x20 || byte == 0x7f || character == ':' ||
           character == '*' || character == '?' || character == '"' ||
           character == '<' || character == '>' || character == '|' ||
           character == '/' || character == '\\';
}

std::string shorten(const std::string &filename)
{
    if (filename.size() <= MaxFilenameBytes) {
        return filename;
    }
    const std::string ext = extension(filename);
    if (!ext.empty() && ext.size() <= MaxKeptExtensionBytes) {
        return text::truncateUtf8(stem(filename),
                                  MaxFilenameBytes - ext.size()) + ext;
    }
    return text::truncateUtf8(filename, MaxFilenameBytes);
}

// Splits "attachment; filename=\"a b.zip\"; size=3" into name/value pairs,
// honouring quoted strings and backslash escapes inside them.
std::vector<std::pair<std::string, std::string>>
splitParameters(const std::string &header)
{
    std::vector<std::pair<std::string, std::string>> parameters;
    std::string name;
    std::string value;
    bool inName = true;
    bool inQuotes = false;
    bool sawQuotes = false;

    const auto flush = [&]() {
        if (!inName) {
            parameters.emplace_back(text::lower(text::trim(name)),
                                    sawQuotes ? value : text::trim(value));
        }
        name.clear();
        value.clear();
        inName = true;
        sawQuotes = false;
    };

    for (size_t index = 0; index < header.size(); ++index) {
        const char character = header[index];
        if (inQuotes) {
            if (character == '\\' && index + 1 < header.size()) {
                value += header[++index];
            } else if (character == '"') {
                inQuotes = false;
            } else {
                value += character;
            }
        } else if (character == ';') {
            flush();
        } else if (inName && character == '=') {
            inName = false;
        } else if (inName) {
            name += character;
        } else if (character == '"' && text::trim(value).empty()) {
            value.clear();
            inQuotes = true;
            sawQuotes = true;
        } else if (!sawQuotes) {
            value += character;
        }
    }
    flush();
    return parameters;
}

std::string decodeExtendedValue(const std::string &value)
{
    const size_t firstQuote = value.find('\'');
    if (firstQuote == std::string::npos) {
        return "";
    }
    const size_t secondQuote = value.find('\'', firstQuote + 1);
    if (secondQuote == std::string::npos) {
        return "";
    }

    const std::string charset = text::lower(value.substr(0, firstQuote));
    const std::string decoded =
        url::percentDecode(value.substr(secondQuote + 1));
    if (charset == "utf-8") {
        return decoded;
    }
    if (charset == "iso-8859-1") {
        return text::latin1ToUtf8(decoded);
    }
    return "";
}

} // namespace

std::string safeFilename(const std::string &name)
{
    std::string filename = text::sanitizeUtf8(basename(name));
    for (char &character : filename) {
        if (isReservedCharacter(character)) {
            character = '_';
        }
    }

    filename = text::trim(filename);
    while (!filename.empty() &&
           (filename.back() == ' ' || filename.back() == '.')) {
        filename.pop_back();
    }

    if (filename.empty() || filename == "." || filename == "..") {
        return DefaultFilename;
    }
    // Never let a server pick a name that looks like 3dsDL's own temp files.
    if (text::startsWithNoCase(filename, ReservedPrefix)) {
        filename.insert(filename.begin(), '_');
    }
    // Cutting a long name short can end it in a space or a dot again.
    filename = shorten(filename);
    while (!filename.empty() &&
           (filename.back() == ' ' || filename.back() == '.')) {
        filename.pop_back();
    }
    return filename.empty() ? DefaultFilename : filename;
}

std::string asciiFilename(const std::string &name)
{
    std::string filename = safeFilename(name);
    std::string output;
    for (size_t index = 0; index < filename.size();) {
        const size_t length = text::utf8SequenceLength(filename, index);
        if (length == 1) {
            output += filename[index];
        } else {
            output += '_';
        }
        index += length == 0 ? 1 : length;
    }
    return safeFilename(output);
}

std::string contentDispositionFilename(const std::string &header)
{
    std::string plain;
    for (const auto &parameter : splitParameters(header)) {
        if (parameter.first == "filename*") {
            const std::string decoded = decodeExtendedValue(parameter.second);
            if (!decoded.empty()) {
                return decoded;
            }
        } else if (parameter.first == "filename" && plain.empty()) {
            plain = parameter.second;
        }
    }
    return plain;
}

std::string urlFilename(const std::string &address)
{
    return url::percentDecode(url::lastPathSegment(address));
}

std::string chooseFilename(const std::string &contentDisposition,
                           const std::string &effectiveUrl)
{
    std::string filename = contentDispositionFilename(contentDisposition);
    if (text::trim(filename).empty()) {
        filename = urlFilename(effectiveUrl);
    }
    return safeFilename(filename);
}

std::string stem(const std::string &filename)
{
    const size_t dot = filename.find_last_of('.');
    return dot == std::string::npos || dot == 0 ? filename
                                                : filename.substr(0, dot);
}

std::string archiveStem(const std::string &filename)
{
    std::string name = stem(filename);
    if (text::endsWithNoCase(name, ".tar")) {
        name.resize(name.size() - 4);
    }
    return name;
}

std::string extension(const std::string &filename)
{
    const size_t dot = filename.find_last_of('.');
    return dot == std::string::npos || dot == 0 ? "" : filename.substr(dot);
}

std::string unusedPath(const std::string &folder, const std::string &filename,
                       const ExistsFunction &exists, unsigned limit)
{
    std::string candidate = folder + filename;
    if (!exists(candidate)) {
        return candidate;
    }

    const std::string base = stem(filename);
    const std::string ext = extension(filename);
    for (unsigned index = 1; index <= limit; ++index) {
        candidate = folder + base + " (" + std::to_string(index) + ")" + ext;
        if (!exists(candidate)) {
            return candidate;
        }
    }
    return "";
}

std::string normaliseFolder(const std::string &folder)
{
    std::string normalised = text::trim(folder);
    if (normalised.empty()) {
        return "sdmc:/";
    }
    if (normalised.back() != '/') {
        normalised += '/';
    }
    return normalised;
}

bool isSdPath(const std::string &path)
{
    return text::startsWithNoCase(path, "sdmc:/");
}

std::string parentFolder(const std::string &folder)
{
    const std::string normalised = normaliseFolder(folder);
    const size_t rootEnd = normalised.find(":/");
    const size_t minimum = rootEnd == std::string::npos ? 1 : rootEnd + 2;
    if (normalised.size() <= minimum) {
        return normalised;
    }
    const size_t slash = normalised.find_last_of('/', normalised.size() - 2);
    if (slash == std::string::npos || slash + 1 < minimum) {
        return normalised.substr(0, minimum);
    }
    return normalised.substr(0, slash + 1);
}

} // namespace naming
