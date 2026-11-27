// port/net: the "human" bot (bot.hpp): a test player that does in turns what people do and the other bots don't, for
// the crash hunts. It isn't trying to win: it rides and leaves cars, buses and trains, tries every weapon it has, picks
// things up, walks into water and blows itself up.
#include "bot.hpp"
#include <map>
#include <math.h>
#include <random>
#include <stdio.h>
#include <stdlib.h>

namespace
{
const float kPi = 3.14159265f;

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

uint32_t Steer(int err)
{
    return err > 80 ? net::kBtnLeft : err < -80 ? net::kBtnRight : 0;
}

bool IsTrain(int model)
{
    return model == 6 || (model >= 59 && model <= 61); // boxcar, TRAIN, TRAINCAB, TRAINFB (Car::IsTrainModel)
}

bool IsSpecial(int model)
{
    return model == 11 || model == 86 || model == 54 || model == 17 || model == 22; // BUS, KRSNABUS, TANK, FIRETRUK, GUNJEEP
}

class HumanBot : public net::BotBrain
{
  public:
    std::string Describe() const override
    {
        char line[200];
        snprintf(line, sizeof(line), "%s at (%.1f, %.1f, %.1f), goal %.1f blocks away, a train down to %.0f; cars %d (specials %d), out at speed %d, trains seen %d, boarded %d, "
                 "weapons tried %d, deaths %d", kNames[mAct], mX, mY, mZ, mGoalDist, mWagonDist, mCars, mSpecials, mFastExits, mTrainsSeen, mTrains, mWeaponsTried, mDeaths);
        return line;
    }

    explicit HumanBot(const std::string& only)
    {
        for (int a = 0; a < kActs; a++)
        {
            mOnly = only == kNames[a] ? a : mOnly;
        }
    }

    uint32_t Think(const net::BotView& v) override
    {
        if (!v.hasBody || v.health <= 0)
        {
            mDeaths += mAlive;
            mAlive = false;
            mActUntil = 0; // something else once back
            return 0;
        }
        mAlive = true;
        mFrame = v.frame;
        mX = v.x, mY = v.y, mZ = v.z;
        mTrainsSeen = 0;
        for (const net::BotView::Seen& s : v.seen)
        {
            mTrainsSeen += s.kind == net::BotView::Seen::kTrain;
        }
        FindStations(v);
        if (v.frame >= mActUntil)
        {
            Choose(v);
        }
        float special = 0;
        if (mOnly < 0 && !v.car && mAct != kDrive && Nearest(v, false, true, &special) && special < 6)
        {
            mAct = kDrive; // a bus or the tank beside it: nobody walks past that
            mActUntil = v.frame + 600;
        }
        if (v.weapon >= 0 && (v.weapon != mLastWeapon))
        {
            mWeaponsTried += !mTried[v.weapon & 31];
            mTried[v.weapon & 31] = true;
            mLastWeapon = v.weapon;
        }
        uint32_t b = 0;
        switch (mAct)
        {
            case kDrive: b = Drive(v); break;
            case kTrain: b = Train(v); break;
            case kFight: b = Fight(v); break;
            case kPickup: b = Pickup(v); break;
            case kWater: b = Water(v); break;
            case kBlast: b = Blast(v); break;
            case kCharge: b = Fight(v, 0.0f); break;
        }
        mFastExits += mInCar && !v.car && mFastExit;
        mInCar = v.car != 0;
        return b;
    }

  private:
    enum Act
    {
        kDrive,
        kTrain,
        kFight,
        kPickup,
        kWater,
        kBlast,
        kCharge, // only as human:charge: the fight, running at the target all the way, firing
        kActs
    };

    int Roll(int n)
    {
        return (int)(mRandom() % (uint32_t)n);
    }

    void Choose(const net::BotView& v)
    {
        static const int kWeights[kActs] = {6, 4, 5, 2, 1, 2, 0};
        int total = 0;
        for (int w : kWeights)
        {
            total += w;
        }
        int r = Roll(total), a = 0;
        while (r >= kWeights[a])
        {
            r -= kWeights[a++];
        }
        mAct = (Act)(mOnly >= 0 ? mOnly : a);
        if (mAct == kTrain && !Nearest(v, true, false))
        {
            mAct = kDrive; // no train on this map (or none seen)
        }
        mActUntil = v.frame + 300 + Roll(600) + (mAct == kTrain ? 900 : 0); // 10 to 30 s at 30 ticks a second; a train takes longer
        mRoute = net::BotRoute();
        mExitAt = 0;
        mWater = -1;
    }

