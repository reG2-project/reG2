// port/net: a client's bad connection warning (link_warning.hpp)
#include "link_warning.hpp"

void net::LinkWarning::Reset()
{
    mArrivals.clear();
    mLastTick = 0;
    mSlowSinceMs = -1;
    mCatchUpMs = -1;
    mWasSynced = false;
}

void net::LinkWarning::OnSnapshot(long long nowMs, uint32_t tick)
{
    // Scattered loss: a longer skip is a gap (Update), a new game's or a resync's
    const uint32_t lost = mLastTick && tick > mLastTick && tick - mLastTick <= 4 ? tick - mLastTick - 1 : 0;
    mArrivals.push_back(Arrival{nowMs, lost});
    mLastTick = tick;
    mLastSnapshotMs = nowMs;
}

void net::LinkWarning::OnCatchUp(long long nowMs)
{
    mCatchUpMs = nowMs;
}

net::LinkLevel net::LinkWarning::Update(long long nowMs, bool connected, bool synced, uint32_t pingMs)
{
    while (!mArrivals.empty() && nowMs - mArrivals.front().ms > kWindowMs)
    {
        mArrivals.pop_front();
    }
    // The catch-ups that follow a sync (the lead settling) or a frame of ours that took long are not the connection's
    if (synced && !mWasSynced)
    {
        mLastSnapshotMs = nowMs; // snapshots are expected from now on
        mOwnCatchUpsUntilMs = nowMs + kHoldMs;
    }
    mWasSynced = synced;
    if (mLastUpdateMs && nowMs - mLastUpdateMs > kGapMs)
    {
        mOwnCatchUpsUntilMs = nowMs + kHoldMs / 2;
    }
    mLastUpdateMs = nowMs;
    if (mCatchUpMs >= 0 && nowMs > mOwnCatchUpsUntilMs)
    {
        mLagUntilMs = mCatchUpMs + kHoldMs;
    }
    mCatchUpMs = -1;
    if (pingMs && (!mBaseline || pingMs < mBaseline))
    {
        mBaseline = pingMs;
        mBaselineMs = nowMs;
    }
    else if (pingMs && nowMs - mBaselineMs >= 100)
    {
        mBaseline += (pingMs - mBaseline + 99) / 100; // a lasting rise is the usual after some 10 s
        mBaselineMs = nowMs;
    }

    const long long gapMs = synced ? nowMs - mLastSnapshotMs : 0;
    uint32_t received = 0, lost = 0;
    for (const Arrival& a : mArrivals)
    {
        received++;
        lost += a.lost;
    }
    const bool lossy = synced && received + lost >= 10 && lost * 100 >= (received + lost) * kLossPercent;
    const bool slowPing = pingMs && pingMs > mBaseline * 3 / 2 + 80;
    if (!lossy && !slowPing)
    {
        mSlowSinceMs = -1;
    }
    else if (mSlowSinceMs < 0)
    {
        mSlowSinceMs = nowMs;
    }

    const bool lostNow = !connected || gapMs > kStallMs;
    // Loss or a high ping waits out the onset, unless the warning shows already
    const bool slow = mSlowSinceMs >= 0 && (nowMs - mSlowSinceMs >= kOnsetMs || nowMs < mLagUntilMs);
    const bool lagNow = lostNow || gapMs > kGapMs || slow;
    if (lagNow)
    {
        mLagUntilMs = nowMs + kHoldMs;
    }
    return lostNow ? kLinkLost : nowMs < mLagUntilMs ? kLinkLag : kLinkGood;
}
