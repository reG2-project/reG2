// GTA2_NET_COMFORT (comfort.hpp): deaths and knock-downs, shots and drawn moves, as "comfort:" lines; the client also
// counts its input's response and the corrections. The script joins the server's lines with a client's by tick.
#include "comfort.hpp"
#include "../internal.hpp"
#include "port/src/interpolation.hpp"
#include "Source/Car.hpp"
#include "Source/Char_Pool.hpp"
#include "Source/Object_5C.hpp"
#include "Source/ObjectPool.hpp"
#include "Source/Ped.hpp"
#include "Source/PedRefTable.hpp"
#include "Source/Player.hpp"
#include "Source/enums.hpp"
#include "Source/sprite.hpp"
#include <chrono>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace
{
using namespace net::engine;

const float kView = 8; // blocks: a camera shows 6 to 8
const float kSeen = 10; // blocks: a correction farther from the local player isn't seen
const uint32_t kOwnCarId = 0xFFFFFFFF;

long long NowMs()
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

// 0 up, 1 down (alive and immobilized: knocked down, held, drowning, shocked), 2 dead
int LifeOf(const Ped* p)
{
    return p->isDead() ? 2 : p->mPedState1 == ped_state_1::immobilized_8 ? 1 : 0;
}

class Lives
{
  public:
    void See(uint32_t id, int state, float dist) // an id seen twice (a player's respawn): the living one
    {
        auto it = mNow.find(id);
        if (it == mNow.end() || state < it->second.first)
        {
            mNow[id] = std::make_pair(state, dist);
        }
    }
    void EndFrame(const char* end, uint32_t tick)
    {
        const long long ms = NowMs();
        for (const auto& n : mNow)
        {
            auto it = mStates.find(n.first);
            const bool fresh = it == mStates.end();
            if (fresh ? n.second.first != 0 : it->second != n.second.first)
            {
                printf("comfort: %s life %u %lld %u %d %d %.1f\n", end, tick, ms, n.first, n.second.first, fresh ? 1 : 0, n.second.second);
            }
        }
        mStates.clear();
        for (const auto& n : mNow)
        {
            mStates[n.first] = n.second.first;
        }
        mNow.clear();
    }

  private:
    std::unordered_map<uint32_t, int> mStates;
    std::unordered_map<uint32_t, std::pair<int, float>> mNow;
};

// A shot: a projectile or a gun's bullet (what a flamethrower sprays isn't counted)
bool ShotModel(int m)
{
    return m == objects::rocket_bullet_128 || m == objects::moving_molotov_138 || m == objects::grenade_obj_183 ||
           m == objects::pistol_bullet_265 || m == objects::machine_gun_bullet_254 || m == objects::shotgun_bullet_192 ||
           m == objects::fire_truck_gun_bullet_199 || m == objects::tank_cannon_148;
}

class Shots
{
  public:
    template <typename OwnerF> void Frame(const char* end, uint32_t tick, float lx, float ly, bool local, OwnerF owner)
    {
        // Keyed by its slot: a restore puts an object back in it, while its id can differ after a resimulation
        std::unordered_map<const Object*, Shot> now;
        std::vector<const Object*> fresh;
        for (Object& o : gObject_2C_Pool->mPool)
        {
            if (!o.mSprite || !o.mDefinition || o.mRemovalState || !ShotModel(o.mModel))
            {
                continue;
            }
            Shot s = {o.mModel, owner(&o), o.mSprite->mXy.x.ToFloat(), o.mSprite->mXy.y.ToFloat(), 0, 0, 0};
            auto it = mShots.find(&o);
            if (it != mShots.end() && Error(it->second, s) < 1.0f)
            {
                s.vx = s.x - it->second.x;
                s.vy = s.y - it->second.y;
                s.printed = it->second.printed;
                it->second.printed = 0; // taken
            }
            else
            {
                fresh.push_back(&o);
            }
            now[&o] = s;
        }
        // Made again in another slot (a client's resimulation): the same shot, the one nearest where it would be
        for (const Object* k : fresh)
        {
            Shot& s = now[k];
            Shot* best = NULL;
            float bestError = 0;
            for (auto& g : mShots)
            {
                const float e = Error(g.second, s), reach = g.second.vx || g.second.vy ? 1.0f : 2.0f;
                if (g.second.printed && e < reach && (!best || e < bestError))
                {
                    best = &g.second, bestError = e;
                }
            }
            if (best)
            {
                s.printed = best->printed;
                s.vx = best->vx;
                s.vy = best->vy;
                best->printed = 0;
                continue;
            }
            s.printed = ++mCount;
            const float dx = s.x - lx, dy = s.y - ly;
            printf("comfort: %s shot+ %u %u %d %u %.2f %.2f %.1f\n", end, tick, s.printed, s.model, s.owner, s.x, s.y,
                   local ? sqrtf(dx * dx + dy * dy) : 0.0f);
        }
        for (const auto& g : mShots)
        {
            if (g.second.printed)
            {
                printf("comfort: %s shot- %u %u\n", end, tick, g.second.printed);
            }
        }
        mShots.swap(now);
    }

  private:
    struct Shot
    {
        int model;
        uint32_t owner;
        float x, y, vx, vy;
        uint32_t printed; // its number in the log
    };
    // How far <s> is from where <g> would be a tick on (of another model or owner: far)
    static float Error(const Shot& g, const Shot& s)
    {
        const float ex = g.x + g.vx - s.x, ey = g.y + g.vy - s.y;
        return g.model == s.model && g.owner == s.owner ? sqrtf(ex * ex + ey * ey) : 1e9f;
    }
    std::unordered_map<const Object*, Shot> mShots;
    uint32_t mCount = 0;
};

// Drawn moves frame to frame against the step before (constant motion): how often one is off by over a quarter block
// and over a block; over 2 blocks in a frame is a teleport (a respawn, a jump)
enum MoveKind
{
    kOwnPlayer,
    kOwnCar,
    kPlayers,
    kCars,
    kPeds,
    kMoveKinds
};

class Moves
{
  public:
    void Feed(int kind, uint32_t id, float x, float y, uint32_t frame)
    {
        Track t = {x, y, 0, 0, frame, 0};
        auto it = mTracks.find(id);
        if (it != mTracks.end() && it->second.frame + 1 == frame)
        {
            const Track& o = it->second;
            const float sx = x - o.x, sy = y - o.y;
            Kind& k = mKinds[kind];
            if (sx * sx + sy * sy > 4)
            {
                k.teleports++;
            }
            else
            {
                if (o.steps)
                {
                    const float ex = sx - o.vx, ey = sy - o.vy, e = sqrtf(ex * ex + ey * ey);
                    k.frames++;
                    k.over[0] += e > 0.25f;
                    k.over[1] += e > 1;
                }
                t.vx = sx;
                t.vy = sy;
                t.steps = o.steps + 1;
            }
        }
        mNext[id] = t;
    }
    void EndFrame()
    {
        mTracks.swap(mNext);
        mNext.clear();
    }
    void Print(const char* end, uint32_t frame) const
    {
        static const char* const kName[kMoveKinds] = {"own_player", "own_car", "players", "cars", "peds"};
        for (int i = 0; i < kMoveKinds; i++)
        {
            const Kind& k = mKinds[i];
            printf("comfort: %s moves %u %s %u %u %u %u\n", end, frame, kName[i], k.frames, k.over[0], k.over[1], k.teleports);
        }
    }

  private:
    struct Track
    {
        float x, y, vx, vy;
        uint32_t frame;
        uint32_t steps;
    };
    struct Kind
    {
        uint32_t frames = 0, over[2] = {0, 0}, teleports = 0;
    };
    std::unordered_map<uint32_t, Track> mTracks, mNext;
    Kind mKinds[kMoveKinds];
};

// Frames from a press (forward from standing still, left, right) to the drawn player or its car moving that way: the
// input chosen at frame F runs in the tick drawn at F + 1
class Response
{
  public:
    void Frame(uint32_t frame, uint32_t input, bool valid, bool inCar, const Car* car, float x, float y, int rot)
    {
        const bool cont = valid && mValid && inCar == mInCar && car == mCar && mFrame + 1 == frame;
        float speed = 0;
        int drot = 0;
        if (cont)
        {
            speed = sqrtf((x - mX) * (x - mX) + (y - mY) * (y - mY));
            drot = ((rot - mRot) % 1440 + 1440 + 720) % 1440 - 720;
        }
        for (size_t i = 0; i < mPending.size();)
        {
            Pending& p = mPending[i];
            const uint32_t late = frame - p.frame;
            const bool moved = p.bit == 0x1 ? speed > (inCar ? 0.001f : 0.005f) : p.bit == 0x4 ? drot > p.drot : drot < p.drot;
            Stat& s = mStats[p.inCar ? p.bit == 0x1 ? 1 : 2 : 0];
            if (!cont || p.inCar != inCar)
            {
                mPending.erase(mPending.begin() + i); // the player changed (dead, into a car): not a response
            }
            else if (moved)
            {
                Press(frame, p, (int)late);
                s.n++;
                s.hist[late < 6 ? late : 6]++;
                mPending.erase(mPending.begin() + i);
            }
            else if (late >= 15)
            {
                Press(frame, p, -1);
                s.timeouts++; // blocked (a wall, a car at rest turning)
                mPending.erase(mPending.begin() + i);
            }
            else
            {
                i++;
            }
        }
        if (cont)
        {
            for (uint32_t bit : {0x1u, 0x4u, 0x8u})
            {
                // Forward from standing still; a turn on foot, or in a car going fast enough to turn
                const bool clear = bit == 0x1 ? speed < 0.005f : (input & 0xC) != 0xC && (!inCar || speed > 0.05f);
                if ((input & bit) && !(mInput & bit) && clear)
                {
                    mPending.push_back(Pending{frame, bit, inCar, drot});
                }
            }
        }
        mValid = valid;
        mInCar = inCar;
        mCar = car;
        mFrame = frame;
        mX = x;
        mY = y;
        mRot = rot;
        mInput = input;
    }
    void Print(uint32_t frame) const
    {
        static const char* const kNames[3] = {"foot", "carfwd", "carturn"};
        for (int c = 0; c < 3; c++)
        {
            const Stat& s = mStats[c];
            printf("comfort: C resp %u %s %u %u %u %u %u %u %u %u %u\n", frame, kNames[c], s.n, s.timeouts, s.hist[0], s.hist[1],
                   s.hist[2], s.hist[3], s.hist[4], s.hist[5], s.hist[6]);
        }
    }

  private:
    struct Pending
    {
        uint32_t frame, bit;
        bool inCar;
        int drot;
    };
    // One line a press in a car, for comfort.py's breakdown by car model: its frames to motion, -1 none
    void Press(uint32_t frame, const Pending& p, int late) const
    {
        if (p.inCar && mCar)
        {
            printf("comfort: C press %u %s %d %d\n", frame, p.bit == 0x1 ? "fwd" : "turn", late, (int)mCar->mCarInfoIdx);
        }
    }
    struct Stat
    {
        uint32_t n = 0, timeouts = 0, hist[7] = {0, 0, 0, 0, 0, 0, 0};
    };
    std::vector<Pending> mPending;
    Stat mStats[3]; // on foot, in a car forward from still, in a car turning
    bool mValid = false, mInCar = false;
    const Car* mCar = NULL;
    uint32_t mFrame = 0, mInput = 0;
    float mX = 0, mY = 0;
    int mRot = 0;
};

struct Corrections
{
    uint32_t n = 0, over[2] = {0, 0};
};

Lives gLives;
Shots gShots;
Moves gMoves;
Response gResponse;
Corrections gCorr[3]; // others, the local player and its car, another player placed over 2 blocks off (a respawn)
uint32_t gSyncedFrames;
uint32_t gSeen[3]; // frames of other players, cars and peds within kSeen of the local player
int gSlot = -1;
uint32_t gTurns[2]; // car updates in another order; those with the local player's car among the cars moved later

uint32_t ServerOwner(const Object* o)
{
    const s32 id = o->mPedRefIdx && o->mPedRefIdx < 255 && gPedRefTable_7F8 ? gPedRefTable_7F8->GetPedId(o->mPedRefIdx) : 0;
    Ped* p = id && gPedManager ? gPedManager->PedById(id) : NULL;
    return p ? PedNetId(p) : 0;
}
} // namespace

