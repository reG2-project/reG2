// port/net (port/docs/netcode.md, "Other players' shots shown"): a client's bullets of other players come from the ticks the
// server forwarded, made in a resimulation, so already in flight when first drawn, or born and gone before any frame.
// Those in flight slide from the shooter's gun (PortInterp::SlideFrom), and each plays the game's muzzle flash from its
// start (PortShotFlash). Display only: what is simulated and hit stays as it is.
#include "internal.hpp"
#include "port/src/interpolation.hpp"
#include "port/src/rand/owner.hpp"
#include "port/src/shot_flash.hpp"
#include "Source/Game.hpp"
#include "Source/Object_5C.hpp"
#include "Source/ParticleManager.hpp"
#include "Source/Particle_4C.hpp"
#include "Source/Ped.hpp"
#include "Source/Player.hpp"
#include "Source/enums.hpp"
#include "Source/rng.hpp"
#include "Source/sprite.hpp"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <map>
#include <tuple>
#include <unordered_map>

unsigned port_FrameCount(); // port/compat/src/video_port.cpp

namespace
{
using namespace net::engine;

const float kMuzzle = 0.25f; // blocks ahead of the ped a gun's shot starts (Weapon::pistol and the others)
const uint32_t kMostAge = 64; // ticks a record is kept at most

// GTA2_NET_SHOTVIS=0: only measured (comfort.cpp), to compare
bool On()
{
    static const char* v = getenv("GTA2_NET_SHOTVIS");
    return !v || *v != '0';
}

// GTA2_NET_SHOTLOG: each shown so, by the frame it is first drawn at (GTA2GL_CAPTURE's)
bool LogOn()
{
    static const bool on = getenv("GTA2_NET_SHOTLOG") != NULL;
    return on;
}

bool GunShot(int model)
{
    return model == objects::pistol_bullet_265 || model == objects::machine_gun_bullet_254 || model == objects::shotgun_bullet_192 ||
           model == objects::rocket_bullet_128;
}

uint32_t Cycle()
{
    return gpRng ? (uint32_t)gpRng->get_cur_rng() : 0;
}

// A shot as the shooter made it: the same in every resimulation of its tick
typedef std::tuple<const Ped*, uint32_t, int> Key; // its shooter, tick and number in that tick

struct Record
{
    Object* o;
    Sprite* s;
    s16 id; // its sprite's instance id
    int model;
    const Ped* shooter;
    uint32_t tick;
    float x, y; // where it started
    bool seen; // at the end of a tick
    bool done; // shown (slid or flashed): kept so that a resimulation making it again doesn't show it again
    bool ended;
    bool pending; // its tick resimulated since, not made again yet
};

std::map<Key, Record> gRecords;
std::unordered_map<const Ped*, int> gMadeThisTick;
std::unordered_map<const Ped*, uint32_t> gFlashTick; // the tick of the last shot its flash was shown for

bool Alive(const Record& r)
{
    return r.o->mSprite == r.s && r.s->mInstanceId == r.id && GunShot(r.o->mModel);
}

// The game's muzzle flash of the shooter's still in the world (ParticleManager::GunMuzzelFlash)
bool Flashing(const Sprite* shooter)
{
    for (Particle_4C* p = gParticle_4C_Pool ? gParticle_4C_Pool->GetFirstUsed() : NULL; p; p = p->mpNext)
    {
        if (p->mState == 40 && p->mpSprite == shooter)
        {
            return true;
        }
    }
    return false;
}

void See(Record& r)
{
    if (r.pending || r.ended || r.done)
    {
        return;
    }
    if (!Alive(r))
    {
        r.ended = true;
        return;
    }
    r.seen = true;
    r.ended = r.o->mRemovalState != 0; // a hit: where it stops
}
} // namespace

