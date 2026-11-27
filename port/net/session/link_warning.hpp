// port/net (port/docs/netcode.md, "Playing"): when a client warns its player of a bad connection, from what it measures:
// snapshots late or lost, the ping far above its usual, a catch-up or jump. Plain data in, a level out, so it is tested on
// its own (port/net/tests/test_link_warning.cpp).
#pragma once
#include <deque>
#include <stdint.h>

namespace net
{
enum LinkLevel
{
    kLinkGood = 0,
    kLinkLag = 1, // lag: the game hitches or corrects
    kLinkLost = 2, // no snapshots: the game stands still or runs blind
};

class LinkWarning
{
  public:
    static const int kGapMs = 250; // no snapshot this long: lag (8 ticks)
    static const int kStallMs = 700; // and this long: lost (Client's kStallMs)
    static const int kOnsetMs = 500; // loss or a high ping held this long before it shows
    static const int kHoldMs = 2000; // a level shown stays this long after its cause is gone
    static const int kWindowMs = 1000; // the loss is measured over this
    static const int kLossPercent = 20;

    void Reset(); // a new connection or game: no history
    void OnSnapshot(long long nowMs, uint32_t tick); // a snapshot of that tick taken (older ones are dropped before)
    void OnCatchUp(long long nowMs); // ticks run at once, or a jump: a visible hitch, unless our own frame stalled
    // Each frame: whether we are <connected> and <synced> (snapshots are expected), the smoothed ping (0: none yet)
    LinkLevel Update(long long nowMs, bool connected, bool synced, uint32_t pingMs);
    uint32_t Baseline() const { return mBaseline; }

  private:
    struct Arrival
    {
        long long ms;
        uint32_t lost; // ticks skipped before this one
    };
    std::deque<Arrival> mArrivals; // the last kWindowMs'
    uint32_t mLastTick = 0;
    long long mLastSnapshotMs = 0;
    long long mCatchUpMs = -1; // the last OnCatchUp's, until Update weighs it
    long long mLastUpdateMs = 0, mOwnCatchUpsUntilMs = 0;
    long long mSlowSinceMs = -1; // loss or a high ping since then (-1: not now)
    long long mLagUntilMs = 0;
    uint32_t mBaseline = 0; // the usual ping: follows a drop at once, a rise slowly
    long long mBaselineMs = 0;
    bool mWasSynced = false;
};
} // namespace net
