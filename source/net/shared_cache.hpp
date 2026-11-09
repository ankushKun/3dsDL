#pragma once

#include <curl/curl.h>

namespace net {

// One cache of DNS answers and TLS sessions for every request the process
// makes, from any thread, so a host reached once is quicker to reach again:
// a download after Receive's look at its link, a retry, the next file from
// the same site. Set it as every handle's CURLOPT_SHARE. Connections are not
// shared, since libcurl cannot share them between threads. It lives as long
// as the process, and is nullptr only when libcurl could not make it.
CURLSH *sharedCache();

} // namespace net
