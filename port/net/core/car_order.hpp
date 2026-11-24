// port/net core (port/docs/netcode.md, "Cars in turn"): the order the cars move in a tick, a car ahead of a car
// following it first, so that the follower's move meets it where it is drawn after the tick.
#pragma once
#include <vector>

namespace net
{
struct CarMotion
{
    float x, y; // in blocks
    float vx, vy; // its move last tick (blocks per tick)
};

// The pool's order (indices into <cars>), with each car moved after the cars close in front of it that move away
// from it; a ring of them keeps the pool's order. True if that is not the pool's order
bool CarOrder(const std::vector<CarMotion>& cars, std::vector<int>& order);
} // namespace net
