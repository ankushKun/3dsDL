#include "url.hpp"

#include <vector>

#include "text.hpp"

#include <algorithm>

namespace url {
namespace {

// Everything after "scheme://", or "" if value has no http(s) scheme.
std::string afterScheme(const std::string &value)
{
    if (text::startsWithNoCase(value, "http://")) {
        return value.substr(7);
    }
    if (text::startsWithNoCase(value, "https://")) {
        return value.substr(8);
    }
    return "";
}

std::string authority(const std::string &value)
{
    const std::string rest = afterScheme(value);
    return rest.substr(0, rest.find_first_of("/?#"));
}

int hexValue(char character)
{
    if (character >= '0' && character <= '9') {
        return character - '0';
    }
    if (character >= 'a' && character <= 'f') {
        return character - 'a' + 10;
    }
    if (character >= 'A' && character <= 'F') {
        return character - 'A' + 10;
    }
    return -1;
}

} // namespace

bool isHttpUrl(const std::string &value)
{
    if (afterScheme(value).empty()) {
        return false;
    }
    for (const char character : value) {
        const unsigned char byte = static_cast<unsigned char>(character);
        if (byte <= 0x20 || byte == 0x7f) {
            return false;
        }
    }
    return !host(value).empty();
}

bool isHttps(const std::string &value)
{
    return text::startsWithNoCase(value, "https://");
}

std::string host(const std::string &value)
{
    std::string hostPart = authority(value);
    const size_t at = hostPart.rfind('@');
    if (at != std::string::npos) {
        hostPart.erase(0, at + 1);
    }

    if (!hostPart.empty() && hostPart.front() == '[') {
        const size_t close = hostPart.find(']');
        return close == std::string::npos
            ? ""
            : text::lower(hostPart.substr(0, close + 1));
    }

    const size_t colon = hostPart.find(':');
    if (colon != std::string::npos) {
        hostPart.erase(colon);
    }
    return text::lower(hostPart);
}

std::string site(const std::string &host)
{
    if (host.empty() || host.front() == '[' ||
        host.find_first_not_of("0123456789.") == std::string::npos) {
        return host;
    }
    std::vector<std::string> labels;
    size_t start = 0;
    while (true) {
        const size_t dot = host.find('.', start);
        labels.push_back(host.substr(start, dot - start));
        if (dot == std::string::npos) {
            break;
        }
        start = dot + 1;
    }
    const size_t count = labels.size();
    if (count <= 2) {
        return host;
    }
    static const char *const SecondLevels[] = {"co", "com", "net", "org", "gov",
                                               "edu", "ac", "or", "ne", "go"};
    size_t keep = 2;
    if (labels[count - 1].size() == 2) {
        for (const char *level : SecondLevels) {
            if (labels[count - 2] == level) {
                keep = 3;
            }
        }
    }
    std::string out;
    for (size_t index = count - keep; index < count; ++index) {
        out += (out.empty() ? "" : ".") + labels[index];
    }
    return out;
}

std::string lastPathSegment(const std::string &value)
{
    std::string rest = afterScheme(value);
    rest.erase(std::min(rest.find_first_of("?#"), rest.size()));

    const size_t pathStart = rest.find('/');
    if (pathStart == std::string::npos) {
        return "";
    }
    const std::string path = rest.substr(pathStart);
    return path.substr(path.find_last_of('/') + 1);
}

std::string percentDecode(const std::string &value)
{
    std::string output;
    output.reserve(value.size());
    for (size_t index = 0; index < value.size(); ++index) {
        if (value[index] == '%' && index + 2 < value.size()) {
            const int high = hexValue(value[index + 1]);
            const int low = hexValue(value[index + 2]);
            if (high >= 0 && low >= 0) {
                output += static_cast<char>((high << 4) | low);
                index += 2;
                continue;
            }
        }
        output += value[index];
    }
    return output;
}

std::string percentEncode(const std::string &value)
{
    static const char *const Hex = "0123456789ABCDEF";
    std::string output;
    output.reserve(value.size());
    for (const char character : value) {
        const unsigned char byte = static_cast<unsigned char>(character);
        if ((byte >= 'a' && byte <= 'z') || (byte >= 'A' && byte <= 'Z') ||
            (byte >= '0' && byte <= '9') || byte == '-' || byte == '.' ||
            byte == '_' || byte == '~') {
            output += character;
        } else {
            output += '%';
            output += Hex[byte >> 4];
            output += Hex[byte & 0x0f];
        }
    }
    return output;
}

} // namespace url
