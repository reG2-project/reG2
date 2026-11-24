// port/net: the bot's view (what its client knows) and the simple bot (bot.hpp).
#include "bot.hpp"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>

namespace
{
const float kPi = 3.14159265f;

// The angle error from facing <rot> to facing the direction (dx, dy), in game units (-720..720)
int TurnTo(int rot, float dx, float dy)
{
    int err = (int)(atan2f(dx, dy) * 720.0f / kPi) - rot;
    while (err > 720)
    {
        err -= 1440;
    }
    while (err < -720)
    {
        err += 1440;
    }
    return err;
}

// 20 degrees either way is straight on: with less, one tick's turn overshot and the bot pressed left and right in
// turn, every tick, as no person does (a client predicting it from its last input was then wrong every tick)
uint32_t Steer(int err)
{
    return err > 80 ? net::kBtnLeft : err < -80 ? net::kBtnRight : 0;
}

// The nearest of a kind (players: living ones, on foot if asked; pickups: weapons only, if asked; with a walker, the
// ones it can walk to and hasn't given up on); NULL if none
const net::BotView::Seen* Nearest(const net::BotView& v, net::BotView::Seen::Kind kind, float* pDist, bool weapons = false,
                                  const net::BotWalker* walker = NULL, bool onFoot = false)
{
    const net::BotView::Seen* best = NULL;
    *pDist = 1e9f;
    for (const net::BotView::Seen& s : v.seen)
    {
        const float d = hypotf(s.x - v.x, s.y - v.y);
        if (s.kind == kind && d < *pDist && (kind != net::BotView::Seen::kPlayer || s.health > 0) && (!weapons || s.weapon) && !(onFoot && s.inCar) &&
            (!walker || (!walker->Skipped(s.id, v.frame) && walker->Reachable(v, s.x, s.y, s.z))))
        {
            *pDist = d;
            best = &s;
        }
    }
    return best;
}

// On foot, to the nearest car and in: walking at it, then a press now and then (the game walks us to the door), or
// looking for one
uint32_t ApproachCar(const net::BotView& v, net::BotWalker& walker)
{
    float d = 0;
    const net::BotView::Seen* car = Nearest(v, net::BotView::Seen::kCar, &d, false, &walker);
    if (!car)
    {
        // None in sight (a client sees cars within its area only): towards someone, where there are roads and cars
        const net::BotView::Seen* p = Nearest(v, net::BotView::Seen::kPlayer, &d, false, &walker);
        return p ? walker.Walk(v, p->id, p->x, p->y, p->z) : net::kBtnForward | (v.frame % 90 < 20 ? net::kBtnLeft : 0);
    }
    if (d < 2.5f)
    {
        return v.frame % 20 < 2 ? net::kBtnEnterExit : 0;
    }
    return walker.Walk(v, car->id, car->x, car->y, car->z);
}

// Nothing in the way of walking or shooting straight at it
bool InLine(const net::BotView& v, const net::BotView::Seen& s)
{
    return !v.map || net::NavClear(*v.map, v.x, v.y, s.x, s.y);
}

// What a weapon is worth on foot (Source/enums.hpp, weapon_type), 0 for none: machine guns, shotgun and rockets most
float WeaponValue(int w)
{
    static const float kValue[15] = {1.0f, 2.0f, 2.5f, 0.6f, 1.2f, 1.2f, 2.0f, 0.5f, 1.8f, 2.0f, 1.5f, 1.0f, 1.0f, 1.0f, 1.0f};
    return w >= 0 && w < 15 ? kValue[w] : 0;
}

// Molotovs and grenades: thrown, as far as the attack button was held (Weapon::ChuckThrowable)
bool Throwable(int w)
{
    return w == 4 || w == 5;
}

// It has a machine gun, a shotgun, a flamethrower or rockets
bool WellArmed(const net::BotView& v)
{
    for (int w = 0; w < 15; w++)
    {
        if ((v.gunMask >> w & 1) && WeaponValue(w) >= 1.8f)
        {
            return true;
        }
    }
    return false;
}

// How near a throw may land: a grenade's blast is the larger
float ThrowNearest(int w)
{
    return w == 5 ? 3.0f : 2.2f;
}

// Worth firing at <d> blocks: rockets from 4 (their blast), a throw from ThrowNearest to 4.6 (Thrower)
bool InRange(int w, float d)
{
    return w == 2 ? d >= 4 : Throwable(w) ? d >= ThrowNearest(w) && d <= 4.6f : true;
}

// The weapon to have in hand against a target <d> blocks away: the best in range (rockets first at a car), else the
// best it has (to get in range with); -1 with none
int WantedWeapon(const net::BotView& v, bool targetInCar, float d)
{
    int best = -1;
    float bestValue = 0;
    bool bestIn = false;
    for (int w = 0; w < 15; w++)
    {
        const float value = WeaponValue(w) + (w == 2 && targetInCar ? 2 : 0);
        const bool in = InRange(w, d);
        if ((v.gunMask >> w & 1) && ((in && !bestIn) || (in == bestIn && value > bestValue)))
        {
            best = w, bestValue = value, bestIn = in;
        }
    }
    return best;
}

// Where the players were when it last saw them: their speed, in blocks per tick
class Motions
{
  public:
    void See(const net::BotView& v)
    {
        for (const net::BotView::Seen& s : v.seen)
        {
            if (s.kind != net::BotView::Seen::kPlayer)
            {
                continue;
            }
            Last* l = NULL;
            for (Last& m : mLast)
            {
                l = m.id == s.id ? &m : l;
            }
            if (!l)
            {
                mLast.push_back(Last{s.id, v.frame, s.x, s.y, 0, 0});
                continue;
            }
            const uint32_t dt = v.frame - l->frame;
            if (dt > 0 && dt < 15)
            {
                const float k = dt >= 3 ? 1.0f : 0.5f; // smoothed when seen every tick
                l->vx += k * ((s.x - l->x) / dt - l->vx), l->vy += k * ((s.y - l->y) / dt - l->vy);
            }
            else if (dt)
            {
                l->vx = l->vy = 0;
            }
            l->frame = v.frame, l->x = s.x, l->y = s.y;
        }
    }
    void Velocity(uint32_t id, float* pVx, float* pVy) const
    {
        *pVx = *pVy = 0;
        for (const Last& m : mLast)
        {
            if (m.id == id)
            {
                *pVx = m.vx, *pVy = m.vy;
            }
        }
    }

