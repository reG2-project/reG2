// port/net (port/docs/netcode.md, "The client"): a snapshot's cars and peds in a client's game. Each server entity gets a
// local one: adopted if the client's game already has the same one (the map script made it on both sides at the
// start), else created; moved to the server's state every snapshot; removed when the server no longer sends it.
// Local cars and peds the server doesn't know (other than the players' own) are removed.
#include "internal.hpp"
#include "dev/dev.hpp"
#include "port/src/interpolation.hpp"
#include "port/src/rand/owner.hpp"
#include "Source/Car.hpp"
#include "Source/Char_Pool.hpp"
#include "Source/Game.hpp"
#include "Source/Ped.hpp"
#include "Source/Player.hpp"
#include "Source/char.hpp"
#include "Source/sprite.hpp"
#include <algorithm>
#include <stdio.h>
#include <set>
#include <unordered_map>
#include <vector>

struct net::engine::ReplicaState
{
    std::unordered_map<uint32_t, Car*> cars;
    std::unordered_map<uint32_t, Ped*> peds;
    std::unordered_map<uint32_t, int32_t> carIds, pedIds;
    std::unordered_map<uint32_t, Object*> objects;
};

namespace
{
using namespace net::engine;
using net::EntityState;
using net::Snapshot;

std::unordered_map<uint32_t, Car*> gCars; // NetId -> this game's car
std::unordered_map<uint32_t, Ped*> gPeds;
// The game's own id of each mapped car and ped (Car::mCarId, Ped::mId): a pool slot this game freed and handed out again
// since is another car or ped, and its mapping goes (Prune)
std::unordered_map<uint32_t, int32_t> gCarIds, gPedIds;
void MakeKeys(); // the reverse maps (the random draws' owners)
const int32_t kAdoptRadius = 4096; // a quarter block: the same car or ped as the server's
// Free slots below which this game's own cars and peds beyond the reach go (EvictOwn), so that the server's have room
const int kReserve = 32;

// For the log (PrintReplicaLog)
int gRefusedPeds[3]; // why: no ped, no char, no sprite
int gRefusedCars;
int gEvicted;
int gOccupiedKept;
int gPedsRemade;
int gPedsKept; // deallocations of the server's peds refused (ClientPedMayGo)

// Room to create a car or a ped without running a pool dry (Pool::Allocate doesn't check, and SpawnPedAt leaks its
// ped when no char is left): a margin for the game's own allocations
bool RoomForCar()
{
    const bool room = FreeSlots(gCar_BC_Pool->mPool.mpStart) > 16 && FreeSlots(gSpritePool->mPool.mpHead) > 32;
    gRefusedCars += !room;
    return room;
}

bool RoomForPed()
{
    const bool ped = FreeSlots(gPedPool->mPool.mpStart) > 16;
    const bool chr = FreeSlots(gChar_B4_Pool->mPool.mpHead) > 16;
    const bool spr = FreeSlots(gSpritePool->mPool.mpHead) > 32;
    gRefusedPeds[0] += !ped;
    gRefusedPeds[1] += !chr;
    gRefusedPeds[2] += !spr;
    return ped && chr && spr;
}

// What one apply of a snapshot works with
struct Pass
{
    std::set<Car*> liveCars; // this game's live cars and peds; a mapped one the game removed by itself (a wreck, a death, or a
    std::set<Ped*> livePeds; // resimulation since the map was saved) is forgotten
    // The players' peds, the cars they sit in and their corpses (Player::Wasted leaves a clone, the aux ped): the
    // players' own (players.cpp), never adopted or removed here
    std::set<const void*> owned;
    std::set<const void*> mapped; // owned, or a snapshot entity's
    std::set<const Car*> playerIn; // the cars players sit in (not the ones they are getting into: those stay the server's)
    std::set<Car*> referenced; // cars a live ped points to
    std::set<uint32_t> seen; // the snapshot's cars and peds
};

void Gather(Pass& a)
{
    for (Car* c = gCar_BC_Pool->mPool.mpPrev; c; c = c->mpNext)
    {
        a.liveCars.insert(c);
    }
    std::vector<Car*> carriages; // a train's wagons aren't in the pool's list
    TrainCars(carriages);
    a.liveCars.insert(carriages.begin(), carriages.end());
    for (Car* c = gCar_BC_Pool->mPool.mpPrev; c; c = c->mpNext)
    {
        if (Car* trailer = TowedTrailer(c)) // nor a towed trailer (Car::AttachTrailer)
        {
            a.liveCars.insert(trailer);
        }
    }
    for (Ped* p = gPedPool->mPool.mpPrev; p; p = p->mpNext)
    {
        a.livePeds.insert(p);
        if (p->mCar && !IsSpareDriver(p))
        {
            a.referenced.insert(p->mCar);
        }
    }
    for (int i = 0; i < PlayerCount(); i++)
    {
        Player* player = GetPlayer(i);
        if (!player || !player->mInUse)
        {
            continue; // out of the game: its last ped is a pedestrian like any other
        }
        Ped* peds[2] = {player->mPlayerPed, player->mAuxPed};
        for (int k = 0; k < 2; k++)
        {
            if (peds[k])
            {
                a.owned.insert(peds[k]);
                if (peds[k]->mCar)
                {
                    a.owned.insert(peds[k]->mCar);
                }
                if (peds[k]->mTargetObjectiveCar) // the car it is getting into
                {
                    a.owned.insert(peds[k]->mTargetObjectiveCar);
                }
            }
        }
        if (player->mPlayerPed && player->mPlayerPed->mCar)
        {
            a.playerIn.insert(player->mPlayerPed->mCar);
        }
    }
    a.mapped = a.owned;
}

// The maps without what the game removed since
void Prune(Pass& a)
{
    for (auto it = gCars.begin(); it != gCars.end();)
    {
        auto id = gCarIds.find(it->first);
        const bool same = id == gCarIds.end() || id->second == it->second->mCarId;
        it = same && a.liveCars.count(it->second) && !it->second->IsDespawning() ? (a.mapped.insert(it->second), ++it) : gCars.erase(it);
    }
    for (auto it = gPeds.begin(); it != gPeds.end();)
    {
        // A player's ped is never a snapshot ped's: one mapped before it was reused for a player's (the pool hands the
        // slot out again) would have the server's ped steer the player (out of its car, the car left driverless)
        // (a passenger isn't "active" either, as a driver isn't, but no car re-adopts it: kept while it sits in one)
        Ped* p = it->second;
        const bool riding = p->mCar && p->mEnterCarAsPassenger;
        auto id = gPedIds.find(it->first);
        const bool same = id == gPedIds.end() || id->second == p->mId;
        it = same && a.livePeds.count(p) && (p->IsActive() || riding) && !a.owned.count(p) ? (a.mapped.insert(p), ++it) : gPeds.erase(it);
    }
    for (auto it = gCarIds.begin(); it != gCarIds.end();)
    {
        it = gCars.count(it->first) ? ++it : gCarIds.erase(it);
    }
    for (auto it = gPedIds.begin(); it != gPedIds.end();)
    {
        it = gPeds.count(it->first) ? ++it : gPedIds.erase(it);
    }
}

// A car goes, unless a ped points to it (its driver, a passenger, a corpse cloned in it, Player::Wasted): that
// ped would point at a removed car (the sound listener's camera follows a player's corpse and reads its car,
// sound_obj::InterrogateAudioEntities)
// A ped here going for it (to get in, after it, at its door) keeps it too: freed, the ped read it on (its sprite,
// Char::UpdateEnteringCar, a crash). It goes on a later snapshot, once the server's ped is about something else
// The shared traffic driver doesn't keep it: the game leaves that one pointing at a freed car itself
// (KillContainedPeds spares a dummy driver). Kept, a car the server dropped drove on here, into the server's cars
// Its char's target car keeps it only while the char goes by it (getting in or out, Char::UpdateEnteringCar to UpdateEnteringTrain,
// UpdateImmobilized, sitting in it): the game never clears it, and a ped long out of the car (a player too) kept it here for good
bool CharUsesTarget(const Ped* p)
{
    const s32 s = p->mPedState1;
    return s == ped_state_1::entering_car_3 || s == ped_state_1::exiting_car_4 || s == ped_state_1::entering_train_5 ||
        s == ped_state_1::immobilized_8 || s == ped_state_1::in_car_10;
}

bool TargetedHere(Car* car)
{
    for (Ped* p = gPedPool->mPool.mpPrev; p; p = p->mpNext)
    {
        Char* c = p->mGameObject;
        const bool going = !p->isDead(); // a corpse's objective car is a leftover too
        if (!IsSpareDriver(p) && ((going && (p->mTargetObjectiveCar == car || p->mTargetToEnter == car)) || p->mCar == car ||
                                  (c && c->mTargetCar == car && CharUsesTarget(p))))
        {
            return true;
        }
    }
    return false;
}

bool RemoveCar(const Pass& a, Car* car)
{
    if (car->IsTrainModel())
    {
        return false; // a train's carriage: its train points to it, and the server's train is mapped to it (ApplyTrains)
    }
    if ((car->mDriver && !IsSpareDriver(car->mDriver)) || a.referenced.count(car) || TargetedHere(car))
    {
        gOccupiedKept++;
        return false;
    }
    car->DespawnSoon();
    return true;
}

bool gFreeing; // in Free

// A server's ped this game lets go (gPortPedMayGo)
void Free(Ped* p)
{
    gFreeing = true;
    p->Deallocate();
    gFreeing = false;
}

// A ped goes, once (Deallocate undoes counters and groups, and marks it inactive)
void RemovePed(Ped* p)
{
    if (p->mCar && p->mCar->mDriver != p && !p->mEnterCarAsPassenger)
    {
        p->mCar = NULL; // not that car's driver (any more): Deallocate would clear whoever is (a player)
    }
    if (p->IsActive() && !p->mCar)
    {
        Free(p);
    }
}

// This game's car for a snapshot car: mapped, adopted (the same car here) or made
Car* MapCar(Pass& a, const EntityState& e)
{
    auto it = gCars.find(e.id);
    Car* car = it != gCars.end() ? it->second : NULL;
    if (car && car->mCarInfoIdx != e.f[kCarModel])
    {
        RemoveCar(a, car); // another model under this id: should not happen, ids aren't reused
        gCars.erase(it);
        car = NULL;
    }
    if (car || IsTrainModel(e.f[kCarModel]))
    {
        return car; // a train's carriage only by its train (ApplyTrains): one made here would have no train
    }
    for (Car* c : a.liveCars)
    {
        if (!a.mapped.count(c) && c->mCarSprite && !c->IsDespawning() && c->mCarInfoIdx == e.f[kCarModel] &&
            Near(c->mCarSprite, e.f[kCarX], e.f[kCarY], kAdoptRadius))
        {
            car = c;
            car->CancelDespawn(); // one removed a snapshot ago, sent again: the same car, not a second one on it
            break;
        }
    }
    // A model the style has (SpawnCarAt reads its car_info) and can wreck, as all the server's cars are
    if (!car && RoomForCar() && CarModelWrecks(e.f[kCarModel]))
    {
        car = gCarManager_6C->SpawnCarAt(Raw(e.f[kCarX]), Raw(e.f[kCarY]), Raw(e.f[kCarZ]), Angle(e.f[kCarRot]), e.f[kCarModel], Fix16(1));
    }
    if (car)
    {
        gCars[e.id] = car;
        gCarIds[e.id] = car->mCarId;
        a.mapped.insert(car);
    }
    return car;
}

void ApplyCar(Pass& a, const EntityState& e)
{
    a.seen.insert(e.id);
    const bool fresh = !gCars.count(e.id); // made or adopted below: never seen alive under this id
    Car* car = MapCar(a, e);
    if (!car || EntityAge(e))
    {
        return; // none, or the state the client already has (a busy area's far car): predicted on here
    }
    // The shared traffic driver, as the server has it (a car with its own driver gets it from its ped entity; a train's
    // lead from its train, ApplyTrains)
    if (!a.owned.count(car) && !car->IsTrainModel())
    {
        const bool spare = IsSpareDriver(car->mDriver);
        if (e.f[kCarDriver] == kDriverTraffic && !car->mDriver)
        {
            car->SpawnDriverPed();
        }
        else if (e.f[kCarDriver] != kDriverTraffic && spare)
        {
            car->SetDriver(NULL);
        }
    }
    // A car a player sits in follows the player (players.cpp), a train's its train; one being entered, the server
    if (!a.playerIn.count(car) || car->IsTrainModel())
    {
        const GridCheck check("moving a car", car);
        MoveCar(car, Raw(e.f[kCarX]), Raw(e.f[kCarY]), Raw(e.f[kCarZ]), Angle(e.f[kCarRot]));
    }
    SetCarPhysics(car, e); // a player's car too: its place came with the player
    ApplyCarAi(car, e);
    const bool wreckHereOnly = !SetCarState(car, e, fresh);
    if (wreckHereOnly && a.playerIn.count(car) && !a.referenced.count(car) && !car->IsTrainModel())
    {
        // A player in it gets out first: driven on, the wreck has lost its roof turret (Car::HandleRoofTurretRotation
        // finds none). The car goes below, the next snapshot makes it again and puts the player back in (players.cpp)
        for (int i = 0; i < PlayerCount(); i++)
        {
            Ped* ped = InGamePed(i);
            if (ped && ped->mCar == car && !ped->isDead() && !InCarTransition(ped))
            {
                ForceOutBesideCar(ped, car);
            }
        }
    }
    if (wreckHereOnly && (!a.playerIn.count(car) || !car->mDriver) && !a.referenced.count(car) && !car->IsTrainModel())
    {
        // A wreck here that isn't one on the server can't be unexploded: it goes, and the next snapshot makes the car
        // again as the server has it
        RemoveCar(a, car);
        gCars.erase(e.id);
        a.mapped.erase(car);
        gWrecksReplaced++;
    }
}

// This game's ped for a snapshot ped: mapped, adopted (its car's driver, or the same ped here) or made
Ped* MapPed(Pass& a, const EntityState& e, Car* serverCar)
{
    auto it = gPeds.find(e.id);
    Ped* ped = it != gPeds.end() ? it->second : NULL;
    if (!ped && serverCar && PassengerThere(e))
    {
        // A passenger: made at its car (ApplyPed seats it)
        if (Sprite* cs = serverCar->mCarSprite; cs && RoomForPed())
        {
            ped = gPedManager->SpawnPedAt(cs->mXy.x, cs->mXy.y, cs->mZpos, (u8)e.f[kPedRemap], cs->mRotation);
        }
    }
    else if (!ped && serverCar)
    {
        // Its car's driver here, if nobody's (the map script or the game made it), else a new one
        Ped* d = serverCar->mDriver;
        if (d && a.livePeds.count(d) && !a.mapped.count(d) && !IsSpareDriver(d))
        {
            ped = d;
        }
        else if ((!d || IsSpareDriver(d)) && RoomForPed())
        {
            serverCar->SetDriver(NULL);
            ped = gPedManager->SpawnDriver(serverCar);
            SetPedKind(ped, e);
        }
    }
    if (!ped && !serverCar)
    {
        for (Ped* p : a.livePeds)
        {
            Sprite* s = CharSprite(p);
            if (!a.mapped.count(p) && s && !p->mCar && p->IsActive() && (u8)p->mRemap == (u8)e.f[kPedRemap] &&
                Near(s, e.f[kPedX], e.f[kPedY], kAdoptRadius))
            {
                ped = p;
                break;
            }
        }
        if (!ped && RoomForPed())
        {
            ped = gPedManager->SpawnPedAt(Raw(e.f[kPedX]), Raw(e.f[kPedY]), Raw(e.f[kPedZ]), (u8)e.f[kPedRemap], Angle(e.f[kPedRot]));
        }
    }
    if (ped && !gPeds.count(e.id))
    {
        gPeds[e.id] = ped;
        gPedIds[e.id] = ped->mId;
        a.mapped.insert(ped);
        ped->mRemap = (char_type)e.f[kPedRemap] ^ 1; // set below, with its char's
    }
    return ped;
}

void ApplyPed(Pass& a, const EntityState& e)
{
    // A driver needs its car here first (cars come before peds: their NetIds are smaller)
    Car* serverCar = RefCar(e.f[kPedCar]);
    if (e.f[kPedCar] && (!serverCar || !a.liveCars.count(serverCar)))
    {
        return;
    }
    a.seen.insert(e.id);
    Ped* ped = MapPed(a, e, serverCar);
    if (!ped || EntityAge(e))
    {
        return; // none, or the state the client already has: predicted on here
    }
    // Its look: the remap and its char's colours (a ped made here, or adopted, had its own: a gang member's showed
    // another gang's)
    if ((u8)ped->mRemap != (u8)e.f[kPedRemap])
    {
        ped->mRemap = (char_type)e.f[kPedRemap];
        if (ped->mGameObject)
        {
            ped->SetRemap((u8)e.f[kPedRemap]);
        }
    }
    // In the server's car, or on foot, as the server has it (when neither side is getting in or out)
    const bool settled = !e.f[kPedState] && !InCarTransition(ped) && !ped->isDead();
    if (serverCar && PassengerThere(e))
    {
        // A passenger there (into a player's car too: an instant gang rides with its leader)
        if (!IsPassengerOf(ped, serverCar) && settled)
        {
            ForceIntoCarAsPassenger(ped, serverCar);
        }
    }
    else if (serverCar)
    {
        // (or its driver here already, but not sitting in it: a police car's driver walked, Police off here)
        if ((serverCar->mDriver != ped || ped->mPedState1 != ped_state_1::in_car_10) && settled && !a.owned.count(serverCar))
        {
            ForceIntoCar(ped, serverCar);
        }
    }
    else if (ped->mCar)
    {
        ApplyPhases(ped, e, kPedPhase, false, NULL); // pulled out there: at its car's door here
        if (settled && ped->mCar)
        {
            ForceOutOfCar(ped, Raw(e.f[kPedX]), Raw(e.f[kPedY]), Raw(e.f[kPedZ]), Angle(e.f[kPedRot]));
        }
    }
    else if (ShownDeathAge(ped) < 0) // shown dead here, it lies where it fell until the server's death
    {
        MovePed(ped, Raw(e.f[kPedX]), Raw(e.f[kPedY]), Raw(e.f[kPedZ]), Angle(e.f[kPedRot]));
    }
    if (!ApplyPedState(ped, e))
    {
        // A corpse of this game's own (a death predicted that the server didn't have): it goes, and the next snapshot
        // makes the ped again
        RemovePed(ped);
        gPeds.erase(e.id);
        a.mapped.erase(ped);
        gPedsRemade++;
    }
}

// The snapshot's trains into this game's, and their carriages mapped to the server's car ids (a carriage is mapped only
// here, so before the cars). A carriage mapped under another id before is that id's no longer
void ApplyTrains(Pass& a, const Snapshot& snap)
{
    std::vector<std::pair<uint32_t, Car*>> carriages;
    for (const EntityState& e : snap.entities)
    {
        if (e.type != kEntityTrain || !ApplyTrain(e, carriages))
        {
            continue;
        }
        for (const auto& c : carriages)
        {
            for (auto it = gCars.begin(); it != gCars.end();)
            {
                it = it->second == c.second && it->first != c.first ? gCars.erase(it) : ++it;
            }
            gCars[c.first] = c.second;
            gCarIds[c.first] = c.second->mCarId;
            a.mapped.insert(c.second);
            a.seen.insert(c.first);
        }
    }
}

// Groups (a gang following its leader) once every ped of the snapshot is here
void ApplyGroups(const Snapshot& snap)
{
    std::vector<std::pair<Ped*, const EntityState*>> groupPeds;
    for (const EntityState& e : snap.entities)
    {
        auto it = e.type == kEntityPed ? gPeds.find(e.id) : gPeds.end();
        if (it != gPeds.end() && (it->second->IsActive() || (it->second->mCar && it->second->mEnterCarAsPassenger)))
        {
            groupPeds.push_back(std::make_pair(it->second, &e));
        }
    }
    ApplyPedGroups(groupPeds);
}

// A server's car or ped lives here as long as it is in the newest snapshot: one left out is gone on the server or out of
// this client's area, and either way nothing more will come for it (port/docs/netcode.md, "The client's world"). It
// goes, unless this game still holds on to it (RemoveCar): mapped still, it goes on a later snapshot. This game's own
// cars and peds nobody mapped go inside the reach (the server has none there)
void RemoveUnsent(const Pass& a, const Snapshot& snap)
{
    const ReachArea area(snap, 1.0f);
    for (auto it = gCars.begin(); it != gCars.end();)
    {
        if (a.seen.count(it->first) || !RemoveCar(a, it->second))
        {
            ++it;
            continue;
        }
        it = gCars.erase(it);
    }
    for (auto it = gPeds.begin(); it != gPeds.end();)
    {
        if (a.seen.count(it->first))
        {
            ++it;
            continue;
        }
        Ped* q = it->second;
        if (q->mCar && q->mEnterCarAsPassenger && !q->IsActive())
        {
            // A passenger the server doesn't have: out of its car's list (Deallocate leaves it there), then gone
            q->mCar->mPassengersList.RemovePed_471240(q);
            q->mEnterCarAsPassenger = 0;
            q->mCar = NULL;
            Free(q);
        }
        else if (it->second->IsActive())
        {
            Ped* p = it->second;
            if (p->mCar && p->mCar->mDriver != p && !p->mEnterCarAsPassenger)
            {
                p->mCar = NULL; // not that car's driver (any more): Deallocate would clear whoever is (a player)
            }
            Free(p);
        }
        it = gPeds.erase(it);
    }
    // Beyond it the snapshot says nothing, and one there (a map script's parked car) is adopted when the server's comes
    // in reach (MapCar), or goes when room is short (EvictOwn)
    for (Car* c : a.liveCars)
    {
        if (!a.owned.count(c) && !a.mapped.count(c) && !c->IsDespawning() && !area.Beyond(c->mCarSprite))
        {
            RemoveCar(a, c);
        }
    }
    for (Ped* p : a.livePeds)
    {
        // Drivers are sent too; passengers (not yet) stay with their car
        const bool passenger = p->mCar && p->mCar->mDriver != p;
        if (!a.owned.count(p) && !a.mapped.count(p) && !passenger && !IsSpareDriver(p) && (p->mGameObject || p->mCar) &&
            p->IsActive() && !area.Beyond(PedSprite(p)))
        {
            p->Deallocate();
        }
    }
}

// This game's own cars and peds beyond the reach are a guess at what the server has there, kept for adopting: when a
// pool runs short they go first, farthest first, so that a server's entity never waits for room. Their slots come back
// after the game's despawn (a car) or at once (a ped)
void EvictOwn(const Pass& a, const Snapshot& snap)
{
    int cars = FreeSlots(gCar_BC_Pool->mPool.mpStart), sprites = FreeSlots(gSpritePool->mPool.mpHead);
    int peds = std::min(FreeSlots(gPedPool->mPool.mpStart), FreeSlots(gChar_B4_Pool->mPool.mpHead));
    const int kSprites = kReserve + 16; // above the 32 RoomForCar and RoomForPed leave
    if (cars >= kReserve && peds >= kReserve && sprites >= kSprites)
    {
        return;
    }
    const ReachArea area(snap, 1.0f);
    std::vector<std::pair<float, Car*>> ownCars;
    for (Car* c : a.liveCars)
    {
        if (!a.owned.count(c) && !a.mapped.count(c) && !c->IsDespawning() && !c->IsTrainModel() && area.Beyond(c->mCarSprite))
        {
            ownCars.push_back(std::make_pair(area.DistanceTo(c->mCarSprite), c));
        }
    }
    std::vector<std::pair<float, Ped*>> ownPeds;
    for (Ped* p : a.livePeds)
    {
        Sprite* s = CharSprite(p);
        if (!a.owned.count(p) && !a.mapped.count(p) && !p->mCar && !IsSpareDriver(p) && p->IsActive() && s && area.Beyond(s))
        {
            ownPeds.push_back(std::make_pair(area.DistanceTo(s), p));
        }
    }
    std::sort(ownCars.begin(), ownCars.end(), [](const std::pair<float, Car*>& x, const std::pair<float, Car*>& y) { return x.first > y.first; });
    std::sort(ownPeds.begin(), ownPeds.end(), [](const std::pair<float, Ped*>& x, const std::pair<float, Ped*>& y) { return x.first > y.first; });
    for (size_t k = 0; k < ownCars.size() && (cars < kReserve || sprites < kSprites); k++)
    {
        if (RemoveCar(a, ownCars[k].second))
        {
            cars++;
            sprites++;
            gEvicted++;
        }
    }
    for (size_t k = 0; k < ownPeds.size() && (peds < kReserve || sprites < kSprites); k++)
    {
        ownPeds[k].second->Deallocate();
        peds++;
        sprites++;
        gEvicted++;
    }
}
} // namespace

