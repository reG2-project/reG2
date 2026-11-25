// port/net dev (port/docs/netcode.md, "Bots"): an arena, bot players on the server itself (--arena): matches for tests and
// the bots' training (a trainer's socket: port/net/scripts/rl_bots.py).
#pragma once
#include "../core/snapshot.hpp"
#include "../engine/engine.hpp"
#include "../session/bot.hpp"
#include <memory>
#include <string>
#include <vector>

namespace net
{
// The server's own bot players (arena.cpp), slots 0..n-1: a brain each, or a trainer's ("remote:<port>[:<teacher>]")
class Arena
{
  public:
    bool Start(const std::vector<std::string>& brains, uint32_t seed, std::string* error); // <seed>: the brains' draws
    int Count() const
    {
        return (int)mBots.size();
    }
    void NewMap()
    {
        mNavMap.reset(); // made again for the next one
    }
    void Think(const Snapshot& world, uint32_t tick); // each bot's buttons for the next tick
    uint32_t Buttons(int slot) const;
    // "net: arena <slot> <brain> frags F deaths D near N dealt H cars C pickups P", its stuck meter's line, and the
    // trainer's last message
    void Finish(const Snapshot& world);

  private:
    struct Bot
    {
        struct Body // a tick's state, as Score reads it
        {
            float x = 0, y = 0;
            int rot = 0, health = 0, frags = 0, deaths = 0, guns = 0; // guns: the weapons with ammo
            int throwables = 0; // molotovs and grenades
            float nearest = 0; // blocks to the nearest other player alive
            uint32_t car = 0;
        };
        std::string kind;
        std::unique_ptr<BotBrain> brain; // NULL: remote
        uint32_t buttons = 0;
        Body last;
        double closeness = 0; // summed per tick: 1 - distance / 50 blocks to the nearest other player
        int dealt = 0, cars = 0, pickups = 0, frags = 0, deaths = 0; // frags and deaths over all the rounds
        float reward = 0; // since the last message (remote)
        bool droveThisLife = false; // (remote) the car reward is paid once a life
        BotMemory memory; // its features' (remote)
        std::unique_ptr<BotBrain> teacher; // a scripted brain on the same view (remote): what it would press
        BotNavStats nav;
        // The stuck meter: a window of 1.5 s on foot (2 s in a car) alive, and how far it got while pressing on
        uint32_t windowFrom = 0, pushing = 0, stuckTicks = 0, carStuckTicks = 0, stuckRun = 0, longestStuck = 0;
        float windowX = 0, windowY = 0;
        bool windowInCar = false, windowOpen = false;
        // Behaviour, in ticks alive: with a gun, on foot unarmed within 1.5 blocks of someone (a fistfight), in a car
        // within 6 blocks of someone on foot; and the damage it dealt unarmed on foot
        uint32_t aliveTicks = 0, armedTicks = 0, fistTicks = 0, carNearFootTicks = 0;
        int fistDealt = 0;
        // Unarmed on foot: punching within 1.2 blocks of a car, near a player in a car; stuck with a car within 1.5
        uint32_t carPunchTicks = 0, fistAtCarTicks = 0, carStuckFootTicks = 0;
        int throws = 0, closeThrows = 0, farThrows = 0; // molotovs and grenades thrown, by the nearest other player
        // Getting into or out of a car under 5 s after the last time; fire on foot armed: presses, and ticks held
        int quickSwitches = 0, firePresses = 0;
        uint32_t carSwitchTick = 0, fireTicks = 0, lastButtons = 0;
        // Idle: a meter window still without pressing on or firing; retreats: seconds going away from someone within
        // 10 blocks, and turnbacks: a retreat followed within 3 s by a second going at the same one
        uint32_t idleTicks = 0, idleRun = 0, longestIdle = 0, firingInWindow = 0, retreatTicks = 0, lastRetreat = 0;
        int turnbacks = 0, fleeFrom = -1, retreatFrom = -1;
        float fleeX = 0, fleeY = 0, fleeOx = 0, fleeOy = 0;
        int windowRot = 0;
        bool windowTurned = false;
    };
    void Meter(Bot& bot, const Bot::Body& me, float carDist);
    void MeterRetreat(Bot& bot, const Bot::Body& me, const std::vector<Bot::Body>& bodies, int self);
    void Score(const Snapshot& world);
    void AddRemote(int slot, std::vector<float>& message);
    bool Exchange(const std::vector<float>& message, bool done);

    std::vector<Bot> mBots;
    std::unique_ptr<engine::NavMap> mNavMap;
    BotView mView;
    std::vector<engine::PickupSpot> mPickups; // the game's weapon pickups, listed every 10 s
    uint32_t mTicks = 0;
    uint64_t mExpanded = 0, mMaxExpanded = 0; // all bots' search cells, in total and in the worst tick
    int mSocket = -1; // to the trainer
};
} // namespace net
