// port/net core (port/docs/netcode.md, "Cars in turn"): see car_order.hpp.
#include "car_order.hpp"
#include <functional>
#include <queue>

namespace
{
const float kReach = 3.0f; // blocks between two cars' centres: a bus's length and more than a tick's move
const float kStill = 1.0f / 256; // a move under this (blocks per tick) is standing
} // namespace

bool net::CarOrder(const std::vector<CarMotion>& cars, std::vector<int>& order)
{
    const int n = (int)cars.size();
    order.clear();
    std::vector<int> moving;
    for (int i = 0; i < n; i++)
    {
        if (cars[i].vx * cars[i].vx + cars[i].vy * cars[i].vy > kStill * kStill)
        {
            moving.push_back(i);
        }
    }
    std::vector<std::vector<int>> after(n); // after[b]: the cars that wait for b
    std::vector<int> waits(n, 0);
    for (int a : moving)
    {
        for (int b : moving)
        {
            const float dx = cars[b].x - cars[a].x, dy = cars[b].y - cars[a].y;
            if (a == b || dx * dx + dy * dy > kReach * kReach)
            {
                continue;
            }
            if (dx * cars[a].vx + dy * cars[a].vy > 0 && dx * cars[b].vx + dy * cars[b].vy > 0)
            {
                after[b].push_back(a);
                waits[a]++;
            }
        }
    }
    std::priority_queue<int, std::vector<int>, std::greater<int>> ready;
    for (int i = 0; i < n; i++)
    {
        if (!waits[i])
        {
            ready.push(i);
        }
    }
    std::vector<bool> done(n, false);
    int next = 0; // the first index not yet ordered, for a ring
    while ((int)order.size() < n)
    {
        int i;
        if (!ready.empty())
        {
            i = ready.top();
            ready.pop();
        }
        else
        {
            while (done[next])
            {
                next++;
            }
            i = next;
        }
        if (done[i])
        {
            continue;
        }
        done[i] = true;
        order.push_back(i);
        for (int a : after[i])
        {
            if (--waits[a] == 0 && !done[a])
            {
                ready.push(a);
            }
        }
    }
    for (int i = 0; i < n; i++)
    {
        if (order[i] != i)
        {
            return true;
        }
    }
    return false;
}
