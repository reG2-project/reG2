// What follows from a network game player's look (the remap a map script's PLAYER_PED gives their ped): the colour of
// their HUD digits (Player::ColorScoreFromRemap) and of the arrow pointing at them (Hud_Arrow::SetPlayerArrowColour).
// The original's looks keep the original's values; the others give more players colours of their own.
#pragma once
#include "../../Source/types.hpp"
#include "../../Source/enums.hpp"

struct PortPlayerLook
{
    s16 mHudPalette; // the user remap of the score and frag digits; 0: the digits keep their colour
    s16 mArrowSprite; // the user sprite of the arrow (1-7); 0: the arrow keeps its sprite
    s16 mArrowPaletteType; // palette_types_enum the arrow is drawn with; 0: its sprite's own colours
    s16 mArrowRemap; // in that palette type
};

// NULL: a look the HUD gives nothing
inline const PortPlayerLook* PortLookOf(s32 remap)
{
    // By look (ped_remap_enum). The HUD digits' user remaps the original leaves: 1, 9 and 11 (0 and 14 hide the digits,
    // 10, 12 and 13 repeat others). The arrows' new colours: cyan and rust user remaps, a purple car remap
    const s16 kUser = palette_types_enum::user_remaps_7, kCar = palette_types_enum::car_remaps_3;
    static const PortPlayerLook kLooks[] = {
        {1, 2, kUser, 1}, // 0 blue police: steel blue, rust
        {0, 0, 0, 0},
        {0, 0, 0, 0},
        {0, 0, 0, 0},
        {0, 0, 0, 0},
        {4, 4, 0, 0}, // 5 red-head redneck
        {4, 4, 0, 0}, // 6 blond redneck, as the red-head
        {8, 5, 0, 0}, // 7 yellow scientist
        {7, 3, 0, 0}, // 8 Zaibatsu
        {5, 6, 0, 0}, // 9 Krishna
        {6, 7, 0, 0}, // 10 Russian
        {3, 1, 0, 0}, // 11 Loonie
        {9, 6, kUser, 9}, // 12 Elvis: cyan, cyan
        {2, 2, 0, 0}, // 13 Yakuza
        {11, 1, kCar, 3}, // 14 fireman: pink, purple
    };
    const PortPlayerLook* look = remap >= 0 && remap < (s32)(sizeof(kLooks) / sizeof(kLooks[0])) ? &kLooks[remap] : NULL;
    return look && (look->mHudPalette || look->mArrowSprite) ? look : NULL;
}

// The font remap that draws text in a HUD colour (Player::mHudPaletteType and mHudPalette), -1: none (white). The fonts
// have 9 (0-8): the original looks' are one below their user remap, the port's pink is 8 and its cyan has none
inline s32 PortFontRemap(s32 paletteType, s32 userRemap)
{
    if (paletteType != palette_types_enum::user_remaps_7)
    {
        return -1;
    }
    return userRemap >= 1 && userRemap <= 8 ? userRemap - 1 : (userRemap == 11 ? 8 : -1);
}

// The palette type and remap the HUD draws a player's text in (its name, its tag time): the original's, a font remap one
// below its user remap, but for the port's looks' colours (PortFontRemap)
inline void PortHudTextPalette(s32 hudPaletteType, s32 hudPalette, s32* pType, u16* pRemap)
{
    const s32 remap = PortFontRemap(hudPaletteType, hudPalette);
    const bool user = hudPaletteType == palette_types_enum::user_remaps_7;
    *pType = user && remap >= 0 ? palette_types_enum::font_remaps_8 : palette_types_enum::sprites_2;
    *pRemap = static_cast<u16>(user ? (remap >= 0 ? remap : 0) : hudPalette - 1);
}

// Hud_Arrow::mArrowSprtIdx for a look's arrow: the sprite in bits 0-3, the remap in 4-9, the palette type in 10-13. The
// game's own values (a sprite, below 16) read as themselves
inline s16 PortArrowIndex(const PortPlayerLook& look)
{
    return (s16)(look.mArrowSprite | look.mArrowRemap << 4 | look.mArrowPaletteType << 10);
}

inline void PortArrowDecode(s16 index, s32* pSprite, s32* pPaletteType, s32* pRemap)
{
    const u16 i = (u16)index;
    *pSprite = i & 0xF;
    *pRemap = i >> 4 & 0x3F;
    *pPaletteType = i >> 10 & 0xF;
}