  private:
    struct Last
    {
        uint32_t id, frame;
        float x, y, vx, vy;
    };
    std::vector<Last> mLast;
};

// Throws standing, turned to where the target will be, attack held for that far (measured: a molotov lands 0.68 + 0.0567
// blocks per tick held, 60 at most, 33 ticks on; a grenade as far, (96 - held) * 0.75 ticks on, in the hand at 96)
class Thrower
{
  public:
    uint32_t Think(const net::BotView& v, const net::BotView::Seen& target, const Motions& motions, net::BotWalker& walker)
    {
        float vx = 0, vy = 0, lx = target.x, ly = target.y, held = 30;
        motions.Velocity(target.id, &vx, &vy);
        for (int i = 0; i < 2; i++) // where it will be when a throw that far lands: held, then in flight
        {
            const float d = hypotf(lx - v.x, ly - v.y), flight = v.weapon == 5 ? (96 - held) * 0.75f : 33.0f;
            const float t = flight + (mHolding ? fmaxf(held - (float)(v.frame - mFrom), 0) : held);
            held = Clamp((d - 0.68f) / 0.0567f, 4, 60);
            lx = target.x + vx * t, ly = target.y + vy * t;
        }
        const float d = hypotf(target.x - v.x, target.y - v.y), lead = hypotf(lx - v.x, ly - v.y);
        const int err = TurnTo(v.rot, lx - v.x, ly - v.y);
        uint32_t aim = Steer(err);
        if (!aim && abs(err) > 16 && (v.frame / net::kBotDecisionTicks) % 2)
        {
            aim = err > 0 ? net::kBtnLeft : net::kBtnRight; // nearly there: short turns, which don't overshoot
        }
        const bool tooNear = d < ThrowNearest(v.weapon) || lead < ThrowNearest(v.weapon) - 0.3f;
        if (mHolding)
        {
            // Let go once held for as far as it is now; too near, it backs off holding on (a grenade not past 85 ticks)
            const uint32_t now = v.frame - mFrom;
            if (v.frame < mFrom || (v.weapon == 5 && now >= 85) || (!tooNear && now >= (uint32_t)Clamp((lead - 0.68f) / 0.0567f, 4, 60)))
            {
                mHolding = false, mRestUntil = v.frame + 12;
                return aim;
            }
            return aim | net::kBtnAttack | (tooNear ? net::kBtnBack : 0);
        }
        if (d > 4.6f || lead > 4.3f)
        {
            return walker.Walk(v, target.id, target.x, target.y, target.z);
        }
        if (tooNear)
        {
            return aim | net::kBtnBack;
        }
        if (v.frame < mRestUntil || abs(err) > 40)
        {
            return aim;
        }
        mHolding = true, mFrom = v.frame;
        return aim | net::kBtnAttack;
    }
    bool Holding() const
    {
        return mHolding;
    }

