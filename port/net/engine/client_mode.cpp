// port/net (port/docs/netcode.md, "The client"): a client simulates what moves, not what decides. Spawning,
// despawning, scripts and the emergency services are the server's; the game's own debug switches turn them off,
// so the decomp needs no hooks for it. Network games clear those switches at the start (ClearDebugFlags),
// so they are set again every frame.
#include "internal.hpp"
#include "Source/Game.hpp"
#include "Source/Player.hpp"
#include "Source/Car.hpp"
#include "Source/CarAI.hpp"
#include "Source/CarChaseTaskTable.hpp"
#include "Source/Char_Pool.hpp"
#include "Source/Ped.hpp"
#include "Source/Object_3C.hpp"
#include "Source/Object_5C.hpp"
#include "Source/sprite.hpp"
#include "Source/debug.hpp"
#include "Source/Police.hpp"
#include "Source/ScriptManager.hpp"
#include "Source/ScriptThread.hpp"
#include "Source/Hud.hpp"
#include "Source/GameSession.hpp"
#include "port/src/net_hooks.hpp"
#include <set>

void net::engine::ProtectPlayers()
{
    for (int i = 0; i < PlayerCount(); i++)
    {
        Ped* ped = InGamePed(i);
        // The car a player sits in or is getting into stays: one marked for removal before (it wasn't the server's,
        // or not yet) would otherwise go with the player in it, as Ped::SpawnPedInCar knows
        Car* cars[2] = {ped ? ped->mCar : NULL, ped ? ped->mTargetObjectiveCar : NULL};
        for (int k = 0; k < 2; k++)
        {
            if (cars[k] && cars[k]->mDespawnStatus != car_despawn_status::active_1)
            {
                cars[k]->mDespawnStatus = car_despawn_status::active_1;
            }
        }
    }
    GuardFatalPhases();
}

void net::engine::GuardFatalPhases()
{
    for (Ped* p = gPedPool->mPool.mpPrev; p; p = p->mpNext)
    {
        if (IsServerPed(p)) // this game's own die as they do: no snapshot would ever end their hold
        {
            GuardPhases(p);
        }
    }
}

namespace
{
// The map script's commands a client runs itself: the control of its level start, and what only shows or sounds here.
// The rest makes or changes what the snapshots carry (cars, their weapons and flags, generators, cranes)
bool ClientScriptCommandRuns(u16 type)
{
    switch (type)
    {
        case SCRCMD_LEVELSTART:
        case SCRCMD_LEVELEND:
        case SCRCMD_START_EXEC:
        case SCRCMD_STOP_EXEC:
        case SCRCMD_SET_SHADING_LEV:
        case SCRCMD_SET_AMBIENT:
        case SCRCMD_CREATE_SOUND:
        case SCRCMD_PUT_CAR_ON_TRAILER: // the level start's cars on car transporters, declared on both sides
            return true;
        default:
            return false;
    }
}

bool LevelStartRunning()
{
    for (ScriptThread* t = miss2_0x11C_Pool ? miss2_0x11C_Pool->mPool.mpPrev : NULL; t; t = t->mpNext)
    {
        const SCR_CMD_HEADER* next = gScriptManager->GetBasePointer(t->mNextCmd);
        if (t->mInLevelStart || (next && next->mType == SCRCMD_LEVELSTART && t->mStopped != 1))
        {
            return true;
        }
    }
    return false;
}
} // namespace

void net::engine::ClientModeFrame()
{
    ProtectPlayers();
    GuardPeds();
    KeepPlayerArrows(); // in the tick: its saves have it, so a rollback doesn't take it back
    bSkip_recycling = true; // no traffic generation, no car despawning (TrafficSpawner::GenerateTraffic)
    bSkip_dummies = true; // no ambient peds (PedManager::Dummies)
    // Scripts run on the server, but for their level start's look and sound here (ClientScriptCommandRuns), until it ends
    bSkip_mission = !LevelStartRunning();
    gPortScriptCommandRuns = ClientScriptCommandRuns;
    bSkip_police = true;
    bSkip_fire_engines = true; // the server's fire trucks arrive as cars; this game's own would never be removed
    gPortNoOwnBus = true; // the server's bus arrives as a car
    gPortServerDespawns = true;
    // With the police off, every ped tick zeroes the wanted points (Ped::PoolUpdate): no cop heads, and the cops chasing a
    // player gave up between snapshots
    gPortKeepWantedPoints = true; // the server's (players' entities, peds' AI state)
    gPortServerKillNotices = true;
    gPortServerKillAwards = true;
    gPortProtectsPlayers = true;
    bSkip_ambulance = true;
    gPortCarMayExplode = ClientCarMayExplode;
    gPortPedMayDie = ClientPedMayDie;
    gPortPedMayFire = ClientPedMayFire;
    gPortPedMayGo = ClientPedMayGo;
}

