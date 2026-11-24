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
#include "Source/CollisionSoundQueue.hpp"
#include "Source/Explosion.hpp"
#include "Source/Object_3C.hpp"
#include "Source/Object_3C_Pool.hpp"
#include "Source/Object_5C.hpp"
#include "Source/Object_8_Pool.hpp"
#include "Source/ObjectPool.hpp"
#include "Source/Ped.hpp"
#include "Source/PedRefTable.hpp"
#include "Source/enums.hpp"
#include "Source/explosion_type.hpp"
#include "Source/rng.hpp"
#include "Source/sprite.hpp"
#include <algorithm>
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
const int kMostMade = 32; // shots made from one snapshot (a busy fight makes a few)

// 0x28..0x2A: the key, 0x2B: the keyed objects it made (PortRand::gObjectMade); nothing of the game's reads them
uint32_t KeyOf(const Object* o)
{
    uint32_t k;
    memcpy(&k, &o->mUnused28, sizeof(k));
    return k & 0xFFFFFF;
}

void SetKey(Object* o, uint32_t k, uint8_t made = 0)
{
    const uint32_t v = (k & 0xFFFFFF) | (uint32_t)made << 24;
    memcpy(&o->mUnused28, &v, sizeof(v));
}

unsigned char* MadeCount(const Object* o)
{
    return (unsigned char*)const_cast<char*>(&o->mUnused2B);
}

// And a loose fire: the burning debris a large blast throws (Explosion::UpdateExplosion_18_19_20_32_33), sent as it is
// now rather than as it started (its way comes from the blast's particles, which differ between the ends)
bool Projectile(int model)
{
    return model == objects::rocket_bullet_128 || model == objects::moving_molotov_138 || model == objects::grenade_obj_183 ||
           model == objects::explosion_fragments_127;
}

