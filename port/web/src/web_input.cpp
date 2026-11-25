// What the page asks of the game between frames: its state, the touch controls' gamepad, the pause.
#include "Frontend.hpp"
#include "Game.hpp"
#include "Ped.hpp"
#include "Player.hpp"
#include "sprite.hpp"
#include "Car.hpp"
#include "enums.hpp"
#include "../../src/pause_menu.hpp"
#include <SDL3/SDL.h>
#include <dinput.h>
#include <emscripten.h>

bool port_GameRunning();
bool port_FrontendActive();
int port_TwinStick();
bool port_ControlHeld(int control);
bool port_MouseLeftDown();
void port_MouseWindowPos(float* pX, float* pY);

namespace
{
SDL_Joystick* sTouchPad;
} // namespace

extern "C" {
// Bits: 1 the frontend, 2 a game, 4 the pause menu, 8 the player in a vehicle, 16 TWIN STICK on foot.
EMSCRIPTEN_KEEPALIVE int port_WebState()
{
    int state = port_FrontendActive() ? 1 : 0;
    if (port_GameRunning())
    {
        state |= 2;
        state |= PortPauseMenu::IsOpen() ? 4 : 0;
        const Player* pPlayer = gGame_0x40->mLocalPlayer;
        state |= pPlayer && pPlayer->mPlayerPed && pPlayer->mPlayerPed->mCar ? 8 : 0;
        state |= port_TwinStick() == 1 ? 16 : 0;
    }
    return state;
}

// Opens the pause menu in a game, as Escape does.
EMSCRIPTEN_KEEPALIVE void port_WebPause()
{
    if (port_GameRunning() && !PortPauseMenu::IsOpen() && gGame_0x40->mLocalPlayer)
    {
        PortPauseMenu::HandleKey(DIK_ESCAPE, gGame_0x40->mLocalPlayer);
    }
}

// For tests: the player's sprite in world blocks (0 x, 1 y), 2 whether he drives, 3 ATTACK held by the pad or the
// mouse, 4 the mouse's left button down.
EMSCRIPTEN_KEEPALIVE double port_WebPlayer(int what)
{
    if (what == 3)
    {
        return port_ControlHeld(4);
    }
    if (what == 4)
    {
        return port_MouseLeftDown();
    }
    if (what >= 10 && what < 10 + SDL_GAMEPAD_AXIS_COUNT)
    {
        return sTouchPad ? SDL_GetJoystickAxis(sTouchPad, what - 10) : -99999;
    }
    const Player* pPlayer = port_GameRunning() && gGame_0x40 ? gGame_0x40->mLocalPlayer : NULL;
    Ped* pPed = pPlayer ? pPlayer->mPlayerPed : NULL;
    if (!pPed)
    {
        return 0;
    }
    if (what == 2)
    {
        return pPed->mCar ? 1 : 0;
    }
    const Sprite* pSprite = pPed->mCar ? pPed->mCar->mCarSprite : pPed->GetSprite();
    return pSprite ? (what ? pSprite->mXy.y.mValue : pSprite->mXy.x.mValue) / 16384.0 : 0;
}

// For tests: a parked car just west of the player on foot, for the touch controls' ENTER.
EMSCRIPTEN_KEEPALIVE int port_WebTestCar()
{
    const Player* pPlayer = port_GameRunning() && gGame_0x40 ? gGame_0x40->mLocalPlayer : NULL;
    Ped* pPed = pPlayer ? pPlayer->mPlayerPed : NULL;
    const Sprite* pSprite = pPed && !pPed->mCar ? pPed->GetSprite() : NULL;
    if (!pSprite || !gCarManager_6C)
    {
        return 0;
    }
    Fix16 x = pSprite->mXy.x;
    x.mValue -= 16384 * 3 / 2;
    return gCarManager_6C->SpawnCarAtCorrectZ(x, pSprite->mXy.y, kAngZero_6F804C, car_model_enum::BUICK) ? 1 : 0;
}

// For tests: the frontend's page (0), its selected option (1) and its state (2); -1 outside the frontend.
EMSCRIPTEN_KEEPALIVE int port_WebMenu(int what)
{
    if (!gFrontend)
    {
        return -1;
    }
    const int page = gFrontend->mCurrentPageIdx;
    return what == 0 ? page : what == 1 ? gFrontend->mMenuPagesArray[page].mCurrentOptionIdx : static_cast<int>(gFrontend->mState);
}

// The mouse in the canvas' CSS pixels, where the game draws its cursor: with the pointer locked, the page's own
// buttons are pointed at with it.
EMSCRIPTEN_KEEPALIVE float port_WebCursorX()
{
    float x, y;
    port_MouseWindowPos(&x, &y);
    return x;
}

EMSCRIPTEN_KEEPALIVE float port_WebCursorY()
{
    float x, y;
    port_MouseWindowPos(&x, &y);
    return y;
}

// The touch controls as a gamepad: SDL_GamepadButton bits, the sticks and triggers from -32768 to 32767
// (triggers 0 to 32767), in SDL_GamepadAxis order.
EMSCRIPTEN_KEEPALIVE void port_WebPad(int buttons, int lx, int ly, int rx, int ry, int lt, int rt)
{
    if (!sTouchPad)
    {
        SDL_VirtualJoystickDesc desc;
        SDL_INIT_INTERFACE(&desc);
        desc.type = SDL_JOYSTICK_TYPE_GAMEPAD;
        desc.naxes = SDL_GAMEPAD_AXIS_COUNT;
        desc.nbuttons = SDL_GAMEPAD_BUTTON_COUNT;
        desc.name = "Touch controls";
        const SDL_JoystickID id = SDL_AttachVirtualJoystick(&desc);
        sTouchPad = id ? SDL_OpenJoystick(id) : NULL;
        if (!sTouchPad)
        {
            return;
        }
    }
    for (int i = 0; i < SDL_GAMEPAD_BUTTON_COUNT; i++)
    {
        SDL_SetJoystickVirtualButton(sTouchPad, i, (buttons >> i) & 1);
    }
    // SDL reads a virtual trigger's whole range, -32768 to 32767, as 0 to 32767: at rest it is -32768, as 0 would
    // be a trigger held halfway (RT attacks with TWIN STICK, LT and RT are specials or the pedals).
    const int axes[SDL_GAMEPAD_AXIS_COUNT] = {lx, ly, rx, ry, lt * 2 - 32768, rt * 2 - 32768};
    for (int i = 0; i < SDL_GAMEPAD_AXIS_COUNT; i++)
    {
        SDL_SetJoystickVirtualAxis(sTouchPad, i, (Sint16)SDL_clamp(axes[i], -32768, 32767));
    }
}
}
