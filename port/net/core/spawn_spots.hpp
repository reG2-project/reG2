// port/net core (port/docs/netcode.md, "Maps and players"): where at a restart zone a player's ped starts, apart from the
// others, and the look of one the map's script has no PLAYER_PED for; from the game's state only, the same everywhere.
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

// What a spawn block needs from the game: a block's ground (false: none, else its level in *z), and whether a player's
// ped stands on it
struct SpawnWorld
{
    virtual bool Ground(int x, int y, int* z) const = 0;
    virtual bool Occupied(int x, int y) const = 0;
};

// The block a ped starts on: in <zones>[first], then each next zone (wrapping) once one is full, the first block in turn
// from the zone's centre (Ped::SpawnCharInZone's) with ground at the level of the zone's first one and nobody on it.
// False: none anywhere (the game's own spot then)
bool SpawnBlock(const std::vector<SpawnZone>& zones, int first, const SpawnWorld& world, int* x, int* y, int* z);
} // namespace net