// A thrown ped's object isn't a shot: keyed, its draws are its own (PortRand::Scope) and not the game's stream
bool KeyedModel(int model)
{
    return Projectile(model) || model == objects::explosion_113 || model == objects::ped_pushed_110;
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

// A debris' fire (Explosion::EmitFireTrail_5: out 20 ticks after it comes to rest), or NULL
Explosion* DebrisFire(const Object* o)
{
    for (const AttachedSprite* a = o->mObj3c ? o->mObj3c->mAttachedSprites.mP18 : NULL; a; a = a->mpNext)
    {
        const Object* fire = a->mpSprite ? a->mpSprite->mObject2CPtr : NULL;
        if (fire && BlastKind(fire) == explosion_type::molotov_fire_5)
        {
            return fire->mpAny.pExplosion;
        }
    }
    return NULL;
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
    const Explosion* fire = !id && o->mModel == objects::explosion_fragments_127 ? DebrisFire(o) : NULL;
    id = fire ? fire->mOwnerPedId : id;
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
    if (o->mModel == objects::explosion_fragments_127)
    {
        const Explosion* fire = DebrisFire(o);
        e.f[kShotTimer] = fire ? fire->mTimer : 0; // 0: out, its light alone keeps it
    }
    e.f[kShotOwner] = (int32_t)OwnerOf(o);
    e.f[kShotBorn] = (int32_t)(Cycle() & 0xFFFF);
    if (kind < 0)
    {
        CaptureMembers(o->mObj3c, kMotion, kMotionCount, e, kShotMotion); // a blast's stands still (its Object_3C isn't set)
    }
    return e;
}

// A debris here as the server's is at this tick
void SetDebris(Object* o, const EntityState& e)
{
    o->RemoveFromCollisionBuckets();
    o->mSprite->set_xyz_lazy(Raw(e.f[kShotX]), Raw(e.f[kShotY]), Raw(e.f[kShotZ]));
    o->mSprite->set_ang_lazy(Angle(e.f[kShotRot]));
    o->AssignToBucket();
    ApplyMembers(o->mObj3c, kMotion, kMotionCount, e, kShotMotion);
    if (Explosion* fire = DebrisFire(o))
    {
        fire->mTimer = e.f[kShotTimer] ? (u16)e.f[kShotTimer] : 1; // 1: out at its next update
    }
}

std::unordered_map<uint32_t, EntityState> gStarts; // a live shot's key -> its start, as first captured
std::unordered_set<uint32_t> gMadeHere; // a client's blasts made from a snapshot: they hurt on the server
uint32_t gKeySkew; // DebugShotSkew
bool gClient;
std::vector<std::pair<Object*, uint32_t>>* gCatchUpMade; // while a made shot is brought on: the keyed objects it makes

void ObjectMade(Object* o)
{
    SetKey(o, KeyedModel(o->mModel) ? PortRand::NextObjectKey() ^ gKeySkew : 0);
    if (gCatchUpMade && KeyOf(o))
    {
        gCatchUpMade->push_back(std::make_pair(o, KeyOf(o)));
    }
}

// Room for a shot and what comes with it (Pool::Allocate doesn't check): a margin for the game's own
bool RoomForShot()
{
    return FreeSlots(gObject_2C_Pool->mpStart) > 32 && FreeSlots(gObject_3C_Pool->mpHead) > 16 && FreeSlots(gObject_8_Pool->mpHead) > 16 &&
           FreeSlots(gSpritePool->mPool.mpHead) > 32;
}

// Brought on <ticks> ticks to the snapshot's, each numbered for the draws as the server ran it; what it hits makes no
// blast, sound or rumble here (the server's blast comes with the next snapshot). False: gone on the way
bool CatchUp(Object* o, uint32_t ticks, uint32_t cycle)
{
    std::vector<std::pair<Object*, uint32_t>> made;
    gCatchUpMade = &made;
    const bool noRumble = gPortNoRumble;
    gPortNoRumble = true;
    const s32 sounds = gCollisionSoundQueue ? gCollisionSoundQueue->mCount : 0;
    const unsigned int tick = PortRand::SetTick(0);
    bool gone = false;
    for (uint32_t t = 0; t < ticks && !gone; t++)
    {
        PortRand::SetTick(cycle - ticks + t);
        PortRand::Scope scope(o);
        gone = o->PoolUpdate();
    }
    PortRand::SetTick(tick);
    if (gCollisionSoundQueue && gCollisionSoundQueue->mCount > sounds)
    {
        gCollisionSoundQueue->mCount = sounds;
    }
    gPortNoRumble = noRumble;
    gCatchUpMade = NULL;
    for (const auto& m : made)
    {
        Object* x = m.first;
        if (x != o && x->mSprite && !x->mRemovalState && KeyOf(x) == m.second && SyncedBlast(BlastKind(x)))
        {
            x->Dealloc();
        }
    }
    if (gone)
    {
        // As the pool's update frees it
        if (o->mPoolListState == object_pool_state::in_pool_list_1)
        {
            gObject_2C_Pool->FindAndDeAllocate(o);
        }
        else
        {
            gObject_2C_Pool->DeAllocate(o);
        }
    }
    return !gone;
}

unsigned int ObjectCreator(const Object* o)
{
    return KeyOf(o);
}

bool BlastHurts(Explosion* x)
{
    return !x->mpObj2C || !gMadeHere.count(KeyOf(x->mpObj2C));
}

bool ServerBlastDebris(Explosion*)
{
    return true;
}

// A client's blast that it didn't predict (a car's the server blew up, or one made at a snapshot): its debris are the
// server's, which come with the snapshots
bool ClientBlastDebris(Explosion* x)
{
    if (!x->mpObj2C || (x->mTimer != 59 && x->mTimer != 69 && x->mTimer != 79 && x->mTimer != 89))
    {
        return true; // no debris this tick (and Explosion::mpAttachedSprite outlives its car: the pool doesn't clear it)
    }
    for (Car* c = gCar_BC_Pool->mPool.mpPrev; c; c = c->mpNext)
    {
        for (const AttachedSprite* a = c->mAttachments.mP18; a; a = a->mpNext)
        {
            if (a->mpSprite == x->mpObj2C->mSprite)
            {
                return false;
            }
        }
    }
    return !gMadeHere.count(KeyOf(x->mpObj2C));
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
        o->mpAny.pExplosion->mTimer = (u16)std::min(e.f[kShotTimer], (int32_t)explosion_timer::start_100);
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
        else if (model == objects::moving_molotov_138 || model == objects::explosion_fragments_127)
        {
            // Its light and its fire, in the order Weapon::throwable (a molotov) and a blast's debris attach them
            const Ang16 zero((s16)0, (u8)0);
            Object* pLight = model == objects::moving_molotov_138 ? gObject_5C->NewLight(94, 138, 2, 0xFF8000, 3, 255) : NULL;
            if (pLight)
            {
                o->mSprite->AttachSprite(pLight->mSprite, 0, 0, zero);
            }
            const bool debris = model == objects::explosion_fragments_127;
            Object* pFire = !debris || e.f[kShotTimer] ? gObject_5C->CreateExplosion(Fix16(113), Fix16(145), 2, zero, explosion_type::molotov_fire_5,
                                                                                        owner ? owner->mId : 0)
                                                       : NULL;
            if (pFire)
            {
                o->mSprite->AttachSprite(pFire->mSprite, 0, 0, zero);
                if (debris && pFire->mpAny.pExplosion)
                {
                    pFire->mpAny.pExplosion->mTimer = (u16)e.f[kShotTimer];
                }
            }
            pLight = model == objects::explosion_fragments_127 ? gObject_5C->NewLight(94, 138, 2, 0xFF8000, 3, 255) : NULL;
            if (pLight)
            {
                o->mSprite->AttachSprite(pLight->mSprite, 0, 0, zero);
            }
        }
    }
    if (owner && model != objects::explosion_fragments_127)
    {
        o->SetDamageOwner(owner->mPedRefIdx);
    }
    SetKey(o, key);
    const uint32_t behind = model == objects::explosion_fragments_127 ? 0 : (cycle - (uint32_t)e.f[kShotBorn]) & 0xFFFF;
    return CatchUp(o, std::min(behind, kMostCatchUp), cycle) ? o : NULL;
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
    gPortBlastDebris = !on ? NULL : client ? ClientBlastDebris : ServerBlastDebris;
    PortRand::gObjectMade = on ? MadeCount : NULL;
    gKeySkew = on && client ? DebugShotSkew() : 0;
    gClient = on && client;
}