bool net::engine::ComfortOn()
{
    static const bool on = getenv("GTA2_NET_COMFORT") != NULL;
    return on;
}

void net::engine::ComfortServerTick(uint32_t tick)
{
    if (!ComfortOn())
    {
        return;
    }
    std::vector<std::pair<float, float>> players;
    for (int i = 0; i < PlayerCount(); i++)
    {
        Ped* ped = InGamePed(i);
        if (Sprite* s = ped ? PedSprite(ped) : NULL)
        {
            players.push_back(std::make_pair(s->mXy.x.ToFloat(), s->mXy.y.ToFloat()));
        }
    }
    const auto nearby = [&](const Sprite* s) {
        for (const auto& p : players)
        {
            const float dx = s->mXy.x.ToFloat() - p.first, dy = s->mXy.y.ToFloat() - p.second;
            if (dx * dx + dy * dy < kView * kView)
            {
                return true;
            }
        }
        return false;
    };
    for (Ped* p = gPedPool->mPool.mpPrev; p; p = p->mpNext)
    {
        if (p->mPlayer && p != p->mPlayer->mPlayerPed)
        {
            continue; // a player's old body: its id is the live ped's
        }
        gLives.See(PedNetId(p), LifeOf(p), 0);
        Sprite* s = !p->mCar ? CharSprite(p) : NULL;
        if (s && (p->mPlayer || nearby(s)))
        {
            gMoves.Feed(p->mPlayer ? kPlayers : kPeds, PedNetId(p), s->mXy.x.ToFloat(), s->mXy.y.ToFloat(), tick);
        }
    }
    gLives.EndFrame("S", tick);
    for (Car* c = gCar_BC_Pool->mPool.mpPrev; c; c = c->mpNext)
    {
        if (c->mCarSprite && !c->IsTrainModel() && nearby(c->mCarSprite))
        {
            gMoves.Feed(kCars, kCarIdBase + c->mCarId, c->mCarSprite->mXy.x.ToFloat(), c->mCarSprite->mXy.y.ToFloat(), tick);
        }
    }
    gMoves.EndFrame();
    gShots.Frame("S", tick, 0, 0, false, ServerOwner);
    if (tick % 300 == 0)
    {
        gMoves.Print("S", tick);
        fflush(stdout);
    }
}

