// port/net core (port/docs/netcode.md, "Running"): see map_players.hpp.
#include "map_players.hpp"
#include "player_limit.hpp"
#include <string.h>

namespace
{
// ScriptManager: u16 mBasePointers[6000] (each command's offset in the data, 0 none), u8 mScriptData[65536]
const size_t kPointers = 6000;
const size_t kData = 65536;
const uint16_t kPlayerPed = 5; // SCRCMD_PLAYER_PED
const uint8_t kRestartZone = 16; // gmp_zone_type Restart_16

uint16_t U16(const uint8_t* p)
{
    return (uint16_t)(p[0] | p[1] << 8);
}
} // namespace

int net::ScriptPlayerPeds(const uint8_t* scr, size_t size)
{
    if (!scr || size < kPointers * 2 + kData)
    {
        return -1;
    }
    const uint8_t* data = scr + kPointers * 2;
    int peds = 0;
    // As ScriptThread::ExecOpCode runs the declarations: from command 1
    for (size_t i = 1; i < kPointers; i++)
    {
        const uint16_t at = U16(scr + i * 2);
        if (at && at + 4u <= kData && U16(data + at + 2) == kPlayerPed)
        {
            peds++;
        }
    }
    return peds;
}

int net::MapPlayers(int scriptPlayerPeds, int mmpPlayerCount)
{
    int n = scriptPlayerPeds < kMaxPlayers ? scriptPlayerPeds : kMaxPlayers;
    n = mmpPlayerCount > 0 && mmpPlayerCount < n ? mmpPlayerCount : n;
    return n > 0 ? n : 0;
}

int net::GmpRestartZones(const uint8_t* gmp, size_t size)
{
    // "GBMP", a 16-bit version, then chunks: a 4-character type and a 32-bit size
    if (!gmp || size < 6 || memcmp(gmp, "GBMP", 4) != 0)
    {
        return -1;
    }
    int zones = 0;
    for (size_t at = 6; at + 8 <= size;)
    {
        const uint32_t n = (uint32_t)(gmp[at + 4] | gmp[at + 5] << 8 | gmp[at + 6] << 16 | (uint32_t)gmp[at + 7] << 24);
        const uint8_t* chunk = gmp + at + 8;
        if (n > size - at - 8)
        {
            return -1;
        }
        if (memcmp(gmp + at, "ZONE", 4) == 0)
        {
            // Each zone: its type, x, y, w, h, the name's length, the name (gmp_map_zone)
            for (size_t z = 0; z + 6 <= n; z += 6 + chunk[z + 5])
            {
                zones += chunk[z] == kRestartZone;
            }
        }
        at += 8 + n;
    }
    return zones;
}
