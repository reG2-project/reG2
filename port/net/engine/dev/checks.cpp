// port/net engine dev (port/docs/netcode.md, "Checks"): the engine's checks and logs, each off unless its GTA2_NET_* switch is
// set: the collision grid and the players' cars, the crash state, the self, animation, sound and gang logs, the server
// marker. Nothing here changes the game.
#include "../internal.hpp"
#include "dev.hpp"
#include "../world.hpp"
#include <chrono>
#include "Source/Camera.hpp"
#include "Source/Car.hpp"
#include "Source/Draw.hpp"
#include "Source/Frontend.hpp"
#include "Source/gtx_0x106C.hpp"
#include "Source/Char_Pool.hpp"
#include "Source/Game.hpp"
#include "Source/ObjectDefinitions.hpp"
#include "Source/Object_3C.hpp"
#include "Source/Object_5C.hpp"
#include "Source/ObjectPool.hpp"
#include "Source/Ped.hpp"
#include "Source/Player.hpp"
#include "Source/SoundObject.hpp"
#include "Source/SpriteGrid.hpp"
#include "Source/Weapon.hpp"
#include "Source/WeaponManager.hpp"
#include "Source/char.hpp"
#include "Source/collide.hpp"
#include "Source/gbh_graphics.hpp"
#include "Source/sound_obj.hpp"
#include "Source/Explosion.hpp"
#include "Source/root_sound.hpp"
#include "Source/sprite.hpp"
#include <math.h>
#if !defined(_WIN32) && !defined(__EMSCRIPTEN__)
#include <dlfcn.h>
#include <execinfo.h>
#endif
#include "port/src/interpolation.hpp"
#include "port/src/net_hooks.hpp"
#include "port/src/rand/owner.hpp"
#include "port/src/rand/state.hpp"
#include "Source/rng.hpp"
#include "Source/PedGroup.hpp"
#include "Source/PublicTransport.hpp"
#include <stdio.h>
#include <stdlib.h>
#include <algorithm>
#include <set>
#include <unordered_map>
#include <string.h>
#include <string>
#include <unordered_map>

const net::engine::DebugFlags& net::engine::Debug()
{
    static const DebugFlags sFlags = {getenv("GTA2_NET_CARDIFF") != NULL, getenv("GTA2_NET_SELFLOG") != NULL, getenv("GTA2_NET_ANIMLOG") != NULL,
                                      getenv("GTA2_NET_SOUNDLOG") != NULL, getenv("GTA2_NET_CHECKGRID") != NULL, getenv("GTA2_NET_PICKUPLOG") != NULL,
                                      getenv("GTA2_NET_CHECKSTALE") != NULL};
    return sFlags;
}

namespace
{
using namespace net::engine;
using net::EntityState;
using net::Snapshot;

uint32_t gDebugFrame;

uint32_t MappedId(const Car* car) // its net id, 0 if no snapshot car is mapped to it
{
    for (auto& c : ReplicaCars())
    {
        if (c.second == car)
        {
            return c.first;
        }
    }
    return 0;
}

// Every sprite linked into a collision grid still has its collision shape (a freed sprite left in the grid crashes the next
// collision query: SpriteGrid::FindCollidingSpriteInRow)
struct LinkWas
{
    int type;
    const void* owner;
    int carModel; // for a car owner
    int carStatus;
    uint32_t frame;
    // for a char owner: its ped as it was (id, states, objective, the snapshot ped it is (0: none), player)
    int pedId = -1, state1 = -1, state2 = -1, objective = -1, player = -1;
    uint32_t netId = 0;
};
std::unordered_map<const void*, LinkWas> gLinkWas; // every linked sprite as the last good pass saw it

// What is wrong with a grid link's sprite (NULL: nothing)
const char* WhatsWrong(Sprite* sp, int type, const std::set<Car*>& cars, const std::set<Char*>& chars)
{
    if (!sp)
    {
        return "no sprite";
    }
    if (type == sprite_types_enum::car_2)
    {
        Car* car = sp->mCarBcPtr;
        return !cars.count(car) ? "a car that is gone" : car->mCarSprite != sp ? "not its car's sprite" : NULL;
    }
    if (type == sprite_types_enum::ped_3)
    {
        Char* ch = sp->mCharB4Ptr;
        return !chars.count(ch) ? "a char that is gone" : ch->mSpritePtr != sp ? "not its char's sprite" : NULL;
    }
    if (type == sprite_types_enum::object_1 || type == sprite_types_enum::code_obj1_4 || type == sprite_types_enum::map_obj_5)
    {
        Object* o = sp->mObject2CPtr;
        return !o                       ? "no object"
             : o->mSprite != sp ? "not its object's sprite"
             : o->mDefinition && o->mDefinition->mCollisionBucketCategory != collision_bucket_category::sprite_grid_1_region_4
                 ? "an object that doesn't go in this grid (a link left from an earlier owner)"
                 : NULL;
    }
    return NULL;
}

void ReportLink(const char* where, int cellX, int y, Sprite* sp, int type, const char* wrong, const std::set<Ped*>& peds)
{
    printf("net: CHECK frame %u %s: grid 1 cell (%d, %d) links sprite %p (type %d): %s\n", gDebugFrame, where, cellX, y, (void*)sp, type, wrong);
    if (type == sprite_types_enum::ped_3 && sp && sp->mCharB4Ptr)
    {
        const Char* ch = sp->mCharB4Ptr;
        bool freed = false;
        for (Char* f = gChar_B4_Pool->mPool.mpHead; f && !freed; f = f->mpNext)
        {
            freed = f == ch;
        }
        Ped* owner = ch->mpPed;
        const bool live = owner && peds.count(owner);
        printf("net: CHECK   the char is %s, points to ped %p (in the pool %d, id %d, its char %p, states %d/%d)\n", freed ? "freed" : "allocated",
               (void*)owner, (int)live, live ? (int)owner->mId : -1, live ? (void*)owner->mGameObject : NULL,
               live ? (int)owner->mPedState1 : -1, live ? (int)owner->mPedState2 : -1);
    }
    auto was = gLinkWas.find(sp);
    if (was != gLinkWas.end())
    {
        printf("net: CHECK   frame %u it was linked as type %d, owner %p (car model %d, despawn status %d; ped %d, states %d/%d, "
               "objective %d, snapshot ped %u, player %d)\n",
               was->second.frame, was->second.type, was->second.owner, was->second.carModel, was->second.carStatus, was->second.pedId,
               was->second.state1, was->second.state2, was->second.objective, was->second.netId, was->second.player);
    }
}

LinkWas Record(Sprite* sp, int type, const std::unordered_map<const Char*, Ped*>& charPed, const std::unordered_map<const Ped*, uint32_t>& netIds)
{
    LinkWas w = {type, type == sprite_types_enum::car_2 ? (const void*)sp->mCarBcPtr : (const void*)sp->mObject2CPtr,
                 type == sprite_types_enum::car_2 ? (int)sp->mCarBcPtr->mCarInfoIdx : -1,
                 type == sprite_types_enum::car_2 ? (int)sp->mCarBcPtr->mDespawnStatus : -1, gDebugFrame};
    if (type == sprite_types_enum::ped_3)
    {
        auto cp = charPed.find(sp->mCharB4Ptr);
        if (cp != charPed.end())
        {
            Ped* p = cp->second;
            w.pedId = (int)p->mId;
            w.state1 = (int)p->mPedState1;
            w.state2 = (int)p->mPedState2;
            w.objective = (int)p->get_objective();
            w.player = p->mPlayer ? (int)p->mPlayer->mPlayerIdx : -1;
            auto n = netIds.find(p);
            w.netId = n != netIds.end() ? n->second : 0;
        }
    }
    return w;
}

// Every link in the collision grid belongs to a live owner whose sprite it is: a car in the car pool, a ped's char, an
// object (particles may be in the grid too and aren't checked). A freed sprite's collision shape is the pool's free list
// (the same field), so its shape tells nothing.
bool GridsSound(const char* where)
{
    static int sReported;
    std::set<Car*> cars;
    std::set<Ped*> peds;
    LiveEntities(cars, peds);
    std::set<Char*> chars;
    std::unordered_map<const Char*, Ped*> charPed;
    for (Ped* p : peds)
    {
        if (p->mGameObject)
        {
            chars.insert(p->mGameObject);
            charPed[p->mGameObject] = p;
        }
    }
    std::unordered_map<const Ped*, uint32_t> netIds;
    for (auto& q : ReplicaPeds())
    {
        netIds[q.second] = q.first;
    }
    std::unordered_map<const void*, LinkWas> now;
    GridCell* const* rows = reinterpret_cast<GridCell* const*>(gSpriteGrid_1); // mRows, private, first
    for (int y = 0; rows && y < 256; y++)
    {
        for (GridCell* c = rows[y]; c; c = c->mpNext)
        {
            for (GridSpriteLink* l = c->mpSpriteLinks; l; l = l->mpNext)
            {
                Sprite* sp = l->mpSprite;
                const int type = sp ? sp->GetType() : -1;
                if (const char* wrong = WhatsWrong(sp, type, cars, chars))
                {
                    if (sReported++ < 4)
                    {
                        ReportLink(where, (int)c->mX, y, sp, type, wrong, peds);
                    }
                    return false; // the last good pass stays what gLinkWas has
                }
                now[sp] = Record(sp, type, charPed, netIds);
            }
        }
    }
    gLinkWas.swap(now);
    return true;
}

bool gMarker;
Fix16 gMarkerX, gMarkerY, gMarkerZ;
} // namespace

