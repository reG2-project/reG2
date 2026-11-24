#include "interpolation.hpp"
#include "Camera.hpp"
#include "Car.hpp"
#include "Game.hpp"
#include "Hud.hpp"
#include "Object_3C.hpp"
#include "char.hpp"
#include "Ped.hpp"
#include "Player.hpp"
#include "sprite.hpp"
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <vector>

namespace PortInterp
{
namespace
{
// Fix16 units: one block is 16384. A sprite or the camera moving further in one step jumped (respawn,
// teleport, a new use of the same sprite): it is drawn where it is.
const s32 kMaxStepMove = 2 * 16384;
const s32 kMaxCameraStepMove = 4 * 16384;
// Ang16 units: 1440 a turn. A sprite turning more than this in one step snapped round.
const s32 kMaxStepTurn = 360;
const s32 kTurn = 1440;

// Fix16 pixels of a 480 line screen, scaled to the screen's height: an arrow moving further in one step
// found a new target or came back into view.
const s32 kMaxArrowStepMove = 64 * 16384;
const s32 kArrowScreenHeight = 480;
const s32 kNumHudArrows = sizeof(gHud_2B00->mArrows.mArray) / sizeof(gHud_2B00->mArrows.mArray[0]);
// The HUD's arrows, then the marker over the player under a roof.
const s32 kNumArrows = kNumHudArrows + 1;

const s32 kNumSprites = sizeof(gSpritePool->mPool.mPool) / sizeof(gSpritePool->mPool.mPool[0]);

struct Pose
{
    s32 mX;
    s32 mY;
    s32 mZ;
    s16 mRot;
    s16 mId; // 0: no live sprite
};

struct Shape
{
    SpriteShape* mpShape;
    u8 mBytes[sizeof(SpriteShape)];

    void Save(SpriteShape* pShape)
    {
        mpShape = pShape;
        if (pShape)
        {
            memcpy(mBytes, pShape, sizeof(SpriteShape));
        }
    }

    bool UpToDate() const
    {
        return mpShape && reinterpret_cast<const SpriteShape*>(mBytes)->mBboxUpToDate;
    }

