#include "../check.hpp"

#include "awake_policy.hpp"

TEST("awake: sleep waits for the work and the linger after it")
{
    awake::Policy policy;
    CHECK(policy.sleepAllowed(0));
    CHECK(!policy.held());

    policy.hold();
    CHECK(policy.held());
    CHECK(!policy.sleepAllowed(1000));
    policy.release(5000);
    CHECK(!policy.held());
    CHECK(!policy.sleepAllowed(5000));
    CHECK(!policy.sleepAllowed(5000 + awake::LingerMs - 1));
    CHECK(policy.sleepAllowed(5000 + awake::LingerMs));
}

TEST("awake: holds overlap, and new work ends the linger")
{
    awake::Policy policy;
    policy.hold();
    policy.hold(); // a download inside Receive
    policy.release(100);
    CHECK(!policy.sleepAllowed(100 + awake::LingerMs));
    policy.release(200);
    CHECK(!policy.sleepAllowed(200 + awake::LingerMs - 1));

    // The next file of the batch arrives before the linger is out.
    policy.hold();
    CHECK(!policy.sleepAllowed(200 + awake::LingerMs * 3));
    policy.release(200 + awake::LingerMs * 3);
    CHECK(!policy.sleepAllowed(200 + awake::LingerMs * 3 + 1));
    CHECK(policy.sleepAllowed(200 + awake::LingerMs * 4));

    // A release without a hold changes nothing.
    policy.release(0);
    CHECK(policy.sleepAllowed(200 + awake::LingerMs * 4));
}