bool net::engine::DebugGridOk(const char* where, Car* car)
{
    if (Debug().checkGrid && !GridsSound(where))
    {
        if (car)
        {
            printf("net: CHECK %s: car %p model %d, kind %d, driver %p, despawn status %d\n", where, (void*)car, (int)car->mCarInfoIdx,
                   (int)car->mCarKind, (void*)car->mDriver, (int)car->mDespawnStatus);
        }
        return false;
    }
    return true;
}

bool net::engine::DebugCheckPlayerCars(const char* where)
{
    static int sReported;
    if (Debug().checkGrid)
    {
        GridsSound(where);
    }
    std::set<Car*> liveCars;
    std::set<Ped*> livePeds;
    LiveEntities(liveCars, livePeds);
    for (int i = 0; i < PlayerCount(); i++)
    {
        Ped* ped = InGamePed(i);
        Car* car = ped ? ped->mCar : NULL;
        if (car && (!liveCars.count(car) || !car->mCarSprite))
        {
            if (sReported++ < 5)
            {
                printf("net: CHECK %s: player %d's ped is in a car that is gone (state %d/%d, despawn status %d)\n", where, i,
                       (int)ped->mPedState1, (int)ped->mPedState2, liveCars.count(car) ? (int)car->mDespawnStatus : -1);
            }
            return false;
        }
        // Player::GetPlayerCar, the car radio's car: its corpse's in camera modes 2 and 3
        Player* pl = GetPlayer(i);
        Car* shown = pl && pl->mInUse ? pl->GetPlayerCar() : NULL;
        if (shown && (!liveCars.count(shown) || !shown->mCarSprite))
        {
            if (sReported++ < 5)
            {
                Ped* q = pl->mCameraMode == 2 || pl->mCameraMode == 3 ? pl->mAuxPed : pl->mPlayerPed;
                printf("net: CHECK %s: player %d's car (camera mode %d, %s ped, state %d/%d, in it %d) is gone (live %d, despawn status %d)\n",
                       where, i, (int)pl->mCameraMode, q == pl->mAuxPed ? "aux" : "own", q ? (int)q->mPedState1 : -1,
                       q ? (int)q->mPedState2 : -1, q && q->mCar == shown, (int)liveCars.count(shown),
                       liveCars.count(shown) ? (int)shown->mDespawnStatus : -1);
            }
            return false;
        }
        // Driving a car that doesn't have it as its driver (Car::ClearDriver reads the driver when it gets out). Not a
        // train's: reversing one hands its driver to the new lead and leaves the ped in its carriage (Train::ReassignTrainHead)
        if (car && ped->mPedState2 == ped_state_2::ped2_driving_10 && !ped->mEnterCarAsPassenger && car->mDriver != ped &&
            !car->IsTrainModel())
        {
            static int sDriver;
            if (sDriver++ < 10)
            {
                Ped* d = car->mDriver;
                printf("net: CHECK %s: player %d drives car %d, which has driver %p (spare %d, player %d), mapped %d\n", where, i,
                       (int)car->mCarId, (void*)d, (int)IsSpareDriver(d), d && d->mPlayer ? (int)d->mPlayer->mPlayerIdx : -1,
                       (int)(MappedId(car) != 0));
            }
            return false;
        }
    }
    return true;
}

namespace
{
uint32_t PedNetId(const Ped* p)
{
    for (auto& q : ReplicaPeds())
    {
        if (q.second == p)
        {
            return q.first;
        }
    }
    return 0;
}

// What is wrong between a ped and the car it drives (NULL: nothing). Ped::ExitCarStateMachine clears the driver of
// mTargetToEnter, not of mCar, and Car::ClearDriver reads that driver
const char* DriverWrong(const Ped* p)
{
    const Car* car = p->mCar;
    if (!car || p->mEnterCarAsPassenger || p->mPedState2 != ped_state_2::ped2_driving_10 || car->IsTrainModel() || IsSpareDriver(p))
    {
        return NULL;
    }
    if (car->mDriver != p)
    {
        return "drives a car whose driver is another";
    }
    if (p->mInternalObjective == objectives_enum::leave_car_36 && p->mTargetToEnter != car)
    {
        return "leaves its car through another car";
    }
    return NULL;
}
} // namespace

void net::engine::DebugCheckDrivers(const char* where, uint32_t snapTick)
{
    static std::set<const Ped*> sWrong; // as the last check found them: each is reported when it goes wrong
    static int sReported;
    if (!Debug().checkGrid || !gPedPool)
    {
        return;
    }
    std::set<const Ped*> now;
    for (Ped* p = gPedPool->mPool.mpPrev; p; p = p->mpNext)
    {
        const char* wrong = DriverWrong(p);
        if (!wrong)
        {
            continue;
        }
        now.insert(p);
        if (sWrong.count(p) || sReported >= 30)
        {
            continue;
        }
        sReported++;
        const Car* car = p->mCar;
        const Car* target = p->mTargetToEnter;
        const Ped* d = car->mDriver;
        printf("net: CHECK frame %u tick %u %s (snapshot %u): ped %u (id %d, player %d, type %d/%d) %s: states %d/%d objective %d/%d, "
               "car %u (id %d) driver %p (ped %u, spare %d), target to enter %u (id %d) driver %p\n",
               gDebugFrame, (uint32_t)gpRng->get_cur_rng(), where, snapTick, PedNetId(p), (int)p->mId,
               p->mPlayer ? (int)p->mPlayer->mPlayerIdx : -1, (int)p->mPedType, (int)p->mOccupation, wrong, (int)p->mPedState1,
               (int)p->mPedState2, (int)p->mObjective, (int)p->mInternalObjective, MappedId(car), (int)car->mCarId, (const void*)d,
               d ? PedNetId(d) : 0u, (int)IsSpareDriver(d), target ? MappedId(target) : 0u, target ? (int)target->mCarId : -1,
               target ? (const void*)target->mDriver : NULL);
        if (d && !IsSpareDriver(d))
        {
            printf("net: CHECK   its driver: ped %u (id %d, player %d, type %d/%d) states %d/%d objective %d/%d, in it %d, char %p\n",
                   PedNetId(d), (int)d->mId, d->mPlayer ? (int)d->mPlayer->mPlayerIdx : -1, (int)d->mPedType, (int)d->mOccupation,
                   (int)d->mPedState1, (int)d->mPedState2, (int)d->mObjective, (int)d->mInternalObjective, (int)(d->mCar == car),
                   (const void*)d->mGameObject);
        }
    }
    sWrong.swap(now);
}

void net::engine::DebugCheckWeapons(const char* where, uint32_t snapTick)
{
    static std::set<std::pair<const void*, std::string>> sWrong; // each wrong holder or list reported once
    static int sReported;
    Game* g = gGame_0x40;
    if (!Debug().checkGrid || !gWeapon_30_Pool || !gPedPool || !g || sReported >= 40)
    {
        return;
    }
    auto report = [&](const void* key, const std::string& what, const Weapon* w, const Ped* p)
    {
        if (!sWrong.insert(std::make_pair(key, what)).second || sReported++ >= 40)
        {
            return;
        }
        printf("net: CHECK frame %u tick %u %s (snapshot %u): weapon %p (type %d, car %p, ped %p, ammo %d) %s", gDebugFrame,
               (uint32_t)gpRng->get_cur_rng(), where, snapTick, (const void*)w, w ? w->mWeaponType : -1,
               w ? (const void*)w->mCar : NULL, w ? (const void*)w->mpPed : NULL, w ? (int)w->mAmmo : -1, what.c_str());
        if (p)
        {
            printf(": ped %u (id %d, player %d, type %d, states %d/%d)", PedNetId(p), (int)p->mId,
                   p->mPlayer ? (int)p->mPlayer->mPlayerIdx : -1, (int)p->mPedType, (int)p->mPedState1, (int)p->mPedState2);
        }
        printf("\n");
    };
    // The pool's two lists, walked at most once round (a list that loops is what this looks for)
    const Pool<Weapon, 255>& pool = gWeapon_30_Pool->mPool;
    std::unordered_map<const Weapon*, int> on; // 1: the used list, 2: the free list
    for (Weapon* w = pool.mpPrev; w; w = w->mpNext)
    {
        if (on.count(w))
        {
            report(&pool, "is on the list of used weapons twice: it loops", w, NULL);
            break;
        }
        on[w] = 1;
    }
    for (Weapon* w = pool.mpStart; w; w = w->mpNext)
    {
        const int was = on.count(w) ? on[w] : 0;
        if (was)
        {
            report(&pool, was == 1 ? "is on both lists, used and free" : "is on the free list twice: it loops", w, NULL);
            break;
        }
        on[w] = 2;
    }
    // A player's weapons are its slot's (its ped and corpse clone may hold them); any other ped frees what it holds
    std::unordered_map<const Weapon*, const Player*> slotOf;
    for (int i = 0; i < (int)g->mNumPlayers; i++)
    {
        const Player* pl = g->mPlayers[i];
        for (int w = 0; pl && w < kPlayerWeapons; w++)
        {
            if (const Weapon* x = pl->mWeapons[w])
            {
                if (on.count(x) && on[x] == 2)
                {
                    report(x, "of a player's slot is on the free list", x, pl->mPlayerPed);
                }
                slotOf[x] = pl;
            }
        }
    }
    std::unordered_map<const Weapon*, const Ped*> heldBy;
    for (Ped* p = gPedPool->mPool.mpPrev; p; p = p->mpNext)
    {
        for (const Weapon* w : {p->mSelectedWeapon, p->mpWeapon})
        {
            if (!w)
            {
                continue;
            }
            if (on.count(w) && on[w] == 2)
            {
                report(p, "held by a ped is on the free list", w, p);
                continue;
            }
            const Player* slot = slotOf.count(w) ? slotOf[w] : NULL;
            const bool players = p->mPlayer && (slot == p->mPlayer || (w->mCar && on.count(w)));
            if (!players && (slot || w->mCar || w->mpPed != p))
            {
                report(p, slot ? "of a player's slot held by another ped" : w->mCar ? "of a car held by a ped" : "of another ped held by a ped", w, p);
            }
            if (!players && heldBy.count(w) && heldBy[w] != p)
            {
                report(p, "held by two peds (the other below)", w, p);
                report(heldBy[w], "held by two peds (the first)", w, heldBy[w]);
            }
            heldBy[w] = p;
        }
    }
}

