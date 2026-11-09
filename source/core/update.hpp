#pragma once

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

// Finding out whether a newer 3dsDL has been released, from GitHub's
// release API, and checking what gets downloaded. Only conversions live
// here; the app does the networking and installing.
namespace update {

// The newest published release. Drafts and pre-releases never appear here.
constexpr const char *LatestReleaseUrl =
    "https://api.github.com/repos/ankushKun/3dsDL/releases/latest";
// Every release carries these files (see .github/workflows/ci.yml).
constexpr const char *CiaAsset = "3dsDL.cia";
constexpr const char *HomebrewAsset = "3dsDL.3dsx";
constexpr const char *ChecksumAsset = "3dsDL.sha256";

struct Asset {
    std::string name;
    std::string url; // browser_download_url
    uint64_t size = 0;
};

struct Release {
    std::string tag;       // "v0.6.1"
    std::string version;   // "0.6.1"
    std::string name;      // "3dsDL v0.6.1"
    std::string notes;     // the release text, in Markdown
    std::string published; // "2026-10-19", or "" when unknown
    std::string page;      // the release's web page
    std::vector<Asset> assets;
};

// Reads GitHub's answer for one release. Fails with a reason when it is not
// a release or has no usable version.
bool parseRelease(const std::string &json, Release &out, std::string &error);

// Compares versions the way SemVer orders them, ignoring a leading "v" and
// any "+build": number by number first ("0.10.0" is newer than "0.9.2",
// "1.0" equals "1.0.0"), then a pre-release before its release
// ("0.8.0-rc.1" is older than "0.8.0", "0.8.0-beta.10" newer than
// "0.8.0-beta.2"). Returns -1, 0 or 1. Text that is not a version compares
// as 0.0.0.
int compareVersions(const std::string &a, const std::string &b);
bool isNewer(const std::string &candidate, const std::string &current);

const Asset *findAsset(const Release &release, const std::string &name);

// The hash for name in a sha256sum listing ("<hex>  <name>" per line,
// with an optional '*' before binary names), lowercased, or "" when the
// listing does not mention it.
std::string checksumFor(const std::string &listing, const std::string &name);

// [start, end) byte offsets into a line.
using Spans = std::vector<std::pair<size_t, size_t>>;

// A line of release notes for the small screen, with what the Markdown
// marked in it.
struct NoteLine {
    std::string text;     // "" between paragraphs
    bool heading = false; // a # heading, or a line bold all through
    Spans bold;           // where **bold** ran in text, outside headings
};
// The release text as plain lines for the small screen: Markdown marks,
// links, images and HTML are removed, list items start with "- " and keep
// their nesting as indentation, and runs of blank lines become one. Starts
// at the "What's new" heading when there is one.
std::vector<NoteLine> noteText(const std::string &markdown);
// The same lines' text alone.
std::vector<std::string> noteLines(const std::string &markdown);

} // namespace update
