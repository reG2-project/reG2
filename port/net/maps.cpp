// port/net (port/docs/netcode.md, "Running"): see maps.hpp.
#include "maps.hpp"
#include "cmdline.hpp"
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

// data/<name>.mmp's files into <g> ([MapFiles] GMPFile, STYFile, SCRFile); false: no such map (printed if <required>),
// one of its files missing, or names a client would refuse (net::BareFileName), printed
bool ReadMmp(const std::string& name, net::engine::NetworkGame& g, bool required)
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
    while (fgets(line, sizeof(line), f))
    {
        char key[64], value[260];
        if (sscanf(line, " %63[^= ] = %259[^\r\n]", key, value) == 2)
        {
            if (!strcmp(key, "GMPFile"))
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
    return found;
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
    // " 6player", " 3Player"
    const size_t space = title.rfind(' ');
    std::string last = space == std::string::npos ? "" : title.substr(space + 1);
    for (char& c : last)
    {
        c = (char)tolower((unsigned char)c);
    }
    if (last.size() == 7 && isdigit((unsigned char)last[0]) && last.compare(1, 6, "player") == 0)
    {
        title.erase(space);
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
        if (!ReadMmp(name, files, true))
        {
            return false;
        }
        sc.maps.push_back(MapFiles{name, files.gmp, files.sty, files.scr});
    }
    if (!sc.maps.empty())
    {
        map = sc.maps[0].name;
    }
    if (!ReadMmp(map, sc.game, true))
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
            if (ReadMmp(name, files, false))
            {
                sc.voteMaps.push_back(MapFiles{name, files.gmp, files.sty, files.scr});
            }
        }
    }
    sc.currentMap = -1;
    for (size_t m = 0; m < sc.voteMaps.size(); m++)
    {
        sc.currentMap = sc.voteMaps[m].scr == sc.game.scr ? (int)m : sc.currentMap;
    }
    if (sc.currentMap < 0) // a map of its own: in the list too
    {
        sc.voteMaps.insert(sc.voteMaps.begin(), MapFiles{map, sc.game.gmp, sc.game.sty, sc.game.scr});
        sc.currentMap = 0;
    }
    return true;
}