EXTERN_GLOBAL(GridCell_Pool*, gGridCell_Pool); // defined in collide.cpp, declared in SpriteGrid.cpp only
EXTERN_GLOBAL(GridSpriteLink_Pool*, gGridSpriteLink_Pool);

void net::engine::DebugCheckGridLists(const char* where, uint32_t snapTick)
{
    static int sSaid;
    if (!Debug().checkGrid || !gGridCell_Pool || !gGridSpriteLink_Pool || sSaid >= 5)
    {
        return;
    }
    const GridCell* cells = gGridCell_Pool->mPool;
    const GridSpriteLink* links = gGridSpriteLink_Pool->mPool;
    auto cellOk = [&](const GridCell* c)
    {
        const size_t off = (size_t)((const char*)c - (const char*)cells);
        return (const char*)c >= (const char*)cells && off < sizeof(gGridCell_Pool->mPool) && off % sizeof(GridCell) == 0;
    };
    auto linkOk = [&](const GridSpriteLink* l)
    {
        const size_t off = (size_t)((const char*)l - (const char*)links);
        return (const char*)l >= (const char*)links && off < sizeof(gGridSpriteLink_Pool->mPool) && off % sizeof(GridSpriteLink) == 0;
    };
    const SpriteGrid* grids[] = {gSpriteGrid_1, gSpriteGrid_2, gSpriteGrid_3};
    for (int gi = 0; gi < 3; gi++)
    {
        // Its rows, the grid's only member (private, at offset 0)
        const GridCell* const* rows = reinterpret_cast<const GridCell* const*>(grids[gi]);
        for (int y = 0; rows && y < 256; y++)
        {
            int n = 0, lastX = -1;
            for (const GridCell* c = rows[y]; c; c = c->mpNext)
            {
                const char* wrong = !cellOk(c) ? "a cell outside the cell pool" : (int)c->mX <= lastX ? "cells out of order" : ++n > 256 ? "a row that loops" : NULL;
                for (const GridSpriteLink* l = wrong ? NULL : c->mpSpriteLinks; l && !wrong; l = l->mpNext)
                {
                    wrong = !linkOk(l) ? "a link outside the link pool" : !l->mpSprite ? "a link without a sprite" : NULL;
                }
                if (wrong)
                {
                    sSaid++;
                    printf("net: CHECK frame %u tick %u %s (snapshot %u): grid %d row %d: %s (%p)\n", gDebugFrame,
                           (uint32_t)gpRng->get_cur_rng(), where, snapTick, gi + 1, y, wrong, (const void*)c);
                    return;
                }
                lastX = c->mX;
            }
        }
    }
}

namespace
{
struct SpriteFreed
{
    uint32_t frame, tick;
    int type, model, links;
    const void* owner;
    void* frames[16];
};
std::unordered_map<const Sprite*, SpriteFreed> gSpriteFreed; // each freed sprite's last free
uint64_t gStaleLinks, gStaleChecks;

// The links to <sp> in the three sprite grids
int GridLinksTo(const Sprite* sp)
{
    int n = 0;
    const SpriteGrid* grids[] = {gSpriteGrid_1, gSpriteGrid_2, gSpriteGrid_3};
    for (const SpriteGrid* g : grids)
    {
        const GridCell* const* rows = reinterpret_cast<const GridCell* const*>(g);
        for (int y = 0; rows && y < 256; y++)
        {
            for (const GridCell* c = rows[y]; c; c = c->mpNext)
            {
                for (const GridSpriteLink* l = c->mpSpriteLinks; l; l = l->mpNext)
                {
                    n += l->mpSprite == sp;
                }
            }
        }
    }
    return n;
}

void SpriteFreedHook(Sprite* sp)
{
    SpriteFreed& f = gSpriteFreed[sp];
    f.frame = gDebugFrame;
    f.tick = gpRng ? (uint32_t)gpRng->get_cur_rng() : 0;
    f.type = (int)sp->mSpriteTypeEnum;
    f.owner = (const void*)sp->mObject2CPtr;
    // An object's model by its definition: Object::PoolDeallocate clears mModel before it frees the sprite
    f.model = -1;
    const ObjectDefinition* def = sp->IsObject2CType() && sp->mObject2CPtr ? sp->mObject2CPtr->mDefinition : NULL;
    for (int i = 0; def && gObjectDefinitions && i < ObjectDefinitions::k_max_definitions && f.model < 0; i++)
    {
        f.model = gObjectDefinitions->mDefinitionByIdx[i] == def ? i : -1;
    }
    f.links = GridLinksTo(sp);
    memset(f.frames, 0, sizeof(f.frames));
#if !defined(_WIN32)
    backtrace(f.frames, 16);
#endif
}
} // namespace

void net::engine::DebugCheckStaleLinks(const char* where, uint32_t snapTick)
{
    static int sSaid;
    if (!Debug().checkStale || !gSpritePool)
    {
        return;
    }
    gStaleChecks++;
    const Sprite* pool = gSpritePool->mPool.mPool;
    const size_t kSprites = sizeof(gSpritePool->mPool.mPool) / sizeof(gSpritePool->mPool.mPool[0]);
    const SpriteGrid* grids[] = {gSpriteGrid_1, gSpriteGrid_2, gSpriteGrid_3};
    for (int gi = 0; gi < 3; gi++)
    {
        const GridCell* const* rows = reinterpret_cast<const GridCell* const*>(grids[gi]);
        for (int y = 0; rows && y < 256; y++)
        {
            for (const GridCell* c = rows[y]; c; c = c->mpNext)
            {
                for (const GridSpriteLink* l = c->mpSpriteLinks; l; l = l->mpNext)
                {
                    const Sprite* sp = l->mpSprite;
                    if (!sp || sp->mInstanceId != 0)
                    {
                        continue;
                    }
                    gStaleLinks++;
                    if (sSaid >= 12)
                    {
                        continue;
                    }
                    sSaid++;
                    const bool inPool = sp >= pool && sp < pool + kSprites;
                    printf("net: CHECK frame %u tick %u %s (snapshot %u): grid %d cell (%d, %d) links freed sprite %p (type %d, %s, at "
                           "(%.2f, %.2f))\n",
                           gDebugFrame, gpRng ? (uint32_t)gpRng->get_cur_rng() : 0, where, snapTick, gi + 1, (int)c->mX, y, (const void*)sp,
                           (int)sp->mSpriteTypeEnum, inPool ? "a pool sprite" : "not a pool sprite", sp->mXy.x.ToFloat(), sp->mXy.y.ToFloat());
                    auto f = gSpriteFreed.find(sp);
                    if (f == gSpriteFreed.end())
                    {
                        printf("net: CHECK   never freed while the check ran\n");
                        continue;
                    }
                    printf("net: CHECK   freed at frame %u tick %u as type %d, owner %p, model %d, with %d grid links then; backtrace:\n",
                           f->second.frame, f->second.tick, f->second.type, f->second.owner, f->second.model, f->second.links);
                    fflush(stdout);
#if !defined(_WIN32)
                    int n = 0;
                    while (n < 16 && f->second.frames[n])
                    {
                        n++;
                    }
                    backtrace_symbols_fd(f->second.frames, n, 1);
#endif
                }
            }
        }
    }
    if (gStaleChecks % 20000 == 0)
    {
        printf("net: stale link check: %llu checks, %llu stale links seen\n", (unsigned long long)gStaleChecks,
               (unsigned long long)gStaleLinks);
    }
}

