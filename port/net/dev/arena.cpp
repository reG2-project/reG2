// port/net (port/docs/netcode.md, "Bots"): the server's own players, a bot brain each, with no client (--arena). For
// matches between bots without the network, headless and fast (net_check.sh's arena step); a windowed server shows
// one of them.
//
// "remote:<port>[:<teacher>]" slots are played by a trainer (port/net/scripts/rl_bots.py) over a local TCP connection, one round trip
// every kBotDecisionTicks for all of them. The game sends (native byte order, all 32-bit):
//   count, done, kBotFeatures, then per remote slot: slot, alive, reward since the last message, the buttons its
//   teacher would press (a scripted brain, "hunter" by default: for kickstarting), its features
// and the trainer answers each slot's buttons. With done (the last message, at --ticks) there is no answer.
#include "arena.hpp"
#include <algorithm>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#if !defined(_WIN32)
#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace
{
const float kNear = 50; // blocks: closer than this to the nearest other player counts towards "near"
const float kClientArea = 24; // blocks: a client's area (ServerConfig::interestRadius)
const uint32_t kRemoteHead = 4; // slot, alive, reward, teacher

// The trainer's reward: frags and deaths most, then damage, pickups and being near someone to fight
const float kFragReward = 1, kDeathReward = -1, kDamageReward = 0.01f, kPickupReward = 0.3f, kNearReward = 0.001f;
const float kApproachReward = 0.01f; // per block nearer the nearest other player (and as much off going away)
const float kCarReward = 0.3f; // the first car of each life (more in one life pay nothing: no getting in and out for it)
const float kCarFragReward = 0.5f; // on top of a frag made from a car

#if !defined(_WIN32)
int Connect(int port)
{
    const int s = socket(AF_INET, SOCK_STREAM, 0);
    sockaddr_in a = {};
    a.sin_family = AF_INET;
    a.sin_port = htons((uint16_t)port);
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (s < 0 || connect(s, (sockaddr*)&a, sizeof(a)) != 0)
    {
        return -1;
    }
    const int one = 1;
    setsockopt(s, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one)); // a round trip per decision
    return s;
}

bool SendAll(int s, const void* data, size_t n)
{
    for (const char* p = (const char*)data; n;)
    {
        const ssize_t k = send(s, p, n, 0);
        if (k <= 0)
        {
            return false;
        }
        p += k, n -= (size_t)k;
    }
    return true;
}

bool ReceiveAll(int s, void* data, size_t n)
{
    for (char* p = (char*)data; n;)
    {
        const ssize_t k = recv(s, p, n, 0);
        if (k <= 0)
        {
            return false;
        }
        p += k, n -= (size_t)k;
    }
    return true;
}
#else
// Training runs on macOS and Linux; Windows has no remote slots
int Connect(int)
{
    return -1;
}
bool SendAll(int, const void*, size_t)
{
    return false;
}
bool ReceiveAll(int, void*, size_t)
{
    return false;
}
#endif
} // namespace

bool net::Arena::Start(const std::vector<std::string>& brains, uint32_t seed, std::string* error)
{
    for (const std::string& kind : brains)
    {
        mBots.push_back(Bot());
        Bot& bot = mBots.back();
        bot.kind = kind;
        if (kind.compare(0, 7, "remote:") == 0)
        {
            if (mSocket < 0 && (mSocket = Connect(atoi(kind.c_str() + 7))) < 0)
            {
                *error = "no trainer at " + kind;
                return false;
            }
            const size_t colon = kind.find(':', 7);
            bot.teacher = CreateBotBrain(colon == std::string::npos ? "hunter" : kind.substr(colon + 1));
            if (!bot.teacher)
            {
                *error = "no teacher brain in " + kind;
                return false;
            }
            bot.teacher->Seed(seed + (uint32_t)mBots.size());
            continue;
        }
        bot.brain = CreateBotBrain(kind);
        if (!bot.brain)
        {
            *error = "no bot brain " + kind;
            return false;
        }
        bot.brain->Seed(seed + (uint32_t)mBots.size());
    }
    return true;
}