uint32_t net::engine::ShotKey(const Object* o)
{
    return KeyOf(o);
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
        uint32_t key = KeyOf(o);
        if (now.count(key) && gClient)
        {
            return; // ids are unique in a snapshot
        }
        while (now.count(key))
        {
            key = (key * 0x9E3779B1u + 1) & 0xFFFFFF; // two live shots with one key: the second another, sent too
            key = key ? key : 1;
            SetKey(o, key, *MadeCount(o));
        }
        auto it = gStarts.find(key);
        const bool known = it != gStarts.end() && it->second.f[kShotModel] == o->mModel;
        EntityState e = known && o->mModel != objects::explosion_fragments_127 ? it->second : StartOf(o, key);
        if (known)
        {
            e.f[kShotBorn] = it->second.f[kShotBorn];
        }
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
    std::vector<Object*> twice;
    ForEachShot([&](Object* o) {
        Object*& at = live[KeyOf(o)];
        if (at)
        {
            twice.push_back(o); // a second with one key: the server's other one has another (CaptureShots)
        }
        else
        {
            at = o;
        }
    });
    for (Object* o : twice)
    {
        o->Dealloc();
        gShotsRemoved++;
    }
    std::set<uint32_t> wanted;
    const ReachArea area(snap, 0.0f);
    int made = 0;
    for (const EntityState& e : snap.entities)
    {
        if (e.type != kEntityShot)
        {
            continue;
        }
        const uint32_t key = e.id - kShotIdBase;
        const int model = e.f[kShotModel];
        wanted.insert(key);
        auto kept = live.find(key);
        if (kept != live.end())
        {
            Object* o = kept->second;
            if (o->mModel == model && (model != objects::explosion_113 || BlastKind(o) == e.f[kShotKind]))
            {
                if (model == objects::explosion_fragments_127)
                {
                    SetDebris(o, e);
                }
                gShotsKept++;
                continue;
            }
            o->Dealloc(); // another shot than the server's with its key: the server's is made instead
            live.erase(kept);
            gShotsRemoved++;
        }
        if (!(Projectile(model) || (model == objects::explosion_113 && SyncedBlast(e.f[kShotKind]))))
        {
            continue; // a model or blast the server never sends (sanitize.cpp's rule: CreateExplosion takes any type)
        }
        if (made >= kMostMade || !RoomForShot())
        {
            continue; // a server making more than a fight does: what it can't have here
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
        made++;
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
