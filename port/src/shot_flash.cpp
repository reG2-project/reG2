#include "shot_flash.hpp"
#include "ObjectDefinitions.hpp"
#include "char.hpp"
#include "sprite.hpp"
#include "twin_stick.hpp"
#include <math.h>

namespace PortShotFlash
{
namespace
{
const s32 kMaxFlashes = 32;
const u32 kTicks = 3; // the particle's sub states 1 to 3, a tick each (Particle_4C::UpdateSkidOrScrapeSpark_state_40_41)

struct Flash
{
    const Sprite* mpShooter;
    s16 mShooterId;
    Ang16 mRotation;
    u32 mTick;
};
Flash sFlashes[kMaxFlashes];
s32 sNumFlashes;
u32 sTick;

s32 LayerOf(Fix16 z)
{
    const s32 layer = static_cast<s32>(floorf(z.ToFloat()));
    return layer < 1 ? 1 : (layer > 7 ? 7 : layer);
}

// The particle's sprite at sub state <sub>, placed as its update places it on a ped
void Draw(const Flash& f, s32 sub)
{
    static Sprite sSprite;
    static SpriteShape sShape;
    Sprite* pShooter = const_cast<Sprite*>(f.mpShooter);
    Char* pChar = pShooter->AsCharB4();
    if (!pChar)
    {
        return;
    }
    Fix16_Point offset(Fix16(-0x333, 0), pChar->mFlagsBf.bReverseVelocity ? Fix16(0x1999, 0) : Fix16(0x1333, 0));
    offset.RotateByAngle(pShooter->mRotation);
    offset = offset.Add(PortTwinStick::ShotLead(pChar));

    sSprite.mpVisualShape = &sShape;
    sSprite.mSpriteTypeEnum = sprite_types_enum::code_obj2_8;
    sSprite.mPaletteType = palette_types_enum::sprites_2;
    sSprite.mTypeSpriteIdx = gObjectDefinitions->mDef112BaseSpriteIdx + sub + 197;
    sSprite.mFlags = 0xA2;
    sSprite.SetNoLighting();
    sSprite.mRotation = f.mRotation;
    sSprite.mXy.x = pShooter->mXy.x + offset.x;
    sSprite.mXy.y = pShooter->mXy.y + offset.y;
    sSprite.mZpos = pShooter->mZpos;
    // Its first frame has the size Sprite::EnsureVisualShape gives a particle, the next ones the sprite's own
    sSprite.UpdateDimensionsFromSpriteIndex();
    if (sub == 1)
    {
        Fix16 w, h, d;
        sShape.GetDimensions(&w, &h, &d);
        sShape.SetWidthHeight(w / 2, h / 2);
    }
    sShape.mBboxUpToDate = false;
    sSprite.Draw();
}
} // namespace

void Add(const Sprite* pShooter, u32 tick)
{
    if (sNumFlashes == kMaxFlashes)
    {
        sFlashes[0] = sFlashes[--sNumFlashes];
    }
    Flash f = {pShooter, pShooter->mInstanceId, pShooter->mRotation, tick};
    sFlashes[sNumFlashes++] = f;
}

void Clear()
{
    sNumFlashes = 0;
}

void SetTick(u32 tick)
{
    sTick = tick;
    for (s32 i = 0; i < sNumFlashes;)
    {
        const Flash& f = sFlashes[i];
        if (sTick - f.mTick >= kTicks || f.mpShooter->mInstanceId != f.mShooterId)
        {
            sFlashes[i] = sFlashes[--sNumFlashes];
            continue;
        }
        i++;
    }
}

void DrawLayer(s32 layer)
{
    for (s32 i = 0; i < sNumFlashes; i++)
    {
        const Flash& f = sFlashes[i];
        if (LayerOf(f.mpShooter->mZpos) == layer)
        {
            Draw(f, 1 + static_cast<s32>(sTick - f.mTick));
        }
    }
}
} // namespace PortShotFlash
