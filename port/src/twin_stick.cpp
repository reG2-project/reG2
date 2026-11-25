#include "twin_stick.hpp"
#include "Camera.hpp"
#include "Car.hpp"
#include "Frontend.hpp"
#include "Game.hpp"
#include "Ped.hpp"
#include "Player.hpp"
#include "char.hpp"
#include "gbh_graphics.hpp"
#include "gtx_0x106C.hpp"
#include "ObjectDefinitions.hpp"
#include "saves.hpp"
#include "settings.hpp"
#include "sprite.hpp"
#include "TextureCache.hpp"
#include "Weapon.hpp"
#include "rng.hpp"
#include "view.hpp"
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include "winmain.hpp"

#if defined(GTA2_PORT_SDL)
bool port_TwinStickInput(float* pMoveX, float* pMoveY, float* pAimX, float* pAimY);
void port_MousePos(float* pX, float* pY);
void port_DrawReticle(f32 x, f32 y, f32 size, f32 alpha);
bool port_LookFurther();
#endif

namespace PortTwinStick
{
namespace
{
// The most the stick turns the ped per step, in Ang16 units (24 degrees); the mouse turns it at once.
const s32 kTurnStep = 96;
// In a jump, the original's turn keys turn the ped by this much more per step, up to the most
// (kJumpTurnAccel and kMaxJumpTurnSpeed in Player::DoPedControlInputs).
const s32 kJumpTurnAccel = 4;
const s32 kMaxJumpTurn = 24;
// A roof gun's turn per step, as SPECIAL with left or right (Car::HandleRoofTurretRotation).
const s32 kTurretStep = 0x14;
// A released stick's aim holds for these steps, as its reticle fades.
const s32 kAimSteps = 10;
// The pad's reticle: from this part of the half screen height away from the ped at the dead zone's edge, out to
// as far as the screen's centre is from its edge (the mouse's reach) with the stick pushed fully, by the deflection
// to the power 1.5; it follows the stick by this part per step. The camera's look-ahead never changes the reach.
const f32 kMinReach = 0.2f;
const f32 kPadFollow = 0.5f;
// AIM CAMERA leads this many half screen heights along the last aim, however far the aim is. While Shift, LT
// or R3 is held it looks this times the aim's offset on screen from the centre, at most this many half heights
// and with the player this many inside the screen's edges. It follows by this part per step.
const f32 kLead = 0.16f;
const f32 kLookFurther = 2.5f;
const f32 kMaxLookFurther = 1.2f;
const f32 kLookEdge = 0.1f;
const f32 kLookFollow = 0.12f;
// A ped shooting at the player on foot leads him by this part of his move while the bullet flies, drawn between
// these anew every this many steps, and by this many blocks at most.
const f32 kMinLead = 0.4f;
const f32 kMaxLead = 0.9f;
const u32 kLeadSteps = 15;
const f32 kMaxLeadBlocks = 1.5f;
// A move further than this in one step is a jump in place (a teleport), not a walk.
const f32 kMaxStepMove = 0.25f;
// A walking ped's sprite faces down, its trailing leg the rows above the torso no wider than this.
const s32 kLegWidth = 5;
// The hips: this part of the way down from the trailing leg to the frame's bottom.
const f32 kHips = 0.35f;
// Backpedalling, the stride kept (sin^2 of the angle off the back) by the leg ahead and the one behind, all of it up to
// kBackFull degrees off. A leg showing less than kMinShow texels is left out.
const f32 kLegLead = 0.5f;
const f32 kLegTrail = 0.1f;
const f32 kBackFull = 45.0f;
const f32 kMinShow = 0.5f;
// Toward the side, the body bobs kBob texels along the move twice a cycle and its rounded back leans kSway texels.
const f32 kBob = 1.0f;
const f32 kSway = 1.0f;
// The largest frame split, in texels, and the most frames kept split.
const s32 kMaxFrame = 64;
const s32 kMaxSplit = 48;

Char* spWalker; // the ped walking this step, along sMove
s32 sMove;
Char* spWalked; // the ped that walked along sMove this step
bool sMoving;
s32 sJumpTurn;
bool sBackward;
bool sMouse;
s32 sAim;
s32 sAimSteps;
f32 sPadX; // the pad's reticle on screen from the ped, in pixels
f32 sPadY;
f32 sAimDirX; // the last aim's direction in the world, zero before any aim
f32 sAimDirY;
f32 sLookX; // in blocks
f32 sLookY;
const Sprite* spTracked; // the player on foot at the last step, where he was and how far he went over it
Fix16 sTrackedX;
Fix16 sTrackedY;
s32 sTrackedCycle;
f32 sMovedX;
f32 sMovedY;

f32 ToF(Fix16 v)
{
    return v.mValue / 16384.0f;
}

// A world direction as an Ang16, as Fix16_Point::GetAngle_40ACD0.
s32 Angle(f32 x, f32 y)
{
    return Fix16::atan2_fixed(Fix16(y), Fix16(x)).rValue;
}

// From the screen's centre along (dx, dy) to its edge, less a margin.
f32 EdgeReach(f32 dx, f32 dy)
{
    const f32 x = static_cast<f32>(gViewCamera->mScreenPxCenterX);
    const f32 y = static_cast<f32>(gViewCamera->mScreenPxCenterY);
    const f32 margin = 0.05f * gViewCamera->mScreenPxHeight;
    const f32 w = static_cast<f32>(gViewCamera->mScreenPxWidth) - margin;
    const f32 h = static_cast<f32>(gViewCamera->mScreenPxHeight) - margin;
    const f32 tx = dx > 0 ? (w - x) / dx : (dx < 0 ? (margin - x) / dx : 1e9f);
    const f32 ty = dy > 0 ? (h - y) / dy : (dy < 0 ? (margin - y) / dy : 1e9f);
    return fmaxf(fminf(tx, ty), 0.0f);
}

// From b to a, -720 to 719.
s32 Diff(s32 a, s32 b)
{
    return ((a - b) % 1440 + 2160) % 1440 - 720;
}

// The player the port's controls move. None while a recording plays (the attract demo): it was made with the original's.
Ped* ControlledPed()
{
    // Single player only: a network game's server gets the input bits alone, so it would neither see nor predict these
    // controls (also with --players 1)
    Player* pPlayer = gGame_0x40 && !gFrontend && gGame_0x40->mNumPlayers == 1 && !bStartNetworkGame && !PortSaves::Replaying()
        ? gGame_0x40->mLocalPlayer
        : NULL;
    return pPlayer && !pPlayer->mDisableAllControls && pPlayer->mCameraMode == 0 ? pPlayer->mPlayerPed : NULL;
}

Ped* PlayerPed()
{
    return PortSettings::TwinStick() ? ControlledPed() : NULL;
}

Ped* OnFootPed()
{
    Ped* pPed = PlayerPed();
    return pPed && pPed->mGameObject && !pPed->get_car() ? pPed : NULL;
}

void SetAim(const Sprite* pSprite, f32 x, f32 y)
{
    const f32 dx = x - ToF(pSprite->mXy.x);
    const f32 dy = y - ToF(pSprite->mXy.y);
    sAim = Angle(dx, dy);
    sAimSteps = kAimSteps;
    const f32 m = sqrtf(dx * dx + dy * dy);
    if (m > 1e-3f)
    {
        sAimDirX = dx / m;
        sAimDirY = dy / m;
    }
}

// The aim from pSprite this step, into sAim and sAimSteps: the mouse, the right stick, or the stick's last aim.
void Aim(const Sprite* pSprite, f32 ax, f32 ay)
{
#if defined(GTA2_PORT_SDL)
    if (sMouse)
    {
        f32 x, y;
        port_MousePos(&x, &y);
        PortView::ScreenToGround(x, y, ToF(pSprite->mZpos), &x, &y);
        SetAim(pSprite, x, y);
    }
    else if ((ax || ay) && gViewCamera)
    {
        f32 x, y;
        PortView::GroundToScreen(ToF(pSprite->mXy.x), ToF(pSprite->mXy.y), ToF(pSprite->mZpos), &x, &y);
        const f32 m = sqrtf(ax * ax + ay * ay);
        const f32 t = fminf((m - 0.25f) / 0.75f, 1.0f);
        const f32 min = kMinReach * gViewCamera->mScreenPxCenterY;
        const f32 reach = fmaxf(EdgeReach(ax / m, ay / m), min);
        const f32 r = min + (reach - min) * t * sqrtf(t);
        sPadX += (ax / m * r - sPadX) * kPadFollow;
        sPadY += (ay / m * r - sPadY) * kPadFollow;
        PortView::ScreenToGround(x + sPadX, y + sPadY, ToF(pSprite->mZpos), &x, &y);
        SetAim(pSprite, x, y);
    }
    else if (sAimSteps > 0)
    {
        sAimSteps--;
    }
#endif
}

// The roof gun of the player's fire truck, tank or gun jeep.
AttachedSprite* Turret(Car* pCar)
{
    const s32 model = pCar->IsFireTruck() ? objects::water_cannon_114
        : (pCar->IsTank() ? objects::tank_cannon_148 : (pCar->IsGunJeep() ? 248 : 0));
    return model ? pCar->mAttachments.GetSpriteForModel(model) : NULL;
}

// The sprite the aim starts from: the player on foot, or his vehicle with a roof gun.
const Sprite* AimFrom()
{
    Ped* pPed = PlayerPed();
    Car* pCar = pPed ? pPed->get_car() : NULL;
    return pCar ? (Turret(pCar) ? pCar->mCarSprite : NULL) : (pPed && pPed->mGameObject ? pPed->GetSprite() : NULL);
}

// A frame's rows: the trailing leg from mTop to above mSplit, the first row wider than a leg; mLeft to mRight its
// columns. The front leg uses the same fields for its rows (mTop to mSplit) and columns.
struct Leg
{
    s32 mTop;
    s32 mSplit;
    s32 mLeft;
    s32 mRight;
};

// A run or walk frame split in two textures: the legs (behind and the front foot) and the rest, the torso.
struct Split
{
    s32 mSprite; // the true sprite index, -1 while unused
    u16 mPal;
    s32 mWidth;
    s32 mHeight;
    s32 mLast; // the last drawn row
    s32 mBack; // the first row from mLeg.mSplit in one piece: the rows above it hold both feet side by side
    bool mHasLeg;
    bool mHasFront;
    Leg mLeg;
    Leg mFront;
    STexture* mpTorso;
    STexture* mpLegs;
    u8 mTorso[kMaxFrame * 256]; // sprite pages are 256 texels wide, as the textures read them
    u8 mLegs[kMaxFrame * 256];
};

struct Box
{
    f32 u0;
    f32 v0;
    f32 u1;
    f32 v1;
};

void SplitFrame(Split* pS, const u8* pData)
{
    const s32 w = pS->mWidth;
    const s32 h = pS->mHeight;
    memset(pS->mLegs, 0, sizeof(pS->mLegs));
    memset(pS->mTorso, 0, sizeof(pS->mTorso));
    for (s32 y = 0; y < h; y++)
    {
        memcpy(pS->mTorso + y * 256, pData + y * 256, w);
    }
    pS->mLast = 0;
    s32 top = -1;
    s32 split = h;
    for (s32 y = 0; y < h; y++)
    {
        s32 n = 0;
        for (s32 x = 0; x < w; x++)
        {
            n += pData[y * 256 + x] != 0;
        }
        pS->mLast = n ? y : pS->mLast;
        top = top < 0 && n ? y : top;
        split = split == h && n > kLegWidth ? y : split;
    }
    Leg& leg = pS->mLeg;
    leg.mTop = top;
    leg.mSplit = split;
    pS->mBack = split;
    pS->mHasLeg = top >= 0 && split - top >= 2 && split < h;
    pS->mHasFront = false;
    if (!pS->mHasLeg)
    {
        return;
    }
    // The legs behind: every pixel above the split, its top row the shoe; their last row carries on to the hips.
    bool shoe[256] = {};
    bool legColour[256] = {};
    leg.mLeft = w;
    leg.mRight = -1;
    for (s32 y = top; y < split; y++)
    {
        for (s32 x = 0; x < w; x++)
        {
            const u8 c = pData[y * 256 + x];
            if (c)
            {
                shoe[c] = shoe[c] || y == top;
                legColour[c] = true;
                pS->mLegs[y * 256 + x] = c;
                pS->mTorso[y * 256 + x] = 0;
                leg.mLeft = x < leg.mLeft ? x : leg.mLeft;
                leg.mRight = x > leg.mRight ? x : leg.mRight;
            }
        }
    }
    memcpy(pS->mLegs + split * 256 + leg.mLeft, pS->mLegs + (split - 1) * 256 + leg.mLeft, leg.mRight + 1 - leg.mLeft);
    for (s32 y = split; y < split + 3 && y < h; y++)
    {
        s32 runs = 0;
        for (s32 x = 0; x < w; x++)
        {
            runs += pData[y * 256 + x] && (x == 0 || !pData[y * 256 + x - 1]);
        }
        if (runs <= 1)
        {
            break;
        }
        pS->mBack = y + 1;
    }
    // The front leg, by the head: a shoe in the front half and the dark trouser pixels joined to it (the leg's colours
    // under 130, or within 3 of them), not the shirt's lighter ones.
    bool trousers[256] = {};
    for (s32 c = 0; c < 130; c++)
    {
        for (s32 d = -3; d <= 3; d++)
        {
            trousers[c] = trousers[c] || (c + d >= 1 && c + d < 130 && legColour[c + d] && !shoe[c + d]);
        }
    }
    static bool sIn[kMaxFrame * kMaxFrame];
    memset(sIn, 0, sizeof(sIn));
    const s32 from = (split + h) / 2;
    bool anyShoe = false;
    for (s32 y = from; y < h; y++)
    {
        for (s32 x = 0; x < w; x++)
        {
            sIn[y * kMaxFrame + x] = shoe[pData[y * 256 + x]];
            anyShoe = anyShoe || sIn[y * kMaxFrame + x];
        }
    }
    for (s32 i = 0; i < 8 && anyShoe; i++)
    {
        for (s32 y = h - 1; y >= from; y--)
        {
            for (s32 x = 0; x < w; x++)
            {
                bool next = false;
                for (s32 n = 0; n < 9; n++)
                {
                    const s32 yy = y + n / 3 - 1;
                    const s32 xx = x + n % 3 - 1;
                    next = next || (yy >= from && yy < h && xx >= 0 && xx < w && sIn[yy * kMaxFrame + xx]);
                }
                sIn[y * kMaxFrame + x] = sIn[y * kMaxFrame + x] || (next && trousers[pData[y * 256 + x]]);
            }
        }
    }
    Leg& front = pS->mFront;
    front.mTop = h;
    front.mSplit = -1;
    front.mLeft = w;
    front.mRight = -1;
    for (s32 y = from; y < h; y++)
    {
        for (s32 x = 0; x < w; x++)
        {
            if (sIn[y * kMaxFrame + x])
            {
                pS->mLegs[y * 256 + x] = pData[y * 256 + x];
                pS->mTorso[y * 256 + x] = 0;
                front.mTop = y < front.mTop ? y : front.mTop;
                front.mSplit = y;
                front.mLeft = x < front.mLeft ? x : front.mLeft;
                front.mRight = x > front.mRight ? x : front.mRight;
            }
        }
    }
    pS->mHasFront = front.mSplit >= 0;
}

// The sprite's frame idx split, made once per frame and kept; NULL if it cannot be.
const Split* GetSplit(const Sprite* pSprite, s32 idx)
{
    static Split sSplits[kMaxSplit];
    static bool sInit = false;
    for (s32 i = 0; i < kMaxSplit && !sInit; i++)
    {
        sSplits[i].mSprite = -1;
    }
    sInit = true;
    const s32 sprite = gGtx_0x106C->GetSpriteTrueIndex(pSprite->mSpriteTypeEnum, idx);
    const u16 pal = gGtx_0x106C->get_phys_pal(pSprite->GetTruePalette());
    Split* pS = NULL;
    for (s32 i = 0; i < kMaxSplit && !pS; i++)
    {
        pS = sSplits[i].mSprite == sprite || sSplits[i].mSprite < 0 ? &sSplits[i] : NULL;
    }
    const sprite_index* pIndex = gGtx_0x106C->get_sprite_index(sprite);
    if (!pS || pIndex->mWidth > kMaxFrame || pIndex->mHeight > kMaxFrame)
    {
        return NULL;
    }
    if (pS->mSprite != sprite)
    {
        pS->mSprite = sprite;
        pS->mPal = pal;
        pS->mWidth = pIndex->mWidth;
        pS->mHeight = pIndex->mHeight;
        SplitFrame(pS, pIndex->mpData);
        pS->mpTorso = pgbh_RegisterTexture(pS->mWidth, pS->mHeight, pS->mTorso, pal, 0);
        pS->mpLegs = pgbh_RegisterTexture(pS->mWidth, pS->mHeight, pS->mLegs, pal, 0);
    }
    else if (pS->mPal != pal)
    {
        pS->mPal = pal;
        pgbh_AssignPalette(pS->mpTorso, pal);
        pgbh_AssignPalette(pS->mpLegs, pal);
    }
    return pS;
}

// The texels tex of pTexture drawn on the frame's texels at, turned by turn about (px, py) in the frame, that point put
// at (dx, dy) texels from the centre of the sprite's quad pV (its world corners after it if count is 8).
void DrawTurned(STexture* pTexture, u32 flags, const Vert* pV, s32 count, const Box& at, const Box& tex, s32 turn,
                f32 px, f32 py, f32 dx, f32 dy)
{
    const f32 du = 1.0f / (pV[1].u - pV[0].u);
    const f32 dv = 1.0f / (pV[3].v - pV[0].v);
    const f32 pu = (pV[0].u + pV[1].u) * 0.5f + dx;
    const f32 pv = (pV[0].v + pV[3].v) * 0.5f + dy;
    const f32 c = cosf(turn * (3.14159265f / 720.0f));
    const f32 s = -sinf(turn * (3.14159265f / 720.0f));
    Vert verts[8];
    for (s32 i = 0; i < 4; i++)
    {
        const bool right = i == 1 || i == 2;
        const f32 u = (right ? at.u1 : at.u0) - px;
        const f32 v = (i < 2 ? at.v0 : at.v1) - py;
        // The quad is flat: a texel's place on screen and in the world is linear in (u, v).
        const f32 tu = (pu + u * c - v * s - pV[0].u) * du;
        const f32 tv = (pv + u * s + v * c - pV[0].v) * dv;
        for (s32 k = 0; k < count; k += 4)
        {
            Vert& out = verts[k + i];
            out = pV[k + i];
            out.x = pV[k].x + tu * (pV[k + 1].x - pV[k].x) + tv * (pV[k + 3].x - pV[k].x);
            out.y = pV[k].y + tu * (pV[k + 1].y - pV[k].y) + tv * (pV[k + 3].y - pV[k].y);
            out.z = pV[k].z + tu * (pV[k + 1].z - pV[k].z) + tv * (pV[k + 3].z - pV[k].z);
        }
        verts[i].u = right ? tex.u1 : tex.u0;
        verts[i].v = i < 2 ? tex.v0 : tex.v1;
    }
    pgbh_DrawQuad(flags, pTexture, verts, 255);
}

// Backpedalling, from the last drawn row: tall frames have empty rows below the head.
f32 Hips(const Split* pS)
{
    const s32 bottom = sBackward ? pS->mLast + 1 : pS->mHeight;
    return pS->mLeg.mSplit + (bottom - pS->mLeg.mSplit) * kHips;
}

f32 HalfWidth(const Split* pS)
{
    return (pS->mWidth - 0.3f) * 0.5f;
}

// The middle of leg, in texels from the frame's middle column.
f32 LegU(const Split* pS, const Leg& leg)
{
    return (leg.mLeft + leg.mRight + 1) * 0.5f - HalfWidth(pS);
}

// How far from the hips the torso of pT covers a leg at u texels across, behind (dir -1) or ahead (dir 1), turned by
// turn; backpedalling, over the leg's three columns.
f32 TorsoCover(const Split* pT, f32 u, s32 turn, f32 dir)
{
    const f32 c = cosf(turn * (3.14159265f / 720.0f));
    const f32 s = -sinf(turn * (3.14159265f / 720.0f));
    const f32 hips = Hips(pT);
    const s32 wide = sBackward ? 1 : 0;
    f32 cover = 0.0f;
    for (s32 o = -wide; o <= wide; o++)
    {
        const f32 uo = u + o;
        for (f32 d = 0.0f; d < 32.0f; d += 0.5f)
        {
            const f32 v = d * dir;
            const s32 x = static_cast<s32>(floorf(HalfWidth(pT) + uo - v * s));
            const s32 y = static_cast<s32>(floorf(hips + v * c));
            const bool in = x >= 0 && y >= 0 && x < pT->mWidth && y < pT->mHeight && pT->mTorso[y * 256 + x];
            cover = in ? fmaxf(cover, d) : cover;
        }
    }
    return cover;
}

// The legs behind of pL, or its front leg, turned by turn about its middle at the hips of pT, pulled in toward them
// by slide texels and cut there, the body moved by (bx, by).
void DrawLeg(const Split* pT, const Split* pL, bool front, u32 flags, const Vert* pV, s32 count, s32 turn, f32 slide,
             f32 bx, f32 by)
{
    const Leg& leg = front ? pL->mFront : pL->mLeg;
    const f32 hips = Hips(pL);
    const f32 l = static_cast<f32>(leg.mLeft);
    const f32 r = leg.mRight + 1.0f;
    const f32 px = sBackward ? (l + r) * 0.5f : HalfWidth(pL);
    const f32 dx = px - HalfWidth(pL) + bx;
    const f32 dy = Hips(pT) - (pT->mHeight - 0.3f) * 0.5f + by;
    if (front)
    {
        const f32 top = fmaxf(leg.mTop - slide, hips);
        const Box strip = {l, top, r, leg.mSplit + 1.0f - slide};
        const Box tex = {l, top + slide, r, strip.v1 + slide};
        if (strip.v0 < strip.v1)
        {
            DrawTurned(pL->mpLegs, flags, pV, count, strip, tex, turn, px, hips, dx, dy);
        }
        return;
    }
    const f32 end = fminf(leg.mSplit + 1.0f + slide, hips);
    const Box strip = {l, leg.mTop + slide, r, end};
    const Box tex = {l, strip.v0 - slide, r, end - slide};
    if (strip.v0 < strip.v1)
    {
        DrawTurned(pL->mpLegs, flags, pV, count, strip, tex, turn, px, hips, dx, dy);
        const Box join = {l, end, r, hips};
        const Box joinTex = {l, leg.mSplit + 0.5f, r, leg.mSplit + 0.5f};
        if (end < hips)
        {
            DrawTurned(pL->mpLegs, flags, pV, count, join, joinTex, turn, px, hips, dx, dy);
        }
    }
}

// The torso of pS from row top, the ends of its first two rows taken in by 2 and 1 texels times round (a rounded back
// where the legs went), round + sway on the left and round - sway on the right.
void DrawTorso(const Split* pS, u32 flags, const Vert* pV, s32 count, f32 top, f32 round, f32 sway, f32 bx, f32 by)
{
    const f32 fu = HalfWidth(pS);
    const f32 fv = (pS->mHeight - 0.3f) * 0.5f;
    s32 row = static_cast<s32>(top);
    bool found = false;
    for (; round > 0.0f && row < pS->mHeight && !found; row++)
    {
        for (s32 x = 0; x < pS->mWidth; x++)
        {
            found = found || pS->mTorso[row * 256 + x];
        }
    }
    for (s32 i = 0; i < 2 && found && row - 1 + i < pS->mHeight; i++)
    {
        const s32 r = row - 1 + i;
        s32 l = pS->mWidth;
        s32 rr = -1;
        for (s32 x = 0; x < pS->mWidth; x++)
        {
            l = pS->mTorso[r * 256 + x] && x < l ? x : l;
            rr = pS->mTorso[r * 256 + x] ? x : rr;
        }
        const f32 inL = fmaxf(round * (2 - i) + sway, 0.0f);
        const f32 inR = fmaxf(round * (2 - i) - sway, 0.0f);
        const Box part = {l + inL, fmaxf(top, static_cast<f32>(r)), rr + 1 - inR, r + 1.0f};
        if (rr >= l && part.u0 < part.u1)
        {
            DrawTurned(pS->mpTorso, flags, pV, count, part, part, 0, fu, fv, bx, by);
        }
        top = r + 1.0f;
    }
    const Box all = {0.0f, top, pS->mWidth - 0.3f, pS->mHeight - 0.3f};
    DrawTurned(pS->mpTorso, flags, pV, count, all, all, 0, fu, fv, bx, by);
}

// How far to pull in a leg that shows shown texels past the torso, shown0 unturned, to keep keep of it.
f32 Slide(f32 shown, f32 shown0, f32 keep)
{
    return fmaxf(sBackward ? shown - shown0 * keep : shown0 * (1.0f - keep), 0.0f);
}

// The player's move over the last step, as walls stop or turn him whatever his velocity.
void TrackMove(const Ped* pPed)
{
    const Sprite* pSprite = pPed && pPed == OnFootPed() ? pPed->GetSprite() : NULL;
    const s32 cycle = gpRng->get_cur_rng();
    sMovedX = sMovedY = 0.0f;
    if (pSprite && pSprite == spTracked && cycle == sTrackedCycle + 1)
    {
        sMovedX = ToF(pSprite->mXy.x - sTrackedX);
        sMovedY = ToF(pSprite->mXy.y - sTrackedY);
        if (sMovedX * sMovedX + sMovedY * sMovedY > kMaxStepMove * kMaxStepMove)
        {
            sMovedX = sMovedY = 0.0f;
        }
    }
    spTracked = pSprite;
    if (pSprite)
    {
        sTrackedX = pSprite->mXy.x;
        sTrackedY = pSprite->mXy.y;
    }
    sTrackedCycle = cycle;
}
} // namespace

void PedControls(Player* pPlayer, Ped* pPed)
{
    spWalker = NULL;
    sMoving = false;
    TrackMove(pPed);
#if defined(GTA2_PORT_SDL)
    if (!pPed || pPed != OnFootPed())
    {
        return;
    }
    Char* pChar = pPed->mGameObject;
    f32 mx, my, ax, ay;
    sMouse = port_TwinStickInput(&mx, &my, &ax, &ay);
    // The movement keys too: forward is up the screen.
    mx += pPlayer->mNowRightPressed - pPlayer->mNowLeftPressed;
    my += pPlayer->mNowDownPressed - pPlayer->mNowForwardPressed;
    const bool moving = mx * mx + my * my > 0.01f;
    const s32 move = moving ? Angle(mx, my) : 0;
    Aim(pPed->GetSprite(), ax, ay);
    const bool aiming = sAimSteps > 0;
    const s32 rotation = pChar->mRotation.rValue;
    s32 turn = aiming || moving ? Diff(aiming ? sAim : move, rotation) : 0;
    turn = sMouse ? turn : (turn > kTurnStep ? kTurnStep : (turn < -kTurnStep ? -kTurnStep : turn));
    const bool jumping = pChar->GetCharState() == Char_B4_state::Jumping_15;
    if (jumping && moving && pPlayer->mNowHandBrakeOrJumpPressed && pPlayer->mHandBrakeOrJumpChanged)
    {
        pChar->mRotation = Ang16(static_cast<s16>(move), 0);
    }
    // In a jump the move steers as the original's turn keys do there, without turning past it.
    s32 jumpTurn = 0;
    if (jumping && moving)
    {
        const s32 diff = Diff(move, pChar->mRotation.rValue);
        jumpTurn = sJumpTurn + (diff > 0 ? kJumpTurnAccel : -kJumpTurnAccel);
        jumpTurn = jumpTurn > kMaxJumpTurn ? kMaxJumpTurn : (jumpTurn < -kMaxJumpTurn ? -kMaxJumpTurn : jumpTurn);
        jumpTurn = (diff >= 0 && jumpTurn > diff) || (diff <= 0 && jumpTurn < diff) ? diff : jumpTurn;
    }
    sJumpTurn = jumpTurn;
    // Standing, no turn that calls off getting in a car.
    pPlayer->mTurnSpeed = Ang16(jumping ? jumpTurn : (!moving && pPed->IsPedGoingToEnterCar() ? 0 : turn));
    pPlayer->mMoveDirection = Fix16(moving ? 1 : 0);
    if (moving)
    {
        pPed->CancelEnterCarObjective();
        spWalker = pChar;
        sMove = move;
        sMoving = !jumping;
    }
#endif
}

bool Walk(Char* pChar, Ang16* pFacing)
{
    if (pChar != spWalker || pChar->mpPed != OnFootPed() || pChar->GetCharState() == Char_B4_state::Jumping_15)
    {
        return false;
    }
    spWalker = NULL;
    spWalked = pChar;
    // The walk adds the turn to mRotation: it goes along sMove, and turns back to the facing at the end.
    const s32 turn = pChar->mpPed->GetTurnSpeed().rValue;
    *pFacing = Ang16(static_cast<s16>(pChar->mRotation.rValue + turn), 0);
    pChar->mRotation = Ang16(static_cast<s16>(sMove - turn), 0);
    return true;
}

void WalkVelocity(Char* pChar)
{
    if (pChar == spWalked)
    {
        spWalked = NULL;
        Ang16 move(static_cast<s16>(sMove), 0);
        pChar->mVelocityVector.SetFromPolar(pChar->mVelocity, move);
    }
}

Fix16_Point ShotLead(const Char* pChar)
{
    Fix16_Point lead(pChar->mVelocityVector.x, pChar->mVelocityVector.y);
    if (pChar->mpPed && pChar->mpPed == OnFootPed())
    {
        const Fix16 sin = Ang16::sine(pChar->mSpritePtr->mRotation);
        const Fix16 cos = Ang16::cosine(pChar->mSpritePtr->mRotation);
        Fix16 along = lead.x * sin + lead.y * cos;
        along = along.mValue > 0 ? along : Fix16(0);
        lead.SetXY(along * sin, along * cos);
    }
    return lead;
}

bool DrawLegs(const Sprite* pSprite, STexture*, u32 flags, const Vert* pVerts, s32 count)
{
    Ped* pPed = OnFootPed();
    const s32 anim = pPed ? pPed->mGameObject->mAnimationState : -1;
    if (!PortSettings::StrafeAnimation() || !pPed || !sMoving || pSprite != pPed->GetSprite() || pSprite->mZoom ||
        (anim != Char_Anim_state::Walking_0 && anim != Char_Anim_state::Running_1))
    {
        return false;
    }
    // The walk and run frames: 0 to 15 from the body's first sprite, 37 to 52 with a weapon (Char::UpdateAnimState).
    const s32 idx = pSprite->mTypeSpriteIdx;
    const s32 body = idx >= 316 ? 316 : (idx >= 158 ? 158 : 0);
    const s32 frame = idx - body - (idx - body >= 37 ? 37 : 0);
    const s32 step = frame % 8;
    const s32 off = Diff(sMove, pSprite->mRotation.rValue);
    if (!off || frame > 15)
    {
        return false;
    }
    // Past 90 degrees off the facing the cycle plays backward, switching on a step that both ways show (0 or 4).
    sBackward = (abs(off) > 360) != sBackward && step % 4 == 0 ? !sBackward : sBackward;
    const s32 cycle = idx - step;
    const s32 drawn = sBackward ? (8 - step) % 8 : step;
    const s32 rel = sBackward ? off - (off > 0 ? 720 : -720) : off;
    const Split* pS = GetSplit(pSprite, cycle + drawn);
    if (!pS)
    {
        return false;
    }
    const f32 sn = sinf(rel * (3.14159265f / 720.0f));
    const f32 cs = cosf(rel * (3.14159265f / 720.0f));
    const f32 t = sn * sn;
    // The stride the legs keep: forward the frame's own, unturned; backpedalling they turn to the move, the frame's own
    // leg ahead and the one half a cycle on behind it.
    const f32 full = cosf(kBackFull * (3.14159265f / 180.0f));
    const f32 keep = sBackward ? fminf(cs * cs / fmaxf(full * full, 1e-3f), 1.0f) : 1.0f - t;
    const s32 turn = sBackward ? rel : 0;
    const f32 kFront = sBackward ? (1.0f - t) * keep : keep;
    const f32 kOwn = sBackward ? (1.0f - (1.0f - kLegLead) * t) * keep : keep;
    const f32 kOther = sBackward ? kLegTrail * t * keep : 0.0f;
    const f32 bob = kBob * t * sinf((step - 2) * (3.14159265f / 2.0f));
    const f32 bx = -sinf(off * (3.14159265f / 720.0f)) * bob;
    const f32 by = cosf(off * (3.14159265f / 720.0f)) * bob;
    if (pS->mHasFront)
    {
        const f32 u = LegU(pS, pS->mFront);
        const f32 reach = pS->mFront.mSplit + 1.0f - Hips(pS);
        const f32 shown0 = reach - TorsoCover(pS, u, 0, 1.0f);
        const f32 shown = reach - TorsoCover(pS, u, turn, 1.0f);
        if (shown0 * kFront >= kMinShow)
        {
            DrawLeg(pS, pS, true, flags, pVerts, count, turn, Slide(shown, shown0, kFront), bx, by);
        }
    }
    const Split* pOther = kOther > 0.0f ? GetSplit(pSprite, cycle + (drawn + 4) % 8) : NULL;
    if (pOther && pOther->mHasLeg)
    {
        const s32 back = rel + (rel > 0 ? -720 : 720);
        const f32 u = LegU(pOther, pOther->mLeg);
        const f32 len = Hips(pOther) - pOther->mLeg.mTop;
        const f32 len0 = len - TorsoCover(pOther, u, 0, -1.0f);
        if (len0 * kOther >= kMinShow)
        {
            const f32 slide = Slide(len - TorsoCover(pS, u, back, -1.0f), len0, kOther);
            DrawLeg(pS, pOther, false, flags, pVerts, count, back, slide, bx, by);
        }
    }
    if (pS->mHasLeg)
    {
        const f32 u = LegU(pS, pS->mLeg);
        const f32 len = Hips(pS) - pS->mLeg.mTop;
        const f32 len0 = len - TorsoCover(pS, u, 0, -1.0f);
        if (len0 * kOwn >= kMinShow)
        {
            const f32 slide = Slide(len - TorsoCover(pS, u, turn, -1.0f), len0, kOwn);
            DrawLeg(pS, pS, false, flags, pVerts, count, turn, slide, bx, by);
        }
    }
    // The feet side by side behind the back go under it with the legs.
    const f32 top = pS->mHasLeg ? pS->mLeg.mSplit + (pS->mBack - pS->mLeg.mSplit) * (1.0f - keep) : 0.0f;
    const f32 sway = kSway * (1.0f - keep) * sinf((step - 2) * (3.14159265f / 4.0f));
    DrawTorso(pS, flags, pVerts, count, top, 1.0f - keep, sway, bx, by);
    return true;
}

void AimTurret(Car* pCar)
{
#if defined(GTA2_PORT_SDL)
    AttachedSprite* pTurret = AimFrom() == pCar->mCarSprite ? Turret(pCar) : NULL;
    if (!pTurret)
    {
        return;
    }
    f32 mx, my, ax, ay;
    sMouse = port_TwinStickInput(&mx, &my, &ax, &ay);
    Aim(pCar->mCarSprite, ax, ay);
    if (sAimSteps > 0)
    {
        // At the original's turn speed. The fire truck's cannon fires backward from its sprite (Weapon.cpp).
        const s32 back = pCar->IsFireTruck() ? 720 : 0;
        const s32 turn = Diff(sAim - pCar->mCarSprite->mRotation.rValue - back, pTurret->mOffsetRot.rValue);
        pTurret->mOffsetRot += Ang16(turn > kTurretStep ? kTurretStep : (turn < -kTurretStep ? -kTurretStep : turn));
        pCar->mTurretRotated = turn != 0;
    }
#endif
}

bool FreeAim(const Ped* pPed)
{
    return pPed == OnFootPed();
}

void LeadAim(Ped* pShooter)
{
    Ped* pTarget = OnFootPed();
    Weapon* pWeapon = pTarget && pShooter->mInternalTargetPed == pTarget ? pShooter->GetWeaponFromPed() : NULL;
    if (!pWeapon || spTracked != pTarget->GetSprite() || (!sMovedX && !sMovedY))
    {
        return;
    }
    switch (pWeapon->mWeaponType)
    {
        case weapon_type::pistol:
        case weapon_type::smg:
        case weapon_type::silence_smg:
        case weapon_type::dual_pistol:
            break;
        default:
            return;
    }
    const f32 dx = ToF(pTarget->mCam.x - pShooter->mCam.x);
    const f32 dy = ToF(pTarget->mCam.y - pShooter->mCam.y);
    // The bullets these weapons fire at peds; the part of the lead is drawn anew every kLeadSteps for each shooter.
    const f32 speed = ToF(gObjectDefinitions->GetObjectDefinition(objects::machine_gun_bullet_254)->mSpeed);
    u32 hash = (pShooter->mId * 0x9E3779B1u) ^ (static_cast<u32>(gpRng->get_cur_rng()) / kLeadSteps * 0x85EBCA6Bu);
    hash = (hash ^ (hash >> 15)) * 0x2C1B3C6Du;
    const f32 part = kMinLead + (kMaxLead - kMinLead) * ((hash >> 8) & 0xFFFF) / 65535.0f;
    const f32 steps = sqrtf(dx * dx + dy * dy) / speed * part;
    f32 lx = sMovedX * steps;
    f32 ly = sMovedY * steps;
    const f32 lead = sqrtf(lx * lx + ly * ly);
    if (lead > kMaxLeadBlocks)
    {
        lx *= kMaxLeadBlocks / lead;
        ly *= kMaxLeadBlocks / lead;
    }
    pShooter->mTargetFacingAngle = Ang16(static_cast<s16>(Angle(dx + lx, dy + ly)), 0);
}

void DrawReticle()
{
#if defined(GTA2_PORT_SDL)
    const Sprite* pSprite = AimFrom();
    if (!pSprite || !gViewCamera || (!sMouse && !sAimSteps) || !PortSettings::Crosshair())
    {
        return;
    }
    f32 x, y;
    if (sMouse)
    {
        port_MousePos(&x, &y);
    }
    else
    {
        PortView::GroundToScreen(ToF(pSprite->mXy.x), ToF(pSprite->mXy.y), ToF(pSprite->mZpos), &x, &y);
        x += sPadX;
        y += sPadY;
    }
    port_DrawReticle(x, y, 18.0f * ToF(gViewCamera->mUiScale), sMouse ? 1.0f : static_cast<f32>(sAimSteps) / kAimSteps);
#endif
}
void AimCamera(Camera* pCamera, Ped* pPed, Fix16& x, Fix16& y)
{
#if defined(GTA2_PORT_SDL)
    Player* pPlayer = gGame_0x40 ? gGame_0x40->mLocalPlayer : NULL;
    if (!pPlayer || pCamera != &pPlayer->mGameCamera || !gViewCamera)
    {
        return;
    }
    if (PortSaves::Replaying() || bStartNetworkGame)
    {
        // A lead left from the last game would move the demo's or a network game's view, and with it where traffic appears.
        sLookX = sLookY = 0.0f;
    }
    f32 wantX = 0.0f;
    f32 wantY = 0.0f;
    if (PortSettings::AimCamera() && pPed == OnFootPed())
    {
        const f32 z = ToF(pPed->GetSprite()->mZpos);
        const f32 cx = static_cast<f32>(gViewCamera->mScreenPxCenterX);
        const f32 cy = static_cast<f32>(gViewCamera->mScreenPxCenterY);
        f32 x0, y0, tx, ty, lx, ly;
        PortView::ScreenToGround(cx, cy, z, &x0, &y0);
        PortView::ScreenToGround(cx, 0.0f, z, &tx, &ty);
        PortView::ScreenToGround(0.0f, cy, z, &lx, &ly);
        const f32 half = sqrtf((tx - x0) * (tx - x0) + (ty - y0) * (ty - y0));
        const f32 halfWidth = sqrtf((lx - x0) * (lx - x0) + (ly - y0) * (ly - y0));
        if (port_LookFurther() && (sMouse || sAimSteps))
        {
            // Measured on the screen, not from the aim's world point, which moves with the camera.
            f32 sx = cx + sPadX;
            f32 sy = cy + sPadY;
            if (sMouse)
            {
                port_MousePos(&sx, &sy);
            }
            f32 x1, y1;
            PortView::ScreenToGround(sx, sy, z, &x1, &y1);
            wantX = (x1 - x0) * kLookFurther;
            wantY = (y1 - y0) * kLookFurther;
            const f32 length = sqrtf(wantX * wantX + wantY * wantY);
            const f32 edge = half * kLookEdge;
            f32 scale = fminf(1.0f, half * kMaxLookFurther / fmaxf(length, 1e-6f));
            scale = fminf(scale, (halfWidth - edge) / fmaxf(fabsf(wantX), 1e-6f));
            scale = fminf(scale, (half - edge) / fmaxf(fabsf(wantY), 1e-6f));
            wantX *= scale;
            wantY *= scale;
        }
        else
        {
            wantX = sAimDirX * kLead * half;
            wantY = sAimDirY * kLead * half;
        }
    }
    sLookX += (wantX - sLookX) * kLookFollow;
    sLookY += (wantY - sLookY) * kLookFollow;
    if (!wantX && !wantY && sLookX * sLookX + sLookY * sLookY < 1e-4f)
    {
        sLookX = sLookY = 0.0f;
    }
    if (sLookX || sLookY)
    {
        x += Fix16(sLookX);
        y += Fix16(sLookY);
    }
#endif
}
} // namespace PortTwinStick

bool port_Driving()
{
    Ped* pPed = PortTwinStick::ControlledPed();
    return pPed && pPed->get_car();
}

bool port_EnteringCar()
{
    Ped* pPed = PortTwinStick::ControlledPed();
    return pPed && !pPed->get_car() && pPed->IsPedGoingToEnterCar();
}

bool port_TwinStickAims()
{
    return PortTwinStick::AimFrom() != NULL;
}

int port_TwinStick()
{
    Ped* pPed = PortTwinStick::PlayerPed();
    return !pPed ? 0 : (pPed->get_car() ? 2 : (pPed->mGameObject ? 1 : 0));
}
