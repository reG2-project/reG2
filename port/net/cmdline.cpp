// port/net (port/docs/netcode.md, "Running"): the netcode's command line; kUsage lists it.
#include "cmdline.hpp"
#include "core/protocol.hpp"
#include <algorithm>
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>

#define NET_STR2(x) #x
#define NET_STR(x) NET_STR2(x)

namespace
{
const char kUsage[] =
    "reG2: GTA2 for today's systems. Without options it starts the game.\n"
    "\n"
    "  --data <folder>       the game folder (or its data folder): else $GTA2_DIR, the current folder or the\n"
    "                        executable's when they hold data/, else the per-user folder\n"
    "  --setup               the setup screens, to replace the game data or add the music\n"
    "  --help                this list\n"
    "\n"
    "A dedicated server (headless: no window, no dialog, no sound; errors go to stderr and reG2-server.log):\n"
    "  --server              needs data/fstyle.sty, nyc.gci, a .gxt and its maps' .mmp, .gmp, .sty, .scr\n"
    "  --port N              UDP port, 27920\n"
    "  --map <mmp>           a multiplayer map in data/, downtown-6p\n"
    "  --maps <mmp>,<mmp>    a rotation: a round on each in turn, the first instead of --map\n"
    "  --players N           player slots (1-" NET_STR(NET_MAX_PLAYERS) ", " NET_STR(NET_MAX_PLAYERS) "), on every map: pick maps made for as many\n"
    "  --gametype 1-3        frags, points, tag\n"
    "  --fraglimit N         10 (a points game's is money: 10000)\n"
    "  --timelimit min       a round's time limit\n"
    "  --tickms N            the simulation step, 33\n"
    "  --police [stars]      police on (off by default), up to 1 star or 1-6\n"
    "  --kit smg             every start and respawn: a machine gun with 50 shots and armour\n"
    "  --zoom N              every player's camera zoom, -5 (wider) to 5 (closer), the CAMERA ZOOM setting's steps; -1\n"
    "  --traffic N           ambient traffic cars per player, 16\n"
    "  --packets N           a snapshot's packets at most, 1-8, 4\n"
    "  --interest blocks     how far around a client its snapshots reach, 24 at zoom 0 (27 at -1)\n"
    "  --window              show the server in a window\n"
    "  The round flow: --minplayers N (2), --warmup s (60), --results s (15), --votes start,map,kick,\n"
    "  --rounds on|off (on, off with --arena)\n"
    "  Bots on the server itself: --arena brain,brain,... (simple, carhop, rammer, soldier, hunter, nn:<file>),\n"
    "  --ticks N (prints the result and ends at tick N), --seed N\n"
    "  SIGINT or SIGTERM ends it, telling its players, with exit code 0.\n"
    "\n"
    "A client:\n"
    "  --connect host[:port] [--name <name>] [--bot [brain]]\n"
    "  --replay <file>       a recorded session (recordings/session-<n>.rec), or a server's (server-<n>.rec):\n"
    "                        that server again, headless, its world checked against the recording's\n"
    "\n"
    "Either end: a link conditioner on this end's sends (tests): --lag ms, --jitter ms, --loss percent,\n"
    "  --lossburst N, --spike ms:on:every; --mtu N (1280, 1428 the most)\n"
    "\n"
    "More: port/docs/netcode.md\n";
} // namespace

void net::PrintUsage()
{
    fputs(kUsage, stdout);
}

