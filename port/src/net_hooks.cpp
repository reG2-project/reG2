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
bool (*gPortWorldStill)();
bool (*gPortCarMayExplode)(Car* pCar);
bool (*gPortScriptCommandRuns)(u16 type);
void (*gPortFrontendSetup)(Frontend* pFrontend);
bool (*gPortFrontendInput)(Frontend* pFrontend);
int (*gPortLeaveNetworkGame)();
void (*gPortDrawOverlay)();
bool (*gPortDrawNetStandings)();
bool (*gPortNextNetworkGame)();
bool gPortKeepWantedPoints;
bool gPortServerKillNotices;
bool gPortServerKillAwards;
bool gPortTagTimeCounts;
u16 (*gPortChatChar)(Player*, s32);
bool gPortProtectsPlayers;
bool (*gPortScoreBonusHook)(Player* pPlayer, s32 voice);
s32 (*gPortKillCamTicks)(Player* pPlayer, Player* pKiller);
Ped* (*gPortDeathViewPed)(Player* pPlayer, Ped* pAuxPed);
int gPortNetRoundPad;