void net::engine::DebugCheckShown(const char* where)
{
    static uint8_t sWrong[MAX_PLAYERS];
    static int sInside[MAX_PLAYERS]; // frames in a row
    if (!Debug().checkGrid || !gGame_0x40 || !gMap_0x370)
    {
        return;
    }
    for (int i = 0; i < PlayerCount() && i < MAX_PLAYERS; i++)
    {
        Player* p = GetPlayer(i);
        Ped* ped = p && p->mInUse ? p->mPlayerPed : NULL;
        Sprite* s = ped && !ped->mCar && !ped->isDead() ? CharSprite(ped) : NULL;
        if (!s)
        {
            sWrong[i] = 0;
            sInside[i] = 0;
            continue;
        }
        // Sprite::Draw's reasons to leave it out, and a place inside a plain block, whose top is drawn over it: for a
        // frame it can be a slope's height carried onto a flat block, which the next tick rounds to its level
        const gmp_block_info* block = gMap_0x370->GetBlock(s->mXy.x.ToInt(), s->mXy.y.ToInt(), s->mZpos.ToInt());
        sInside[i] = block && GET_BLOCK_TYPE(block->mSlopeType) != AIR && GET_SLOPE_BITS(block->mSlopeType) == 0 ? sInside[i] + 1 : 0;
        const bool waiting = p->IsWaitingToRespawn(ped);
        const bool invisible = ped->mBf21C.bInvisible && !p->mPowerUpTimers[power_up_indices::Invisibility_11] && !p->IsUser();
        const bool inside = sInside[i] >= 2;
        const uint8_t wrong = (waiting ? 1 : 0) | (invisible ? 2 : 0) | (inside ? 4 : 0);
        if (wrong & ~sWrong[i])
        {
            printf("net: CHECK frame %u %s: player %d alive but not shown:%s%s%s, at (%.2f, %.2f, %.3f), ped state %d/%d\n", gDebugFrame,
                   where, i, waiting ? " drawn as its body waiting to respawn" : "", invisible ? " invisible without the power-up" : "",
                   inside ? " inside a block" : "", s->mXy.x.ToFloat(), s->mXy.y.ToFloat(), s->mZpos.ToFloat(), (int)ped->mPedState1,
                   (int)ped->mPedState2);
        }
        sWrong[i] = wrong;
    }
}

void net::engine::DebugCheckTurrets(const char* where, uint32_t snapTick)
{
    static std::set<const Car*> sSaid; // each car once (a pool slot reused for another car is said again only once)
    if (!Debug().checkGrid || !gCar_BC_Pool || sSaid.size() >= 20)
    {
        return;
    }
    for (Car* c = gCar_BC_Pool->mPool.mpPrev; c; c = c->mpNext)
    {
        const int model = c->IsFireTruck() ? objects::water_cannon_114 : c->IsTank() ? objects::tank_cannon_148 : c->IsGunJeep() ? 248 : -1;
        // A wreck has none (Car::HandleCarExplosion), unless a player drives it: it turns the turret
        if (model < 0 || !c->mCarSprite || c->mAttachments.GetSpriteForModel(model) || sSaid.count(c) ||
            (IsWrecked(c) && !(c->mDriver && c->mDriver->mPlayer)))
        {
            continue;
        }
        sSaid.insert(c);
        const Ped* d = c->mDriver;
        printf("net: CHECK frame %u tick %u %s (snapshot %u): car %u (id %d, model %d) has no roof turret: damage %d, wreck countdown %d, "
               "engine %d, wrecked %d, driver %p (player %d)\n",
               gDebugFrame, (uint32_t)gpRng->get_cur_rng(), where, snapTick, MappedId(c), (int)c->mCarId, (int)c->mCarInfoIdx,
               (int)c->mDamage, (int)c->mWreckCountdown, (int)c->mEngineStatus, (int)IsWrecked(c), (const void*)d,
               d && d->mPlayer ? (int)d->mPlayer->mPlayerIdx : -1);
    }
}

// Whether a car or ped is on its pool's list, allocating nothing (malloc can hang after a heap corruption); a bounded
// walk, as the list may be what broke
static bool LiveCar(const Car* car)
{
    int n = 0;
    for (const Car* c = gCar_BC_Pool ? gCar_BC_Pool->mPool.mpPrev : NULL; car && c && n < 4096; c = c->mpNext, n++)
    {
        if (c == car)
        {
            return true;
        }
    }
    // A train's wagons aren't on the list (TrainCars)
    for (int i = 0; car && gPublicTransport_181C && i < 10; i++)
    {
        const Train& t = gPublicTransport_181C->mTrainArray[i];
        for (int k = 0; t.mAllocState == 2 && k <= t.mWagonCount && k < kTrainCarriages; k++)
        {
            if (t.mCarriages[k] == car)
            {
                return true;
            }
        }
    }
    return false;
}

static bool LivePed(const Ped* ped)
{
    int n = 0;
    for (const Ped* p = gPedPool ? gPedPool->mPool.mpPrev : NULL; ped && p && n < 4096; p = p->mpNext, n++)
    {
        if (p == ped)
        {
            return true;
        }
    }
    return false;
}

// The crash report's line for a ped and for a car: what they are, and whether what they point to is live
static void CrashPed(const char* what, const Ped* q)
{
    if (!q)
    {
        return;
    }
    const bool live = LivePed(q);
    const Char* c = live ? q->mGameObject : NULL;
    const Sprite* sp = c ? c->mSpritePtr : NULL;
    fprintf(stderr, "net: crash state: %s ped %p live %d id %d states %d/%d objective %d player %p car %p (live %d) objective car %p (live %d) to enter %p (live %d) char %p state %d velocity %.3f at (%.2f, %.2f, %.2f)\n",
            what, (const void*)q, live, live ? (int)q->mId : -1, live ? (int)q->mPedState1 : -1, live ? (int)q->mPedState2 : -1,
            live ? (int)q->mObjective : -1, live ? (void*)q->mPlayer : NULL, live ? (void*)q->mCar : NULL,
            live ? (int)LiveCar(q->mCar) : -1, live ? (void*)q->mTargetObjectiveCar : NULL,
            live ? (int)LiveCar(q->mTargetObjectiveCar) : -1, live ? (void*)q->mTargetToEnter : NULL,
            live ? (int)LiveCar(q->mTargetToEnter) : -1, (const void*)c, c ? (int)c->mCharState : -1,
            c ? c->mVelocity.ToFloat() : 0.f, sp ? sp->mXy.x.ToFloat() : 0.f, sp ? sp->mXy.y.ToFloat() : 0.f,
            sp ? sp->mZpos.ToFloat() : 0.f);
}

static void CrashCar(const char* what, const Car* car)
{
    if (!car)
    {
        return;
    }
    const bool live = LiveCar(car);
    const Sprite* sp = live ? car->mCarSprite : NULL;
    fprintf(stderr, "net: crash state: %s car %p live %d id %d model %d despawn status %d driver %p (live %d) at (%.2f, %.2f, %.2f)\n", what,
            (const void*)car, live, live ? (int)car->mCarId : -1, live ? (int)car->mCarInfoIdx : -1,
            live ? (int)car->mDespawnStatus : -1, live ? (void*)car->mDriver : NULL,
            live ? (int)LivePed(car->mDriver) : -1, sp ? sp->mXy.x.ToFloat() : 0.f, sp ? sp->mXy.y.ToFloat() : 0.f,
            sp ? sp->mZpos.ToFloat() : 0.f);
}

void net::engine::DebugOnCrash()
{
    Game* g = gGame_0x40;
    fprintf(stderr, "net: crash state: frame %u, tick %u\n", gDebugFrame, gpRng ? (uint32_t)gpRng->get_cur_rng() : 0u);
    if (gCopyingBlock)
    {
        fprintf(stderr, "net: crash state: saving or restoring the block %s at %p, %zu bytes\n", gCopyingBlock->name, (void*)gCopyingBlock->addr,
                gCopyingBlock->size);
    }
    CrashPed("the last updated", PortRand::gLastPed);
    CrashCar("the last updated", PortRand::gLastCar);
    if (!g || !g->mLocalPlayer)
    {
        return;
    }
    Player* p = g->mLocalPlayer;
    Ped* ped = p->mPlayerPed;
    CrashPed("the local player's", ped);
    CrashPed("the local player's corpse (aux)", p->mAuxPed);
    fprintf(stderr, "net: crash state: camera mode %d, the player's car (Player::GetPlayerCar) %p\n", (int)p->mCameraMode,
            (void*)p->GetPlayerCar());
    Ped* camPed = p->mGameCamera.mTargetPed;
    Car* camCar = p->mGameCamera.mTargetCar;
    fprintf(stderr, "net: crash state: local player %d in use %d, ped %p (live %d) car %p, camera ped %p (live %d, the player's %d) car %p (live %d)\n",
            (int)p->mPlayerIdx, (int)p->mInUse, (void*)ped, (int)LivePed(ped), ped ? (void*)ped->mCar : NULL,
            (void*)camPed, (int)LivePed(camPed), camPed == ped, (void*)camCar, (int)LiveCar(camCar));
    // The sound listener's camera (sound_obj::InterrogateAudioEntities reads its followed ped's car)
    Camera* listener = NULL;
    if (gSound_obj.mType5Idx)
    {
        SoundObject* so = gSound_obj.mAudioEntities[gSound_obj.mType5Idx].mpObj;
        listener = so ? so->mpAny.pCamera_0xBC : NULL;
    }
    Camera* cams[3] = {&p->mGameCamera, &p->mAuxGameCamera, &p->mViewCamera};
    for (int k = 0; k < 3; k++)
    {
        Ped* cp = cams[k]->mTargetPed;
        fprintf(stderr, "net: crash state: camera %d%s follows ped %p (live %d, in car %p live %d)\n", k, cams[k] == listener ? " (the listener)" : "",
                (void*)cp, (int)LivePed(cp), cp && LivePed(cp) ? (void*)cp->mCar : NULL,
                cp && LivePed(cp) ? (int)LiveCar(cp->mCar) : -1);
    }
    if (listener && listener != cams[0] && listener != cams[1] && listener != cams[2])
    {
        Ped* cp = listener->mTargetPed;
        fprintf(stderr, "net: crash state: the listener is another camera %p, follows ped %p (live %d, aux of the player %d)\n", (void*)listener,
                (void*)cp, (int)LivePed(cp), cp == p->mAuxPed);
    }
    Car* cars[2] = {ped ? ped->mCar : NULL, camPed ? camPed->mCar : NULL};
    for (int k = 0; k < 2; k++)
    {
        if (cars[k])
        {
            fprintf(stderr, "net: crash state: car %p live %d sprite %p despawn status %d driver %p mapped %d\n", (void*)cars[k],
                    (int)LiveCar(cars[k]), (void*)cars[k]->mCarSprite, (int)cars[k]->mDespawnStatus,
                    (void*)cars[k]->mDriver, (int)(MappedId(cars[k]) != 0));
        }
    }
}

