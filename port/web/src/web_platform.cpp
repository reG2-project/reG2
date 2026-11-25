// What the web build does in place of the desktop's: the game folder the page filled, and the yield to
// the browser once per frame.
#include <SDL3/SDL.h>
#include <emscripten.h>
#include <unistd.h>

extern "C" {
// web_library.js: resolves at the browser's next frame; the browser's frame period in ms; a frame drawn.
void port_WebWaitFrame();
double port_WebFramePeriodMs();
void port_WebCountPresent();
}

// Source/winmain.cpp: the game's video mode from the registry's screen settings, as a resolution change applies it.
void __stdcall SetVideoModeFromSettings();
void OnVideoModeChanged();
bool port_FrontendActive();
bool port_GameRunning();

namespace
{
bool sPresented;
bool sIdle;
Uint64 sLastYield;
Uint64 sResizedAt; // the canvas changed size; its new resolution applies once it settles
} // namespace

// The page mounts the game's data and the player's files at /reG2 before main.
bool port_PrepareGameFolder(bool, bool, const char*)
{
    // SDL's own yields (in SDL_Delay and the swap) would need Asyncify on every stack that reaches them.
    SDL_SetHint(SDL_HINT_EMSCRIPTEN_ASYNCIFY, "0");
    // The sound is mixed on the page's thread, between frames: SDL doubles this to 2048 frames, 43 ms at 48 kHz,
    // also the delay miles.cpp gives each sound to keep the game's rhythm. ?audioframes sets it (GTA2_AUDIO_FRAMES).
    const char* pFrames = SDL_getenv("GTA2_AUDIO_FRAMES");
    SDL_SetHint(SDL_HINT_AUDIO_DEVICE_SAMPLE_FRAMES, pFrames ? pFrames : "1024");
    return chdir("/reG2") == 0;
}

void port_SetWindowIcon(SDL_Window*)
{
}

extern "C" void port_WebMarkPresented()
{
    sPresented = true;
    port_WebCountPresent();
}

// The canvas has a new size (a window resize, full screen entered or left): the game takes it as its resolution.
void port_WebCanvasResized()
{
    sResizedAt = SDL_GetTicks();
}

extern "C" void port_WebMarkIdle()
{
    sIdle = true;
}

extern "C" unsigned long long port_WebRefreshNs()
{
    return static_cast<unsigned long long>(port_WebFramePeriodMs() * 1e6);
}

// Called at the top of GameWinMain's loop: after a frame was drawn, when the loop waits for the next step,
// or at the latest every 50 ms, the browser gets its turn (events, sound, presenting the canvas). The frontend
// polls the clock between its 30 Hz updates without waiting: after 2 ms of that, it yields too.
extern "C" void port_WebYield()
{
    const Uint64 now = SDL_GetTicks();
    if (sResizedAt && now - sResizedAt >= 200 && (port_GameRunning() || port_FrontendActive()))
    {
        sResizedAt = 0;
        SetVideoModeFromSettings();
        OnVideoModeChanged();
    }
    if (sPresented || sIdle || now - sLastYield >= 50 || (now - sLastYield >= 2 && port_FrontendActive()))
    {
        sPresented = false;
        sIdle = false;
        port_WebWaitFrame();
        sLastYield = SDL_GetTicks();
    }
}
