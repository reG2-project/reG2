// port/net engine internals, shared by the engine's files. Decomp types: only for engine/.
#pragma once
#include "engine.hpp"
#include "Source/ang16.hpp"
#include "Source/fix16.hpp"
#include "Source/network_data.hpp"
#include <set>
#include <stddef.h>
#include <unordered_map>

class Car;
class Object;
class Ped;
class Player;
class Sprite;

namespace net
{
namespace engine
{
// The schema's fields (schema.cpp builds the schema in this order). Every entity with a place has x, y, z and rot
// first (PutPosition)
const int kPlayerWeapons = 28; // Player::mWeapons: its own (0..14) and its car weapons (15..27)
const int kPowerUps = 17; // Player::mPowerUpTimers
const int kKillerLaw = 63; // kPlayerKiller: killed by the police
enum PlayerField
{
    kPlayerX,
    kPlayerY,
    kPlayerZ,
    kPlayerRot,
    kPlayerInCar,
    kPlayerButtons,
    kPlayerHealth,
    kPlayerDeaths, // how often the server's player died (wraps at 256)
    kPlayerKiller, // who killed it last: player index + 1, 0 nobody, kKillerLaw the police
    kPlayerWeapon, // Player::mCurrWeaponIdx + 1 (0: none)
    kPlayerFrags,
    kPlayerPoints,
    kPlayerAmmo0, // each weapon's ammo
    kPlayerCar = kPlayerAmmo0 + kPlayerWeapons, // the car it drives (CarRef)
    kPlayerState, // kPlayerTransition | kPlayerDead | kPlayerLeaving | kPlayerOnFire
    kPlayerTarget, // the car it is going to get into (Ped::mTargetObjectiveCar, CarRef)
    kPlayerName, // its name (Player::mPlayerName), 4 characters a field
    kPlayerNameEnd = kPlayerName + 4,
    kPlayerPowerUp = kPlayerNameEnd, // the power-up timers
    kPlayerPowerUpEnd = kPlayerPowerUp + kPowerUps,
    kPlayerChatting = kPlayerPowerUpEnd, // typing a chat line (Player::mIsChatting, Hud_ChatInput)
    kPlayerChat, // the line so far (mChatText), 4 characters a field
    kPlayerChatEnd = kPlayerChat + 20,
    kPlayerWanted = kPlayerChatEnd, // the wanted points (Ped::mWantedPoints: the HUD's cop heads, the police's chase)
    kPlayerMoney, // the money on the HUD (Player::mScores; in a points game the score players see)
    kPlayerMultiplier, // and its multiplier (Player::mMultipliers)
    kPlayerDeathEvents, // how often it died this game (wraps at 256): what a client carries out (kPlayerDeaths: the round's)
    kPlayerBonus, // its scoring bonuses this game (wraps at 256) << 4 | the last one's voice line (ScoreBonus)
};
enum PlayerState
{
    kPlayerTransition = 1, // getting into or out of a car (InCarTransition)
    kPlayerDead = 2,
    kPlayerLeaving = 4, // getting out of one (LeavingCar)
    kPlayerOnFire = 8, // burning (Ped::mBf21C.bOnFire)
};
enum WorldField
{
    kWorldCycle, // the game's tick counter (gpRng's mRng): timers are stamped with it (a car's damage, a ped's voice...)
    kWorldRand, // the game's random state (rand(), port/src/rand/rand.cpp): a resimulation draws what the server drew
    kWorldReach, // how far this client's snapshot reaches (blocks x 16): the farthest it kept, the rest cut for size
    kWorldIt, // the tag game's "it" (TagGame::mFugitive): player index + 1, 0 nobody
    kWorldItTime, // and the seconds each player has been it (mItTimeSecs), MAX_PLAYERS of them
    kWorldZoneTurn = kWorldItTime + MAX_PLAYERS, // which restart zone is next (Map_0x370::GetNextZoneOfTypeRoundRobin): a respawn here goes where the server's does
    kWorldRound, // the round (rounds.cpp): its number, the last one's winner (player + 1) and the seconds left
    kWorldWinner,
    kWorldSecondsLeft,
    kWorldGame, // the server's game since its start (GameGeneration): a client takes only its current one's
    kWorldSeed, // the seed of the entities' own random draws (port/src/rand/owner.hpp)
    kWorldLights, // the traffic lights (one phase for all, TrafficLights): the phase, its timer, the cars' light state
    kWorldPhase, // the round flow's state (core/round_state.hpp), as RoundState has it
    kWorldPhaseLeft,
    kWorldMinPlayers,
    kWorldReady,
    kWorldVote,
    kWorldVoteArg,
    kWorldVoteYes,
    kWorldVoteNo,
    kWorldVoteLeft,
    kWorldMapChoice, // kRoundSlots of them
    // The map's light level (MapRenderer's mAmbient, mTargetAmbient, mAmbientStep, raw): its script sets it, and scripts
    // run on the server only
    kWorldAmbient = kWorldMapChoice + kRoundSlots,
    kWorldTargetAmbient,
    kWorldAmbientStep,
    // The police (Police: its service runs on the server only), what a client's cops, cop cars and HUD read: the highest
    // wanted level, the strongest crew in a pursuit, a crew ped on screen, the last cop whose shot hit (a ped's net id)
    kWorldPoliceLevel,
    kWorldPoliceCrewType,
    kWorldPoliceCrewOnscreen,
    kWorldPoliceFiring,
    // and each pursuit (Police::mPursuitTargets, four): its criminal (net id), level, state, timer, crews
    // and whether it is chased
    kWorldPursuitCriminal,
    kWorldPursuitLevel = kWorldPursuitCriminal + 4,
    kWorldPursuitState = kWorldPursuitLevel + 4,
    kWorldPursuitTimer = kWorldPursuitState + 4,
    kWorldPursuitCrews = kWorldPursuitTimer + 4,
    kWorldPursuitChased = kWorldPursuitCrews + 4,
};
enum CarField
{
    kCarX,
    kCarY,
    kCarZ,
    kCarRot,
    kCarModel,
    kCarDriver, // kDriverNone, kDriverTraffic (the car manager's shared spare driver), kDriverPed (sent as a ped entity)
    kCarDamage, // Car::mDamage (32000 and up: exploded), the server's
    kCarAreas, // its dented areas (Car::mDamagedAreas)
    kCarWrecked, // kCarWreck | kCarWreckBurns
    kCarLights, // Car::mLightFlags (brake, head, emergency lights...), mFlashPhaseCounter, mHorn, mHornCooldown
    kCarFlash,
    kCarHorn,
    kCarHornCooldown,
    kCarRemap, // its colour (the sprite's remap): a respray, or a car made here, had another
    kCarTurret, // its roof turret's angle on the car (a tank's cannon, a gun jeep's gun, a fire truck's water cannon)
    kCarTrailer, // the trailer a cab tows (CarRef; the trailer, off the pool's list then, is sent as a car of its own)
    kCarFlags, // the behaviour bits of Car::mFlags (kCarFlagsSent: proofs, alarm, no passengers...), the level start's
    kCarPhysics, // then its whole CarPhysics, member by member (cars.cpp; all 0 without physics), then its AI (car_ai.cpp)
};
enum CarWrecked
{
    kCarWreck = 1, // exploded or burning out (the wreck countdown running, or a wreck)
    kCarWreckBurns = 2, // its wreck's fire (Car::SpawnFire's), which burns only where a fire engine was sent
};
enum CarDriver
{
    kDriverNone = 0,
    kDriverTraffic = 1,
    kDriverPed = 2,
};
enum PedField
{
    kPedX,
    kPedY,
    kPedZ,
    kPedRot,
    kPedRemap,
    kPedCar, // the car it drives or rides in (CarRef; its position is the car's then; a passenger: PassengerThere)
    kPedState, // 1: getting into or out of a car
    kPedAi, // then its AI state (ped_state.cpp)
};
enum ObjectField
{
    kObjX,
    kObjY,
    kObjZ,
    kObjRot,
    kObjModel, // Object::mModel (objects::)
    kObjIdx, // Object::mPedRefIdx (what kind, for some)
};
const int kTrainCarriages = 11; // Train::mCarriages: the lead and up to 10 wagons
const int kTrainDriverPlayer = 3;
enum TrainField
{
    kTrainX, // its lead carriage's place
    kTrainY,
    kTrainZ,
    kTrainRot,
    kTrainWagons, // Train::mWagonCount
    kTrainReversed, // the cab isn't the lead (Train::ReassignTrainHead)
    // Train::mState (its speed and direction), mStopPhase, mWaitTimer, mIsHijacked, mPassengersFleeing,
    // mPassengerTimer, mPassengerCount
    kTrainState,
    kTrainStopPhase,
    kTrainWaitTimer,
    kTrainHijacked,
    kTrainFleeing,
    kTrainPassengerTimer,
    kTrainPassengerCount,
    kTrainSpeed, // its lead's CarAI::mSpeed (raw)
    kTrainDriver, // its lead's driver: kDriverNone, kDriverTraffic, kDriverPed (not a player), or kTrainDriverPlayer + slot
    // The station it heads for (index in PublicTransport::mStations + 1, 0 none), and that station's mPlatformState
    // and mpStoppedTrain (train index + 1, 0 none)
    kTrainStation,
    kTrainPlatform,
    kTrainStopped,
    kTrainCar0, // each carriage (CarRef), lead first
    kTrainDamage0 = kTrainCar0 + kTrainCarriages, // and its Car::mDamage
    kTrainTimer0 = kTrainDamage0 + kTrainCarriages, // Train::mExplosionTimers
    kTrainFieldCount = kTrainTimer0 + kTrainCarriages,
};

// Shared helpers (snapshot.cpp)
Player* GetPlayer(int slot); // NULL past the game's players
Ped* InGamePed(int slot); // its ped while the slot is in the game (Player::mInUse)
inline Fix16 Raw(int32_t v) // a field's position value
{
    return Fix16(v, 0);
}
inline Ang16 Angle(int32_t v)
{
    return Ang16((s16)v, (u8)0);
}
// A car as a field: Car::mCarId + 1, 0 none; and back to this game's car for it (NULL: none or not here)
int32_t CarRef(const Car* car);
uint32_t CarRefNetId(int32_t ref);
Car* RefCar(int32_t ref);
Sprite* CharSprite(const Ped* ped); // its char's (on foot), NULL without one
Sprite* PedSprite(Ped* ped); // its car's when it is in one
EntityState Entity(uint32_t id, uint8_t type);
void PutPosition(EntityState& e, const Sprite* s); // fields 0..3
bool Near(const Sprite* s, int32_t x, int32_t y, int32_t radius); // within <radius> (raw) on both axes
const EntityState* FindWorld(const Snapshot& snap);
uint32_t PedNetId(const Ped* p); // a ped as a field (a player's entity or a ped's), 0 none
Ped* NetIdPed(uint32_t id); // and back to this game's ped (NULL: none or not here)
// The area a snapshot covered around the local player, less a margin (blocks): what the snapshot lacks is gone only
// inside it (beyond, a busy area's snapshot was cut for size). Without a local player, nothing is inside or beyond
class ReachArea
{
  public:
    ReachArea(const Snapshot& snap, float margin);
    bool Inside(const Sprite* s) const;
    bool Beyond(const Sprite* s) const;
    float DistanceTo(const Sprite* s) const;

