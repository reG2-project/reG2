// port/net (port/docs/netcode.md, "The server"): player slots coming into and going out of a running network game. A slot
// without a client is out of the game the way the game handles a player who left (Player::Disconnect:
// its ped walks on as a pedestrian). A slot that gets a client gets a new player ped at the next restart zone,
// as the map script's PLAYER_PED makes one (ScriptThread::Handle_PLAYER_PED) and Player::InitPlayerPed
// sets it up, but with the slot's own weapons kept: InitPlayerPed would allocate a new set each time.
#include "internal.hpp"
#include "Source/Char_Pool.hpp"
#include "Source/Game.hpp"
#include "Source/GameSession.hpp"
#include "Source/Ped.hpp"
#include "Source/Player.hpp"
#include "Source/TagGame.hpp"
#include "Source/Weapon.hpp"
#include "Source/sprite.hpp"
#include "Source/Car.hpp"
#include "Source/map_0x370.hpp"
#include "Source/ped_graphic_type.hpp"
#include <string.h>

namespace
{
// A slot's look + 1 (0: not known yet), from the ped the map script gave it, this game's (gRemapGame)
u8 gRemap[MAX_PLAYERS];
int gRemapGame = -1;

void ForgetLooksOfLastGame()
{
    if (gRemapGame != net::engine::GameGeneration())
    {
        gRemapGame = net::engine::GameGeneration();
        memset(gRemap, 0, sizeof(gRemap));
    }
}
} // namespace

bool net::engine::SlotInGame(int slot)
{
    Player* p = GetPlayer(slot);
    return p && p->mInUse && p->mPlayerPed;
}
// (A slot out of the game keeps its last ped pointer, a pedestrian now, as after Player::Disconnect; only
// slots in the game count anywhere in the netcode)

void net::engine::TakeOutOfGame(int slot)
{
    Player* p = GetPlayer(slot);
    if (!p || !p->mInUse)
    {
        return;
    }
    ForgetLooksOfLastGame();
    if (p->mPlayerPed && slot < MAX_PLAYERS && !gRemap[slot])
    {
        gRemap[slot] = (u8)(p->mPlayerPed->mRemap + 1);
    }
    if (Ped* ped = p->mPlayerPed)
    {
        // Out of its car first: the ped walks on as a pedestrian, a local slot's camera follows it, and a car it sat in
        // can be despawned under it (the sound read the freed car through the camera)
        if (ped->mCar && !InCarTransition(ped))
        {
            ForceOutBesideCar(ped, ped->mCar);
        }
    }
    // The pedestrian it leaves keeps only weapons of its own, as its Deallocate frees them into the pool: not the slot's
    // (the slot keeps them, as Player.cpp clears a corpse clone's), nor a car's (a player ped's current weapon while it
    // drives a car that has one: still on the pool's list of used weapons, which loops once it is allocated again), nor
    // one freed already (its car's, emptied as it got out)
    for (Ped* ped : {p->mPlayerPed, p->mAuxPed})
    {
        for (Weapon** held : {ped ? &ped->mSelectedWeapon : NULL, ped ? &ped->mpWeapon : NULL})
        {
            if (!held || !*held)
            {
                continue;
            }
            bool own = (*held)->mpPed == ped && !(*held)->mCar;
            for (int w = 0; own && w < kPlayerWeapons; w++)
            {
                own = p->mWeapons[w] != *held;
            }
            if (!own)
            {
                *held = NULL;
            }
        }
    }
    if (gTagGame.mFugitive == p)
    {
        // "It" to the nearest player, as when "it" dies with no killer (TagGame::UpdateFugitive); else nobody stays
        // "it" for the rest of the round. Alone: the next player in the game's (TagGame::ExecuteGamemodeTick)
        gTagGame.UpdateFugitive(p, NULL);
        gTagGame.mFugitive = gTagGame.mFugitive == p ? NULL : gTagGame.mFugitive;
    }
    ForgetKillCams(slot); // a new player in the slot isn't the killer
    p->Disconnect(); // keeps its ped pointer, as the game does: the HUD and camera of a local slot use it
}

// No client in this slot at the game's start: the ped the map script made goes, not a pedestrian in a player's look
// standing at a spawn (PutInGame makes a new one). The slot keeps the pointer, as once a departed player's ped despawns:
// the HUD of the server's own slot reads it
void net::engine::NeverInGame(int slot)
{
    TakeOutOfGame(slot);
    Player* p = GetPlayer(slot);
    Ped* ped = p ? p->mPlayerPed : NULL;
    if (ped && ped->IsActive() && !ped->mCar)
    {
        ped->Deallocate();
    }
}

// A death's or an arrest's countdown over, the player's part, as Player::Wasted and Player::Busted end it: else one
// caught counting down stays "wasted" with its ped alive (it no longer dies, so the countdown never runs): it can't
// attack, its deaths no longer count, and its camera stays on the corpse it left
void net::engine::EndDeath(Player* p)
{
    if (!p->mWastedOrBusted && !p->mAuxPed)
    {
        return;
    }
    p->ClearKFWeapon();
    p->mCameraMode = 0;
    p->mGameCamera.SetSmoothFollow();
    if (p->mAuxPed)
    {
        p->mAuxPed->Deallocate();
        p->mAuxPed = NULL;
    }
    p->mAuxCamActive = false;
    p->mWastedOrBusted = false;
    p->mBusted = false;
    p->mAuxPedDying = false;
    p->mDeathCountdown = 0;
}

