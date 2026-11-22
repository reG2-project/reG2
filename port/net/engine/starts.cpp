// port/net (port/docs/netcode.md, "Maps and players"): where a network game's players start. The map's script gives
// as many peds as it has PLAYER_PEDs; the players past them get one at the restart zones, as Handle_PLAYER_PED and
// Player::InitPlayerPed make one, in a look of their own. A ped starting at a restart zone (those, a join, a respawn)
// takes a block of it no other player stands on (core/spawn_spots.hpp). Both read the game's state only, so the server
// and its clients place them alike.
#include "internal.hpp"
#include "../core/spawn_spots.hpp"
#include "Source/Char_Pool.hpp"
#include "Source/Game.hpp"
#include "Source/Ped.hpp"
#include "Source/Player.hpp"
#include "Source/map_0x370.hpp"
#include "Source/ped_graphic_type.hpp"
#include "port/src/net_hooks.hpp"
#include <stdio.h>
#include <string>

namespace
{
const u8 kRestartZone = 0x10;
const int kMostZones = 40; // as many as Map_0x370::GetNextZoneOfTypeRoundRobin turns through

// The map's restart zones in order, without moving the game's turn (its round robin is the script's and the respawns')
std::vector<gmp_map_zone*> RestartZones()
{
    std::vector<gmp_map_zone*> zones;
    if (!gMap_0x370 || !gMap_0x370->mpZoneData || !gMap_0x370->mpZones)
    {
        return zones;
    }
    for (u16 i = 0; i < gMap_0x370->mpZones->mNumZones && (int)zones.size() < kMostZones; i++)
    {
        gmp_map_zone* z = gMap_0x370->GetZone(i);
        if (z->mZoneType == kRestartZone)
        {
            zones.push_back(z);
        }
    }
    return zones;
}

struct GameWorld : net::SpawnWorld
{
    const Ped* self;
    explicit GameWorld(const Ped* s) : self(s)
    {
    }
    bool Ground(int x, int y, int* z) const override
    {
        s32 found = 0;
        if (x < 0 || x > 255 || y < 0 || y > 255 || !gMap_0x370->FindHighestBlockForCoord(x, y, &found))
        {
            return false;
        }
        *z = found;
        return true;
    }
    // Another player's ped less than a block from the block's centre
    bool Occupied(int x, int y) const override
    {
        const s32 cx = (x << 14) + (1 << 13), cy = (y << 14) + (1 << 13), kBlock = 1 << 14;
        for (s32 i = 0; i < gGame_0x40->mNumPlayers; i++)
        {
            Player* p = gGame_0x40->mPlayers[i];
            const Ped* ped = p && p->mInUse ? p->mPlayerPed : NULL;
            if (!ped || ped == self)
            {
                continue;
            }
            const s32 dx = ped->get_cam_x().mValue - cx, dy = ped->get_cam_y().mValue - cy;
            if (dx < kBlock && dx > -kBlock && dy < kBlock && dy > -kBlock)
            {
                return true;
            }
        }
        return false;
    }
};

bool FreeBlock(const std::vector<gmp_map_zone*>& zones, int first, const Ped* self, s32* x, s32* y, s32* z)
{
    std::vector<net::SpawnZone> rects;
    for (gmp_map_zone* zone : zones)
    {
        rects.push_back(net::SpawnZone{zone->mX, zone->mY, zone->mW, zone->mH});
    }
    int bx = 0, by = 0, bz = 0;
    if (!gGame_0x40 || !net::SpawnBlock(rects, first, GameWorld(self), &bx, &by, &bz))
    {
        return false;
    }
    *x = bx;
    *y = by;
    *z = bz;
    return true;
}

// Ped::SpawnCharInZone: a block of <zone> nobody stands on, else of the next restart zones
bool StartBlock(Ped* ped, gmp_map_zone* zone, s32* x, s32* y, s32* z)
{
    const std::vector<gmp_map_zone*> zones = RestartZones();
    for (size_t i = 0; i < zones.size(); i++)
    {
        if (zones[i] == zone)
        {
            return FreeBlock(zones, (int)i, ped, x, y, z);
        }
    }
    return false; // not a restart zone: the game's own spot
}

// Game::BootGame, after the script's PLAYER_PEDs: the k-th player without a ped at the k-th restart zone
void ExtraPlayerPeds()
{
    const std::vector<gmp_map_zone*> zones = RestartZones();
    uint32_t looks = 0;
    for (s32 i = 0; i < gGame_0x40->mNumPlayers; i++)
    {
        Player* p = gGame_0x40->mPlayers[i];
        if (p && p->mPlayerPed)
        {
            looks |= 1u << ((u8)p->mPlayerPed->mRemap & 31);
        }
    }
    int k = 0;
    while (Player* p = gGame_0x40->GetFirstPlayerWithoutPed())
    {
        s32 x = 0, y = 0, z = 0;
        Ped* ped = NULL;
        const u8 look = net::FreeLook(&looks);
        if (!zones.empty())
        {
            gmp_map_zone* zone = zones[k % zones.size()];
            if (!FreeBlock(zones, k % (int)zones.size(), NULL, &x, &y, &z))
            {
                x = zone->mX + (zone->mW >> 1); // all taken: the zone's centre, where the game would start it
                y = zone->mY + (zone->mH >> 1);
            }
            const Fix16 fx = Fix16(x) + net::engine::Raw(1 << 13), fy = Fix16(y) + net::engine::Raw(1 << 13); // the block's centre
            ped = gPedManager->SpawnPedAt(fx, fy, gMap_0x370->FindGroundZForCoord(fx, fy), look, Ang16((s16)0, (u8)0));
        }
        if (!ped)
        {
            printf("net: player %d has no ped on this map (no restart zone, or the peds are all taken)\n", (int)p->mPlayerIdx);
            p->Disconnect(); // out of the game, as a slot nobody takes
            continue;
        }
        ped->SetPedType(ped_type::player_2);
        ped->set_health(100);
        p->InitPlayerPed(ped);
        ped->mGraphicType = ped_graphic_type::character_1;
        k++;
    }
    if (k)
    {
        std::string starts;
        for (s32 i = 0; i < gGame_0x40->mNumPlayers; i++)
        {
            const Ped* ped = gGame_0x40->mPlayers[i]->mPlayerPed;
            char one[48];
            snprintf(one, sizeof(one), " %d:%.1f,%.1f/%d", (int)i, ped ? ped->get_cam_x().ToFloat() : -1.0f, ped ? ped->get_cam_y().ToFloat() : -1.0f,
                     ped ? (int)(u8)ped->mRemap : -1);
            starts += one;
        }
        printf("net: %d players past the script's start at its restart zones; player:x,y/look%s\n", k, starts.c_str());
    }
}
} // namespace

void net::engine::InstallStarts(bool on)
{
    gPortExtraPlayerPeds = on ? ExtraPlayerPeds : NULL;
    gPortSpawnBlock = on ? StartBlock : NULL;
}
