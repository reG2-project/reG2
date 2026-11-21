// port/net (port/docs/netcode.md, "Entities"): a train (PublicTransport's Train) as a snapshot entity. Both ends make
// the same trains when the map loads (PublicTransport::SpawnTrainsFromStations), so a client's train i is the server's
// train i: the entity maps its carriages to the server's car ids and puts the server's state into it. Only the lead
// carriage is in the car pool's list; the wagons follow it in its update (Car::TrainUpdate).
#include "internal.hpp"
#include "Source/Car.hpp"
#include "Source/CarAI.hpp"
#include "Source/Ped.hpp"
#include "Source/Player.hpp"
#include "Source/PublicTransport.hpp"
#include "Source/SpriteGrid.hpp"
#include "Source/map_0x370.hpp"
#include "Source/explosion_type.hpp"
#include "Source/sprite.hpp"
#include <stdio.h>

namespace
{
using namespace net::engine;
using net::EntityState;

static_assert(kTrainFieldCount <= net::kMaxFields, "a train's fields");
const int kTrains = 10; // PublicTransport::mTrainArray
const int kStations = 100; // PublicTransport::mStations

// A train in use: made at a station, its lead and every wagon there (the game never frees one)
Train* LiveTrain(int i)
{
    if (!gPublicTransport_181C || i < 0 || i >= kTrains)
    {
        return NULL;
    }
    Train* t = &gPublicTransport_181C->mTrainArray[i];
    if (t->mAllocState != 2 || !t->mCarriages[0] || t->mWagonCount > kTrainCarriages - 1)
    {
        return NULL;
    }
    for (int k = 0; k <= t->mWagonCount; k++)
    {
        if (!t->mCarriages[k] || !t->mCarriages[k]->mCarSprite)
        {
            return NULL;
        }
    }
    return t;
}

int StationIndex(const TrainStation* s)
{
    const TrainStation* first = gPublicTransport_181C->mStations;
    return s && s >= first && s < first + kStations ? (int)(s - first) : -1;
}

int TrainIndex(const Train* t)
{
    const Train* first = gPublicTransport_181C->mTrainArray;
    return t && t >= first && t < first + kTrains ? (int)(t - first) : -1;
}

// The cab isn't the lead: the train reversed (Train::ReassignTrainHead swaps the lead and the last wagon)
bool Reversed(const Train* t)
{
    return t->mCarriages[0]->mCarInfoIdx != car_model_enum::TRAINCAB;
}

// The server's train reversed and this one not, or the other way: swapped as the game swaps them. The game's swap
// reads both ends' physics and the lead's driver (the traffic driver, or the player driving it)
bool MatchReversed(Train* t, bool there)
{
    if (Reversed(t) == there)
    {
        return true;
    }
    Car* lead = t->mCarriages[0];
    Car* last = t->mCarriages[t->mWagonCount];
    if (!t->mWagonCount || !lead->mPhysics || !last->mPhysics)
    {
        return false;
    }
    if (!lead->mDriver)
    {
        lead->SpawnDriverPed();
    }
    t->ReassignTrainHead();
    return Reversed(t) == there;
}

// The lead's driver as the server has it: whether a player drives decides the train's state (PublicTransportService). A
// ped driving it gets in by its own entity; the traffic driver stands in until then
void MatchDriver(Car* lead, int there)
{
    Ped* here = lead->mDriver;
    if (there >= kTrainDriverPlayer)
    {
        Player* p = GetPlayer(there - kTrainDriverPlayer);
        Ped* ped = p && p->mInUse ? p->mPlayerPed : NULL;
        if (ped && here != ped)
        {
            lead->mDriver = ped; // as ReassignTrainHead hands it over: the ped's own car stays as it is
        }
    }
    else if (there == kDriverNone && IsSpareDriver(here))
    {
        lead->SetDriver(NULL);
    }
    else if (there != kDriverNone && !here)
    {
        lead->SpawnDriverPed();
    }
}

// Its wagons behind its lead, as the lead's update puts them (Car::UpdateTrainCarriagesOnTrack, without the doors and
// timers it services): a lead moved by a snapshot isn't apart from them until its next update
void PlaceWagons(Train* t)
{
    const Sprite* ls = t->mCarriages[0]->mCarSprite;
    Fix16 x = ls->mXy.x, y = ls->mXy.y, z = ls->mZpos, speed;
    const bool forward = gPublicTransport_181C->GetTrainSpeed(t->mCarriages[0], &speed);
    for (int k = 1; k <= t->mWagonCount; k++)
    {
        s32 dir = forward ? gMap_0x370->AdvanceAlongRailwayChecked(&x, &y, &z, Fix16(2)) : gMap_0x370->AdvanceAlongRailway(&x, &y, &z, Fix16(2));
        Car* c = t->mCarriages[k];
        if (c->mDespawnStatus == car_despawn_status::despawning_5)
        {
            continue; // out of the collision grid (Car::TrainUpdate)
        }
        Sprite* s = c->mCarSprite;
        gSpriteGrid_1->RemoveFromRegionBuckets(s);
        s->set_xyz_lazy(x, y, z);
        s->set_ang_lazy(ReturnAngleFromRoadDirection(dir));
        gSpriteGrid_1->AddToRegionBuckets(s);
    }
}

int gMismatched; // for the log: trains this game has otherwise

// A train's place on a railway, with a block under it, and within 16 blocks of where it is here: its wagons are
// placed along the rails from it (Map_0x370::SnapZToRailway and AdvanceAlongRailway read the blocks unchecked)
bool OnRails(const Train* t, const net::EntityState& e)
{
    using namespace net::engine;
    const Fix16 x = Raw(e.f[kTrainX]), y = Raw(e.f[kTrainY]), z = Raw(e.f[kTrainZ]);
    const Sprite* lead = t->mCarriages[0]->mCarSprite;
    if (Fix16::MaxAbsDistance(lead->mXy.x, lead->mXy.y, x, y) > Fix16(16))
    {
        return false;
    }
    s32 railZ = 0;
    return gMap_0x370->FindRailwayAtCoord(x.ToInt(), y.ToInt(), railZ) && gMap_0x370->GetBlock(x.ToInt(), y.ToInt(), (z - Fix16(1)).ToInt());
}
} // namespace

