#include "laser.hpp"
#include "Camera.hpp"
#include "Car.hpp"
#include "CollisionSoundQueue.hpp"
#include "CrimeReportQueue.hpp"
#include "Fix16_Point.hpp"
#include "Object_5C.hpp"
#include "ParticleManager.hpp"
#include "Ped.hpp"
#include "Player.hpp"
#include "PlayerCrimeStats.hpp"
#include "Weapon.hpp"
#include "char.hpp"
#include "debug.hpp"
#include "enums.hpp"
#include "ped_death_cause.hpp"
#include "sprite.hpp"
#include "winmain.hpp"
#include "frame_loop.hpp"
#include "view.hpp"
#include <math.h>

#if defined(GTA2_PORT_SDL)
void port_DrawParticles(s32 slot, s32 width, s32 height, const u32* pRgba, u32 version, const f32* pCorners, const u32* pColours,
                        s32 count, bool additive);
void port_DrawPicture(s32 slot, s32 width, s32 height, const u32* pRgba, const f32* pXyuv, const u32* pColours, s32 count, bool nearest);
float port_PixelScale();
#endif

namespace PortLaser
{
bool gCheat;

namespace
{
// In logic steps: a pulse every other step, bursts of about 2.5 seconds, 1.5 seconds to cool down.
const u8 kPulseSteps = 2;
const s32 kBurstPulses = 38;
const u8 kCooldownSteps = 45;
// Per pulse; a pistol bullet takes 25 from a ped, a machine gun bullet 800 from a car.
const s16 kPedDamage = 15;
const s16 kCarDamage = 1000;
const f32 kRange = 8.0f;
// The gun's tip in the shooter's sprite, as drawn when holding it.
const f32 kMuzzleX = -0.03f;
const f32 kMuzzleY = 0.14f;
// Seconds a pulse is drawn, fading.
const f32 kLife = 0.09f;

struct Shot
{
    Ped* mpPed;
    s32 mPedId;
    f32 mDx, mDy, mLength, mZ;
    f64 mBorn;
};
const s32 kMaxShots = 8;
Shot sShots[kMaxShots];
s32 sNextShot;

void Record(Ped* pPed, Ang16 aim, f32 length)
{
    Shot& s = sShots[sNextShot];
    sNextShot = (sNextShot + 1) % kMaxShots;
    s.mpPed = pPed;
    s.mPedId = pPed->mId;
    s.mDx = Ang16::sine(aim).ToFloat();
    s.mDy = Ang16::cosine(aim).ToFloat();
    s.mLength = length;
    s.mZ = pPed->get_cam_z().ToFloat();
    s.mBorn = PortFrameLoop::WorldSeconds();
}

// The game's hits, as ChainShockTargets deals them for the electro gun, with a bullet's death cause.
void HitPed(Ped* pShooter, Ped* pTarget, bool effects, Ang16 aim)
{
    pTarget->SetAttacker(pShooter);
    pTarget->mKillerId = pShooter->mId;
    pTarget->mDeathCause = ped_death_cause::bullet_11;
    pTarget->mKillerIdTimer = 50;
    pTarget->TakeDamage(static_cast<s16>(kPedDamage * GetDamageMultiplier(pShooter)));
    if (effects)
    {
        gParticle_8->EmitBloodBurst(pTarget->get_cam_x(), pTarget->get_cam_y(), pTarget->get_cam_z(), aim);
    }
}

void HitCar(Ped* pShooter, Car* pCar, const Fix16_Point& at)
{
    if (pCar->IsFlagSet(car_flag::Bulletproof))
    {
        return;
    }
    pCar->mExploderPedId = pShooter->mId;
    pCar->mDeathCause = ped_death_cause::bullet_11;
    pCar->mExploderTimer = 50;
    const s16 damage = pCar->AccumulateDamage(static_cast<s16>(kCarDamage * GetDamageMultiplier(pShooter)), &at);
    pCar->TriggerCarAlarm();
    if (pShooter->PedTypeIs(ped_type::player_2) && damage > 0)
    {
        pShooter->mPlayer->mScores.AwardCarDamageScoreHit(pCar, 1);
    }
}

// Steps the game's collision probe along the aim, as TraceBeamToHitTarget does the electro baton's beam, to the
// first wall, car or living ped; returns the beam's length in blocks.
f32 Trace(Ped* pPed, Fix16 x, Fix16 y, Fix16 z, Ang16 aim, bool effects)
{
    const Fix16 stepLen(0.1);
    const s32 steps = static_cast<s32>(kRange / 0.1f);
    Sprite* pProbe = gObject_5C->mCollisionProbeSprite;
    gCollisionTarget.Reset();
    pProbe->set_xyz_lazy(x, y, z);
    pProbe->set_ang_lazy(aim);
    pProbe->AllocCollisionShape(stepLen, stepLen, stepLen);
    Fix16_Point step;
    step.FromPolar_41E210(stepLen, aim);
    s32 i = 0;
    for (; i < steps; i++)
    {
        pProbe->set_xy_lazy(pProbe->mXy.x + step.x, pProbe->mXy.y + step.y);
        if (pProbe->CheckBulletWallCollision())
        {
            if (effects)
            {
                gParticle_8->EmitImpactParticles(pProbe->mXy.x, pProbe->mXy.y, z, Fix16_Point(-step.x, -step.y));
            }
            break;
        }
        Sprite* pHit = pProbe->QuerySpriteCollision(sprite_types_enum::ped_3);
        if (!pHit)
        {
            continue;
        }
        if (pHit->mSpriteTypeEnum == sprite_types_enum::car_2)
        {
            HitCar(pPed, pHit->mCarBcPtr, Fix16_Point(pProbe->mXy.x, pProbe->mXy.y));
            if (effects)
            {
                gParticle_8->EmitImpactParticles(pProbe->mXy.x, pProbe->mXy.y, z, Fix16_Point(-step.x, -step.y));
            }
            break;
        }
        if (pHit->mSpriteTypeEnum == sprite_types_enum::ped_3)
        {
            Ped* pTarget = pHit->mCharB4Ptr->mpPed;
            const s32 state = pTarget->mPedState1;
            if (pTarget != pPed && state != ped_state_1::immobilized_8 && state != ped_state_1::dead_9)
            {
                HitPed(pPed, pTarget, effects, aim);
                break;
            }
        }
    }
    return (i + 1) * 0.1f;
}

// The beam across its width: a white-hot core in a red glow, premultiplied for additive drawing; its alpha covers
// what is under the beam, so that it stays red on a bright ground.
const s32 kTexW = 2;
const s32 kTexH = 32;
u32 sTexture[kTexW * kTexH];

void MakeTexture()
{
    for (s32 v = 0; v < kTexH; v++)
    {
        const f32 d = fabsf((v + 0.5f) / kTexH - 0.5f) * 2.0f;
        const f32 glow = (1.0f - d) * (1.0f - d);
        const f32 core = d < 0.08f ? 1.0f : (d < 0.28f ? (0.28f - d) / 0.2f : 0.0f);
        const u32 r = static_cast<u32>(fminf(255.0f, 255.0f * glow + 255.0f * core));
        const u32 g = static_cast<u32>(fminf(255.0f, 10.0f * glow + 235.0f * core * core));
        const u32 b = static_cast<u32>(fminf(255.0f, 10.0f * glow + 225.0f * core * core));
        const u32 a = static_cast<u32>(fminf(255.0f, 340.0f * glow + 255.0f * core));
        for (s32 u = 0; u < kTexW; u++)
        {
            sTexture[v * kTexW + u] = r | g << 8 | b << 16 | a << 24;
        }
    }
}

s32 LayerOf(f32 z)
{
    const s32 layer = static_cast<s32>(floorf(z));
    return layer < 1 ? 1 : (layer > 7 ? 7 : layer);
}

// The HUD icon, drawn by hand: a grey rifle with a red energy cell and emitter.
const s32 kIconW = 40;
const s32 kIconH = 13;
const char* const kIcon[kIconH] = {
    "........................................",
    "........kkkkkkkkkkkkkkkkkkkk............",
    ".......kllllllllllllllllllllk...........",
    "kkkk..kggggggggggggggggggggggkkkkkkkkkk.",
    "kdgk.kggdkkkkkkkkkdgggggggggglllllllkkkk",
    "kdgkkkggdkrRrRrRrkdggggggggggddddddkRRRk",
    "kdggggggdkrrrrrrrkdggggggggggkkkkkkkrRRk",
    "kddgggggdkkkkkkkkkdgggggkkkkk......kkkk.",
    ".kddddddddddddddddddddk.................",
    "..kkkkkkkkkkkkkkdgggdk..................",
    "...............kdgggk...................",
    "...............kddddk...................",
    "...............kkkkk....................",
};
u32 sIcon[kIconW * kIconH];

void MakeIcon()
{
    for (s32 y = 0; y < kIconH; y++)
    {
        for (s32 x = 0; x < kIconW; x++)
        {
            u32 rgb;
            switch (kIcon[y][x])
            {
                case 'k': rgb = 0x181820; break;
                case 'd': rgb = 0x40444C; break;
                case 'g': rgb = 0x74787F; break;
                case 'l': rgb = 0xB0B4BC; break;
                case 'r': rgb = 0xD01818; break;
                case 'R': rgb = 0xFF7868; break;
                default: rgb = 0; break;
            }
            // RGBA bytes.
            sIcon[y * kIconW + x] = rgb ? ((rgb >> 16) & 0xFF) | (rgb & 0xFF00) | ((rgb & 0xFF) << 16) | 0xFF000000u : 0;
        }
    }
}
} // namespace

bool Granted()
{
    return (gCheat || bGet_all_weapons) && !bStartNetworkGame;
}

void Fire(Weapon* pWeapon)
{
    Ped* pPed = pWeapon->mpPed;
    // mUnused8, which the game clears (Weapon::init) and never reads: the pulses fired in this burst.
    if (pWeapon->mReloadTimer)
    {
        pWeapon->mReloadTimer--;
        // The electro gun's looping sound goes on between the pulses of a burst.
        pWeapon->SetShotFired(pWeapon->mUnused8 != 0);
        return;
    }
    if (++pWeapon->mUnused8 >= kBurstPulses)
    {
        pWeapon->mUnused8 = 0;
        pWeapon->mReloadTimer = kCooldownSteps;
    }
    else
    {
        pWeapon->mReloadTimer = kPulseSteps - 1;
    }
    pWeapon->ApplyFastReload();
    pWeapon->SetShotFired(true);

    const Ang16 aim = pPed->ComputeAimAngle();
    Fix16_Point muzzle;
    muzzle.x = Fix16(static_cast<f64>(kMuzzleX));
    muzzle.y = Fix16(static_cast<f64>(kMuzzleY));
    muzzle.RotateByAngle(pPed->mGameObject->mSpritePtr->mRotation);
    const bool effects = pWeapon->mUnused8 % 3 == 1;
    const f32 length = Trace(pPed, pPed->get_cam_x() + muzzle.x, pPed->get_cam_y() + muzzle.y, pPed->get_cam_z(), aim, effects);
    Record(pPed, aim, length);

    if (pPed->PedTypeIs(ped_type::player_2))
    {
        pWeapon->DecreaseAmmo();
    }
    pPed->AddThreateningPedToList();
    if (pPed->is_player())
    {
        gCrimeReportQueue->ReportCrimeForPed(crime_stats_type::weapon_fired_2, pPed);
    }
}

s32 GetBeams(Beam* pOut, s32 max)
{
    const f64 now = PortFrameLoop::WorldSeconds();
    s32 count = 0;
    for (s32 i = 0; i < kMaxShots && count < max; i++)
    {
        const Shot& s = sShots[i];
        const f32 age = static_cast<f32>(now - s.mBorn);
        if (!s.mpPed || age < 0.0f || age >= kLife || s.mpPed->mId != s.mPedId || !s.mpPed->mGameObject)
        {
            continue;
        }
        // From the shooter's sprite as drawn (interpolated), so that the beam stays on the gun.
        const Sprite* pSprite = s.mpPed->mGameObject->mSpritePtr;
        const f32 sx = Ang16::sine(pSprite->mRotation).ToFloat();
        const f32 sy = Ang16::cosine(pSprite->mRotation).ToFloat();
        Beam& b = pOut[count++];
        b.mX0 = pSprite->mXy.x.ToFloat() + kMuzzleX * sy + kMuzzleY * sx;
        b.mY0 = pSprite->mXy.y.ToFloat() - kMuzzleX * sx + kMuzzleY * sy;
        b.mX1 = b.mX0 + s.mDx * s.mLength;
        b.mY1 = b.mY0 + s.mDy * s.mLength;
        b.mZ = s.mZ;
        b.mFade = 1.0f - age / kLife;
    }
    return count;
}

void DrawLayer(s32 layer)
{
#if defined(GTA2_PORT_SDL)
    static bool sTextureMade;
    Beam beams[kMaxShots];
    const s32 count = GetBeams(beams, kMaxShots);
    f32 corners[kMaxShots * 28];
    f32 glowCorners[kMaxShots * 28];
    u32 glows[kMaxShots];
    u32 colours[kMaxShots];
    u32 covers[kMaxShots];
    s32 quads = 0;
    for (s32 i = 0; i < count; i++)
    {
        const Beam& b = beams[i];
        if (LayerOf(b.mZ) != layer)
        {
            continue;
        }
        f32 ax, ay, bx, by, ex, ey;
        PortView::GroundToScreen(b.mX0, b.mY0, b.mZ, &ax, &ay);
        PortView::GroundToScreen(b.mX1, b.mY1, b.mZ, &bx, &by);
        PortView::GroundToScreen(b.mX0 + 1.0f, b.mY0, b.mZ, &ex, &ey);
        const f32 len = sqrtf((bx - ax) * (bx - ax) + (by - ay) * (by - ay));
        if (len < 1.0f)
        {
            continue;
        }
        // Twelve of the game's texels wide (a block is 64), as many screen pixels as they cover.
        const f32 half = fmaxf((ex - ax) * 6.0f / 64.0f, 3.0f);
        const f32 nx = -(by - ay) / len * half;
        const f32 ny = (bx - ax) / len * half;
        const f32 sx[4] = {ax + nx, bx + nx, bx - nx, ax - nx};
        const f32 sy[4] = {ay + ny, by + ny, by - ny, ay - ny};
        // A faint wide glow round the halo: the same texture, three times as wide.
        const f32 gx[4] = {ax + 3.0f * nx, bx + 3.0f * nx, bx - 3.0f * nx, ax - 3.0f * nx};
        const f32 gy[4] = {ay + 3.0f * ny, by + 3.0f * ny, by - 3.0f * ny, ay - 3.0f * ny};
        const f32 wx[4] = {b.mX0, b.mX1, b.mX1, b.mX0};
        const f32 wy[4] = {b.mY0, b.mY1, b.mY1, b.mY0};
        for (s32 k = 0; k < 4; k++)
        {
            f32* o = &corners[(quads * 4 + k) * 7];
            o[0] = sx[k];
            o[1] = sy[k];
            o[2] = k == 1 || k == 2 ? 1.0f : 0.0f;
            o[3] = k >= 2 ? 1.0f : 0.0f;
            o[4] = wx[k];
            o[5] = wy[k];
            o[6] = b.mZ;
            f32* g = &glowCorners[(quads * 4 + k) * 7];
            memcpy(g, o, 7 * sizeof(f32));
            g[0] = gx[k];
            g[1] = gy[k];
        }
        const u32 v = static_cast<u32>(255.0f * b.mFade);
        covers[quads] = static_cast<u32>(200.0f * b.mFade) << 24 | 0xFFFFFFu;
        const u32 w = static_cast<u32>(70.0f * b.mFade);
        glows[quads] = 0xFF000000u | w << 16 | w << 8 | w;
        colours[quads++] = 0xFF000000u | v << 16 | v << 8 | v;
    }
    if (!quads)
    {
        return;
    }
    if (!sTextureMade)
    {
        MakeTexture();
        sTextureMade = true;
    }
    port_DrawParticles(2, kTexW, kTexH, sTexture, 1, corners, covers, quads, false);
    port_DrawParticles(2, kTexW, kTexH, sTexture, 1, glowCorners, glows, quads, true);
    port_DrawParticles(2, kTexW, kTexH, sTexture, 1, corners, colours, quads, true);
#else
    (void)layer;
#endif
}

bool DrawHudIcon(s16 weapon_idx, s32 right, s32 top)
{
#if defined(GTA2_PORT_SDL)
    if (weapon_idx != kWeapon)
    {
        return false;
    }
    static bool sIconMade;
    if (!sIconMade)
    {
        MakeIcon();
        sIconMade = true;
    }
    // A whole number of window pixels a texel, near the HUD's scale.
    const f32 pixels = port_PixelScale();
    const f32 times = fmaxf(floorf(gViewCamera->mUiScale.ToFloat() * pixels + 0.5f), 1.0f);
    const f32 w = kIconW * times / pixels;
    const f32 h = kIconH * times / pixels;
    const f32 left = floorf((UI_X(Fix16(right)).ToFloat() - w) * pixels + 0.5f) / pixels;
    const f32 up = floorf(UI_Y(Fix16(top)).ToFloat() * pixels + 0.5f) / pixels;
    const f32 xyuv[24] = {left, up, 0, 0, left + w, up, 1, 0, left + w, up + h, 1, 1,
                          left, up, 0, 0, left + w, up + h, 1, 1, left, up + h, 0, 1};
    const u32 colours[6] = {0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu};
    port_DrawPicture(4, kIconW, kIconH, sIcon, xyuv, colours, 6, true);
    return true;
#else
    (void)weapon_idx;
    (void)right;
    (void)top;
    return false;
#endif
}
} // namespace PortLaser
