// port/net (port/docs/netcode.md, "Shots"): rockets, grenades, molotovs and blasts. Both ends simulate them; each has a
// prediction key both compute (port/src/rand/owner.hpp: its creator, tick and index), kept in the object so a restore
// brings it back with it. The server sends each once, as it was at the end of its first tick; at a snapshot's tick a
// client makes the ones it lacks (brought on to that tick) and removes, silently, the ones the server doesn't have.
#include "internal.hpp"
#include "members.hpp"
#include "dev/dev.hpp"
#include "port/src/net_hooks.hpp"
#include "port/src/rand/owner.hpp"
#include "Source/Car.hpp"
#include "Source/Char_Pool.hpp"
#include "Source/Explosion.hpp"
#include "Source/Object_3C.hpp"
#include "Source/Object_5C.hpp"
#include "Source/ObjectPool.hpp"
#include "Source/Ped.hpp"
#include "Source/PedRefTable.hpp"
#include "Source/enums.hpp"
#include "Source/explosion_type.hpp"
#include "Source/rng.hpp"
#include "Source/sprite.hpp"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <set>
#include <unordered_map>
#include <unordered_set>

namespace
{
using namespace net::engine;
using net::EntityState;
using net::Snapshot;

// An Object_3C's motion: its way, speeds, gain and brake, and what it collides with
const Member kMotion[] = {
    NET_MEMBER(Object_3C, mAngle, 2, true),
    NET_MEMBER(Object_3C, mSpeed, 4, true),
    NET_MEMBER(Object_3C, mZSpeed, 4, true),
    NET_MEMBER(Object_3C, mSavedSpeed, 4, true),
    NET_MEMBER(Object_3C, mFriction, 4, true),
    NET_MEMBER(Object_3C, mZAccel, 4, true),
    NET_MEMBER(Object_3C, mNextDefinitionTimer, 2, true),
    NET_MEMBER(Object_3C, mbAirborne, 2, true),
    NET_MEMBER(Object_3C, mSpriteCollideMode, 4, true),
    NET_MEMBER(Object_3C, mbOnSlope, 1, false),
};
const size_t kMotionCount = sizeof(kMotion) / sizeof(kMotion[0]);
const uint32_t kMostCatchUp = 64; // ticks a made shot is brought on at most (a snapshot's lag)

uint32_t KeyOf(const Object* o)
{
    uint32_t k;
    memcpy(&k, &o->mUnused28, sizeof(k)); // 0x28..0x2B: nothing of the game's reads them
    return k;
}

void SetKey(Object* o, uint32_t k)
{
    memcpy(&o->mUnused28, &k, sizeof(k));
}

bool Projectile(int model)
{
    return model == objects::rocket_bullet_128 || model == objects::moving_molotov_138 || model == objects::grenade_obj_183;
}

bool KeyedModel(int model)
{
    return Projectile(model) || model == objects::explosion_113;
}

bool SyncedBlast(int kind)
{
    return kind == explosion_type::small_18 || kind == explosion_type::item_19 || kind == explosion_type::large_20 ||
           kind == explosion_type::no_ring_32 || kind == explosion_type::small_33;
}

int BlastKind(const Object* o)
{
    return o->mModel == objects::explosion_113 && o->mHasExplosion && o->mpAny.pExplosion ? o->mpAny.pExplosion->mType : -1;
}

bool Synced(const Object* o)
{
    return o->mSprite && o->mDefinition && !o->mRemovalState && KeyOf(o) &&
           (Projectile(o->mModel) || (o->mObj3c && SyncedBlast(BlastKind(o))));
}

void AddAttached(const SpriteList& list, std::unordered_set<const Sprite*>& out)
{
    for (const AttachedSprite* a = list.mP18; a; a = a->mpNext)
    {
        out.insert(a->mpSprite);
    }
}

// What rides on a car or an object: its carrier's (a car's explosion, cars.cpp; a molotov's fire)
std::unordered_set<const Sprite*> AttachedSprites()
{
    std::unordered_set<const Sprite*> out;
    std::vector<Car*> cars;
    TrainCars(cars);
    for (Car* c = gCar_BC_Pool->mPool.mpPrev; c; c = c->mpNext)
    {
        cars.push_back(c);
        if (Car* t = TowedTrailer(c))
        {
            cars.push_back(t);
        }
    }
    for (const Car* c : cars)
    {
        AddAttached(c->mAttachments, out);
    }
    for (const Object& o : gObject_2C_Pool->mPool)
    {
        if (o.mSprite && o.mObj3c && !o.mRemovalState)
        {
            AddAttached(o.mObj3c->mAttachedSprites, out);
        }
    }
    return out;
}

// Every synced object on its own, on the pool's list or not
template <typename F> void ForEachShot(F f)
{
    const std::unordered_set<const Sprite*> attached = AttachedSprites();
    for (Object& o : gObject_2C_Pool->mPool)
    {
        if (Synced(&o) && !attached.count(o.mSprite))
        {
            f(&o);
        }
    }
}

uint32_t Cycle()
{
    return gpRng ? (uint32_t)gpRng->get_cur_rng() : 0;
}

uint32_t OwnerOf(const Object* o)
{
    s32 id = BlastKind(o) >= 0 ? o->mpAny.pExplosion->mOwnerPedId : 0;
    if (!id && o->mPedRefIdx && o->mPedRefIdx < 255)
    {
        id = gPedRefTable_7F8->GetPedId(o->mPedRefIdx);
    }
    Ped* p = id && gPedManager ? gPedManager->PedById(id) : NULL;
    return p ? PedNetId(p) : 0;
}

EntityState StartOf(const Object* o, uint32_t key)
{
    EntityState e = Entity(kShotIdBase + key, kEntityShot);
    PutPosition(e, o->mSprite);
    e.f[kShotModel] = o->mModel;
    const int kind = BlastKind(o);
    e.f[kShotKind] = kind < 0 ? 0 : kind;
    e.f[kShotTimer] = kind >= 0 ? o->mpAny.pExplosion->mTimer : o->mModel == objects::grenade_obj_183 && o->mpAny.o8 ? (uint16_t)o->mpAny.o8->mTimer : 0;
    e.f[kShotOwner] = (int32_t)OwnerOf(o);
    e.f[kShotBorn] = (int32_t)(Cycle() & 0xFFFF);
    if (kind < 0)
    {
        CaptureMembers(o->mObj3c, kMotion, kMotionCount, e, kShotMotion); // a blast's stands still (its Object_3C isn't set)
    }
    return e;
}

std::unordered_map<uint32_t, EntityState> gStarts; // a live shot's key -> its start, as first captured
std::unordered_set<uint32_t> gMadeHere; // a client's blasts made from a snapshot: they hurt on the server
uint32_t gGhostTick; // GTA2_NET_TESTGHOSTSHOT
uint32_t gKeySkew; // GTA2_NET_SHOTSKEW=1: a client's keys none of the server's (the shots check's red run)

void ObjectMade(Object* o)
{
    SetKey(o, KeyedModel(o->mModel) ? PortRand::NextObjectKey() ^ gKeySkew : 0);
}

unsigned int ObjectCreator(const Object* o)
{
    return KeyOf(o);
}

bool BlastHurts(Explosion* x)
{
    return !x->mpObj2C || !gMadeHere.count(KeyOf(x->mpObj2C));
}

// In a network game a blast throws its debris seen or not, on both ends alike
bool BlastDebris(Explosion*)
{
    return true;
}

bool ShotLogOn()
{
    static const bool on = getenv("GTA2_NET_SHOTLOG") != NULL;
    return on;
}

// The server's shot made here as its weapon or blast makes one, then brought on to the snapshot's tick
Object* MakeShot(const EntityState& e, uint32_t key, uint32_t cycle)
{
    const int model = e.f[kShotModel];
    Ped* owner = NetIdPed((uint32_t)e.f[kShotOwner]);
    if (owner && !owner->mPedRefIdx)
    {
        owner->mPedRefIdx = gPedRefTable_7F8->AllocForPed(owner->mId);
    }
    const Fix16 x = Raw(e.f[kShotX]), y = Raw(e.f[kShotY]), z = Raw(e.f[kShotZ]);
    Object* o;
    if (model == objects::explosion_113)
    {
        o = gObject_5C->CreateExplosion(x, y, z, Angle(e.f[kShotRot]), e.f[kShotKind], owner ? owner->mId : 0);
        if (!o || !o->mpAny.pExplosion)
        {
            return NULL;
        }
        o->mpAny.pExplosion->mTimer = (u16)e.f[kShotTimer];
        gMadeHere.insert(key);
    }
    else
    {
        o = gObject_5C->NewMovingObj(model, x, y, z, Angle(e.f[kShotMotion]), Angle(e.f[kShotRot]), Fix16(0), Fix16(0), Fix16(0));
        if (!o || !o->mObj3c)
        {
            return NULL;
        }
        ApplyMembers(o->mObj3c, kMotion, kMotionCount, e, kShotMotion);
        if (model == objects::grenade_obj_183 && o->mpAny.o8)
        {
            o->SetO8Timer((s16)e.f[kShotTimer]);
        }
        else if (model == objects::moving_molotov_138)
        {
            // Its light and its fire, as Weapon::throwable attaches them
            Object* pLight = gObject_5C->NewLight(94, 138, 2, 0xFF8000, 3, 255);
            if (pLight)
            {
                o->mSprite->AttachSprite(pLight->mSprite, 0, 0, Ang16((s16)0, (u8)0));
            }
            Object* pFire = gObject_5C->CreateExplosion(Fix16(113), Fix16(145), 2, Ang16((s16)0, (u8)0), explosion_type::molotov_fire_5,
                                                        owner ? owner->mId : 0);
            if (pFire)
            {
                o->mSprite->AttachSprite(pFire->mSprite, 0, 0, Ang16((s16)0, (u8)0));
            }
        }
    }
    if (owner)
    {
        o->SetDamageOwner(owner->mPedRefIdx);
    }
    SetKey(o, key);
    const uint32_t behind = (cycle - (uint32_t)e.f[kShotBorn]) & 0xFFFF;
    for (uint32_t t = 0; t < behind && t < kMostCatchUp; t++)
    {
        PortRand::Scope scope(o);
        if (o->PoolUpdate())
        {
            // Gone on the way (a flight that differs here): as the pool's update frees it
            if (o->mPoolListState == object_pool_state::in_pool_list_1)
            {
                gObject_2C_Pool->FindAndDeAllocate(o);
            }
            else
            {
                gObject_2C_Pool->DeAllocate(o);
            }
            return NULL;
        }
    }
    return o;
}
} // namespace

