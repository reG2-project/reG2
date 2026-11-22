// The network game's hooks (port/docs/netcode.md, "The game's code"): what the game's code (Source/, under GTA2_PORT),
// compat/ and port/src/ call or read for it, in one place; port/net/ sets them. Each is NULL, or the original's value,
// until a network game starts.
#pragma once
#include "types.hpp"

class Car;
class Ped;
class Player;
class gmp_map_zone;
class Sprite;
struct Frontend;

// The crash report (compat/src/main.cpp): installed for network games only, as it isn't signal-safe; the hook is called
// last, the network game's view of the game
void port_InstallCrashReport();
extern void (*gPortCrashHook)();
// After a sprite moved (Sprite::InvalidateZColAndBBoxes, every setter of its place calls it); NULL but for a check
// (GTA2_NET_CHECKGRID: one moved off the map)
extern void (*gPortSpriteMovedHook)(Sprite*);
// While the game waits for its next step: the netcode's polling, so a ping is answered at once and neither a step nor
// a frame wait adds to the round trip
extern void (*gPortIdleHook)();
// A network client's: reG2's frame loop runs its game (winmain.cpp), the netcode's frame hook after each step and
// frames drawn in between; a server and a replay run the original's, a step then a frame
extern bool gPortNetFrameLoop;
// A headless dedicated server's: the original's frame path (winmain.cpp) runs no Game::Draw after a step, as nobody
// sees it and the steps don't depend on it (port/docs/netcode.md, "The game loop")
extern bool gPortNoDraw;

