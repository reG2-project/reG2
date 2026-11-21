// port/net (port/docs/netcode.md, "Playing"): a match as rounds on one map. The game ends a network game at its frag
// limit (PlayerScoreTracker: frags >= the limit) and its time limit (ManageNetworkTimeAndHudPager), which a
// dedicated server can't come back from; so the game gets neither (StartNetworkGame) and the server ends a round
// itself (session/server.cpp), here: scores, weapons and power-ups gone, everyone at a restart zone.
#include "internal.hpp"
#include "../core/crc32.hpp"
#include "Source/Game.hpp"
#include "Source/GameSession.hpp"
#include "Source/Hud.hpp"
#include "Source/Player.hpp"
#include "Source/hud_message_priority.hpp"
#include <stdio.h>
#include <string.h>
#include <vector>

net::engine::Round& net::engine::CurrentRound()
{
    static Round round;
    return round;
}

void net::engine::NewRound()
{
    for (int i = 0; i < PlayerCount() && i < MAX_PLAYERS; i++)
    {
        // Every slot's score (a new map's game starts before the map script made the players' peds)
        gGameSession.mFragsList[i] = 0;
        gGameSession.mPointsList[i] = 0;
        ResetDeaths(i);
        Player* p = InGamePed(i) ? GetPlayer(i) : NULL;
        if (p)
        {
            p->UnloadCarWeapons();
            p->RemovePlayerWeapons();
            p->ClearPowerUpsExceptJailCard();
            ResetHudScores(p);
            PutInGame(i, true); // at the next restart zone
        }
    }
}

namespace
{
int gExitTicks; // the pause left before a map change ends the game
} // namespace

std::string net::engine::MissingMapFile(const std::string& gmp, const std::string& sty, const std::string& scr)
{
    for (const std::string* name : {&gmp, &sty, &scr})
    {
        FILE* f = fopen(("data\\" + *name).c_str(), "rb"); // as the game opens it
        if (!f)
        {
            return *name;
        }
        fclose(f);
    }
    return "";
}

uint32_t net::engine::DataFileCrc(const std::string& name)
{
    FILE* f = fopen(("data\\" + name).c_str(), "rb");
    if (!f)
    {
        return 0;
    }
    uint32_t crc = 0;
    std::vector<char> buffer(1 << 16);
    size_t n;
    while ((n = fread(buffer.data(), 1, buffer.size(), f)) > 0)
    {
        crc = Crc32(buffer.data(), n, crc);
    }
    fclose(f);
    return crc;
}

void net::engine::ChangeMap(const std::string& gmp, const std::string& sty, const std::string& scr, int pauseTicks)
{
    SetNextMap(gmp, sty, scr);
    gExitTicks = pauseTicks;
    KeepMapChange();
}

// The game's exit timer, set again every frame: a client's restore brings back a game that wasn't ending
void net::engine::KeepMapChange()
{
    if (MapChangePending() && gGame_0x40)
    {
        gGame_0x40->ExitGameNoBonus(gExitTicks, GameExitType::MultiplayerExit_5); // only ever sooner
        gExitTicks = gExitTicks > 1 ? gExitTicks - 1 : 1;
    }
}

void net::engine::ResetForNewGame()
{
    ClearReplicas();
    ClearPickups();
    for (int i = 0; i < MAX_PLAYERS; i++)
    {
        ResetDeaths(i);
    }
    ResetDeathEvents();
}