  private:
    static float Clamp(float x, float lo, float hi)
    {
        return x < lo ? lo : x > hi ? hi : x;
    }
    bool mHolding = false;
    uint32_t mFrom = 0, mRestUntil = 0;
};

// A car within <reach> blocks of its box (at its level): it would punch that, not a body
bool CarAt(const net::BotView& v, float reach)
{
    for (const net::BotView::Seen& s : v.seen)
    {
        if (s.kind == net::BotView::Seen::kCar && fabsf(s.z - v.z) < 1.0f && hypotf(s.x - v.x, s.y - v.y) < s.length * 0.5f + reach)
        {
            return true;
        }
    }
    return false;
}

// The weapon pickups it knows of, by place: the map's (its game's list) from the start, then as seen: one seen again is
// refreshed, one not there while it stands within 8 blocks (in what its client sees) is taken, and left alone for 30 s
class PickupMemory
{
  public:
    void See(const net::BotView& v)
    {
        for (size_t i = 0; v.pickups && i < v.pickups->size(); i++)
        {
            const net::engine::PickupSpot& p = (*v.pickups)[i];
            if (!Find(p.x, p.y) && mSpots.size() < kMaxSpots)
            {
                net::BotView::Seen s = {};
                s.kind = net::BotView::Seen::kPickup, s.id = kListedId + (uint32_t)i, s.player = -1;
                s.x = p.x, s.y = p.y, s.z = p.z, s.weapon = true, s.model = p.model, s.held = -1;
                mSpots.push_back(Spot{s, v.frame, 0});
            }
        }
        for (const net::BotView::Seen& s : v.seen)
        {
            if (s.kind != net::BotView::Seen::kPickup || !s.weapon)
            {
                continue;
            }
            Spot* spot = Find(s.x, s.y);
            if (!spot && mSpots.size() < kMaxSpots)
            {
                mSpots.push_back(Spot());
                spot = &mSpots.back();
            }
            if (spot)
            {
                spot->s = s, spot->seenAt = v.frame, spot->emptyUntil = 0;
            }
        }
        for (Spot& spot : mSpots)
        {
            if (spot.seenAt != v.frame && v.frame >= spot.emptyUntil && hypotf(spot.s.x - v.x, spot.s.y - v.y) < 8)
            {
                spot.emptyUntil = v.frame + 900;
            }
        }
    }
    // The best to fetch: nearest for its worth (half for a kind it has), one it can walk to and hasn't given up on
    const net::BotView::Seen* Best(const net::BotView& v, const net::BotWalker& walker, float* pDist) const
    {
        const net::BotView::Seen* best = NULL;
        float bestScore = 1e9f;
        for (const Spot& spot : mSpots)
        {
            const int w = spot.s.model - 200;
            const float d = hypotf(spot.s.x - v.x, spot.s.y - v.y);
            const float score = d / (WeaponValue(w) * (v.gunMask >> w & 1 ? 0.5f : 1.0f));
            if (v.frame >= spot.emptyUntil && score < bestScore && !walker.Skipped(spot.s.id, v.frame) &&
                walker.Reachable(v, spot.s.x, spot.s.y, spot.s.z))
            {
                bestScore = score, *pDist = d, best = &spot.s;
            }
        }
        return best;
    }

  private:
    static const size_t kMaxSpots = 128;
    static const uint32_t kListedId = 0xFE000000; // a listed spot's id until seen (no entity's: engine.hpp's id bases)
    struct Spot
    {
        net::BotView::Seen s;
        uint32_t seenAt, emptyUntil;
    };
    Spot* Find(float x, float y)
    {
        for (Spot& spot : mSpots)
        {
            if (fabsf(spot.s.x - x) < 0.5f && fabsf(spot.s.y - y) < 0.5f)
            {
                return &spot;
            }
        }
        return NULL;
    }
    std::vector<Spot> mSpots;
};

// Cars driven at it: each car's move since the last tick it saw it, and the way out of one coming within a block in
// the next 20 ticks (a jump when it is about to hit, else a step aside); 0 if none
class CarDodge
{
  public:
    uint32_t Think(const net::BotView& v)
    {
        uint32_t b = 0;
        std::vector<Last> now;
        for (const net::BotView::Seen& s : v.seen)
        {
            if (s.kind != net::BotView::Seen::kCar || hypotf(s.x - v.x, s.y - v.y) > 12)
            {
                continue;
            }
            for (const Last& l : mLast)
            {
                if (l.id != s.id || v.frame - l.frame > 3 || b || v.car)
                {
                    continue;
                }
                const float dt = (float)(v.frame - l.frame), vx = (s.x - l.x) / dt, vy = (s.y - l.y) / dt, v2 = vx * vx + vy * vy;
                const float px = v.x - s.x, py = v.y - s.y, t = (px * vx + py * vy) / (v2 > 1e-6f ? v2 : 1e-6f);
                const float mx = px - vx * t, my = py - vy * t; // from its line to the bot, where it passes nearest
                if (v2 < 0.08f * 0.08f || t <= 0 || t > 20 || hypotf(mx, my) > 1.0f)
                {
                    continue;
                }
                mDodges += v.frame - mLastDodge > 30;
                mLastDodge = v.frame;
                if (t < 6)
                {
                    b = net::kBtnJump | net::kBtnForward;
                    continue;
                }
                // Away from its line, on the side the bot already is (either side when on it)
                const float side = mx * vy - my * vx >= 0 ? 1.0f : -1.0f;
                b = net::kBtnForward | Steer(TurnTo(v.rot, side * vy, -side * vx));
            }
            now.push_back(Last{s.id, v.frame, s.x, s.y});
        }
        mLast.swap(now);
        return b;
    }
    int Dodges() const
    {
        return mDodges;
    }

