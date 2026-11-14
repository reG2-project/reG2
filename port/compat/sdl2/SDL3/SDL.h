#pragma once

#ifdef GTA2_PORT_SDL2

#include <SDL2/SDL.h>
#include <math.h>
#include <stdint.h>
#include <string>

#if defined(_WIN32)
#define SDL_PLATFORM_WINDOWS 1
#elif defined(__APPLE__)
#define SDL_PLATFORM_MACOS 1
#else
#define SDL_PLATFORM_LINUX 1
#endif

typedef SDL_GameController SDL_Gamepad;

#define SDL_WindowID Uint32
#define SDL_SharedObject void
#define SDL_AtomicInt SDL_atomic_t
#define SDL_Time Uint64
#define SDL_GamepadButton SDL_GameControllerButton
#define SDL_GamepadAxis SDL_GameControllerAxis

#define SDL_GAMEPAD_BUTTON_SOUTH SDL_CONTROLLER_BUTTON_A
#define SDL_GAMEPAD_BUTTON_EAST SDL_CONTROLLER_BUTTON_B
#define SDL_GAMEPAD_BUTTON_WEST SDL_CONTROLLER_BUTTON_X
#define SDL_GAMEPAD_BUTTON_NORTH SDL_CONTROLLER_BUTTON_Y
#define SDL_GAMEPAD_BUTTON_BACK SDL_CONTROLLER_BUTTON_BACK
#define SDL_GAMEPAD_BUTTON_GUIDE SDL_CONTROLLER_BUTTON_GUIDE
#define SDL_GAMEPAD_BUTTON_START SDL_CONTROLLER_BUTTON_START
#define SDL_GAMEPAD_BUTTON_LEFT_STICK SDL_CONTROLLER_BUTTON_LEFTSTICK
#define SDL_GAMEPAD_BUTTON_RIGHT_STICK SDL_CONTROLLER_BUTTON_RIGHTSTICK
#define SDL_GAMEPAD_BUTTON_LEFT_SHOULDER SDL_CONTROLLER_BUTTON_LEFTSHOULDER
#define SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER SDL_CONTROLLER_BUTTON_RIGHTSHOULDER
#define SDL_GAMEPAD_BUTTON_DPAD_UP SDL_CONTROLLER_BUTTON_DPAD_UP
#define SDL_GAMEPAD_BUTTON_DPAD_DOWN SDL_CONTROLLER_BUTTON_DPAD_DOWN
#define SDL_GAMEPAD_BUTTON_DPAD_LEFT SDL_CONTROLLER_BUTTON_DPAD_LEFT
#define SDL_GAMEPAD_BUTTON_DPAD_RIGHT SDL_CONTROLLER_BUTTON_DPAD_RIGHT
#define SDL_GAMEPAD_BUTTON_COUNT SDL_CONTROLLER_BUTTON_MAX

#define SDL_GAMEPAD_AXIS_LEFTX SDL_CONTROLLER_AXIS_LEFTX
#define SDL_GAMEPAD_AXIS_RIGHTX SDL_CONTROLLER_AXIS_RIGHTX
#define SDL_GAMEPAD_AXIS_LEFTY SDL_CONTROLLER_AXIS_LEFTY
#define SDL_GAMEPAD_AXIS_RIGHTY SDL_CONTROLLER_AXIS_RIGHTY
#define SDL_GAMEPAD_AXIS_LEFT_TRIGGER SDL_CONTROLLER_AXIS_TRIGGERLEFT
#define SDL_GAMEPAD_AXIS_RIGHT_TRIGGER SDL_CONTROLLER_AXIS_TRIGGERRIGHT
#define SDL_GAMEPAD_AXIS_COUNT SDL_CONTROLLER_AXIS_MAX

#define SDL_JOYSTICK_TYPE_GAMEPAD SDL_JOYSTICK_TYPE_GAMECONTROLLER

#define SDL_EVENT_QUIT SDL_QUIT
#define SDL_EVENT_WINDOW_CLOSE_REQUESTED SDL_QUIT
#define SDL_EVENT_WINDOW_FOCUS_LOST SDL_WINDOWEVENT_FOCUS_LOST
#define SDL_EVENT_WINDOW_FOCUS_GAINED SDL_WINDOWEVENT_FOCUS_GAINED
#define SDL_EVENT_KEY_DOWN SDL_KEYDOWN
#define SDL_EVENT_KEY_UP SDL_KEYUP
#define SDL_EVENT_MOUSE_MOTION SDL_MOUSEMOTION
#define SDL_EVENT_MOUSE_BUTTON_DOWN SDL_MOUSEBUTTONDOWN
#define SDL_EVENT_MOUSE_BUTTON_UP SDL_MOUSEBUTTONUP
#define SDL_EVENT_MOUSE_WHEEL SDL_MOUSEWHEEL
#define SDL_EVENT_GAMEPAD_ADDED SDL_CONTROLLERDEVICEADDED
#define SDL_EVENT_GAMEPAD_REMOVED SDL_CONTROLLERDEVICEREMOVED
#define SDL_EVENT_GAMEPAD_BUTTON_DOWN SDL_CONTROLLERBUTTONDOWN
#define SDL_EVENT_GAMEPAD_AXIS_MOTION SDL_CONTROLLERAXISMOTION

