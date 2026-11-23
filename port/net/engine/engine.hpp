// port/net (port/docs/netcode.md, "Code layout"): what the session sees of the game. Plain types only: the decomp's
// types stay inside engine/, so the session and modes don't move when the decomp does.
#pragma once
#include "../core/round_state.hpp"
#include "../core/snapshot.hpp"
#include <stdint.h>
#include <string>
#include <vector>

namespace net
{
namespace engine
{
// --- The game (snapshot.cpp, netplay.cpp, client_mode.cpp, guards.cpp, replica.cpp) ---

// A network game for the game to boot (netplay.cpp answers the game's network layer with it)
struct NetworkGame
{
    int players = 1;
    int localPlayer = 0; // the server's is a dummy: nobody's input arrives through the keyboard there
    std::string gmp, sty, scr; // file names in data/
    int gameType = 1; // 1 frags, 2 points, 3 tag
    int fragLimit = 10;
    int timeLimitMin = 10;
    int police = 0;
    int tickMs = 33;
};
// Before WinMain: the game boots this network game (with -N on its command line)
void StartNetworkGame(const NetworkGame& game);
// Once per network frame, after the tick and before drawing (NetPlay::SendToAll)
typedef void (*FrameHook)(void* user);
void SetFrameHook(FrameHook hook, void* user);
bool GameRunning(); // a game with its players exists
int PlayerCount();
uint32_t LocalInput(); // what this machine's controls give for the next tick
void SetPlayerInput(int player, uint32_t buttons); // for the next tick
// A server's: what the player's client typed with the next tick's key press (0: the game's table), for its chat line
void SetPlayerTyped(int player, uint32_t buttons, uint16_t typed);
uint16_t TypedChar(uint32_t buttons); // a client's: the character its key press in <buttons> typed here (0: none)
// The length of a network tick (the game's own setting offers 50, 40 or 33 ms)
void SetTickMs(int ms);
// The client's pause menu (port/src/pause_menu.cpp), over the game going on: Escape opens it, a key while open is its own
// (with the character it typed), and what was chosen in it is taken once
bool MenuOpen();
void MenuKey(uint32_t dik, uint16_t typed);
enum MenuChoice
{
    kMenuNothing,
    kMenuLeave,
    kMenuReady,
    kMenuYes,
    kMenuNo,
};
MenuChoice TakeMenuChoice();
// A player as the pause menu's PLAYERS and the scoreboard list it
struct PlayerRow
{
    int slot;
    std::string name;
    int score, frags, deaths, ping; // score: the game type's (frags, points, seconds "it"); ping: -1 unknown
    bool you, dead, chatting, ready; // ready: in a warmup
};
// What it shows while open: PLAYERS, READY (1) or NOT READY (2) or neither (0), an open vote's question ("": none)
void OfferMenu(const std::vector<PlayerRow>& players, const char* scoreHeading, int ready, const std::string& vote);
// The client's round keys on the gamepad: Back as F1, and in the results the d-pad and A as the arrows and Enter
void SetRoundPad(bool results);
bool PadMode(); // the gamepad gave the last input: the hints name its buttons
void IdleWait(); // while the game waits for its next step: a millisecond's sleep when that is more than 1 ms away
// The real milliseconds a server may wait on the network before its next step: until about 2 ms before it is due (0
// when that is closer), IdleWait doing the rest as for any wait, so that the steps keep their timing
int NetworkWaitMs();
void HoldStep(); // the next step put off by a tick (an empty server's: no step while it waits)
void SeedRandom(uint32_t seed); // the game's rand() state (a server's: clients take it from snapshots)
// A network game's random draws: each entity's update and each server service its own (port/src/rand/owner.hpp)
void SetRandOwners(bool client);
// What a client doesn't simulate: spawning and despawning, scripts, police; called every frame
void ClientModeFrame();
// Cars' attached objects (a tank's turret, a fire engine's cannon) back on their cars, after the client moved the cars
// (a snapshot, the blend): the game puts them there in the car's own update, before that (Object::UpdateMovementAndEffects)
void AnchorAttachments();
// After every tick, on the server too: what the game's own logic can leave behind that crashes it later (guards.cpp)
void GuardPeds();

// --- Snapshots (schema.cpp, snapshot.cpp, players.cpp) ---

// The net schema: entities as plain numbers
const Schema& NetSchema();
enum EntityKind : uint8_t
{
    kEntityPlayer = 0,
    kEntityCar = 1,
    kEntityPed = 2,
    kEntityWorld = 3, // one, kWorldId: game-wide state (no position), in every client's snapshot
    kEntityObject = 4, // a pickup (pickups.cpp)
    kEntityTrain = 5, // a train (trains.cpp): in every client's snapshot
};
const uint32_t kWorldId = 0;
// NetIds: a player's is its slot + 1; cars, peds and pickups have theirs from these on
const uint32_t kCarIdBase = 0x01000000; // + Car::mCarId
const uint32_t kPedIdBase = 0x02000000; // + Ped::mId
const uint32_t kObjIdBase = 0x03000000; // + Object::mId
const uint32_t kTrainIdBase = 0x04000000; // + its index in PublicTransport::mTrainArray
inline bool IsPlayerId(uint32_t id)
{
    return id < kCarIdBase;
}
inline bool IsCarId(uint32_t id)
{
    return id >= kCarIdBase && id < kPedIdBase;
}
// A position field (Fix16 raw: 14 fraction bits) in blocks
inline float Blocks(int32_t raw)
{
    return raw / 16384.0f;
}
// Every player, car, ped, pickup and train
void Capture(Snapshot& out);
// A snapshot from the server made safe for the game, before anything uses it (sanitize.cpp): each field into its valid
// range, a changed one printed as a CHECK line. SnapshotClamps counts the fields changed so far
void SanitizeSnapshot(Snapshot& snap);
int SnapshotClamps();
// Puts a snapshot's players, cars, peds and pickups into this game
void Apply(const Snapshot& snap);
// The scores (frags, points) of a snapshot's players: before a resimulation, and after it so the server's count is shown
void ApplyScores(const Snapshot& snap);
// How far a snapshot's area reaches: the server sets it per client (the farthest entity it kept), a client removes
// what is missing only within it (blocks)
void SetSnapshotReach(Snapshot& snap, float blocks);
// The age field of cars, peds and pickups (-1: none): how many ticks old the state is. The server sends the far
// entities of a busy area less often; a client applies only what is new (0) and predicts the rest on
int AgeField(uint8_t type);
int EntityAge(const EntityState& e);

// --- Players (players.cpp, spawn.cpp) ---

// A player entity's player index and input bits; false for other entities
bool PlayerOf(const EntityState& e, int* pPlayer, uint32_t* pButtons);
int PlayerHealth(const EntityState& e); // a player entity's health
int PlayerFrags(const EntityState& e);
int PlayerPoints(const EntityState& e);
int TagSeconds(int slot); // a tag game's: the seconds the slot was "it" (the game's tag winner has the most)
int PlayerDeaths(const EntityState& e);
uint32_t PlayerCarId(const EntityState& e); // the NetId of the car it drives, 0 on foot
int PlayerWeapon(const EntityState& e); // its weapon in hand (Player::mCurrWeaponIdx), -1: none
int PlayerAmmo(const EntityState& e, int weapon); // 0..14 on foot, 15..27 a car's
// A pickup entity's Object::mModel (objects:: in Source/enums.hpp: 200..227 weapons, 228.. power-ups)
int PickupModel(const EntityState& e);
int CarModel(const EntityState& e); // a car entity's Car::mCarInfoIdx (car_model_enum in Source/enums.hpp), -1 for others
// The position of a player's ped or car, in blocks; false without a ped
bool PlayerPosition(int player, float* pX, float* pY);
bool PlayerPose(int player, float* pX, float* pY, float* pZ, int* pRot); // and its height and heading (0..1439)
bool PlayerWasted(int player); // dead and waiting to respawn (Player::mWastedOrBusted)
bool PlayerInCar(int player); // in this game
// How far a player's camera shows (blocks, centre to corner): it zooms out with speed
float PlayerViewRadius(int player);
// The spawning (a server's): the players the limits are for, and how many ticks ahead of this game each one's client
// shows its player (0: here, a bot); its cameras' bounds move that far along their motion for each tick (spawn.cpp)
void SetSpawnPlayers(int players);
void SetSpawnLead(int player, int ticks);
int SpawnLead(int player);
std::string SpawnStats(); // for the log: the ambient traffic and peds, the moving objects, and their limits
// A slot's name as the game shows it (over the player, in kill messages): Player::mPlayerName, up to 15
void SetPlayerName(int slot, const char* name);
std::string PlayerName(int slot); // as the game shows it
// The local player's chat line, kept through a restore and its resimulation (which see no key events): what this
// player types shows at once; the others' comes with the snapshots
struct LocalChat
{
    char chatting = 0;
    int length = 0;
    short text[79] = {};
};
LocalChat TakeLocalChat();
void PutLocalChat(const LocalChat& chat);
// A player entity's chat line, and whether it is still being typed (the game keeps the line once Enter ends it)
bool PlayerChat(const EntityState& e, std::string* pLine);

// --- Rounds (rounds.cpp, netplay.cpp, slots.cpp, world_entity.cpp): the frag and time limits are the netcode's (the game gets none it would end on), a
// round that reaches one starts the next on the same map. The server keeps the round; snapshots carry it ---

typedef RoundState Round; // the round's phase, votes and limits (session/round_flow.cpp keeps a server's)
Round& CurrentRound();
void NewRound(); // every slot in the game respawned with no score, weapons or power-ups
void FreshScores(int slot); // its frags, points and HUD (money, frags, multiplier) as at a game's start (a new game's players)
void ShowHudMessage(const char* text); // a line over the frame for a few seconds (overlay.cpp, with the status lines)
// A chat line someone sent, over the frame above the lines being typed: the last 5, each for 10 s (overlay.cpp)
void AddChatLine(const std::string& line);
// Lines drawn over the frame until changed, centred near the top (the round's warmup, votes, results; overlay.cpp)
void SetNetPanel(const std::vector<std::string>& lines);
// One line drawn large in the middle of the frame (the game's message font) until changed; empty: none
void SetNetBanner(const std::string& text);
// The next map, a game of <players> with this machine's as <localPlayer>: this one ends after <pauseTicks> and that one starts at once, without the frontend
// (winmain.cpp, gPortNextNetworkGame); KeepMapChange every frame until then. GameGeneration counts the games, for
// the session to start over when it changes
void ChangeMap(const std::string& gmp, const std::string& sty, const std::string& scr, int players, int localPlayer, int pauseTicks);
// The first of a map's files that isn't in data/ (the game's fatal error if it opened it); empty: all are
std::string MissingMapFile(const std::string& gmp, const std::string& sty, const std::string& scr);
// data/<name>'s CRC-32, read as the game opens it (0: it can't be read)
uint32_t DataFileCrc(const std::string& name);
// This network game ends at once for the frontend (LeftForMenu() says so to winmain.cpp's gPortLeaveNetworkGame), and
// StopNetworkGame puts back what a single-player game expects
void LeaveForMenu();
bool LeftForMenu();
bool GameClosed(); // the game ended by closing (its window): the program ends
void StopNetworkGame();
void KeepMapChange();
int GameGeneration();
int SnapshotGame(const Snapshot& snap); // the server's GameGeneration at it (-1: no world in it)
bool MapChangePending();
// Player slots in a running network game (slots.cpp): out of the game as the game handles a player who left, in
// again with a new ped at the next restart zone (and an empty score if <freshScore>)
bool SlotInGame(int slot);
void TakeOutOfGame(int slot);
void NeverInGame(int slot); // out from the game's start: its ped (the map script's) goes too
void PutInGame(int slot, bool freshScore);

// --- Rollback (rollback.cpp): the whole game world and this client's replicas, saved after a tick and restored to
// resimulate from there ---

class SavedWorld
{
  public:
    SavedWorld();
    ~SavedWorld();
    SavedWorld(const SavedWorld&) = delete;
    SavedWorld& operator=(const SavedWorld&) = delete;
    uint32_t tick = 0;
    bool valid = false;
    struct Impl;
    Impl* mImpl;
};
void SaveGame(SavedWorld& out, uint32_t tick);
void RestoreGame(const SavedWorld& in);
bool Restorable(const SavedWorld& in); // saved with the rollback set as it is now
// One tick without drawing or sound, with every player's buttons (count: players)
void ResimTick(const uint32_t* buttons, int count);
// Before resimulating the tick the frame loop just ran: the corrected game's places the frames draw from
void ResimStepStart();

// --- A client's view (replica.cpp, snapshot.cpp) ---

// Where this game has its mapped cars and peds now: NetId and x, y in blocks
struct ReplicaPos
{
    uint32_t id;
    float x, y;
};
void ReplicaPositions(std::vector<ReplicaPos>& out);
// Where the players on foot, and the cars and peds the snapshots map, are now (raw Fix16, Ang16 0..1439), and a
// move of one to a pose (grid-safe; players in a car or at a car door, dead ones and cars a carrier holds stay). For
// blending a correction in (client.cpp)
struct Pose
{
    uint32_t id;
    int32_t x, y, z;
    int32_t rot;
};
void Poses(std::vector<Pose>& out);
void SetPose(const Pose& p);
bool PoseOf(const EntityState& e, Pose* out); // a snapshot entity's, as Poses has them (players on foot, cars, peds)

// --- Bots (navmap.cpp): the static map as a bot may know it. For every cell the height a walker is at, and which of
// its four neighbours it can walk to. Plain data, built once per map, so a bot plans its own way without the game's
// routes ---

struct NavMap
{
    static const int kSize = 256;
    enum Way : uint8_t
    {
        kNorth = 1, // -y
        kSouth = 2, // +y
        kWest = 4, // -x
        kEast = 8, // +x
    };
    uint8_t height[kSize * kSize]; // 255: nobody stands here
    uint8_t ways[kSize * kSize];
    bool water[kSize * kSize]; // its top block is water: a walker drowns there
};
void BuildNavMap(NavMap& out);
void DumpNavMap(const NavMap& map, const char* path); // as a picture (PGM): grey by height, black blocked

// --- Statistics for the logs (stats.cpp, replica.cpp) ---

// A client's replicas: how many cars and peds are mapped, and how far (in blocks, the largest) the mapped ones are
// from <snap> right now, before it is applied: the drift of one tick of the client's own sim
void ReplicaStats(const Snapshot& snap, int* pCars, int* pPeds, float* pMaxDrift);
void PoolUse(int* pCars, int* pPeds, int* pChars); // live cars, peds, chars in this game
void PrintReplicaLog(); // what the replicas did since the last call (explosions carried out, wrecks and corpses made again...)
// Drivers: how many peds the snapshot has in a car, how many of their cars have a driver here, and how many of
// those are the same (mapped) ped
void DriverStats(const Snapshot& snap, int* pSent, int* pDriven, int* pSame);
// Traffic cars: how many the snapshot has with the shared traffic driver, and how many have it here too
void TrafficDriverStats(const Snapshot& snap, int* pSent, int* pSame);

// --- Over the frame (overlay.cpp) ---

void DrawOverlay(); // the game calls it while drawing (Game::Draw, gPortDrawOverlay)
// A client's connection, over the frame: <corner> small at the bottom right (the ping), <centre> across the middle
// (an outage); empty: nothing
void SetNetStatus(const std::string& corner, const std::string& centre);
// The scoreboard, over the middle of the frame instead of the round's panel while shown (scoreboard.cpp): the map's title,
// two lines under it, the players by score (their $ sign and name in their HUD colour, the local one marked)
struct Scoreboard
{
    std::string title;
    std::string lines[2];
    const char* scoreHeading; // the game type's score
    bool frags; // a frags column after the score (a points or tag game's)
    std::vector<PlayerRow> rows;
};
void SetScoreboard(const Scoreboard* pBoard); // NULL: hidden
bool ScoresHeld(); // SCORES, or the gamepad's Back, held (compat/src/pad.cpp)

// The checks, logs and test cheats are engine/dev/dev.hpp's.
} // namespace engine
} // namespace net