void net::engine::ShotVisMade(Object* o)
{
    const Ped* p = PortRand::gLastPed; // the ped whose update runs: its weapon fires (Ped::ManageWeapon)
    const Player* local = gGame_0x40 ? gGame_0x40->mLocalPlayer : NULL;
    if (!GunShot(o->mModel) || !o->mSprite || !p || !p->mPlayer || (local && local->mPlayerPed == p) || p->mCar)
    {
        return;
    }
    const Sprite* ps = CharSprite(p);
    const float x = o->mSprite->mXy.x.ToFloat(), y = o->mSprite->mXy.y.ToFloat();
    if (!ps || fabsf(x - ps->mXy.x.ToFloat()) > 2 * kMuzzle || fabsf(y - ps->mXy.y.ToFloat()) > 2 * kMuzzle)
    {
        return; // not its gun's
    }
    const Key key(p, Cycle(), gMadeThisTick[p]++);
    Record& r = gRecords[key];
    if (r.done)
    {
        return;
    }
    r = Record();
    r.o = o, r.s = o->mSprite, r.id = o->mSprite->mInstanceId, r.model = o->mModel, r.shooter = p, r.tick = Cycle();
    r.x = x, r.y = y;
}

void net::engine::ShotVisTick()
{
    gMadeThisTick.clear();
    for (auto& kv : gRecords)
    {
        See(kv.second);
    }
}

void net::engine::ShotVisRollback(uint32_t tick)
{
    for (auto& kv : gRecords)
    {
        Record& r = kv.second;
        if (r.done)
        {
            continue;
        }
        if ((int32_t)(r.tick - tick) > 0)
        {
            r.pending = true;
        }
        else
        {
            r.ended = false; // the restored world has it again
        }
    }
}

void net::engine::ShotVisFrame()
{
    const uint32_t now = Cycle();
    const Player* local = gGame_0x40 ? gGame_0x40->mLocalPlayer : NULL;
    const Sprite* me = local && local->mPlayerPed ? PedSprite(local->mPlayerPed) : NULL;
    const auto away = [&](float x, float y) {
        return me ? sqrtf((x - me->mXy.x.ToFloat()) * (x - me->mXy.x.ToFloat()) + (y - me->mXy.y.ToFloat()) * (y - me->mXy.y.ToFloat())) : 0.0f;
    };
    for (auto it = gRecords.begin(); it != gRecords.end();)
    {
        Record& r = it->second;
        See(r);
        const Sprite* ps = CharSprite(r.shooter);
        if (now - r.tick > kMostAge || (r.pending && !r.done))
        {
            it = gRecords.erase(it); // old enough, or never fired in the corrected game
            continue;
        }
        if (r.done)
        {
            ++it;
            continue;
        }
        r.done = true;
        // A shot's pellets share its flash
        auto last = gFlashTick.find(r.shooter);
        if (ps && On() && (last == gFlashTick.end() || last->second != r.tick) && !Flashing(ps))
        {
            PortShotFlash::Add(ps, now);
        }
        gFlashTick[r.shooter] = r.tick;
        if (!r.ended && ps)
        {
            PortInterp::SlideFrom(r.s, ps, kMuzzle, On());
            if (LogOn())
            {
                printf("net: SHOTVIS slide frame %u tick %u: model %d %u ticks old, %.1f blocks away\n", port_FrameCount() + 1, now, r.model,
                       now - r.tick, away(r.x, r.y));
            }
        }
        else if (LogOn())
        {
            printf("net: SHOTVIS unseen frame %u tick %u: model %d made %u, %s, %.1f blocks away\n", port_FrameCount() + 1, now, r.model, r.tick,
                   r.seen ? "seen at a tick" : "gone in its first", away(r.x, r.y));
        }
        ++it;
    }
    PortShotFlash::SetTick(now);
}

void net::engine::ClearShotVis()
{
    gRecords.clear();
    gMadeThisTick.clear();
    gFlashTick.clear();
    PortShotFlash::Clear();
}
