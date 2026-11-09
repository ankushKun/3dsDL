#include "update.hpp"

#include "json.hpp"

#include <algorithm>
#include <cctype>
#include <cstdlib>

namespace update {

namespace {

struct Version {
    std::vector<long> numbers;
    std::string label; // "beta.1" in "0.8.0-beta.1"; empty for a release
};

// Reads "v1.2.3-beta.1+build" leniently: the numbers until something else,
// then a label after '-', and nothing after '+'.
Version parseVersion(const std::string &text)
{
    Version out;
    size_t position = 0;
    while (position < text.size() &&
           std::isspace(static_cast<unsigned char>(text[position]))) {
        ++position;
    }
    if (position < text.size() && (text[position] == 'v' || text[position] == 'V')) {
        ++position;
    }
    while (position < text.size() &&
           std::isdigit(static_cast<unsigned char>(text[position]))) {
        long value = 0;
        while (position < text.size() &&
               std::isdigit(static_cast<unsigned char>(text[position]))) {
            if (value < 100000000) {
                value = value * 10 + (text[position] - '0');
            }
            ++position;
        }
        out.numbers.push_back(value);
        if (position + 1 < text.size() && text[position] == '.' &&
            std::isdigit(static_cast<unsigned char>(text[position + 1]))) {
            ++position;
        } else {
            break;
        }
    }
    if (!out.numbers.empty() && position < text.size() && text[position] == '-') {
        const size_t end = text.find_first_of("+ \t\r\n", position + 1);
        out.label = text.substr(position + 1, end == std::string::npos
                                                  ? std::string::npos
                                                  : end - position - 1);
    }
    return out;
}

std::vector<long> versionParts(const std::string &text)
{
    return parseVersion(text).numbers;
}

std::vector<std::string> labelParts(const std::string &label)
{
    std::vector<std::string> parts;
    size_t start = 0;
    while (true) {
        const size_t dot = label.find('.', start);
        parts.push_back(label.substr(start, dot == std::string::npos
                                                ? std::string::npos
                                                : dot - start));
        if (dot == std::string::npos) {
            return parts;
        }
        start = dot + 1;
    }
}

bool isNumber(const std::string &part)
{
    return !part.empty() && part.find_first_not_of("0123456789") == std::string::npos;
}

// SemVer's order for pre-release labels: part by part, numbers by value and
// below words, words by their characters, and a shorter label first when
// one runs out.
int compareLabels(const std::string &a, const std::string &b)
{
    const std::vector<std::string> left = labelParts(a);
    const std::vector<std::string> right = labelParts(b);
    for (size_t index = 0; index < left.size() && index < right.size(); ++index) {
        const std::string &x = left[index];
        const std::string &y = right[index];
        int order = 0;
        if (isNumber(x) && isNumber(y)) {
            const std::string xs = x.substr(std::min(x.find_first_not_of('0'), x.size() - 1));
            const std::string ys = y.substr(std::min(y.find_first_not_of('0'), y.size() - 1));
            order = xs.size() != ys.size() ? (xs.size() < ys.size() ? -1 : 1)
                                           : xs.compare(ys);
        } else if (isNumber(x) != isNumber(y)) {
            order = isNumber(x) ? -1 : 1;
        } else {
            order = x.compare(y);
        }
        if (order != 0) {
            return order < 0 ? -1 : 1;
        }
    }
    return left.size() == right.size() ? 0 : (left.size() < right.size() ? -1 : 1);
}

std::string trim(const std::string &text)
{
    size_t start = 0;
    size_t end = text.size();
    while (start < end && std::isspace(static_cast<unsigned char>(text[start]))) {
        ++start;
    }
    while (end > start && std::isspace(static_cast<unsigned char>(text[end - 1]))) {
        --end;
    }
    return text.substr(start, end - start);
}

std::string lower(std::string text)
{
    for (char &c : text) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return text;
}

// Inline Markdown to plain text: images go, links keep their words, and
// emphasis and code marks are dropped.
// bold, when given, gets where **bold** and __bold__ ran in what is
// returned, as [start, end) offsets.
std::string plainInline(const std::string &text, Spans *bold = nullptr)
{
    std::string out;
    size_t index = 0;
    size_t boldFrom = std::string::npos; // where an open bold run started
    while (index < text.size()) {
        const char c = text[index];
        if (c == '<') {
            const size_t close = text.find('>', index);
            if (close != std::string::npos) {
                index = close + 1;
                continue;
            }
        }
        const bool image = c == '!' && index + 1 < text.size() &&
                           text[index + 1] == '[';
        if (c == '[' || image) {
            const size_t open = image ? index + 1 : index;
            const size_t close = text.find(']', open);
            if (close != std::string::npos && close + 1 < text.size() &&
                text[close + 1] == '(') {
                const size_t end = text.find(')', close);
                if (end != std::string::npos) {
                    if (!image) {
                        Spans inner;
                        const size_t at = out.size();
                        out += plainInline(text.substr(open + 1, close - open - 1),
                                           bold != nullptr ? &inner : nullptr);
                        for (const auto &span : inner) {
                            bold->push_back({at + span.first, at + span.second});
                        }
                    }
                    index = end + 1;
                    continue;
                }
            }
        }
        const bool doubled = index + 1 < text.size() && text[index + 1] == c;
        if ((c == '*' || c == '_') && doubled) {
            if (boldFrom == std::string::npos) {
                boldFrom = out.size();
            } else {
                if (bold != nullptr && out.size() > boldFrom) {
                    bold->push_back({boldFrom, out.size()});
                }
                boldFrom = std::string::npos;
            }
            index += 2;
            continue;
        }
        if (c == '*' || c == '`') {
            ++index;
            continue;
        }
        out += c;
        ++index;
    }
    return out;
}

} // namespace

bool parseRelease(const std::string &text, Release &out, std::string &error)
{
    out = Release();
    json::Value root;
    if (!json::parse(text, root, error)) {
        error = "The release information could not be read. " + error;
        return false;
    }
    if (root.type != json::Type::Object) {
        error = "The release information was not a release.";
        return false;
    }
    out.tag = root["tag_name"].text();
    if (versionParts(out.tag).empty()) {
        error = out.tag.empty() ? "The release has no version."
                                : "The release version \"" + out.tag +
                                      "\" is not a version number.";
        return false;
    }
    out.version = out.tag;
    if (!out.version.empty() && (out.version[0] == 'v' || out.version[0] == 'V')) {
        out.version.erase(0, 1);
    }
    out.name = root["name"].text(out.tag);
    out.notes = root["body"].text();
    out.page = root["html_url"].text();
    const std::string published = root["published_at"].text();
    if (published.size() >= 10) {
        out.published = published.substr(0, 10);
    }
    for (const json::Value &item : root["assets"].items) {
        Asset asset;
        asset.name = item["name"].text();
        asset.url = item["browser_download_url"].text();
        const int64_t size = item["size"].integer(0);
        asset.size = size > 0 ? static_cast<uint64_t>(size) : 0;
        if (!asset.name.empty() && !asset.url.empty()) {
            out.assets.push_back(asset);
        }
    }
    return true;
}

int compareVersions(const std::string &a, const std::string &b)
{
    const Version first = parseVersion(a);
    const Version second = parseVersion(b);
    std::vector<long> left = first.numbers;
    std::vector<long> right = second.numbers;
    const size_t length = std::max(left.size(), right.size());
    left.resize(length, 0);
    right.resize(length, 0);
    for (size_t index = 0; index < length; ++index) {
        if (left[index] != right[index]) {
            return left[index] < right[index] ? -1 : 1;
        }
    }
    // The same numbers: a pre-release comes before its release.
    if (first.label.empty() || second.label.empty()) {
        return first.label.empty() == second.label.empty()
                   ? 0
                   : (first.label.empty() ? 1 : -1);
    }
    return compareLabels(first.label, second.label);
}

bool isNewer(const std::string &candidate, const std::string &current)
{
    return compareVersions(candidate, current) > 0;
}

const Asset *findAsset(const Release &release, const std::string &name)
{
    for (const Asset &asset : release.assets) {
        if (asset.name == name) {
            return &asset;
        }
    }
    return nullptr;
}

std::string checksumFor(const std::string &listing, const std::string &name)
{
    size_t start = 0;
    while (start < listing.size()) {
        size_t end = listing.find('\n', start);
        if (end == std::string::npos) {
            end = listing.size();
        }
        const std::string line = trim(listing.substr(start, end - start));
        start = end + 1;
        const size_t space = line.find_first_of(" \t");
        if (space != 64) {
            continue;
        }
        std::string file = trim(line.substr(space));
        if (!file.empty() && file[0] == '*') {
            file.erase(0, 1);
        }
        if (file != name) {
            continue;
        }
        const std::string hash = lower(line.substr(0, 64));
        if (hash.find_first_not_of("0123456789abcdef") == std::string::npos) {
            return hash;
        }
    }
    return "";
}

std::vector<NoteLine> noteText(const std::string &markdown)
{
    std::vector<std::string> raw;
    size_t start = 0;
    while (start <= markdown.size()) {
        size_t end = markdown.find('\n', start);
        if (end == std::string::npos) {
            end = markdown.size();
        }
        std::string line = markdown.substr(start, end - start);
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        raw.push_back(line);
        start = end + 1;
    }

    // Skip the install instructions above "What's new" when it is there.
    size_t first = 0;
    for (size_t index = 0; index < raw.size(); ++index) {
        const std::string line = lower(trim(raw[index]));
        if (line.rfind("#", 0) == 0 &&
            line.find("what's new") != std::string::npos) {
            first = index + 1;
            break;
        }
    }

    std::vector<NoteLine> lines;
    bool inComment = false;
    for (size_t index = first; index < raw.size(); ++index) {
        std::string line = raw[index];
        if (inComment) {
            if (line.find("-->") != std::string::npos) {
                inComment = false;
            }
            continue;
        }
        if (trim(line).rfind("<!--", 0) == 0) {
            inComment = line.find("-->") == std::string::npos;
            continue;
        }
        size_t indent = 0;
        while (indent < line.size() && (line[indent] == ' ' || line[indent] == '\t')) {
            ++indent;
        }
        std::string body = trim(line);
        std::string prefix;
        const bool heading = body.rfind("#", 0) == 0;
        if (heading) {
            body = trim(body.substr(body.find_first_not_of('#')));
        } else if (body.rfind("- ", 0) == 0 || body.rfind("* ", 0) == 0 ||
                   body.rfind("+ ", 0) == 0) {
            prefix = std::string((indent / 2) * 2, ' ') + "- ";
            body = trim(body.substr(2));
        } else if (body == "---" || body == "***") {
            body.clear();
        }
        Spans bold;
        const std::string plain = plainInline(body, &bold);
        const size_t lead = std::min(plain.size(), plain.find_first_not_of(" \t"));
        body = trim(plain);
        // GitHub's generated link to the full comparison means nothing here.
        if (lower(body).rfind("full changelog", 0) == 0) {
            continue;
        }
        if (body.empty()) {
            if (!lines.empty() && !lines.back().text.empty()) {
                lines.push_back({});
            }
            continue;
        }
        NoteLine note;
        note.text = prefix + body;
        for (const auto &span : bold) {
            const size_t start = std::max(span.first, lead) - lead;
            const size_t end = std::min(span.second - lead, body.size());
            if (span.second > lead && start < end) {
                note.bold.push_back({prefix.size() + start, prefix.size() + end});
            }
        }
        // A line that is bold all through stands for a heading too.
        note.heading = heading ||
                       (prefix.empty() && note.bold.size() == 1 &&
                        note.bold[0].first == 0 && note.bold[0].second == body.size());
        if (note.heading) {
            note.bold.clear();
        }
        lines.push_back(note);
    }
    while (!lines.empty() && lines.back().text.empty()) {
        lines.pop_back();
    }
    return lines;
}

std::vector<std::string> noteLines(const std::string &markdown)
{
    std::vector<std::string> lines;
    for (const NoteLine &note : noteText(markdown)) {
        lines.push_back(note.text);
    }
    return lines;
}

} // namespace update
