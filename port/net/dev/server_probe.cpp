#include "server_probe.hpp"
#include "../engine/dev/comfort.hpp"
#include "../engine/dev/dev.hpp"
#include "../session/env.hpp"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

namespace
{
// <tick>:<a>:<b>[,...] into events of <kind>; <minFields> of the three needed
void Parse(const char* list, int kind, int minFields, int defaultB, std::vector<int>& out)
{
    for (const char* k = list; k && *k; k = strchr(k, ','), k = k ? k + 1 : NULL)
    {
        unsigned tick = 0;
        int a = 0, b = defaultB;
        if (sscanf(k, "%u:%d:%d", &tick, &a, &b) >= minFields)
        {
            out.insert(out.end(), {kind, (int)tick, a, b});
        }
    }
}
} // namespace

void net::ServerProbe::Start()
{
    std::vector<int> v; // kind, tick, a, b per event
    Parse(Env().testKill, TestEvent::kKill, 2, -1, v); // <tick>:<victim>:<killer>, the game's own death
    Parse(Env().testPowerUp, TestEvent::kPowerUp, 3, 0, v); // <tick>:<player>:<power-up> (12: an instant gang)
    Parse(Env().testCar, TestEvent::kCar, 3, 0, v); // <tick>:<player>:<model> beside the player (11: a bus)
    Parse(Env().testTrain, TestEvent::kTrain, 2, 0, v); // <tick>:<player>[:<train>] at the wheel of that train
    Parse(Env().testStreak, TestEvent::kStreak, 2, 0, v); // <tick>:<player>: a kill streak's bonus (its 20th kill)
    Parse(Env().testFire, TestEvent::kFire, 2, 0, v); // <tick>:<player>: set on fire
    Parse(Env().testBlast, TestEvent::kBlast, 2, 0, v); // <tick>:<player>[:<tenths of a block>]: an explosion beside it
    Parse(Env().testPhase, TestEvent::kPhase, 2, 0, v); // <tick>:<player>[:<what>]: a phase (DebugPhase)
    Parse(Env().testShot, TestEvent::kShot, 3, 0, v); // <tick>:<player>:<model>: a rocket (128), molotov (138) or grenade (183) beside it
    // <tick>:<player>:<n>[:<blocks>]: the n ambient peds nearest it (at least that far) set on fire
    for (const char* k = Env().testPatients; k && *k; k = strchr(k, ','), k = k ? k + 1 : NULL)
    {
        unsigned tick = 0;
        int player = 0, n = 0, blocks = 0;
        if (sscanf(k, "%u:%d:%d:%d", &tick, &player, &n, &blocks) >= 3)
        {
            v.insert(v.end(), {TestEvent::kPatients, (int)tick, player, n + 1000 * blocks});
        }
    }
    for (size_t i = 0; i < v.size(); i += 4)
    {
        mEvents.push_back(TestEvent{(TestEvent::Kind)v[i], (uint32_t)v[i + 1], v[i + 2], v[i + 3]});
    }
    if (const char* wanted = Env().testWanted) // <points>: every player at least that wanted (600: a star)
    {
        mWanted = atoi(wanted);
    }
    // <weapon>:<ammo>[,<weapon>:<ammo>...]: every player kept armed (weapon_type, 0 pistol, 1 smg, ..., 15.. a car's)
    for (const char* arm = Env().testArm; arm && *arm; arm = strchr(arm, ',') ? strchr(arm, ',') + 1 : "")
    {
        int weapon = 0, ammo = 50;
        sscanf(arm, "%d:%d", &weapon, &ammo);
        mArm.push_back(std::make_pair(weapon, ammo));
    }
    mJerk.PrintJumps(Env().jerkLog >= 2);
}

void net::ServerProbe::Tests(uint32_t tick, int players)
{
    // In the order of the switches: the kills, then the power-ups, then the cars, then the trains
    for (const TestEvent& e : mEvents)
    {
        if (e.tick != tick)
        {
            continue;
        }
        switch (e.kind)
        {
            case TestEvent::kKill:
                printf("net: test kill of player %d by %d at tick %u\n", e.player, e.arg, tick);
                engine::DebugKill(e.player, e.arg);
                break;
            case TestEvent::kPowerUp:
                printf("net: test power-up %d for player %d at tick %u\n", e.arg, e.player, tick);
                engine::DebugPowerUp(e.player, e.arg);
                break;
            case TestEvent::kCar:
                printf("net: test car %d for player %d at tick %u\n", e.arg, e.player, tick);
                engine::DebugCar(e.player, e.arg);
                break;
            case TestEvent::kTrain:
                printf("net: test train %d for player %d at tick %u\n", e.arg, e.player, tick);
                engine::DebugTrain(e.player, e.arg);
                break;
            case TestEvent::kStreak:
                printf("net: test kill streak for player %d at tick %u\n", e.player, tick);
                engine::DebugStreak(e.player);
                break;
            case TestEvent::kFire:
                printf("net: test fire on player %d at tick %u\n", e.player, tick);
                engine::DebugFire(e.player);
                break;
            case TestEvent::kBlast:
                printf("net: test blast %d tenths of a block from player %d at tick %u\n", e.arg, e.player, tick);
                engine::DebugBlast(e.player, e.arg);
                break;
            case TestEvent::kPhase:
                printf("net: test phase %d of player %d at tick %u\n", e.arg, e.player, tick);
                engine::DebugPhase(e.player, e.arg);
                break;
            case TestEvent::kShot:
                printf("net: test shot %d beside player %d at tick %u\n", e.arg, e.player, tick);
                engine::DebugShot(e.player, e.arg);
                break;
            case TestEvent::kPatients:
                printf("net: test fire on %d ambient peds %d blocks or more from player %d at tick %u\n", e.arg % 1000, e.arg / 1000, e.player, tick);
                engine::DebugPatients(e.player, e.arg % 1000, e.arg / 1000);
                break;
        }
    }
    for (int i = 0; i < players; i++)
    {
        if (mWanted >= 0)
        {
            engine::DebugWanted(i, mWanted);
        }
        for (const std::pair<int, int>& arm : mArm)
        {
            engine::DebugArm(i, arm.first, arm.second);
        }
    }
}

void net::ServerProbe::Captured(uint32_t tick, const Snapshot& world)
{
    engine::GangLog(true);
    engine::DebugCheckDrivers("after the tick", tick);
    engine::DebugCheckWeapons("after the tick", tick);
    engine::DebugCheckTurrets("after the tick", tick);
    engine::DebugCheckGridLists("after the tick", tick);
    engine::StackLog(true);
    engine::TrainLog(true);
    engine::FireLog(true);
    engine::PoseLog(true);
    engine::ThrowLog(true, 0);
    engine::ShotLog(true);
    engine::PedLog("server");
    if (Env().snapCheck)
    {
        Snapshot copy = world;
        engine::SanitizeSnapshot(copy);
    }
    engine::InstallSpriteCheck();
    engine::InstallSfxLog(true);
    engine::DumpPeds(tick, true);
    engine::StartRandLog();
    engine::ComfortServerTick(tick);
    if (!Env().jerkLog)
    {
        return;
    }
    std::vector<engine::Pose> poses;
    for (const EntityState& e : world.entities)
    {
        engine::Pose p;
        if (engine::PoseOf(e, &p))
        {
            poses.push_back(p);
        }
    }
    mJerk.Feed(tick, poses);
    if (tick % 900 == 0)
    {
        mJerk.Report("server", tick);
    }
}
