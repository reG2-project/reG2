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
// <zone>'s blocks ring by ring from its centre, each ring row by row, its top and bottom rows whole and the others'
// two ends; within the zone only
bool InZone(const net::SpawnZone& zone, const net::SpawnWorld& world, int* x, int* y, int* z)
{
    const int cx = zone.x + (zone.w >> 1), cy = zone.y + (zone.h >> 1);
    const int rings = zone.w > zone.h ? zone.w : zone.h;
    const int x0 = zone.x, x1 = zone.x + zone.w - 1, y0 = zone.y, y1 = zone.y + zone.h - 1;
    bool level = false;
    int base = 0;
    auto take = [&](int bx, int by) {
        int bz = 0;
        if (!world.Ground(bx, by, &bz))
        {
            return false;
        }
        if (!level)
        {
            level = true;
            base = bz;
        }
        if (bz != base || world.Occupied(bx, by))
        {
            return false;
        }
        *x = bx;
        *y = by;
        *z = bz;
        return true;
    };
    for (int r = 0; r <= rings; r++)
    {
        const int top = cy - r > y0 ? cy - r : y0, bottom = cy + r < y1 ? cy + r : y1;
        for (int by = top; by <= bottom; by++)
        {
            if (by == cy - r || by == cy + r)
            {
                const int left = cx - r > x0 ? cx - r : x0, right = cx + r < x1 ? cx + r : x1;
                for (int bx = left; bx <= right; bx++)
                {
                    if (take(bx, by))
                    {
                        return true;
                    }
                }
                continue;
            }
            if ((cx - r >= x0 && cx - r <= x1 && take(cx - r, by)) || (r && cx + r >= x0 && cx + r <= x1 && take(cx + r, by)))
            {
                return true;
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
