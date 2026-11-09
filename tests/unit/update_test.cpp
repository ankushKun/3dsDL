#include "../check.hpp"

#include "json.hpp"
#include "sha256.hpp"
#include "update.hpp"

#include <string>
#include <vector>

namespace {

// Trimmed from a real answer of GitHub's releases/latest API.
const char *const ReleaseJson = R"({
  "url": "https://api.github.com/repos/ankushKun/3dsDL/releases/1",
  "html_url": "https://github.com/ankushKun/3dsDL/releases/tag/v0.6.1",
  "tag_name": "v0.6.1",
  "name": "3dsDL v0.6.1",
  "draft": false,
  "prerelease": false,
  "published_at": "2026-10-20T08:15:00Z",
  "assets": [
    {"name": "3dsDL.3dsx", "size": 1953112,
     "browser_download_url": "https://github.com/ankushKun/3dsDL/releases/download/v0.6.1/3dsDL.3dsx"},
    {"name": "3dsDL.cia", "size": 2051520,
     "browser_download_url": "https://github.com/ankushKun/3dsDL/releases/download/v0.6.1/3dsDL.cia"},
    {"name": "3dsDL.sha256", "size": 150,
     "browser_download_url": "https://github.com/ankushKun/3dsDL/releases/download/v0.6.1/3dsDL.sha256"}
  ],
  "body": "<p align=\"center\"><img src=\"x.jpg\"></p>\r\n\r\n**Install:** scan it.\r\n\r\n## What's new\r\n\r\n### Updates — built in\r\n\r\n- A **notification** when a [new version](https://example.com) is out.\r\n  - Nested `item`.\r\n\r\n\r\n\r\n**Full Changelog**: https://github.com/ankushKun/3dsDL/compare/v0.6.0...v0.6.1\r\n"
})";

} // namespace

TEST("json: reads objects, arrays, numbers and escapes")
{
    json::Value value;
    std::string error;
    CHECK(json::parse(R"( {"a": [1, -2.5e1, true, null], "b": "x\"é😀\n"} )",
                      value, error));
    CHECK_EQ(value["a"].items.size(), static_cast<size_t>(4));
    CHECK_EQ(value["a"].items[0].integer(), static_cast<int64_t>(1));
    CHECK_EQ(value["a"].items[1].number, -25.0);
    CHECK(value["a"].items[2].boolean);
    CHECK(value["a"].items[3].isNull());
    CHECK_EQ(value["b"].text(), std::string("x\"\xC3\xA9\xF0\x9F\x98\x80\n"));
    CHECK(value["missing"].isNull());
    CHECK_EQ(value["missing"]["deeper"].text("none"), std::string("none"));
}

TEST("json: rejects broken documents")
{
    json::Value value;
    std::string error;
    for (const char *text : {"", "{", "[1,]", "{\"a\" 1}", "\"open", "tru",
                             "[1] 2", "{\"a\":01x}", "\"tab\there\""}) {
        error.clear();
        CHECK(!json::parse(text, value, error));
        CHECK(!error.empty());
    }
    std::string deep(100, '[');
    deep += std::string(100, ']');
    CHECK(!json::parse(deep, value, error));
    CHECK(json::parse(deep, value, error, 200));
}

TEST("sha256: matches the standard test vectors")
{
    CHECK_EQ(sha256::ofString(""),
             std::string("e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"));
    CHECK_EQ(sha256::ofString("abc"),
             std::string("ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"));
    CHECK_EQ(sha256::ofString("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq"),
             std::string("248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1"));
    // A million 'a's, fed in uneven pieces.
    sha256::Hasher hasher;
    const std::string piece(997, 'a');
    size_t left = 1000000;
    while (left > 0) {
        const size_t take = std::min(left, piece.size());
        hasher.update(piece.data(), take);
        left -= take;
    }
    CHECK_EQ(hasher.finishHex(),
             std::string("cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0"));
    CHECK_EQ(sha256::ofFile("/no/such/file"), std::string(""));
}