void net::Arena::Think(const Snapshot& world, uint32_t tick)
{
    if (!mNavMap)
    {
        mNavMap.reset(new engine::NavMap());
        engine::BuildNavMap(*mNavMap);
        LabelNavParts(*mNavMap);
    }
    Score(world);
    if (mPickups.empty() || tick % 300 == 0)
    {
        engine::WeaponPickups(mPickups);
    }
    uint64_t expanded = 0;
    for (const Bot& bot : mBots)
    {
        expanded += bot.nav.expanded;
    }
    std::vector<float> message;
    for (size_t i = 0; i < mBots.size(); i++)
    {
        Bot& bot = mBots[i];
        if (bot.brain || tick % kBotDecisionTicks == 0)
        {
            FillBotView(mView, world, (int)i, tick, mNavMap.get());
            mView.stats = &bot.nav;
            mView.pickups = &mPickups;
            // What a client's snapshots would have: every player and train, the rest within its area (server.hpp)
            const float x = mView.x, y = mView.y;
            mView.seen.erase(std::remove_if(mView.seen.begin(), mView.seen.end(),
                                            [&](const BotView::Seen& s) {
                                                return s.kind != BotView::Seen::kPlayer && s.kind != BotView::Seen::kTrain &&
                                                       hypotf(s.x - x, s.y - y) > kClientArea;
                                            }),
                             mView.seen.end());
        }
        if (bot.brain)
        {
            bot.buttons = bot.brain->Think(mView);
        }
        else if (tick % kBotDecisionTicks == 0)
        {
            AddRemote((int)i, message);
        }
    }
    uint64_t after = 0;
    for (const Bot& bot : mBots)
    {
        after += bot.nav.expanded;
    }
    mExpanded += after - expanded;
    mMaxExpanded = after - expanded > mMaxExpanded ? after - expanded : mMaxExpanded;
    if (!message.empty() && !Exchange(message, false))
    {
        fprintf(stderr, "net: the trainer is gone\n");
        _Exit(1); // not exit(): the game's shutdown from here never ended, and the orphans took the trainer's CPU
    }
}

// A remote slot's part of the message: slot, alive, reward, teacher, features (as floats; the ints as their bits)
void net::Arena::AddRemote(int slot, std::vector<float>& message)
{
    Bot& bot = mBots[slot];
    const bool alive = mView.hasBody && mView.health > 0;
    const uint32_t head[2] = {(uint32_t)slot, alive}, teacher = alive ? bot.teacher->Think(mView) : 0;
    message.resize(message.size() + 2);
    memcpy(&message[message.size() - 2], head, sizeof(head));
    message.push_back(bot.reward);
    message.push_back(0);
    memcpy(&message.back(), &teacher, sizeof(teacher));
    bot.reward = 0;
    message.resize(message.size() + kBotFeatures, 0.0f);
    if (alive)
    {
        BotFeatures(mView, bot.memory, &message[message.size() - kBotFeatures]);
    }
    else
    {
        bot.memory = BotMemory();
    }
}

bool net::Arena::Exchange(const std::vector<float>& message, bool done)
{
    const uint32_t per = kRemoteHead + kBotFeatures, count = (uint32_t)(message.size() / per);
    const uint32_t head[3] = {count, done, (uint32_t)kBotFeatures};
    std::vector<uint32_t> buttons(count);
    if (!SendAll(mSocket, head, sizeof(head)) || !SendAll(mSocket, message.data(), message.size() * sizeof(float)))
    {
        return false;
    }
    if (done)
    {
        return true;
    }
    if (!ReceiveAll(mSocket, buttons.data(), count * sizeof(uint32_t)))
    {
        return false;
    }
    for (uint32_t k = 0; k < count; k++)
    {
        uint32_t slot = 0;
        memcpy(&slot, &message[k * per], sizeof(slot));
        mBots[slot].buttons = buttons[k];
    }
    return true;
}