int net::engine::gShotsKept, net::engine::gShotsMade, net::engine::gShotsRemoved, net::engine::gShotsBeyond;

void net::engine::ShotMotionFields(std::vector<FieldDesc>& out)
{
    AppendFields(kMotion, kMotionCount, out);
}

void net::engine::InstallShots(bool on, bool client)
{
    gPortObjectMade = on ? ObjectMade : NULL;
    PortRand::gObjectKey = on ? ObjectCreator : NULL;
    gPortBlastHurts = on && client ? BlastHurts : NULL;
    gPortBlastDebris = on ? BlastDebris : NULL;
    const char* ghost = getenv("GTA2_NET_TESTGHOSTSHOT");
    gGhostTick = on && client && ghost ? (uint32_t)atoi(ghost) : 0;
    gKeySkew = on && client && getenv("GTA2_NET_SHOTSKEW") ? 0x800000 : 0;
}

void net::engine::ClearShots()
{
    gStarts.clear();
    gMadeHere.clear();
}

void net::engine::CaptureShots(Snapshot& out)
{
    std::unordered_map<uint32_t, EntityState> now;
    ForEachShot([&](Object* o) {
        const uint32_t key = KeyOf(o);
        if (now.count(key))
        {
            return; // two live shots with one key: the first only (ids are unique in a snapshot)
        }
        auto it = gStarts.find(key);
        const EntityState e = it != gStarts.end() && it->second.f[kShotModel] == o->mModel ? it->second : StartOf(o, key);
        now[key] = e;
        out.entities.push_back(e);
    });
    gStarts.swap(now);
}