TEST("update: reads a GitHub release")
{
    update::Release release;
    std::string error;
    CHECK(update::parseRelease(ReleaseJson, release, error));
    CHECK_EQ(release.tag, std::string("v0.6.1"));
    CHECK_EQ(release.version, std::string("0.6.1"));
    CHECK_EQ(release.name, std::string("3dsDL v0.6.1"));
    CHECK_EQ(release.published, std::string("2026-10-20"));
    CHECK_EQ(release.assets.size(), static_cast<size_t>(3));
    const update::Asset *cia = update::findAsset(release, update::CiaAsset);
    CHECK(cia != nullptr);
    if (cia != nullptr) {
        CHECK_EQ(cia->size, static_cast<uint64_t>(2051520));
        CHECK(cia->url.find("/v0.6.1/3dsDL.cia") != std::string::npos);
    }
    CHECK(update::findAsset(release, "other.zip") == nullptr);

    CHECK(!update::parseRelease("[]", release, error));
    CHECK(!update::parseRelease(R"({"tag_name": "nightly"})", release, error));
    CHECK(error.find("nightly") != std::string::npos);
    CHECK(!update::parseRelease("{\"message\": \"Not Found\"}", release, error));
}

TEST("update: compares versions number by number")
{
    CHECK(update::isNewer("v0.6.1", "0.6.0"));
    CHECK(update::isNewer("0.10.0", "0.9.9"));
    CHECK(update::isNewer("1.0", "0.99.99"));
    CHECK(!update::isNewer("0.6.0", "0.6.0"));
    CHECK(!update::isNewer("v0.5.9", "0.6.0"));
    CHECK_EQ(update::compareVersions("1.0", "1.0.0"), 0);
    CHECK_EQ(update::compareVersions("0.7.0-beta", "0.7.0"), -1);
    CHECK_EQ(update::compareVersions("garbage", "0.0.0"), 0);
    CHECK_EQ(update::compareVersions("0.6.0", "0.6.2"), -1);
}

TEST("update: finds a file's checksum in a sha256sum listing")
{
    const std::string listing =
        "1f2E3d4c5b6a79881f2e3d4c5b6a79881f2e3d4c5b6a79881f2e3d4c5b6a7988  3dsDL.3dsx\n"
        "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa *3dsDL.cia\r\n"
        "not a hash  3dsDL.smdh\n";
    CHECK_EQ(update::checksumFor(listing, "3dsDL.3dsx"),
             std::string("1f2e3d4c5b6a79881f2e3d4c5b6a79881f2e3d4c5b6a79881f2e3d4c5b6a7988"));
    CHECK_EQ(update::checksumFor(listing, "3dsDL.cia"), std::string(64, 'a'));
    CHECK_EQ(update::checksumFor(listing, "3dsDL.smdh"), std::string(""));
    CHECK_EQ(update::checksumFor(listing, "3dsDL"), std::string(""));
}

TEST("update: turns release notes into plain lines")
{
    update::Release release;
    std::string error;
    CHECK(update::parseRelease(ReleaseJson, release, error));
    const std::vector<std::string> lines = update::noteLines(release.notes);
    const std::vector<std::string> expected = {
        "Updates \xE2\x80\x94 built in",
        "",
        "- A notification when a new version is out.",
        "  - Nested item.",
    };
    CHECK_EQ(lines.size(), expected.size());
    for (size_t index = 0; index < std::min(lines.size(), expected.size()); ++index) {
        CHECK_EQ(lines[index], expected[index]);
    }
    // Without a "What's new" heading everything is kept.
    const std::vector<std::string> plain = update::noteLines("Fixes.\n\n\n- One");
    CHECK_EQ(plain.size(), static_cast<size_t>(3));
}

