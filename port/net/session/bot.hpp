// port/net (port/docs/netcode.md, "Bots"): bots as players that perceive and decide on their own. A bot gets a view of
// what its client knows (its own body as predicted, the snapshot's players, cars and peds, the static map) and
// returns the input bits a person would press. It uses none of the game's logic (no route finder, no ped AI), so
// other brains can come in: smarter ones, or one that sends the view to another process and reads its answer.
#pragma once
#include "../engine/engine.hpp"
#include <memory>
#include <stdint.h>
#include <string>
#include <vector>

namespace net
{
// The game's input bits (Source/input_bits.hpp)
enum BotButton : uint32_t
{
    kBtnForward = 0x1,
    kBtnBack = 0x2,
    kBtnLeft = 0x4, // turns towards larger angles
    kBtnRight = 0x8,
    kBtnAttack = 0x10,
    kBtnEnterExit = 0x20,
    kBtnJump = 0x40, // handbrake in a car
    kBtnPrevWeapon = 0x80,
    kBtnNextWeapon = 0x100,
    kBtnSpecial = 0x200, // the horn in a car
};

// What a bot's navigation did (the arena's stuck meter prints it)
struct BotNavStats
{
    uint32_t searches = 0, failed = 0, giveUps = 0, recoveries = 0;
    uint64_t expanded = 0; // cells looked at by its searches
};

struct BotView
{
    uint32_t frame = 0;
    // Its own body: x, y, z in blocks, rot 0..1439 (walking while facing rot moves along (sin, cos): measured)
    bool hasBody = false;
    float x = 0, y = 0, z = 0;
    int rot = 0;
    int health = 0;
    uint32_t car = 0; // the car it drives (a Seen kCar id), 0 on foot
    int weapon = -1, ammo = 0; // the weapon in hand (Source/Weapon.hpp, 0..14; -1: fists) and its ammo
    int guns = 0; // the weapons it has ammo for (on foot)
    struct Seen
    {
        enum Kind
        {
            kPlayer,
            kCar,
            kPed,
            kPickup, // a weapon, power-up or token
            kTrain, // a train, where its lead carriage is (its carriages near are kCar)
        } kind;
        uint32_t id;
        int player; // for kPlayer
        float x, y, z;
        int rot;
        int health; // for kPlayer
        bool weapon; // for kPickup: a weapon (else a power-up)
        bool inCar; // for kPlayer: it drives
        int model; // for kCar: its model (car_model_enum: 11 a bus, 54 the tank, 59..61 and 6 a train's)
    };
    std::vector<Seen> seen; // what the client's snapshot has, others only
    const engine::NavMap* map = nullptr;
    BotNavStats* stats = nullptr; // counted into when set (arena bots)
};

class BotBrain
{
  public:
    virtual ~BotBrain()
    {
    }
    virtual uint32_t Think(const BotView& view) = 0;
    virtual void Seed(uint32_t) // a brain that draws: its draws from <seed> (a server's: so that a replay draws the same)
    {
    }
    virtual std::string Describe() const // a line for the client's log
    {
        return std::string();
    }
};

// "simple": walks to the nearest player along its own route and shoots when close and facing it
// "carhop": walks to the nearest car, gets in, drives a while, gets out, again (tests players in cars)
// "rammer": gets into the nearest car and drives at the nearest player to hit it
// "hunter": the rammer when a car is near and nobody is close (and once in a car), the simple one otherwise
// "soldier": a weapon, then a car, then to the nearest player: runs it over or gets out and shoots, whichever suits
// "human": what people do and the other bots don't, in turns: cars and buses (in, about, out at speed), trains (on at a
// station, a ride, off), every weapon it has, pickups, walking into water and blowing itself up (bot_human.cpp);
// "human:<act>" does that one only
// "nn:<file>": a neural network's (bot_nn.cpp), its weights from the file (port/net/scripts/rl_bots.py)
std::unique_ptr<BotBrain> CreateBotBrain(const std::string& kind);
std::unique_ptr<BotBrain> CreateNeuralBrain(const std::string& path); // NULL if the file is no network of this shape
std::unique_ptr<BotBrain> CreateHumanBrain(const std::string& only); // "human:<act>": that act only (drive, train, fight...)

// A way to a goal (bot_nav.cpp), replanned every second or when the goal moves to another cell
struct BotRoute
{
    std::vector<int> path; // the cells (y * 256 + x) still to go
    int goal = -1;
    uint32_t replanAt = 0;
    bool none = false; // no way found: straight at it
    // The point to head for: a few cells ahead on the route (smoother than cell by cell), or the goal
    void Next(const BotView& v, float goalX, float goalY, float* pX, float* pY);
};

// The network bots decide every this many ticks and hold their buttons in between, in training (arena.cpp) and
// in play (bot_nn.cpp) alike: their features (speed, last buttons) are per decision
const uint32_t kBotDecisionTicks = 3;

// What the network bots see (bot_nn.cpp): a BotView as numbers, about -1..1, relative to where it faces
const int kBotFeatures = 62;
struct BotMemory // what the features remember between ticks
{
    float lastX = -1, lastY = 0;
    uint32_t lastButtons = 0;
    BotRoute toPlayer, toWeapon; // the ways to the nearest player and weapon, as a compass
};
void BotFeatures(const BotView& view, BotMemory& memory, float* out);
// What they do: a choice in each group (turn left/no/right, forward/no/back, then attack, enter/exit, jump and next
// weapon, each no/yes)
const int kBotGroups = 6;
const int kBotGroupSize[kBotGroups] = {3, 3, 2, 2, 2, 2};
const int kBotLogits = 14;
uint32_t BotButtons(const int* choice);

// The view a client gives its bot (bot.cpp)
void FillBotView(BotView& view, const Snapshot& snap, int self, uint32_t frame, const engine::NavMap* map);

// Navigation (bot_nav.cpp, no game code): the cells (y * 256 + x) from start to goal, start excluded; false if
// there is no way within <maxExpand> cells looked at
bool FindPath(const engine::NavMap& map, int sx, int sy, int gx, int gy, std::vector<int>& path, int maxExpand = 40000,
              int* pExpanded = nullptr);
} // namespace net
