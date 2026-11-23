// port/net core (port/docs/netcode.md, "Maps and players"): where at a restart zone a player's ped starts, apart from the
// others by its slot, and the look of one the map's script has no PLAYER_PED for; the same everywhere.
#pragma once
#include <stdint.h>
#include <vector>

namespace net
{
// A restart zone, in blocks (gmp_map_zone)
struct SpawnZone
{
    int x, y, w, h;
};

// The first of the 10 looks the HUD tells apart (0, 5, 7 to 14, port/src/player_looks.hpp) that no remap in <used> (bit
// per remap) has, 6 counting as 5 (its colours); the look is added to <used>. All taken: 0
uint8_t FreeLook(uint32_t* used);

// What a spawn block needs from the map: a block's ground (false: none, else its level in *z)
struct SpawnWorld
{
    virtual bool Ground(int x, int y, int* z) const = 0;
};

// The block player <slot> starts on: of the blocks in turn from each zone's centre (Ped::SpawnCharInZone's) with ground
// at the level of the zone's first one, through <zones>[first] then each next zone (wrapping), the <slot>-th (counted
// round again past the last). From the map only, so a client predicting its respawn picks the server's block whatever
// the other players do. False: no block with ground anywhere (the game's own spot then)
bool SpawnBlock(const std::vector<SpawnZone>& zones, int first, int slot, const SpawnWorld& world, int* x, int* y, int* z);
} // namespace net