void net::engine::PutInGame(int slot, bool freshScore)
{
    Player* p = GetPlayer(slot);
    gmp_map_zone* zone = gMap_0x370->GetNextZoneOfTypeRoundRobin(0x10); // a Restart zone, round robin
    if (!p || !zone)
    {
        return;
    }
    if (freshScore)
    {
        // A new player: nothing of the slot's last one, who may have left with a score, deaths, "it" time or armour
        FreshScores(slot);
        p->ClearPowerUps(); // the timers only: a fresh ped, or one the respawn's reset made plain
    }
    if (p->mInUse && p->mPlayerPed)
    {
        // Still in: a fresh start where the others respawn, as the game's death and respawn over at once. The game
        // respawns a ped no car holds (a car's blast kills its driver and clears it, Car::KillContainedPeds): a car
        // kept it as its driver. Only the car lets go: getting out already, it has its char (another would stay in
        // the collision grid). The reset its respawn ends with: else a ped falling or knocked down stays
        // immobilized for good, its walk and its death only stored (ChangeNextPedState1)
        Ped* ped = p->mPlayerPed;
        if (Car* car = ped->mCar)
        {
            if (car->mDriver == ped)
            {
                car->ClearDriver(); // its car weapons unloaded too
            }
            if (ped->mEnterCarAsPassenger)
            {
                car->mPassengersList.RemovePed_471240(ped);
            }
        }
        ped->RespawnPed(zone);
        if (ped->mGameObject)
        {
            ped->ResetForPlayerRespawn();
        }
        EndDeath(p);
        // Its camera there too, not panning over for seconds: until it sees the ped, the ped counts as off screen, and
        // a dead player ped off screen for 200 ticks is freed as a corpse (Ped::PoolUpdate), with the player still
        // pointing at it (a crash at its respawn, or once the memory is another ped)
        SnapCamera(p);
        return;
    }
    const Fix16 x = Fix16(zone->mX + zone->mW / 2), y = Fix16(zone->mY + zone->mH / 2);
    const Fix16 z = gMap_0x370->FindGroundZForCoord(x, y);
    ForgetLooksOfLastGame();
    const u8 remap = slot < MAX_PLAYERS && gRemap[slot] ? (u8)(gRemap[slot] - 1) : 0;
    Ped* ped = gPedManager->SpawnPedAt(x, y, z, remap, Ang16((s16)0, (u8)0));
    if (!ped)
    {
        return;
    }
    ped->SetPedType(ped_type::player_2);
    ped->set_health(100);
    // Player::InitPlayerPed, without new weapons
    p->mWastedOrBusted = 0;
    p->mBusted = 0;
    p->mAuxPedDying = 0;
    p->mDeathCountdown = 0;
    p->mPlayerPed = ped;
    p->mAuxPed = NULL;
    p->mTrafficSpawnCounter = 0;
    p->mTrafficSpawnThreshold = 1000;
    ped->SetPlayer(p, 0);
    p->mCameraMode = 0;
    p->mTurnSpeed = Ang16(0); // a jump's turn (Player::HandleControls): kAngZero_6FE754 and kJumpTurnAccel * 2 + kAng3
    p->mTurnAccel = Ang16(20);
    p->mMoveDirection = Fix16(0);
    p->mJumping = false; // as Player::InitializePlayerState: else a ped that left in the air turns as if jumping
    // Its own weapons; its car's weapons stay its car's, which may be gone by now (one that left getting out kept them)
    for (int w = 0; w < weapon_type::car_bomb; w++)
    {
        if (Weapon* weapon = p->mWeapons[w])
        {
            weapon->set_pPed(ped);
            weapon->mAmmo = freshScore ? 0 : weapon->mAmmo; // a player back after a drop keeps its ammo
        }
    }
    p->ClearCarWeapons();
    p->mCurrWeaponIdx = -1;
    p->mSavedPedWeaponIdx = -1;
    p->mSavedCarWeaponIdx = 27;
    ped->mGraphicType = ped_graphic_type::character_1;
    p->set_in_use(1);
    ped->RespawnPed(zone); // a free spot in the zone, health, a moment of invulnerability
    SnapCamera(p);
    // As Player::InitializePlayerState: else the aux camera of a death the slot left in stays on, following the old ped
    // (a pedestrian, or another ped once freed), and the traffic spawner reads that ped's car
    p->mAuxGameCamera.UpdateFollowPedCamera(ped);
    p->mAuxGameCamera.CommitPendingTarget();
    p->mAuxCamActive = false;
}

// For a slot that joins and every slot of a new game, not only at a round's start: else the server's players have
// the multiplier 0 through the warmup
void net::engine::ResetHudScores(Player* p)
{
    p->mScores.SetMoney(0); // the HUD's money, frags and multiplier as at a game's start
    p->mScores.mMultiplayerFrags.SetValueClamped(0);
    p->mMultipliers.SetValueClamped(1);
}

void net::engine::FreshScores(int slot)
{
    if (slot < MAX_PLAYERS) // the game session's tally outlives a game: the last round's frags otherwise
    {
        gGameSession.mFragsList[slot] = 0;
        gGameSession.mPointsList[slot] = 0;
        gTagGame.mItTimeSecs[slot] = 0; // a tag game's score
        ResetDeaths(slot);
    }
    if (Player* p = GetPlayer(slot))
    {
        ResetHudScores(p);
    }
}
