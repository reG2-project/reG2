// What the minimap and the MAP show, read from the level: the map's picture from its blocks and lid tiles, the pins,
// the gangs' territories and colours, the districts' names, and the waypoint.
#include "minimap.hpp"
#include "minimap_internal.hpp"
#if defined(GTA2_PORT_SDL)
#include "Cranes.hpp"
#include "Game.hpp"
#include "Gang.hpp"
#include "Hud.hpp"
#include "ObjectPool.hpp"
#include "Ped.hpp"
#include "Player.hpp"
#include "ScriptManager.hpp"
#include "ScriptThread.hpp"
#include "TextTable.hpp"
#include "block_face.hpp"
#include "enums.hpp"
#include "gtx_0x106C.hpp"
#include "map_0x370.hpp"
#include "png.hpp"
#include "sprite.hpp"
#include <math.h>
#include <string.h>
#include <algorithm>
#include <array>
#endif

namespace PortMinimap
{
#if defined(GTA2_PORT_SDL)
namespace
{
// The markers, pins and frame: original art from port/docs/minimap/assets (tools/make_all.py, tools/fullmap.py), at 1x
// and 2x.
const unsigned char kFrame1x[] = {
#embed "../resources/minimap/frame_1x.png"
};
const unsigned char kFrame2x[] = {
#embed "../resources/minimap/frame_2x.png"
};
const unsigned char kGlass1x[] = {
#embed "../resources/minimap/glass_1x.png"
};
const unsigned char kGlass2x[] = {
#embed "../resources/minimap/glass_2x.png"
};
const unsigned char kArrow1x[] = {
#embed "../resources/minimap/player_arrow_1x.png"
};
const unsigned char kArrow2x[] = {
#embed "../resources/minimap/player_arrow_2x.png"
};
const unsigned char kBlip1x[] = {
#embed "../resources/minimap/blip_1x.png"
};
const unsigned char kBlip2x[] = {
#embed "../resources/minimap/blip_2x.png"
};
const unsigned char kSave1x[] = {
#embed "../resources/minimap/save_1x.png"
};
const unsigned char kSave2x[] = {
#embed "../resources/minimap/save_2x.png"
};
const unsigned char kGarage1x[] = {
#embed "../resources/minimap/garage_1x.png"
};
const unsigned char kGarage2x[] = {
#embed "../resources/minimap/garage_2x.png"
};
const unsigned char kRestart1x[] = {
#embed "../resources/minimap/restart_1x.png"
};
const unsigned char kRestart2x[] = {
#embed "../resources/minimap/restart_2x.png"
};
const unsigned char kArrest1x[] = {
#embed "../resources/minimap/arrest_1x.png"
};
const unsigned char kArrest2x[] = {
#embed "../resources/minimap/arrest_2x.png"
};
const unsigned char kStation1x[] = {
#embed "../resources/minimap/station_1x.png"
};
const unsigned char kStation2x[] = {
#embed "../resources/minimap/station_2x.png"
};
const unsigned char kCrane1x[] = {
#embed "../resources/minimap/crane_1x.png"
};
const unsigned char kCrane2x[] = {
#embed "../resources/minimap/crane_2x.png"
};
const unsigned char kPhoneGreen1x[] = {
#embed "../resources/minimap/phone_green_1x.png"
};
const unsigned char kPhoneGreen2x[] = {
#embed "../resources/minimap/phone_green_2x.png"
};
const unsigned char kPhoneYellow1x[] = {
#embed "../resources/minimap/phone_yellow_1x.png"
};
const unsigned char kPhoneYellow2x[] = {
#embed "../resources/minimap/phone_yellow_2x.png"
};
const unsigned char kPhoneRed1x[] = {
#embed "../resources/minimap/phone_red_1x.png"
};
const unsigned char kPhoneRed2x[] = {
#embed "../resources/minimap/phone_red_2x.png"
};
const unsigned char kPhoneInfo1x[] = {
#embed "../resources/minimap/phone_info_1x.png"
};
const unsigned char kPhoneInfo2x[] = {
#embed "../resources/minimap/phone_info_2x.png"
};

struct Art
{
    Slot mSlot;
    const unsigned char* mPng[2];
    size_t mSize[2];
};
const Art kArt[] = {
    {kFrameSlot, {kFrame1x, kFrame2x}, {sizeof(kFrame1x), sizeof(kFrame2x)}},
    {kGlassSlot, {kGlass1x, kGlass2x}, {sizeof(kGlass1x), sizeof(kGlass2x)}},
    {kArrowSlot, {kArrow1x, kArrow2x}, {sizeof(kArrow1x), sizeof(kArrow2x)}},
    {kBlipSlot, {kBlip1x, kBlip2x}, {sizeof(kBlip1x), sizeof(kBlip2x)}},
    {kSaveSlot, {kSave1x, kSave2x}, {sizeof(kSave1x), sizeof(kSave2x)}},
    {kGarageSlot, {kGarage1x, kGarage2x}, {sizeof(kGarage1x), sizeof(kGarage2x)}},
    {kRestartSlot, {kRestart1x, kRestart2x}, {sizeof(kRestart1x), sizeof(kRestart2x)}},
    {kArrestSlot, {kArrest1x, kArrest2x}, {sizeof(kArrest1x), sizeof(kArrest2x)}},
    {kStationSlot, {kStation1x, kStation2x}, {sizeof(kStation1x), sizeof(kStation2x)}},
    {kCraneSlot, {kCrane1x, kCrane2x}, {sizeof(kCrane1x), sizeof(kCrane2x)}},
    {kPhoneGreenSlot, {kPhoneGreen1x, kPhoneGreen2x}, {sizeof(kPhoneGreen1x), sizeof(kPhoneGreen2x)}},
    {kPhoneYellowSlot, {kPhoneYellow1x, kPhoneYellow2x}, {sizeof(kPhoneYellow1x), sizeof(kPhoneYellow2x)}},
    {kPhoneRedSlot, {kPhoneRed1x, kPhoneRed2x}, {sizeof(kPhoneRed1x), sizeof(kPhoneRed2x)}},
    {kPhoneInfoSlot, {kPhoneInfo1x, kPhoneInfo2x}, {sizeof(kPhoneInfo1x), sizeof(kPhoneInfo2x)}},
};
bool sLoaded;
bool sLoadedOk;

const s32 kLevels = 8;
// Tiles a style has (gtx_0x106C::InitTileMapping): a lid's tile above them is no picture. 1023 marks a diagonal block's
// three-sided lid (MapRenderer::DrawTriangularDiagonal); read as a tile, it would be past the style's tiles.
const s32 kStyleTiles = 992;

// The level loaded since the map's picture was built (Reset).
bool sStale = true;

// Frames until the pins are looked for again: the cranes and the gangs' respect come with the level's script, after
// the map loads.
s32 sPinCountdown;
const s32 kPinRefreshFrames = 120;

const f32 kWaypointReached = 3.0f;

// Per lid tile, -1 until worked out: the share of its texels that are see-through (colour 0), and its colour in 4x4
// cells (RGB, 0 to 1). Tiles live four to a 256 texel wide row (gtx_0x106C::get_tile), their palette is the tile's
// (palette_types_enum::tiles_1), a physical palette's colours 64 palettes apart (gtx_0x106C::GetPalData), BGRA.
f32 sTileClear[kStyleTiles];
f32 sTileCells[kStyleTiles][kTexelsPerBlock][kTexelsPerBlock][3];

void ReadTile(s32 tile)
{
    if (sTileClear[tile] >= 0.0f)
    {
        return;
    }
    const u8* pTexels = gGtx_0x106C->get_tile(static_cast<u16>(tile));
    const s16 palette = gGtx_0x106C->GetTruePalette(palette_types_enum::tiles_1, static_cast<s16>(tile));
    const u8* pPalette = gGtx_0x106C->GetPalData(gGtx_0x106C->get_phys_pal(palette));
    const s32 cell = 64 / kTexelsPerBlock;
    s32 clear = 0;
    for (s32 cy = 0; cy < kTexelsPerBlock; cy++)
    {
        for (s32 cx = 0; cx < kTexelsPerBlock; cx++)
        {
            f32 sum[3] = {0, 0, 0};
            s32 n = 0;
            for (s32 y = cy * cell; y < (cy + 1) * cell; y++)
            {
                for (s32 x = cx * cell; x < (cx + 1) * cell; x++)
                {
                    const u8 index = pTexels[y * 256 + x];
                    if (!index)
                    {
                        clear++;
                        continue;
                    }
                    const u8* pColour = pPalette + index * 256;
                    sum[0] += pColour[2];
                    sum[1] += pColour[1];
                    sum[2] += pColour[0];
                    n++;
                }
            }
            for (s32 k = 0; k < 3; k++)
            {
                sTileCells[tile][cy][cx][k] = n ? sum[k] / (n * 255.0f) : 0.0f;
            }
        }
    }
    sTileClear[tile] = clear / 4096.0f;
}

// The top lid of a column: its level (1 and 2 the ground, 3 up roofs) and face word. A mostly see-through lid above
// the ground lets the one below show, as for tree tops and wires (render_map.py), and so does a lid without a tile.
bool TopLid(s32 x, s32 y, s32* pHeight, u16* pLid)
{
    for (s32 z = kLevels - 1; z >= 0; z--)
    {
        const gmp_block_info* pBlock = gMap_0x370->GetBlockOrNull(static_cast<u8>(x), static_cast<u8>(y), static_cast<u8>(z));
        const s32 tile = pBlock ? pBlock->mLid & block_face::tile_idx_mask : 0;
        if (!tile || tile >= kStyleTiles)
        {
            continue;
        }
        ReadTile(tile);
        if (z > 0 && sTileClear[tile] > 0.6f)
        {
            continue;
        }
        *pHeight = z;
        *pLid = pBlock->mLid;
        return true;
    }
    return false;
}

// The map from the lids as seen from above (turned and flipped as the face word says), roofs lighter as they rise and
// a shadow to the bottom right of what stands higher (render_map.py's shading).
void BuildMap()
{
    static s8 sHeight[kMapBlocks][kMapBlocks];
    static u16 sLid[kMapBlocks][kMapBlocks];
    for (f32& clear : sTileClear)
    {
        clear = -1.0f; // the level's style may differ from the last one's
    }
    for (s32 y = 0; y < kMapBlocks; y++)
    {
        for (s32 x = 0; x < kMapBlocks; x++)
        {
            s32 height = 0;
            u16 lid = 0;
            sHeight[y][x] = TopLid(x, y, &height, &lid) ? static_cast<s8>(height) : -1;
            sLid[y][x] = lid;
            sWater[y][x] = sHeight[y][x] < 0 || gGtx_0x106C->IsWaterTile(lid & block_face::tile_idx_mask);
        }
    }
    const auto heightAt = [](s32 x, s32 y) {
        x = x < 0 ? 0 : (x >= kMapBlocks ? kMapBlocks - 1 : x);
        y = y < 0 ? 0 : (y >= kMapBlocks ? kMapBlocks - 1 : y);
        return static_cast<f32>(sHeight[y][x] < 0 ? 0 : sHeight[y][x]);
    };
    sMap.assign(static_cast<size_t>(kMapTexels) * kMapTexels, Rgba(0.04f, 0.06f, 0.08f, 1.0f));
    for (s32 y = 0; y < kMapBlocks; y++)
    {
        for (s32 x = 0; x < kMapBlocks; x++)
        {
            if (sHeight[y][x] < 0)
            {
                continue;
            }
            const f32 h = heightAt(x, y);
            f32 cast = 0.0f;
            for (s32 k = 1; k <= 2; k++)
            {
                cast = fmaxf(cast, fminf(fmaxf(heightAt(x - k, y - k) - h, 0.0f), 3.0f) / (3.0f * k));
            }
            // Brighter than the lids' own colours, which are dark at this size.
            const f32 shade = 1.25f * (0.85f + 0.045f * fminf(fmaxf(h - 2.0f, 0.0f), 6.0f)) * (1.0f - 0.35f * fminf(cast, 1.0f));
            const u16 lid = sLid[y][x];
            const s32 tile = lid & block_face::tile_idx_mask;
            const bool flip = (lid & block_face::flip) != 0;
            const s32 turns = (lid >> 14) & 3; // quarter turns clockwise
            for (s32 ty = 0; ty < kTexelsPerBlock; ty++)
            {
                for (s32 tx = 0; tx < kTexelsPerBlock; tx++)
                {
                    // The tile's cell shown here: undo the turns, then the flip.
                    s32 cx = tx, cy = ty;
                    for (s32 t = 0; t < turns; t++)
                    {
                        const s32 ox = cx;
                        cx = cy;
                        cy = kTexelsPerBlock - 1 - ox;
                    }
                    if (flip)
                    {
                        cx = kTexelsPerBlock - 1 - cx;
                    }
                    const f32* c = sTileCells[tile][cy][cx];
                    sMap[static_cast<size_t>(y * kTexelsPerBlock + ty) * kMapTexels + x * kTexelsPerBlock + tx] =
                        Rgba(c[0] * shade, c[1] * shade, c[2] * shade, 1.0f);
                }
            }
        }
    }
    sMapVersion++;
}

// The gangs' territories, from the map's gang zones.
void FindTurfs()
{
    sTurfs.clear();
    if (!gGangPool_CA8 || !gMap_0x370->mpZones)
    {
        return;
    }
    for (u16 i = 0; i < gMap_0x370->mpZones->mNumZones; i++)
    {
        const gmp_map_zone* pZone = gMap_0x370->GetZone(i);
        Gang* pGang = pZone && pZone->mZoneType == Gang_14 ? gGangPool_CA8->FindGangByName(pZone->mName) : NULL;
        // As the respect meters (GangPool::FirstGang): the zones of the police and others are no gang of the player's.
        if (pGang && pGang->mUsed && pGang->HasKillRespectChange() && pGang->mArrowColour)
        {
            sTurfs.push_back(Turf{pGang, {static_cast<f32>(pZone->mX), static_cast<f32>(pZone->mY), static_cast<f32>(pZone->mX + pZone->mW),
                                          static_cast<f32>(pZone->mY + pZone->mH)}});
        }
    }
}

// The districts: the navigation zones by name, each name once at the area-weighted middle of its zones.
void FindDistricts()
{
    sDistricts.clear();
    if (!gMap_0x370->mpZones || !gText_0x14)
    {
        return;
    }
    for (u16 i = 0; i < gMap_0x370->mpZones->mNumZones; i++)
    {
        gmp_map_zone* pZone = gMap_0x370->GetZone(i);
        wchar_t* pName = pZone && pZone->mZoneType == Navigation_1 ? pZone->get_zone_str() : NULL;
        if (!pName)
        {
            continue;
        }
        wchar_t name[48];
        wcsncpy(name, gText_0x14->StrToUpper(pName), 47);
        name[47] = 0;
        const f32 area = static_cast<f32>(pZone->mW) * pZone->mH;
        const f32 x = pZone->mX + pZone->mW / 2.0f, y = pZone->mY + pZone->mH / 2.0f;
        size_t d = 0;
        while (d < sDistricts.size() && wcscmp(sDistricts[d].mName, name))
        {
            d++;
        }
        if (d == sDistricts.size())
        {
            District district = {};
            wcscpy(district.mName, name);
            sDistricts.push_back(district);
        }
        District& district = sDistricts[d];
        // A running weighted mean: the middle so far, moved toward this zone by its share of the area.
        district.mArea += area;
        district.mX += (x - district.mX) * area / district.mArea;
        district.mY += (y - district.mY) * area / district.mArea;
    }
    // The largest first: they keep their labels where names crowd.
    std::sort(sDistricts.begin(), sDistricts.end(), [](const District& a, const District& b) { return a.mArea > b.mArea; });
}

// A pin, unless one of its kind is already within this many blocks: one for a garage's bays, a station's platforms.
void AddPin(f32 x, f32 y, Slot slot, f32 apart)
{
    for (const Pin& pin : sPins)
    {
        if (pin.mSlot == slot && fabsf(pin.mX - x) < apart && fabsf(pin.mY - y) < apart)
        {
            return;
        }
    }
    sPins.push_back(Pin{x, y, slot, slot == kRestartSlot || slot == kArrestSlot});
}

// The church: an area where the player on foot starts a thread whose first commands save the game (THREAD_DECLARE5,
// then DO_SAVE_GAME, as tools/fullmap.py reads the .scr). The trigger's position is the area's centre (its touch point).
void FindChurches()
{
    if (!gScriptManager)
    {
        return;
    }
    const u16 kNumBasePointers = GTA2_COUNTOF(gScriptManager->mBasePointers);
    for (u16 i = 0; i < kNumBasePointers; i++)
    {
        const SCR_CMD_HEADER* pCmd = gScriptManager->GetBasePointer(i);
        if (!pCmd || pCmd->mType != SCRCMD_THREAD_DECLARE5)
        {
            continue;
        }
        const SCR_CHAR_AREA_ANY* pArea = reinterpret_cast<const SCR_CHAR_AREA_ANY*>(pCmd);
        s32 next = pArea->mUnusedE; // the thread's first command
        bool saves = false;
        for (s32 k = 0; k < 4 && next > 0 && next < kNumBasePointers && !saves; k++)
        {
            const SCR_CMD_HEADER* pThreadCmd = gScriptManager->GetBasePointer(static_cast<u16>(next));
            if (!pThreadCmd)
            {
                break;
            }
            saves = pThreadCmd->mType == SCRCMD_DO_SAVE_GAME;
            next = pThreadCmd->mCmdNext;
        }
        if (saves)
        {
            sPins.push_back(Pin{ToF(pArea->mX), ToF(pArea->mY), kSaveSlot, false});
        }
    }
}

// The garages: car shop bays (car_shop_130 objects), each respraying or selling a car weapon (Car::HandleShops), in a
// row two blocks apart: one pin for the row. Those the level's script declares (OBJ_DATA, opcodes 15 to 20, as
// tools/fullmap.py reads them), wherever the player is, and those in the object pool, which a job's script may add.
void FindGarages()
{
    const f32 kRow = 12.0f;
    if (gScriptManager)
    {
        const u16 kNumBasePointers = GTA2_COUNTOF(gScriptManager->mBasePointers);
        for (u16 i = 0; i < kNumBasePointers; i++)
        {
            const SCR_CMD_HEADER* pCmd = gScriptManager->GetBasePointer(i);
            if (!pCmd || pCmd->mType < SCRCMD_OBJ_DECSET_2D || pCmd->mType > SCRCMD_OBJ_DECSET_3D_STR)
            {
                continue;
            }
            const SCR_OBJ_DATA* pObj = reinterpret_cast<const SCR_OBJ_DATA*>(pCmd);
            if (pObj->mObjId == objects::car_shop_130)
            {
                AddPin(ToF(pObj->mPos.x), ToF(pObj->mPos.y), kGarageSlot, kRow);
            }
        }
    }
    for (Object* pObj = gObject_2C_Pool ? gObject_2C_Pool->GetFirstUsed() : NULL; pObj; pObj = pObj->mpNext)
    {
        if (pObj->mModel == objects::car_shop_130 && pObj->mSprite)
        {
            AddPin(ToF(pObj->mSprite->mXy.x), ToF(pObj->mSprite->mXy.y), kGarageSlot, kRow);
        }
    }
}

// The cranes the level's script set up (CranePool), at their rotors.
void FindCranes()
{
    if (!gCranePool_D9C)
    {
        return;
    }
    for (s32 i = 0; i < gCranePool_D9C->mCount && i < GTA2_COUNTOF_S(gCranePool_D9C->mCranes); i++)
    {
        const Crane& crane = gCranePool_D9C->mCranes[i];
        if (crane.mRotorObj && crane.mRotorObj->mSprite)
        {
            AddPin(ToF(crane.mRotorObj->mSprite->mXy.x), ToF(crane.mRotorObj->mSprite->mXy.y), kCraneSlot, 4.0f);
        }
    }
}

// The hospitals and police stations (the map's restart zones) and the train stations (their platforms' zones).
void FindZonePins()
{
    if (!gMap_0x370->mpZones)
    {
        return;
    }
    for (u16 i = 0; i < gMap_0x370->mpZones->mNumZones; i++)
    {
        const gmp_map_zone* pZone = gMap_0x370->GetZone(i);
        if (!pZone)
        {
            continue;
        }
        const f32 x = pZone->mX + pZone->mW / 2.0f, y = pZone->mY + pZone->mH / 2.0f;
        switch (pZone->mZoneType)
        {
            case gmp_zone_type_enum::restart:
                AddPin(x, y, kRestartSlot, 2.0f);
                break;
            case gmp_zone_type_enum::arrest_restart:
                AddPin(x, y, kArrestSlot, 2.0f);
                break;
            case gmp_zone_type_enum::railway_station_platform:
                AddPin(x, y, kStationSlot, 8.0f);
                break;
        }
    }
}

// Each gang's colour, the brighter half of its respect meter pip's texels (user sprite 70 plus its arrow colour, as
// tools/fullmap.py takes it): its bar's colour beside its icon on the HUD, read from the player's style once per level.
const s32 kMaxArrowColours = 16;
const f32 kMinSaturation = 0.75f;
const f32 kMinValue = 0.85f;
f32 sGangColours[kMaxArrowColours][3];
bool sGangColourRead[kMaxArrowColours];

void ReadGangColour(s32 arrowColour)
{
    sGangColours[arrowColour][0] = -1;
    const u16 index = gGtx_0x106C->GetSpriteTrueIndex(sprite_types_enum::user_6, static_cast<s16>(70 + arrowColour));
    const sprite_index* pSprite = gGtx_0x106C->get_sprite_index(index);
    if (!pSprite || !pSprite->mpData)
    {
        return;
    }
    const s16 palette = gGtx_0x106C->GetTruePalette(palette_types_enum::sprites_2, static_cast<s16>(index));
    const u8* pPalette = gGtx_0x106C->GetPalData(gGtx_0x106C->get_phys_pal(palette));
    struct Texel
    {
        f32 mBrightness;
        f32 mRgb[3];
    };
    std::vector<Texel> texels;
    for (s32 y = 0; y < pSprite->mHeight; y++)
    {
        for (s32 x = 0; x < pSprite->mWidth; x++)
        {
            const u8 c = pSprite->mpData[y * 256 + x];
            if (c)
            {
                const u8* pColour = pPalette + c * 256;
                texels.push_back(Texel{static_cast<f32>(pColour[0] + pColour[1] + pColour[2]),
                                       {pColour[2] / 255.0f, pColour[1] / 255.0f, pColour[0] / 255.0f}});
            }
        }
    }
    if (texels.empty())
    {
        return;
    }
    std::vector<f32> sorted;
    for (const Texel& t : texels)
    {
        sorted.push_back(t.mBrightness);
    }
    std::sort(sorted.begin(), sorted.end());
    const f32 median = sorted[sorted.size() / 2];
    f32 sum[3] = {0, 0, 0};
    s32 count = 0;
    for (const Texel& t : texels)
    {
        if (t.mBrightness >= median)
        {
            for (s32 k = 0; k < 3; k++)
            {
                sum[k] += t.mRgb[k];
            }
            count++;
        }
    }
    // Its hue, stronger: the Zaibatsu's pip is a pale grey-cyan that hardly shows as a tint, the other gangs' are already
    // strong (saturation 0.73 and up).
    f32* rgb = sGangColours[arrowColour];
    for (s32 k = 0; k < 3; k++)
    {
        rgb[k] = sum[k] / count;
    }
    const f32 high = fmaxf(rgb[0], fmaxf(rgb[1], rgb[2])), low = fminf(rgb[0], fminf(rgb[1], rgb[2]));
    if (high > 0.0f)
    {
        const f32 saturation = fmaxf((high - low) / high, kMinSaturation), value = fmaxf(high, kMinValue);
        for (s32 k = 0; k < 3; k++)
        {
            // Each channel's place between the lowest and the highest kept, stretched to the new saturation and value.
            const f32 t = high > low ? (rgb[k] - low) / (high - low) : 1.0f;
            rgb[k] = value * (1.0f - saturation * (1.0f - t));
        }
    }
}

// The game's job arrow (user sprite 0, the big arrow Hud_Arrow::DrawArrow draws) from the player's style, recoloured in
// the waypoint's blue: its brightness kept, its hue turned.
void BuildWaypointArrow()
{
    Picture& arrow = sPictures[kWaypointArrowSlot][0];
    arrow.mWidth = arrow.mHeight = 0;
    arrow.mPixels.clear();
    const u16 index = gGtx_0x106C->GetSpriteTrueIndex(sprite_types_enum::user_6, 0);
    const sprite_index* pSprite = gGtx_0x106C->get_sprite_index(index);
    if (!pSprite || !pSprite->mpData || !pSprite->mWidth || !pSprite->mHeight)
    {
        return;
    }
    const s16 palette = gGtx_0x106C->GetTruePalette(palette_types_enum::sprites_2, static_cast<s16>(index));
    const u8* pPalette = gGtx_0x106C->GetPalData(gGtx_0x106C->get_phys_pal(palette));
    arrow.mWidth = pSprite->mWidth;
    arrow.mHeight = pSprite->mHeight;
    arrow.mPixels.assign(static_cast<size_t>(arrow.mWidth) * arrow.mHeight, 0);
    const f32 to[3] = {0.25f, 0.85f, 1.0f};
    for (s32 y = 0; y < arrow.mHeight; y++)
    {
        for (s32 x = 0; x < arrow.mWidth; x++)
        {
            const u8 c = pSprite->mpData[y * 256 + x];
            if (!c)
            {
                continue;
            }
            const u8* pColour = pPalette + c * 256;
            const f32 r = pColour[2] / 255.0f, g = pColour[1] / 255.0f, b = pColour[0] / 255.0f;
            const f32 bright = fmaxf(r, fmaxf(g, b)), low = fminf(r, fminf(g, b));
            // Grey (its rim, its shading) stays grey; the colour goes to the waypoint's.
            const f32 sat = bright > 0 ? (bright - low) / bright : 0.0f;
            const f32 grey = (r + g + b) / 3;
            arrow.mPixels[static_cast<size_t>(y) * arrow.mWidth + x] = Rgba(fminf(grey * (1 - sat) + bright * to[0] * sat, 1.0f),
                                                                            fminf(grey * (1 - sat) + bright * to[1] * sat, 1.0f),
                                                                            fminf(grey * (1 - sat) + bright * to[2] * sat, 1.0f), 1.0f);
        }
    }
    sPictures[kWaypointArrowSlot][1] = arrow;
}
} // namespace

Picture sPictures[kNumSlots][2];
std::vector<u32> sMap;
u32 sMapVersion;
std::vector<u32> sStyled;
u32 sStyledVersion;
bool sWater[kMapBlocks][kMapBlocks];
std::vector<Pin> sPins;
std::vector<Turf> sTurfs;
std::vector<District> sDistricts;
bool sWaypoint;
f32 sWaypointX, sWaypointY;

u32 Rgba(f32 r, f32 g, f32 b, f32 a)
{
    const auto byte = [](f32 v) { return static_cast<u32>(fminf(fmaxf(v, 0.0f), 1.0f) * 255.0f + 0.5f); };
    return byte(r) | (byte(g) << 8) | (byte(b) << 16) | (byte(a) << 24);
}

bool Load()
{
    if (sLoaded)
    {
        return sLoadedOk;
    }
    sLoaded = true;
    sLoadedOk = true;
    for (const Art& art : kArt)
    {
        for (s32 s = 0; s < 2; s++)
        {
            Picture& p = sPictures[art.mSlot][s];
            sLoadedOk &= PortPng::Decode(art.mPng[s], art.mSize[s], &p.mPixels, &p.mWidth, &p.mHeight);
        }
    }
    // The grey blip: the red dome's shading, its brightest texel white, to take the targets' colour.
    for (s32 s = 0; s < 2 && sLoadedOk; s++)
    {
        const Picture& blip = sPictures[kBlipSlot][s];
        Picture& grey = sPictures[kGreyBlipSlot][s];
        grey = blip;
        u32 brightest = 1;
        for (u32 c : blip.mPixels)
        {
            brightest = (c & 0xFF) > brightest ? (c & 0xFF) : brightest;
        }
        for (u32& c : grey.mPixels)
        {
            const u32 v = (c & 0xFF) * 255 / brightest;
            c = (c & 0xFF000000u) | v | (v << 8) | (v << 16);
        }
    }
    return sLoadedOk;
}

bool GangColour(s32 arrowColour, f32* pRgb)
{
    if (arrowColour <= 0 || arrowColour >= kMaxArrowColours)
    {
        return false;
    }
    if (!sGangColourRead[arrowColour])
    {
        sGangColourRead[arrowColour] = true;
        ReadGangColour(arrowColour);
    }
    if (sGangColours[arrowColour][0] < 0)
    {
        return false;
    }
    memcpy(pRgb, sGangColours[arrowColour], sizeof(sGangColours[arrowColour]));
    return true;
}

void ToggleWaypoint(f32 x, f32 y, f32 near)
{
    if (sWaypoint && fabsf(sWaypointX - x) < near && fabsf(sWaypointY - y) < near)
    {
        sWaypoint = false;
        return;
    }
    sWaypoint = true;
    sWaypointX = fminf(fmaxf(x, 0.0f), static_cast<f32>(kMapBlocks));
    sWaypointY = fminf(fmaxf(y, 0.0f), static_cast<f32>(kMapBlocks));
}

// The map's own colours, each gang's territory tinted with the gang's colour and edged with it, as San Andreas shows
// the territories of its gang wars.
void BuildStyled()
{
    static s8 sTurf[kMapBlocks][kMapBlocks]; // index in colours, -1 none
    std::vector<const Gang*> gangs;
    std::vector<std::array<f32, 3>> colours;
    memset(sTurf, -1, sizeof(sTurf));
    for (const Turf& turf : sTurfs)
    {
        size_t g = 0;
        while (g < gangs.size() && gangs[g] != turf.mGang)
        {
            g++;
        }
        if (g == gangs.size())
        {
            std::array<f32, 3> rgb = {-1, -1, -1};
            GangColour(turf.mGang->mArrowColour, rgb.data());
            gangs.push_back(turf.mGang);
            colours.push_back(rgb);
        }
        for (s32 y = static_cast<s32>(turf.mRect[1]); y < static_cast<s32>(turf.mRect[3]) && y < kMapBlocks; y++)
        {
            for (s32 x = static_cast<s32>(turf.mRect[0]); x < static_cast<s32>(turf.mRect[2]) && x < kMapBlocks; x++)
            {
                sTurf[y][x] = static_cast<s8>(g);
            }
        }
    }
    const auto turfAt = [](s32 x, s32 y) { return x < 0 || y < 0 || x >= kMapBlocks || y >= kMapBlocks ? -1 : sTurf[y][x]; };
    sStyled = sMap;
    for (s32 ty = 0; ty < kMapTexels; ty++)
    {
        for (s32 tx = 0; tx < kMapTexels; tx++)
        {
            const s32 bx = tx / kTexelsPerBlock, by = ty / kTexelsPerBlock;
            const s32 g = sTurf[by][bx];
            if (g < 0 || sWater[by][bx] || colours[static_cast<size_t>(g)][0] < 0)
            {
                continue;
            }
            const f32* tint = colours[static_cast<size_t>(g)].data();
            // The territory's edge: the outer texel of a block next to another territory or none.
            const s32 sx = tx % kTexelsPerBlock, sy = ty % kTexelsPerBlock;
            const bool edge = (sx == 0 && turfAt(bx - 1, by) != g) || (sx == kTexelsPerBlock - 1 && turfAt(bx + 1, by) != g) ||
                (sy == 0 && turfAt(bx, by - 1) != g) || (sy == kTexelsPerBlock - 1 && turfAt(bx, by + 1) != g);
            u32& c = sStyled[static_cast<size_t>(ty) * kMapTexels + tx];
            const f32 rgb[3] = {(c & 0xFF) / 255.0f, ((c >> 8) & 0xFF) / 255.0f, ((c >> 16) & 0xFF) / 255.0f};
            const f32 lum = 0.30f * rgb[0] + 0.59f * rgb[1] + 0.11f * rgb[2];
            const f32 k = edge ? 0.85f : 0.4f;
            f32 out[3];
            for (s32 i = 0; i < 3; i++)
            {
                // The tint keeps the map's light and shade: the gang's colour at the texel's brightness.
                const f32 tinted = tint[i] * (0.3f + 1.1f * lum);
                out[i] = rgb[i] + (tinted - rgb[i]) * k;
            }
            c = Rgba(out[0], out[1], out[2], 1.0f);
        }
    }
    sStyledVersion++;
}

void LandBounds(f32* pRect)
{
    // The level's zones mark where it is played, the city's districts or a bonus level's arena; without any, the land.
    s32 l = kMapBlocks, t = kMapBlocks, r = 0, b = 0;
    const auto add = [&](s32 x0, s32 y0, s32 x1, s32 y1) {
        l = x0 < l ? x0 : l;
        t = y0 < t ? y0 : t;
        r = x1 > r ? x1 : r;
        b = y1 > b ? y1 : b;
    };
    for (u16 i = 0; gMap_0x370->mpZones && i < gMap_0x370->mpZones->mNumZones; i++)
    {
        const gmp_map_zone* pZone = gMap_0x370->GetZone(i);
        if (pZone && pZone->mW && pZone->mH)
        {
            add(pZone->mX, pZone->mY, pZone->mX + pZone->mW, pZone->mY + pZone->mH);
        }
    }
    for (s32 y = 0; y < kMapBlocks && r <= l; y++)
    {
        for (s32 x = 0; x < kMapBlocks; x++)
        {
            if (!sWater[y][x])
            {
                add(x, y, x + 1, y + 1);
            }
        }
    }
    if (r <= l || b <= t)
    {
        l = t = 0;
        r = b = kMapBlocks;
    }
    pRect[0] = static_cast<f32>(l);
    pRect[1] = static_cast<f32>(t);
    pRect[2] = static_cast<f32>(r);
    pRect[3] = static_cast<f32>(b);
}

const Sprite* Prepare()
{
    if (!gGame_0x40 || !gHud_2B00 || !gGtx_0x106C || !gMap_0x370 || !gMap_0x370->mpDmap || !gMap_0x370->mpDmap->mBlock)
    {
        return NULL;
    }
    Player* pPlayer = gGame_0x40->mLocalPlayer;
    Ped* pPed = pPlayer ? pPlayer->mPlayerPed : NULL;
    const Sprite* pSprite = pPed ? pPed->GetSprite() : NULL;
    if (!pSprite || !Load())
    {
        return NULL;
    }
    if (sStale)
    {
        sStale = false;
        BuildMap();
        BuildWaypointArrow();
        FindDistricts();
        sPinCountdown = 0;
    }
    const f32 toWaypointX = ToF(pSprite->mXy.x) - sWaypointX, toWaypointY = ToF(pSprite->mXy.y) - sWaypointY;
    if (sWaypoint && fabsf(toWaypointX) < kWaypointReached && fabsf(toWaypointY) < kWaypointReached)
    {
        sWaypoint = false;
    }
    if (--sPinCountdown <= 0)
    {
        sPinCountdown = kPinRefreshFrames;
        sPins.clear();
        FindTurfs();
        FindZonePins();
        FindGarages();
        FindCranes();
        FindChurches(); // last, drawn over the others
    }
    return pSprite;
}
#endif

void Reset()
{
#if defined(GTA2_PORT_SDL)
    // A new level: nothing of the last one's is kept, its gangs' pointers least of all.
    sStale = true;
    sPins.clear();
    sTurfs.clear();
    sDistricts.clear();
    sWaypoint = false;
    memset(sGangColourRead, 0, sizeof(sGangColourRead));
    ResetCityMap();
#endif
}
} // namespace PortMinimap
