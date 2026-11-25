// port/net (port/docs/netcode.md, "Code layout"): the session's lifetime: a server, a client, a replay, or a join from the
// frontend's network page, started from the command line (cmdline.cpp) and ended when the game ends.
#include "net_main.hpp"
#include "cmdline.hpp"
#include "maps.hpp"
#include "port/src/build_id.hpp"
#include "port/src/net_hooks.hpp"
#include "engine/dev/dev.hpp"
#include "session/client.hpp"
#include "session/server.hpp"
#include "port/src/rand/state.hpp" // the sound's and particles' own streams
#include <SDL3/SDL.h>
#include <atomic>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#if !defined(_WIN32)
    #include <execinfo.h>
    #include <pthread.h>
    #include <sys/stat.h>
    #include <unistd.h>
#endif

void port_RegistryReadOnly(); // compat/src/registry.cpp: gta2_registry.ini read, never written

namespace
{
net::Server* gServer;
net::Client* gClient;
int gTickMs = 33;

void StopIfRequested();

// The main loop's progress for the watchdog: the hooks run, and the last phase entered
std::atomic<uint32_t> gHookRuns;
std::atomic<const char*> gPhase{"start"};
std::atomic<bool> gWatching; // a network game's: the frontend waits as long as it likes

void Phase(const char* phase)
{
    gHookRuns.fetch_add(1, std::memory_order_relaxed);
    gPhase.store(phase, std::memory_order_relaxed);
}

// A line to stderr and, when it is another file, to stdout: signal-safe, for the ends that print nothing else
void Note(const char* line)
{
    const size_t n = strlen(line);
#if defined(_WIN32)
    fputs(line, stderr);
    fflush(stderr);
    (void)n;
#else
    struct stat out, err;
    const bool same = fstat(1, &out) == 0 && fstat(2, &err) == 0 && out.st_dev == err.st_dev && out.st_ino == err.st_ino;
    ssize_t r = write(2, line, n);
    if (!same)
    {
        r = write(1, line, n);
    }
    (void)r;
#endif
}

#if !defined(_WIN32)
pthread_t gMainThread;

void OnBacktraceSignal(int)
{
    void* frames[64];
    backtrace_symbols_fd(frames, backtrace(frames, 64), 2);
}

void OnPipeSignal(int)
{
    static volatile sig_atomic_t sOnce;
    if (!sOnce)
    {
        sOnce = 1;
        Note("reg2: SIGPIPE (the log's reader gone?), going on without it\n");
    }
}
#endif

void NoteExit()
{
    char line[96];
    snprintf(line, sizeof(line), "reg2: exit() after %u hook runs, phase %s\n", gHookRuns.load(), gPhase.load());
    Note(line);
}

// A main loop that stops for 10 s says so, with its backtrace, and again when it goes on
int SDLCALL Watchdog(void*)
{
    uint32_t last = gHookRuns.load();
    int still = 0;
    for (;;)
    {
        SDL_Delay(1000);
        const uint32_t now = gHookRuns.load();
        char line[128];
        if (!gWatching.load())
        {
            last = now;
            still = 0;
            continue;
        }
        if (now != last)
        {
            if (still >= 10)
            {
                snprintf(line, sizeof(line), "reg2: main loop going again after %d s\n", still);
                Note(line);
            }
            last = now;
            still = 0;
            continue;
        }
        if (++still % 60 != 10)
        {
            continue;
        }
        snprintf(line, sizeof(line), "reg2: main loop stalled %d s after %u hook runs, phase %s; its backtrace:\n", still, now,
                 gPhase.load());
        Note(line);
#if !defined(_WIN32)
        pthread_kill(gMainThread, SIGUSR2);
#endif
    }
    return 0;
}

// Every end of a network game's process says how it ended, and a stalled main loop where it is
void WatchExits()
{
    gWatching = true;
    static bool sOnce;
    if (sOnce)
    {
        return;
    }
    sOnce = true;
    atexit(NoteExit);
#if !defined(_WIN32)
    gMainThread = pthread_self();
    void* frame;
    backtrace(&frame, 1); // loads what backtrace needs now, not in the handler
    signal(SIGUSR2, OnBacktraceSignal);
    signal(SIGPIPE, OnPipeSignal);
#endif
    SDL_DetachThread(SDL_CreateThread(Watchdog, "reg2 watchdog", NULL));
}

void OnFrame(void*)
{
    Phase("net frame");
    StopIfRequested();
    if (net::engine::GameRunning())
    {
        net::engine::SetTickMs(gTickMs); // every frame: a new round's or map's game starts at the default
    }
    if (gServer)
    {
        gServer->Frame();
    }
    if (gClient)
    {
        gClient->Frame();
    }
    Phase("game frame");
}

// A client that quits, or is stopped, leaves the server at once: else its slot stays taken until the server's timeout
// (5-10 s), and a player joining meanwhile finds the server full. A server that ends tells its clients
void LeaveServer()
{
    if (gClient)
    {
        gClient->Leave();
    }
    if (gServer)
    {
        gServer->Shutdown();
    }
}

volatile sig_atomic_t gStopRequested;

// A stop signal's work, from the frame or idle hook: the handler can't leave the server itself, as ENet allocates and
// frees, and the signal may come in the middle of a malloc (it would wait on that lock for ever)
void StopIfRequested()
{
    if (gStopRequested)
    {
        char line[96];
        snprintf(line, sizeof(line), "reg2: signal %d, leaving the game after %u hook runs\n", (int)gStopRequested, gHookRuns.load());
        Note(line);
        LeaveServer();
        _Exit(0);
    }
}

void OnStopSignal(int sig)
{
    char line[128];
    if (gStopRequested)
    {
        snprintf(line, sizeof(line), "reg2: signal %d again, ending at once\n", sig);
        Note(line);
        _Exit(0);
    }
    gStopRequested = sig;
    snprintf(line, sizeof(line), "reg2: signal %d, phase %s\n", sig, gPhase.load());
    Note(line);
    // Neither hook may run soon (a map loading, the frontend): out without leaving after a second
#if defined(_WIN32)
    SDL_Delay(1000); // the handler has a thread of its own
    Note("reg2: no hook ran within 1 s of the signal, ending without leaving\n");
    _Exit(0);
#else
    signal(SIGALRM, [](int) {
        char line[128];
        snprintf(line, sizeof(line), "reg2: no hook ran within 1 s of the signal (phase %s), ending without leaving\n", gPhase.load());
        Note(line);
        _Exit(0);
    });
    alarm(1);
#endif
}

// Before any network game's start: what each end draws its random numbers from, and the log
void NetworkSetup(bool server)
{
    // A crash's report, with the netcode's view of the game last: network games only (the report isn't signal-safe)
    port_InstallCrashReport();
    gPortCrashHook = net::engine::DebugOnCrash;
    WatchExits();
    PortRand::SetSoundSeparate(true); // sound draws its own random numbers: a resimulation without sound draws the same
    PortRand::SetParticleSeparate(true); // and particles: a resimulation makes the flames it made before
    net::engine::SetRandOwners(!server); // and each entity and server service its own: the same wherever it runs
    if (!server)
    {
        gPortDrawOverlay = net::engine::DrawOverlay; // the ping, an outage, and (GTA2_NET_MARKER) where the newest snapshot has us
        gPortDrawNetStandings = net::engine::DrawStandings;
    }
#if defined(_WIN32)
    setvbuf(stdout, NULL, _IONBF, 0); // the CRT has no line buffering (_IOLBF is full buffering, and a size of 0 is refused)
#else
    setvbuf(stdout, NULL, _IOLBF, 0); // the net: lines as they happen, also when the process is killed
#endif
}

// A client the server welcomed: its game is the one the game starts next
void StartClientGame(const net::engine::NetworkGame& game)
{
    atexit(LeaveServer);
    gPortIdleHook = [] { Phase("idle"); StopIfRequested(); gClient->Idle(); net::engine::IdleWait(); }; // pongs timed as they come
    signal(SIGTERM, OnStopSignal);
    signal(SIGINT, OnStopSignal);
#if !defined(_WIN32)
    signal(SIGHUP, OnStopSignal);
#endif
    net::engine::StartNetworkGame(game);
    gTickMs = game.tickMs;
    net::engine::SetFrameHook(OnFrame, NULL);
    gPortNetFrameLoop = true; // frames at the display's rate, interpolated between the ticks
}

net::ClientConfig gMenuConfig; // the command line's client options (--name, --lag...), for a join from the frontend
net::engine::NetworkGame gMenuGame;
} // namespace