void net::engine::DebugSetFrame(uint32_t frame)
{
    gDebugFrame = frame;
}

void net::engine::DebugOwnCarDiff(const Snapshot& snap)
{
    Ped* ped = GetPlayer(LocalPlayer()) ? GetPlayer(LocalPlayer())->mPlayerPed : NULL;
    Car* car = ped ? ped->mCar : NULL;
    if (!car)
    {
        return;
    }
    for (const EntityState& e : snap.entities)
    {
        if (e.type == kEntityCar && ReplicaCar(e.id) == car)
        {
            DebugCarDiff(car, e, snap.tick);
        }
    }
}

std::string net::engine::DebugPlayerCar(int player, const Snapshot& snap)
{
    Player* p = GetPlayer(player);
    Ped* ped = p ? p->mPlayerPed : NULL;
    if (!ped)
    {
        return "";
    }
    char line[256];
    int n = snprintf(line, sizeof(line), "here: states %d/%d objective %d door %d car %p%s%s", (int)ped->mPedState1,
                     (int)ped->mPedState2, (int)ped->get_objective(), (int)ped->mTargetCarDoor, (void*)ped->mCar,
                     InCarTransition(ped) ? " (transition)" : "", ped->mTargetObjectiveCar ? " (target car)" : "");
    for (const EntityState& e : snap.entities)
    {
        if (e.type == kEntityPlayer && e.id == (uint32_t)player + 1)
        {
            float lx = 0, ly = 0;
            PlayerPosition(LocalPlayer(), &lx, &ly);
            const float dx = e.f[kPlayerX] / 16384.0f - lx, dy = e.f[kPlayerY] / 16384.0f - ly;
            snprintf(line + n, sizeof(line) - n, "; tick %u there: car %d (%p here) state %d target %d at (%.2f, %.2f), %.1f blocks from us", snap.tick,
                     e.f[kPlayerCar], (void*)RefCar(e.f[kPlayerCar]), e.f[kPlayerState], e.f[kPlayerTarget], e.f[kPlayerX] / 16384.0f,
                     e.f[kPlayerY] / 16384.0f, sqrtf(dx * dx + dy * dy));
        }
    }
    return line;
}

void net::engine::DebugSelfLog(uint32_t frame)
{
    static uint32_t sUntil;
    Player* p = Debug().selfLog ? GetPlayer(LocalPlayer()) : NULL;
    Ped* ped = p ? p->mPlayerPed : NULL;
    if (!ped)
    {
        return;
    }
    if (p->mWastedOrBusted || ped->isDead())
    {
        sUntil = frame + 30;
    }
    if (frame > sUntil)
    {
        return;
    }
    Sprite* s = PedSprite(ped);
    const Foci foci = PlayerFoci(LocalPlayer());
    int cars = 0; // around what the view follows
    for (Car* c = gCar_BC_Pool->mPool.mpPrev; c && foci.count; c = c->mpNext)
    {
        cars += c->mCarSprite && Near(c->mCarSprite, Fix16(foci.x[0]).mValue, Fix16(foci.y[0]).mValue, Fix16(10).mValue);
    }
    printf("net: frame %u, self: at (%.2f, %.2f) states %d/%d dead %d wasted %d countdown %d aux %p camera mode %d health %d char %p car %p "
           "objective %d, view at (%.2f, %.2f) with %d cars within 10 blocks, camera at (%.2f, %.2f, %.2f)\n",
           frame, s ? s->mXy.x.ToFloat() : -1.0f, s ? s->mXy.y.ToFloat() : -1.0f, (int)ped->mPedState1,
           (int)ped->mPedState2, (int)ped->isDead(), (int)p->mWastedOrBusted, (int)p->mDeathCountdown,
           (void*)p->mAuxPed, (int)p->mCameraMode, (int)ped->mHealth, (void*)ped->mGameObject,
           (void*)ped->mCar, (int)ped->get_objective(), foci.x[0], foci.y[0], cars, p->mViewCamera.mCurPos.mX.ToFloat(),
           p->mViewCamera.mCurPos.mY.ToFloat(), p->mViewCamera.mCurPos.mZ.ToFloat());
}

void net::engine::DebugAnimLog(uint32_t frame)
{
    if (!Debug().animLog || frame % 3)
    {
        return;
    }
    std::string line;
    char buf[160];
    for (int i = 0; i < PlayerCount(); i++)
    {
        Ped* ped = InGamePed(i);
        Char* c = ped ? ped->mGameObject : NULL;
        if (c && i != LocalPlayer())
        {
            snprintf(buf, sizeof(buf), " p%d[in 0x%x ped %d/%d char %d anim %d frame %d timer %d]", i, (unsigned)GetPlayer(i)->mInputs,
                     (int)ped->mPedState1, (int)ped->mPedState2, (int)c->mCharState, (int)c->mAnimationState,
                     (int)c->mAnimationFrame, (int)c->mFrameTimer);
            line += buf;
        }
    }
    printf("net: frame %u, anim:%s\n", frame, line.c_str());
}

void net::engine::DebugSoundChannels(uint32_t frame)
{
    static std::string sLast;
    if (!Debug().soundLog)
    {
        return;
    }
    std::string line;
    char buf[96];
    for (int i = 0; i < SOUND_CHANNELS; i++)
    {
        const sound_0x68& a = gSound_obj.mActiveSamples[i];
        if (a.mbIsBeingPlayed)
        {
            snprintf(buf, sizeof(buf), " ch%d:ent%d/samp%d/loops%u/type%d/rel%u/ftp%d", i, a.mEntityIndex, a.mSampIdx, a.mLoopCount,
                     a.mType, a.mReleasingVolumeDivider, a.mFramesToPlay);
            line += buf;
            const int ent = a.mEntityIndex;
            const AudioEntity* ae = ent >= 0 && ent < 1020 ? &gSound_obj.mAudioEntities[ent] : NULL;
            if (ae && ae->mbUsed && ae->mpObj && ae->mpObj->mObjectType == 7)
            {
                const Weapon* w = ae->mpObj->mpAny.pWeapon_30;
                const Ped* ped = w ? w->mpPed : NULL;
                snprintf(buf, sizeof(buf), "[weapon %d ammo %d reload %d shot %d ped %d player %d]", w ? w->mWeaponType : -1, w ? w->mAmmo : -1,
                         w ? w->mReloadTimer : -1, w ? w->mShotFired : -1, ped ? ped->mId : -1,
                         ped && ped->mPlayer ? ped->mPlayer->mPlayerIdx : -1);
                line += buf;
            }
        }
    }
    // The car radio: the local player's car, the station and its variant, and how many other players are in cars
    static std::string sRadioLast;
    const u8 e = gSound_obj.mActiveEmitterIdx[0];
    Car* car = gGame_0x40 && gGame_0x40->mLocalPlayer ? gGame_0x40->mLocalPlayer->GetPlayerCar() : NULL;
    int others = 0;
    for (int i = 0; gGame_0x40 && i < gGame_0x40->mNumPlayers; i++)
    {
        Player* p = gGame_0x40->mPlayers[i];
        others += p && p != gGame_0x40->mLocalPlayer && p->GetPlayerCar();
    }
    snprintf(buf, sizeof(buf), "car %d, in %d, station %d%s, others in cars %d", car ? car->mCarId : -1, gSound_obj.mbInRadioCar,
             e < 5 ? gSound_obj.mRadioEmitters[e].mStationIdx : -1, gSound_obj.mMainVariant ? "" : "A", others);
    if (sRadioLast != buf)
    {
        printf("net: frame %u, radio: %s\n", frame, buf);
        sRadioLast = buf;
    }
    if (line != sLast)
    {
        printf("net: frame %u, sound channels:%s (init %d)\n", frame, line.c_str(), gSound_obj.mbSoundInitialized);
        sLast = line;
    }
}

void net::engine::SetServerMarker(int32_t x, int32_t y, int32_t z)
{
    gMarker = true;
    gMarkerX = Raw(x);
    gMarkerY = Raw(y);
    gMarkerZ = Raw(z);
}

// GTA2_NET_MARKER: the square where the newest snapshot has the local player, over the frame (DrawOverlay)
void net::engine::DebugDrawMarker(Camera* cam)
{
    if (!gMarker)
    {
        return;
    }
    Fix16 sx, sy;
    cam->ProjectWorldToScreen_4B90E0(gMarkerX, gMarkerY, gMarkerZ, &sx, &sy);
    const float x = sx.ToFloat(), y = sy.ToFloat(), r = 4.0f;
    Vert v[4] = {};
    v[0].x = x - r, v[0].y = y - r;
    v[1].x = x + r, v[1].y = y - r;
    v[2].x = x + r, v[2].y = y + r;
    v[3].x = x - r, v[3].y = y + r;
    pgbh_DrawFlatRect(v, (s32)0xFFFF00FF); // magenta: where the server has us (a snapshot's tick, before the prediction)
}

