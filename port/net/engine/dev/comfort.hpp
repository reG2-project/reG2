// port/net engine dev (GTA2_NET_COMFORT=1 on the server and its clients): what a player feels, read by
// port/net/scripts/comfort.py. Off unless the switch is set.
#pragma once
#include <stdint.h>

namespace net
{
namespace engine
{
bool ComfortOn();
// Once per server tick, after it ran
void ComfortServerTick(uint32_t tick);
// Once per client frame, the game as it is drawn: <tick> the predicted one, <input> the one chosen for the next tick
void ComfortClientFrame(uint32_t frame, uint32_t tick, int slot, uint32_t input);
// A correction found by a reconcile, before its blend (in blocks), of what is at <x>, <y>
void ComfortCorrection(uint32_t id, float blocks, float x, float y);
} // namespace engine
} // namespace net