    // A press now and then, not held (the game acts on the change)
    uint32_t Press(uint32_t button) const
    {
        return mFrame % 12 < 2 ? button : 0;
    }

    const net::BotView::Seen* Nearest(const net::BotView& v, bool train, bool special, float* pDist = NULL) const
    {
        const net::BotView::Seen* best = NULL;
        float bestD = 1e9f;
        for (const net::BotView::Seen& s : v.seen)
        {
            const bool isTrain = s.kind == net::BotView::Seen::kTrain || (s.kind == net::BotView::Seen::kCar && IsTrain(s.model));
            if ((s.kind != net::BotView::Seen::kCar && s.kind != net::BotView::Seen::kTrain) || isTrain != train || (special && !IsSpecial(s.model)))
            {
                continue;
            }
            const float d = hypotf(s.x - v.x, s.y - v.y);
            if (d < bestD)
            {
                bestD = d;
                best = &s;
            }
        }
        if (pDist)
        {
            *pDist = bestD;
        }
        return best;
    }

    // Along the route to (x, y); stuck for a second: half a second back and turning
    uint32_t Walk(const net::BotView& v, float x, float y)
    {
        Unstick(v);
        if (v.frame < mUnstickUntil)
        {
            return net::kBtnBack | net::kBtnLeft;
        }
        float wx = x, wy = y;
        mGoalDist = hypotf(x - v.x, y - v.y);
        mRoute.Next(v, x, y, &wx, &wy);
        const int err = TurnTo(v.rot, wx - v.x, wy - v.y);
        return Steer(err) | (abs(err) < 300 ? net::kBtnForward : 0);
    }

    void Unstick(const net::BotView& v)
    {
        if (v.frame >= mStuckCheck)
        {
            const bool still = hypotf(v.x - mStuckX, v.y - mStuckY) < 0.3f;
            mStuckX = v.x, mStuckY = v.y, mStuckCheck = v.frame + 30;
            if (still && v.frame >= mUnstickUntil)
            {
                mUnstickUntil = v.frame + 15;
                mStuckCount++;
            }
            else if (!still)
            {
                mStuckCount = 0;
            }
        }
    }

    uint32_t OnFootExtras(const net::BotView& v)
    {
        uint32_t b = 0;
        if (Roll(200) == 0)
        {
            b |= net::kBtnJump;
        }
        if (Roll(400) == 0)
        {
            b |= net::kBtnSpecial; // a taunt
        }
        if (Roll(90) == 0)
        {
            b |= Roll(2) ? net::kBtnNextWeapon : net::kBtnPrevWeapon;
        }
        (void)v;
        return b;
    }

    // Into the nearest car (a bus or the tank first, if one is near), about town, firing its weapons and honking, and
    // out after a while, often at speed
    uint32_t Drive(const net::BotView& v)
    {
        if (v.car)
        {
            if (!mInCar)
            {
                mCars++;
                for (const net::BotView::Seen& s : v.seen)
                {
                    mSpecials += s.id == v.car && IsSpecial(s.model);
                }
                mExitAt = v.frame + 120 + Roll(360);
                mFastExit = Roll(2) == 0;
            }
            if (v.frame >= mExitAt)
            {
                return Press(net::kBtnEnterExit) | (mFastExit ? net::kBtnForward : 0);
            }
            if (v.frame >= mSteerUntil)
            {
                mSteer = Roll(5) == 0 ? net::kBtnLeft : Roll(4) == 0 ? net::kBtnRight : 0;
                mSteerUntil = v.frame + 15 + Roll(45);
            }
            Unstick(v);
            uint32_t b = v.frame < mUnstickUntil ? net::kBtnBack | net::kBtnRight : net::kBtnForward | mSteer;
            if (Roll(60) == 0)
            {
                b |= net::kBtnJump; // the handbrake
            }
            if (Roll(4) == 0)
            {
                b |= net::kBtnAttack; // a car weapon, if it has one
            }
            if (Roll(100) == 0)
            {
                b |= net::kBtnSpecial;
            }
            if (Roll(150) == 0)
            {
                b |= net::kBtnNextWeapon;
            }
            return b;
        }
        if (mInCar)
        {
            mActUntil = v.frame + 30 + Roll(60); // out: a moment on foot, then something else
            return 0;
        }
        float d = 0, ds = 0;
        const net::BotView::Seen* special = Nearest(v, false, true, &ds);
        const net::BotView::Seen* car = special && ds < 25 ? special : Nearest(v, false, false, &d);
        d = car == special ? ds : d;
        if (!car)
        {
            return Fight(v);
        }
        if (d < 2.5f)
        {
            return Press(net::kBtnEnterExit); // the game walks us to the door
        }
        return Walk(v, car->x, car->y) | OnFootExtras(v);
    }

