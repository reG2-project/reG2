// Entry point: the log, the game folder as working directory and the setup when it has no game
// data, SDL start-up, then the game's WinMain.
#include "port_platform.hpp"
#include "../../setup/setup.hpp"
#include "reg2_version.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <string>
#include <vector>
#include "../../net/net_main.hpp"
#include "../../src/net_hooks.hpp" // gPortCrashHook, gPortServer
#if !defined(_WIN32) && !defined(__EMSCRIPTEN__)
    #include <execinfo.h>
    #include <pthread.h>
    #include <signal.h>
    #include <unistd.h>
#endif

void port_RegisterRenderer();

// The options read here, not the game's or the netcode's: how many arguments arg takes with it, -1 for none of these
static int StartArg(const char* arg)
{
    return !strcmp(arg, "--data") ? 1 : !strcmp(arg, "--setup") || !strcmp(arg, "--help") || !strcmp(arg, "-h") ? 0 : -1;
}

// The game folder, and the setup if it has no game data (--setup: the setup screens anyway, to
// replace the data or add the music). Before the game's global constructors, which read the
// registry and data files.
static void EnterGameFolder(int argc, char** argv)
{
    bool forceSetup = false;
    const char* dataDir = NULL;
    for (int i = 1; i < argc; i++)
    {
        if (!strcmp(argv[i], "--help") || !strcmp(argv[i], "-h"))
        {
            NetMain_PrintUsage();
            exit(0);
        }
        forceSetup |= !strcmp(argv[i], "--setup");
        gPortServer |= !strcmp(argv[i], "--server");
        if (!strcmp(argv[i], "--data"))
        {
            if (i + 1 >= argc)
            {
                fprintf(stderr, "reg2: --data <folder>: the game folder, or its data folder\n");
                exit(1);
            }
            dataDir = argv[++i];
        }
    }
    // Before the setup's windows and the game's: on Linux the id matches the packages' desktop file.
    SDL_SetAppMetadata("reG2", GTA2_VERSION, GTA2_APP_ID);
    port_StartLog(argc, argv);
#if defined(_WIN32)
    port_InstallCrashHandler();
#endif
    if (!port_PrepareGameFolder(forceSetup, gPortServer, dataDir))
    {
        port_LogExit(gPortServer);
        exit(gPortServer); // a player who left the setup quit: 0
    }
}

#if defined(_WIN32)
__attribute__((constructor(101))) static void EnterGameFolderAtStart()
{
    EnterGameFolder(__argc, __argv);
}
#elif defined(__EMSCRIPTEN__)
// Constructors take no arguments in WebAssembly; the page gives the game none.
__attribute__((constructor(101))) static void EnterGameFolderAtStart()
{
    EnterGameFolder(0, NULL);
}
#else
// glibc and macOS pass the program's arguments to constructors.
__attribute__((constructor(101))) static void EnterGameFolderAtStart(int argc, char** argv, char**)
{
    EnterGameFolder(argc, argv);
}
#endif

#if !defined(_WIN32) && !defined(__EMSCRIPTEN__)
static volatile sig_atomic_t sCrashSignal;

// The report isn't all signal-safe (a lock the crash left held, malloc after a heap corruption): if it hangs, the
// process still dies of its signal
static void OnCrashReportHung(int)
{
    static const char kLine[] = "reg2: the crash report hung, ending the process\n";
    write(2, kLine, sizeof(kLine) - 1);
    const int sig = sCrashSignal;
    signal(sig, SIG_DFL);
    sigset_t set;
    sigemptyset(&set);
    sigaddset(&set, sig);
    pthread_sigmask(SIG_UNBLOCK, &set, NULL);
    raise(sig);
    _exit(128 + sig);
}