bool net::ParseOptions(std::vector<std::string>& args, NetOptions& o)
{
    ServerConfig& sc = o.sc;
    ClientConfig& cc = o.cc;
    sc.game.players = 0; // no --players: NET_MAX_PLAYERS (SetUpMaps)
    std::vector<std::string> rest;
    // A server's options: an error without --server, so a single-player game never takes one (--traffic) or drops it
    static const char* const kServerOnly[] = {"--window", "--port", "--map", "--maps", "--minplayers", "--warmup", "--results",
                                              "--votes", "--rounds", "--players", "--gametype", "--fraglimit", "--timelimit",
                                              "--tickms", "--interest", "--traffic", "--police", "--kit", "--zoom", "--packets", "--arena", "--seed",
                                              "--ticks"};
    std::string serverOption;
    bool fragLimitGiven = false, interestGiven = false;
    for (size_t i = 0; i < args.size(); i++)
    {
        const std::string& a = args[i];
        const bool more = i + 1 < args.size();
        for (const char* s : kServerOnly)
        {
            serverOption = serverOption.empty() && a == s ? a : serverOption;
        }
        if (a == "--server")
        {
            o.server = true;
        }
        else if (a == "--window")
        {
            o.window = true;
        }
        else if (a == "--connect" && more)
        {
            o.connect = args[++i];
        }
        else if (a == "--port" && more)
        {
            const int port = atoi(args[++i].c_str());
            if (port < 1 || port > 65535)
            {
                fprintf(stderr, "net: --port %s isn't a port (1-65535)\n", args[i].c_str());
                return false;
            }
            sc.port = (uint16_t)port;
        }
        else if (a == "--map" && more)
        {
            o.map = args[++i];
        }
        else if (a == "--minplayers" && more)
        {
            sc.rules.minPlayers = atoi(args[++i].c_str());
        }
        else if (a == "--warmup" && more)
        {
            sc.rules.warmupSec = atoi(args[++i].c_str());
        }
        else if (a == "--results" && more)
        {
            sc.rules.resultsSec = atoi(args[++i].c_str());
        }
        else if (a == "--votes" && more)
        {
            const std::string v = args[++i];
            sc.rules.votes = (v.find("start") != std::string::npos ? 1u << kVoteStart : 0) |
                             (v.find("map") != std::string::npos ? 1u << kVoteMap : 0) | (v.find("kick") != std::string::npos ? 1u << kVoteKick : 0);
        }
        else if (a == "--rounds" && more)
        {
            o.rounds = args[++i]; // on, off (default: on, off with --arena)
        }
        else if (a == "--maps" && more)
        {
            o.maps = args[++i];
        }
        else if (a == "--players" && more)
        {
            const int players = atoi(args[++i].c_str());
            if (players < 1 || players > kMaxPlayers)
            {
                fprintf(stderr, "net: --players %s: 1-%d\n", args[i].c_str(), kMaxPlayers);
                return false;
            }
            sc.game.players = players;
        }
        else if (a == "--gametype" && more)
        {
            sc.game.gameType = atoi(args[++i].c_str());
        }
        else if (a == "--fraglimit" && more)
        {
            sc.game.fragLimit = atoi(args[++i].c_str());
            fragLimitGiven = true;
        }
        else if (a == "--timelimit" && more)
        {
            sc.game.timeLimitMin = atoi(args[++i].c_str());
        }
        else if (a == "--tickms" && more)
        {
            sc.game.tickMs = atoi(args[++i].c_str());
        }
        else if (a == "--interest" && more)
        {
            sc.interestRadius = (float)atof(args[++i].c_str());
            interestGiven = true;
        }
        else if (a == "--traffic" && more)
        {
            o.traffic = atoi(args[++i].c_str());
        }
        else if (a == "--police")
        {
            sc.game.police = 1; // the lobby's "police" box (off by default there too)
            if (more && isdigit((unsigned char)args[i + 1][0]))
            {
                sc.game.police = std::max(1, std::min(6, atoi(args[++i].c_str()))); // the most stars (the game's own: 1)
            }
        }
        else if (a == "--kit" && more)
        {
            sc.game.kit = args[++i] == "smg" ? 1 : 0;
        }
        else if (a == "--zoom" && more)
        {
            sc.game.zoom = std::max(-5, std::min(5, atoi(args[++i].c_str())));
        }
        else if (a == "--packets" && more)
        {
            sc.packets = atoi(args[++i].c_str());
        }
        else if (a == "--arena" && more)
        {
            sc.arena = SplitCommas(args[++i]);
        }
        else if (a == "--seed" && more)
        {
            sc.seed = (uint32_t)strtoul(args[++i].c_str(), NULL, 10);
        }
        else if (a == "--ticks" && more)
        {
            sc.endTick = (uint32_t)atoi(args[++i].c_str());
        }
        else if (a == "--name" && more)
        {
            cc.name = args[++i];
        }
        else if (a == "--bot")
        {
            cc.bot = more && args[i + 1].compare(0, 2, "--") != 0 ? args[++i] : "simple"; // --bot [brain]
            if (!CreateBotBrain(cc.bot))
            {
                fprintf(stderr, "net: no bot brain called %s\n", cc.bot.c_str());
                return false;
            }
        }
        else if (a == "--replay" && more)
        {
            o.replay = args[++i];
        }
        else if (a == "--mtu" && more)
        {
            SetLinkMtu(atoi(args[++i].c_str()));
        }
        else if (a == "--lag" && more)
        {
            sc.link.lagMs = cc.link.lagMs = atoi(args[++i].c_str());
        }
        else if (a == "--jitter" && more)
        {
            sc.link.jitterMs = cc.link.jitterMs = atoi(args[++i].c_str());
        }
        else if (a == "--loss" && more)
        {
            sc.link.lossPercent = cc.link.lossPercent = atoi(args[++i].c_str());
        }
        else if (a == "--lossburst" && more)
        {
            sc.link.lossBurst = cc.link.lossBurst = atoi(args[++i].c_str());
        }
        else if (a == "--spike" && more)
        {
            LinkShape& l = cc.link;
            if (sscanf(args[++i].c_str(), "%d:%d:%d", &l.spikeMs, &l.spikeOnS, &l.spikeEveryS) != 3 || l.spikeEveryS <= 0)
            {
                fprintf(stderr, "net: --spike ms:on:every (seconds)\n");
                return false;
            }
            sc.link.spikeMs = l.spikeMs, sc.link.spikeOnS = l.spikeOnS, sc.link.spikeEveryS = l.spikeEveryS;
        }
        else
        {
            rest.push_back(a);
        }
    }
    if (!serverOption.empty() && !o.server)
    {
        fprintf(stderr, "net: %s is a server's option (--server)\n", serverOption.c_str());
        return false;
    }
    if (sc.game.gameType == 2 && !fragLimitGiven)
    {
        sc.game.fragLimit = 10000; // a points game's limit is money: the original lobby's default (NetUi, "s_limit")
    }
    if (!interestGiven)
    {
        sc.interestRadius *= ViewScale(sc.game.zoom); // the same margin around a wider view
    }
    args = rest;
    return true;
}

void net::ParseAddress(const std::string& address, std::string* host, uint16_t* port)
{
    const size_t colon = address.rfind(':');
    *host = address.substr(0, colon);
    if (colon != std::string::npos)
    {
        *port = (uint16_t)atoi(address.c_str() + colon + 1);
    }
}

std::vector<std::string> net::SplitCommas(const std::string& list)
{
    std::vector<std::string> out;
    for (size_t at = 0, end = 0; end != std::string::npos; at = end + 1)
    {
        end = list.find(',', at);
        out.push_back(list.substr(at, end == std::string::npos ? end : end - at));
    }
    return out;
}