void net::engine::ApplyCarsAndPeds(const Snapshot& snap)
{
    Pass a;
    Gather(a);
    Prune(a);
    ApplyTrains(a, snap);
    for (const EntityState& e : snap.entities)
    {
        if (e.type == kEntityCar)
        {
            ApplyCar(a, e);
        }
        else if (e.type == kEntityPed)
        {
            ApplyPed(a, e);
        }
    }
    ApplyGroups(snap);
    for (const EntityState& e : snap.entities) // once both cars are here
    {
        Car* cab = e.type == kEntityCar && !EntityAge(e) ? ReplicaCar(e.id) : NULL;
        if (cab && !cab->IsTrainModel())
        {
            ApplyTrailer(cab, e);
        }
    }
    RemoveUnsent(a, snap);
    EvictOwn(a, snap);
}

void net::engine::PrintReplicaLog()
{
    PrintTrainLog();
    if (gExplosionsCarried + gWrecksReplaced + gWrecksMade)
    {
        printf("net: %d explosions the server had carried out, %d wrecks of this game's own made again, %d of the server's made as wrecks\n",
               gExplosionsCarried, gWrecksReplaced, gWrecksMade);
        gExplosionsCarried = gWrecksReplaced = gWrecksMade = 0;
    }
    const CarStateDiffs& d = gCarStateDiffs;
    if (d.damage + d.fireAbove + d.fireBelow + d.wreckHereOnly + d.wreckThereOnly)
    {
        printf("net: cars against the snapshots: %d compared, damage differed %d, fire here above the server's %d, below %d, "
               "wrecked here only %d, there only %d\n", d.cars, d.damage, d.fireAbove, d.fireBelow, d.wreckHereOnly, d.wreckThereOnly);
    }
    gCarStateDiffs = CarStateDiffs();
    if (gPickupsMade + gPickupsRemoved)
    {
        printf("net: %d pickups made as the server's, %d removed\n", gPickupsMade, gPickupsRemoved);
        gPickupsMade = gPickupsRemoved = 0;
    }
    if (gShotsMade + gShotsRemoved + gShotsBeyond)
    {
        printf("net: shots against the snapshots: %d kept, %d made as the server's, %d removed, %d beyond the reach\n", gShotsKept, gShotsMade,
               gShotsRemoved, gShotsBeyond);
    }
    gShotsKept = gShotsMade = gShotsRemoved = gShotsBeyond = 0;
    if (gPedsRemade)
    {
        printf("net: %d corpses of this game's own made again\n", gPedsRemade);
        gPedsRemade = 0;
    }
    if (gPedsKept)
    {
        printf("net: %d removals of the server's peds by this game refused\n", gPedsKept);
        gPedsKept = 0;
    }
    if (gPedsRevived)
    {
        printf("net: %d corpses of this game's alive on the server, revived\n", gPedsRevived);
        gPedsRevived = 0;
    }
    if (gDeathsHeld)
    {
        printf("net: %d deaths of this game's own held for the server's\n", gDeathsHeld);
        gDeathsHeld = 0;
    }
    if (gSoundsGiven)
    {
        printf("net: %d sounds of the resimulated ticks played\n", gSoundsGiven);
        gSoundsGiven = 0;
    }
    if (gOccupiedKept)
    {
        printf("net: %d removals of occupied cars skipped\n", gOccupiedKept);
        gOccupiedKept = 0;
    }
    if (gRefusedCars + gEvicted)
    {
        printf("net: %d of the server's cars waited for room (free: %d cars, %d sprites); %d cars and peds of this game's own "
               "evicted beyond the reach\n",
               gRefusedCars, FreeSlots(gCar_BC_Pool->mPool.mpStart), FreeSlots(gSpritePool->mPool.mpHead), gEvicted);
        gRefusedCars = gEvicted = 0;
    }
    if (gRefusedPeds[0] + gRefusedPeds[1] + gRefusedPeds[2])
    {
        printf("net: peds not created: %d for want of a ped, %d of a char, %d of a sprite (free: %d peds, %d chars, %d sprites)\n", gRefusedPeds[0],
               gRefusedPeds[1], gRefusedPeds[2], FreeSlots(gPedPool->mPool.mpStart),
               FreeSlots(gChar_B4_Pool->mPool.mpHead), FreeSlots(gSpritePool->mPool.mpHead));
        gRefusedPeds[0] = gRefusedPeds[1] = gRefusedPeds[2] = 0;
    }
}