void net::engine::ComfortClientFrame(uint32_t frame, uint32_t tick, int slot, uint32_t input)
{
    if (!ComfortOn())
    {
        return;
    }
    gSyncedFrames++;
    gSlot = slot;
    Ped* me = InGamePed(slot);
    Sprite* ms = me ? PedSprite(me) : NULL;
    const float lx = ms ? ms->mXy.x.ToFloat() : 0, ly = ms ? ms->mXy.y.ToFloat() : 0;
    const auto dist = [&](const Sprite* s) {
        const float dx = s->mXy.x.ToFloat() - lx, dy = s->mXy.y.ToFloat() - ly;
        return sqrtf(dx * dx + dy * dy);
    };
    std::unordered_set<Ped*> pool;
    for (Ped* p = gPedPool->mPool.mpPrev; p; p = p->mpNext)
    {
        pool.insert(p);
    }
    std::unordered_map<const Ped*, uint32_t> netIds;
    for (int i = 0; i < PlayerCount(); i++)
    {
        if (Ped* p = InGamePed(i))
        {
            netIds[p] = (uint32_t)i + 1;
            Sprite* s = PedSprite(p);
            gLives.See((uint32_t)i + 1, LifeOf(p), s ? dist(s) : 0);
        }
    }
    for (const auto& q : ReplicaPeds())
    {
        if (pool.count(q.second) && !q.second->mPlayer)
        {
            netIds[q.second] = q.first;
            Sprite* s = PedSprite(q.second);
            if (s && dist(s) < 12)
            {
                gLives.See(q.first, LifeOf(q.second), dist(s));
            }
        }
    }
    gLives.EndFrame("C", tick);
    const Car* myCar = me && me->mCar && me->mCar->mDriver == me ? me->mCar : NULL;
    std::vector<Pose> poses;
    Poses(poses);
    for (Pose& p : poses)
    {
        int32_t ox, oy, orot; // as drawn
        if (!IsCarId(p.id) && PedOffset(p.id, &ox, &oy, &orot))
        {
            p.x += ox;
            p.y += oy;
        }
        const float dx = Blocks(p.x) - lx, dy = Blocks(p.y) - ly;
        const bool inView = dx * dx + dy * dy < kView * kView;
        if (dx * dx + dy * dy < kSeen * kSeen && p.id != (uint32_t)slot + 1 && !(IsCarId(p.id) && me && me->mCar && ReplicaCar(p.id) == me->mCar))
        {
            gSeen[IsPlayerId(p.id) ? 0 : IsCarId(p.id) ? 1 : 2]++;
        }
        if (p.id == (uint32_t)slot + 1)
        {
            gMoves.Feed(kOwnPlayer, p.id, Blocks(p.x), Blocks(p.y), frame);
        }
        else if (IsPlayerId(p.id))
        {
            gMoves.Feed(kPlayers, p.id, Blocks(p.x), Blocks(p.y), frame);
        }
        else if (inView && !(IsCarId(p.id) && me && me->mCar && ReplicaCar(p.id) == me->mCar))
        {
            gMoves.Feed(IsCarId(p.id) ? kCars : kPeds, p.id, Blocks(p.x), Blocks(p.y), frame);
        }
    }
    if (me && me->mCar && ms)
    {
        gMoves.Feed(kOwnCar, kOwnCarId - (uint32_t)(me->mCar->mCarId & 0xFFFF), ms->mXy.x.ToFloat(), ms->mXy.y.ToFloat(), frame);
    }
    gMoves.EndFrame();
    gShots.Frame("C", tick, lx, ly, true, [&](const Object* o) {
        const s32 id = o->mPedRefIdx && o->mPedRefIdx < 255 && gPedRefTable_7F8 ? gPedRefTable_7F8->GetPedId(o->mPedRefIdx) : 0;
        Ped* p = id && gPedManager ? gPedManager->PedById(id) : NULL;
        auto it = p ? netIds.find(p) : netIds.end();
        return it != netIds.end() ? it->second : 0u;
    });
    // Another player's shot first drawn: its distance from the shooter's drawn gun, without and with the slide
    for (float would, drawn; PortInterp::TakeSlideMeasure(&would, &drawn);)
    {
        printf("comfort: C shotdraw %u %.2f %.2f\n", frame, would, drawn);
    }
    const bool valid = ms && me && !me->isDead() && me->mPedState1 != ped_state_1::immobilized_8 && (!me->mCar || myCar);
    gResponse.Frame(frame, input, valid, me && me->mCar, me ? me->mCar : NULL, valid ? ms->mXy.x.ToFloat() : 0,
                    valid ? ms->mXy.y.ToFloat() : 0, valid ? ms->mRotation.rValue : 0);
    if (frame % 300 == 0)
    {
        gMoves.Print("C", frame);
        gResponse.Print(frame);
        for (int i = 0; i < 3; i++)
        {
            printf("comfort: C corr %u %s %u %u %u\n", frame, i == 2 ? "placed" : i ? "self" : "others", gCorr[i].n, gCorr[i].over[0], gCorr[i].over[1]);
        }
        printf("comfort: C seen %u %u %u %u\n", frame, gSeen[0], gSeen[1], gSeen[2]);
        printf("comfort: C turns %u %u %u\n", frame, gTurns[0], gTurns[1]);
        printf("comfort: C synced %u %u\n", frame, gSyncedFrames);
        fflush(stdout);
    }
}

