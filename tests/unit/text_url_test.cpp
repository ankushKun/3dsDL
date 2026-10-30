#include "../check.hpp"

#include "text.hpp"
#include "url.hpp"

TEST("text: trim and case helpers")
{
    CHECK_EQ(text::trim("  a b \r\n"), std::string("a b"));
    CHECK_EQ(text::trim(" \t\n"), std::string(""));
    CHECK_EQ(text::lower("HTTPS://X"), std::string("https://x"));
    CHECK(text::startsWithNoCase("HTTP://example.com", "http://"));
    CHECK(!text::startsWithNoCase("htt", "http://"));
    CHECK(text::endsWithNoCase("Archive.ZIP", ".zip"));
}

TEST("text: UTF-8 validation rejects malformed sequences")
{
    CHECK(text::isValidUtf8("plain ascii"));
    CHECK(text::isValidUtf8("na\xc3\xafve \xe2\x82\xac \xf0\x9f\x8e\xae"));
    CHECK(!text::isValidUtf8("\xc3"));             // truncated
    CHECK(!text::isValidUtf8("\xc0\xaf"));         // overlong '/'
    CHECK(!text::isValidUtf8("\xed\xa0\x80"));     // surrogate
    CHECK(!text::isValidUtf8("\xf4\x90\x80\x80")); // past U+10FFFF
    CHECK(!text::isValidUtf8("caf\xe9"));          // ISO-8859-1 byte
    CHECK_EQ(text::sanitizeUtf8("caf\xe9!"), std::string("caf_!"));
}

TEST("text: truncation never splits a character")
{
    const std::string euro = "\xe2\x82\xac"; // 3 bytes
    CHECK_EQ(text::truncateUtf8("ab" + euro, 4), std::string("ab"));
    CHECK_EQ(text::truncateUtf8("ab" + euro, 5), "ab" + euro);
    CHECK_EQ(text::truncateUtf8("abc", 10), std::string("abc"));
    CHECK(text::isValidUtf8(text::truncateUtf8(euro + euro + euro, 7)));
}

TEST("text: latin-1 conversion and single-line fields")
{
    CHECK_EQ(text::latin1ToUtf8("caf\xe9"), std::string("caf\xc3\xa9"));
    CHECK_EQ(text::singleLine("a\tb\nc"), std::string("a b c"));
}

TEST("url: accepts only http and https links")
{
    CHECK(url::isHttpUrl("https://example.com/file.zip"));
    CHECK(url::isHttpUrl("HTTP://EXAMPLE.COM/FILE.ZIP"));
    CHECK(url::isHttpUrl("http://[::1]:8080/x"));
    CHECK(!url::isHttpUrl("ftp://example.com/file"));
    CHECK(!url::isHttpUrl("example.com/file.zip"));
    CHECK(!url::isHttpUrl("https://"));
    CHECK(!url::isHttpUrl("https://exa mple.com/"));
    CHECK(!url::isHttpUrl("https://example.com/\x01"));
    CHECK(!url::isHttpUrl("WIFI:S:home;T:WPA;P:secret;;"));
    CHECK(url::isHttps("HTTPS://x"));
    CHECK(!url::isHttps("http://x"));
}

TEST("url: host extraction")
{
    CHECK_EQ(url::host("https://Example.COM/a"), std::string("example.com"));
    CHECK_EQ(url::host("http://user:pw@host.net:8080/a?b"),
             std::string("host.net"));
    CHECK_EQ(url::host("http://[::1]:8080/"), std::string("[::1]"));
    CHECK_EQ(url::host("https://example.com"), std::string("example.com"));
    CHECK_EQ(url::host("https://example.com?q=1"), std::string("example.com"));
}

TEST("url: last path segment and percent decoding")
{
    CHECK_EQ(url::lastPathSegment("https://x/a/b/c.zip?d=1#e"),
             std::string("c.zip"));
    CHECK_EQ(url::lastPathSegment("https://x/a/"), std::string(""));
    CHECK_EQ(url::lastPathSegment("https://x"), std::string(""));
    CHECK_EQ(url::lastPathSegment("https://x?y=/z.zip"), std::string(""));
    CHECK_EQ(url::percentDecode("a%20b%2Fc"), std::string("a b/c"));
    CHECK_EQ(url::percentDecode("100%"), std::string("100%"));
    CHECK_EQ(url::percentDecode("%zz%4"), std::string("%zz%4"));
    CHECK_EQ(url::percentEncode("a b/c,(d)\n~-._"),
             std::string("a%20b%2Fc%2C%28d%29%0A~-._"));
    CHECK_EQ(url::percentEncode("\xc3\xa9"), std::string("%C3%A9"));
    CHECK_EQ(url::percentDecode(url::percentEncode("x=1&y=%2")),
             std::string("x=1&y=%2"));
}

TEST("url: the site a host belongs to")
{
    CHECK_EQ(url::site("dl3.example.com"), std::string("example.com"));
    CHECK_EQ(url::site("example.com"), std::string("example.com"));
    CHECK_EQ(url::site("a.b.example.co.uk"), std::string("example.co.uk"));
    CHECK_EQ(url::site("files.example.com.au"), std::string("example.com.au"));
    CHECK_EQ(url::site("www.example.de"), std::string("example.de"));
    CHECK_EQ(url::site("192.168.1.2"), std::string("192.168.1.2"));
    CHECK_EQ(url::site("[::1]"), std::string("[::1]"));
    CHECK_EQ(url::site("localhost"), std::string("localhost"));
    CHECK_EQ(url::site(""), std::string(""));
    CHECK(url::site("objects.githubusercontent.com") != url::site("github.com"));
}