#define SDL_KMOD_NONE KMOD_NONE
#define SDL_KMOD_LALT KMOD_LALT
#define SDL_KMOD_ALT KMOD_ALT
#define SDL_KMOD_CTRL KMOD_CTRL
#define SDL_KMOD_SHIFT KMOD_SHIFT
#define SDL_SCANCODE_COUNT SDL_NUM_SCANCODES

#define SDL_INIT_GAMEPAD SDL_INIT_GAMECONTROLLER
#define SDL_HINT_AUDIO_DRIVER SDL_HINT_AUDIODRIVER
#define SDL_AUDIO_F32 AUDIO_F32SYS
#define SDL_Swap16LE SDL_SwapLE16
#define SDL_Init(flags) (SDL_Init(flags) == 0)
#define SDL_InitSubSystem(flags) (SDL_InitSubSystem(flags) == 0)
#define SDL_ShowMessageBox(data, id) (SDL_ShowMessageBox(data, id) == 0)
#define SDL_ShowSimpleMessageBox(...) (SDL_ShowSimpleMessageBox(__VA_ARGS__) == 0)
#define SDL_GL_MakeCurrent(window, context) (SDL_GL_MakeCurrent(window, context) == 0)
#define SDL_GL_LoadLibrary(path) (SDL_GL_LoadLibrary(path) == 0)
#define SDL_GL_SetSwapInterval(interval) (SDL_GL_SetSwapInterval(interval) == 0)
#define SDL_NS_PER_SECOND 1000000000ull
#define SDL_IOFromConstMem SDL_RWFromConstMem
#define SDL_DestroySurface SDL_FreeSurface
#define SDL_GL_DestroyContext SDL_GL_DeleteContext
#define SDL_GetLogOutputFunction SDL_LogGetOutputFunction
#define SDL_SetLogOutputFunction SDL_LogSetOutputFunction
#define SDL_GetAtomicInt SDL_AtomicGet
#define SDL_SetAtomicInt SDL_AtomicSet
#define SDL_setenv_unsafe SDL_setenv
#define SDL_SetRenderScale SDL_RenderSetScale
#define SDL_SetRenderVSync SDL_RenderSetVSync
#define SDL_SetJoystickVirtualAxis SDL_JoystickSetVirtualAxis
#define SDL_SetJoystickVirtualButton SDL_JoystickSetVirtualButton
#define SDL_OpenJoystick SDL_JoystickOpen
#define SDL_GetGamepadButton SDL_GameControllerGetButton
#define SDL_GetGamepadAxis SDL_GameControllerGetAxis
#define SDL_CloseGamepad SDL_GameControllerClose
#define SDL_GetGamepadFromID SDL_GameControllerFromInstanceID
#define SDL_GetGamepadAxisFromString SDL_GameControllerGetAxisFromString
#define SDL_SetWindowFullscreenMode SDL_SetWindowDisplayMode
#define SDL_GetDisplayForWindow SDL_GetWindowDisplayIndex
#define SDL_INIT_INTERFACE(p) SDL_zero(*(p))
#define SDL_GetPrimaryDisplay() 0
#define SDL_GetPreferredLocales(...) SDL_GetPreferredLocales_I(__VA_ARGS__)
#define SDL_GetVersion() SDL2_VersionNumber()
#define SDL_VERSIONNUM_MAJOR(v) ((v) / 1000)
#define SDL_VERSIONNUM_MINOR(v) (((v) / 100) % 10)
#define SDL_VERSIONNUM_MICRO(v) ((v) % 10)

inline SDL_Window* SDL_CreateWindow(const char* pTitle, int w, int h, Uint32 flags)
{
    return SDL_CreateWindow(pTitle, SDL_WINDOWPOS_UNDEFINED, SDL_WINDOWPOS_UNDEFINED, w, h, flags);
}

inline int SDL_CreateWindowAndRenderer(const char* pTitle, int w, int h, Uint32 flags, SDL_Window** ppWindow,
                                       SDL_Renderer** ppRenderer)
{
    const int result = SDL_CreateWindowAndRenderer(w, h, flags, ppWindow, ppRenderer);
    if (result == 0 && pTitle)
    {
        SDL_SetWindowTitle(*ppWindow, pTitle);
    }
    return result;
}

inline int SDL_SetWindowFullscreen(SDL_Window* pWindow, bool fullscreen)
{
    return SDL_SetWindowFullscreen(pWindow, (Uint32)(fullscreen ? SDL_WINDOW_FULLSCREEN_DESKTOP : 0));
}

