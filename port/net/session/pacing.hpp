// port/net (port/docs/netcode.md, "The server"): a client's tick pacing. It runs its ticks at the server's measured rate,
// a few percent slower or faster in proportion to how far its commands are from their target lead, and ramps every
// change. Plain data in, a step length out, so it is tested on its own (port/net/tests/test_pacing.cpp).
#pragma once
#include <stdint.h>

namespace net
{
class Pacer
{
  public:
    static constexpr double kTargetSlack = 1.5; // ticks early our commands should reach the server, on average
    static constexpr double kBand = 0.5; // within this of the target, the server's rate alone: the slack's own jitter
    static constexpr double kEarlyGain = 0.05; // slower by this per tick of the slack above that band
    static constexpr double kLateGain = 0.15; // faster by this per tick below it: a late command loses an input
    static constexpr double kMostSlow = 0.2; // at most this much slower than the server
    static constexpr double kMostFast = 0.25; // and this much faster
    static constexpr double kRamp = 0.01; // the step length changes by at most this part of a tick per tick
    static const int kBucketTicks = 15; // the server's clock: each bucket's least delayed snapshot
    static const int kBuckets = 7; // about 3.5 s of them

    void Reset(int tickUs); // a new connection: the welcome's tick length, no history
    void OnSnapshot(long long nowUs, uint32_t tick); // the newest snapshot so far came, at our game clock's <nowUs>
    void OnSlack(int slack); // its word on our commands, once it has heard of our ticks since a jump
    int NextUs(bool slackKnown); // each tick: the time to the next one (slackKnown: no jump since the last OnSlack)

    double ServerTickUs() const; // the server's tick length on our clock (its nominal one until measured)
    double SlackAverage() const { return mSlackAvg; }
    bool SlackKnown() const { return mSlackKnown; }

  private:
    struct Bucket
    {
        uint32_t tick;
        double offsetUs; // arrival less tick * the nominal tick length: the least of the bucket's
    };
    int mTickUs = 33000;
    double mPaceUs = 33000;
    Bucket mBuckets[kBuckets] = {};
    int mBucketCount = 0; // filled, the newest at mBucketCount - 1 (the one still filling)
    uint32_t mLastTick = 0;
    double mServerUs = 0; // the last estimate (0: none)
    double mSlackAvg = 0;
    bool mSlackKnown = false;
};
} // namespace net
