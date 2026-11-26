// The pause menu's MAP: the whole city, moved and zoomed, gang territories tinted, the districts' names, the pins, phones
// and targets, a waypoint, and a key of what the level has. The pause menu lays the page out and writes its texts.
#include "minimap.hpp"
#include "minimap_internal.hpp"
#if defined(GTA2_PORT_SDL)
#include "Game.hpp"
#include "Gang.hpp"
#include "Hud.hpp"
#include "Player.hpp"
#include "ScriptManager.hpp"
#include "sprite.hpp"
#include <SDL3/SDL.h>
#include <dinput.h>
#include <math.h>
#include <string.h>

bool port_DikHeld(DWORD dik);
bool port_TakeMouse(float* pX, float* pY, int* pLeft, int* pRight, int* pWheel);
bool port_MouseLeftDown();
bool port_TwinStickInput(float* pMoveX, float* pMoveY, float* pAimX, float* pAimY);
#endif

namespace PortMinimap
{
#if defined(GTA2_PORT_SDL)
namespace
{
// The view: its middle in blocks, the blocks across its height, opened fitted to the land.
f32 sCityX, sCityY;
f32 sCityAcross;
bool sCityFit;
const f32 kCityMinAcross = 20.0f;
const f32 kCityZoomStep = 1.25f;
f32 sCityMaxAcross = kMapBlocks * 1.05f;
// The mouse: dragging the map since its button went down, and how far it moved (a click if little).
bool sCityDragging;
f32 sCityMouseX, sCityMouseY;
f32 sCityDragTravel;
// Blocks: a click or Enter this close to the waypoint takes it away, further moves it there.
f32 sCityNear = 4.0f;
Uint64 sCityTicks;
// Where DrawCityBase drew, for DrawCityMarkers.
View sCityView;
f32 sCityButtons[4];
const Sprite* spCitySprite;

struct Label
{
    const wchar_t* mText;
    f32 mX, mY;
};
const s32 kMaxLabels = 64;
Label sLabels[kMaxLabels];
s32 sNumLabels;

// The key's rows: what the level has, then each gang's territory.
enum Look
{
    kPictureLook,
    kTargetLook,
    kPhoneLook,
    kLockedLook,
    kWaypointLook,
    kTerritoryLook,
};
struct KeyRow
{
    Look mLook;
    Slot mSlot;
    s32 mValue; // a phone's arrow colour, a territory's gang arrow colour
    const wchar_t* mName;
};
std::vector<KeyRow> sKey;

// Zoomed by steps (in for more than 0), keeping the place under (ax, ay) where it is on screen.
void ZoomCity(f32 steps, f32 ax, f32 ay, f32 centreX, f32 centreY, f32 height)
{
    const f32 ppb = height / sCityAcross;
    const f32 wx = sCityX + (ax - centreX) / ppb, wy = sCityY + (ay - centreY) / ppb;
    sCityAcross = fminf(fmaxf(sCityAcross * powf(kCityZoomStep, -steps), kCityMinAcross), sCityMaxAcross);
    const f32 ppb2 = height / sCityAcross;
    sCityX = wx - (ax - centreX) / ppb2;
    sCityY = wy - (ay - centreY) / ppb2;
}

// The button under (x, y): 0 zoom in, 1 zoom out, 2 back to the player; -1 none.
s32 ButtonAt(f32 x, f32 y)
{
    const f32* b = sCityButtons;
    if (b[2] <= b[0] || x < b[0] || x >= b[2] || y < b[1] || y >= b[3])
    {
        return -1;
    }
    return static_cast<s32>((x - b[0]) * 3 / (b[2] - b[0]));
}

void PressButton(s32 button, f32 height)
{
    const View& v = sCityView;
    if (button == 0 || button == 1)
    {
        ZoomCity(button == 0 ? 1.0f : -1.0f, v.mCentreX, v.mCentreY, v.mCentreX, v.mCentreY, height);
    }
    else if (button == 2)
    {
        CentreCityMap();
    }
}

// Keys, the pad and the mouse; true when the right button asks to go back.
bool HandleInput(f32 left, f32 top, f32 right, f32 bottom, f32 ui)
{
    const f32 height = bottom - top;
    const f32 centreX = (left + right) / 2, centreY = (top + bottom) / 2;
    // Moved by the arrows or WASD while held, the left stick or the d-pad, dragged by the mouse; zoomed by its wheel, under
    // the pointer, and the right stick.
    const Uint64 now = SDL_GetTicks();
    const f32 dt = sCityTicks ? fminf((now - sCityTicks) / 1000.0f, 0.1f) : 0.0f;
    sCityTicks = now;
    f32 moveX, moveY, aimX, aimY;
    port_TwinStickInput(&moveX, &moveY, &aimX, &aimY);
    moveX += (port_DikHeld(DIK_RIGHT) || port_DikHeld(DIK_D)) - (port_DikHeld(DIK_LEFT) || port_DikHeld(DIK_A));
    moveY += (port_DikHeld(DIK_DOWN) || port_DikHeld(DIK_S)) - (port_DikHeld(DIK_UP) || port_DikHeld(DIK_W));
    moveX = fminf(fmaxf(moveX, -1.0f), 1.0f);
    moveY = fminf(fmaxf(moveY, -1.0f), 1.0f);
    sCityX += moveX * sCityAcross * 0.75f * dt;
    sCityY += moveY * sCityAcross * 0.75f * dt;
    if (fabsf(aimY) > 0.2f)
    {
        ZoomCity(-aimY * 4.0f * dt, centreX, centreY, centreX, centreY, height);
    }
    float mouseX, mouseY;
    int leftClicks, rightClicks, wheel;
    port_TakeMouse(&mouseX, &mouseY, &leftClicks, &rightClicks, &wheel);
    const bool inside = mouseX >= left && mouseX < right && mouseY >= top && mouseY < bottom;
    if (wheel && inside)
    {
        ZoomCity(static_cast<f32>(wheel), mouseX, mouseY, centreX, centreY, height);
    }
    if (port_MouseLeftDown() && (sCityDragging || inside))
    {
        if (sCityDragging)
        {
            sCityX -= (mouseX - sCityMouseX) * sCityAcross / height;
            sCityY -= (mouseY - sCityMouseY) * sCityAcross / height;
            sCityDragTravel += fabsf(mouseX - sCityMouseX) + fabsf(mouseY - sCityMouseY);
        }
        else
        {
            sCityDragTravel = 0;
        }
        sCityDragging = true;
        sCityMouseX = mouseX;
        sCityMouseY = mouseY;
    }
    else
    {
        // A click, not a drag: a button's, else the waypoint there, or none if it was on it. A tap of a touchpad can press
        // and release between two frames, never seen down: its press counted (leftClicks) at the pointer.
        const bool click = sCityDragging ? sCityDragTravel < 6.0f * ui : leftClicks > 0;
        const f32 clickX = sCityDragging ? sCityMouseX : mouseX, clickY = sCityDragging ? sCityMouseY : mouseY;
        const s32 button = click ? ButtonAt(clickX, clickY) : -1;
        if (button >= 0)
        {
            PressButton(button, height);
        }
        else if (click && clickX >= left && clickX < right && clickY >= top && clickY < bottom)
        {
            const f32 blocksPerPixel = sCityAcross / height;
            ToggleWaypoint(sCityX + (clickX - centreX) * blocksPerPixel, sCityY + (clickY - centreY) * blocksPerPixel, sCityNear);
        }
        sCityDragging = false;
    }
    sCityX = fminf(fmaxf(sCityX, 0.0f), static_cast<f32>(kMapBlocks));
    sCityY = fminf(fmaxf(sCityY, 0.0f), static_cast<f32>(kMapBlocks));
    sCityNear = 7.0f * ui * sCityAcross / height; // about the waypoint mark's size
    return rightClicks > 0;
}

// The districts' names where one fits on screen, away from the others; the largest districts first.
void PlaceLabels(const View& v)
{
    sNumLabels = 0;
    const f32 ui = v.mUi;
    for (const District& d : sDistricts)
    {
        f32 x, y, out;
        if (sNumLabels == kMaxLabels || sqrtf(d.mArea) * v.mPixelsPerBlock < 40.0f * ui / 1.5f ||
            !ToScreen(v, d.mX, d.mY, 12.0f * ui, 1.0f, &x, &y, &out) || out >= 1.0f)
        {
            continue;
        }
        bool crowded = false;
        for (s32 k = 0; k < sNumLabels && !crowded; k++)
        {
            crowded = fabsf(sLabels[k].mX - x) < 90.0f * ui / 1.5f && fabsf(sLabels[k].mY - y) < 16.0f * ui / 1.5f;
        }
        if (!crowded)
        {
            sLabels[sNumLabels++] = Label{d.mName, x, y};
        }
    }
}

// The key: the kinds of pins, phones and targets the level has now, then each gang's territory.
void BuildKey()
{
    sKey.clear();
    sKey.push_back(KeyRow{kPictureLook, kArrowSlot, 0, L"YOU"});
    sKey.push_back(KeyRow{kWaypointLook, kGreyBlipSlot, 0, L"WAYPOINT"});
    bool target = false, phones[4] = {}, locked = false, info = false;
    const bool onMission = gScriptManager && gScriptManager->IsOnMission();
    for (Hud_Arrow& arrow : gHud_2B00->mArrows.mArray)
    {
        const Hud_ArrowTargets& t = arrow.mTargets;
        if (arrow.HasNeitherTarget() || (t.mGangInfo.mGang && onMission))
        {
            continue;
        }
        const bool job = t.mArrowColour >= 1 && t.mArrowColour <= 3;
        target |= !t.mGangInfo.mGang;
        info |= t.mGangInfo.mGang && !job;
        if (t.mGangInfo.mGang && job)
        {
            phones[t.mArrowColour] = true;
            locked |= !PhoneAvailable(t);
        }
    }
    if (target)
    {
        sKey.push_back(KeyRow{kTargetLook, kGreyBlipSlot, 0, L"JOB TARGET"});
    }
    const struct
    {
        s32 mColour;
        const wchar_t* mName;
    } kPhones[] = {{3, L"EASY JOB"}, {1, L"MEDIUM JOB"}, {2, L"HARD JOB"}};
    for (const auto& phone : kPhones)
    {
        if (phones[phone.mColour])
        {
            sKey.push_back(KeyRow{kPhoneLook, kGreyBlipSlot, phone.mColour, phone.mName});
        }
    }
    if (locked)
    {
        sKey.push_back(KeyRow{kLockedLook, kGreyBlipSlot, 2, L"NEEDS MORE RESPECT"});
    }
    if (info)
    {
        sKey.push_back(KeyRow{kPhoneLook, kGreyBlipSlot, 0, L"GANG INFORMATION"});
    }
    const struct
    {
        Slot mSlot;
        const wchar_t* mName;
    } kPins[] = {{kSaveSlot, L"CHURCH - SAVE"}, {kGarageSlot, L"GARAGE"},          {kCraneSlot, L"CRANE"},
                 {kRestartSlot, L"HOSPITAL"},    {kArrestSlot, L"POLICE STATION"}, {kStationSlot, L"TRAIN STATION"}};
    for (const auto& kind : kPins)
    {
        for (const Pin& pin : sPins)
        {
            if (pin.mSlot == kind.mSlot)
            {
                sKey.push_back(KeyRow{kPictureLook, kind.mSlot, 0, kind.mName});
                break;
            }
        }
    }
    for (size_t i = 0; i < sTurfs.size(); i++)
    {
        bool seen = false;
        for (size_t k = 0; k < i && !seen; k++)
        {
            seen = sTurfs[k].mGang == sTurfs[i].mGang;
        }
        f32 rgb[3];
        if (!seen && GangColour(sTurfs[i].mGang->mArrowColour, rgb))
        {
            sKey.push_back(KeyRow{kTerritoryLook, kGreyBlipSlot, sTurfs[i].mGang->mArrowColour, L"TERRITORY"});
        }
    }
}

void DrawButtons(f32 ui)
{
    const f32* b = sCityButtons;
    if (b[2] <= b[0])
    {
        return;
    }
    const f32 w = (b[2] - b[0]) / 3, gap = 2.0f * ui;
    for (s32 i = 0; i < 3; i++)
    {
        const f32 l = b[0] + i * w + gap / 2, r = b[0] + (i + 1) * w - gap / 2;
        const f32 x = (l + r) / 2, y = (b[1] + b[3]) / 2, arm = fminf(r - l, b[3] - b[1]) * 0.28f, thin = 1.2f * ui;
        DrawRect(l, b[1], r, b[3], Rgba(0.16f, 0.15f, 0.13f, 0.95f));
        DrawRect(l, b[1], r, b[1] + thin, Rgba(0.45f, 0.4f, 0.32f, 1.0f));
        if (i < 2)
        {
            DrawRect(x - arm, y - thin, x + arm, y + thin, Rgba(1, 1, 1, 1));
        }
        if (i == 0)
        {
            DrawRect(x - thin, y - arm, x + thin, y + arm, Rgba(1, 1, 1, 1));
        }
        if (i == 2)
        {
            View v = {};
            v.mUi = ui;
            v.mScale = ui >= 1.5f ? 1 : 0;
            v.mAlpha = 1.0f;
            DrawIcon(v, kArrowSlot, x, y, Rgba(1, 1, 1, 1));
        }
    }
}
} // namespace

void ResetCityMap()
{
    sCityAcross = 0;
    sNumLabels = 0;
    sKey.clear();
    spCitySprite = NULL;
}
#endif

void OpenCityMap()
{
#if defined(GTA2_PORT_SDL)
    if (Prepare())
    {
        BuildStyled();
    }
    // Fitted to the land once the page's size is known (DrawCityBase): the whole city, or a bonus level's island.
    sCityFit = true;
    sCityAcross = kMapBlocks;
    sCityDragging = false;
    sCityTicks = 0;
    int left, right, wheel;
    float x, y;
    port_TakeMouse(&x, &y, &left, &right, &wheel);
#endif
}

void CentreCityMap()
{
#if defined(GTA2_PORT_SDL)
    if (const Sprite* pSprite = Prepare())
    {
        sCityX = ToF(pSprite->mXy.x);
        sCityY = ToF(pSprite->mXy.y);
    }
#endif
}

void ZoomCityMap(s32 steps)
{
#if defined(GTA2_PORT_SDL)
    // About the view's middle: its size does not matter.
    ZoomCity(static_cast<f32>(steps), 0, 0, 0, 0, 1);
#else
    (void)steps;
#endif
}

void ToggleCityWaypoint()
{
#if defined(GTA2_PORT_SDL)
    if (sCityAcross > 0)
    {
        ToggleWaypoint(sCityX, sCityY, sCityNear);
    }
#endif
}

bool DrawCityBase(f32 left, f32 top, f32 right, f32 bottom, f32 ui, const f32* pButtons)
{
#if defined(GTA2_PORT_SDL)
    spCitySprite = Prepare();
    if (!spCitySprite || sCityAcross <= 0)
    {
        spCitySprite = NULL;
        return false;
    }
    const f32 width = right - left, height = bottom - top;
    memcpy(sCityButtons, pButtons, sizeof(sCityButtons));
    // The whole city fits in the page at the furthest zoom, whatever its shape.
    sCityMaxAcross = kMapBlocks * 1.05f * fmaxf(1.0f, height / width);
    if (sCityFit)
    {
        sCityFit = false;
        f32 land[4];
        LandBounds(land);
        sCityX = (land[0] + land[2]) / 2;
        sCityY = (land[1] + land[3]) / 2;
        sCityAcross = fminf(fmaxf(fmaxf(land[3] - land[1], (land[2] - land[0]) * height / width) * 1.06f, kCityMinAcross), sCityMaxAcross);
    }
    const bool back = HandleInput(left, top, right, bottom, ui);

    View& v = sCityView;
    v.mUi = ui;
    v.mScale = ui >= 1.5f ? 1 : 0;
    v.mHole[0] = left;
    v.mHole[1] = top;
    v.mHole[2] = right;
    v.mHole[3] = bottom;
    v.mCentreX = (left + right) / 2;
    v.mCentreY = (top + bottom) / 2;
    v.mPlayerX = sCityX;
    v.mPlayerY = sCityY;
    v.mPixelsPerBlock = height / sCityAcross;
    v.mAlpha = 1.0f;
    port_ClipMinimap(v.mHole);
    DrawMap(v, true);
    port_ClipMinimap(NULL);
    PlaceLabels(v);
    BuildKey();
    return back;
#else
    (void)left;
    (void)top;
    (void)right;
    (void)bottom;
    (void)ui;
    (void)pButtons;
    return false;
#endif
}

void DrawCityMarkers()
{
#if defined(GTA2_PORT_SDL)
    if (!spCitySprite)
    {
        return;
    }
    const View& v = sCityView;
    const f32 ui = v.mUi;
    port_ClipMinimap(v.mHole);
    for (const Pin& pin : sPins)
    {
        f32 x, y, out;
        if (ToScreen(v, pin.mX, pin.mY, 0.0f, 1.0f, &x, &y, &out))
        {
            DrawIcon(v, pin.mSlot, x, y, Rgba(1, 1, 1, 1));
        }
    }
    DrawTargets(v, ToF(spCitySprite->mZpos), true);
    if (sWaypoint)
    {
        f32 x, y, out;
        if (ToScreen(v, sWaypointX, sWaypointY, 0.0f, 1.0f, &x, &y, &out))
        {
            DrawWaypointMark(x, y, ui, 1.0f);
        }
    }
    // The cross at the middle, where Enter puts the waypoint.
    const f32 arm = 6.0f * ui, thin = 0.5f * ui;
    DrawRect(v.mCentreX - arm, v.mCentreY - thin, v.mCentreX + arm, v.mCentreY + thin, Rgba(1, 1, 1, 0.7f));
    DrawRect(v.mCentreX - thin, v.mCentreY - arm, v.mCentreX + thin, v.mCentreY + arm, Rgba(1, 1, 1, 0.7f));
    f32 px, py, out;
    if (ToScreen(v, ToF(spCitySprite->mXy.x), ToF(spCitySprite->mXy.y), 6.0f * ui, 1e9f, &px, &py, &out))
    {
        View big = v;
        big.mUi = ui * 1.5f;
        big.mScale = 1;
        DrawPlayer(big, spCitySprite, px, py);
    }
    port_ClipMinimap(NULL);
    DrawButtons(ui);
#endif
}

s32 CityMapLabels()
{
#if defined(GTA2_PORT_SDL)
    return sNumLabels;
#else
    return 0;
#endif
}

const wchar_t* CityMapLabel(s32 i, f32* pX, f32* pY)
{
#if defined(GTA2_PORT_SDL)
    if (i < 0 || i >= sNumLabels)
    {
        return NULL;
    }
    *pX = sLabels[i].mX;
    *pY = sLabels[i].mY;
    return sLabels[i].mText;
#else
    (void)i;
    (void)pX;
    (void)pY;
    return NULL;
#endif
}

const wchar_t* LegendName(s32 i)
{
#if defined(GTA2_PORT_SDL)
    return i >= 0 && i < static_cast<s32>(sKey.size()) ? sKey[static_cast<size_t>(i)].mName : NULL;
#else
    (void)i;
    return NULL;
#endif
}

void DrawLegendIcon(s32 i, f32 x, f32 y, f32 ui)
{
#if defined(GTA2_PORT_SDL)
    if (!LegendName(i) || !Load())
    {
        return;
    }
    const KeyRow& row = sKey[static_cast<size_t>(i)];
    View v = {};
    v.mUi = ui;
    v.mScale = ui >= 1.5f ? 1 : 0;
    v.mAlpha = 1.0f;
    switch (row.mLook)
    {
        case kTargetLook:
            DrawIcon(v, row.mSlot, x, y, TargetColour(1.0f), kTargetSize);
            break;
        case kPhoneLook:
        case kLockedLook:
            DrawPhone(v, row.mValue, x, y, row.mLook == kLockedLook ? kLockedAlpha : 1.0f);
            break;
        case kWaypointLook:
            DrawWaypointMark(x, y, ui, 1.0f);
            break;
        case kTerritoryLook:
        {
            // The territory's tint behind its gang's icon.
            f32 rgb[3];
            GangColour(row.mValue, rgb);
            const f32 w = 7.0f * ui, h = 5.0f * ui;
            DrawRect(x - w - ui, y - h - ui, x + w + ui, y + h + ui, Rgba(0, 0, 0, 1));
            DrawRect(x - w, y - h, x + w, y + h, Rgba(rgb[0], rgb[1], rgb[2], 1));
            DrawGangIcon(v, row.mValue, x, y, 9.0f);
            break;
        }
        default:
            DrawIcon(v, row.mSlot, x, y, Rgba(1, 1, 1, 1));
            break;
    }
#else
    (void)i;
    (void)x;
    (void)y;
    (void)ui;
#endif
}

void DrawShade(f32 left, f32 top, f32 right, f32 bottom, f32 alpha)
{
#if defined(GTA2_PORT_SDL)
    DrawRect(left, top, right, bottom, Rgba(0.0f, 0.0f, 0.0f, alpha));
#else
    (void)left;
    (void)top;
    (void)right;
    (void)bottom;
    (void)alpha;
#endif
}
} // namespace PortMinimap
