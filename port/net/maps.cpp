// port/net (port/docs/netcode.md, "Running"): see maps.hpp.
#include "maps.hpp"
#include "cmdline.hpp"
#include "core/map_players.hpp"
#include <SDL3/SDL.h>
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

namespace
{
std::string GameFolder()
{
    char* dir = SDL_GetCurrentDirectory();
    const std::string s = dir ? dir : "the game folder";
    SDL_free(dir);
    return s;
}

// The PLAYER_PEDs of data/<scr> (-1: unreadable)
int ScriptPlayerPeds(const std::string& scr)
{
    FILE* f = fopen(("data/" + scr).c_str(), "rb"); // the port's: whatever the case, as the game opens it
    if (!f)
    {
        return -1;
    }
    std::vector<uint8_t> data(6000 * 2 + 65536);
    const size_t n = fread(data.data(), 1, data.size(), f);
    fclose(f);
    return net::ScriptPlayerPeds(data.data(), n);
}

// data/<name>.mmp's files into <g> ([MapFiles] GMPFile, STYFile, SCRFile) and the players it takes into <players>
// (net::MapPlayers); false: no such map (printed if <required>), one of its files missing, names a client would refuse
// (net::BareFileName) or a script without players, printed
bool ReadMmp(const std::string& name, net::engine::NetworkGame& g, bool required, int* players)
{
    if (!net::BareFileName(name, ""))
    {
        fprintf(stderr, "net: %s: a map's name is letters, digits, - and _ (at most %u)\n", name.c_str(), (unsigned)net::kMaxFileName);
        return false;
    }
    FILE* f = fopen(("data/" + name + ".mmp").c_str(), "r");
    if (!f)
    {
        if (required)
        {
            fprintf(stderr, "net: no map data/%s.mmp in %s\n", name.c_str(), GameFolder().c_str());
        }
        return false;
    }
    char line[512];
    int playerCount = 0;
    while (fgets(line, sizeof(line), f))
    {
        char key[64], value[260];
        if (sscanf(line, " %63[^= ] = %259[^\r\n]", key, value) == 2)
        {
            for (size_t n = strlen(value); n && isspace((unsigned char)value[n - 1]); n--)
            {
                value[n - 1] = 0; // "redneck_arena.gmp "
            }
            if (!strcmp(key, "PlayerCount"))
            {
                playerCount = atoi(value);
            }
            else if (!strcmp(key, "GMPFile"))
            {
                g.gmp = value;
            }
            else if (!strcmp(key, "STYFile"))
            {
                g.sty = value;
            }
            else if (!strcmp(key, "SCRFile"))
            {
                g.scr = value;
            }
        }
    }
    fclose(f);
    if (g.gmp.empty() || g.sty.empty() || g.scr.empty())
    {
        fprintf(stderr, "net: data/%s.mmp in %s: no GMPFile, STYFile or SCRFile\n", name.c_str(), GameFolder().c_str());
        return false;
    }
    const std::string bad = net::BadMapName(name, g.gmp, g.sty, g.scr);
    if (!bad.empty())
    {
        fprintf(stderr, "net: data/%s.mmp: its %s file isn't a plain file name in data/ (letters, digits, - and _, at most %u, and its extension)\n",
                name.c_str(), bad.c_str(), (unsigned)net::kMaxFileName);
        return false;
    }
    bool found = true;
    for (const std::string* file : {&g.gmp, &g.sty, &g.scr})
    {
        FILE* data = fopen(("data/" + *file).c_str(), "rb"); // the port's: whatever the case, as the game opens it
        if (data)
        {
            fclose(data);
        }
        else
        {
            fprintf(stderr, "net: data/%s.mmp's data/%s isn't in %s\n", name.c_str(), file->c_str(), GameFolder().c_str());
            found = false;
        }
    }
    if (!found)
    {
        return false;
    }
    const int peds = ScriptPlayerPeds(g.scr);
    *players = net::MapPlayers(peds, playerCount);
    if (*players < 1)
    {
        fprintf(stderr, "net: data/%s.mmp's script data/%s %s\n", name.c_str(), g.scr.c_str(), peds < 0 ? "isn't a script" : "has no PLAYER_PED");
        return false;
    }
    return true;
}
} // namespace

