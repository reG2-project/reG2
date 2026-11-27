// port/net tests: a client's tick pacing (session/pacing.cpp) against made up servers and links.
#include "../session/pacing.hpp"
#include "check.hpp"
#include <deque>
#include <math.h>

namespace
{
const int kTickUs = 33000;
const long long kStepUs = 100; // the simulation's resolution

struct Message
{
    long long atUs;
    uint32_t tick;
    int slack;
};

struct Link
{
    long long delayUs; // one way
    long long jitterUs;
    uint32_t seed;
    long long Delay()
    {
        seed = seed * 1103515245 + 12345;
        return delayUs + (jitterUs ? (long long)((seed >> 8) % (uint32_t)(2 * jitterUs + 1)) - jitterUs : 0);
    }
};

struct Result
{
    int worstSlackAfter = -1000, leastSlackAfter = 1000; // the true slack's range from <settleUs> on
    double worstRamp = 0; // the largest change of the step length from one tick to the next, in us
    double meanPaceAfterUs = 0;
};

struct Server
{
    double tickUs;
    long long changeAtUs; // from then on (-1: never) it ticks every <laterUs>, after a stall of <stallUs>
    double laterUs;
    long long stallUs;
};

// <seconds> of a server, our commands <startSlack> early at first; the slack and the pace measured from <settleUs> on
Result Run(Server server, Link link, int startSlack, double seconds, long long settleUs)
{
    double serverUs = server.tickUs;
    long long stallAtUs = server.changeAtUs;
    net::Pacer pacer;
    pacer.Reset(kTickUs);
    std::deque<Message> toClient, toServer;
    uint32_t serverTick = 0, newestCmd = 0;
    const uint32_t lead = startSlack + (uint32_t)(link.delayUs / kTickUs);
    uint32_t clientTick = lead;
    double nextServerUs = serverUs, nextClientUs = kTickUs;
    int lastPace = kTickUs;
    Result r;
    long long paceSum = 0, paceCount = 0;
    for (long long now = 0; now < (long long)(seconds * 1e6); now += kStepUs)
    {
        while (!toServer.empty() && toServer.front().atUs <= now)
        {
            newestCmd = toServer.front().tick > newestCmd ? toServer.front().tick : newestCmd;
            toServer.pop_front();
        }
        if (now >= nextServerUs)
        {
            serverTick++;
            const int slack = (int)newestCmd - (int)(serverTick + 1);
            if (now >= settleUs)
            {
                r.worstSlackAfter = slack > r.worstSlackAfter ? slack : r.worstSlackAfter;
                r.leastSlackAfter = slack < r.leastSlackAfter ? slack : r.leastSlackAfter;
            }
            toClient.push_back(Message{now + link.Delay(), serverTick, slack});
            nextServerUs += serverUs;
            if (stallAtUs >= 0 && now >= stallAtUs)
            {
                serverUs = server.laterUs;
                nextServerUs += server.stallUs;
                stallAtUs = -1;
            }
        }
        if (now >= nextClientUs)
        {
            clientTick++;
            uint32_t newest = 0;
            for (std::deque<Message>::iterator m = toClient.begin(); m != toClient.end();)
            {
                if (m->atUs <= now)
                {
                    if (m->tick > newest)
                    {
                        pacer.OnSnapshot(now, m->tick);
                        pacer.OnSlack(m->slack);
                        newest = m->tick;
                    }
                    m = toClient.erase(m);
                }
                else
                {
                    ++m;
                }
            }
            toServer.push_back(Message{now + link.Delay(), clientTick + 1, 0});
            const int pace = pacer.NextUs(true);
            r.worstRamp = fabs((double)(pace - lastPace)) > r.worstRamp ? fabs((double)(pace - lastPace)) : r.worstRamp;
            lastPace = pace;
            if (now >= settleUs)
            {
                paceSum += pace;
                paceCount++;
            }
            nextClientUs += pace;
        }
    }
    r.meanPaceAfterUs = paceCount ? (double)paceSum / paceCount : 0;
    return r;
}

void CheckSmooth(const Result& r)
{
    CHECK(r.worstRamp <= net::Pacer::kRamp * kTickUs + 1);
}
} // namespace

int main()
{
    // A server 12% slow from the start, our commands 13 ticks early: back to the target within 5 s, and they stay there
    {
        const Result r = Run(Server{kTickUs * 1.12, -1, 0, 0}, Link{25000, 3000, 1}, 13, 30, 5000000);
        CHECK(r.leastSlackAfter >= 0 && r.worstSlackAfter <= 3);
        CHECK(fabs(r.meanPaceAfterUs - kTickUs * 1.12) < kTickUs * 0.02);
        CheckSmooth(r);
    }
    // A server that turns 12% slow after 10 s: the lead hardly moves
    {
        const Result r = Run(Server{kTickUs, 10000000, kTickUs * 1.12, 0}, Link{25000, 3000, 6}, 2, 30, 3000000);
        CHECK(r.leastSlackAfter >= 0 && r.worstSlackAfter <= 4);
        CheckSmooth(r);
    }
    // The server at its tick length, a usual link: a step length within a percent of it, never late
    {
        const Result r = Run(Server{kTickUs, -1, 0, 0}, Link{50000, 10000, 2}, 1, 30, 2000000);
        CHECK(r.leastSlackAfter >= 0 && r.worstSlackAfter <= 3);
        CHECK(fabs(r.meanPaceAfterUs - kTickUs) < kTickUs * 0.01);
        CheckSmooth(r);
    }
    // A server 5% fast, our commands late at first
    {
        const Result r = Run(Server{kTickUs * 0.95, -1, 0, 0}, Link{30000, 5000, 3}, -2, 30, 3000000);
        CHECK(r.leastSlackAfter >= 0 && r.worstSlackAfter <= 3);
        CheckSmooth(r);
    }
    // A server that stalls 400 ms: the ticks it lost are paced back within 3 s, its clock still measured right
    {
        const Result r = Run(Server{kTickUs, 10000000, kTickUs, 400000}, Link{25000, 3000, 4}, 2, 30, 13400000);
        CHECK(r.leastSlackAfter >= 0 && r.worstSlackAfter <= 3);
        CHECK(fabs(r.meanPaceAfterUs - kTickUs) < kTickUs * 0.01);
        CheckSmooth(r);
    }
    // A long round trip (300 ms) and a server 10% slow: no swinging around the target
    {
        const Result r = Run(Server{kTickUs * 1.1, -1, 0, 0}, Link{150000, 10000, 5}, 12, 40, 6000000);
        CHECK(r.leastSlackAfter >= 0 && r.worstSlackAfter <= 3);
        CheckSmooth(r);
    }
    printf("test_pacing: ok\n");
    return 0;
}
