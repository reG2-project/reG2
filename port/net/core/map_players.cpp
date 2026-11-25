// port/net core (port/docs/netcode.md, "Running"): see map_players.hpp.
#include "map_players.hpp"
#include "player_limit.hpp"

namespace
{
// ScriptManager: u16 mBasePointers[6000] (each command's offset in the data, 0 none), u8 mScriptData[65536]
const size_t kPointers = 6000;
const size_t kData = 65536;
const uint16_t kPlayerPed = 5; // SCRCMD_PLAYER_PED

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
