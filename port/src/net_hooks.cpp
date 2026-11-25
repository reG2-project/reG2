// Every hook's definition and default, in net_hooks.hpp's order (port/docs/netcode.md, "The game's code"). port/net/
// sets them; the game's code only reads or calls them.
#include "net_hooks.hpp"

void (*gPortCrashHook)();
void (*gPortSpriteMovedHook)(Sprite*);
void (*gPortIdleHook)();
bool gPortNetFrameLoop;
s32 gPortNetPaceMs = -1;
bool gPortNoDraw;

bool gPortNoAudio;
void (*gPortSampleStartHook)(const void*);
void (*gPortSoundRequestsHook)(bool);
bool gPortServer;
bool gPortNoRumble;
s32 gPortTrafficCap = 16;
s32 gPortSpawnPlayers = 1;
void (*gPortAfterPlayersHook)();
void (*gPortExtraPlayerPeds)();
bool (*gPortSpawnBlock)(Ped*, gmp_map_zone*, s32*, s32*, s32*);
void (*gPortUpdateCars)();
bool (*gPortWorldStill)();
bool (*gPortCarMayExplode)(Car* pCar);
s32 (*gPortCarHornId)(const Car* pCar);
void (*gPortObjectMade)(Object* pObject);
bool (*gPortBlastHurts)(Explosion* pExplosion);
bool (*gPortBlastDebris)(Explosion* pExplosion);
bool (*gPortPedMayDie)(Ped* pPed);
bool (*gPortCharSolid)(const Char* pChar);
bool (*gPortPedMayFire)(Ped* pPed);
bool (*gPortMayPullOut)(Ped* pVictim);
bool (*gPortPedMayGo)(Ped* pPed);
bool (*gPortScriptCommandRuns)(u16 type);
void (*gPortFrontendSetup)(Frontend* pFrontend);
bool (*gPortFrontendInput)(Frontend* pFrontend);
int (*gPortLeaveNetworkGame)();
void (*gPortDrawOverlay)();
bool (*gPortDrawNetStandings)();
bool (*gPortNextNetworkGame)();
bool gPortKeepWantedPoints;
bool gPortNoOwnBus;
bool gPortServerDespawns;
bool gPortServerKillNotices;
bool gPortServerKillAwards;
bool gPortTagTimeCounts;
bool gPortGroundedObjects;
bool gPortPlayersDeathCry;
bool gPortFullQueueRanked;
u16 (*gPortChatChar)(Player*, s32);
bool gPortProtectsPlayers;
bool (*gPortScoreBonusHook)(Player* pPlayer, s32 voice);
s32 (*gPortKillCamTicks)(Player* pPlayer, Player* pKiller);
Ped* (*gPortDeathViewPed)(Player* pPlayer, Ped* pAuxPed);
int gPortNetRoundPad;