    // Where trains stand still for a second: stations
    void FindStations(const net::BotView& v)
    {
        for (const net::BotView::Seen& s : v.seen)
        {
            if (s.kind != net::BotView::Seen::kTrain)
            {
                continue;
            }
            TrainSeen& t = mTrainsSeenAt[s.id];
            t.still = hypotf(s.x - t.x, s.y - t.y) < 0.02f ? t.still + 1 : 0;
            t.x = s.x, t.y = s.y;
            bool known = false;
            for (const Station& st : mStations)
            {
                known = known || hypotf(st.x - s.x, st.y - s.y) < 8;
            }
            if (t.still == 30 && !known)
            {
                mStations.push_back(Station{s.x, s.y, Platform(v, s.x, s.y)});
            }
        }
    }

    // A walkable cell beside a station's track, the highest near it (the platform): the cell, -1 if none
    static int Platform(const net::BotView& v, float x, float y)
    {
        const int N = net::engine::NavMap::kSize;
        int best = -1, bestH = -1;
        for (int cy = (int)y - 4; v.map && cy <= (int)y + 4; cy++)
        {
            for (int cx = (int)x - 4; cx <= (int)x + 4; cx++)
            {
                const int c = cy * N + cx;
                if (cx >= 0 && cy >= 0 && cx < N && cy < N && v.map->ways[c] && v.map->height[c] != 255 && v.map->height[c] > bestH &&
                    (abs(cx - (int)x) > 0 || abs(cy - (int)y) > 0))
                {
                    bestH = v.map->height[c];
                    best = c;
                }
            }
        }
        return best;
    }

    // To a station's platform, a wait for a train, on board (Enter by a wagon: the game takes a player on at a
    // station), a ride, then off
    uint32_t Train(const net::BotView& v)
    {
        if (v.car)
        {
            if (!mInCar)
            {
                mTrains++;
                mExitAt = v.frame + 300 + Roll(600);
                mActUntil = mExitAt + 600;
            }
            return v.frame >= mExitAt ? Press(net::kBtnEnterExit) : 0;
        }
        if (mInCar)
        {
            mActUntil = v.frame + 30;
            return 0;
        }
        float d = 0;
        const net::BotView::Seen* wagon = Nearest(v, true, false, &d);
        mWagonDist = d < mWagonDist ? d : mWagonDist;
        if (wagon && d < 4.0f)
        {
            return Steer(TurnTo(v.rot, wagon->x - v.x, wagon->y - v.y)) | Press(net::kBtnEnterExit); // the game looks in front
        }
        for (const net::BotView::Seen& t : v.seen)
        {
            if (t.kind == net::BotView::Seen::kTrain && fabsf(t.z - v.z) < 0.5f && hypotf(t.x - v.x, t.y - v.y) < 12)
            {
                mGoalDist = 0;
                return 0; // on a platform (as high as a train near): waiting for it to stop
            }
        }
        const Station* station = NULL;
        float best = 1e9f;
        for (const Station& st : mStations)
        {
            const float sd = hypotf(st.x - v.x, st.y - v.y);
            if (st.platform >= 0 && sd < best)
            {
                best = sd;
                station = &st;
            }
        }
        if (!station)
        {
            return wagon ? Walk(v, wagon->x, wagon->y) : Fight(v);
        }
        const int N = net::engine::NavMap::kSize;
        const float px = station->platform % N + 0.5f, py = station->platform / N + 0.5f;
        if (hypotf(px - v.x, py - v.y) < 1.0f)
        {
            mGoalDist = 0;
            return 0; // waiting on the platform
        }
        return Walk(v, px, py);
    }

    // Every weapon in turn at the nearest player, or ahead when nobody is near
    uint32_t Fight(const net::BotView& v, float closeTo = 3.0f)
    {
        if (v.car)
        {
            return Press(net::kBtnEnterExit);
        }
        uint32_t b = OnFootExtras(v);
        if (v.weapon < 0 && v.guns)
        {
            b |= Press(net::kBtnNextWeapon);
        }
        const net::BotView::Seen* target = NULL;
        float best = 1e9f;
        for (const net::BotView::Seen& s : v.seen)
        {
            const float d = hypotf(s.x - v.x, s.y - v.y);
            if (s.kind == net::BotView::Seen::kPlayer && s.health > 0 && d < best)
            {
                best = d;
                target = &s;
            }
        }
        if (target && best < 12)
        {
            const int err = TurnTo(v.rot, target->x - v.x, target->y - v.y);
            b |= Steer(err) | (best > closeTo ? net::kBtnForward : 0);
            return b | (abs(err) < 120 && Roll(3) ? net::kBtnAttack : 0);
        }
        if (target)
        {
            return b | Walk(v, target->x, target->y);
        }
        return b | net::kBtnForward | (Roll(10) == 0 ? net::kBtnLeft : 0) | (Roll(5) == 0 ? net::kBtnAttack : 0);
    }