// winmain.cpp, a network game over: a client that left goes back to the network page, the netcode as before the join
int LeaveNetworkGame()
{
    if (!gClient || (!net::engine::LeftForMenu() && net::engine::GameClosed()))
    {
        return -1; // a server's or a replay's, or the window closed: the game's own end
    }
    if (!net::engine::LeftForMenu())
    {
        gClient->Leave(); // its game ended another way: off the server too, and back to the network page
    }
    NetMenu_SetStatus(gClient ? gClient->LeftWhy() : "");
    delete gClient; // it left the server already
    gClient = NULL;
    gPortIdleHook = NULL;
    gPortDrawOverlay = NULL;
    gPortDrawNetStandings = NULL;
    net::engine::SetStandings(std::vector<net::engine::PlayerRow>(), 0);
    gPortNetFrameLoop = false;
    gWatching = false;
    net::engine::SetPaceMs(-1);
    PortRand::SetSoundSeparate(false);
    PortRand::SetParticleSeparate(false);
    net::engine::StopNetworkGame();
    printf("net: back to the frontend\n");
    return NetMenu_Page();
}

bool NetMenu_Join(const std::string& address, const std::string& name, std::string* error)
{
    NetMenu_Cancel();
    net::ClientConfig cc = gMenuConfig;
    net::ParseAddress(address, &cc.host, &cc.port);
    cc.name = name;
    gClient = new net::Client();
    if (!gClient->BeginConnect(cc, error))
    {
        NetMenu_Cancel();
        return false;
    }
    printf("net: joining %s:%u as %s\n", cc.host.c_str(), cc.port, name.c_str());
    return true;
}