std::string net::MapTitle(const std::string& name)
{
    if (!BareFileName(name, ""))
    {
        return name;
    }
    FILE* f = fopen(("data/" + name + ".mmp").c_str(), "r");
    if (!f)
    {
        return name;
    }
    std::string title;
    char line[512];
    while (fgets(line, sizeof(line), f) && title.empty())
    {
        char key[64], value[260];
        if (sscanf(line, " %63[^= ] = %259[^\r\n]", key, value) == 2 && !strcmp(key, "Description"))
        {
            title = value;
        }
    }
    fclose(f);
    while (!title.empty() && isspace((unsigned char)title.back()))
    {
        title.pop_back();
    }
    // " 6player", " 3Player", " 10player", " 4 player"
    std::string lower = title;
    for (char& c : lower)
    {
        c = (char)tolower((unsigned char)c);
    }
    if (lower.size() > 6 && lower.compare(lower.size() - 6, 6, "player") == 0)
    {
        size_t end = lower.size() - 6;
        end -= end && lower[end - 1] == ' ';
        size_t start = end;
        while (start && isdigit((unsigned char)lower[start - 1]))
        {
            start--;
        }
        if (start < end && start && lower[start - 1] == ' ')
        {
            title.erase(start - 1);
        }
    }
    return title.empty() ? name : title;
}

bool net::SetUpMaps(const std::string& firstMap, const std::string& maps, ServerConfig& sc)
{
    std::string map = firstMap;
    // The rotation: each a map's files, the first played first
    for (const std::string& name : maps.empty() ? std::vector<std::string>() : SplitCommas(maps))
    {
        engine::NetworkGame files;
        int players = 0;
        if (!ReadMmp(name, files, true, &players))
        {
            return false;
        }
        sc.maps.push_back(MapFiles{name, files.gmp, files.sty, files.scr, players});
    }
    if (!sc.maps.empty())
    {
        map = sc.maps[0].name;
    }
    const int asked = sc.game.players;
    int firstPlayers = 0;
    if (!ReadMmp(map, sc.game, true, &firstPlayers))
    {
        return false;
    }
    // What a vote chooses from: the rotation, else the game's multiplayer maps (those there are)
    sc.voteMaps = sc.maps;
    if (sc.maps.size() < 2)
    {
        sc.voteMaps.clear();
        // The six maps the game ships, by name
        for (const char* name : {"mp1-6p", "mp2-6p", "mp5-6p", "downtown-6p", "industrial-6p", "res-6p"})
        {
            engine::NetworkGame files;
            int players = 0;
            if (ReadMmp(name, files, false, &players))
            {
                sc.voteMaps.push_back(MapFiles{name, files.gmp, files.sty, files.scr, players});
            }
        }
    }
    // By name, else by script (Mini City's .mmp files share one, each its own player count)
    sc.currentMap = -1;
    for (size_t m = 0; m < sc.voteMaps.size(); m++)
    {
        sc.currentMap = sc.currentMap < 0 && sc.voteMaps[m].scr == sc.game.scr ? (int)m : sc.currentMap;
    }
    for (size_t m = 0; m < sc.voteMaps.size(); m++)
    {
        sc.currentMap = SDL_strcasecmp(sc.voteMaps[m].name.c_str(), map.c_str()) == 0 ? (int)m : sc.currentMap;
    }
    if (sc.currentMap < 0) // a map of its own: in the list too
    {
        sc.voteMaps.insert(sc.voteMaps.begin(), MapFiles{map, sc.game.gmp, sc.game.sty, sc.game.scr, firstPlayers});
        sc.currentMap = 0;
    }
    // The slots: as many as the server's biggest map takes at most; the game on a map, as many as it takes
    int most = 0;
    for (const MapFiles& m : sc.voteMaps)
    {
        most = m.players > most ? m.players : most;
    }
    sc.slots = asked && asked < most ? asked : most;
    if (asked > most)
    {
        printf("net: --players %d: the maps here take %d players at most, %d player slots\n", asked, most, sc.slots);
    }
    sc.game.players = firstPlayers < sc.slots ? firstPlayers : sc.slots;
    if (sc.game.players < sc.slots)
    {
        printf("net: %s takes %d players, the other slots open on a map that takes more\n", map.c_str(), sc.game.players);
    }
    return true;
}