TEST("update: release notes keep their headings and where bold ran")
{
    const std::vector<update::NoteLine> notes = update::noteText(
        "## What's new\n"
        "### Files\n"
        "- **Favourites:** R stars a folder, and **L** opens them.\n"
        "  - A [**linked**](https://example.com) word and __this__.\n"
        "A paragraph, then a list:\n"
        "**Bold all through**\n"
        "- Unclosed **bold\n"
        "- snake_case and *italic* stay plain\n");
    CHECK_EQ(notes.size(), static_cast<size_t>(7));
    CHECK_EQ(notes[0].text, std::string("Files"));
    CHECK(notes[0].heading);
    CHECK(notes[0].bold.empty());

    CHECK_EQ(notes[1].text,
             std::string("- Favourites: R stars a folder, and L opens them."));
    CHECK(!notes[1].heading);
    CHECK_EQ(notes[1].bold.size(), static_cast<size_t>(2));
    CHECK_EQ(notes[1].text.substr(notes[1].bold[0].first,
                                  notes[1].bold[0].second - notes[1].bold[0].first),
             std::string("Favourites:"));
    CHECK_EQ(notes[1].text.substr(notes[1].bold[1].first,
                                  notes[1].bold[1].second - notes[1].bold[1].first),
             std::string("L"));

    // Nested items keep their indent, and bold inside a link counts.
    CHECK_EQ(notes[2].text, std::string("  - A linked word and this."));
    CHECK_EQ(notes[2].bold.size(), static_cast<size_t>(2));
    CHECK_EQ(notes[2].text.substr(notes[2].bold[0].first, 6), std::string("linked"));
    CHECK_EQ(notes[2].text.substr(notes[2].bold[1].first, 4), std::string("this"));

    // Only a # heading or a line bold all through is a heading, not a
    // paragraph that comes before a list.
    CHECK(!notes[3].heading);
    CHECK_EQ(notes[4].text, std::string("Bold all through"));
    CHECK(notes[4].heading);
    CHECK(notes[4].bold.empty());

    // An unclosed run marks nothing.
    CHECK_EQ(notes[5].text, std::string("- Unclosed bold"));
    CHECK(notes[5].bold.empty());
    CHECK_EQ(notes[6].text, std::string("- snake_case and italic stay plain"));
    CHECK(notes[6].bold.empty());
}

TEST("update: pre-releases come before their release, in SemVer order")
{
    // A tester on a beta is offered the release, and nothing older.
    CHECK(update::isNewer("v0.8.0", "0.8.0-beta.3"));
    CHECK(!update::isNewer("v0.8.0-beta.3", "0.8.0"));
    CHECK(update::isNewer("0.8.0-beta.1", "0.7.13"));
    CHECK(!update::isNewer("0.7.2", "0.8.0-beta.1"));
    const char *order[] = {"0.8.0-alpha", "0.8.0-alpha.1", "0.8.0-alpha.beta",
                           "0.8.0-beta", "0.8.0-beta.2", "0.8.0-beta.10",
                           "0.8.0-rc.1", "0.8.0"};
    for (size_t index = 0; index + 1 < sizeof(order) / sizeof(order[0]); ++index) {
        CHECK_EQ(update::compareVersions(order[index], order[index + 1]), -1);
        CHECK_EQ(update::compareVersions(order[index + 1], order[index]), 1);
    }
    CHECK_EQ(update::compareVersions("0.8.0-beta.1", "v0.8.0-beta.1"), 0);
    CHECK_EQ(update::compareVersions("0.8.0+abc", "0.8.0"), 0);
    CHECK_EQ(update::compareVersions("0.8.0-rc.1+abc", "0.8.0-rc.1"), 0);
    CHECK_EQ(update::compareVersions("0.8.0-beta.01", "0.8.0-beta.1"), 0);

    update::Release release;
    std::string error;
    CHECK(update::parseRelease(R"({"tag_name":"v0.8.0-beta.2","name":"3dsDL v0.8.0-beta.2","assets":[]})",
                               release, error));
    CHECK_EQ(release.version, std::string("0.8.0-beta.2"));
}
