#pragma once

#include <curl/curl.h>

#include <string>

namespace net {

struct Authorities;

// How a request checks the server's certificate. Make one for a request, or
// one for a whole download, whose connections all run on one thread, and
// call use() on every handle it makes.
//
// On the console the CA bundle is parsed once and kept: curl's mbedTLS
// backend would otherwise parse all of it again for every connection,
// redirects and split connections included. mbedTLS fills caches in a parsed
// bundle the first time it checks a signature with it, without locking, so a
// parsed bundle serves one Trust at a time, and a Trust that finds them all
// taken parses another. Nothing is parsed until the first TLS connection.
//
// The console clock holds local time and mbedTLS reads it as UTC, so west of
// UTC a certificate issued in the last few hours looks not valid yet, and
// east of it one that expires in the next few hours looks expired already. A
// certificate counts as valid at any moment the clock could stand for in one
// of the world's time zones (tz::coversConsoleTime).
//
// On the host, whose clock knows its zone, curl reads caFile itself.
class Trust {
public:
    explicit Trust(const std::string &caFile);
    ~Trust();
    Trust(const Trust &) = delete;
    Trust &operator=(const Trust &) = delete;

    void use(CURL *curl) const;

private:
    static CURLcode configure(CURL *curl, void *config, void *trust);

    std::string caFile_;
    mutable Authorities *authorities_ = nullptr; // console only
};

} // namespace net
