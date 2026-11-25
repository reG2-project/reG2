// port/net core (port/docs/netcode.md, "Running"): how many players a multiplayer map takes. Its script makes a ped for
// each (PLAYER_PED, ScriptThread::Handle_PLAYER_PED) and the game has no other: the PLAYER_PEDs, at most the .mmp's
// PlayerCount when it has one (maps since Vike's 11.3 leave it out: any number), at most kMaxPlayers.
#pragma once
#include <stddef.h>
#include <stdint.h>

namespace net
{
// The PLAYER_PED declarations of a compiled script (data/<name>.scr, as ScriptManager::Load reads it); -1: too short
int ScriptPlayerPeds(const uint8_t* scr, size_t size);
// <mmpPlayerCount> 0: none in the .mmp
int MapPlayers(int scriptPlayerPeds, int mmpPlayerCount);
} // namespace net