void net::engine::ApplyShots(const Snapshot& snap)
{
    const EntityState* w = FindWorld(snap);
    if (!w)
    {
        return;
    }
    const uint32_t cycle = (uint32_t)w->f[kWorldCycle];
    std::unordered_map<uint32_t, Object*> live;
    ForEachShot([&](Object* o) { live[KeyOf(o)] = o; });
    std::set<uint32_t> wanted;
    const ReachArea area(snap, 0.0f);
    for (const EntityState& e : snap.entities)
    {
        if (e.type != kEntityShot)
        {
            continue;
        }
        const uint32_t key = e.id - kShotIdBase;
        wanted.insert(key);
        if (live.count(key))
        {
            gShotsKept++;
            continue;
        }
        const int model = e.f[kShotModel];
        if (!(Projectile(model) || (model == objects::explosion_113 && SyncedBlast(e.f[kShotKind]))))
        {
            continue; // a model or blast the server never sends (sanitize.cpp's rule: CreateExplosion takes any type)
        }
        if (area.DistanceTo(Blocks(e.f[kShotX]), Blocks(e.f[kShotY])) > SnapshotReach(snap))
        {
            gShotsBeyond++; // where this game's world is a guess
            if (ShotLogOn())
            {
                printf("net: SHOT beyond tick %u: %06x model %d, %.1f blocks away, reach %.1f\n", cycle, key, model,
                       area.DistanceTo(Blocks(e.f[kShotX]), Blocks(e.f[kShotY])), SnapshotReach(snap));
            }
            continue;
        }
        Object* o = MakeShot(e, key, cycle);
        gShotsMade += o != NULL;
        if (ShotLogOn())
        {
            printf("net: SHOT made tick %u: %06x model %d kind %d owner %d born %d behind %u%s\n", cycle, key, model, e.f[kShotKind],
                   e.f[kShotOwner], e.f[kShotBorn], (cycle - (uint32_t)e.f[kShotBorn]) & 0xFFFF, o ? "" : ", gone on the way");
        }
    }
    for (const auto& kv : live)
    {
        if (!wanted.count(kv.first))
        {
            if (ShotLogOn())
            {
                printf("net: SHOT removed tick %u: %06x model %d kind %d owner %u, %s the reach\n", cycle, kv.first, (int)kv.second->mModel,
                       BlastKind(kv.second), OwnerOf(kv.second), area.DistanceTo(kv.second->mSprite) > SnapshotReach(snap) ? "beyond" : "within");
            }
            kv.second->Dealloc(); // not there at this tick: never fired there, or already gone; no impact here either
            gShotsRemoved++;
        }
    }
    for (auto it = gMadeHere.begin(); it != gMadeHere.end();)
    {
        it = wanted.count(*it) ? ++it : gMadeHere.erase(it);
    }
}

