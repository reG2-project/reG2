// What the minimap (minimap.cpp), the pause menu's MAP (city_map.cpp) and the data both show (minimap_data.cpp) share:
// the level's map picture and pins, the art, and the drawing helpers.
#pragma once

#if defined(GTA2_PORT_SDL)
#include "types.hpp"
#include "fix16.hpp"
#include <vector>

class Gang;
class Sprite;
class Hud_ArrowTargets;

void port_DrawMinimap(s32 slot, s32 width, s32 height, const u32* pRgba, u32 version, const f32* pXyuv, const u32* pColours, s32 count,
                      bool nearest);
void port_ClipMinimap(const f32* pRect);

namespace PortMinimap
{
// Pictures, each with a texture of its own per scale in port_DrawMinimap (TextureSlot).
enum Slot
{
    kMapSlot,
    kStyledSlot, // the MAP's picture, gang territories tinted
    kFrameSlot,  // frame E of port/docs/minimap, slim: the tubes round the map
    kGlassSlot,  // and its glass over the map
    kArrowSlot,
    kBlipSlot,
    kGreyBlipSlot, // the blip without its red, tinted
    kSaveSlot,
    kGarageSlot,
    kRestartSlot, // a hospital, where the player comes back after dying
    kArrestSlot,  // a police station, where the player comes back after an arrest
    kStationSlot,
    kCraneSlot,
    kPhoneGreenSlot, // the job phones by their colour, and the gangs' information phones
    kPhoneYellowSlot,
    kPhoneRedSlot,
    kPhoneInfoSlot,
    kWaypointArrowSlot, // the game's job arrow in the waypoint's blue
    kNumSlots,
};

struct Picture
{
    std::vector<u32> mPixels; // bytes r, g, b, a
    s32 mWidth;
    s32 mHeight;
};
// By slot and scale: 0 for the 1x pictures, 1 for 2x.
extern Picture sPictures[kNumSlots][2];
// The pictures from their PNGs (once): false if one does not decode.
bool Load();

// The map: 256x256 blocks at 4 texels each, a texel the average colour of 16x16 texels of the block's lid.
const s32 kMapBlocks = 256;
const s32 kTexelsPerBlock = 4;
const s32 kMapTexels = kMapBlocks * kTexelsPerBlock;
extern std::vector<u32> sMap;
extern u32 sMapVersion;
extern std::vector<u32> sStyled;
extern u32 sStyledVersion;
// Water, or no lid at all (the sea round the city), per block.
extern bool sWater[kMapBlocks][kMapBlocks];

struct Pin
{
    f32 mX, mY;
    Slot mSlot;
    bool mMapOnly; // the MAP only, not the minimap (hospitals and police stations, which the game takes the player to)
};
extern std::vector<Pin> sPins;

// A gang's territory: a gang zone (Gang_14) of its name.
struct Turf
{
    Gang* mGang;
    f32 mRect[4]; // left, top, right, bottom in blocks
};
extern std::vector<Turf> sTurfs;

// A district: the navigation zones (Navigation_1) of one name, its label at their area-weighted middle.
struct District
{
    wchar_t mName[48];
    f32 mX, mY;
    f32 mArea; // blocks
};
extern std::vector<District> sDistricts;

// The waypoint set on the MAP, cleared as the player reaches it and with the level.
extern bool sWaypoint;
extern f32 sWaypointX, sWaypointY;
// A waypoint at (x, y), or none if one was within near blocks of it.
void ToggleWaypoint(f32 x, f32 y, f32 near);

// The player's sprite (its car's in one) once the art, the map's picture and the pins are ready; NULL without a game.
// Clears the waypoint the player has reached.
const Sprite* Prepare();
// The MAP's picture, from the map and the territories.
void BuildStyled();
// A gang's colour from the player's style (its respect meter's pip); false if it has none.
bool GangColour(s32 arrowColour, f32* pRgb);
// Where the level is played, in blocks (left, top, right, bottom): its zones' bounds, else its land's; the MAP opens on it.
void LandBounds(f32* pRect);

u32 Rgba(f32 r, f32 g, f32 b, f32 a);
inline f32 ToF(Fix16 v)
{
    return v.ToFloat();
}

// Where a map is on screen and what it shows.
struct View
{
    f32 mUi;                // the HUD's scale
    s32 mScale;             // 0 for the 1x pictures, 1 for 2x
    f32 mHole[4];           // the map: left, top, right, bottom
    f32 mCentreX, mCentreY; // its middle on screen
    f32 mPlayerX, mPlayerY; // there in the world (blocks)
    f32 mPixelsPerBlock;
    f32 mAlpha;
};

void DrawRect(f32 left, f32 top, f32 right, f32 bottom, u32 rgba);
// A picture centred at (x, y), at its size on the HUD times size.
void DrawIcon(const View& v, Slot slot, f32 x, f32 y, u32 rgba, f32 size = 1.0f);
// A picture turned by angle (radians, as Fix16_Point::RotateByAngle turns), centred at (x, y), k screen pixels a texel.
void DrawTurned(Slot slot, s32 scale, f32 x, f32 y, f32 k, f32 angle, u32 rgba);
void DrawTriangle(f32 x, f32 y, f32 size, bool up, u32 rgba);
// A world position on screen, held inside the map less inset: false when it is further than maxOut times the map's
// half size. pOut: how many half sizes away it is (1 at the edge).
bool ToScreen(const View& v, f32 wx, f32 wy, f32 inset, f32 maxOut, f32* pX, f32* pY, f32* pOut);
// Full inside the map, fading to half from 1 to 5 times its size.
f32 EdgeAlpha(f32 out);
void DrawMap(const View& v, bool styled);
// A gang's icon from the respect meters, size HUD pixels wide.
void DrawGangIcon(const View& v, s32 arrowColour, f32 x, f32 y, f32 size);
// A phone's badge: the arrow's colour 1 yellow, 2 red, 3 green, else a gang's information phone.
void DrawPhone(const View& v, s32 arrowColour, f32 x, f32 y, f32 alpha);
// The HUD's arrows: a job's targets and, off a job, the phones; city: the MAP's, every phone wherever it is.
void DrawTargets(const View& v, f32 playerZ, bool city);
// A job phone the player has too little respect with its gang for yet (Hud_Arrow::CheckVisibility).
bool PhoneAvailable(const Hud_ArrowTargets& t);
const f32 kLockedAlpha = 0.4f;
u32 TargetColour(f32 alpha);
const f32 kTargetSize = 1.3f;
void DrawPlayer(const View& v, const Sprite* pSprite, f32 x, f32 y);
void DrawWaypointMark(f32 x, f32 y, f32 ui, f32 alpha);
// The MAP's view and key, with the level (Reset).
void ResetCityMap();
} // namespace PortMinimap
#endif
