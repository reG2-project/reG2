#include "fire_smoke.hpp"
#include "Explosion.hpp"
#include "Game.hpp"
#include "Object_5C.hpp"
#include "ObjectDefinitions.hpp"
#include "enums.hpp"
#include "gtx_0x106C.hpp"
#include "day_night.hpp"
#include "explosion_type.hpp"
#include "frame_loop.hpp"
#include "lights.hpp"
#include "settings.hpp"
#include "shot_flash.hpp"
#include "sprite.hpp"
#include "view.hpp"
#include <math.h>
#include <string.h>

#if defined(GTA2_PORT_SDL)
void port_ClearSmokeLights();
void port_DrawParticles(s32 slot, s32 width, s32 height, const u32* pRgba, u32 version, const f32* pCorners, const u32* pColours,
                        s32 count, bool additive);
#endif

namespace PortFireSmoke
{
namespace
{
// The game has no smoke over its fires: its rising smoke particles (Particle_4C state 6) use a 1x1 placeholder
// sprite, and only the flames' last frames turn into small grey puffs. Those frames are the wisps here: the
// smoke is many of them, drawn at the game's texel density. All sizes in blocks, times in seconds.

// How a kind of fire smokes: wisps a second at full strength, the share of them that are large billows, their
// size, life and opacity, embers a second, the spread round the source, its height above the fire's sprite, and
// how fast the strength ramps in and dies away.
struct Kind
{
    f32 mRate, mBig, mScale, mLife, mAlpha, mEmbers, mSpread, mHeight, mRampIn, mRampOut;
};
// Burning car (Car::SpawnFire) and wreck; the fire levels of a damaged car; fires; molotov flames; blasts.
const Kind kCarFire = {8.0f, 0.30f, 1.1f, 3.4f, 0.46f, 3.5f, 0.30f, 0.40f, 0.3f, 4.0f};
const Kind kCarFire1 = {12.0f, 0.00f, 1.0f, 2.8f, 0.80f, 0.5f, 0.10f, 0.20f, 0.3f, 2.0f};
const Kind kCarFire2 = {14.0f, 0.10f, 1.0f, 3.0f, 0.80f, 0.9f, 0.13f, 0.25f, 0.3f, 2.5f};
const Kind kCarFire3 = {16.0f, 0.15f, 1.0f, 3.1f, 0.78f, 1.5f, 0.16f, 0.25f, 0.3f, 3.0f};
const Kind kFire = {8.0f, 0.25f, 1.0f, 3.0f, 0.50f, 1.8f, 0.28f, 0.25f, 0.3f, 3.0f};
const Kind kMolotov = {4.0f, 0.00f, 0.8f, 2.2f, 0.45f, 0.6f, 0.20f, 0.15f, 0.6f, 1.5f};
const Kind kBlast = {12.0f, 0.45f, 1.2f, 3.4f, 0.50f, 0.0f, 0.55f, 0.35f, 0.1f, 0.5f};

const f32 kRise = 0.65f; // blocks a second at birth, slowing down
// Blocks a second: up the screen, as the game's own flame and smoke particles drift when their fire stands still
// (Particle_4C, 0.01 to 0.05 blocks a step), so that the smoke leans the way the flames and the heat haze do.
const f32 kWindX = 0.05f, kWindY = -0.30f;
const f32 kTrail = 5.0f; // wisps a block a moving fire adds
const f32 kTurbulence = 0.35f; // blocks a wisp is pushed about by the moving noise, at the end of its life
const f32 kEmberLift = 1.1f; // blocks a second squared pulling an ember's climb back

const s32 kMaxEmitters = 48;
const s32 kMaxPuffs = 256;
const s32 kMaxEmbers = 64;

struct Emitter
{
    const void* mpKey;
    s32 mId;
    const Kind* mpKind;
    f32 mX, mY, mZ;
    f32 mTarget, mLevel;
    f32 mPuffs, mEmbers; // puffs and embers due, in parts of the next one's share
    f32 mMoved; // blocks the fire moved since the last emission
    u32 mSeed, mCount;
    bool mSeen;
};

struct Puff
{
    f32 mX, mY, mZ;
    f64 mBorn;
    f32 mLife, mScale, mAlpha, mRise, mGlow;
    u32 mSeed;
    bool mBig;
};

struct Ember
{
    f32 mX, mY, mZ, mVx, mVy, mVz;
    f64 mBorn;
    f32 mLife;
    u32 mSeed;
};

Emitter sEmitters[kMaxEmitters];
s32 sNumEmitters;
Puff sPuffs[kMaxPuffs];
s32 sNumPuffs;
Ember sEmbers[kMaxEmbers];
s32 sNumEmbers;
f64 sLastSeconds = -1.0;

// Hashed value in [0, 1) for item i of a seed.
f32 Hash(u32 seed, u32 i)
{
    u32 h = seed * 0x9E3779B1u ^ i * 0x85EBCA77u;
    h ^= h >> 15;
    h *= 0x2C1B3C6Du;
    h ^= h >> 12;
    h *= 0x297A2D39u;
    h ^= h >> 15;
    return (h & 0xFFFFFF) / 16777216.0f;
}

f32 Clamp01(f32 v)
{
    return v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v);
}

f32 Smooth(f32 e0, f32 e1, f32 v)
{
    const f32 t = Clamp01((v - e0) / (e1 - e0));
    return t * t * (3.0f - 2.0f * t);
}

// Smooth value noise in [0, 1] over x, y and t, a lattice cell apart.
f32 Noise3(u32 seed, f32 x, f32 y, f32 t)
{
    const f32 fx = floorf(x), fy = floorf(y), ft = floorf(t);
    const s32 ix = static_cast<s32>(fx), iy = static_cast<s32>(fy), it = static_cast<s32>(ft);
    const f32 sx = (x - fx) * (x - fx) * (3.0f - 2.0f * (x - fx)), sy = (y - fy) * (y - fy) * (3.0f - 2.0f * (y - fy));
    const f32 st = (t - ft) * (t - ft) * (3.0f - 2.0f * (t - ft));
    f32 v[8];
    for (s32 k = 0; k < 8; k++)
    {
        v[k] = Hash(seed ^ static_cast<u32>(it + (k >> 2)) * 0x632BE5ABu, static_cast<u32>(ix + (k & 1)) * 73856093u ^
                                                                             static_cast<u32>(iy + ((k >> 1) & 1)) * 19349663u);
    }
    for (s32 k = 0; k < 4; k++)
    {
        v[k] += (v[k + 4] - v[k]) * st;
    }
    const f32 a = v[0] + (v[1] - v[0]) * sx, b = v[2] + (v[3] - v[2]) * sx;
    return a + (b - a) * sy;
}

// The wisps: the game's own smoke frames from the loaded style, the grey puffs the car fires' flames end in
// (code_obj + 103 to 109) and the dark billows the burning wrecks' flames end in (+ 157 to 160), each in a cell of
// 34 texels, colours from their palette greyed down to a dusty grey brown. Built at run time, never shipped.
const s32 kSmallFrames = 7, kBigFrames = 4, kFrames = kSmallFrames + kBigFrames;
const s32 kCell = 34;
const s32 kAtlasW = 256, kAtlasH = 128;
const s32 kCellsPerRow = kAtlasW / kCell;
u32 sSmokePixels[kAtlasW * kAtlasH];
u32 sSmokeVersion;
const void* sSmokeSource;

// Whether the wisps' frames are in hand for the loaded style (rebuilt when it changes).
bool MakeSmokeTexture()
{
    if (!gGtx_0x106C || !gObjectDefinitions)
    {
        return false;
    }
    const s32 base = gObjectDefinitions->mDef112BaseSpriteIdx;
    const sprite_index* pFirst =
        gGtx_0x106C->get_sprite_index(gGtx_0x106C->GetSpriteTrueIndex(sprite_types_enum::code_obj2_8, static_cast<s16>(base + 103)));
    if (!pFirst || !pFirst->mpData)
    {
        return false;
    }
    if (pFirst->mpData == sSmokeSource)
    {
        return true;
    }
    memset(sSmokePixels, 0, sizeof(sSmokePixels));
    u32 version = 2166136261u;
    for (s32 f = 0; f < kFrames; f++)
    {
        const s16 idx = static_cast<s16>(base + (f < kSmallFrames ? 103 + f : 157 + f - kSmallFrames));
        const u16 trueIdx = gGtx_0x106C->GetSpriteTrueIndex(sprite_types_enum::code_obj2_8, idx);
        const sprite_index* pIndex = gGtx_0x106C->get_sprite_index(trueIdx);
        const u8* pPal = gGtx_0x106C->GetPalData(gGtx_0x106C->get_phys_pal(gGtx_0x106C->GetTruePalette(palette_types_enum::sprites_2, trueIdx)));
        const s32 w = pIndex->mWidth < kCell - 2 ? pIndex->mWidth : kCell - 2;
        const s32 h = pIndex->mHeight < kCell - 2 ? pIndex->mHeight : kCell - 2;
        const s32 x0 = (f % kCellsPerRow) * kCell + (kCell - w) / 2, y0 = (f / kCellsPerRow) * kCell + (kCell - h) / 2;
        for (s32 y = 0; y < h; y++)
        {
            for (s32 x = 0; x < w; x++)
            {
                const u8 c = pIndex->mpData[y * 256 + x];
                if (!c)
                {
                    continue;
                }
                // Palette entries are 64 dwords apart, 0x00RRGGBB.
                u32 rgb;
                memcpy(&rgb, pPal + c * 256, 4);
                const f32 r = static_cast<f32>((rgb >> 16) & 0xFF), g = static_cast<f32>((rgb >> 8) & 0xFF), b = static_cast<f32>(rgb & 0xFF);
                const f32 lum0 = 0.3f * r + 0.59f * g + 0.11f * b;
                // A third of the colour kept, a little warm, the shading flattened towards a mid grey and the near
                // black of the billows lifted, so that overlapping wisps merge into one smoke.
                const f32 lum = 140.0f + (lum0 - 140.0f) * 0.6f;
                const f32 k = 0.33f;
                const f32 lift = 30.0f * (1.0f - lum / 255.0f);
                const u32 ro = static_cast<u32>(fminf(lum + (r - lum) * k + lift + 6.0f, 255.0f));
                const u32 go = static_cast<u32>(fminf(lum + (g - lum) * k + lift, 255.0f));
                const u32 bo = static_cast<u32>(fminf(lum + (b - lum) * k + lift - 6.0f, 255.0f));
                const u32 px = ro | go << 8 | bo << 16 | 0xFF000000u;
                sSmokePixels[(y0 + y) * kAtlasW + x0 + x] = px;
                version = (version ^ px) * 16777619u;
            }
        }
    }
    // A texel next to a transparent one is half opaque: a soft rim a texel wide, as the game's own filtering gives.
    for (s32 y = 1; y < kAtlasH - 1; y++)
    {
        for (s32 x = 1; x < kAtlasW - 1; x++)
        {
            u32& px = sSmokePixels[y * kAtlasW + x];
            if (px && (!sSmokePixels[y * kAtlasW + x - 1] || !sSmokePixels[y * kAtlasW + x + 1] || !sSmokePixels[(y - 1) * kAtlasW + x] ||
                       !sSmokePixels[(y + 1) * kAtlasW + x]))
            {
                px = (px & 0xFFFFFFu) | 0x8C000000u;
            }
        }
    }
    sSmokeSource = pFirst->mpData;
    sSmokeVersion = version;
    return true;
}

// An ember: a solid square, drawn as one or two of the game's texels on whole pixels.
const s32 kEmberTex = 4;
u32 sEmberPixels[kEmberTex * kEmberTex];

void MakeEmberTexture()
{
    for (s32 i = 0; i < kEmberTex * kEmberTex; i++)
    {
        sEmberPixels[i] = 0xFFFFFFFFu;
    }
}

const Kind* KindOf(const Explosion& explosion, f32* pTarget)
{
    *pTarget = 1.0f;
    switch (explosion.mType)
    {
        case explosion_type::car_fire_4:
            return &kCarFire;
        case explosion_type::car_fire_level1_12:
            return &kCarFire1;
        case explosion_type::car_fire_level2_13:
            return &kCarFire2;
        case explosion_type::car_fire_level3_14:
            return &kCarFire3;
        case explosion_type::fire_3:
            return &kFire;
        case explosion_type::molotov_fire_5:
            return &kMolotov;
        case explosion_type::small_18:
        case explosion_type::item_19:
        case explosion_type::large_20:
        case explosion_type::no_ring_32:
        case explosion_type::small_33:
        {
            // Once the fireball has burnt out (its countdown from 100), for about a second.
            const s32 t = explosion.mTimer;
            *pTarget = t <= 42 && t > 10 ? (explosion.mType == explosion_type::large_20 ? 1.0f : 0.75f) : 0.0f;
            return &kBlast;
        }
        default:
            return NULL;
    }
}

void Track(const Explosion& explosion)
{
    const Sprite* pSprite = explosion.mpObj2C ? explosion.mpObj2C->mSprite : NULL;
    f32 target;
    const Kind* pKind = KindOf(explosion, &target);
    if (!pSprite || !pKind)
    {
        return;
    }
    Emitter* pEmitter = NULL;
    for (s32 i = 0; i < sNumEmitters && !pEmitter; i++)
    {
        pEmitter = sEmitters[i].mpKey == &explosion && sEmitters[i].mId == explosion.mId ? &sEmitters[i] : NULL;
    }
    if (!pEmitter)
    {
        if (sNumEmitters == kMaxEmitters)
        {
            return;
        }
        pEmitter = &sEmitters[sNumEmitters++];
        memset(pEmitter, 0, sizeof(*pEmitter));
        pEmitter->mpKey = &explosion;
        pEmitter->mId = explosion.mId;
        pEmitter->mSeed = static_cast<u32>(reinterpret_cast<uintptr_t>(&explosion) >> 3) * 2654435761u ^
                          static_cast<u16>(explosion.mId) * 40503u;
    }
    const f32 x = pSprite->mXy.x.ToFloat(), y = pSprite->mXy.y.ToFloat();
    if (pEmitter->mpKind)
    {
        pEmitter->mMoved += fminf(sqrtf((x - pEmitter->mX) * (x - pEmitter->mX) + (y - pEmitter->mY) * (y - pEmitter->mY)), 1.0f);
    }
    pEmitter->mpKind = pKind;
    pEmitter->mX = x;
    pEmitter->mY = y;
    pEmitter->mZ = pSprite->mZpos.ToFloat();
    pEmitter->mTarget = target;
    pEmitter->mSeen = true;
}

// How far up the screen a fire's flames reach from its sprite (their particles drift that way), where its smoke
// comes out.
f32 FlameTip(const Kind& k)
{
    return &k == &kBlast ? 0.0f : 2.0f * k.mSpread + 0.05f;
}

f32 Share(const Emitter& e, u32 salt, f32 base, f32 range)
{
    return base + range * Hash(e.mSeed, e.mCount * 8 + salt);
}

// What the emitter gives off over the last dt seconds, each puff and ember born at its own time within them.
void Emit(Emitter& e, f64 now, f32 dt)
{
    const Kind& k = *e.mpKind;
    const f32 rate = k.mRate * e.mLevel;
    // Smouldering once the flames are gone: thinner smoke, no glow and no embers.
    const bool burning = e.mSeen && e.mTarget > 0.0f;
    // Wisps come at random times (each due after an exponential share of the rate), in gusts.
    const f32 gust = 0.35f + 1.3f * Noise3(e.mSeed, 0.0f, 0.0f, static_cast<f32>(fmod(now, 4096.0)) * 0.9f);
    // A moving fire (a car driven while burning) lays a trail: more wisps for every block it covers.
    e.mPuffs += rate * gust * dt + (burning ? kTrail * e.mLevel * e.mMoved : 0.0f);
    e.mMoved = 0.0f;
    for (f32 share = -logf(1.0f - 0.95f * Hash(e.mSeed, e.mCount * 8 + 4)); e.mPuffs >= share;
         share = -logf(1.0f - 0.95f * Hash(e.mSeed, e.mCount * 8 + 4)))
    {
        e.mPuffs -= share;
        const u32 n = e.mCount++;
        if (sNumPuffs < kMaxPuffs)
        {
            Puff& p = sPuffs[sNumPuffs++];
            const f32 a = Hash(e.mSeed, n * 8) * 6.2831853f;
            const f32 r = k.mSpread * sqrtf(Hash(e.mSeed, n * 8 + 1));
            p.mX = e.mX + cosf(a) * r;
            p.mY = e.mY + sinf(a) * r - FlameTip(k);
            p.mZ = e.mZ + k.mHeight * (0.5f + Hash(e.mSeed, n * 8 + 5));
            p.mBorn = now - e.mPuffs / (rate * gust);
            p.mLife = k.mLife * (0.6f + 0.8f * Hash(e.mSeed, n * 8 + 2));
            p.mBig = Hash(e.mSeed, n * 8 + 6) < k.mBig;
            p.mScale = k.mScale * (p.mBig ? 0.7f + 0.4f * Hash(e.mSeed, n * 8 + 3) : 0.8f + 0.8f * Hash(e.mSeed, n * 8 + 3));
            p.mAlpha = k.mAlpha * (0.75f + 0.25f * Hash(e.mSeed, n * 8 + 7)) * (burning ? 1.0f : 0.5f + 0.5f * e.mLevel);
            p.mRise = kRise * (0.6f + 0.8f * Hash(e.mSeed ^ 0x55u, n));
            p.mGlow = burning ? e.mLevel : 0.0f;
            p.mSeed = e.mSeed ^ n * 0x27D4EB2Du;
        }
    }
    const f32 emberRate = burning ? k.mEmbers * e.mLevel : 0.0f;
    e.mEmbers += emberRate * dt;
    for (f32 share = Share(e, 2, 0.4f, 1.2f); e.mEmbers >= share; share = Share(e, 2, 0.4f, 1.2f))
    {
        e.mEmbers -= share;
        const u32 n = e.mCount++;
        if (sNumEmbers < kMaxEmbers)
        {
            Ember& m = sEmbers[sNumEmbers++];
            const f32 a = Hash(e.mSeed, n * 8 + 5) * 6.2831853f;
            const f32 r = k.mSpread * 0.7f * sqrtf(Hash(e.mSeed, n * 8 + 6));
            const f32 speed = 0.4f + 0.6f * Hash(e.mSeed, n * 8 + 7);
            m.mX = e.mX + cosf(a) * r;
            m.mY = e.mY + sinf(a) * r;
            m.mZ = e.mZ + k.mHeight * 0.5f;
            m.mVx = cosf(a) * speed;
            m.mVy = sinf(a) * speed;
            m.mVz = 0.9f + 0.8f * Hash(e.mSeed, n * 8 + 3);
            m.mBorn = now - e.mEmbers / emberRate;
            m.mLife = 0.8f + 0.8f * Hash(e.mSeed, n * 8 + 1);
            m.mSeed = e.mSeed ^ n * 0x165667B1u;
        }
    }
}

// One quad: corners round the screen centre (sx, sy), half sizes in pixels and blocks, turned by angle.
void Corners(f32* pOut, f32 sx, f32 sy, f32 halfPx, f32 x, f32 y, f32 z, f32 halfWorld, f32 angle, f32 u0, f32 v0, f32 du, f32 dv)
{
    const f32 c = cosf(angle), s = sinf(angle);
    const f32 kx[4] = {-1, 1, 1, -1}, ky[4] = {-1, -1, 1, 1};
    for (s32 i = 0; i < 4; i++)
    {
        const f32 rx = kx[i] * c - ky[i] * s, ry = kx[i] * s + ky[i] * c;
        f32* o = &pOut[i * 7];
        o[0] = sx + rx * halfPx;
        o[1] = sy + ry * halfPx;
        o[2] = u0 + (kx[i] > 0 ? du : 0.0f);
        o[3] = v0 + (ky[i] > 0 ? dv : 0.0f);
        o[4] = x + rx * halfWorld;
        o[5] = y + ry * halfWorld;
        o[6] = z;
    }
}

// The frame's quads, lowest first, and where each layer's quads start (MapRenderer::Draw's layers 1 to 7).
f32 sPuffCorners[kMaxPuffs * 28];
u32 sPuffColours[kMaxPuffs];
s32 sPuffStart[9];
f32 sEmberCorners[kMaxEmbers * 28];
u32 sEmberColours[kMaxEmbers];
s32 sEmberStart[9];

s32 LayerOf(f32 z)
{
    const s32 layer = static_cast<s32>(floorf(z));
    return layer < 1 ? 1 : (layer > 7 ? 7 : layer);
}

// pStart[layer] .. pStart[layer + 1] are that layer's quads, for quads given by layer in ascending order.
void MarkLayers(s32* pStart, const s32* pLayers, s32 count)
{
    for (s32 layer = 0, i = 0; layer <= 8; layer++)
    {
        while (i < count && pLayers[i] < layer)
        {
            i++;
        }
        pStart[layer] = i;
    }
}

void BuildPuffs(f64 now)
{
    // Lowest first, so that the wisps higher up, nearer the camera, cover them.
    static f32 sZ[kMaxPuffs];
    static s32 sOrder[kMaxPuffs];
    static s32 sLayers[kMaxPuffs];
    s32 n = 0;
    for (s32 i = 0; i < sNumPuffs; i++)
    {
        const Puff& p = sPuffs[i];
        const f32 u = Clamp01(static_cast<f32>(now - p.mBorn) / p.mLife);
        sZ[i] = p.mZ + p.mRise * p.mLife / 1.7f * (1.0f - powf(1.0f - u, 1.7f));
        s32 j = n++;
        while (j > 0 && sZ[sOrder[j - 1]] > sZ[i])
        {
            sOrder[j] = sOrder[j - 1];
            j--;
        }
        sOrder[j] = i;
    }
    const f32 t = static_cast<f32>(fmod(now, 4096.0));
    s32 count = 0;
    for (s32 o = 0; o < n; o++)
    {
        const Puff& p = sPuffs[sOrder[o]];
        const f32 age = static_cast<f32>(now - p.mBorn);
        const f32 u = Clamp01(age / p.mLife);
        const f32 ease = 1.0f - (1.0f - u) * (1.0f - u);
        f32 alpha = p.mAlpha * Smooth(0.0f, 0.05f, u) * powf(1.0f - u, 0.85f);
        if (alpha < 0.004f)
        {
            continue;
        }
        // Carried up the screen, more as it climbs, and pushed about by a moving noise that neighbouring wisps share,
        // so that the plume curls and breaks up.
        const f32 wind = age * (0.6f + 0.4f * u) * (0.8f + 0.4f * Hash(p.mSeed, 1));
        f32 x = p.mX + kWindX * wind, y = p.mY + kWindY * wind;
        const f32 push = kTurbulence * powf(u, 0.7f);
        x += push * (2.0f * Noise3(0x7E1Au, x * 1.6f, y * 1.6f, t * 0.5f) - 1.0f);
        y += push * (2.0f * Noise3(0x3C5Fu, x * 1.6f, y * 1.6f, t * 0.5f) - 1.0f);
        // Lit from below by the flames while it is still over them, or over any other: a deep orange there and
        // thinner, as a grey over the yellow flames would turn them olive.
        f32 warm = p.mGlow * (1.0f - Smooth(0.0f, 0.25f, u));
        for (s32 i = 0; i < sNumEmitters; i++)
        {
            const Emitter& e = sEmitters[i];
            if (e.mSeen && e.mTarget > 0.0f && e.mpKind != &kBlast)
            {
                const f32 dx = x - e.mX, dy = y - e.mY + 0.5f * FlameTip(*e.mpKind), reach = 0.15f + 1.4f * e.mpKind->mSpread;
                warm = fmaxf(warm, e.mLevel * (1.0f - Smooth(0.5f * reach, reach, sqrtf(dx * dx + dy * dy))));
            }
        }
        alpha *= 1.0f - 0.55f * warm;
        if (alpha < 0.004f)
        {
            continue;
        }
        // The game's frames in turn as the wisp thins out, from a random one; a cell is 34 texels, 64 to a block.
        const s32 first = p.mBig ? kSmallFrames : static_cast<s32>(Hash(p.mSeed, 2) * 2.99f);
        const s32 last = p.mBig ? kFrames - 1 : kSmallFrames - 1;
        const s32 frame = first + static_cast<s32>(u * (last - first + 0.99f));
        const f32 size = kCell / 64.0f * p.mScale * (1.25f + 0.45f * ease);
        const f32 z = sZ[sOrder[o]];
        f32 sx, sy, ex, ey;
        PortView::GroundToScreen(x, y, z, &sx, &sy);
        PortView::GroundToScreen(x + size * 0.5f, y, z, &ex, &ey);
        const f32 half = ex - sx;
        if (half <= 0.0f || half > 600.0f)
        {
            continue;
        }
        // A little darker where it leaves the fire.
        const f32 shade = 0.78f + 0.22f * u;
        const u32 r = static_cast<u32>(255.0f * fminf(shade + 0.25f * warm, 1.0f));
        const u32 g = static_cast<u32>(255.0f * shade * (1.0f - 0.45f * warm));
        const u32 b = static_cast<u32>(255.0f * shade * (1.0f - 0.75f * warm));
        const u32 a = static_cast<u32>(alpha * 255.0f);
        const f32 spin = Hash(p.mSeed, 4) * 6.2831853f + (Hash(p.mSeed, 5) - 0.5f) * 1.2f * u;
        const bool flip = Hash(p.mSeed, 6) < 0.5f;
        const f32 du = static_cast<f32>(kCell) / kAtlasW, dv = static_cast<f32>(kCell) / kAtlasH;
        const f32 u0 = (frame % kCellsPerRow) * du, v0 = (frame / kCellsPerRow) * dv;
        Corners(&sPuffCorners[count * 28], sx, sy, half, x, y, z, size * 0.5f, spin, flip ? u0 + du : u0, v0, flip ? -du : du, dv);
        sPuffColours[count] = a << 24 | r << 16 | g << 8 | b;
        sLayers[count++] = LayerOf(z);
    }
    MarkLayers(sPuffStart, sLayers, count);
}

// The embers' colours as they cool, in steps as a palette would have them.
const u32 kEmberSteps[4] = {0xFFFFEC96u, 0xFFFFB432u, 0xFFEB6414u, 0xFF96280Au};

void BuildEmbers(f64 now)
{
    static s32 sOrder[kMaxEmbers];
    static f32 sZ[kMaxEmbers];
    static s32 sLayers[kMaxEmbers];
    s32 n = 0;
    for (s32 i = 0; i < sNumEmbers; i++)
    {
        const Ember& m = sEmbers[i];
        const f32 t = static_cast<f32>(now - m.mBorn);
        sZ[i] = m.mZ + m.mVz * t - 0.5f * kEmberLift * t * t;
        s32 j = n++;
        while (j > 0 && sZ[sOrder[j - 1]] > sZ[i])
        {
            sOrder[j] = sOrder[j - 1];
            j--;
        }
        sOrder[j] = i;
    }
    s32 count = 0;
    for (s32 o = 0; o < n; o++)
    {
        const Ember& m = sEmbers[sOrder[o]];
        const f32 t = static_cast<f32>(now - m.mBorn);
        const f32 u = Clamp01(t / m.mLife);
        const f32 x = m.mX + (m.mVx + kWindX * 0.6f) * t;
        const f32 y = m.mY + (m.mVy + kWindY * 0.6f) * t;
        const f32 z = sZ[sOrder[o]];
        f32 sx, sy, ex, ey;
        PortView::GroundToScreen(x, y, z, &sx, &sy);
        PortView::GroundToScreen(x + 1.0f / 64.0f, y, z, &ex, &ey);
        // One or two of the game's texels, on whole pixels; cooling in steps, and a hard flicker every other step.
        const f32 texels = Hash(m.mSeed, 1) < 0.6f ? 1.0f : 2.0f;
        const f32 size = fmaxf(floorf((ex - sx) * texels + 0.5f), 1.0f);
        const f32 left = floorf(sx - size * 0.5f + 0.5f), top = floorf(sy - size * 0.5f + 0.5f);
        const u32 colour = kEmberSteps[static_cast<s32>(u * 3.99f)];
        const bool dim = Hash(m.mSeed, static_cast<u32>(t * 15.0f) + 2) < 0.3f;
        const u32 rgb = dim ? (colour >> 1) & 0x7F7F7Fu : colour & 0xFFFFFFu;
        Corners(&sEmberCorners[count * 28], left + size * 0.5f, top + size * 0.5f, size * 0.5f, x, y, z, texels / 128.0f, 0.0f, 0.0f, 0.0f,
                1.0f, 1.0f);
        sEmberColours[count] = 0xFF000000u | rgb;
        sLayers[count++] = LayerOf(z);
    }
    MarkLayers(sEmberStart, sLayers, count);
}

void Clear()
{
    sNumEmitters = sNumPuffs = sNumEmbers = 0;
}

// The frame's emitters, wisps and embers, and their quads.
void Update(bool smoke)
{
    const f64 now = PortFrameLoop::WorldSeconds();
    // Time going back (a new game) or leaping forward: nothing carries over.
    if (now < sLastSeconds - 0.001 || now - sLastSeconds > 2.0)
    {
        Clear();
    }
    const f32 dt = static_cast<f32>(now - sLastSeconds);
    sLastSeconds = now;

    for (s32 i = 0; i < sNumEmitters; i++)
    {
        sEmitters[i].mSeen = false;
    }
    for (s32 i = 0; gExplosionPool_7A8 && i < 40; i++)
    {
        if (gExplosionPool_7A8->mbUsed[i])
        {
            Track(gExplosionPool_7A8->mExplosions[i]);
        }
    }
    for (s32 i = 0; gExplosionPool_3D4 && i < 20; i++)
    {
        if (gExplosionPool_3D4->mbUsed[i])
        {
            Track(gExplosionPool_3D4->mExplosions[i]);
        }
    }
    s32 kept = 0;
    for (s32 i = 0; i < sNumEmitters; i++)
    {
        Emitter e = sEmitters[i];
        const f32 target = e.mSeen ? e.mTarget : 0.0f;
        const f32 step = dt > 0.0f && dt < 0.25f ? dt : 0.0f;
        e.mLevel = e.mLevel < target ? fminf(target, e.mLevel + step / e.mpKind->mRampIn)
                                     : fmaxf(target, e.mLevel - step / e.mpKind->mRampOut);
        if (!e.mSeen && e.mLevel <= 0.0f)
        {
            continue;
        }
        if (step > 0.0f)
        {
            Emit(e, now, step);
        }
        sEmitters[kept++] = e;
    }
    sNumEmitters = kept;

    kept = 0;
    for (s32 i = 0; i < sNumPuffs; i++)
    {
        if (now - sPuffs[i].mBorn < sPuffs[i].mLife)
        {
            sPuffs[kept++] = sPuffs[i];
        }
    }
    sNumPuffs = kept;
    kept = 0;
    for (s32 i = 0; i < sNumEmbers; i++)
    {
        if (now - sEmbers[i].mBorn < sEmbers[i].mLife)
        {
            sEmbers[kept++] = sEmbers[i];
        }
    }
    sNumEmbers = kept;

#if defined(GTA2_PORT_SDL)
    // The fires' and blasts' light on the smoke, where the level's lights do not give it (NOON, the cycle's day).
    port_ClearSmokeLights();
    PortLights::SmokeLights(1.0f - PortDayNight::LightsShare());
#endif
    memset(sPuffStart, 0, sizeof(sPuffStart));
    if (smoke)
    {
        BuildPuffs(now);
    }
    BuildEmbers(now);
}
} // namespace

