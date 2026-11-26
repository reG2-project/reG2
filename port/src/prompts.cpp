#include "prompts.hpp"
#include "png.hpp"
#include <math.h>
#include <string.h>
#include <vector>
#if defined(GTA2_PORT_SDL)
#include <SDL3/SDL.h>

void port_DrawPicture(s32 slot, s32 width, s32 height, const u32* pRgba, const f32* pXyuv, const u32* pColours, s32 count, bool nearest);
float port_PixelScale();
SDL_Gamepad* port_ActivePad();
#endif

namespace PortPrompts
{
namespace
{
#if defined(GTA2_PORT_SDL)
// One row of 16 pixel cells, by port/resources/prompts_atlas.py: each pad's A, B, X and Y (or their symbols),
// shoulders, Back, Start and triggers, the sticks, the d-pad's left, then Enter (two cells), Escape, Delete, Left, Right,
// Backspace (two cells) and the letters.
const unsigned char kColourPng[] = {
#embed "../resources/prompts_colour.png"
};

enum Cell
{
    kXboxCells = 0,
    kPlayStationCells = 10,
    kStickCells = 20,
    kDpadLeftCell = 22,
    kEnterCell = 23,
    kBackspaceCell = 28,
    kLetterCells = 29,
};
// After the welcome screen's.
const s32 kSlot = 3;

std::vector<u32> sPixels;
s32 sWidth;
bool sLoaded;

bool Load()
{
    if (!sLoaded)
    {
        sLoaded = true;
        s32 height = 0;
        if (!PortPng::Decode(kColourPng, sizeof(kColourPng), &sPixels, &sWidth, &height) || height != 16)
        {
            sPixels.clear();
        }
    }
    return !sPixels.empty();
}

bool Wide(s32 cell)
{
    return cell == kEnterCell || cell == kBackspaceCell;
}

s32 CellX(s32 cell)
{
    return cell * 16 + (cell > kEnterCell ? 16 : 0) + (cell > kBackspaceCell ? 16 : 0);
}

s32 PadCells(SDL_Gamepad* pPad)
{
    switch (SDL_GetGamepadType(pPad))
    {
        case SDL_GAMEPAD_TYPE_PS3:
        case SDL_GAMEPAD_TYPE_PS4:
        case SDL_GAMEPAD_TYPE_PS5:
            return kPlayStationCells;
        // Nintendo pads too: their face buttons' letters come from the labels below.
        default:
            return kXboxCells;
    }
}

s32 CellOf(Glyph glyph)
{
    if (glyph >= kKeyA)
    {
        return kLetterCells + glyph - kKeyA;
    }
    if (glyph >= kKeyEnter)
    {
        return kEnterCell + glyph - kKeyEnter;
    }
    SDL_Gamepad* pPad = port_ActivePad();
    const s32 cells = PadCells(pPad);
    if (glyph == kPadDpadLeft)
    {
        return kDpadLeftCell;
    }
    if (glyph == kPadLeftStick || glyph == kPadRightStick)
    {
        return kStickCells + glyph - kPadLeftStick;
    }
    if (glyph > kPadNorth)
    {
        return cells + (glyph > kPadStart ? glyph - 2 : glyph);
    }
    // The label printed on the button there: a Nintendo pad has B at the bottom and A on the right.
    static const SDL_GamepadButton kFaces[4] = {SDL_GAMEPAD_BUTTON_SOUTH, SDL_GAMEPAD_BUTTON_EAST, SDL_GAMEPAD_BUTTON_WEST,
                                                SDL_GAMEPAD_BUTTON_NORTH};
    switch (SDL_GetGamepadButtonLabel(pPad, kFaces[glyph]))
    {
        case SDL_GAMEPAD_BUTTON_LABEL_A:
        case SDL_GAMEPAD_BUTTON_LABEL_CROSS:
            return cells;
        case SDL_GAMEPAD_BUTTON_LABEL_B:
        case SDL_GAMEPAD_BUTTON_LABEL_CIRCLE:
            return cells + 1;
        case SDL_GAMEPAD_BUTTON_LABEL_X:
        case SDL_GAMEPAD_BUTTON_LABEL_SQUARE:
            return cells + 2;
        case SDL_GAMEPAD_BUTTON_LABEL_Y:
        case SDL_GAMEPAD_BUTTON_LABEL_TRIANGLE:
            return cells + 3;
        default:
            return cells + glyph;
    }
}
#endif
} // namespace

bool Available()
{
#if defined(GTA2_PORT_SDL)
    return Load();
#else
    return false;
#endif
}

Glyph Letter(s32 letter)
{
    return letter >= 'A' && letter <= 'Z' ? static_cast<Glyph>(kKeyA + letter - 'A') : kNoGlyph;
}

f32 Size(f32 height)
{
#if defined(GTA2_PORT_SDL)
    const f32 pixels = port_PixelScale();
    const f32 times = floorf(height * pixels / 16 + 0.5f);
    return 16 * (times < 1 ? 1 : times) / pixels;
#else
    return height;
#endif
}

f32 Width(Glyph glyph, f32 size)
{
#if defined(GTA2_PORT_SDL)
    return glyph == kNoGlyph ? 0 : (Wide(CellOf(glyph)) ? size * 2 : size);
#else
    (void)glyph;
    (void)size;
    return 0;
#endif
}

void Draw(Glyph glyph, f32 x, f32 y, f32 size, s32 colour, bool dim)
{
#if defined(GTA2_PORT_SDL)
    if (glyph == kNoGlyph || !Load())
    {
        return;
    }
    const s32 cell = CellOf(glyph);
    const s32 texels = Wide(cell) ? 32 : 16;
    // On whole pixels of the window, so each texel covers the same number of them.
    const f32 pixels = port_PixelScale();
    const f32 left = floorf(x * pixels + 0.5f) / pixels;
    const f32 top = floorf((y - size / 2) * pixels + 0.5f) / pixels;
    const f32 right = left + size * texels / 16;
    const f32 bottom = top + size;
    const f32 u0 = static_cast<f32>(CellX(cell)) / sWidth;
    const f32 u1 = static_cast<f32>(CellX(cell) + texels) / sWidth;
    const f32 xyuv[24] = {left, top, u0, 0, right, top, u1, 0, right, bottom, u1, 1,
                          left, top, u0, 0, right, bottom, u1, 1, left, bottom, u0, 1};
    (void)colour;
    const u32 rgb = dim ? 0x707070u : 0xFFFFFFu;
    const u32 rgba = ((rgb >> 16) & 0xFF) | (rgb & 0xFF00) | ((rgb & 0xFF) << 16) | 0xFF000000u;
    const u32 colours[6] = {rgba, rgba, rgba, rgba, rgba, rgba};
    port_DrawPicture(kSlot, sWidth, 16, sPixels.data(), xyuv, colours, 6, true);
#else
    (void)glyph;
    (void)x;
    (void)y;
    (void)size;
    (void)colour;
    (void)dim;
#endif
}
} // namespace PortPrompts