  private:
    struct Last
    {
        uint32_t id, frame;
        float x, y;
    };
    std::vector<Last> mLast;
    uint32_t mLastDodge = 0;
    int mDodges = 0;
};

// A press now and then, not held: once every 4 decisions of 3 ticks
uint32_t Press(const net::BotView& v, uint32_t button)
{
    return (v.frame / net::kBotDecisionTicks) % 4 == 0 ? button : 0;
}

// A brain that decides its buttons each frame and remembers them for its log line
class DecidingBot : public net::BotBrain
{
  public:
    uint32_t Think(const net::BotView& v) override
    {
        static const bool sCarFire = getenv("GTA2_NET_BOTCARFIRE") != NULL; // a test's: car weapons fired while driving
        mButtons = Decide(v) | (sCarFire && v.car && v.frame / 30 % 3 == 0 ? net::kBtnAttack : 0);
        // A test's: a roof turret turned while driving (SPECIAL with left), a second in three
        static const bool sTurret = getenv("GTA2_NET_BOTTURRET") != NULL;
        if (sTurret && v.car && v.frame / 30 % 3 == 1)
        {
            mButtons = (mButtons & ~(net::kBtnLeft | net::kBtnRight)) | net::kBtnSpecial | net::kBtnLeft;
        }
        return mButtons;
    }

  protected:
    virtual uint32_t Decide(const net::BotView& v) = 0;
    uint32_t mButtons = 0;
};

// Walks to the nearest player along its own route (replanned every second, or when the target moves to another
// cell), turns to face it and shoots when it is close; backs off and turns when stuck; jumps or steps out of the way of
// a car driven at it. Without a gun it fetches the best weapon it knows of, unless a fistfight is worth it: the target
// unarmed, above the 30 health under which punches only knock it down, and hurt by its punches lately.
class SimpleBot : public DecidingBot
{
  public:
    std::string Describe() const override
    {
        char line[192];
        snprintf(line, sizeof(line), "at (%.1f, %.1f) rot %d, target %.1f blocks away, route %zu cells, %d dodges, buttons 0x%x%s", mX, mY, mRot,
                 mTargetDist, mWalker.route.path.size(), mDodge.Dodges(), mButtons, mWalker.route.none ? " (no route found)" : "");
        return line;
    }
    // Without a good gun, and one worth fetching within 30 blocks
    bool WantsWeapon(const net::BotView& v) const
    {
        float d = 0;
        return !WellArmed(v) && mPickups.Best(v, mWalker, &d) && d < 30;
    }

