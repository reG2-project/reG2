// port/net tests: a client's bad connection warning (session/link_warning.cpp) on made up links.
#include "../session/link_warning.hpp"
#include "check.hpp"
#include <stdlib.h>

namespace
{
const int kTickMs = 33;

// <ms> of a link that delivers a tick's snapshot unless <drop> says not, <ping> ms; the worst level seen
net::LinkLevel Run(net::LinkWarning& w, long long& now, uint32_t& tick, int ms, uint32_t ping, bool (*drop)(uint32_t), bool connected = true)
{
    net::LinkLevel worst = net::kLinkGood;
    for (const long long end = now + ms; now < end; now += kTickMs)
    {
        tick++;
        if (connected && !drop(tick))
        {
            w.OnSnapshot(now, tick);
        }
        const net::LinkLevel l = w.Update(now, connected, true, ping);
        worst = l > worst ? l : worst;
    }
    return worst;
}

bool None(uint32_t)
{
    return false;
}
bool All(uint32_t)
{
    return true;
}
bool OneInTen(uint32_t t)
{
    return t % 10 == 0;
}
bool OneInThree(uint32_t t)
{
    return t % 3 == 0;
}
} // namespace

int main()
{
    {
        // A steady link with 10% loss and a ping wandering 30 to 100 ms: never a warning
        net::LinkWarning w;
        long long now = 1000;
        uint32_t tick = 0;
        srand(1);
        for (int i = 0; i < 600; i++)
        {
            CHECK(Run(w, now, tick, 100, 30 + rand() % 70, OneInTen) == net::kLinkGood);
        }
    }
    {
        // A 400 ms gap: lag at once, held for a while after, then gone
        net::LinkWarning w;
        long long now = 1000;
        uint32_t tick = 0;
        CHECK(Run(w, now, tick, 3000, 60, None) == net::kLinkGood);
        CHECK(Run(w, now, tick, 400, 60, All) == net::kLinkLag);
        CHECK(Run(w, now, tick, 1500, 60, None) == net::kLinkLag);
        Run(w, now, tick, 600, 60, None);
        CHECK(Run(w, now, tick, 1000, 60, None) == net::kLinkGood);
        // A second without snapshots: lost, then lag a while once they come again
        CHECK(Run(w, now, tick, 1000, 60, All) == net::kLinkLost);
        CHECK(w.Update(now, true, true, 60) == net::kLinkLost);
        w.OnSnapshot(now, ++tick);
        CHECK(w.Update(now, true, true, 60) == net::kLinkLag);
        // Disconnected: lost
        CHECK(Run(w, now, tick, 100, 60, None, false) == net::kLinkLost);
    }
    {
        // A third of the snapshots lost: lag after the onset, not before
        net::LinkWarning w;
        long long now = 1000;
        uint32_t tick = 0;
        CHECK(Run(w, now, tick, 3000, 60, None) == net::kLinkGood);
        CHECK(Run(w, now, tick, 600, 60, OneInThree) == net::kLinkGood);
        CHECK(Run(w, now, tick, 600, 60, OneInThree) == net::kLinkLag);
    }
    {
        // The ping from 50 to 300 ms: lag; staying there, it becomes the usual and the warning goes
        net::LinkWarning w;
        long long now = 1000;
        uint32_t tick = 0;
        CHECK(Run(w, now, tick, 3000, 50, None) == net::kLinkGood);
        CHECK(Run(w, now, tick, 1000, 300, None) == net::kLinkLag);
        Run(w, now, tick, 30000, 300, None);
        CHECK(Run(w, now, tick, 3000, 300, None) == net::kLinkGood);
        CHECK(w.Baseline() > 250);
    }
    {
        // A catch-up: lag for the hold, without any other cause
        net::LinkWarning w;
        long long now = 1000;
        uint32_t tick = 0;
        CHECK(Run(w, now, tick, 3000, 60, None) == net::kLinkGood);
        w.OnCatchUp(now);
        CHECK(Run(w, now, tick, 1900, 60, None) == net::kLinkLag);
        Run(w, now, tick, 200, 60, None);
        CHECK(Run(w, now, tick, 1000, 60, None) == net::kLinkGood);
        // A frame of ours took 400 ms, the snapshots of the while taken at its end, and a jump: not the connection's
        now += 400;
        tick += 12;
        w.OnSnapshot(now, tick);
        w.OnCatchUp(now);
        CHECK(w.Update(now, true, true, 60) == net::kLinkGood);
        w.OnCatchUp(now + 33);
        CHECK(Run(w, now, tick, 1000, 60, None) == net::kLinkGood);
        // Not synced (joining, a map change): no snapshots expected
        CHECK(w.Update(now + 5000, true, false, 60) == net::kLinkGood);
    }
    printf("test_link_warning: OK\n");
    return 0;
}