net::engine::ReplicaState* net::engine::SaveReplicas(ReplicaState* reuse)
{
    ReplicaState* state = reuse ? reuse : new ReplicaState();
    state->cars = gCars;
    state->peds = gPeds;
    state->carIds = gCarIds;
    state->pedIds = gPedIds;
    state->objects = PickupMap();
    return state;
}

void net::engine::RestoreReplicas(const ReplicaState* state)
{
    gCars = state->cars;
    gPeds = state->peds;
    gCarIds = state->carIds;
    gPedIds = state->pedIds;
    PickupMap() = state->objects;
    MakeKeys();
}

void net::engine::FreeReplicas(ReplicaState* state)
{
    delete state;
}

Ped* net::engine::ReplicaPed(uint32_t netId)
{
    auto it = gPeds.find(netId);
    return it != gPeds.end() ? it->second : NULL;
}

Car* net::engine::ReplicaCar(uint32_t netId)
{
    auto it = gCars.find(netId);
    return it != gCars.end() ? it->second : NULL;
}

// A new game (the next map): the old game's cars and peds are gone, and their pool slots are the new game's
void net::engine::ClearReplicas()
{
    gCars.clear();
    gPeds.clear();
    gCarIds.clear();
    gPedIds.clear();
}

// --- The owners of random draws (port/src/rand/owner.hpp): an entity's key is the server's net id ---