  private:
    uint32_t Decide(const net::BotView& v) override
    {
        if (!v.hasBody || v.health <= 0)
        {
            return 0;
        }
        mX = v.x;
        mY = v.y;
        mRot = v.rot;
        mPickups.See(v);
        mMotions.See(v);
        if (const uint32_t dodge = mDodge.Think(v))
        {
            return dodge;
        }
        float best = 0;
        const net::BotView::Seen* target = Nearest(v, net::BotView::Seen::kPlayer, &best, false, &mWalker);
        const bool fists = v.guns == 0;
        if (mThrower.Holding() && target && Throwable(v.weapon))
        {
            return mThrower.Think(v, *target, mMotions, mWalker); // a throw under way: through
        }
        if (fists && target && target->inCar)
        {
            // Unarmed, someone driving: its car when it stands near (the game pulls it out), else someone on foot
            float vx = 0, vy = 0;
            mMotions.Velocity(target->id, &vx, &vy);
            if (best < 8 && hypotf(vx, vy) < 0.05f)
            {
                return best < 2.5f ? Press(v, net::kBtnEnterExit) : mWalker.Walk(v, target->id, target->x, target->y, target->z);
            }
            target = Nearest(v, net::BotView::Seen::kPlayer, &best, false, &mWalker, true);
        }
        mTargetDist = target ? best : -1;
        if (fists && target && best < 1.5f && target->id == mFightId)
        {
            if (target->health < mFightHealth)
            {
                mFightHealth = target->health, mFightSince = v.frame;
            }
            else if (v.frame - mFightSince > 120) // no harm done in 4 s: not worth it for a while
            {
                mNoFistsUntil = v.frame + 450;
            }
        }
        else if (fists && target && best < 1.5f)
        {
            mFightId = target->id, mFightHealth = target->health, mFightSince = v.frame;
        }
        const bool fistFight = fists && target && best < 1.5f && target->held < 0 && target->health > 30 && v.frame >= mNoFistsUntil;
        float weaponDist = 0;
        const net::BotView::Seen* weapon = fists && !fistFight ? mPickups.Best(v, mWalker, &weaponDist) : NULL;
        if (weapon)
        {
            return mWalker.Walk(v, weapon->id, weapon->x, weapon->y, weapon->z);
        }
        if (!target)
        {
            return 0;
        }
        const float dx = target->x - v.x, dy = target->y - v.y;
        if (best < 6.0f && InLine(v, *target)) // close: face it and fight
        {
            const int wanted = WantedWeapon(v, target->inCar, best);
            if (!fists && v.weapon != wanted && !mThrower.Holding())
            {
                return (best > 1.2f ? mWalker.Walk(v, target->id, target->x, target->y, target->z) : 0) | Press(v, net::kBtnNextWeapon);
            }
            if (Throwable(v.weapon))
            {
                return mThrower.Think(v, *target, mMotions, mWalker);
            }
            const int err = TurnTo(v.rot, dx, dy);
            // Fists hit a body within a quarter block (Ped::FindNearbyPed): onto it, with the button held, never at a
            // car. Closing in with the walker, which gets it unstuck.
            uint32_t b = best > (fists ? 0.2f : 1.2f) ? mWalker.Walk(v, target->id, target->x, target->y, target->z) : Steer(err);
            const bool fire = fists ? best < 0.5f && !target->inCar && !CarAt(v, 0.6f) : v.frame % 4 < 2 && InRange(v.weapon, best);
            if (abs(err) < 100 && fire)
            {
                b |= net::kBtnAttack;
            }
            return b;
        }
        return mWalker.Walk(v, target->id, target->x, target->y, target->z); // far, or round a wall
    }

    float mX = 0, mY = 0, mTargetDist = -1;
    int mRot = 0, mFightHealth = 0;
    uint32_t mFightId = 0, mFightSince = 0, mNoFistsUntil = 0;
    net::BotWalker mWalker;
    PickupMemory mPickups;
    CarDodge mDodge;
    Motions mMotions;
    Thrower mThrower;
};
// Gets into the nearest car, drives about 10 s with a turn now and then, gets out, and again
class CarHopBot : public DecidingBot
{
  public:
    std::string Describe() const override
    {
        char line[128];
        snprintf(line, sizeof(line), "%s, %d cars entered, %d left, buttons 0x%x", mInCar ? "driving" : "on foot", mEntered, mLeft, mButtons);
        return line;
    }

  private:
    uint32_t Decide(const net::BotView& v) override
    {
        if (!v.hasBody || v.health <= 0)
        {
            return 0;
        }
        const bool inCar = v.car != 0;
        if (inCar != mInCar)
        {
            mInCar = inCar;
            mEntered += inCar;
            mLeft += !inCar;
            mSince = v.frame;
        }
        const uint32_t press = v.frame % 20 < 2 ? net::kBtnEnterExit : 0; // a press now and then, not held
        if (inCar)
        {
            if (v.frame - mSince > 300)
            {
                return press; // out again
            }
            const int phase = (int)((v.frame - mSince) / 45) % 4;
            return net::kBtnForward | (phase == 1 ? net::kBtnLeft : phase == 3 ? net::kBtnRight : 0);
        }
        if (v.frame - mSince < 60)
        {
            return 0; // a moment on foot after getting out
        }
        return ApproachCar(v, mWalker);
    }

    bool mInCar = false;
    uint32_t mSince = 0;
    int mEntered = 0, mLeft = 0;
    net::BotWalker mWalker;
};
// Gets into the nearest car and drives at the nearest other player, to hit it; backs out with the wheel turned the
// other way when it hasn't moved for a second. Gets out when wedged or when it has to (a wreck, a death), then the next car.
class RamBot : public DecidingBot
{
  public:
    std::string Describe() const override
    {
        char line[128];
        snprintf(line, sizeof(line), "%s, %d cars entered, %d times backing out, buttons 0x%x", mInCar ? "driving" : "on foot", mEntered,
                 mReversals, mButtons);
        return line;
    }

