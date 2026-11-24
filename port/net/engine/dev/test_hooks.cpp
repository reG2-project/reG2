// port/net engine dev (port/docs/netcode.md, "Checks"): a server's test cheats, for the checks that need a death, a weapon, a
// wanted level, a power-up, a car or a train at a given tick (GTA2_NET_TEST*, run by the server's test hooks, port/net/dev).
#include "../internal.hpp"
#include "dev.hpp"
#include "Source/Car.hpp"
#include "Source/Char_Pool.hpp"
#include "Source/char.hpp"
#include "Source/Game.hpp"
#include "Source/Ped.hpp"
#include "Source/Player.hpp"
#include "Source/PublicTransport.hpp"
#include "Source/Weapon.hpp"
#include "Source/WeaponManager.hpp"
#include "Source/map_0x370.hpp"
#include "Source/Object_5C.hpp"
#include "Source/explosion_type.hpp"
#include "Source/sprite.hpp"
#include "Source/rng.hpp"
#include "port/src/net_hooks.hpp"
#include <algorithm>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <string>
#include <vector>

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
    const int damage = model / 1000 * 1000; // model + 1000 * n: the car damaged by n * 1000 (16000: on fire, 25000: burning)
    model %= 1000;
    Ped* ped = InGamePed(player);
    if (!CarModelWrecks(model))
    {
        printf("net: test car model %d has no wreck: not spawned\n", model);
        return;
    }
    if (ped && !ped->mCar && gCarManager_6C)
    {
        // Beside the player, as the debug menu spawns one (ImGuiDebug.cpp): scale 1, counted as recycled
        Fix16 scale;
        scale.mValue = 0x4000;
        Fix16 off;
        off.FromInt(2);
        // A truck cab comes with a container trailer (CarManager::SpawnCabAndTrailer, as the map's parked trucks)
        if (model == car_model_enum::TRUKCAB1 || model == car_model_enum::TRUKCAB2)
        {
            gCarManager_6C->SpawnCabAndTrailer(ped->get_cam_x() + off, ped->get_cam_y(), 0, model, car_model_enum::TRUKCONT);
            return;
        }
        Car* car = gCarManager_6C->SpawnCarAt(ped->get_cam_x() + off, ped->get_cam_y(), ped->get_cam_z(), 0, model, scale);
        if (car)
        {
            car->IncrementCarStats(car_kind::recycled_1);
            if (damage)
            {
                car->AccumulateDamage((s16)damage, Fix16_Point(0, 0)); // as a hit does, its fire with it
            }
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
    Ped* ped = p ? InGamePed(player) : NULL;
    if (ped && weapon >= weapon_type::car_bomb && weapon < kPlayerWeapons && ped->mCar && ped->mCar->mDriver == ped &&
        !gWeapon_8->find(ped->mCar, weapon))
    {
        gWeapon_8->add_car_weapon(weapon, (u8)ammo, ped->mCar); // a car weapon: the car's, while the player drives one
    }
    else if (ped && weapon >= 0 && weapon < kPlayerWeapons && p->mWeapons[weapon] && p->mWeapons[weapon]->mAmmo == 0)
    {
        p->AddWeaponWithAmmo(weapon, (u8)ammo);
    }
}

void net::engine::DebugPatients(int player, int count, int fromBlocks)
{
    Ped* ped = InGamePed(player);
    if (!ped)
    {
        return;
    }
    std::vector<std::pair<s32, Ped*>> candidates;
    for (Ped* p = gPedPool->mPool.mpPrev; p; p = p->mpNext)
    {
        if (!p->mPlayer && !p->mCar && p->mGameObject && !p->isDead() && !p->IsOnFire())
        {
            const s32 d = Fix16::MaxAbsDistance(p->get_cam_x(), p->get_cam_y(), ped->get_cam_x(), ped->get_cam_y()).mValue;
            if (d >= fromBlocks << 14)
            {
                candidates.push_back(std::make_pair(d, p));
            }
        }
    }
    std::sort(candidates.begin(), candidates.end(), [](const std::pair<s32, Ped*>& a, const std::pair<s32, Ped*>& b) { return a.first < b.first; });
    for (int i = 0; i < count && i < (int)candidates.size(); i++)
    {
        candidates[i].second->SetOnFire();
    }
}

void net::engine::DebugStreak(int player)
{
    Player* p = GetPlayer(player);
    if (p && InGamePed(player))
    {
        p->mScores.mExecutionCount = 20; // its next PlayerScoreTracker::Service gives the bonus
    }
}

void net::engine::DebugBlast(int player, int tenths)
{
    Ped* ped = InGamePed(player);
    Sprite* s = ped ? PedSprite(ped) : NULL;
    // None on a player just respawned: invulnerable, it would walk into the blast again and again, which no client has
    if (s && ped->mInvulnerability <= 0)
    {
        const Fix16 x = s->mXy.x + Fix16(tenths * 16384 / 10, 0);
        gObject_5C->CreateExplosion(x, s->mXy.y, s->mZpos, Ang16((s16)0, (u8)0), explosion_type::small_18, 0);
    }
}

void net::engine::DebugFire(int player)
{
    if (Ped* ped = InGamePed(player))
    {
        ped->SetOnFire();
    }
}

namespace
{
// Drowned (0), electrocuted (1) or knocked down (2), as the game starts them
void StartPhase(Ped* ped, int what)
{
    if (!ped || !ped->mGameObject || ped->mCar || ped->isDead() || ped->mPedState1 == ped_state_1::immobilized_8)
    {
        return;
    }
    if (what == 0)
    {
        ped->mGameObject->DrownPed();
        return;
    }
    ped->ChangeNextPedState1(ped_state_1::immobilized_8);
    ped->ChangeNextPedState2(what == 1 ? ped_state_2::electrocuted_27 : ped_state_2::lying_on_floor_22);
    ped->SetStateInitPending();
}

Ped* NearestAmbient(Ped* to)
{
    Ped* best = NULL;
    s32 bestD = 10 << 14;
    for (Ped* p = gPedPool->mPool.mpPrev; to && p; p = p->mpNext)
    {
        if (!p->mPlayer && !p->mCar && p->mGameObject && !p->isDead() && p->mPedState1 != ped_state_1::immobilized_8)
        {
            const s32 d = Fix16::MaxAbsDistance(p->get_cam_x(), p->get_cam_y(), to->get_cam_x(), to->get_cam_y()).mValue;
            if (d < bestD)
            {
                best = p, bestD = d;
            }
        }
    }
    return best;
}

// Code 4's jacks: the first never pulls (called off after 10 ticks at the door), the next waits its first 10 ticks there;
// a client predicts both pulls at once, the first to be cancelled
int gPullAsks;
bool gNeverPull;
Ped* sVictim;
bool HeldPull(Ped* victim)
{
    if (victim != sVictim)
    {
        return true; // another jack (an ambient car thief's)
    }
    return ++gPullAsks > 10 && !gNeverPull;
}
} // namespace

void net::engine::DebugPhase(int player, int what)
{
    Ped* ped = InGamePed(player);
    if (!ped)
    {
        return;
    }
    if (what >= 10)
    {
        Ped* p = NearestAmbient(ped);
        Sprite* s = p ? CharSprite(p) : NULL;
        if (!s)
        {
            return;
        }
        printf("net: test phase %d on ped %u\n", what - 10, PedNetId(p));
        if (what == 13)
        {
            gObject_5C->CreateExplosion(s->mXy.x, s->mXy.y, s->mZpos, Ang16((s16)0, (u8)0), explosion_type::small_18, 0);
        }
        else
        {
            StartPhase(p, what - 10);
        }
        return;
    }
    if (what == 0)
    {
        StartPhase(ped, 0);
    }
    else if (what == 1 && ped->mGameObject && !ped->mCar && ped->mPedState1 != ped_state_1::immobilized_8)
    {
        ped->mShockCounter = ped->mElectrocutionThreshold; // Ped::PoolUpdate electrocutes it
    }
    else if (what == 2 && !ped->mCar && gCarManager_6C)
    {
        Fix16 scale;
        scale.mValue = 0x4000;
        Car* car = gCarManager_6C->SpawnCarAt(ped->get_cam_x() + Fix16(2 << 14, 0), ped->get_cam_y(), ped->get_cam_z(), 0, car_model_enum::MERC, scale);
        if (car)
        {
            car->IncrementCarStats(car_kind::recycled_1);
            ForceIntoCar(ped, car);
        }
    }
    else if (what == 3 && ped->mCar && ped->mCar->mCarSprite)
    {
        const Sprite* cs = ped->mCar->mCarSprite;
        StartPulledOut(ped, ped->mCar, 0, cs->mXy.x + Fix16(1), cs->mXy.y, cs->mZpos);
    }
    // Code 4: tried at several ticks, the jacker sent again while its victim still sits in the car made for it
    static Car* sCar;
    static Fix16 sX, sY, sZ;
    if (what != 4 || ped->isDead() || !gCarManager_6C)
    {
        return;
    }
    if (sVictim && gNeverPull && gPullAsks >= 10)
    {
        const uint32_t tick = gpRng ? (uint32_t)gpRng->get_cur_rng() : 0;
        printf("net: test phase 4: jack called off at tick %u\n", tick);
        ped->SetObjective(objectives_enum::no_obj_0, 9999);
        ped->SetObjective2(objectives_enum::no_obj_0);
        CancelPhase(ped);
        gNeverPull = false;
        gPullAsks = 0;
        return;
    }
    if (ped->mCar && ped->mCar->mCarSprite && ped->mCar->mDriver == ped && ped->mCar != sCar)
    {
        const Sprite* cs = ped->mCar->mCarSprite;
        ForceOutOfCar(ped, cs->mXy.x + Fix16(1), cs->mXy.y, cs->mZpos, cs->mRotation); // on foot for the next try
        return;
    }
    if (!sVictim && !ped->mCar && ped->mGameObject && InCarTransition(ped))
    {
        ped->SetObjective(objectives_enum::no_obj_0, 9999); // its bot's own way into a car: called off for the jack
        ped->SetObjective2(objectives_enum::no_obj_0);
        CancelPhase(ped);
    }
    if (ped->mCar || !ped->mGameObject || InCarTransition(ped))
    {
        return;
    }
    if (sVictim)
    {
        if (sVictim->mCar == sCar && sCar->mDriver == sVictim && sCar->mCarSprite)
        {
            Fix16 x, y;
            sCar->GetDoorWorldPosition(0, &x, &y); // its bot may have driven off
            MovePed(ped, x, y, sCar->mCarSprite->mZpos, Angle(0));
            StartEntering(ped, sCar);
        }
        return;
    }
    for (int i = PlayerCount() - 1; i >= 0; i--) // the last first: player 0 has the test blasts at its feet
    {
        Ped* victim = InGamePed(i);
        if (victim && victim != ped && !victim->mCar && victim->mGameObject && !victim->isDead())
        {
            Fix16 scale;
            scale.mValue = 0x4000;
            sX = victim->get_cam_x(), sY = victim->get_cam_y(), sZ = victim->get_cam_z();
            Car* car = gCarManager_6C->SpawnCarAt(sX, sY, sZ, 0, car_model_enum::MERC, scale); // where the victim stands: on its ground
            if (car)
            {
                printf("net: test phase 4: player %d jacks player %d\n", player, i);
                car->IncrementCarStats(car_kind::recycled_1);
                ForceIntoCar(victim, car);
                Fix16 x, y;
                car->GetDoorWorldPosition(0, &x, &y);
                MovePed(ped, x, y, sZ, Angle(0));
                StartEntering(ped, car);
                sVictim = victim;
                sCar = car;
                gPullAsks = 0;
                gNeverPull = true;
                gPortMayPullOut = HeldPull;
            }
            return;
        }
    }
}

void net::engine::DebugLocalPhase()
{
    static const char* spec = getenv("GTA2_NET_TESTLOCALPHASE");
    const uint32_t tick = gpRng ? (uint32_t)gpRng->get_cur_rng() : 0;
    for (const char* k = spec; k && *k; k = strchr(k, ','), k = k ? k + 1 : NULL)
    {
        unsigned t = 0;
        int what = 0;
        if (sscanf(k, "%u:%d", &t, &what) == 2 && t == tick)
        {
            StartPhase(InGamePed(LocalPlayer()), what);
        }
    }
}