// Per tick: the trainer's rewards, and for the result line how close each bot kept to the nearest other player, the
// health others lost while it faced them, the cars it got into and the pickups it took
void net::Arena::Score(const Snapshot& world)
{
    std::vector<Bot::Body> bodies(mBots.size());
    std::vector<float> cars;
    for (const EntityState& e : world.entities)
    {
        if (e.type == engine::kEntityCar)
        {
            cars.push_back(engine::Blocks(e.f[0])), cars.push_back(engine::Blocks(e.f[1]));
        }
        int p = 0;
        uint32_t b = 0;
        if (engine::PlayerOf(e, &p, &b) && p < (int)bodies.size())
        {
            Bot::Body& body = bodies[p];
            body.x = engine::Blocks(e.f[0]), body.y = engine::Blocks(e.f[1]), body.rot = e.f[3];
            body.health = engine::PlayerHealth(e), body.frags = engine::PlayerFrags(e), body.deaths = engine::PlayerDeaths(e);
            body.car = engine::PlayerCarId(e);
            for (int w = 0; w < 15; w++)
            {
                body.guns += engine::PlayerAmmo(e, w) > 0;
            }
            body.throwables = engine::PlayerAmmo(e, 4) + engine::PlayerAmmo(e, 5);
        }
    }
    // A new round sets every score back to 0: no frag or death that tick
    bool newRound = false;
    for (size_t i = 0; i < mBots.size(); i++)
    {
        newRound |= bodies[i].deaths < mBots[i].last.deaths;
    }
    for (size_t i = 0; i < mBots.size(); i++)
    {
        const Bot::Body& me = bodies[i];
        Bot& bot = mBots[i];
        const int frags = newRound ? 0 : me.frags - bot.last.frags, deaths = newRound ? 0 : me.deaths - bot.last.deaths;
        float nearest = 1e9f, shooterDist = 1e9f, nearestOnFoot = 1e9f, nearestInCar = 1e9f, carDist = 1e9f;
        for (size_t c = 0; c + 1 < cars.size(); c += 2)
        {
            const float d = hypotf(cars[c] - me.x, cars[c + 1] - me.y);
            carDist = d < carDist ? d : carDist;
        }
        int shooter = -1;
        for (size_t j = 0; j < mBots.size(); j++)
        {
            const Bot::Body& other = bodies[j];
            const float dx = me.x - other.x, dy = me.y - other.y, d = hypotf(dx, dy);
            if (j == i || other.health <= 0)
            {
                continue;
            }
            nearest = d < nearest ? d : nearest;
            nearestOnFoot = !other.car && d < nearestOnFoot ? d : nearestOnFoot;
            nearestInCar = other.car && d < nearestInCar ? d : nearestInCar;
            // Who faces it (30 degrees, 10 blocks) or stands at it (3) hurt it; cars and fire mislead it
            int err = (int)(atan2f(dx, dy) * 720.0f / 3.14159265f) - other.rot;
            err = ((err % 1440) + 2160) % 1440 - 720;
            if ((d < 3 || (d < 10 && abs(err) < 120)) && d < shooterDist)
            {
                shooterDist = d;
                shooter = (int)j;
            }
        }
        if (me.health > 0 && nearest < kNear)
        {
            bot.closeness += 1 - nearest / kNear;
            bot.reward += kNearReward * (1 - nearest / kNear);
            const float closer = bot.last.nearest - nearest; // The nearest one may be another than last tick's
            bot.reward += bot.last.health > 0 && fabsf(closer) < 1 ? kApproachReward * closer : 0;
        }
        const int lost = bot.last.health - me.health;
        if (lost > 0 && shooter >= 0 && bot.last.health > 0)
        {
            mBots[shooter].dealt += lost;
            mBots[shooter].fistDealt += !bodies[shooter].guns && !bodies[shooter].car ? lost : 0;
            mBots[shooter].reward += kDamageReward * lost;
        }
        if (mTicks > 0)
        {
            bot.reward += kFragReward * frags + kDeathReward * deaths;
            bot.reward += me.car || bot.last.car ? kCarFragReward * frags : 0;
            bot.frags += frags, bot.deaths += deaths;
            bot.droveThisLife &= deaths == 0;
            if (me.car && !bot.droveThisLife)
            {
                bot.droveThisLife = true;
                bot.reward += kCarReward;
            }
            // A new kind of weapon (more ammo for one it has isn't counted: GTA2_NET_TESTARM refills its own)
            const bool took = me.guns > bot.last.guns && me.health > 0 && bot.last.health > 0;
            bot.pickups += took;
            bot.reward += took ? kPickupReward : 0;
            bot.cars += me.car && !bot.last.car;
            if ((me.car != 0) != (bot.last.car != 0) && me.health > 0 && bot.last.health > 0)
            {
                bot.quickSwitches += bot.carSwitchTick && mTicks - bot.carSwitchTick < 150;
                bot.carSwitchTick = mTicks;
            }
            const bool firing = (bot.buttons & kBtnAttack) && me.health > 0 && !me.car && me.guns > 0;
            bot.firePresses += firing && !(bot.lastButtons & kBtnAttack);
            bot.fireTicks += firing;
            bot.lastButtons = bot.buttons;
            if (me.throwables < bot.last.throwables && me.health > 0 && bot.last.health > 0 && !me.car)
            {
                bot.throws++;
                bot.closeThrows += nearest < 1.8f;
                bot.farThrows += nearest > 5;
            }
        }
        if (me.health > 0)
        {
            bot.aliveTicks++;
            bot.armedTicks += me.guns > 0;
            bot.fistTicks += !me.guns && !me.car && nearest < 1.5f;
            bot.carNearFootTicks += me.car && nearestOnFoot < 6;
            const bool unarmedOnFoot = !me.guns && !me.car;
            bot.carPunchTicks += unarmedOnFoot && (bot.buttons & kBtnAttack) && carDist < 1.2f;
            bot.fistAtCarTicks += unarmedOnFoot && nearestInCar < 1.5f;
        }
        Meter(bot, me, carDist);
        MeterRetreat(bot, me, bodies, (int)i);
        bot.last = me;
        bot.last.nearest = nearest;
    }
    mTicks++;
}