  private:
    uint32_t Decide(const net::BotView& v) override
    {
        if (!v.hasBody || v.health <= 0)
        {
            return 0;
        }
        const bool inCar = v.car != 0;
        if (inCar != mInCar)
        {
            mInCar = inCar;
            mEntered += inCar;
        }
        if (!inCar)
        {
            mWedged = 0;
            return ApproachCar(v, mWalker);
        }
        float d = 0;
        const net::BotView::Seen* target = Nearest(v, net::BotView::Seen::kPlayer, &d);
        // Stuck: hardly moved over the last second. Stuck 4 s in a row with nobody pinned in front: out, and that car
        // left alone for a while.
        if (v.frame % 30 == 0)
        {
            const bool still = hypotf(v.x - mLastX, v.y - mLastY) < 0.5f;
            mLastX = v.x;
            mLastY = v.y;
            if (still && v.frame >= mBackUntil)
            {
                mBackUntil = v.frame + 30;
                mReversals++;
            }
            mWedged = still && (!target || d > 3) ? mWedged + 1 : 0;
            if (mWedged == 4)
            {
                mWalker.GiveUp(v, v.car);
            }
        }
        // Well armed: out to shoot someone on foot near, or with rockets a car (a car runs people over, but a gun
        // hits them as they dodge)
        const bool outToShoot = WellArmed(v) && target && (target->inCar ? (v.gunMask >> 2 & 1) && d < 8 : d < 6);
        if (mWedged >= 4 || outToShoot)
        {
            return v.frame % 20 < 2 ? net::kBtnEnterExit : 0;
        }
        const int err = target ? TurnTo(v.rot, target->x - v.x, target->y - v.y) : (v.frame / 60 % 2 ? 200 : -200); // none in sight: wander
        if (v.frame < mBackUntil)
        {
            return net::kBtnBack | (err > 0 ? net::kBtnRight : net::kBtnLeft); // backing, the wheel the other way
        }
        return net::kBtnForward | Steer(err);
    }

    bool mInCar = false;
    int mEntered = 0, mReversals = 0, mWedged = 0;
    float mLastX = 0, mLastY = 0;
    uint32_t mBackUntil = 0;
    net::BotWalker mWalker;
};
// The fighter on foot and the rammer in a car: takes a car when one is near, nobody is close and it has the weapon it
// wants, drives at people, and fights on foot otherwise (a teacher for the trained bots: one brain, so what it shows is consistent)
class HunterBot : public DecidingBot
{
  public:
    std::string Describe() const override
    {
        return (mDriving ? "driving: " + mCar.Describe() : "on foot: " + mFoot.Describe());
    }

  private:
    uint32_t Decide(const net::BotView& v) override
    {
        float enemy = 0, car = 0;
        Nearest(v, net::BotView::Seen::kPlayer, &enemy);
        Nearest(v, net::BotView::Seen::kCar, &car);
        if (!v.car && mWasInCar)
        {
            mNoCarUntil = v.frame + 300; // out to fight: on foot for a while
        }
        mWasInCar = v.car != 0;
        mDriving = v.car != 0 || (car < 10 && enemy > 8 && v.frame >= mNoCarUntil && !mFoot.WantsWeapon(v));
        // Both see every tick, so their timers and stuck checks stay right whichever decides
        const uint32_t foot = mFoot.Think(v), drive = mCar.Think(v);
        return mDriving ? drive : foot;
    }

    SimpleBot mFoot;
    RamBot mCar;
    bool mDriving = false, mWasInCar = false;
    uint32_t mNoCarUntil = 0;
};
// The whole round: a weapon first, then a car, then along its route to the nearest player, and there whatever suits:
// runs it over when coming at it fast and head on, else gets out and shoots. Pinned against someone in a car, it gets
// out too. On foot with a gun it closes in and fires while facing it (a car from 3 blocks); without one, it takes the
// car of someone driving near, else fetches a weapon. It doesn't punch.
class SoldierBot : public DecidingBot
{
  public:
    std::string Describe() const override
    {
        char line[160];
        snprintf(line, sizeof(line), "%s, target %.1f blocks away, speed %.2f, %d cars entered, %d left, buttons 0x%x", mInCar ? "driving" : "on foot",
                 mTargetDist, mSpeed, mEntered, mLeft, mButtons);
        return line;
    }

