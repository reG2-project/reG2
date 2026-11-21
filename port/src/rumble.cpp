#include "rumble.hpp"
#include "Car.hpp"
#include "Frontend.hpp"
#include "Game.hpp"
#include "Ped.hpp"
#include "Player.hpp"
#include "pause_menu.hpp"
#include "saves.hpp"
#include "settings.hpp"
#include "sprite.hpp"
#include "net_hooks.hpp"
#include <math.h>

#if defined(GTA2_PORT_SDL)
void port_Rumble(float low, float high, int ms);
#endif

namespace PortRumble
{
namespace
{
const f32 kExplosionRange = 5.0f; // blocks

f32 Clamp01(f32 v)
{
    return v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v);
}

// The player at the pad, NULL in a replay or while nothing should shake.
const Ped* LocalPed()
{
    Player* pPlayer = gGame_0x40 && !gFrontend ? gGame_0x40->mLocalPlayer : NULL;
    if (!pPlayer || !pPlayer->IsUser() || pPlayer->mShowQuitMessage || gGame_0x40->IsPaused() || PortPauseMenu::IsOpen() ||
        PortSaves::Replaying() || !PortSettings::Vibration() || gPortNoRumble)
    {
        return NULL;
    }
    return pPlayer->mPlayerPed;
}

void Play(f32 low, f32 high, s32 ms)
{
#if defined(GTA2_PORT_SDL)
    port_Rumble(low, high, ms);
#else
    (void)low;
    (void)high;
    (void)ms;
#endif
}
} // namespace

void Hit(const Ped* pPed, s32 damage)
{
    if (damage <= 0 || pPed != LocalPed() || pPed->mCar || pPed->get_health() <= 0)
    {
        return;
    }
    // Fire, falls and the electro gun hurt a little every step: a steady buzz rather than a pulse.
    const f32 strength = 0.225f + 0.015f * (damage < 20 ? damage : 20);
    Play(strength, strength * 0.8f, damage < 5 ? 120 : 200);
}

void Explosion(Fix16 x, Fix16 y)
{
    const Ped* pPed = LocalPed();
    const Sprite* pSprite = pPed && (pPed->mCar || pPed->mGameObject) ? pPed->GetSprite() : NULL;
    if (!pSprite)
    {
        return;
    }
    const f32 dx = (x - pSprite->mXy.x).ToFloat();
    const f32 dy = (y - pSprite->mXy.y).ToFloat();
    const f32 strength = 1.0f - sqrtf(dx * dx + dy * dy) / kExplosionRange;
    if (strength >= 0.1f)
    {
        Play(0.9f * strength, 0.63f * strength, static_cast<s32>(150 + 150 * strength));
    }
}

void Crash(const Car* pCar, Fix16 damage)
{
    const Ped* pPed = LocalPed();
    // Up to 0.3 the car only scrapes, the original dents it past that; full strength from 3 (600 damage).
    const f32 strength = Clamp01((damage.ToFloat() - 0.3f) / 2.7f);
    if (pPed && pCar->mDriver == pPed && strength > 0.0f)
    {
        // 0.6 as strong, up to 0.8 for the biggest crashes.
        const f32 scale = 0.8f * (0.75f + 0.25f * strength * strength * strength);
        Play((0.3f + 0.7f * strength) * scale, (0.2f + 0.6f * strength) * scale, static_cast<s32>(100 + 150 * strength));
    }
}
} // namespace PortRumble

#if defined(GTA2_PORT_SDL)
bool port_RumbleAllowed()
{
    return PortRumble::LocalPed() != NULL;
}
#endif