void net::engine::CaptureTrains(Snapshot& out)
{
    for (int i = 0; i < kTrains; i++)
    {
        const Train* t = LiveTrain(i);
        if (!t)
        {
            continue;
        }
        EntityState e = Entity(kTrainIdBase + (uint32_t)i, kEntityTrain);
        const Car* lead = t->mCarriages[0];
        PutPosition(e, lead->mCarSprite);
        e.f[kTrainWagons] = t->mWagonCount;
        e.f[kTrainReversed] = Reversed(t);
        e.f[kTrainState] = t->mState;
        e.f[kTrainStopPhase] = t->mStopPhase;
        e.f[kTrainWaitTimer] = t->mWaitTimer;
        e.f[kTrainHijacked] = t->mIsHijacked;
        e.f[kTrainFleeing] = t->mPassengersFleeing;
        e.f[kTrainPassengerTimer] = t->mPassengerTimer;
        e.f[kTrainPassengerCount] = t->mPassengerCount;
        e.f[kTrainSpeed] = lead->mAI ? lead->mAI->mSpeed.mValue : 0;
        const Ped* d = lead->mDriver;
        e.f[kTrainDriver] = !d ? kDriverNone : IsSpareDriver(d) ? kDriverTraffic : d->mPlayer ? kTrainDriverPlayer + d->mPlayer->mPlayerIdx : kDriverPed;
        const TrainStation* s = t->mStation;
        e.f[kTrainStation] = StationIndex(s) + 1;
        e.f[kTrainPlatform] = s ? s->mPlatformState : 0;
        e.f[kTrainStopped] = s ? TrainIndex(s->mpStoppedTrain) + 1 : 0;
        for (int k = 0; k < kTrainCarriages; k++)
        {
            const Car* c = k <= t->mWagonCount ? t->mCarriages[k] : NULL;
            e.f[kTrainCar0 + k] = CarRef(c);
            e.f[kTrainDamage0 + k] = c ? c->mDamage : 0;
            e.f[kTrainTimer0 + k] = (uint8_t)t->mExplosionTimers[k]; // -1: none
        }
        out.entities.push_back(e);
    }
}

void net::engine::TrainCars(std::vector<Car*>& out)
{
    for (int i = 0; i < kTrains; i++)
    {
        if (const Train* t = LiveTrain(i))
        {
            for (int k = 0; k <= t->mWagonCount; k++)
            {
                out.push_back(t->mCarriages[k]);
            }
        }
    }
}

