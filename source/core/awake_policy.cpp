#include "awake_policy.hpp"

namespace awake {

void Policy::hold()
{
    ++holds_;
    lingering_ = false;
}

void Policy::release(uint64_t now)
{
    if (holds_ > 0 && --holds_ == 0) {
        lingering_ = true;
        releasedAt_ = now;
    }
}

bool Policy::sleepAllowed(uint64_t now) const
{
    return holds_ == 0 && (!lingering_ || now - releasedAt_ >= LingerMs);
}

} // namespace awake