namespace
{
std::unordered_map<const Ped*, uint32_t> gPedKeys; // a client's: this game's ped -> the server's id, made each tick
std::unordered_map<const Car*, uint32_t> gCarKeys;
// A client's own peds and cars (the server has none of them) own their draws too: the game's stream is the server's
const uint32_t kOwnPedKey = 0x7C000000, kOwnCarKey = 0x7D000000;

// Made again every tick and restore (a few hundred entries); the maps change with every apply and restore
void MakeKeys()
{
    gPedKeys.clear();
    gCarKeys.clear();
    for (const auto& kv : gPeds)
    {
        gPedKeys[kv.second] = kv.first;
    }
    for (const auto& kv : gCars)
    {
        gCarKeys[kv.second] = kv.first;
    }
}

unsigned int ServerPedKey(const Ped* p)
{
    return p->mPlayer ? 1 + p->mPlayer->mPlayerIdx : net::engine::kPedIdBase + p->mId;
}

unsigned int ServerCarKey(const Car* c)
{
    return net::engine::kCarIdBase + c->mCarId;
}

unsigned int ClientPedKey(const Ped* p)
{
    if (p->mPlayer)
    {
        return 1 + p->mPlayer->mPlayerIdx;
    }
    auto it = gPedKeys.find(p);
    return it != gPedKeys.end() ? it->second : kOwnPedKey + p->mId; // one of this game's own: not the game's stream either
}

unsigned int ClientCarKey(const Car* c)
{
    auto it = gCarKeys.find(c);
    return it != gCarKeys.end() ? it->second : kOwnCarKey + c->mCarId;
}

void ClientTickStart()
{
    MakeKeys();
    ProtectPlayers(); // before every tick, live or resimulated: a frame may run several
    DebugGhostShot();
    DebugLocalPhase();
    ShotVisTick();
}
} // namespace