// A crash prints the faulting address and its backtrace to stderr (macOS throttles its crash reports), and the network
// game's view of the game, then dies as it would have
static void OnCrash(int sig, siginfo_t* info, void*)
{
    sCrashSignal = sig;
    struct sigaction hung = {};
    hung.sa_handler = OnCrashReportHung;
    sigaction(SIGALRM, &hung, NULL);
    alarm(5);
    char line[160];
    const int n0 =
        snprintf(line, sizeof(line), "\nreg2: crashed (signal %d) at address %p; backtrace:\n", sig, info ? info->si_addr : NULL);
    write(2, line, n0 > 0 ? (size_t)n0 : 0);
    void* frames[64];
    backtrace_symbols_fd(frames, backtrace(frames, 64), 2);
    static bool sOnce;
    if (!sOnce && gPortCrashHook) // not signal-safe, but worth it once (the alarm ends a hang)
    {
        sOnce = true;
        gPortCrashHook();
    }
    signal(sig, SIG_DFL);
    raise(sig);
}
#endif

void port_InstallCrashReport()
{
#if !defined(_WIN32) && !defined(__EMSCRIPTEN__)
    void* frame;
    backtrace(&frame, 1); // loads what backtrace needs now, not in the handler
    struct sigaction sa = {};
    sa.sa_sigaction = OnCrash;
    sa.sa_flags = SA_SIGINFO;
    for (int sig : {SIGSEGV, SIGBUS, SIGABRT, SIGTRAP, SIGILL, SIGFPE})
    {
        sigaction(sig, &sa, NULL);
    }
#endif
}

int main(int argc, char** argv)
{
    // Wine drops SDL_* variables before a Windows program sees them.
    if (const char* driver = getenv("GTA2_AUDIO_DRIVER"))
    {
        SDL_SetHint(SDL_HINT_AUDIO_DRIVER, driver);
    }
    if (const char* file = getenv("GTA2_AUDIO_FILE"))
    {
        SDL_SetHint(SDL_HINT_AUDIO_DISK_OUTPUT_FILE, file);
    }
    std::vector<std::string> args;
    for (int i = 1; i < argc; i++)
    {
        const int taken = StartArg(argv[i]);
        if (taken < 0)
        {
            args.push_back(argv[i]);
        }
        i += taken > 0 ? taken : 0;
    }
    const int net = NetMain_Init(args); // --server, --connect (port/docs/netcode.md)
    if (net < 0)
    {
        port_LogExit(1);
        return 1;
    }
    if (net > 0)
    {
        args.push_back("-N"); // the game's switch for a network game
    }
    std::string cmdLine;
    for (const std::string& arg : args)
    {
        cmdLine += (cmdLine.empty() ? "" : " ");
        cmdLine += arg;
    }

    if (port_Headless())
    {
        SDL_SetHint(SDL_HINT_VIDEO_DRIVER, "dummy");
        SDL_SetHint(SDL_HINT_AUDIO_DRIVER, "dummy");
    }
    // Headless: no gamepads, as their every event poll asks the system for new ones
    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS | (port_Headless() ? 0 : SDL_INIT_GAMEPAD)))
    {
        SDL_Log("SDL_Init: %s", SDL_GetError());
        port_LogExit(1);
        return 1;
    }
    const int sdl = SDL_GetVersion();
    SDL_Log("SDL %d.%d.%d, video driver %s", SDL_VERSIONNUM_MAJOR(sdl), SDL_VERSIONNUM_MINOR(sdl), SDL_VERSIONNUM_MICRO(sdl),
            SDL_GetCurrentVideoDriver());
    port_RegisterRenderer();
    port_InitTestInput();

    const int result = WinMain(reinterpret_cast<HINSTANCE>(1), NULL, &cmdLine[0], SW_SHOWNORMAL);
#if defined(__EMSCRIPTEN__)
    // The page takes over (stores the files, shows itself); the runtime stays, as exit() from a resumed stack traps.
    port_WebQuit(result);
#endif
    port_LogExit(result);
    SDL_Quit();
    return result;
}
