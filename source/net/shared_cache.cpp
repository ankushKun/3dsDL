#include "shared_cache.hpp"

#include <mutex>

namespace net {
namespace {

std::mutex locks[CURL_LOCK_DATA_LAST];
std::mutex making;
CURLSH *cache = nullptr;

void lock(CURL *, curl_lock_data data, curl_lock_access, void *)
{
    locks[data].lock();
}

void unlock(CURL *, curl_lock_data data, void *)
{
    locks[data].unlock();
}

} // namespace

CURLSH *sharedCache()
{
    std::lock_guard<std::mutex> guard(making);
    if (cache == nullptr) {
        cache = curl_share_init();
        if (cache != nullptr) {
            curl_share_setopt(cache, CURLSHOPT_LOCKFUNC, lock);
            curl_share_setopt(cache, CURLSHOPT_UNLOCKFUNC, unlock);
            curl_share_setopt(cache, CURLSHOPT_SHARE, CURL_LOCK_DATA_DNS);
            curl_share_setopt(cache, CURLSHOPT_SHARE,
                              CURL_LOCK_DATA_SSL_SESSION);
        }
    }
    return cache;
}

} // namespace net