    uint32_t Pickup(const net::BotView& v)
    {
        if (v.car)
        {
            return Press(net::kBtnEnterExit);
        }
        const net::BotView::Seen* best = NULL;
        float bestD = 1e9f;
        for (const net::BotView::Seen& s : v.seen)
        {
            const float d = hypotf(s.x - v.x, s.y - v.y);
            if (s.kind == net::BotView::Seen::kPickup && d < bestD)
            {
                bestD = d;
                best = &s;
            }
        }
        return best ? Walk(v, best->x, best->y) | OnFootExtras(v) : Fight(v);
    }

    // To the nearest water within 40 blocks, and in
    uint32_t Water(const net::BotView& v)
    {
        if (v.car)
        {
            return Press(net::kBtnEnterExit);
        }
        const int N = net::engine::NavMap::kSize;
        if (mWater < 0 && v.map)
        {
            const int cx = (int)v.x, cy = (int)v.y;
            int bestD = 1 << 30;
            for (int y = cy - 40; y <= cy + 40; y++)
            {
                for (int x = cx - 40; x <= cx + 40; x++)
                {
                    const int d = (x - cx) * (x - cx) + (y - cy) * (y - cy);
                    if (x >= 0 && y >= 0 && x < N && y < N && v.map->water[y * N + x] && d < bestD)
                    {
                        bestD = d;
                        mWater = y * N + x;
                    }
                }
            }
            if (mWater < 0)
            {
                mAct = kBlast; // no water near: another way
                return 0;
            }
        }
        if (mWater < 0)
        {
            return Fight(v);
        }
        return Walk(v, mWater % N + 0.5f, mWater / N + 0.5f);
    }

    // A blast weapon (rockets, molotovs, grenades, the flamethrower) fired at the first wall it walks into
    uint32_t Blast(const net::BotView& v)
    {
        if (v.car)
        {
            return Press(net::kBtnEnterExit);
        }
        const bool blast = v.weapon == 2 || v.weapon == 4 || v.weapon == 5 || v.weapon == 8;
        if (!blast)
        {
            if (!v.guns || mFrame % 300 > 240)
            {
                return Fight(v); // none of them: the next best thing
            }
            return Press(net::kBtnNextWeapon) | net::kBtnForward;
        }
        Unstick(v);
        if (mStuckCount > 0)
        {
            return net::kBtnAttack | (Roll(3) == 0 ? net::kBtnForward : 0); // against the wall: fire into it
        }
        return net::kBtnForward | (v.weapon == 8 ? net::kBtnAttack : 0); // the flamethrower: into its own flames
    }

    void Seed(uint32_t seed) override
    {
        mRandom.seed(seed);
    }

    static const char* const kNames[];
    int mOnly = -1;
    std::mt19937 mRandom{std::random_device{}()}; // not the game's random: that one is part of its state
    Act mAct = kDrive;
    uint32_t mFrame = 0, mActUntil = 0, mExitAt = 0, mSteer = 0, mSteerUntil = 0;
    bool mInCar = false, mFastExit = false, mAlive = false;
    net::BotRoute mRoute;
    int mWater = -1;
    float mStuckX = 0, mStuckY = 0;
    uint32_t mStuckCheck = 0, mUnstickUntil = 0;
    int mStuckCount = 0;
    int mLastWeapon = -1;
    bool mTried[32] = {};
    int mTrainsSeen = 0;
    struct TrainSeen
    {
        float x = 0, y = 0;
        int still = 0;
    };
    std::map<uint32_t, TrainSeen> mTrainsSeenAt;
    struct Station
    {
        float x, y;
        int platform; // its cell
    };
    std::vector<Station> mStations;
    float mX = 0, mY = 0, mZ = 0, mGoalDist = -1, mWagonDist = 999;
    int mCars = 0, mSpecials = 0, mFastExits = 0, mTrains = 0, mWeaponsTried = 0, mDeaths = 0;
};
const char* const HumanBot::kNames[] = {"drive", "train", "fight", "pickup", "water", "blast", "charge"};
} // namespace

std::unique_ptr<net::BotBrain> net::CreateHumanBrain(const std::string& only)
{
    return std::unique_ptr<BotBrain>(new HumanBot(only));
}