inline int SDL_SetWindowMouseGrab(SDL_Window* pWindow, bool grabbed)
{
    SDL_SetWindowGrab(pWindow, grabbed ? SDL_TRUE : SDL_FALSE);
    return 0;
}

inline const SDL_DisplayMode* SDL_GetDesktopDisplayMode(int displayID)
{
    static SDL_DisplayMode mode;
    return SDL_GetDesktopDisplayMode(displayID, &mode) == 0 ? &mode : NULL;
}

inline const SDL_DisplayMode* SDL_GetCurrentDisplayMode(int displayID)
{
    static SDL_DisplayMode mode;
    return SDL_GetCurrentDisplayMode(displayID, &mode) == 0 ? &mode : NULL;
}

inline int SDL_ShowCursor(bool shown)
{
    return SDL_ShowCursor(shown ? SDL_ENABLE : SDL_DISABLE);
}

inline bool SDL_ShowCursor()
{
    return SDL_ShowCursor(SDL_ENABLE) != 0;
}

inline bool SDL_HideCursor()
{
    return SDL_ShowCursor(SDL_DISABLE) != 0;
}

inline void SDL_SetWindowBordered(SDL_Window* pWindow, bool bordered)
{
    SDL_SetWindowBordered(pWindow, bordered ? SDL_TRUE : SDL_FALSE);
}

inline SDL_Surface* SDL_CreateSurfaceFrom(int w, int h, Uint32 format, void* pPixels, int pitch)
{
    return SDL_CreateRGBSurfaceWithFormatFrom(pPixels, w, h, SDL_BITSPERPIXEL(format), pitch, format);
}

inline int SDL_GetMouseState(float* pX, float* pY)
{
    int x = 0;
    int y = 0;
    const Uint32 buttons = SDL_GetMouseState(&x, &y);
    *pX = (float)x;
    *pY = (float)y;
    return (int)buttons;
}

inline SDL_Keycode SDL_GetKeyFromScancode(SDL_Scancode sc, SDL_Keymod, bool)
{
    return SDL_GetKeyFromScancode(sc);
}

inline bool SDL_GL_GetSwapInterval(int* pInterval)
{
    const int interval = SDL_GL_GetSwapInterval();
    *pInterval = interval < 0 ? 1 : interval;
    return true;
}

inline Uint64 SDL_GetTicksNS()
{
    const Uint64 freq = (Uint64)SDL_GetPerformanceFrequency();
    const Uint64 counter = (Uint64)SDL_GetPerformanceCounter();
    return counter / freq * 1000000000ull + counter % freq * 1000000000ull / freq;
}

inline void SDL_DelayPrecise(Uint64 ns)
{
    SDL_Delay((Uint32)((ns + 999999) / 1000000));
}

inline void SDL_SetAppMetadata(const char* pTitle, const char*, const char*)
{
    SDL_SetHint(SDL_HINT_APP_NAME, pTitle);
}

inline int SDL_RenderFillRect(SDL_Renderer* pRenderer, const SDL_FRect* pRect)
{
    const int x = (int)floorf(pRect->x);
    const int y = (int)floorf(pRect->y);
    const SDL_Rect r = {x, y, (int)ceilf(pRect->x + pRect->w) - x, (int)ceilf(pRect->y + pRect->h) - y};
    return SDL_RenderFillRect(pRenderer, &r);
}

inline int SDL_RenderRect(SDL_Renderer* pRenderer, const SDL_FRect* pRect)
{
    const int x = (int)floorf(pRect->x);
    const int y = (int)floorf(pRect->y);
    const SDL_Rect r = {x, y, (int)ceilf(pRect->x + pRect->w) - x, (int)ceilf(pRect->y + pRect->h) - y};
    return SDL_RenderDrawRect(pRenderer, &r);
}

SDL_DisplayMode** SDL_GetFullscreenDisplayModes(int displayID, int* pCount);
SDL_JoystickID* SDL_GetGamepads(int* pCount);
const char* SDL_GetGamepadNameForID(SDL_JoystickID id);
SDL_Gamepad* SDL_OpenGamepad(SDL_JoystickID id);
SDL_JoystickID SDL_GetGamepadID(SDL_Gamepad* pPad);
SDL_GamepadButton SDL_GetGamepadButtonFromString(const char* pName);
SDL_JoystickID SDL_AttachVirtualJoystick(const SDL_VirtualJoystickDesc* pDesc);
float SDL_GetDisplayContentScale(int displayID);
int SDL_GetCurrentTime(SDL_Time* pNow);
int SDL2_VersionNumber();
SDL_Locale* SDL_GetPreferredLocales_I(...);
bool SDL2_PickPath(bool folder, const char* pFilter, std::string* pOut);
char* SDL2_ProcessCapture(const char* const* args);

#endif