void DrawLayer(s32 layer)
{
    static bool sEmberTexture;
    static bool sSmoke;
    PortShotFlash::DrawLayer(layer); // with the same layers, whatever FIRE EFFECTS is set to
    if (!PortSettings::FireEffects() || !gGame_0x40 || !gGame_0x40->mDrawPlayer || layer < 1 || layer > 7)
    {
        Clear();
#if defined(GTA2_PORT_SDL)
        port_ClearSmokeLights();
#endif
        return;
    }
    // The first layer of the frame's map draw brings everything up to date.
    if (layer == 1)
    {
        if (!sEmberTexture)
        {
            MakeEmberTexture();
            sEmberTexture = true;
        }
        sSmoke = MakeSmokeTexture();
        Update(sSmoke);
    }
#if defined(GTA2_PORT_SDL)
    const s32 puffs = sPuffStart[layer + 1] - sPuffStart[layer];
    if (sSmoke && puffs)
    {
        port_DrawParticles(0, kAtlasW, kAtlasH, sSmokePixels, sSmokeVersion, &sPuffCorners[sPuffStart[layer] * 28],
                           &sPuffColours[sPuffStart[layer]], puffs, false);
    }
    const s32 embers = sEmberStart[layer + 1] - sEmberStart[layer];
    if (embers)
    {
        port_DrawParticles(1, kEmberTex, kEmberTex, sEmberPixels, 1, &sEmberCorners[sEmberStart[layer] * 28],
                           &sEmberColours[sEmberStart[layer]], embers, true);
    }
#endif
}
} // namespace PortFireSmoke