  private:
    uint32_t Decide(const net::BotView& v) override
    {
        if (!v.hasBody || v.health <= 0)
        {
            mBackUntil = 0;
            return 0;
        }
        if ((v.car != 0) != mInCar)
        {
            mInCar = v.car != 0;
            mEntered += mInCar;
            mLeft += !mInCar;
        }
        // Its speed in blocks per tick, smoothed over a few decisions (called every tick or every few)
        const uint32_t ticks = v.frame > mLastFrame ? v.frame - mLastFrame : 1;
        mSpeed = 0.7f * mSpeed + 0.3f * hypotf(v.x - mLastX, v.y - mLastY) / ticks;
        mLastX = v.x, mLastY = v.y, mLastFrame = v.frame;
        float d = 0;
        const net::BotView::Seen* target = Nearest(v, net::BotView::Seen::kPlayer, &d);
        mTargetDist = target ? d : -1;
        if (mInCar)
        {
            return Drive(v, target, d);
        }
        mBackUntil = 0;
        mPickups.See(v);
        mMotions.See(v);
        if (const uint32_t dodge = mDodge.Think(v))
        {
            return dodge;
        }
        if (mThrower.Holding() && target && Throwable(v.weapon))
        {
            return mThrower.Think(v, *target, mMotions, mWalker); // a throw under way: through
        }
        const net::BotView::Seen* walkable = Nearest(v, net::BotView::Seen::kPlayer, &d, false, &mWalker);
        target = walkable ? walkable : target;
        // Unarmed and someone drives near: its car, with it pulled out (the game does that on enter)
        if (target && target->inCar && !v.guns && d < 8)
        {
            return d < 2.5f ? Press(v, net::kBtnEnterExit) : Walk(v, *target);
        }
        // Unarmed: to a weapon whatever happens (fistfights between players get stuck, with no harm done)
        float far = 0;
        const net::BotView::Seen* weapon = v.guns ? NULL : mPickups.Best(v, mWalker, &far);
        if (weapon)
        {
            return Walk(v, *weapon);
        }
        if (target && d < 10 && v.guns && InLine(v, *target))
        {
            return Fight(v, *target, d);
        }
        const net::BotView::Seen* car = Nearest(v, net::BotView::Seen::kCar, &far, false, &mWalker);
        if (car && far < 15)
        {
            return far < 2.5f ? Press(v, net::kBtnEnterExit) : Walk(v, *car); // the game walks it to the door
        }
        return walkable ? Walk(v, *walkable) : 0;
    }

    uint32_t Walk(const net::BotView& v, const net::BotView::Seen& goal)
    {
        return mWalker.Walk(v, goal.id, goal.x, goal.y, goal.z);
    }

    // Up close and firing: a gun fires straight ahead, and from more than a couple of blocks most shots miss
    uint32_t Fight(const net::BotView& v, const net::BotView::Seen& target, float d)
    {
        const int err = TurnTo(v.rot, target.x - v.x, target.y - v.y);
        uint32_t b = Steer(err);
        if (!b && abs(err) > 20 && (v.frame / net::kBotDecisionTicks) % 2)
        {
            b = err > 0 ? net::kBtnLeft : net::kBtnRight; // nearly there: short turns, which don't overshoot
        }
        if (v.weapon != WantedWeapon(v, target.inCar, d) && !mThrower.Holding())
        {
            return (d > 1.5f ? Walk(v, target) : b) | Press(v, net::kBtnNextWeapon);
        }
        if (Throwable(v.weapon))
        {
            return mThrower.Think(v, target, mMotions, mWalker);
        }
        // Rockets blow it up too up close: from 4 blocks. A car under fire burns and blows up: from 3.
        const bool rockets = v.weapon == 2;
        const float nearest = rockets ? 4 : target.inCar ? 3 : 0.8f, farthest = rockets ? 7 : target.inCar ? 4 : 1.5f;
        if (d > farthest)
        {
            b = Walk(v, target); // the walker gets it unstuck
        }
        b |= d < nearest ? net::kBtnBack : 0;
        return b | (abs(err) <= 80 && InRange(v.weapon, d) ? net::kBtnAttack : 0); // held: guns fire while it is
    }

    uint32_t Drive(const net::BotView& v, const net::BotView::Seen* target, float d)
    {
        // Stuck: hardly moved over the last second: back out with the wheel the other way; stuck again and again: out
        if (v.frame >= mStuckCheck)
        {
            const bool still = hypotf(v.x - mStuckX, v.y - mStuckY) < 0.5f;
            mStuckX = v.x, mStuckY = v.y, mStuckCheck = v.frame + 30;
            mStuckTimes = still ? mStuckTimes + 1 : 0;
            if (still && v.frame >= mBackUntil)
            {
                mBackUntil = v.frame + 30;
            }
        }
        if (!target)
        {
            return net::kBtnForward | (v.frame / 60 % 2 ? net::kBtnLeft : net::kBtnRight); // nobody in sight: about
        }
        const int direct = TurnTo(v.rot, target->x - v.x, target->y - v.y);
        // Pinned against someone (hardly moving for 1.5 s with it near): out, to shoot it or take its car
        const uint32_t step = v.frame - mDriveFrame <= net::kBotDecisionTicks ? v.frame - mDriveFrame : 0; // 0 on a new drive
        mSlowFor = mSpeed < 0.04f ? mSlowFor + step : 0;
        mDriveFrame = v.frame;
        if (d < 8 && mSlowFor >= 45)
        {
            return Press(v, net::kBtnEnterExit);
        }
        if (v.frame < mBackUntil)
        {
            return mStuckTimes >= 3 ? Press(v, net::kBtnEnterExit) : net::kBtnBack | (direct > 0 ? net::kBtnRight : net::kBtnLeft);
        }
        // Near it, armed (rockets for a car), and not coming at it fast and head on: out, to shoot
        if ((target->inCar ? v.gunMask >> 2 & 1 : v.guns) && d < 7 && (mSpeed < 0.12f || abs(direct) > 240))
        {
            return Press(v, net::kBtnEnterExit);
        }
        float wx = target->x, wy = target->y;
        if (d > 10)
        {
            mRoute.Next(v, target->x, target->y, &wx, &wy, target->z); // far: along the route; near: straight at it
        }
        return net::kBtnForward | Steer(TurnTo(v.rot, wx - v.x, wy - v.y));
    }

