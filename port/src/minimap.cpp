// The minimap in the HUD's bottom left corner (frame E of port/docs/minimap, slim), the MAP waypoint's arrow by the player, and
// the drawing both maps share.
#include "minimap.hpp"
#include "minimap_internal.hpp"
#if defined(GTA2_PORT_SDL)
#include "Camera.hpp"
#include "Draw.hpp"
#include "Game.hpp"
#include "Gang.hpp"
#include "Hud.hpp"
#include "Player.hpp"
#include "ScriptManager.hpp"
#include "enums.hpp"
#include "gtx_0x106C.hpp"
#include "settings.hpp"
#include "sprite.hpp"
#include "winmain.hpp"
#include <math.h>
#include <string.h>
#endif

namespace PortMinimap
{
#if defined(GTA2_PORT_SDL)
namespace
{
const f32 kPi = 3.14159265f;

// Frame E, slim, in HUD pixels at 1x: its size, the map's hole, the tubes' middle lines (the rail the gang icons ride
// on), its corner caps and the N cap on the top tube (tools/designs.py tube_frame_slim), its distance from the screen's
// bottom left corner.
const f32 kFrameW = 92.0f;
const f32 kFrameH = 80.0f;
const f32 kHole[4] = {7.0f, 7.0f, 85.0f, 73.0f};
const f32 kRail[4] = {3.6f, 3.6f, 88.4f, 76.4f};
const f32 kRailCorner = 6.0f;
const f32 kNorthCap = 5.5f; // its half width
const f32 kMargin = 6.0f;
const f32 kGangIconSize = 9.0f;

// Blocks across the map: closer on foot, further out as the camera rises with speed (its height 0 to 14).
const f32 kMinBlocksAcross = 22.0f;
const f32 kMaxBlocksAcross = 36.0f;
const f32 kBlocksPerCameraHeight = 1.0f;

// Pins up to this many times the map's half size away show at its edge.
const f32 kPinLeadOut = 2.0f;

// A target this far above or below the player (blocks) shows as a triangle pointing that way.
const f32 kLevelTolerance = 0.5f;

// port_DrawMinimap's texture for a picture at a scale.
s32 TextureSlot(Slot slot, s32 scale)
{
    return slot * 2 + scale;
}

void DrawPicture(const View& v, Slot slot, f32 x, f32 y, f32 alpha)
{
    DrawIcon(v, slot, x, y, Rgba(1, 1, 1, alpha));
}

void DrawPins(const View& v)
{
    for (const Pin& pin : sPins)
    {
        // As GTA3's radar: a place a little past the edge already shows there, fading, before it comes into view.
        f32 x, y, out;
        if (!pin.mMapOnly && ToScreen(v, pin.mX, pin.mY, 4.0f * v.mUi, kPinLeadOut, &x, &y, &out))
        {
            DrawPicture(v, pin.mSlot, x, y, EdgeAlpha(out) * v.mAlpha);
        }
    }
}

// Each gang's icon on the frame's tubes, as the game's gang arrows lead there: toward its information phone, above the
// phone once it is on the map. A gang without one: toward the nearest part of its territory, in the top left corner
// while the player is in it. frame: the frame's left and top on screen.
void DrawTurfs(const View& v, f32 frameLeft, f32 frameTop)
{
    const f32 ui = v.mUi;
    const f32 rail[4] = {frameLeft + kRail[0] * ui, frameTop + kRail[1] * ui, frameLeft + kRail[2] * ui, frameTop + kRail[3] * ui};
    const f32 railX = (rail[0] + rail[2]) / 2, railY = (rail[1] + rail[3]) / 2;
    const f32 halfW = (rail[2] - rail[0]) / 2, halfH = (rail[3] - rail[1]) / 2;
    const Gang* drawn[16];
    s32 numDrawn = 0;
    f32 placed[16][2];
    s32 numPlaced = 0;
    for (const Turf& turf : sTurfs)
    {
        bool seen = false;
        for (s32 i = 0; i < numDrawn && !seen; i++)
        {
            seen = drawn[i] == turf.mGang;
        }
        if (seen || numDrawn == 16)
        {
            continue;
        }
        const Gang* pGang = turf.mGang;
        drawn[numDrawn++] = pGang;
        // The nearest point of all its zones.
        f32 bestX = 0, bestY = 0, best = 1e9f;
        for (const Turf& other : sTurfs)
        {
            if (other.mGang != pGang)
            {
                continue;
            }
            const f32 nx = fminf(fmaxf(v.mPlayerX, other.mRect[0]), other.mRect[2]);
            const f32 ny = fminf(fmaxf(v.mPlayerY, other.mRect[1]), other.mRect[3]);
            const f32 d = (nx - v.mPlayerX) * (nx - v.mPlayerX) + (ny - v.mPlayerY) * (ny - v.mPlayerY);
            if (d < best)
            {
                best = d;
                bestX = nx;
                bestY = ny;
            }
        }
        const bool phone = pGang->mInfoPhoneX.mValue || pGang->mInfoPhoneY.mValue;
        if (phone)
        {
            bestX = ToF(pGang->mInfoPhoneX);
            bestY = ToF(pGang->mInfoPhoneY);
        }
        const bool corner = best < 0.01f && !phone;
        const f32 dx = corner ? -1.0f : bestX - v.mPlayerX, dy = corner ? -1.0f : bestY - v.mPlayerY;
        f32 inView;
        f32 phoneX, phoneY;
        if (phone && ToScreen(v, bestX, bestY, 0.0f, 0.85f, &phoneX, &phoneY, &inView))
        {
            // The phone in view: the icon just above it, which says whose it is.
            DrawGangIcon(v, pGang->mArrowColour, phoneX, phoneY - 9.0f * ui, kGangIconSize);
            continue;
        }
        // On the tubes, the direction stretched to them, off the corner caps and the N cap.
        const f32 k = 1.0f / fmaxf(fabsf(dx) / halfW, fabsf(dy) / halfH);
        const bool alongX = fabsf(dy) / halfH >= fabsf(dx) / halfW; // on the top or bottom tube
        const f32 cap = kRailCorner * ui;
        f32 x = alongX ? fminf(fmaxf(railX + dx * k, rail[0] + cap), rail[2] - cap) : (dx < 0 ? rail[0] : rail[2]);
        f32 y = alongX ? (dy < 0 ? rail[1] : rail[3]) : fminf(fmaxf(railY + dy * k, rail[1] + cap), rail[3] - cap);
        const f32 clearance = (kNorthCap + kGangIconSize / 2 + 3.0f) * ui;
        if (alongX && dy < 0 && fabsf(x - railX) < clearance)
        {
            x = railX + (x < railX ? -clearance : clearance);
        }
        // Along the tube from the icons already there, toward the side its target lies on.
        const f32 step = (kGangIconSize + 1.0f) * ui;
        for (s32 tries = 0; tries < 4; tries++)
        {
            bool overlaps = false;
            for (s32 i = 0; i < numPlaced && !overlaps; i++)
            {
                overlaps = fabsf(placed[i][0] - x) < step && fabsf(placed[i][1] - y) < step;
            }
            if (!overlaps)
            {
                break;
            }
            const f32 side = alongX ? (dx != 0 ? dx : x - railX) : (dy != 0 ? dy : y - railY);
            (alongX ? x : y) += side < 0 ? -step : step;
        }
        if (numPlaced < 16)
        {
            placed[numPlaced][0] = x;
            placed[numPlaced][1] = y;
            numPlaced++;
        }
        DrawGangIcon(v, pGang->mArrowColour, x, y, kGangIconSize);
    }
}
} // namespace

void DrawRect(f32 left, f32 top, f32 right, f32 bottom, u32 rgba)
{
    const f32 xyuv[24] = {left, top, 0, 0, right, top, 0, 0, right, bottom, 0, 0, left, top, 0, 0, right, bottom, 0, 0, left, bottom, 0, 0};
    const u32 colours[6] = {rgba, rgba, rgba, rgba, rgba, rgba};
    port_DrawMinimap(-1, 0, 0, NULL, 0, xyuv, colours, 6, false);
}

void DrawIcon(const View& v, Slot slot, f32 x, f32 y, u32 rgba, f32 size)
{
    const Picture& p = sPictures[slot][v.mScale];
    const f32 k = v.mUi / (v.mScale + 1) * size;
    const f32 hw = p.mWidth * k / 2;
    const f32 hh = p.mHeight * k / 2;
    const f32 l = x - hw, t = y - hh, r = x + hw, b = y + hh;
    const f32 xyuv[24] = {l, t, 0, 0, r, t, 1, 0, r, b, 1, 1, l, t, 0, 0, r, b, 1, 1, l, b, 0, 1};
    const u32 colours[6] = {rgba, rgba, rgba, rgba, rgba, rgba};
    port_DrawMinimap(TextureSlot(slot, v.mScale), p.mWidth, p.mHeight, p.mPixels.data(), slot == kWaypointArrowSlot ? sMapVersion : 1, xyuv,
                     colours, 6, false);
}

void DrawTurned(Slot slot, s32 scale, f32 x, f32 y, f32 k, f32 angle, u32 rgba)
{
    const Picture& p = sPictures[slot][scale];
    if (!p.mWidth)
    {
        return;
    }
    const f32 hw = p.mWidth * k / 2, hh = p.mHeight * k / 2;
    const f32 s = sinf(angle), c = cosf(angle);
    const f32 corners[4][4] = {{-hw, -hh, 0, 0}, {hw, -hh, 1, 0}, {hw, hh, 1, 1}, {-hw, hh, 0, 1}};
    f32 pts[4][4];
    for (s32 i = 0; i < 4; i++)
    {
        const f32 cx = corners[i][0], cy = corners[i][1];
        pts[i][0] = x + cx * c + cy * s;
        pts[i][1] = y - cx * s + cy * c;
        pts[i][2] = corners[i][2];
        pts[i][3] = corners[i][3];
    }
    const s32 order[6] = {0, 1, 2, 0, 2, 3};
    f32 xyuv[24];
    u32 colours[6];
    for (s32 i = 0; i < 6; i++)
    {
        memcpy(&xyuv[i * 4], pts[order[i]], sizeof(pts[0]));
        colours[i] = rgba;
    }
    // The waypoint's arrow is rebuilt with each level's style, as the map is.
    port_DrawMinimap(TextureSlot(slot, scale), p.mWidth, p.mHeight, p.mPixels.data(), slot == kWaypointArrowSlot ? sMapVersion : 1, xyuv,
                     colours, 6, false);
}

void DrawTriangle(f32 x, f32 y, f32 size, bool up, u32 rgba)
{
    const f32 dir = up ? -1.0f : 1.0f;
    const f32 xyuv[12] = {x - size, y - dir * size * 0.6f, 0, 0, x + size, y - dir * size * 0.6f, 0, 0, x, y + dir * size * 0.8f, 0, 0};
    const u32 colours[3] = {rgba, rgba, rgba};
    port_DrawMinimap(-1, 0, 0, NULL, 0, xyuv, colours, 3, false);
}

bool ToScreen(const View& v, f32 wx, f32 wy, f32 inset, f32 maxOut, f32* pX, f32* pY, f32* pOut)
{
    const f32 halfW = (v.mHole[2] - v.mHole[0]) / 2 - inset;
    const f32 halfH = (v.mHole[3] - v.mHole[1]) / 2 - inset;
    const f32 dx = (wx - v.mPlayerX) * v.mPixelsPerBlock;
    const f32 dy = (wy - v.mPlayerY) * v.mPixelsPerBlock;
    const f32 out = fmaxf(fabsf(dx) / halfW, fabsf(dy) / halfH);
    *pOut = out;
    if (out > maxOut)
    {
        return false;
    }
    const f32 k = out > 1.0f ? 1.0f / out : 1.0f;
    *pX = v.mCentreX + dx * k;
    *pY = v.mCentreY + dy * k;
    return true;
}

f32 EdgeAlpha(f32 out)
{
    return out <= 1.0f ? 1.0f : (out >= 5.0f ? 0.5f : 1.0f - 0.5f * (out - 1.0f) / 4.0f);
}

void DrawMap(const View& v, bool styled)
{
    const f32* h = v.mHole;
    const f32 ppb = v.mPixelsPerBlock;
    const auto u = [&](f32 sx) { return (v.mPlayerX + (sx - v.mCentreX) / ppb) / kMapBlocks; };
    const auto w = [&](f32 sy) { return (v.mPlayerY + (sy - v.mCentreY) / ppb) / kMapBlocks; };
    // Off the map's edge: the empty map's colour.
    DrawRect(h[0], h[1], h[2], h[3], Rgba(0.04f, 0.06f, 0.08f, v.mAlpha));
    const f32 left = fmaxf(h[0], v.mCentreX - v.mPlayerX * ppb);
    const f32 top = fmaxf(h[1], v.mCentreY - v.mPlayerY * ppb);
    const f32 right = fminf(h[2], v.mCentreX + (kMapBlocks - v.mPlayerX) * ppb);
    const f32 bottom = fminf(h[3], v.mCentreY + (kMapBlocks - v.mPlayerY) * ppb);
    if (right <= left || bottom <= top)
    {
        return;
    }
    const f32 xyuv[24] = {left, top, u(left), w(top), right, top, u(right), w(top), right, bottom, u(right), w(bottom),
                          left, top, u(left), w(top), right, bottom, u(right), w(bottom), left, bottom, u(left), w(bottom)};
    const u32 white = Rgba(1, 1, 1, v.mAlpha);
    const u32 colours[6] = {white, white, white, white, white, white};
    const Slot slot = styled ? kStyledSlot : kMapSlot;
    const u32* pPixels = styled ? sStyled.data() : sMap.data();
    port_DrawMinimap(TextureSlot(slot, 0), kMapTexels, kMapTexels, pPixels, styled ? sStyledVersion : sMapVersion, xyuv, colours, 6, false);
}

// A sprite of the player's style as the HUD draws them (DrawFigure), see-through below alpha 1 (CalcQuadFlags' mode 1,
// 31 steps).
static void DrawSprite(s32 type, s16 sprite, s32 paletteType, s16 remap, f32 x, f32 y, f32 width, f32 alpha)
{
    const sprite_index* pIndex = gGtx_0x106C->get_sprite_index(gGtx_0x106C->GetSpriteTrueIndex(type, sprite));
    const s32 texels = pIndex ? (pIndex->mWidth > pIndex->mHeight ? pIndex->mWidth : pIndex->mHeight) : 0;
    if (!texels)
    {
        return;
    }
    const bool faded = alpha < 0.99f;
    DrawFigure(type, sprite, Fix16(x), Fix16(y), Ang16(0), Fix16(width / texels), paletteType, remap, faded ? 1 : 0,
               faded ? static_cast<u8>(alpha * 31.0f + 0.5f) : 0, false);
}

void DrawGangIcon(const View& v, s32 arrowColour, f32 x, f32 y, f32 size)
{
    // The respect meters' icon: user sprite 63 plus the gang's arrow colour (Hud_GangRespectBars).
    const s16 sprite = static_cast<s16>(63 + arrowColour);
    DrawSprite(sprite_types_enum::user_6, sprite, palette_types_enum::sprites_2, 0, x, y, v.mUi * size, v.mAlpha);
}

void DrawPhone(const View& v, s32 arrowColour, f32 x, f32 y, f32 alpha)
{
    // The phones' colours by the arrow's (Hud.cpp get_phone_colour).
    const Slot slot = arrowColour == 1   ? kPhoneYellowSlot
                      : arrowColour == 2 ? kPhoneRedSlot
                      : arrowColour == 3 ? kPhoneGreenSlot
                                         : kPhoneInfoSlot;
    DrawIcon(v, slot, x, y, Rgba(1, 1, 1, alpha));
}

bool PhoneAvailable(const Hud_ArrowTargets& t)
{
    const Player* pPlayer = gGame_0x40 ? gGame_0x40->mLocalPlayer : NULL;
    return !pPlayer || !t.mGangInfo.mGang || t.mGangInfo.mGang->GetRespectForPlayer(pPlayer->get_idx()) >= t.mGangInfo.mMinRespect;
}

u32 TargetColour(f32 alpha)
{
    // Pink, as GTA3's radar shows a job's targets, apart from the phones' red.
    return Rgba(1.0f, 0.35f, 0.8f, alpha);
}

void DrawTargets(const View& v, f32 playerZ, bool city)
{
    const bool onMission = gScriptManager && gScriptManager->IsOnMission();
    for (Hud_Arrow& arrow : gHud_2B00->mArrows.mArray)
    {
        if (arrow.HasNeitherTarget())
        {
            continue;
        }
        const Hud_ArrowTargets& t = arrow.mTargets;
        const ArrowTrace* traces[2] = {&t.mPrimaryTarget, &t.mSecondaryTarget};
        for (const ArrowTrace* pTrace : traces)
        {
            if (pTrace->HasNoTarget())
            {
                continue;
            }
            const f32 wx = ToF(pTrace->mAimX), wy = ToF(pTrace->mAimY), wz = ToF(pTrace->mAimZ);
            f32 x, y, out;
            if (t.mGangInfo.mGang)
            {
                // A phone: off a job, where its arrow shows or within twice the map's size; on the MAP wherever it is.
                if (onMission || !ToScreen(v, wx, wy, 3.0f * v.mUi, city ? 1.0f : 2.0f, &x, &y, &out) || (out > 1.0f && !arrow.IsVisible()))
                {
                    continue;
                }
                const bool job = t.mArrowColour >= 1 && t.mArrowColour <= 3;
                const f32 locked = !job || PhoneAvailable(t) ? 1.0f : kLockedAlpha;
                DrawPhone(v, t.mArrowColour, x, y, EdgeAlpha(out) * v.mAlpha * locked);
                continue;
            }
            // A job's target, at the edge when far.
            ToScreen(v, wx, wy, 3.0f * v.mUi, 1e9f, &x, &y, &out);
            const f32 alpha = EdgeAlpha(out) * v.mAlpha;
            const f32 dz = wz - playerZ;
            if (fabsf(dz) <= kLevelTolerance)
            {
                DrawIcon(v, kGreyBlipSlot, x, y, TargetColour(alpha), kTargetSize);
            }
            else
            {
                const f32 size = 3.5f * v.mUi;
                DrawTriangle(x, y, size * 1.4f, dz > 0, Rgba(0, 0, 0, alpha));
                DrawTriangle(x, y, size, dz > 0, TargetColour(alpha));
            }
        }
    }
}

void DrawPlayer(const View& v, const Sprite* pSprite, f32 x, f32 y)
{
    // A heading of 0 faces south, +y (char.cpp moves by its sine and cosine), the arrow's picture north: half a turn more.
    const f32 angle = pSprite->mRotation.rValue * (2.0f * kPi / 1440.0f) + kPi;
    DrawTurned(kArrowSlot, v.mScale, x, y, v.mUi / (v.mScale + 1), angle, Rgba(1, 1, 1, v.mAlpha));
}

static void DrawDiamond(f32 x, f32 y, f32 size, u32 rgba)
{
    const f32 xyuv[24] = {x, y - size, 0, 0, x + size, y, 0, 0, x, y + size, 0, 0, x, y - size, 0, 0, x, y + size, 0, 0, x - size, y, 0, 0};
    const u32 colours[6] = {rgba, rgba, rgba, rgba, rgba, rgba};
    port_DrawMinimap(-1, 0, 0, NULL, 0, xyuv, colours, 6, false);
}

void DrawWaypointMark(f32 x, f32 y, f32 ui, f32 alpha)
{
    DrawDiamond(x, y, 5.5f * ui, Rgba(0, 0, 0, alpha));
    DrawDiamond(x, y, 4.0f * ui, Rgba(0.25f, 0.85f, 1.0f, alpha));
}
#endif

void Draw()
{
#if defined(GTA2_PORT_SDL)
    const Sprite* pSprite = PortSettings::Minimap() && gViewCamera ? Prepare() : NULL;
    if (!pSprite)
    {
        return;
    }
    View v;
    v.mUi = ToF(gViewCamera->mUiScale);
    v.mScale = v.mUi >= 1.5f ? 1 : 0;
    // At the screen's bottom left, kept at its distance from that corner of the safe area.
    const f32 frameLeft = ToF(UI_X(Fix16(0))) + kMargin * v.mUi;
    const f32 frameTop = ToF(UI_Y(Fix16(480))) - (kMargin + kFrameH) * v.mUi;
    for (s32 i = 0; i < 4; i++)
    {
        v.mHole[i] = (i % 2 ? frameTop : frameLeft) + kHole[i] * v.mUi;
    }
    v.mCentreX = (v.mHole[0] + v.mHole[2]) / 2;
    v.mCentreY = (v.mHole[1] + v.mHole[3]) / 2;
    v.mPlayerX = ToF(pSprite->mXy.x);
    v.mPlayerY = ToF(pSprite->mXy.y);
    const f32 across = fminf(fmaxf(kMinBlocksAcross + kBlocksPerCameraHeight * ToF(gViewCamera->mCurPos.mZ), kMinBlocksAcross),
                             kMaxBlocksAcross);
    v.mPixelsPerBlock = (v.mHole[2] - v.mHole[0]) / across;
    // A brief's face (from x 8 of the layout, which is centred) and text reach this corner on narrower screens: faded
    // while one shows there.
    f32 briefLeft;
    {
        PortUiCentred centred;
        briefLeft = ToF(UI_X(Fix16(8)));
    }
    v.mAlpha = gHud_2B00->mBrief.mCurrBriefs && frameLeft + kFrameW * v.mUi > briefLeft ? 0.3f : 1.0f;

    port_ClipMinimap(v.mHole);
    DrawMap(v, false);
    DrawPins(v);
    DrawTargets(v, ToF(pSprite->mZpos), false);
    if (sWaypoint)
    {
        f32 x, y, out;
        ToScreen(v, sWaypointX, sWaypointY, 4.0f * v.mUi, 1e9f, &x, &y, &out);
        DrawWaypointMark(x, y, v.mUi, v.mAlpha);
    }
    DrawPlayer(v, pSprite, v.mCentreX, v.mCentreY);
    port_ClipMinimap(NULL);
    const f32 frameX = frameLeft + kFrameW / 2 * v.mUi, frameY = frameTop + kFrameH / 2 * v.mUi;
    DrawPicture(v, kGlassSlot, frameX, frameY, v.mAlpha);
    DrawPicture(v, kFrameSlot, frameX, frameY, v.mAlpha);
    DrawTurfs(v, frameLeft, frameTop);
#endif
}

void DrawWaypointArrow()
{
#if defined(GTA2_PORT_SDL)
    // With the game's arrows, as they go round the player toward their targets (Hud_Arrow::UpdateScreenPos): at a little
    // distance from the player in the waypoint's direction, or just before it when it is that close.
    if (!sWaypoint || !PortSettings::GuideArrows() || IsNetworkGame())
    {
        return;
    }
    const Sprite* pSprite = Prepare();
    Player* pPlayer = gGame_0x40 ? gGame_0x40->mLocalPlayer : NULL;
    if (!pSprite || !sWaypoint || !pPlayer)
    {
        return;
    }
    Fix16 px, py, pz;
    pPlayer->get_pos(&px, &py, &pz);
    const f32 dx = sWaypointX - ToF(px), dy = sWaypointY - ToF(py);
    const f32 distance = sqrtf(dx * dx + dy * dy);
    if (distance < 0.01f)
    {
        return;
    }
    const f32 radius = fminf(pPlayer->GetPlayerCar() ? 1.6f : 1.2f, fmaxf(distance - 0.25f, 0.0f));
    Camera* pCam = pPlayer->get_camera();
    Fix16 sx, sy;
    pCam->ProjectWorldToScreen_4B90E0(Fix16(ToF(px) + dx / distance * radius), Fix16(ToF(py) + dy / distance * radius), pz, &sx, &sy);
    // Turned as the game turns its arrows: the angle of the player less the target, as DrawFigure turns a sprite.
    Fix16_Point displacement;
    displacement.SetXY(Fix16(-dx), Fix16(-dy));
    const f32 angle = displacement.GetAngle().rValue * (2.0f * kPi / 1440.0f);
    DrawTurned(kWaypointArrowSlot, 0, ToF(sx), ToF(sy), ToF(pCam->mUiScale), angle, Rgba(1, 1, 1, 1));
#endif
}
} // namespace PortMinimap