// GTA2_NET_GANGLOG=1: player 0's group (an instant gang) every 15 game ticks, by the server's ids, on the server and on
// a client alike, to put the two side by side by tick
void net::engine::GangLog(bool server)
{
    static const bool on = getenv("GTA2_NET_GANGLOG") != NULL;
    Ped* leader = on ? InGamePed(0) : NULL;
    PedGroup* g = leader ? leader->mPedGroup : NULL;
    const uint32_t tick = (uint32_t)gpRng->get_cur_rng();
    if (!g || tick % 15)
    {
        return;
    }
    std::unordered_map<const Ped*, uint32_t> ids;
    for (const auto& r : ReplicaPeds())
    {
        ids[r.second] = r.first;
    }
    for (int i = -1; i < (int)g->mCount34 && i < 9; i++)
    {
        Ped* p = i < 0 ? leader : g->mPedList[i];
        if (!p)
        {
            continue;
        }
        const uint32_t id = server ? kPedIdBase + p->mId : (ids.count(p) ? ids[p] : 0);
        Car* car = p->mCar;
        printf("net: GANG %s tick %u %s %u: states %d/%d objective %d door %d car %s model %d at (%.2f, %.2f) rot %d\n", server ? "server" : "client",
               tick, i < 0 ? "leader" : "member", id, (int)p->mPedState1, (int)p->mPedState2, (int)p->mObjective,
               (int)p->mTargetCarDoor, car ? (car == leader->mCar ? "leader's" : "other") : "none", car ? (int)car->mCarInfoIdx : -1, p->get_cam_x().ToFloat(),
               p->get_cam_y().ToFloat(), (int)(PedSprite(p) ? PedSprite(p)->mRotation.rValue : -1));
    }
}


namespace
{
const char* Who(Car* c)
{
    Ped* d = c->mDriver;
    return !d ? "nobody" : !d->mPlayer ? "a ped" : InGamePed(LocalPlayer()) == d ? "the local player" : "a player";
}
} // namespace

void net::engine::StackLog(bool server)
{
    static const bool on = getenv("GTA2_NET_STACKLOG") != NULL;
    static std::set<std::pair<Car*, Car*>> sSeen; // ponytail: printed once per pair, never cleared (a dev log)
    if (!on || !gCar_BC_Pool)
    {
        return;
    }
    const uint32_t tick = (uint32_t)gpRng->get_cur_rng();
    for (Car* a = gCar_BC_Pool->mPool.mpPrev; a; a = a->mpNext)
    {
        for (Car* b = a->mpNext; b; b = b->mpNext)
        {
            Sprite* sa = a->mCarSprite;
            Sprite* sb = b->mCarSprite;
            if (!sa || !sb)
            {
                continue;
            }
            const float dx = (sa->mXy.x - sb->mXy.x).ToFloat(), dy = (sa->mXy.y - sb->mXy.y).ToFloat();
            // Closer than 0.4 blocks: cars queueing are farther apart, and a queue isn't a stack
            if (dx * dx + dy * dy > 0.4f * 0.4f || !sSeen.insert(std::make_pair(a, b)).second)
            {
                continue;
            }
            printf("net: STACK %s tick %u: car %u model %d (%.2f, %.2f, %.2f) status %d physics %d, car %u model %d (%.2f, %.2f, %.2f) status %d physics %d; driven by %s, %s\n",
                   server ? "server" : "client", tick, server ? 0u : MappedId(a), (int)a->mCarInfoIdx, sa->mXy.x.ToFloat(),
                   sa->mXy.y.ToFloat(), sa->mZpos.ToFloat(), (int)a->mDespawnStatus, a->mPhysics != NULL,
                   server ? 0u : MappedId(b), (int)b->mCarInfoIdx, sb->mXy.x.ToFloat(), sb->mXy.y.ToFloat(),
                   sb->mZpos.ToFloat(), (int)b->mDespawnStatus, b->mPhysics != NULL, Who(a), Who(b));
        }
    }
}

void net::engine::TrainLog(bool server)
{
    static const bool on = getenv("GTA2_NET_TRAINLOG") != NULL;
    static uint32_t sLast;
    const uint32_t tick = gpRng ? (uint32_t)gpRng->get_cur_rng() : 0;
    if (!on || !gPublicTransport_181C || tick % 150 || tick == sLast)
    {
        return;
    }
    sLast = tick;
    for (int i = 0; i < 10; i++)
    {
        const Train& t = gPublicTransport_181C->mTrainArray[i];
        Car* lead = t.mCarriages[0];
        const Car* last = t.mWagonCount < 11 ? t.mCarriages[t.mWagonCount] : NULL;
        if (t.mAllocState != 2 || !lead || !lead->mCarSprite || !last || !last->mCarSprite)
        {
            continue;
        }
        const Sprite* s = lead->mCarSprite;
        // Raw Fix16 values: the same text on both ends when the state is (a 32-bit x87 build rounds floats otherwise)
        printf("net: TRAIN %s tick %u: train %d lead model %d (%d, %d, %d) rot %d, last wagon (%d, %d), state %d phase %d wait %d, %s\n",
               server ? "server" : "client", tick, i, (int)lead->mCarInfoIdx, (int)s->mXy.x.mValue, (int)s->mXy.y.mValue,
               (int)s->mZpos.mValue, (int)s->mRotation.rValue, (int)last->mCarSprite->mXy.x.mValue, (int)last->mCarSprite->mXy.y.mValue,
               t.mState, t.mStopPhase, (int)t.mWaitTimer, Who(lead));
    }
}

// GTA2_NET_CHECKGRID: a sprite moved off the map (its rows and cells in the collision grid would be past the grid's),
// what it is and the code that moved it: the first few
static void SpriteMovedOff(Sprite* sp)
{
    static int sSaid;
    const int32_t kMax = 256 << 14;
    const int32_t x = sp->mXy.x.mValue, y = sp->mXy.y.mValue;
    if ((x >= 0 && x < kMax && y >= 0 && y < kMax) || sSaid >= 6)
    {
        return;
    }
    sSaid++;
    printf("net: CHECK frame %u tick %u: sprite %p type %d moved off the map to (%.2f, %.2f, %.2f); backtrace:\n", gDebugFrame,
           (uint32_t)gpRng->get_cur_rng(), (void*)sp, (int)sp->mSpriteTypeEnum, sp->mXy.x.ToFloat(),
           sp->mXy.y.ToFloat(), sp->mZpos.ToFloat());
    if (sp->mSpriteTypeEnum == sprite_types_enum::ped_3 && sp->mCharB4Ptr)
    {
        Char* c = sp->mCharB4Ptr;
        Ped* p = c->mpPed;
        uint32_t id = 0;
        for (const auto& r : ReplicaPeds())
        {
            id = r.second == p ? r.first : id;
        }
        printf("net: CHECK   char %p state %d velocity %.4f (raw %d) run speed %.4f rotation %d conveyor (%.4f, %.4f); ped %p net %u id %d states %d/%d player %p\n",
               (void*)c, (int)c->mCharState, c->mVelocity.ToFloat(), c->mVelocity.mValue, c->mRunOrJumpSpeed.ToFloat(),
               (int)c->mRotation.rValue, c->mConveyorDx.ToFloat(), c->mConveyorDy.ToFloat(), (void*)p, id,
               p ? (int)p->mId : -1, p ? (int)p->mPedState1 : -1, p ? (int)p->mPedState2 : -1,
               p ? (void*)p->mPlayer : NULL);
    }
    fflush(stdout);
#if !defined(_WIN32) && !defined(__EMSCRIPTEN__)
    void* frames[24];
    const int n = backtrace(frames, 24);
    backtrace_symbols_fd(frames, n, 1);
#endif
}

// GTA2_NET_CHECKGRID: the hook goes in with the first frame's checks
void net::engine::InstallSpriteCheck()
{
    if (Debug().checkGrid)
    {
        gPortSpriteMovedHook = SpriteMovedOff;
    }
    if (Debug().checkStale)
    {
        gPortSpriteFreedHook = SpriteFreedHook;
    }
}