void net::engine::SetRandOwners(bool client)
{
    PortRand::gOwned = true;
    PortRand::gPedKey = client ? ClientPedKey : ServerPedKey;
    PortRand::gCarKey = client ? ClientCarKey : ServerCarKey;
    PortRand::gTickHook = client ? ClientTickStart : NULL;
    InstallShots(true, client);
}

void net::engine::ClearRandOwners()
{
    PortRand::gOwned = false;
    PortRand::gPedKey = NULL;
    PortRand::gCarKey = NULL;
    PortRand::gTickHook = NULL;
    InstallShots(false, false);
}

const std::unordered_map<uint32_t, Car*>& net::engine::ReplicaCars()
{
    return gCars;
}

const std::unordered_map<uint32_t, Ped*>& net::engine::ReplicaPeds()
{
    return gPeds;
}

bool net::engine::ClientPedMayGo(Ped* ped)
{
    if (gFreeing || ped->mCar || InServerDeath())
    {
        return true; // in a car, it goes with its car (Car::KillContainedPeds), whose state is the server's
    }
    if (IsServerPed(ped))
    {
        gPedsKept++;
        return false;
    }
    return true;
}

namespace
{
// The mapped ones still in this game (the maps are pruned only when a snapshot is applied)
template <class F> void ForLiveReplicas(F f)
{
    std::set<Car*> cars;
    std::set<Ped*> peds;
    for (Car* c = gCar_BC_Pool->mPool.mpPrev; c; c = c->mpNext)
    {
        cars.insert(c);
    }
    for (Ped* p = gPedPool->mPool.mpPrev; p; p = p->mpNext)
    {
        peds.insert(p);
    }
    for (auto& c : gCars)
    {
        if (cars.count(c.second))
        {
            f(c.first, c.second, (Ped*)NULL);
        }
    }
    for (auto& q : gPeds)
    {
        if (peds.count(q.second))
        {
            f(q.first, (Car*)NULL, q.second);
        }
    }
}
} // namespace