int NetMenu_Poll(std::string* error)
{
    if (!gClient)
    {
        return -1;
    }
    const int result = gClient->PollConnect(&gMenuGame, error);
    if (result < 0)
    {
        printf("net: %s\n", error->c_str());
        NetMenu_Cancel();
    }
    else if (result > 0)
    {
        NetworkSetup(false);
        StartClientGame(gMenuGame);
    }
    return result;
}

void NetMenu_Cancel()
{
    delete gClient; // its transport's destructor disconnects
    gClient = NULL;
}

const std::string& NetMenu_DefaultName()
{
    return gMenuConfig.name;
}

void NetMain_PrintUsage()
{
    net::PrintUsage();
}

int NetMain_Init(std::vector<std::string>& args)
{
    gPortLeaveNetworkGame = LeaveNetworkGame;
    const std::vector<std::string> commandLine = args;
    net::NetOptions o;
    if (!net::ParseOptions(args, o))
    {
        return -1;
    }
    // A server's recording replays as that server: its command line again, its switches unless set here, no network
    const bool serverReplay = !o.replay.empty() && net::ServerReplay::IsServerRecording(o.replay.c_str());
    if (serverReplay)
    {
        net::ServerStart start;
        net::ServerReplay header;
        std::string error;
        if (!header.Open(o.replay.c_str(), &start, &error))
        {
            fprintf(stderr, "net: %s\n", error.c_str());
            return -1;
        }
        for (const std::string& v : start.env)
        {
            const size_t eq = v.find('=');
            const std::string name = v.substr(0, eq), value = eq == std::string::npos ? "" : v.substr(eq + 1);
            if (!getenv(name.c_str()))
            {
#if defined(_WIN32)
                _putenv_s(name.c_str(), value.c_str());
#else
                setenv(name.c_str(), value.c_str(), 0);
#endif
            }
        }
        const std::string path = o.replay;
        o = net::NetOptions();
        if (!net::ParseOptions(start.args, o))
        {
            return -1;
        }
        o.sc.replay = path;
        o.window = false;
        printf("net: replaying the server of %s\n", path.c_str());
    }
    net::ServerConfig& sc = o.sc;
    net::ClientConfig& cc = o.cc;
    sc.build = cc.build = port_BuildId();
    if (!o.server && o.connect.empty() && o.replay.empty())
    {
        gMenuConfig = cc; // the frontend's network page may join a server later (port/net/menu.cpp)
        NetMenu_Install();
        return 0;
    }
    NetworkSetup(o.server);
    gPortTrafficCap = o.traffic;
    gPortNoAudio = o.server && !getenv("GTA2_NET_SFXLOG"); // nobody hears a server (port/src/net_hooks.hpp) but its sample log
    gPortNoRumble = o.server;
    std::string error;
    if (o.server)
    {
        // A client in the same folder saves the settings: a server's whole-file rewrites would undo its changes
        port_RegistryReadOnly();
        if (!o.window)
        {
            SDL_SetEnvironmentVariable(SDL_GetEnvironment(), "GTA2_HEADLESS", "1", true); // what port_Headless reads
            gPortNoDraw = true;
        }
        if (!net::SetUpMaps(o.map, o.maps, sc))
        {
            return -1;
        }
        // The round flow (session/round_flow.cpp): on unless the server plays its own bots (an arena: tests, training)
        sc.rules.enabled = o.rounds.empty() ? sc.arena.empty() : o.rounds == "on";
        sc.rules.gameType = sc.game.gameType;
        sc.rules.fragLimit = sc.game.fragLimit;
        sc.rules.timeLimitMin = sc.game.timeLimitMin;
        sc.rules.ticksPerSecond = 1000 / (sc.game.tickMs > 0 ? sc.game.tickMs : 33);
        sc.rules.rotation = sc.maps.size() > 1;
        sc.game.localPlayer = 0; // nobody plays on the server: every slot's input comes from the network
        sc.args = commandLine;
        gServer = new net::Server();
        if (!gServer->Start(sc, &error))
        {
            fprintf(stderr, "net: %s\n", error.c_str());
            return -1;
        }
        net::engine::StartNetworkGame(sc.game);
        gTickMs = sc.game.tickMs;
        if (serverReplay)
        {
            net::engine::SetPaceMs(0); // as fast as it runs, its ticks of the live server's length
            net::engine::SetFrameHook(OnFrame, NULL);
            return 1;
        }
        gPortIdleHook = [] { Phase("idle"); StopIfRequested(); gServer->Idle(); net::engine::IdleWait(); }; // pings answered at once
        atexit(LeaveServer);
        signal(SIGTERM, OnStopSignal);
        signal(SIGINT, OnStopSignal);
#if !defined(_WIN32)
        signal(SIGHUP, OnStopSignal);
#endif
    }
    else if (!o.replay.empty())
    {
        gClient = new net::Client();
        net::engine::NetworkGame game;
        if (!gClient->StartReplay(o.replay.c_str(), &game, &error))
        {
            fprintf(stderr, "net: %s\n", error.c_str());
            return -1;
        }
        net::engine::StartNetworkGame(game);
        gTickMs = game.tickMs;
        net::engine::SetPaceMs(0); // as fast as it runs
    }
    else
    {
        net::ParseAddress(o.connect, &cc.host, &cc.port);
        gClient = new net::Client();
        net::engine::NetworkGame game;
        if (!gClient->Connect(cc, &game, &error))
        {
            fprintf(stderr, "net: %s\n", error.c_str());
            return -1;
        }
        StartClientGame(game);
        gMenuConfig = cc;
        NetMenu_Install(); // left, it goes to the frontend: the network page there
        return 1;
    }
    net::engine::SetFrameHook(OnFrame, NULL);
    return 1;
}