// Stuck: alive through a window, pressing on (forward, or back in a car) for most of it, and under 0.3 blocks (on foot)
// or 0.5 (in a car) from where it started
void net::Arena::Meter(Bot& bot, const Bot::Body& me, float carDist)
{
    const bool inCar = me.car != 0;
    if (me.health <= 0 || (bot.windowOpen && inCar != bot.windowInCar))
    {
        bot.windowOpen = false;
        bot.stuckRun = me.health <= 0 ? 0 : bot.stuckRun;
        return;
    }
    if (!bot.windowOpen)
    {
        bot.windowOpen = true;
        bot.windowFrom = mTicks, bot.pushing = 0, bot.firingInWindow = 0, bot.windowInCar = inCar, bot.windowX = me.x, bot.windowY = me.y;
        bot.windowRot = me.rot, bot.windowTurned = false;
    }
    bot.pushing += (bot.buttons & (inCar ? kBtnForward | kBtnBack : kBtnForward)) != 0;
    bot.firingInWindow += (bot.buttons & kBtnAttack) != 0;
    bot.windowTurned |= (bot.buttons & (kBtnLeft | kBtnRight)) != 0;
    const uint32_t length = inCar ? 60 : 45;
    if (mTicks - bot.windowFrom + 1 < length)
    {
        return;
    }
    const bool stuck = bot.pushing * 2 >= length && hypotf(me.x - bot.windowX, me.y - bot.windowY) < (inCar ? 0.5f : 0.3f);
    bot.stuckTicks += stuck ? length : 0;
    bot.carStuckTicks += stuck && inCar ? length : 0;
    bot.carStuckFootTicks += stuck && !inCar && carDist < 1.5f ? length : 0;
    bot.stuckRun = stuck ? bot.stuckRun + length : 0;
    bot.longestStuck = bot.stuckRun > bot.longestStuck ? bot.stuckRun : bot.longestStuck;
    // Turning without the body turning or moving at all: the game holds it (a player just respawned)
    const bool held = bot.windowTurned && me.rot == bot.windowRot && me.x == bot.windowX && me.y == bot.windowY;
    const bool idle = bot.pushing * 2 < length && bot.firingInWindow * 2 < length && !held &&
                      hypotf(me.x - bot.windowX, me.y - bot.windowY) < (inCar ? 0.5f : 0.3f);
    bot.idleTicks += idle ? length : 0;
    bot.idleRun = idle ? bot.idleRun + length : 0;
    bot.longestIdle = bot.idleRun > bot.longestIdle ? bot.idleRun : bot.longestIdle;
    bot.windowOpen = false;
}