// GTA2_NET_SFXLOG: each sample the mixer starts, by the game's tick, what made it and where (to compare a server's with
// a client's). A server with it opens its audio device and listens at slot <value>'s camera
namespace
{
void LogSampleStart(const void*)
{
    // ProcessActiveQueues starts a sample on the first channel not playing, and marks it after the start
    int ch = -1;
    for (int i = 0; i < gSound_obj.mnActiveSamples && ch < 0; i++)
    {
        ch = gSound_obj.mActiveSamples[i].mbIsBeingPlayed ? -1 : i;
    }
    if (ch < 0)
    {
        return;
    }
    const sound_0x68& a = gSound_obj.mActiveSamples[ch];
    const int ent = a.mEntityIndex;
    const SoundObject* o = ent >= 0 && ent < 1020 && gSound_obj.mAudioEntities[ent].mbUsed ? gSound_obj.mAudioEntities[ent].mpObj : NULL;
    char src[48] = "?";
    if (o && o->mObjectType == SoundObjectTypeEnum::Weapon_30_7 && o->mpAny.pWeapon_30)
    {
        const Weapon* w = o->mpAny.pWeapon_30;
        snprintf(src, sizeof(src), "weapon%d/p%d", w->mWeaponType, w->mpPed && w->mpPed->mPlayer ? w->mpPed->mPlayer->mPlayerIdx : -1);
    }
    else if (o && o->mObjectType == SoundObjectTypeEnum::Sprite_1 && o->mpAny.pSprite)
    {
        Sprite* s = o->mpAny.pSprite;
        if (Car* c = s->AsCar())
        {
            snprintf(src, sizeof(src), "car%d/p%d", c->mCarInfoIdx, c->mDriver && c->mDriver->mPlayer ? c->mDriver->mPlayer->mPlayerIdx : -1);
        }
        else if (Char* c = s->AsCharB4())
        {
            snprintf(src, sizeof(src), "ped/p%d", c->mpPed && c->mpPed->mPlayer ? c->mpPed->mPlayer->mPlayerIdx : -1);
        }
        else if (Object* obj = s->AsObject2C())
        {
            snprintf(src, sizeof(src), "obj%d", obj->mModel);
            if (obj->mModel == objects::explosion_113 && obj->mpAny.pExplosion)
            {
                snprintf(src + strlen(src), sizeof(src) - strlen(src), "/type%d", obj->mpAny.pExplosion->mType);
            }
        }
    }
    else if (o)
    {
        snprintf(src, sizeof(src), "type%d", o->mObjectType);
    }
    printf("sfx: tick %u samp %u %s at %d,%d loops %u vol %u\n", gpRng ? (uint32_t)gpRng->get_cur_rng() : 0, a.mSampIdx, src,
           a.mObj.mX.ToInt(), a.mObj.mY.ToInt(), a.mLoopCount, a.mnVolume);
}

// A player's death cry: unheard (far, vol0: its distance), queued or not (full, queued: its distance and volume; a
// resimulated tick's is the sound log's), and how long its channel played (cut, ended: the frames played and the sample's)
void LogDeathCry(const char* what, s32 a, s32 b, const sound_0x68& q)
{
    printf("cry: tick %u %s %d %d at %d,%d listener %d,%d%s\n", gpRng ? (uint32_t)gpRng->get_cur_rng() : 0, what, (int)a, (int)b,
           q.mObj.mX.ToInt(), q.mObj.mY.ToInt(), gSound_obj.mListenerX.ToInt(), gSound_obj.mListenerY.ToInt(),
           gPortSoundSide ? " resim" : "");
}
} // namespace

void net::engine::InstallSfxLog(bool server)
{
    const char* on = getenv("GTA2_NET_SFXLOG");
    if (!on)
    {
        return;
    }
    gPortSampleStartHook = LogSampleStart;
    gPortDeathCryHook = LogDeathCry;
    const int slot = atoi(on);
    Game* g = gGame_0x40;
    Player* p = server && g && slot > 0 && slot < (int)g->mNumPlayers ? g->mPlayers[slot] : NULL;
    // The game's listener is its user's (slot 0 on a server): its sound object moves to that player, which then sets it
    Player* user = p ? g->mPlayers[0] : NULL;
    if (user && user->mSoundObj)
    {
        if (p->mSoundObj && p->mSoundObj != user->mSoundObj)
        {
            gRoot_sound.DestroySoundObj(p->mSoundObj);
        }
        p->mSoundObj = user->mSoundObj;
        user->mSoundObj = NULL;
    }
    // Player::Service points it at the view camera only while the slot is in the game; else it is still player 0's
    if (p && p->mSoundObj && p->mSoundObj->mpAny.pCamera_0xBC != &p->mViewCamera)
    {
        p->mSoundObj->mpAny.pCamera_0xBC = &p->mGameCamera;
    }
}

// GTA2_NET_FIRELOG: every fire at every 30th tick of the game's cycle (the same tick on both ends): a car's (its net id,
// wrecked or not, its fire explosions' types), the ones on nothing (a molotov's, a flamethrower's, an explosion's) by
// place, and the peds on fire
void net::engine::FireLog(bool server)
{
    static const bool on = getenv("GTA2_NET_FIRELOG") != NULL;
    static uint32_t sLast;
    const uint32_t tick = gpRng ? (uint32_t)gpRng->get_cur_rng() : 0;
    if (!on || !gCar_BC_Pool || !gExplosionPool_7A8 || tick % 30 || tick == sLast)
    {
        return;
    }
    sLast = tick;
    const char* end = server ? "server" : "client";
    std::set<const Object*> onCars;
    for (Car* c = gCar_BC_Pool->mPool.mpPrev; c; c = c->mpNext)
    {
        std::string types;
        for (AttachedSprite* a = c->mAttachments.mP18; a; a = a->mpNext)
        {
            const Object* o = a->mpSprite ? a->mpSprite->mObject2CPtr : NULL;
            const s32 st = a->mpSprite ? a->mpSprite->GetType() : 0;
            if (o && (st == sprite_types_enum::object_1 || st == sprite_types_enum::code_obj1_4 || st == sprite_types_enum::map_obj_5) &&
                o->IsFireExplosion() && o->mpAny.pExplosion)
            {
                onCars.insert(o);
                types += (types.empty() ? "" : ",") + std::to_string(o->mpAny.pExplosion->mType);
            }
        }
        if (!types.empty())
        {
            const uint32_t id = server ? kCarIdBase + (uint32_t)c->mCarId : MappedId(c);
            printf("net: FIRE %s tick %u: car %u wreck %d damage %d model %d at (%.1f, %.1f) types %s\n", end, tick, id, (int)IsWrecked(c),
                   (int)c->mDamage, (int)c->mCarInfoIdx, c->mCarSprite->mXy.x.ToFloat(), c->mCarSprite->mXy.y.ToFloat(), types.c_str());
        }
    }
    for (int pool = 0; pool < 2; pool++)
    {
        const int n = pool ? 20 : 40;
        for (int i = 0; i < n; i++)
        {
            const bool used = pool ? (gExplosionPool_3D4 && gExplosionPool_3D4->mbUsed[i]) : gExplosionPool_7A8->mbUsed[i];
            const Explosion& x = pool ? gExplosionPool_3D4->mExplosions[i] : gExplosionPool_7A8->mExplosions[i];
            if (!used || !x.IsFireType() || !x.mpObj2C || !x.mpObj2C->mSprite || onCars.count(x.mpObj2C))
            {
                continue;
            }
            printf("net: FIRE %s tick %u: loose type %d timer %d at (%.1f, %.1f)\n", end, tick, (int)x.mType, (int)x.mTimer,
                   x.mpObj2C->mSprite->mXy.x.ToFloat(), x.mpObj2C->mSprite->mXy.y.ToFloat());
        }
    }
    int burning = 0;
    for (Ped* p = gPedPool ? gPedPool->mPool.mpPrev : NULL; p; p = p->mpNext)
    {
        burning += p->mBf21C.bOnFire;
    }
    const Ped* me = server ? NULL : InGamePed(LocalPlayer());
    printf("net: FIRE %s tick %u: peds burning %d, the local player at (%.1f, %.1f)\n", end, tick, burning, me ? me->get_cam_x().ToFloat() : -1.0f,
           me ? me->get_cam_y().ToFloat() : -1.0f);
}

void net::engine::PoseLog(bool server)
{
    static const bool on = getenv("GTA2_NET_POSELOG") != NULL;
    static uint32_t sLast;
    const uint32_t tick = gpRng ? (uint32_t)gpRng->get_cur_rng() : 0;
    if (!on || tick == sLast)
    {
        return;
    }
    sLast = tick;
    for (int i = 0; i < PlayerCount(); i++)
    {
        Ped* ped = InGamePed(i);
        Char* c = ped ? ped->mGameObject : NULL;
        if (c && !ped->mCar && !ped->isDead())
        {
            printf("net: POSE %s tick %u: p%d attack %d weapon %d anim %d moving %d\n", server ? "server" : "client", tick, i,
                   (int)ped->mBf21C.bAttacking, ped->mSelectedWeapon ? (int)ped->mSelectedWeapon->mWeaponType : -1,
                   (int)c->mAnimationState, c->mVelocity.mValue != 0);
        }
    }
}

void net::engine::DriveLog(bool server)
{
    static const bool on = getenv("GTA2_NET_HITLOG") != NULL;
    static uint32_t sLast;
    const uint32_t tick = gpRng ? (uint32_t)gpRng->get_cur_rng() : 0;
    if (!on || tick == sLast)
    {
        return;
    }
    sLast = tick;
    for (int i = 0; i < PlayerCount(); i++)
    {
        Ped* ped = InGamePed(i);
        Sprite* s = ped && ped->mCar ? PedSprite(ped) : NULL;
        if (s)
        {
            printf("net: DRIVE %s tick %u: p%d at (%.3f, %.3f) rot %d\n", server ? "server" : "client", tick, i, s->mXy.x.ToFloat(),
                   s->mXy.y.ToFloat(), (int)s->mRotation.rValue);
        }
    }
}

