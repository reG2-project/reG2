// port/net core (port/docs/netcode.md): the most players a network game has, the original game's 6 raised. A port build's
// MAX_PLAYERS (Source/Game.hpp) is this, and the netcode's slot masks and fields are sized from it.
#pragma once

#define NET_MAX_PLAYERS 10

namespace net
{
const int kMaxPlayers = NET_MAX_PLAYERS;

// The bits a field needs for the values 0..max
constexpr int BitsFor(int max)
{
    return max > 1 ? 1 + BitsFor(max >> 1) : 1;
}
static_assert(kMaxPlayers <= 16, "slot masks are 16 bits (SlotMask)");
} // namespace net