// No audio device for the game: sound_obj then doesn't run at all. A network server's: nobody hears it, and its
// listener follows slot 0's camera, whose ped (once that player left) can be any ped's, in any car
extern bool gPortNoAudio;
// Called in sound_obj::ServiceSoundEffects before (false) and after (true) the audio entities ask for their samples;
// NULL but on a network client, whose sound log notes them and adds what its resimulated ticks owe
extern void (*gPortSoundRequestsHook)(bool requested);
// After the mixer starts a sample (compat/src/miles.cpp, miles3d.cpp: its handle); NULL but for a log (GTA2_NET_SFXLOG)
extern void (*gPortSampleStartHook)(const void* handle);
// A dedicated server (--server), known before anything runs (compat/src/main.cpp): no message box, no setup, and none of
// the files only a player uses (the keyboard layout, the attract mode replays)
extern bool gPortServer;
// No pad vibration from the game's events (rumble.cpp): a network server's, whose local player is slot 0's remote one,
// and a client's resimulation, whose hits and crashes were felt when those ticks were predicted
extern bool gPortNoRumble;
extern s32 gPortTrafficCap; // the ambient traffic cars per player: 16, or a network server's --traffic
// The players the ambient traffic and peds are for (Car.cpp, Char_Pool.cpp: the game's limits are for the whole game, and
// the first players' cameras took them all); 1 but on a network server
extern s32 gPortSpawnPlayers;
// The ambient traffic cars for the whole game (Car.cpp, in place of the original's 16)
inline s32 PortTrafficLimit()
{
    return gPortTrafficCap * gPortSpawnPlayers;
}
// Called in Game::UpdateGame after the players' tick (their cameras moved), before anything spawns; NULL but on a
// network server, which moves a client's cameras' bounds to where that client already shows them (its lag ahead): a car
// or ped spawned out of sight here would come into a fast client's view
extern void (*gPortAfterPlayersHook)();
// Called in Game::BootGame after the map script's level start, before the players are set up: NULL but in a network
// game, whose players past the script's PLAYER_PEDs get a ped there
extern void (*gPortExtraPlayerPeds)();
// Asked as a ped (re)starts at a restart zone (Ped::SpawnCharInZone): true, and it starts on the block *x, *y at the
// level *z instead of the zone's first block with ground. NULL but in a network game: a block nobody stands on
extern bool (*gPortSpawnBlock)(Ped* pPed, gmp_map_zone* pZone, s32* x, s32* y, s32* z);
// Asked in Game::UpdateGame after the players' tick: true, and nothing else runs this tick but the HUD (a network
// game's results: the world stands still, the chat goes on)
extern bool (*gPortWorldStill)();
// Asked as a car would blow up (Car::HandleCarExplosion): false, and it doesn't. NULL but on a network client, whose cars
// the server sends blow up when the server's do (a prediction can't be taken back)
extern bool (*gPortCarMayExplode)(Car* pCar);
// Asked for each command a script thread runs (ScriptThread::PreExecOpCode): false, and it is stepped over. NULL but on a
// network client, which runs its map script's level start for what only shows or sounds here (the server runs the rest)
extern bool (*gPortScriptCommandRuns)(u16 type);
// The frontend's network page (port/net/menu.cpp): set up after the game's own (SetupMenuStringsOptionsElements),
// and its input taken first (UpdatePageFromUserInput: true, the game's handling is skipped)
extern void (*gPortFrontendSetup)(Frontend* pFrontend);
extern bool (*gPortFrontendInput)(Frontend* pFrontend);
// winmain.cpp, a network game over: the frontend page to go back to (a client that left), or -1 (the game's own)
extern int (*gPortLeaveNetworkGame)();
extern void (*gPortDrawOverlay)(); // drawn last in Game::Draw; NULL unless the netcode draws over the frame
// Asked when a network game has ended (winmain.cpp): true to start the next one at once, without the frontend (a
// dedicated server's map change)
extern bool (*gPortNextNetworkGame)();
// A network client's: its police are off (bSkip_police) but the server's wanted points stand, so Ped's tick neither
// zeroes them nor registers a criminal: zeroed each tick, the HUD would show no cop heads and the cops chasing a player
// would give up between snapshots
extern bool gPortKeepWantedPoints;
// A network client's: the HUD's kill notices (Hud_MpMessage::AnnounceKill) are the server's deaths only, not the ones
// its prediction starts and a restore undoes
extern bool gPortServerKillNotices;
// A network client's: a kill's points and bonuses (PlayerScoreTracker::AwardPedKilledScore, with the multi-kill messages
// and voices) are the server's, whose scores come with its snapshots, not its prediction's
extern bool gPortServerKillAwards;
// A network game's: the tag game counts the fugitive's seconds (TagGame::ExecuteGamemodeTick) though the game has no
// time limit (the netcode ends the rounds), whose flag the original counts by
extern bool gPortTagTimeCounts;
// A network server's: the character a player's client typed with the key <dik> this tick, in its keyboard's layout
// (0: none), for its chat line (Hud_ChatInput::HandleChatInputKey); the server's own table is a US keyboard's
extern u16 (*gPortChatChar)(Player* pPlayer, s32 dik);
// A network client's: its players are kept at mInvulnerability 2 so that only the server takes their health
// (ProtectPlayers); how a ped reacts to a car, a blast or fire goes by the invulnerability the server gives
extern bool gPortProtectsPlayers;
inline bool PortReactsInvulnerable(s32 invulnerability, bool player)
{
    return gPortProtectsPlayers && player ? invulnerability > 2 : invulnerability > 0;
}
// A scoring bonus (PlayerScoreTracker: a kill streak, a stunt...) with its voice line (voice_line.hpp): true, and this
// machine shows its message and plays the voice. NULL: the user's player's, as the original; in a network game the
// server sends each player its own and a client shows only those
extern bool (*gPortScoreBonusHook)(Player* pPlayer, s32 voice);
// A network client's round keys on the gamepad (port_NetRoundPad, compat/src/pad.cpp): 0 none (no network game), 1
// Back is F1 (ready, or yes to a vote), 2 the round's results: also the d-pad and A as the arrows and Enter (a map)
extern int gPortNetRoundPad;
