#include "tls.hpp"

#ifdef __3DS__
#include "timezone.hpp"

#include <mbedtls/ssl.h>
#include <mbedtls/x509_crt.h>

#include <algorithm>
#include <ctime>
#include <mutex>
#include <vector>

// From 8.10 curl installs a verify callback of its own, which this one would
// replace. Chain to it before moving to a newer devkitPro curl.
static_assert(LIBCURL_VERSION_NUM < 0x080a00,
              "chain to curl's mbedTLS verify callback");

namespace net {

// A parsed CA bundle.
struct Authorities {
    std::string file;
    mbedtls_x509_crt chain;
    bool parsed = false;
    bool taken = false;
};

namespace {

// Parsed bundles kept between requests. More are only parsed while more
// requests run at once, and those beyond this many are freed as they end.
constexpr size_t Kept = 2;

std::mutex poolLock;
std::vector<Authorities *> pool;

Authorities *take(const std::string &file)
{
    {
        std::lock_guard<std::mutex> guard(poolLock);
        for (Authorities *authorities : pool) {
            if (!authorities->taken && authorities->file == file) {
                authorities->taken = true;
                return authorities;
            }
        }
    }
    // Parsed outside the lock, so the others need not wait for it.
    Authorities *authorities = new Authorities;
    authorities->file = file;
    authorities->taken = true;
    mbedtls_x509_crt_init(&authorities->chain);
    // As curl does, a bundle with some certificates mbedTLS cannot read
    // still trusts the rest.
    authorities->parsed =
        mbedtls_x509_crt_parse_file(&authorities->chain, file.c_str()) >= 0;
    std::lock_guard<std::mutex> guard(poolLock);
    pool.push_back(authorities);
    return authorities;
}

void giveBack(Authorities *authorities)
{
    {
        std::lock_guard<std::mutex> guard(poolLock);
        if (authorities->parsed && pool.size() <= Kept) {
            authorities->taken = false;
            return;
        }
        pool.erase(std::find(pool.begin(), pool.end(), authorities));
    }
    mbedtls_x509_crt_free(&authorities->chain);
    delete authorities;
}

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

} // namespace

Trust::Trust(const std::string &caFile) : caFile_(caFile) {}

Trust::~Trust()
{
    if (authorities_ != nullptr) {
        giveBack(authorities_);
    }
}

void Trust::use(CURL *curl) const
{
    if (!caFile_.empty()) {
        // Otherwise curl parses a bundle for every connection too, its
        // default sdmc:/config/ssl/cacert.pem when none is named.
        curl_easy_setopt(curl, CURLOPT_CAINFO, nullptr);
        curl_easy_setopt(curl, CURLOPT_CAPATH, nullptr);
    }
    curl_easy_setopt(curl, CURLOPT_SSL_CTX_FUNCTION, configure);
    curl_easy_setopt(curl, CURLOPT_SSL_CTX_DATA, const_cast<Trust *>(this));
}

// curl hands over the connection's mbedTLS configuration last, just before
// the handshake, after setting a CA chain of its own, which this replaces.
CURLcode Trust::configure(CURL *, void *config, void *data)
{
    mbedtls_ssl_config *ssl = static_cast<mbedtls_ssl_config *>(config);
    const Trust &trust = *static_cast<const Trust *>(data);
    if (!trust.caFile_.empty()) {
        if (trust.authorities_ == nullptr) {
            trust.authorities_ = take(trust.caFile_);
        }
        if (!trust.authorities_->parsed) {
            return CURLE_SSL_CACERT_BADFILE;
        }
        mbedtls_ssl_conf_ca_chain(ssl, &trust.authorities_->chain, nullptr);
    }
    mbedtls_ssl_conf_verify(ssl, verify, nullptr);
    return CURLE_OK;
}

} // namespace net

#else

namespace net {

Trust::Trust(const std::string &caFile) : caFile_(caFile) {}

Trust::~Trust()
{
    (void)authorities_;
}

void Trust::use(CURL *curl) const
{
    if (!caFile_.empty()) {
        curl_easy_setopt(curl, CURLOPT_CAINFO, caFile_.c_str());
    }
}

} // namespace net

#endif
