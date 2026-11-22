// port/net core (port/docs/netcode.md, "Maps and players"): see spawn_spots.hpp.
#include "spawn_spots.hpp"

uint8_t net::FreeLook(uint32_t* used)
{
    static const uint8_t kLooks[] = {0, 5, 7, 8, 9, 10, 11, 12, 13, 14};
    const uint32_t taken = *used | (*used >> 6 & 1) << 5;
    for (uint8_t look : kLooks)
    {
        if (!(taken >> look & 1))
        {
            *used |= 1u << look;
            return look;
        }
    }
    return 0;
}

namespace
{
// <zone>'s blocks ring by ring from its centre, each ring row by row
bool InZone(const net::SpawnZone& zone, const net::SpawnWorld& world, int* x, int* y, int* z)
{
    const int cx = zone.x + (zone.w >> 1), cy = zone.y + (zone.h >> 1);
    const int rings = zone.w > zone.h ? zone.w : zone.h;
    bool level = false;
    int base = 0;
    for (int r = 0; r <= rings; r++)
    {
        for (int by = cy - r; by <= cy + r; by++)
        {
            for (int bx = cx - r; bx <= cx + r; bx++)
            {
                const bool ring = by == cy - r || by == cy + r || bx == cx - r || bx == cx + r;
                const bool inside = bx >= zone.x && bx < zone.x + zone.w && by >= zone.y && by < zone.y + zone.h;
                int bz = 0;
                if (!ring || !inside || !world.Ground(bx, by, &bz))
                {
                    continue;
                }
                if (!level)
                {
                    level = true;
                    base = bz;
                }
                if (bz == base && !world.Occupied(bx, by))
                {
                    *x = bx;
                    *y = by;
                    *z = bz;
                    return true;
                }
            }
        }
    }
    return false;
}
} // namespace

bool net::SpawnBlock(const std::vector<SpawnZone>& zones, int first, const SpawnWorld& world, int* x, int* y, int* z)
{
    const int n = (int)zones.size();
    const int start = first >= 0 && first < n ? first : 0;
    for (int k = 0; k < n; k++)
    {
        if (InZone(zones[(start + k) % n], world, x, y, z))
        {
            return true;
        }
    }
    return false;
}
