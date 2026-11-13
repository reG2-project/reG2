// Entry point: the log, the game folder as working directory and the setup when it has no game
// data, SDL start-up, then the game's WinMain.
#include "port_platform.hpp"
#include "../../setup/setup.hpp"
#include "reg2_version.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <string>

void port_RegisterRenderer();

static bool IsSetupArg(const char* arg)
{
    return strcmp(arg, "--setup") == 0;
}

// The game folder, and the setup if it has no game data (--setup: the setup screens anyway, to
// replace the data or add the music). Before the game's global constructors, which read the
// registry and data files.
static void EnterGameFolder(int argc, char** argv)
{
    // Before the setup's windows and the game's: on Linux the id matches the packages' desktop file.
    SDL_SetAppMetadata("reG2", GTA2_VERSION, GTA2_APP_ID);
    port_StartLog(argc, argv);
    bool forceSetup = false;
    for (int i = 1; i < argc; i++)
    {
        forceSetup |= IsSetupArg(argv[i]);
    }
    if (!port_PrepareGameFolder(forceSetup))
    {
        port_LogExit(0);
        exit(0);
    }
}

#if defined(_WIN32)
__attribute__((constructor(101))) static void EnterGameFolderAtStart()
{
    EnterGameFolder(__argc, __argv);
}
#else
// glibc and macOS pass the program's arguments to constructors.
__attribute__((constructor(101))) static void EnterGameFolderAtStart(int argc, char** argv, char**)
{
    EnterGameFolder(argc, argv);
}
#endif

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
    std::string cmdLine;
    for (int i = 1; i < argc; i++)
    {
        if (!IsSetupArg(argv[i]))
        {
            cmdLine += (cmdLine.empty() ? "" : " ");
            cmdLine += argv[i];
        }
    }

    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMEPAD | SDL_INIT_EVENTS))
    {
        SDL_Log("SDL_Init: %s", SDL_GetError());
        port_LogExit(1);
        return 1;
    }
    port_RegisterRenderer();
    port_InitTestInput();

    const int result = WinMain(reinterpret_cast<HINSTANCE>(1), NULL, &cmdLine[0], SW_SHOWNORMAL);
    port_LogExit(result);
    SDL_Quit();
    return result;
}
