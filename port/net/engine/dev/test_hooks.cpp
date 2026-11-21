// port/net engine dev (port/docs/netcode.md, "Checks"): a server's test cheats, for the checks that need a death, a weapon, a
// wanted level, a power-up, a car or a train at a given tick (GTA2_NET_TEST*, run by the server's test hooks, port/net/dev).
#include "../internal.hpp"
#include "dev.hpp"
#include "Source/Car.hpp"
#include "Source/Game.hpp"
#include "Source/Ped.hpp"
#include "Source/Player.hpp"
#include "Source/PublicTransport.hpp"
#include "Source/Weapon.hpp"
#include "Source/map_0x370.hpp"
#include <stdlib.h>
#include <string>

void net::engine::DebugKill(int victim, int killer)
{
    Ped* ped = InGamePed(victim);
    if (ped && !ped->isDead())
    {
        KillCredited(ped, killer);
    }
}

void net::engine::DebugWanted(int player, int points)
{
    Ped* ped = InGamePed(player);
    if (ped && !ped->isDead() && ped->mWantedPoints < points)
    {
        ped->mWantedPoints = (s16)points;
    }
}

void net::engine::DebugPowerUp(int player, int powerUp)
{
    Player* p = GetPlayer(player);
    if (p && InGamePed(player) && powerUp >= 0 && powerUp < kPowerUps)
    {
        p->CollectPowerUp(powerUp);
    }
}

void net::engine::DebugCar(int player, int model)
{
    Ped* ped = InGamePed(player);
    if (ped && !ped->mCar && gCarManager_6C)
    {
        // Beside the player, as the debug menu spawns one (ImGuiDebug.cpp): scale 1, counted as recycled
        Fix16 scale;
        scale.mValue = 0x4000;
        Fix16 off;
        off.FromInt(2);
        Car* car = gCarManager_6C->SpawnCarAt(ped->get_cam_x() + off, ped->get_cam_y(), ped->get_cam_z(), 0, model, scale);
        if (car)
        {
            car->IncrementCarStats(car_kind::recycled_1);
        }
    }
}

void net::engine::DebugTrain(int player, int train)
{
    Player* p = GetPlayer(player);
    Ped* ped = InGamePed(player);
    const int i = train % 10;
    Train* t = gPublicTransport_181C && train >= 0 && train < 30 ? &gPublicTransport_181C->mTrainArray[i] : NULL;
    if (!t || t->mAllocState != 2 || !t->mCarriages[0] || !t->mCarriages[0]->mDriver)
    {
        return;
    }
    if (train >= 20)
    {
        // On the platform of the station it stops at next: the zone's highest cell
        const gmp_map_zone* zone = t->mStation ? t->mStation->mpZone : NULL;
        int bestX = -1, bestY = -1;
        s32 bestZ = -1;
        for (int y = zone ? zone->mY : 0; zone && y < zone->mY + zone->mH; y++)
        {
            for (int x = zone->mX; x < zone->mX + zone->mW; x++)
            {
                s32 z = -1;
                if (gMap_0x370->FindHighestBlockForCoord(x, y, &z) && z > bestZ)
                {
                    bestX = x, bestY = y, bestZ = z;
                }
            }
        }
        if (ped && !ped->mCar && !ped->isDead() && bestX >= 0)
        {
            Fix16 x, y, z;
            x.mValue = (bestX << 14) + 0x2000;
            y.mValue = (bestY << 14) + 0x2000;
            z.FromInt(bestZ + 1);
            MovePed(ped, x, y, z, Angle(0));
            SnapCamera(p); // else its ped stays off its screen, and dead there the game frees it (Ped::PoolUpdate)
        }
    }
    else if (train >= 10)
    {
        // Into reverse as a player driving it puts it there (from standing): the cab and the last wagon swap ends
        t->mState = 2;
        t->DecrementState();
    }
    else if (ped && !ped->mCar && !ped->isDead())
    {
        ForceIntoCar(ped, t->mCarriages[0]);
    }
}

void net::engine::DebugArm(int player, int weapon, int ammo)
{
    Player* p = GetPlayer(player);
    if (p && InGamePed(player) && weapon >= 0 && weapon < kPlayerWeapons && p->mWeapons[weapon] &&
        p->mWeapons[weapon]->mAmmo == 0)
    {
        p->AddWeaponWithAmmo(weapon, (u8)ammo);
    }
}