void net::engine::ComfortCorrection(uint32_t id, float blocks, float x, float y)
{
    if (!ComfortOn() || blocks <= 0 || gSlot < 0)
    {
        return;
    }
    Ped* me = InGamePed(gSlot);
    Sprite* ms = me ? PedSprite(me) : NULL;
    const float dx = ms ? x - ms->mXy.x.ToFloat() : 0, dy = ms ? y - ms->mXy.y.ToFloat() : 0;
    if (dx * dx + dy * dy > kSeen * kSeen)
    {
        return; // out of sight
    }
    const bool self = id == (uint32_t)gSlot + 1 || (IsCarId(id) && me && me->mCar && ReplicaCar(id) == me->mCar);
    Corrections& c = gCorr[!self && IsPlayerId(id) && blocks > 2 ? 2 : self ? 1 : 0];
    c.n++;
    c.over[0] += blocks > 0.25f;
    c.over[1] += blocks > 1;
    if (blocks > 0.25f && !self)
    {
        // One line each for comfort.py's breakdown: kind 0 player, 1 car, 2 the server's ped, 3 the client's own ped
        const Ped* p = IsPlayerId(id) ? InGamePed((int)id - 1) : IsCarId(id) ? NULL : ReplicaPed(id);
        const Car* car = IsCarId(id) ? ReplicaCar(id) : NULL;
        const int kind = IsPlayerId(id) ? 0 : car ? 1 : p && IsServerPed(p) ? 2 : 3;
        printf("comfort: C cv %u %u %.2f %d %d %d %d %d %d %.1f\n", gSyncedFrames, id, blocks, kind, p ? p->mPedState1 : -1,
               p ? p->mPedState2 : -1, p ? PhaseOf(p) : -1, car && car->mDriver ? (car->mDriver->mPlayer ? 2 : 1) : 0,
               car ? car->IsTrainModel() : 0, sqrtf(dx * dx + dy * dy));
    }
}

void net::engine::ComfortCarTurns(const std::vector<Car*>& cars, const std::vector<int>& order)
{
    if (!ComfortOn() || gSlot < 0)
    {
        return;
    }
    gTurns[0]++;
    Ped* me = InGamePed(gSlot);
    for (size_t k = 0, last = 0; k < order.size() && me && me->mCar; k++)
    {
        if (cars[order[k]] == me->mCar)
        {
            gTurns[1] += (size_t)order[k] < last; // after a car the pool has after it
            break;
        }
        last = (size_t)order[k] > last ? order[k] : last;
    }
}
