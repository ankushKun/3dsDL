#include "tls_clock.hpp"

#ifdef __3DS__
#include "timezone.hpp"

#include <mbedtls/ssl.h>
#include <mbedtls/x509_crt.h>

#include <ctime>

// From 8.10 curl installs a verify callback of its own, which this one would
// replace. Chain to it before moving to a newer devkitPro curl.
static_assert(LIBCURL_VERSION_NUM < 0x080a00,
              "chain to curl's mbedTLS verify callback");

namespace net {

namespace {

int64_t unixSeconds(const mbedtls_x509_time &time)
{
    return tz::unixSeconds(time.year, time.mon, time.day, time.hour,
                           time.min, time.sec);
}

// Called for each certificate in the chain once mbedTLS has checked it, with
// what it found wrong. mbedTLS took the clock for UTC, as time() returns it.
int verify(void *, mbedtls_x509_crt *certificate, int, uint32_t *flags)
{
    constexpr uint32_t Dates =
        MBEDTLS_X509_BADCERT_FUTURE | MBEDTLS_X509_BADCERT_EXPIRED;
    if ((*flags & Dates) != 0 &&
        tz::coversConsoleTime(unixSeconds(certificate->valid_from),
                              unixSeconds(certificate->valid_to),
                              std::time(nullptr))) {
        *flags &= ~Dates;
    }
    return 0;
}

// curl hands over the connection's mbedTLS configuration before the
// handshake.
CURLcode configure(CURL *, void *config, void *)
{
    mbedtls_ssl_conf_verify(static_cast<mbedtls_ssl_config *>(config), verify,
                            nullptr);
    return CURLE_OK;
}

} // namespace

void useLocalClock(CURL *curl)
{
    curl_easy_setopt(curl, CURLOPT_SSL_CTX_FUNCTION, configure);
}

} // namespace net

#else

namespace net {

void useLocalClock(CURL *) {}

} // namespace net

#endif
