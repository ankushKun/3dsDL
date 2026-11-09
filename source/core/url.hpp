#pragma once

#include <string>

namespace url {

// True for an http:// or https:// URL with a host and no whitespace or
// control characters. The scheme is matched case-insensitively because QR
// codes in alphanumeric mode can only carry upper-case letters.
bool isHttpUrl(const std::string &value);
bool isHttps(const std::string &value);

// Lower-case host name without user info or port, or "" when there is none.
std::string host(const std::string &value);

// The site a host (as host gives it) belongs to, to tell its servers from
// another site's: the last two labels ("dl3.example.com" is "example.com"),
// or three under a two-letter country code's common second level
// ("example.co.uk"). An address is its own site.
std::string site(const std::string &host);

// The download of a file shared from Google Drive, given any of its share,
// view or download links, with Drive's question about files too large to
// scan for viruses already answered. "" for any other link, including Drive
// folders and Google Docs. The result is its own answer.
std::string driveDownload(const std::string &value);

// Last path segment, still percent-encoded, without query or fragment.
std::string lastPathSegment(const std::string &value);

// Decodes %XX escapes. Malformed escapes are kept as they are.
std::string percentDecode(const std::string &value);
// Escapes everything but letters, digits and -._~ for a query value.
std::string percentEncode(const std::string &value);

} // namespace url