// Each second: how far it went away from (or at) the nearest other player within 10 blocks at the start of it
void net::Arena::MeterRetreat(Bot& bot, const Bot::Body& me, const std::vector<Bot::Body>& bodies, int self)
{
    if (me.health <= 0)
    {
        bot.fleeFrom = bot.retreatFrom = -1;
        return;
    }
    if (mTicks % 30 != 0)
    {
        return;
    }
    if (bot.fleeFrom >= 0)
    {
        const float away = (me.x - bot.fleeX) * bot.fleeOx + (me.y - bot.fleeY) * bot.fleeOy;
        if (away > 1.5f)
        {
            bot.retreatTicks += 30;
            bot.retreatFrom = bot.fleeFrom, bot.lastRetreat = mTicks;
        }
        else if (away < -1.5f && bot.fleeFrom == bot.retreatFrom && mTicks - bot.lastRetreat <= 90)
        {
            bot.turnbacks++;
            bot.retreatFrom = -1;
        }
    }
    bot.fleeFrom = -1;
    float nearest = 10;
    for (size_t j = 0; j < bodies.size(); j++)
    {
        const float dx = me.x - bodies[j].x, dy = me.y - bodies[j].y, d = hypotf(dx, dy);
        if ((int)j != self && bodies[j].health > 0 && d < nearest && d > 0.01f)
        {
            nearest = d, bot.fleeFrom = (int)j, bot.fleeX = me.x, bot.fleeY = me.y, bot.fleeOx = dx / d, bot.fleeOy = dy / d;
        }
    }
}

uint32_t net::Arena::Buttons(int slot) const
{
    return mBots[slot].buttons;
}

void net::Arena::Finish(const Snapshot& world)
{
    for (const EntityState& e : world.entities)
    {
        int p = 0;
        uint32_t b = 0;
        if (engine::PlayerOf(e, &p, &b) && p < (int)mBots.size())
        {
            const Bot& bot = mBots[p];
            printf("net: arena %d %s frags %d deaths %d near %.3f dealt %d cars %d pickups %d\n", p, bot.kind.c_str(), bot.frags, bot.deaths, mTicks ? bot.closeness / mTicks : 0.0, bot.dealt, bot.cars, bot.pickups);
            printf("net: arena %d %s stuck %.1f s longest %.1f s searches %u failed %u giveups %u recoveries %u in a car %.1f s\n", p,
                   bot.kind.c_str(), bot.stuckTicks / 30.0, bot.longestStuck / 30.0, bot.nav.searches, bot.nav.failed, bot.nav.giveUps,
                   bot.nav.recoveries, bot.carStuckTicks / 30.0);
            printf("net: arena %d %s behaviour alive %.1f s armed %.1f s fists %.1f s dealing %d in a car near one on foot %.1f s\n", p,
                   bot.kind.c_str(), bot.aliveTicks / 30.0, bot.armedTicks / 30.0, bot.fistTicks / 30.0, bot.fistDealt,
                   bot.carNearFootTicks / 30.0);
            printf("net: arena %d %s cars punched %.1f s fists near one in a car %.1f s stuck by a car %.1f s round cars %.1f s\n", p, bot.kind.c_str(),
                   bot.carPunchTicks / 30.0, bot.fistAtCarTicks / 30.0, bot.carStuckFootTicks / 30.0, bot.nav.avoided / 30.0);
            printf("net: arena %d %s throws %d nearer than 1.8 %d farther than 5 %d\n", p, bot.kind.c_str(), bot.throws, bot.closeThrows,
                   bot.farThrows);
            printf("net: arena %d %s car switches under 5 s %d fire presses %d held %.1f s\n", p, bot.kind.c_str(), bot.quickSwitches,
                   bot.firePresses, bot.fireTicks / 30.0);
            printf("net: arena %d %s idle %.1f s longest %.1f s retreats %.1f s turnbacks %d\n", p, bot.kind.c_str(), bot.idleTicks / 30.0,
                   bot.longestIdle / 30.0, bot.retreatTicks / 30.0, bot.turnbacks);
        }
    }
    printf("net: arena search cells per tick: mean %.0f max %llu\n", mTicks ? (double)mExpanded / mTicks : 0.0, (unsigned long long)mMaxExpanded);
    if (mSocket >= 0)
    {
        std::vector<float> message;
        for (size_t i = 0; i < mBots.size(); i++)
        {
            if (!mBots[i].brain)
            {
                mView = BotView(); // its last reward; no features
                AddRemote((int)i, message);
            }
        }
        Exchange(message, true);
    }
}
