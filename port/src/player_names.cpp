#include "player_names.hpp"
#include "Camera.hpp"
#include "Car.hpp"
#include "Draw.hpp"
#include "Frontend.hpp"
#include "Game.hpp"
#include "Ped.hpp"
#include "Player.hpp"
#include "char.hpp"
#include "player_looks.hpp"
#include "sprite.hpp"
#include "view.hpp"

namespace
{
// Blocks above the sprite's centre the name's foot is drawn at: clear of a ped's head, of most of a car's roof
const f32 kLiftOnFoot = 0.3f, kLiftInCar = 0.55f;
// Of the HUD's text size: a label in the world, not a line of the HUD
const f32 kScale = 0.75f;
// The text font's capitals end this far down its line (blank rows above them)
const f32 kCapsFoot = 17.0f;

f32 ToF(Fix16 v)
{
    return v.mValue / 16384.0f;
}

Fix16 FromF(f32 v)
{
    return Fix16(static_cast<s32>(v * 16384.0f), 0);
}
} // namespace

void PortDrawPlayerNames(u16 font)
{
    if (!gGame_0x40 || !gViewCamera)
    {
        return;
    }
    const f32 scale = ToF(gViewCamera->mUiScale) * kScale;
    const f32 screenW = static_cast<f32>(gViewCamera->mScreenPxWidth), screenH = static_cast<f32>(gViewCamera->mScreenPxHeight);
    for (Player* pPlayer = gGame_0x40->IterateFirstPlayer(); pPlayer; pPlayer = gGame_0x40->IterateNextPlayer())
    {
        Ped* pPed = pPlayer->mPlayerPed;
        // A wasted or busted player is named again at its respawn, not on its corpse or its body waiting there
        if (pPlayer->mIsUser || !pPed || pPed->mBf21C.bInvisible || pPlayer->mWastedOrBusted)
        {
            continue;
        }
        // Where the frame draws it (interpolated between steps): its car's sprite, else its own
        Sprite* pSprite = pPed->mCar ? pPed->mCar->mCarSprite : pPed->mGameObject ? pPed->mGameObject->mSpritePtr : NULL;
        if (!pSprite)
        {
            continue;
        }
        const f32 lift = pPed->mCar ? kLiftInCar : kLiftOnFoot;
        f32 x, foot;
        PortView::GroundToScreen(ToF(pSprite->mXy.x), ToF(pSprite->mXy.y) - lift, ToF(pSprite->mZpos), &x, &foot);
        const f32 width = Frontend::GetMaxTextWidth(pPlayer->mPlayerName, font) * scale;
        const f32 left = x - width / 2, top = foot - kCapsFoot * scale;
        if (left > screenW || left + width < 0 || top > screenH || foot < 0)
        {
            continue;
        }
        s32 paletteType;
        u16 palette;
        PortHudTextPalette(pPlayer->mHudPaletteType, pPlayer->mHudPalette, &paletteType, &palette);
        DrawString(pPlayer->mPlayerName, FromF(left), FromF(top), font, FromF(scale), paletteType, palette, 0, 0);
    }
}