    net::BotRoute mRoute;
    bool mInCar = false;
    int mEntered = 0, mLeft = 0;
    float mTargetDist = -1, mSpeed = 0, mLastX = 0, mLastY = 0, mStuckX = 0, mStuckY = 0;
    uint32_t mLastFrame = 0, mStuckCheck = 0, mBackUntil = 0, mSlowFor = 0, mDriveFrame = 0;
    net::BotWalker mWalker;
    int mStuckTimes = 0;
    PickupMemory mPickups;
    CarDodge mDodge;
    Motions mMotions;
    Thrower mThrower;
};
} // namespace

std::unique_ptr<net::BotBrain> net::CreateBotBrain(const std::string& kind)
{
    if (kind == "carhop")
    {
        return std::unique_ptr<BotBrain>(new CarHopBot());
    }
    if (kind == "soldier")
    {
        return std::unique_ptr<BotBrain>(new SoldierBot());
    }
    if (kind == "hunter")
    {
        return std::unique_ptr<BotBrain>(new HunterBot());
    }
    if (kind == "rammer")
    {
        return std::unique_ptr<BotBrain>(new RamBot());
    }
    if (kind.compare(0, 5, "human") == 0)
    {
        return CreateHumanBrain(kind.size() > 6 ? kind.substr(6) : "");
    }
    if (kind.compare(0, 3, "nn:") == 0)
    {
        return CreateNeuralBrain(kind.substr(3));
    }
    if (kind.empty() || kind == "simple")
    {
        return std::unique_ptr<BotBrain>(new SimpleBot());
    }
    return nullptr;
}

void net::FillBotView(BotView& view, const Snapshot& snap, int self, uint32_t frame, const engine::NavMap* map)
{
    view.frame = frame;
    view.map = map;
    view.seen.clear();
    view.hasBody = engine::PlayerPose(self, &view.x, &view.y, &view.z, &view.rot); // as its client predicts it
    view.health = 0;
    for (size_t i = 0; i < snap.entities.size(); i++)
    {
        const EntityState& e = snap.entities[i];
        if (e.type == engine::kEntityWorld || e.type == engine::kEntityShot)
        {
            continue; // the world has no position, a shot's is where it started
        }
        BotView::Seen s;
        s.id = e.id;
        s.player = -1;
        s.x = engine::Blocks(e.f[0]); // every entity type with a place starts with x, y, z, rot
        s.y = engine::Blocks(e.f[1]);
        s.z = engine::Blocks(e.f[2]);
        s.rot = e.f[3];
        s.health = 0;
        s.inCar = false;
        s.held = -1;
        s.model = e.type == engine::kEntityObject ? engine::PickupModel(e) : engine::CarModel(e);
        if (e.type != engine::kEntityCar || !engine::CarSize(s.model, &s.width, &s.length))
        {
            s.width = 0.8f, s.length = 1.6f;
        }
        s.weapon = engine::PickupModel(e) >= 200 && engine::PickupModel(e) <= 227; // Ped::HandlePickupCollision
        uint32_t buttons = 0;
        if (engine::PlayerOf(e, &s.player, &buttons))
        {
            s.kind = BotView::Seen::kPlayer;
            s.health = engine::PlayerHealth(e);
            s.inCar = engine::PlayerCarId(e) != 0;
            s.held = engine::PlayerWeapon(e);
            if (s.player == self)
            {
                view.health = s.health;
                view.car = engine::PlayerCarId(e);
                view.weapon = engine::PlayerWeapon(e);
                view.ammo = engine::PlayerAmmo(e, view.weapon);
                view.guns = 0;
                view.gunMask = 0;
                for (int w = 0; w < 15; w++)
                {
                    view.guns += engine::PlayerAmmo(e, w) > 0;
                    view.gunMask |= engine::PlayerAmmo(e, w) > 0 ? 1u << w : 0;
                }
                continue;
            }
        }
        else
        {
            s.kind = e.type == engine::kEntityCar     ? BotView::Seen::kCar
                     : e.type == engine::kEntityTrain ? BotView::Seen::kTrain
                     : e.type == engine::kEntityObject ? BotView::Seen::kPickup
                                                       : BotView::Seen::kPed;
        }
        view.seen.push_back(s);
    }
}