  private:
    bool mLocated;
    float mX = 0, mY = 0, mReach;
};
// The one dummy ped every traffic car shares as its driver (Car::SpawnDriverPed, CarManager::mSpareDriver): no
// ped of its own, never sent, moved, forced or removed
bool IsSpareDriver(const Ped* ped);
bool OnMap(const Fix16& x, const Fix16& y, uint32_t id); // in the map's 256 blocks; else a CHECK line (snapshot.cpp)
// Moved and turned at once, the collision grid kept right (cars.cpp, snapshot.cpp)
void MoveCar(Car* car, const Fix16& x, const Fix16& y, const Fix16& z, const Ang16& rot);
void MovePed(Ped* ped, const Fix16& x, const Fix16& y, const Fix16& z, const Ang16& rot);
void CapturePed(Ped* p, Car* driving, Car* riding, Snapshot& out);

// The engine's own parts of engine.hpp's areas
int MaxWantedStars(); // the booted game's police setting: the most stars, 0 with police off (netplay.cpp)
int LocalPlayer(); // snapshot.cpp
void ClearRandOwners(); // back to the game's own streams (a single-player game after a network one; replica.cpp)
void KeepPlayerArrows(); // an arrow on the HUD to each other player in the game, made again when one is missing (guards.cpp)
float SnapshotReach(const Snapshot& snap); // schema.cpp
void ResetForNewGame(); // what the engine keeps of a game: the replicas, pickups, death counts (rounds.cpp)
void SetNextMap(const std::string& gmp, const std::string& sty, const std::string& scr, int players, int localPlayer); // netplay.cpp: the next game's

// Players (players.cpp)
void CapturePlayer(int slot, Snapshot& out);
void CaptureWorld(Snapshot& out); // the world entity (world_entity.cpp)
void ApplyWorld(const EntityState& w);
void ApplySlots(const Snapshot& snap); // the slots in the game: the snapshot's
void ApplyPlayer(const EntityState& e);
void KillCredited(Ped* ped, int killerSlot); // the game's own death, credited to a player (-1: nobody)
void KillNow(Ped* ped);
void ApplyFire(Ped* p, bool onFire); // on fire or not as the server has it, the game's own way // the game's own death (Ped::Kill), dead at once even when knocked down
void SnapCamera(Player* p); // the camera at its ped at once, not panning there
void ResetDeaths(int slot); // the round's death count the snapshots carry (a new round)
void ResetDeathEvents(); // and the game's, which a client carries out (a new game), and the scoring bonuses
bool ScoreBonus(Player* p, s32 voice); // gPortScoreBonusHook: a server counts it for the player, nobody shows it there
void ResetHudScores(Player* p); // the HUD's money, frags and multiplier as at a game's start (slots.cpp)

// A client's game (client_mode.cpp)
// The client's sound log (sound_log.cpp): a restore of that tick, a resimulated tick's sound without the mixer (applied:
// what a snapshot's Apply left, before its first tick), the network game's end
void SoundLogRestored(uint32_t tick);
void SoundLogResimTick(bool applied);
void SoundLogStop();
void ProtectPlayers(); // player peds take no damage on a client: their health and deaths are the server's
void SeenByAll(); // every ped and car counts as seen: no removal off screen on a client

// Cars (cars.cpp, car_ai.cpp)
void CarPhysicsFields(std::vector<FieldDesc>& out);
void CaptureCar(Car* c, Snapshot& out);
bool CarHasPhysics(const EntityState& e); // a car entity's physics is there (awake): not all zeros
Car* TowedTrailer(const Car* cab); // the trailer a cab tows (Car::mpTrailer), NULL: none
void ApplyTrailer(Car* cab, const EntityState& e); // hitched or not as the server has it
bool ObjectModelUsable(int model); // an object model the game has a definition for (sanitize.cpp)
void SetCarPhysics(Car* car, const EntityState& e); // the physics the snapshot has for it, after MoveCar
// Whether it has exploded (or is burning out); its state as the server has it: an explosion the server had is carried
// out (the game's own), damage, dents, lights, horn and colour are set. False: it is a wreck here and not there (it has
// to be made again)
bool IsWrecked(const Car* car);
// A model the style has and can wreck (Car::TurnToWreck reads its wreck's car_info): the HOTDOG_D models and the wrecks
// have none, and the game never spawns them (no rating, no script makes one)
bool CarModelWrecks(int model);
// fresh: made or adopted for this entity in this apply, so a wreck there is made one here without blowing up
bool SetCarState(Car* car, const EntityState& e, bool fresh = false);
void ExplodeAsServer(Car* car); // a car blown up as the server's was (gPortCarMayExplode lets it)
extern int gExplosionsCarried, gWrecksReplaced, gWrecksMade; // for the log
// For the log: how each car's state here compared with the snapshot's, before it was applied
struct CarStateDiffs
{
    int cars, damage, fireAbove, fireBelow, wreckHereOnly, wreckThereOnly;
};
extern CarStateDiffs gCarStateDiffs;
int FireLevel(int32_t damage); // Car::mDamageLevel for that damage
bool ClientCarMayExplode(Car* car); // gPortCarMayExplode on a client
void CarAiFields(std::vector<FieldDesc>& out); // after the physics
void CaptureCarAi(const Car* car, EntityState& e);
void ApplyCarAi(Car* car, const EntityState& e);

// Trains (trains.cpp)
void CaptureTrains(Snapshot& out);
void TrainCars(std::vector<Car*>& out); // every carriage of this game's trains (only their leads are in the car pool's list)
// The server's state into this game's train of the same index, and its carriages by the server's car ids. False: this
// game's train differs (nothing applied)
bool ApplyTrain(const EntityState& e, std::vector<std::pair<uint32_t, Car*>>& carriages);
bool IsTrainModel(int model); // a car model that is a train's carriage (Car::IsTrainModel)
// A player driving a train sits in its lead: reversing makes the other end the lead and its driver and leaves the ped in
// its old carriage (Train::ReassignTrainHead), so the ped goes with the seat; one not aboard drives it no longer (guards.cpp)
void GuardTrains();
void PrintTrainLog();

// Getting into and out of cars (car_entry.cpp): at once, ending as the game's own entering and leaving do; or started as
// the enter key starts it
bool InCarTransition(Ped* ped); // on its way into or out of a car: states, or an objective to do it
bool LeavingCar(Ped* ped); // on its way out of one
bool AtCarDoor(Ped* ped); // getting in or out at the door (not the walk there)
void ForceIntoCar(Ped* ped, Car* car);
void ForceIntoCarAsPassenger(Ped* ped, Car* car);
bool IsPassengerOf(Ped* ped, Car* car); // sitting in it as a passenger
bool PassengerThere(const EntityState& e); // a snapshot ped with a car: a passenger, not its driver (ped_state.cpp)
void ForceOutOfCar(Ped* ped, const Fix16& x, const Fix16& y, const Fix16& z, const Ang16& rot);
void ForceOutBesideCar(Ped* ped, Car* car); // out at the car's side (a car's driver needn't point back to it)
void StartEntering(Ped* ped, Car* car);
void StartLeaving(Ped* ped);

// A client's cars and peds (replica.cpp)
void ApplyCarsAndPeds(const Snapshot& snap);
Car* ReplicaCar(uint32_t netId); // this game's car for a server car, if it has one
Ped* ReplicaPed(uint32_t netId); // and ped
const std::unordered_map<uint32_t, Car*>& ReplicaCars();
void ClearReplicas(); // a new game: nothing mapped (rounds.cpp)
const std::unordered_map<uint32_t, Ped*>& ReplicaPeds();
void ReplicaPoses(std::vector<Pose>& out); // the cars and peds of Poses/SetPose
void SetReplicaPose(const Pose& p);
// The replica mapping, saved and restored with the world (rollback.cpp): an opaque copy
struct ReplicaState;
ReplicaState* SaveReplicas(ReplicaState* reuse);
void RestoreReplicas(const ReplicaState* state);
void FreeReplicas(ReplicaState* state);

// A ped's AI state (ped_state.cpp): its fields (kPedAi on), capture, apply. Apply: false when the ped is dead here and
// not on the server (it has to be made again)
void PedStateFields(std::vector<FieldDesc>& out);
void PedStateRefFields(std::vector<int>* peds, std::vector<int>* cars); // the ped entity's fields that are references (net ids)
void CapturePedState(const Ped* p, EntityState& e);
bool ApplyPedState(Ped* p, const EntityState& e);
// Its type and occupation as the server's. A driver made here (PedManager::SpawnDriver) is otherwise the game's dummy
// driver, which a carjack or a shot at its car replaces by a ped of its own while it sits on in the car
void SetPedKind(Ped* p, const EntityState& e);
void ApplyPedGroups(const std::vector<std::pair<Ped*, const EntityState*>>& peds); // once the snapshot's peds are here

// Pickups (pickups.cpp): the server's in a snapshot, and a client's made to match
void CapturePickups(Snapshot& out);
void ApplyPickups(const Snapshot& snap);
std::unordered_map<uint32_t, Object*>& PickupMap(); // for the rollback (ReplicaState)
void ClearPickups(); // a new game: none mapped
extern int gPickupsMade, gPickupsRemoved; // for the log

// This game's live cars and peds (a mapped one may have been removed by the game since; stats.cpp)
void LiveEntities(std::set<Car*>& cars, std::set<Ped*>& peds);

// Other files' statics, by their 10.5.exe address (port/src/port_globals.hpp)
inline u32& NetworkPlayerIdx() // winmain.cpp: this machine's player
{
    static u32& s = PortStatic<u32>(0x6F56C8);
    return s;
}
inline Network_PlayerInputs& CurrentNetInputs() // winmain.cpp: the inputs of the tick, every player's
{
    static Network_PlayerInputs& s = PortStatic<Network_PlayerInputs>(0x6F57D8);
    return s;
}
inline Network_PlayerInputs& PrevNetInputs() // winmain.cpp: the inputs the last tick ran with
{
    static Network_PlayerInputs& s = PortStatic<Network_PlayerInputs>(0x6F5B28);
    return s;
}
inline s16& ZoneTurn() // map_0x370.cpp: the restart zones' turn, so a client respawns where the server does
{
    static s16& s = PortStatic<s16>(0x6F626C);
    return s;
}
} // namespace engine
} // namespace net