void net::engine::ThrowLog(bool server, uint32_t snap)
{
    static const bool on = getenv("GTA2_NET_THROWLOG") != NULL;
    static uint32_t sLast;
    const uint32_t tick = gpRng ? (uint32_t)gpRng->get_cur_rng() : 0;
    if (!on || !gExplosionPool_7A8 || tick == sLast)
    {
        return;
    }
    sLast = tick;
    const char* end = server ? "server" : "client";
    for (int i = 0; i < PlayerCount(); i++)
    {
        Ped* ped = InGamePed(i);
        Char* c = ped ? ped->mGameObject : NULL;
        Sprite* s = ped ? PedSprite(ped) : NULL;
        if (!ped || !s || (!ped->mpObj2C && ped->mPedState1 != ped_state_1::immobilized_8 && !ped->isDead() && !ped->mBf21C.bOnFire))
        {
            continue;
        }
        const Object* o = ped->mpObj2C;
        int killer = -1;
        for (int j = 0; j < PlayerCount() && ped->mKillerIdTimer > 0; j++)
        {
            killer = InGamePed(j) && InGamePed(j)->mId == ped->mKillerId ? j : killer;
        }
        printf("net: THROW %s tick %u: p%d at (%.2f, %.2f, %.2f) ped %d/%d stored %d/%d char %d/%d anim %d health %d inv %d fire %d "
               "cause %d obj %s(%.2f, %.2f, %.2f) speed %.3f z %.3f timeout %d snap %u by p%d\n",
               end, tick, i, s->mXy.x.ToFloat(), s->mXy.y.ToFloat(), s->mZpos.ToFloat(), (int)ped->mPedState1, (int)ped->mPedState2,
               (int)ped->mStoredPedState1, (int)ped->mStoredPedState2, c ? (int)c->mPedState1 : -1, c ? (int)c->mPedState2 : -1,
               c ? (int)c->mAnimationState : -1, (int)ped->mHealth, (int)ped->mInvulnerability, (int)ped->mBf21C.bOnFire,
               (int)ped->mDeathCause, o ? "" : "none ", o && o->mSprite ? o->mSprite->mXy.x.ToFloat() : 0.0f,
               o && o->mSprite ? o->mSprite->mXy.y.ToFloat() : 0.0f, o && o->mSprite ? o->mSprite->mZpos.ToFloat() : 0.0f,
               o && o->mObj3c ? o->mObj3c->mSpeed.ToFloat() : 0.0f, o && o->mObj3c ? o->mObj3c->mZSpeed.ToFloat() : 0.0f,
               c ? (int)c->mTimeout : -1, server ? tick : snap, killer);
    }
    for (int pool = 0; pool < 2; pool++)
    {
        const int n = pool ? 20 : 40;
        for (int k = 0; k < n; k++)
        {
            const bool used = pool ? (gExplosionPool_3D4 && gExplosionPool_3D4->mbUsed[k]) : gExplosionPool_7A8->mbUsed[k];
            const Explosion& x = pool ? gExplosionPool_3D4->mExplosions[k] : gExplosionPool_7A8->mExplosions[k];
            if (used && x.mpObj2C && x.mpObj2C->mSprite && x.mTimer >= 97 && x.mTimer <= 100)
            {
                printf("net: THROW %s tick %u: explosion type %d timer %d owner %d at (%.2f, %.2f, %.2f)\n", end, tick, (int)x.mType,
                       (int)x.mTimer, (int)x.mOwnerPedId, x.mpObj2C->mSprite->mXy.x.ToFloat(), x.mpObj2C->mSprite->mXy.y.ToFloat(),
                       x.mpObj2C->mSprite->mZpos.ToFloat());
            }
        }
    }
}

void net::engine::PedLog(const char* end)
{
    static const char* on = getenv("GTA2_NET_PEDLOG");
    static uint32_t sLast[2];
    const uint32_t tick = gpRng ? (uint32_t)gpRng->get_cur_rng() : 0;
    const bool live = strcmp(end, "applied") != 0; // a live tick once; an apply each time
    Ped* watched = on ? InGamePed(atoi(on)) : NULL;
    const Sprite* w = watched ? PedSprite(watched) : NULL;
    if (!w || (live && tick == sLast[end[0] == 's']))
    {
        return;
    }
    if (live)
    {
        sLast[end[0] == 's'] = tick;
    }
    auto log = [&](uint32_t id, Ped* p) {
        const Sprite* s = CharSprite(p);
        if (!s || p->mPlayer || !Near(s, w->mXy.x.mValue, w->mXy.y.mValue, 12 << 14))
        {
            return;
        }
        EntityState e = Entity(id, kEntityPed);
        CapturePhases(p, e, kPedPhase);
        printf("net: PEDPHASE %s tick %u: %u %d/%d phase %d timer %d health %d car %d\n", end, tick, id, (int)p->mPedState1,
               (int)p->mPedState2, e.f[kPedPhase + kPhase], e.f[kPedPhase + kPhaseTimer], (int)p->mHealth, InCarTransition(p) ? 1 : 0);
    };
    if (end[0] == 's')
    {
        for (Ped* p = gPedPool->mPool.mpPrev; p; p = p->mpNext)
        {
            log(PedNetId(p), p);
        }
    }
    else
    {
        for (const auto& kv : ReplicaPeds())
        {
            log(kv.first, kv.second);
        }
    }
}

namespace
{
std::string DeathLogPed(const char* name, Ped* ped)
{
    char buf[200];
    Char* c = ped ? ped->mGameObject : NULL;
    Sprite* s = c ? c->mSpritePtr : NULL;
    if (!ped)
    {
        return "";
    }
    snprintf(buf, sizeof(buf), " %s[id %d ped %d/%d h %d", name, ped->mId, (int)ped->mPedState1, (int)ped->mPedState2, (int)ped->mHealth);
    std::string out = buf;
    if (c && s)
    {
        snprintf(buf, sizeof(buf), " char %d/%d cs %d anim %d frame %d spr %d layer %d at %.2f,%.2f", (int)c->mPedState1, (int)c->mPedState2,
                 (int)c->mCharState, (int)c->mAnimationState, (int)c->mAnimationFrame, (int)s->mTypeSpriteIdx, (int)s->mDrawLayer, s->mXy.x.ToFloat(),
                 s->mXy.y.ToFloat());
        out += buf;
    }
    else
    {
        out += " no char";
    }
    return out + "]";
}
} // namespace

void net::engine::DebugDeathLog(bool server)
{
    const uint32_t frame = gDebugFrame;
    static const bool on = getenv("GTA2_NET_DEATHLOG") != NULL;
    static int sAfter[MAX_PLAYERS];
    // What stood for the player last frame (its ped, then its corpse while the player's, then kept), drawn there
    struct Shown
    {
        const Sprite* mpSprite;
        s16 mId;
        float mX, mY;
        int mAnim;
        bool mCorpse;
        int mKeptFrames; // its corpse followed after the respawn
    };
    static Shown sShown[MAX_PLAYERS];
    if (!on || !gGame_0x40)
    {
        return;
    }
    const char* end = server ? "server" : "client";
    for (int i = 0; i < PlayerCount() && i < MAX_PLAYERS; i++)
    {
        Player* p = GetPlayer(i);
        Ped* ped = p ? p->mPlayerPed : NULL;
        Shown& last = sShown[i];
        if (!p || !ped || !ped->mGameObject || (!server && i == LocalPlayer()))
        {
            last.mpSprite = NULL;
            continue;
        }
        const bool odd = ped->mPedState1 == ped_state_1::immobilized_8 || ped->isDead() || p->mWastedOrBusted || p->mAuxPed ||
            ped->mHealth < 100;
        sAfter[i] = odd ? 10 : sAfter[i] - 1;
        // The corpse once there is, then the same sprite after the respawn (kept), else the ped
        Ped* aux = p->mAuxPed && p->mAuxPed->mGameObject ? p->mAuxPed : NULL;
        const Sprite* s = aux ? aux->mGameObject->mSpritePtr : NULL;
        const Char* c = aux ? aux->mGameObject : NULL;
        bool kept = false;
        if (!s && last.mpSprite && last.mCorpse && last.mpSprite->mInstanceId == last.mId && last.mKeptFrames < 150)
        {
            for (Ped* q = gPedPool->mPool.mpPrev; q && !c; q = q->mpNext)
            {
                c = q->mGameObject && q->mGameObject->mSpritePtr == last.mpSprite && q->isDead() ? q->mGameObject : NULL;
            }
            s = c ? last.mpSprite : NULL;
            kept = s != NULL;
        }
        if (!s)
        {
            c = ped->mGameObject;
            s = c->mSpritePtr;
        }
        if (!s)
        {
            last.mpSprite = NULL;
            continue;
        }
        float x = s->mXy.x.ToFloat(), y = s->mXy.y.ToFloat();
        float gx, gy;
        if (PortInterp::GlideOffset(s, &gx, &gy))
        {
            x += gx, y += gy;
        }
        const bool corpse = s != ped->mGameObject->mSpritePtr;
        const float dx = x - last.mX, dy = y - last.mY, d = sqrtf(dx * dx + dy * dy);
        if (last.mpSprite && corpse && !last.mCorpse)
        {
            printf("net: frame %u, %s deathjump p%d %.2f\n", frame, end, i, d);
        }
        else if (last.mpSprite && corpse && (d > 0.01f || c->mAnimationState != last.mAnim))
        {
            printf("net: frame %u, %s corpsemove p%d %.3f anim %d frame %d%s\n", frame, end, i, d, (int)c->mAnimationState,
                   (int)c->mAnimationFrame, kept ? " kept" : "");
        }
        last = Shown{s, s->mInstanceId, x, y, (int)c->mAnimationState, corpse, kept ? last.mKeptFrames + 1 : 0};
        if (sAfter[i] <= 0)
        {
            continue;
        }
        printf("net: frame %u, tick %u, %s death p%d: wasted %d count %d cam %d%s%s\n", frame, gpRng ? (uint32_t)gpRng->get_cur_rng() : 0,
               end, i, (int)p->mWastedOrBusted, (int)p->mDeathCountdown, (int)p->mCameraMode, DeathLogPed("ped", ped).c_str(),
               DeathLogPed("aux", p->mAuxPed).c_str());
    }
}