    void Restore(SpriteShape* pCurrent) const
    {
        if (mpShape && pCurrent == mpShape)
        {
            memcpy(mpShape, mBytes, sizeof(SpriteShape));
        }
    }
};

struct Saved
{
    Sprite* mpSprite;
    Pose mPose;
    char_type mZCol;
    Shape mVisual;
    Shape mCollision;
};

Pose sPoses[kNumSprites];
Pose sResimPoses[kNumSprites];
bool sResimValid;

struct Corrected
{
    const Sprite* mpSprite;
    Pose mPose;
};
std::vector<Corrected> sCorrected;
Camera* spCamera;
CameraPos sCameraPos;
bool sValid;

struct ArrowRef
{
    Fix16* mpX;
    Fix16* mpY;
    Ang16* mpRot;
    bool mShown;
};

struct ArrowPose
{
    s32 mX;
    s32 mY;
    s16 mRot;
    bool mShown; // a hidden arrow keeps the spot it was last shown at
};
ArrowPose sArrows[kNumArrows];
ArrowPose sArrowsDrawn[kNumArrows];
bool sArrowsApplied;

std::vector<Saved> sSaved;

// A player's ped's place as the step left it (Ped::mCam, where its name is drawn over it in a network game): moved by
// as much as its sprite (its car's in one) is moved for the draw
struct SavedPed
{
    Ped* mpPed;
    Fix16_Vec mCam;
};
std::vector<SavedPed> sSavedPeds;
u32 sMoved;
u32 sJumped;
u32 sSnapped;
s32 sSavedAt[kNumSprites]; // each sprite's index in sSaved, -1: none
bool sOnOwner[kNumSprites]; // drawn on the car or ped it is on (ApplyAttached)
bool sMeasure = getenv("GTA2_LOOP_TRACE") != NULL;
u32 sObjOff;
f32 sObjWorst;
Camera* spAppliedCamera;
u8 sCameraBytes[sizeof(Camera)];
u32 sSteps;
f64 sDrawTime;

// Steps a late shot takes to slide from the gun to its place: 3 frames at 60 fps
const f64 kSlideSteps = 1.5;
struct Slide
{
    Sprite* mpSprite;
    const Sprite* mpFrom;
    s16 mId, mFromId;
    f32 mMuzzle;
    bool mSlide;
    f64 mStart; // the time of its first frame, -1 before it
};
std::vector<Slide> sSlides;
std::vector<std::pair<f32, f32>> sSlideMeasures;

ArrowRef ArrowAt(s32 i)
{
    ArrowRef ref;
    if (i < kNumHudArrows)
    {
        Hud_Arrow& arrow = gHud_2B00->mArrows.mArray[i];
        ref.mpX = &arrow.mScreenPosX;
        ref.mpY = &arrow.mScreenPosY;
        ref.mpRot = &arrow.mRotation;
        ref.mShown = !arrow.HasNeitherTarget() && arrow.IsVisible();
    }
    else
    {
        Hud_UnderRoofArrowMarker& marker = gHud_2B00->mUnderRoofArrowMarker;
        ref.mpX = &marker.mScreenX;
        ref.mpY = &marker.mScreenY;
        ref.mpRot = &marker.mRotation;
        ref.mShown = marker.mPedUnderSolid;
    }
    return ref;
}

ArrowPose PoseOf(const ArrowRef& arrow)
{
    ArrowPose pose;
    pose.mX = arrow.mpX->mValue;
    pose.mY = arrow.mpY->mValue;
    pose.mRot = arrow.mpRot->rValue;
    pose.mShown = arrow.mShown;
    return pose;
}

Pose PoseOf(const Sprite& sprite)
{
    Pose pose;
    pose.mX = sprite.mXy.x.mValue;
    pose.mY = sprite.mXy.y.mValue;
    pose.mZ = sprite.mZpos.mValue;
    pose.mRot = sprite.mRotation.rValue;
    pose.mId = sprite.mInstanceId;
    return pose;
}

s32 Abs(s32 v)
{
    return v < 0 ? -v : v;
}

s32 Lerp(s32 prev, s32 cur, f32 alpha)
{
    const f64 v = prev + static_cast<f64>(cur - prev) * alpha;
    return static_cast<s32>(v < 0 ? v - 0.5 : v + 0.5);
}

// The shortest way round, in [-720, 720).
s32 TurnBetween(s32 prev, s32 cur)
{
    return ((cur - prev) % kTurn + kTurn + kTurn / 2) % kTurn - kTurn / 2;
}

Camera* ViewCamera()
{
    return gGame_0x40 && gGame_0x40->mDrawPlayer ? &gGame_0x40->mDrawPlayer->mViewCamera : NULL;
}

// Its place as the step left it, put back after the draw (Restore)
void Save(Sprite& sprite)
{
    Saved saved;
    saved.mpSprite = &sprite;
    saved.mPose = PoseOf(sprite);
    saved.mZCol = sprite.mZCol;
    saved.mVisual.Save(sprite.mpVisualShape);
    saved.mCollision.Save(sprite.mpCollisionShape != sprite.mpVisualShape ? sprite.mpCollisionShape : NULL);
    sSavedAt[&sprite - gSpritePool->mPool.mPool] = static_cast<s32>(sSaved.size());
    sSaved.push_back(saved);
}

void ApplySprite(Sprite& sprite, const Pose& prev, f32 alpha)
{
    const Pose cur = PoseOf(sprite);
    if (!cur.mId || cur.mId != prev.mId)
    {
        return;
    }
    const bool moved = cur.mX != prev.mX || cur.mY != prev.mY || cur.mZ != prev.mZ;
    s32 turn = TurnBetween(prev.mRot, cur.mRot);
    if (Abs(turn) > kMaxStepTurn)
    {
        turn = 0;
        sSnapped++;
    }
    if (!moved && !turn)
    {
        return;
    }
    if (Abs(cur.mX - prev.mX) > kMaxStepMove || Abs(cur.mY - prev.mY) > kMaxStepMove || Abs(cur.mZ - prev.mZ) > kMaxStepMove)
    {
        sJumped++;
        return;
    }
    sMoved++;
    Save(sprite);
    sprite.mXy.x.mValue = Lerp(prev.mX, cur.mX, alpha);
    sprite.mXy.y.mValue = Lerp(prev.mY, cur.mY, alpha);
    sprite.mZpos.mValue = Lerp(prev.mZ, cur.mZ, alpha);
    sprite.mRotation.rValue = static_cast<s16>(cur.mRot - turn + Lerp(0, turn, alpha));
    sprite.mRotation.Normalize();
    sprite.InvalidateZColAndBBoxes();
}

// The caches the draw fills (bounding boxes, z layer) are left as a draw at the real positions leaves them.
void RestoreSprite(const Saved& saved)
{
    Sprite& sprite = *saved.mpSprite;
    const bool visualFilled = sprite.mpVisualShape && sprite.mpVisualShape->mBboxUpToDate && !saved.mVisual.UpToDate();
    const bool collisionFilled = saved.mCollision.mpShape && sprite.mpCollisionShape == saved.mCollision.mpShape &&
                                 sprite.mpCollisionShape->mBboxUpToDate && !saved.mCollision.UpToDate();
    const bool zColFilled = saved.mZCol == -1 && sprite.mZCol != -1;

    sprite.mXy.x.mValue = saved.mPose.mX;
    sprite.mXy.y.mValue = saved.mPose.mY;
    sprite.mZpos.mValue = saved.mPose.mZ;
    sprite.mRotation.rValue = saved.mPose.mRot;
    sprite.mZCol = saved.mZCol;
    saved.mVisual.Restore(sprite.mpVisualShape);
    saved.mCollision.Restore(sprite.mpCollisionShape);
    if (visualFilled)
    {
        sprite.mpVisualShape->mBboxUpToDate = false;
        sprite.mpVisualShape->UpdateRotatedBoundingBox(sprite.mXy.x, sprite.mXy.y, sprite.mZpos, sprite.mRotation);
    }
    if (collisionFilled)
    {
        sprite.mpCollisionShape->mBboxUpToDate = false;
        sprite.UpdateCollisionBoundsIfNeeded();
    }
    if (zColFilled)
    {
        sprite.ComputeZLayer();
    }
}
// Where an object attached to a car goes for a car at <x, y, rot>, as AttachedSprite::UpdateAttached puts it
void AttachedPlace(const AttachedSprite& a, Fix16 x, Fix16 y, Ang16 rot, Fix16* pX, Fix16* pY)
{
    *pX = x;
    *pY = y;
    if (a.mOffsetX != Fix16(0) || a.mOffsetY != Fix16(0))
    {
        Fix16 ox = a.mOffsetX;
        Fix16 oy = a.mOffsetY;
        Ang16::RotateVector(ox, oy, rot);
        *pX = x + ox;
        *pY = y + oy;
    }
}

// The objects on a car (its flames, smoke, lights, a roof turret) or a ped (its fire) drawn where it is drawn: lerped on
// their own, an object made this step (drawn where the step put it) would be off it. Not on a car that turned further
// than a car steers in a step (a train's carriage at a corner, whose lights stay put while it is drawn turning)
const s32 kMaxAttachedTurn = 90;

struct Owner
{
    Sprite* mpSprite;
    SpriteList* mpList;
    Pose mAtStep;
};

void ApplyAttached(const std::vector<Owner>& owners)
{
    for (size_t i = 0; i < owners.size(); i++)
    {
        const Sprite* pOwner = owners[i].mpSprite;
        const Pose& step = owners[i].mAtStep;
        const Pose& before = sPoses[pOwner - gSpritePool->mPool.mPool];
        if (before.mId != step.mId || Abs(TurnBetween(before.mRot, step.mRot)) > kMaxAttachedTurn)
        {
            continue;
        }
        Fix16 stepX, stepY;
        stepX.mValue = step.mX;
        stepY.mValue = step.mY;
        Ang16 stepRot;
        stepRot.rValue = step.mRot;
        for (AttachedSprite* a = owners[i].mpList->mP18; a; a = a->mpNext)
        {
            Sprite* pSprite = a->mpSprite;
            if (!pSprite || !pSprite->AsObject2C())
            {
                continue;
            }
            // Only one the step put on it (an object that left it keeps its own place)
            Fix16 x, y;
            AttachedPlace(*a, stepX, stepY, stepRot, &x, &y);
            const s32 slot = static_cast<s32>(pSprite - gSpritePool->mPool.mPool);
            const Saved* pSaved = sSavedAt[slot] >= 0 ? &sSaved[sSavedAt[slot]] : NULL;
            const Pose real = pSaved ? pSaved->mPose : PoseOf(*pSprite);
            if (Abs(real.mX - x.mValue) > 4 || Abs(real.mY - y.mValue) > 4)
            {
                continue;
            }
            if (!pSaved)
            {
                Save(*pSprite);
            }
            sOnOwner[slot] = true;
            AttachedPlace(*a, pOwner->mXy.x, pOwner->mXy.y, pOwner->mRotation, &x, &y);
            pSprite->mXy.x = x;
            pSprite->mXy.y = y;
            pSprite->mZpos = pOwner->mZpos;
            pSprite->mRotation = pOwner->mRotation + a->mOffsetRot;
            pSprite->mRotation.Normalize();
            pSprite->InvalidateZColAndBBoxes();
        }
    }
}

// Where the step left an owner, the place the objects on it were put at
Pose StepPose(const Sprite& sprite)
{
    for (size_t i = 0; i < sCorrected.size(); i++)
    {
        if (sCorrected[i].mpSprite == &sprite)
        {
            return sCorrected[i].mPose;
        }
    }
    return PoseOf(sprite);
}

bool IsCarOrPed(const Sprite& sprite)
{
    return sprite.mSpriteTypeEnum == sprite_types_enum::car_2 || sprite.mSpriteTypeEnum == sprite_types_enum::ped_3;
}

// Where a sprite is drawn from. After a network client's rollback the world before the step was another: a car's or a
// ped's place in it is the one last drawn (the netcode blends their corrections in), anything else's is the corrected
// game's, as the same slot and id may hold another object there (an explosion's particle where a flame's was)
const Pose& FromPose(const Sprite& sprite, s32 i)
{
    return sResimValid && !IsCarOrPed(sprite) ? sResimPoses[i] : sPoses[i];
}

// GTA2_LOOP_TRACE's objs: the objects drawn other than between their places before and after the step in the game as
// it is now (after a rollback, the corrected one), and the worst of them (blocks)
void Measure(f32 alpha)
{
    sObjOff = 0;
    sObjWorst = 0;
    for (s32 i = 0; sMeasure && i < kNumSprites; i++)
    {
        const Sprite& sprite = gSpritePool->mPool.mPool[i];
        if (!sprite.mInstanceId || IsCarOrPed(sprite) || sOnOwner[i])
        {
            continue;
        }
        const Pose cur = sSavedAt[i] >= 0 ? sSaved[sSavedAt[i]].mPose : PoseOf(sprite);
        const Pose& from = sResimValid ? sResimPoses[i] : sPoses[i];
        s32 x = cur.mX, y = cur.mY;
        if (from.mId == cur.mId && Abs(cur.mX - from.mX) <= kMaxStepMove && Abs(cur.mY - from.mY) <= kMaxStepMove &&
            Abs(cur.mZ - from.mZ) <= kMaxStepMove)
        {
            x = Lerp(from.mX, cur.mX, alpha);
            y = Lerp(from.mY, cur.mY, alpha);
        }
        const f32 dx = static_cast<f32>(sprite.mXy.x.mValue - x), dy = static_cast<f32>(sprite.mXy.y.mValue - y);
        const f32 off = sqrtf(dx * dx + dy * dy) / 16384.0f;
        if (off > 1.0f / 16)
        {
            sObjOff++;
            sObjWorst = off > sObjWorst ? off : sObjWorst;
        }
    }
}
// After the sprites are placed for the draw: each sliding shot between its shooter's drawn gun and its drawn place
void ApplySlides()
{
    for (size_t i = 0; i < sSlides.size();)
    {
        Slide& slide = sSlides[i];
        Sprite& sprite = *slide.mpSprite;
        if (sprite.mInstanceId != slide.mId || slide.mpFrom->mInstanceId != slide.mFromId || (slide.mStart >= 0 && sDrawTime - slide.mStart >= kSlideSteps))
        {
            sSlides.erase(sSlides.begin() + static_cast<std::ptrdiff_t>(i));
            continue;
        }
        const Sprite& from = *slide.mpFrom;
        const f32 gx = from.mXy.x.ToFloat() + slide.mMuzzle * Ang16::sine(from.mRotation).ToFloat();
        const f32 gy = from.mXy.y.ToFloat() + slide.mMuzzle * Ang16::cosine(from.mRotation).ToFloat();
        const f32 gz = from.mZpos.ToFloat();
        const f32 x = sprite.mXy.x.ToFloat(), y = sprite.mXy.y.ToFloat(), z = sprite.mZpos.ToFloat();
        const bool first = slide.mStart < 0;
        if (first)
        {
            slide.mStart = sDrawTime;
        }
        const f32 f = slide.mSlide ? static_cast<f32>((sDrawTime - slide.mStart) / kSlideSteps) : 1.0f;
        if (f < 1.0f)
        {
            const s32 at = static_cast<s32>(&sprite - gSpritePool->mPool.mPool);
            if (sSavedAt[at] < 0)
            {
                Save(sprite);
            }
            sprite.mXy.x = Fix16(gx + (x - gx) * f);
            sprite.mXy.y = Fix16(gy + (y - gy) * f);
            sprite.mZpos = Fix16(gz + (z - gz) * f);
            sprite.InvalidateZColAndBBoxes();
        }
        if (first && sSlideMeasures.size() < 256)
        {
            const f32 dx = sprite.mXy.x.ToFloat() - gx, dy = sprite.mXy.y.ToFloat() - gy;
            sSlideMeasures.push_back(std::make_pair(sqrtf((x - gx) * (x - gx) + (y - gy) * (y - gy)), sqrtf(dx * dx + dy * dy)));
        }
        i++;
    }
}
} // namespace

void Snapshot()
{
    sSteps++;
    sValid = gSpritePool && ViewCamera();
    if (!sValid)
    {
        return;
    }
    for (s32 i = 0; i < kNumSprites; i++)
    {
        sPoses[i] = PoseOf(gSpritePool->mPool.mPool[i]);
    }
    sResimValid = false;
    sCorrected.clear();
    spCamera = ViewCamera();
    sCameraPos = spCamera->mCurPos;
    for (s32 i = 0; gHud_2B00 && i < kNumArrows; i++)
    {
        sArrows[i] = PoseOf(ArrowAt(i));
    }
}

void SnapshotResim()
{
    sResimValid = sValid && gSpritePool;
    sCorrected.clear();
    for (s32 i = 0; sResimValid && i < kNumSprites; i++)
    {
        sResimPoses[i] = PoseOf(gSpritePool->mPool.mPool[i]);
    }
}

void Clear()
{
    sValid = false;
    sSlides.clear();
}

void SlideFrom(Sprite* pSprite, const Sprite* pFrom, f32 muzzle, bool slide)
{
    if (pSprite && pFrom && sSlides.size() < 128)
    {
        Slide s = {pSprite, pFrom, pSprite->mInstanceId, pFrom->mInstanceId, muzzle, slide, -1.0};
        sSlides.push_back(s);
    }
}

bool TakeSlideMeasure(f32* pWouldBe, f32* pDrawn)
{
    if (sSlideMeasures.empty())
    {
        return false;
    }
    *pWouldBe = sSlideMeasures.back().first;
    *pDrawn = sSlideMeasures.back().second;
    sSlideMeasures.pop_back();
    return true;
}

f64 DrawTime()
{
    return sDrawTime;
}

void LaidMoving(Sprite* pSprite, s32 dx, s32 dy)
{
    if (!sValid || !gSpritePool)
    {
        return;
    }
    const s32 i = static_cast<s32>(pSprite - gSpritePool->mPool.mPool);
    if (i < 0 || i >= kNumSprites)
    {
        return;
    }
    Pose from = PoseOf(*pSprite);
    from.mX -= dx;
    from.mY -= dy;
    sPoses[i] = from;
    // After a rollback an object is drawn from the corrected game's place (FromPose)
    if (sResimValid)
    {
        sResimPoses[i] = from;
    }
}

void Correcting(const Sprite* pSprite)
{
    for (size_t i = 0; pSprite && i < sCorrected.size(); i++)
    {
        if (sCorrected[i].mpSprite == pSprite)
        {
            return;
        }
    }
    if (pSprite)
    {
        Corrected c = {pSprite, PoseOf(*pSprite)};
        sCorrected.push_back(c);
    }
}

void Apply(f32 alpha)
{
    sSaved.clear();
    memset(sSavedAt, 0xFF, sizeof(sSavedAt));
    memset(sOnOwner, 0, sizeof(sOnOwner));
    spAppliedCamera = NULL;
    sDrawTime = sSteps + static_cast<f64>(alpha);
    if (!sValid || !gSpritePool || ViewCamera() != spCamera)
    {
        return;
    }
    // Each player's ped and where its sprite is at the step, before the sprites move
    struct PlayerSprite
    {
        Ped* mpPed;
        Sprite* mpSprite;
        Pose mPose;
    };
    PlayerSprite players[MAX_PLAYERS];
    s32 numPlayers = 0;
    sSavedPeds.clear();
    for (s32 i = 0; i < gGame_0x40->mNumPlayers && i < MAX_PLAYERS; i++)
    {
        Player* pPlayer = gGame_0x40->mPlayers[i];
        Ped* pPed = pPlayer ? pPlayer->mPlayerPed : NULL;
        Sprite* pSprite = pPed && (pPed->mCar || pPed->mGameObject) ? pPed->GetSprite() : NULL;
        if (pSprite)
        {
            PlayerSprite p = {pPed, pSprite, PoseOf(*pSprite)};
            players[numPlayers++] = p;
        }
    }
    std::vector<Owner> owners;
    for (Car* pCar = gCar_BC_Pool ? gCar_BC_Pool->mPool.mpPrev : NULL; pCar; pCar = pCar->mpNext)
    {
        if (pCar->mCarSprite && pCar->mAttachments.mP18)
        {
            Owner owner = {pCar->mCarSprite, &pCar->mAttachments, StepPose(*pCar->mCarSprite)};
            owners.push_back(owner);
        }
    }
    for (s32 i = 0; i < kNumSprites; i++)
    {
        Sprite& sprite = gSpritePool->mPool.mPool[i];
        Char* pChar = sprite.mInstanceId ? sprite.AsCharB4() : NULL;
        if (pChar && pChar->mObj2c.mP18)
        {
            Owner owner = {&sprite, &pChar->mObj2c, StepPose(sprite)};
            owners.push_back(owner);
        }
    }
    for (s32 i = 0; i < kNumSprites; i++)
    {
        Sprite& sprite = gSpritePool->mPool.mPool[i];
        ApplySprite(sprite, FromPose(sprite, i), alpha);
    }
    ApplyAttached(owners);
    Measure(alpha);
    ApplySlides();
    for (s32 i = 0; i < numPlayers; i++)
    {
        const PlayerSprite& p = players[i];
        const s32 dx = p.mpSprite->mXy.x.mValue - p.mPose.mX, dy = p.mpSprite->mXy.y.mValue - p.mPose.mY;
        const s32 dz = p.mpSprite->mZpos.mValue - p.mPose.mZ;
        if (dx || dy || dz)
        {
            SavedPed saved = {p.mpPed, p.mpPed->mCam};
            sSavedPeds.push_back(saved);
            p.mpPed->mCam.x.mValue += dx;
            p.mpPed->mCam.y.mValue += dy;
            p.mpPed->mCam.z.mValue += dz;
        }
    }

    Camera* pCamera = spCamera;
    const CameraPos cur = pCamera->mCurPos;
    const CameraPos& prev = sCameraPos;
    if (Abs(cur.mX.mValue - prev.mX.mValue) <= kMaxCameraStepMove && Abs(cur.mY.mValue - prev.mY.mValue) <= kMaxCameraStepMove &&
        Abs(cur.mZ.mValue - prev.mZ.mValue) <= kMaxCameraStepMove)
    {
        memcpy(sCameraBytes, pCamera, sizeof(Camera));
        spAppliedCamera = pCamera;
        pCamera->mCurPos.mX.mValue = Lerp(prev.mX.mValue, cur.mX.mValue, alpha);
        pCamera->mCurPos.mY.mValue = Lerp(prev.mY.mValue, cur.mY.mValue, alpha);
        pCamera->mCurPos.mZ.mValue = Lerp(prev.mZ.mValue, cur.mZ.mValue, alpha);
        pCamera->mCurPos.mZoom.mValue = Lerp(prev.mZoom.mValue, cur.mZoom.mValue, alpha);
        pCamera->UpdateBoundaries();
    }

    // The HUD's arrows: their screen spot and angle are worked out once a step, from the player and the camera.
    sArrowsApplied = gHud_2B00 != NULL;
    const s32 height = pCamera->mScreenPxHeight > 0 ? pCamera->mScreenPxHeight : kArrowScreenHeight;
    const s32 maxArrowMove = static_cast<s32>(static_cast<long long>(kMaxArrowStepMove) * height / kArrowScreenHeight);
    for (s32 i = 0; sArrowsApplied && i < kNumArrows; i++)
    {
        const ArrowRef arrow = ArrowAt(i);
        const ArrowPose cur = PoseOf(arrow);
        const ArrowPose& prev = sArrows[i];
        const s32 turn = TurnBetween(prev.mRot, cur.mRot);
        sArrowsDrawn[i] = cur;
        if (prev.mShown && cur.mShown && Abs(cur.mX - prev.mX) <= maxArrowMove && Abs(cur.mY - prev.mY) <= maxArrowMove && Abs(turn) <= kMaxStepTurn)
        {
            arrow.mpX->mValue = Lerp(prev.mX, cur.mX, alpha);
            arrow.mpY->mValue = Lerp(prev.mY, cur.mY, alpha);
            arrow.mpRot->rValue = static_cast<s16>(cur.mRot - turn + Lerp(0, turn, alpha));
            arrow.mpRot->Normalize();
        }
    }
}

void Restore()
{
    for (s32 i = 0; sArrowsApplied && i < kNumArrows; i++)
    {
        const ArrowRef arrow = ArrowAt(i);
        arrow.mpX->mValue = sArrowsDrawn[i].mX;
        arrow.mpY->mValue = sArrowsDrawn[i].mY;
        arrow.mpRot->rValue = sArrowsDrawn[i].mRot;
    }
    sArrowsApplied = false;
    if (spAppliedCamera)
    {
        memcpy(spAppliedCamera, sCameraBytes, sizeof(Camera));
        spAppliedCamera = NULL;
    }
    for (size_t i = sSaved.size(); i-- > 0;)
    {
        RestoreSprite(sSaved[i]);
    }
    sSaved.clear();
    for (size_t i = 0; i < sSavedPeds.size(); i++)
    {
        sSavedPeds[i].mpPed->mCam = sSavedPeds[i].mCam;
    }
    sSavedPeds.clear();
}

bool Steered(const Sprite* pSprite)
{
    const s32 i = gSpritePool ? static_cast<s32>(pSprite - gSpritePool->mPool.mPool) : -1;
    if (i < 0 || i >= kNumSprites)
    {
        return true;
    }
    // In a draw the sprite is drawn turned part of the way: its place at the step is the one saved
    s32 rot = pSprite->mRotation.rValue;
    for (size_t k = 0; k < sSaved.size(); k++)
    {
        rot = sSaved[k].mpSprite == pSprite ? sSaved[k].mPose.mRot : rot;
    }
    return sPoses[i].mId != pSprite->mInstanceId || Abs(TurnBetween(sPoses[i].mRot, rot)) <= kMaxAttachedTurn;
}

void ObjectCounts(u32* pOff, f32* pWorst)
{
    *pOff = sObjOff;
    *pWorst = sObjWorst;
}

void TakeCounts(u32* pMoved, u32* pJumped, u32* pSnapped)
{
    *pMoved = sMoved;
    *pJumped = sJumped;
    *pSnapped = sSnapped;
    sMoved = 0;
    sJumped = 0;
    sSnapped = 0;
}
} // namespace PortInterp