void net::engine::ReplicaPositions(std::vector<ReplicaPos>& out)
{
    out.clear();
    ForLiveReplicas([&](uint32_t id, Car* car, Ped* ped) {
        if (Sprite* s = car ? car->mCarSprite : CharSprite(ped))
        {
            out.push_back(ReplicaPos{id, s->mXy.x.ToFloat(), s->mXy.y.ToFloat()});
        }
    });
}

// A train isn't blended: its wagons follow its lead only in the lead's own update
void net::engine::ReplicaPoses(std::vector<Pose>& out)
{
    ForLiveReplicas([&](uint32_t id, Car* car, Ped* ped) {
        if (Sprite* s = car ? (car->IsTrainModel() ? NULL : car->mCarSprite) : !ped->mCar ? CharSprite(ped) : NULL)
        {
            out.push_back(Pose{id, s->mXy.x.mValue, s->mXy.y.mValue, s->mZpos.mValue, s->mRotation.rValue});
        }
    });
}

void net::engine::SetReplicaPose(const Pose& p)
{
    if (p.id >= kPedIdBase)
    {
        Ped* ped = ReplicaPed(p.id);
        if (ped && !ped->mCar && !AtCarDoor(ped) && PhaseOf(ped) != kPhaseDoor && !ped->isDead())
        {
            if (const Sprite* s = CharSprite(ped))
            {
                PortInterp::Correcting(s);
                MoveThrow(ped, p.x - s->mXy.x.mValue, p.y - s->mXy.y.mValue); // its char follows the throw's object
            }
            MovePed(ped, Raw(p.x), Raw(p.y), Raw(p.z), Angle(p.rot));
        }
    }
    else if (Car* car = ReplicaCar(p.id))
    {
        if (car->mCarSprite)
        {
            PortInterp::Correcting(car->mCarSprite);
            MoveCar(car, Raw(p.x), Raw(p.y), Raw(p.z), Angle(p.rot));
        }
    }
}

bool net::engine::IsServerPed(const Ped* p)
{
    return p->mPlayer || gPedKeys.count(p);
}
