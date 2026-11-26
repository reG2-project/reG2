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
#include "Source/TagGame.hpp"
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
        FreshScores(i); // every slot's (a new map's game starts before the map script made the players' peds)
        Player* p = InGamePed(i) ? GetPlayer(i) : NULL;
        if (p)
        {
            p->UnloadCarWeapons();
            p->RemovePlayerWeapons();
            p->ClearPowerUpsExceptJailCard();
            PutInGame(i, true); // at the next restart zone
        }
    }
    // A tag game's "it" again as at a game's start: the first player's, at the next tick (TagGame::ExecuteGamemodeTick)
    gTagGame.mFugitive = NULL;
    gTagGame.mTickTimer = 0;
}

namespace
{
uint32_t gEndTick; // the netcode tick a map change ends the game after
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

void net::engine::ChangeMap(const std::string& gmp, const std::string& sty, const std::string& scr, int players, int localPlayer, uint32_t tick,
                            uint32_t endTick)
{
    SetNextMap(gmp, sty, scr, players, localPlayer);
    gEndTick = endTick;
    KeepMapChange(tick);
}

// The game's exit timer, set again every frame: a client's restore brings back a game that wasn't ending
void net::engine::KeepMapChange(uint32_t tick)
{
    if (MapChangePending() && gGame_0x40)
    {
        // A network game's timer counts twice a tick (Game::ExecuteGame and do_network_and_local_inputs); only ever sooner
        gGame_0x40->ExitGameNoBonus(tick < gEndTick ? (s32)(2 * (gEndTick - tick)) : 0, GameExitType::MultiplayerExit_5);
    }
}

void net::engine::ResetForNewGame()
{
    ClearReplicas();
    ClearPickups();
    ClearShots();
    SoundLogNewGame();
    for (int i = 0; i < MAX_PLAYERS; i++)
    {
        ResetDeaths(i);
    }
    ResetDeathEvents();
}