void net::engine::ShotsTickStart()
{
    Ped* ped = gGhostTick && Cycle() == gGhostTick ? InGamePed(LocalPlayer()) : NULL;
    Sprite* s = ped ? PedSprite(ped) : NULL;
    if (s)
    {
        // A rocket the server never fires, made in every run of this tick: the snapshot of the first tick after it
        // removes it
        PortRand::Scope scope(0x7E000001u);
        Object* o = gObject_5C->NewMovingObj(objects::rocket_bullet_128, s->mXy.x, s->mXy.y + Fix16(2), s->mZpos, Ang16((s16)0, (u8)0),
                                             Ang16((s16)0, (u8)0), Fix16(0x800, 0), Fix16(0), Fix16(0));
        if (o)
        {
            printf("net: test ghost shot key %06x\n", KeyOf(o));
        }
    }
}

void net::engine::ShotLog(bool server)
{
    static uint32_t sLast;
    const uint32_t tick = Cycle();
    if (!ShotLogOn() || !gObject_2C_Pool || tick == sLast)
    {
        return;
    }
    sLast = tick;
    ForEachShot([&](Object* o) {
        printf("net: SHOT %s tick %u: %06x model %d kind %d timer %d owner %u at (%.2f, %.2f, %.2f)\n", server ? "server" : "client", tick, KeyOf(o),
               (int)o->mModel, BlastKind(o), BlastKind(o) >= 0 ? (int)o->mpAny.pExplosion->mTimer : -1, OwnerOf(o), o->mSprite->mXy.x.ToFloat(),
               o->mSprite->mXy.y.ToFloat(), o->mSprite->mZpos.ToFloat());
    });
}

void net::engine::DebugShot(int player, int model)
{
    Ped* ped = InGamePed(player);
    Sprite* s = ped ? PedSprite(ped) : NULL;
    if (!s || !Projectile(model))
    {
        return;
    }
    if (!ped->mPedRefIdx)
    {
        ped->mPedRefIdx = gPedRefTable_7F8->AllocForPed(ped->mId);
    }
    // East of it, thrown east (a rocket flies on), as Weapon::throwable sets a throw off
    gObject_5C->SetPendingDamageOwner(ped->mPedRefIdx);
    Object* o = gObject_5C->NewMovingObj(model, s->mXy.x + Fix16(1), s->mXy.y, s->mZpos + Fix16(0x2000, 0), Ang16((s16)0, (u8)0),
                                         Ang16((s16)0, (u8)0), Fix16(0x800, 0), -Fix16(0x20, 0), model == objects::rocket_bullet_128 ? Fix16(0) : Fix16(0x666, 0));
    if (o && model == objects::grenade_obj_183 && o->mpAny.o8)
    {
        o->SetO8Timer(6);
    }
    if (o)
    {
        printf("net: test shot key %06x\n", KeyOf(o));
    }
}
