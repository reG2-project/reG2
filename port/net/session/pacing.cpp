// port/net: a client's tick pacing (pacing.hpp).
#include "pacing.hpp"
#include <algorithm>
#include <math.h>

namespace
{
const double kSlackSmoothing = 0.25; // of each snapshot's slack into the average
const uint32_t kMostGap = 300; // ticks between two snapshots beyond which the server's clock is measured anew
const int kLeastBuckets = 4; // complete ones before the measure counts
} // namespace

void net::Pacer::Reset(int tickUs)
{
    mTickUs = tickUs > 0 ? tickUs : 33000;
    mPaceUs = mTickUs;
    mBucketCount = 0;
    mLastTick = 0;
    mServerUs = 0;
    mSlackAvg = 0;
    mSlackKnown = false;
}

void net::Pacer::OnSnapshot(long long nowUs, uint32_t tick)
{
    // A new game or a reconnect starts the server's ticks elsewhere
    if (mBucketCount && (tick <= mLastTick || tick - mLastTick > kMostGap))
    {
        mBucketCount = 0;
    }
    mLastTick = tick;
    const double offset = (double)nowUs - (double)tick * mTickUs;
    Bucket* b = mBucketCount ? &mBuckets[mBucketCount - 1] : NULL;
    if (b && tick / kBucketTicks == b->tick / kBucketTicks)
    {
        if (offset < b->offsetUs)
        {
            b->tick = tick;
            b->offsetUs = offset;
        }
        return;
    }
    if (mBucketCount == kBuckets)
    {
        std::copy(mBuckets + 1, mBuckets + kBuckets, mBuckets);
        mBucketCount--;
    }
    mBuckets[mBucketCount++] = Bucket{tick, offset};
    // The least delayed snapshots follow the server's clock; the median slope between neighbours ignores a stall's step
    const int n = mBucketCount - 1; // the complete ones
    if (n < kLeastBuckets)
    {
        return;
    }
    double slopes[kBuckets];
    int count = 0;
    for (int i = 1; i < n; i++)
    {
        slopes[count++] = (mBuckets[i].offsetUs - mBuckets[i - 1].offsetUs) / (double)(mBuckets[i].tick - mBuckets[i - 1].tick);
    }
    std::nth_element(slopes, slopes + count / 2, slopes + count);
    mServerUs = std::min(std::max(mTickUs + slopes[count / 2], mTickUs * 0.75), mTickUs * 1.5);
}

void net::Pacer::OnSlack(int slack)
{
    mSlackAvg = mSlackKnown ? mSlackAvg + kSlackSmoothing * (slack - mSlackAvg) : slack;
    mSlackKnown = true;
}

double net::Pacer::ServerTickUs() const
{
    return mServerUs > 0 ? mServerUs : mTickUs;
}

int net::Pacer::NextUs(bool slackKnown)
{
    mSlackKnown = mSlackKnown && slackKnown;
    double target = ServerTickUs();
    if (mSlackKnown)
    {
        const double from = mSlackAvg - kTargetSlack;
        const double off = from > kBand ? from - kBand : from < -kBand ? from + kBand : 0;
        const double ratio = 1 + off * (off > 0 ? kEarlyGain : kLateGain);
        target *= std::min(std::max(ratio, 1 - kMostFast), 1 + kMostSlow);
    }
    const double most = kRamp * mTickUs;
    mPaceUs += std::min(std::max(target - mPaceUs, -most), most);
    return (int)lround(mPaceUs);
}
