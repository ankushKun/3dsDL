#pragma once

#include <curl/curl.h>

// The console clock holds local time and mbedTLS reads it as UTC, so west of
// UTC a certificate issued in the last few hours looks not valid yet, and
// east of it one that expires in the next few hours looks expired already.
namespace net {

// Makes the handle accept a certificate that is valid at some moment the
// console clock could stand for in one of the world's time zones
// (tz::coversConsoleTime). Call it on every handle that verifies
// certificates. On the host, whose clock knows its zone, it does nothing.
void useLocalClock(CURL *curl);

} // namespace net