bool net::engine::ApplyTrain(const EntityState& e, std::vector<std::pair<uint32_t, Car*>>& carriages)
{
    carriages.clear();
    Train* t = LiveTrain((int)(e.id - kTrainIdBase));
    if (!t || t->mWagonCount != e.f[kTrainWagons] || !MatchReversed(t, e.f[kTrainReversed] != 0) || !OnRails(t, e))
    {
        gMismatched++;
        return false;
    }
    for (int k = 0; k <= t->mWagonCount; k++)
    {
        if (e.f[kTrainCar0 + k])
        {
            carriages.push_back(std::make_pair(CarRefNetId(e.f[kTrainCar0 + k]), t->mCarriages[k]));
        }
    }
    t->mState = e.f[kTrainState];
    t->mStopPhase = e.f[kTrainStopPhase];
    t->mWaitTimer = (s16)e.f[kTrainWaitTimer];
    t->mIsHijacked = e.f[kTrainHijacked] != 0;
    t->mPassengersFleeing = e.f[kTrainFleeing] != 0;
    t->mPassengerTimer = (char_type)e.f[kTrainPassengerTimer];
    t->mPassengerCount = (char_type)e.f[kTrainPassengerCount];
    // A train station only (the stop phases read its points); the train it says stopped there, a live one
    const int station = e.f[kTrainStation] - 1;
    if (station >= 0 && station < kStations && gPublicTransport_181C->mStations[station].mStationType == 2)
    {
        t->mStation = &gPublicTransport_181C->mStations[station];
    }
    if (TrainStation* s = t->mStation)
    {
        s->mPlatformState = e.f[kTrainPlatform];
        s->mpStoppedTrain = LiveTrain(e.f[kTrainStopped] - 1);
    }
    for (int k = 0; k < kTrainCarriages; k++)
    {
        t->mExplosionTimers[k] = (char_type)e.f[kTrainTimer0 + k];
    }
    // Damage: an explosion the server had is carried out as SetCarState does; a wreck of this game's own stays
    for (int k = 0; k <= t->mWagonCount; k++)
    {
        Car* c = t->mCarriages[k];
        const bool there = e.f[kTrainDamage0 + k] >= 32000;
        if (there && !IsWrecked(c))
        {
            ExplodeAsServer(c);
        }
        else if (!there && !IsWrecked(c))
        {
            c->mDamage = (s16)e.f[kTrainDamage0 + k];
        }
    }
    // The lead where the server has it, a player in it or not (the snapshot's car entity has it too, but only near this
    // client's player), and its wagons behind it
    Car* lead = t->mCarriages[0];
    MoveCar(lead, Raw(e.f[kTrainX]), Raw(e.f[kTrainY]), Raw(e.f[kTrainZ]), Angle(e.f[kTrainRot]));
    PlaceWagons(t);
    if (lead->mAI)
    {
        lead->mAI->mSpeed = Fix16(e.f[kTrainSpeed], 0);
    }
    MatchDriver(lead, e.f[kTrainDriver]);
    return true;
}

void net::engine::GuardTrains()
{
    for (int i = 0; i < kTrains; i++)
    {
        Train* t = LiveTrain(i);
        Ped* d = t ? t->mCarriages[0]->mDriver : NULL;
        if (!d || !d->mPlayer || d->mCar == t->mCarriages[0])
        {
            continue;
        }
        bool aboard = false;
        for (int k = 1; k <= t->mWagonCount; k++)
        {
            aboard = aboard || d->mCar == t->mCarriages[k];
        }
        if (aboard && d->mPedState1 == ped_state_1::in_car_10 && !d->mEnterCarAsPassenger)
        {
            d->mCar = t->mCarriages[0];
        }
        else if (!aboard)
        {
            t->mCarriages[0]->SetDriver(NULL); // out of the train: as the game's own leaving clears a lead's driver
        }
    }
}

bool net::engine::IsTrainModel(int model)
{
    return model == car_model_enum::TRAIN || model == car_model_enum::TRAINCAB || model == car_model_enum::TRAINFB ||
           model == car_model_enum::boxcar;
}

void net::engine::PrintTrainLog()
{
    if (gMismatched)
    {
        printf("net: %d train entities not applied (this game's train differs)\n", gMismatched);
        gMismatched = 0;
    }
}
