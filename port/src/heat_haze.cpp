#include "heat_haze.hpp"
#include "Char_Pool.hpp"
#include "Explosion.hpp"
#include "Game.hpp"
#include "Object_5C.hpp"
#include "ParticleManager.hpp"
#include "Ped.hpp"
#include "enums.hpp"
#include "explosion_type.hpp"
#include "frame_loop.hpp"
#include "settings.hpp"
#include "sprite.hpp"
#include "view.hpp"
#include <math.h>
#include <string.h>

#if defined(GTA2_PORT_SDL)
void port_HeatLevel(f32 level);
void port_Haze(f32 texel, f32 worldX, f32 worldY, f32 time, f32 amplitude, f32 ripple, f32 speed, f32 rise);
#endif

namespace PortHeatHaze
{
namespace
{
// The look, as DOOM (2016) renders hot areas into a distortion map: a smooth noise offset of up to kAmplitude ground
// texels (64 to a block), ripples kRipple texels across changing kSpeed times a second, over the flame sprites' own
// shapes, strongest at the flames' top and trailing kRise blocks above them.
const f32 kAmplitude = 3.5f;
const f32 kRipple = 12.0f;
const f32 kSpeed = 0.95f;
const f32 kRise = 0.6f;

// A fire's own sprite followed over the frames, so that its heat ramps in and dies away: the level it heads for
// (0..1) and has now, seconds to ramp in and out.
struct Heat
{
    const void* mpKey;
    const Sprite* mpSprite;
    s32 mId;
    f32 mZ;
    f32 mTarget, mLevel;
    f32 mRampIn, mRampOut;
    bool mSeen;
};
const s32 kMaxHeat = 48;
Heat sHeat[kMaxHeat];
s32 sNumHeat;

void AddHeat(const void* pKey, s32 id, const Sprite* pSprite, f32 strength, f32 rampIn, f32 rampOut)
{
    if (!pSprite || strength <= 0.0f)
    {
        return;
    }
    Heat* pHeat = NULL;
    for (s32 i = 0; i < sNumHeat && !pHeat; i++)
    {
        pHeat = sHeat[i].mpKey == pKey && sHeat[i].mId == id ? &sHeat[i] : NULL;
    }
    if (!pHeat)
    {
        if (sNumHeat == kMaxHeat)
        {
            return;
        }
        pHeat = &sHeat[sNumHeat++];
        pHeat->mpKey = pKey;
        pHeat->mId = id;
        pHeat->mLevel = 0.0f;
    }
    pHeat->mpSprite = pSprite;
    pHeat->mZ = pSprite->mZpos.ToFloat();
    pHeat->mTarget = strength > 1.0f ? 1.0f : strength;
    pHeat->mRampIn = rampIn;
    pHeat->mRampOut = rampOut;
    pHeat->mSeen = true;
}

// Fires and blasts. A blast heats the air only once its flash is over (its countdown from 100 below 45) and is gone
// by 10.
void ExplosionHeat(const Explosion& explosion)
{
    const Sprite* pSprite = explosion.mpObj2C ? explosion.mpObj2C->mSprite : NULL;
    switch (explosion.mType)
    {
        case explosion_type::fire_3:
            AddHeat(&explosion, explosion.mId, pSprite, 0.9f, 1.0f, 0.8f);
            break;
        case explosion_type::molotov_fire_5:
            AddHeat(&explosion, explosion.mId, pSprite, 0.8f, 0.6f, 0.6f);
            break;
        case explosion_type::car_fire_level1_12:
            AddHeat(&explosion, explosion.mId, pSprite, 0.5f, 1.0f, 0.8f);
            break;
        case explosion_type::car_fire_level2_13:
            AddHeat(&explosion, explosion.mId, pSprite, 0.75f, 1.0f, 0.8f);
            break;
        case explosion_type::car_fire_4:
        case explosion_type::car_fire_level3_14:
            AddHeat(&explosion, explosion.mId, pSprite, 1.0f, 1.0f, 0.8f);
            break;
        case explosion_type::small_18:
        case explosion_type::item_19:
        case explosion_type::large_20:
        case explosion_type::building_45_22:
        case explosion_type::building_225_23:
        case explosion_type::building_135_24:
        case explosion_type::building_315_25:
        case explosion_type::no_ring_32:
        case explosion_type::small_33:
        {
            const f32 t = static_cast<f32>(explosion.mTimer);
            const f32 heat = (t > 45.0f || t < 10.0f) ? 0.0f : fminf((45.0f - t) / 10.0f, 1.0f) * fminf((t - 10.0f) / 15.0f, 1.0f);
            AddHeat(&explosion, explosion.mId, pSprite, 0.8f * heat, 0.2f, 0.3f);
            break;
        }
        default:
            break;
    }
}

void CollectHeat()
{
    for (s32 i = 0; gExplosionPool_7A8 && i < 40; i++)
    {
        if (gExplosionPool_7A8->mbUsed[i])
        {
            ExplosionHeat(gExplosionPool_7A8->mExplosions[i]);
        }
    }
    for (s32 i = 0; gExplosionPool_3D4 && i < 20; i++)
    {
        if (gExplosionPool_3D4->mbUsed[i])
        {
            ExplosionHeat(gExplosionPool_3D4->mExplosions[i]);
        }
    }
    for (Ped* pPed = gPedPool ? gPedPool->mPool.GetFirstUsed() : NULL; pPed; pPed = pPed->mpNext)
    {
        if (pPed->IsOnFire() && pPed->mGameObject)
        {
            AddHeat(pPed, static_cast<s32>(pPed->mId), pPed->mGameObject->mSpritePtr, 0.6f, 0.5f, 0.5f);
        }
    }
    // Flamethrower flames (ParticleManager::EmitFlameStreamSegment, state 31): short lived, so their heat comes and
    // goes quickly.
    for (Particle_4C* p = gParticle_4C_Pool ? gParticle_4C_Pool->GetFirstUsed() : NULL; p; p = p->mpNext)
    {
        if (p->mState == 31)
        {
            AddHeat(p, p->mId, p->mpOwnSprite, 0.6f, 0.25f, 0.4f);
        }
    }
}

// Whether the last frame's haze is on, and the hot sprites it found: the particles fires and blasts emit
// (Explosion's emitters: states 3 to 6 and 36 flames, 18 to 20 fireballs), flamethrower flames (31) and the fires'
// own objects, by sprite.
bool sOn;
const s32 kHotSlots = 1024;
const Sprite* sHotSprite[kHotSlots];
f32 sHotLevel[kHotSlots];

u32 HotSlot(const Sprite* pSprite)
{
    return static_cast<u32>(reinterpret_cast<uintptr_t>(pSprite) >> 3) * 0x9E3779B1u >> 22;
}

void AddHot(const Sprite* pSprite, f32 level)
{
    for (u32 i = HotSlot(pSprite), n = 0; pSprite && n < 64; i = (i + 1) & (kHotSlots - 1), n++)
    {
        if (!sHotSprite[i] || sHotSprite[i] == pSprite)
        {
            sHotSprite[i] = pSprite;
            sHotLevel[i] = fmaxf(sHotLevel[i], level);
            return;
        }
    }
}

f32 HotLevel(const Sprite* pSprite)
{
    for (u32 i = HotSlot(pSprite), n = 0; sHotSprite[i] && n < 64; i = (i + 1) & (kHotSlots - 1), n++)
    {
        if (sHotSprite[i] == pSprite)
        {
            return sHotLevel[i];
        }
    }
    return 0.0f;
}

void CollectHotSprites()
{
    memset(sHotSprite, 0, sizeof(sHotSprite));
    memset(sHotLevel, 0, sizeof(sHotLevel));
    for (Particle_4C* p = gParticle_4C_Pool ? gParticle_4C_Pool->GetFirstUsed() : NULL; p; p = p->mpNext)
    {
        const s32 state = p->mState;
        if ((state >= 3 && state <= 6) || state == 36)
        {
            AddHot(p->mpOwnSprite, 1.0f);
        }
        else if ((state >= 18 && state <= 20) || state == 31)
        {
            AddHot(p->mpOwnSprite, 0.8f);
        }
    }
    for (s32 i = 0; i < sNumHeat; i++)
    {
        AddHot(sHeat[i].mpSprite, sHeat[i].mLevel);
    }
}
} // namespace

void Draw()
{
#if defined(GTA2_PORT_SDL)
    static f64 sLastSeconds;
    sOn = false;
    if (!PortSettings::FireEffects() || !gGame_0x40 || !gGame_0x40->mDrawPlayer)
    {
        sNumHeat = 0;
        return;
    }
    const f64 seconds = PortFrameLoop::WorldSeconds();
    const f32 dt = static_cast<f32>(seconds - sLastSeconds);
    sLastSeconds = seconds;
    for (s32 i = 0; i < sNumHeat; i++)
    {
        sHeat[i].mSeen = false;
    }
    CollectHeat();
    s32 kept = 0;
    f32 gridZ = 0.0f;
    f32 strongest = 0.0f;
    for (s32 i = 0; i < sNumHeat; i++)
    {
        Heat h = sHeat[i];
        // Heat that is gone fades from where it was last; a long pause or a jump starts nothing half way.
        const f32 target = h.mSeen ? h.mTarget : 0.0f;
        const f32 step = dt > 0.0f && dt < 0.25f ? dt : 0.0f;
        h.mLevel = h.mLevel < target ? fminf(target, h.mLevel + step / h.mRampIn) : fmaxf(target, h.mLevel - step / h.mRampOut);
        if (!h.mSeen && h.mLevel <= 0.0f)
        {
            continue;
        }
        sHeat[kept++] = h;
        if (h.mLevel > strongest)
        {
            strongest = h.mLevel;
            gridZ = h.mZ;
        }
    }
    sNumHeat = kept;
    sOn = true;
    CollectHotSprites();
    if (strongest <= 0.0f)
    {
        return;
    }
    // The noise follows the ground at the strongest fire's height, 64 texels to a block.
    f32 x0, y0, x1, y1, wx, wy;
    PortView::GroundToScreen(0.0f, 0.0f, gridZ, &x0, &y0);
    PortView::GroundToScreen(1.0f, 0.0f, gridZ, &x1, &y1);
    PortView::ScreenToGround(0.0f, 0.0f, gridZ, &wx, &wy);
    port_Haze((x1 - x0) / 64.0f, wx * 64.0f, wy * 64.0f, static_cast<f32>(fmod(seconds, 3600.0)), kAmplitude, kRipple, kSpeed, kRise);
#endif
}

void HeatSprite(const Sprite* pSprite)
{
#if defined(GTA2_PORT_SDL)
    static bool sSet;
    f32 level = 0.0f;
    if (pSprite && sOn)
    {
        const s32 type = pSprite->mSpriteTypeEnum;
        const bool object = type == sprite_types_enum::object_1 || type == sprite_types_enum::code_obj1_4 || type == sprite_types_enum::map_obj_5;
        if (object && pSprite->mObject2CPtr && pSprite->mObject2CPtr->mModel == objects::fire_197)
        {
            // A burning ped's flames (Char::DrawFlamesAndStartScreamTimer).
            level = 0.7f;
        }
        else
        {
            level = HotLevel(pSprite);
        }
    }
    if (level > 0.0f || sSet)
    {
        port_HeatLevel(level);
        sSet = level > 0.0f;
    }
#else
    (void)pSprite;
#endif
}
} // namespace PortHeatHaze
