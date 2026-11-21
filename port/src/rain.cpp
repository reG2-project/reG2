#include "rain.hpp"
#include "Game.hpp"
#include "frame_loop.hpp"
#include "map_0x370.hpp"
#include "view.hpp"
#include <math.h>
#include <stdlib.h>
#include <string.h>

#if defined(GTA2_PORT_SDL)
void port_RainScene(u32 bits, f32 eyeX, f32 eyeY, f32 eyeZ, f32 time, f32 ground, f32 glow, f32 puddles);
void port_RainReflect(f32 time);
void port_DrawParticles(s32 slot, s32 width, s32 height, const u32* pRgba, u32 version, const f32* pCorners, const u32* pColours,
                        s32 count, bool additive);
#endif

namespace PortRain
{
namespace
{
enum
{
    kStreaks = 1,
    kWet = 2,
    kReflect = 4,
};

u32 Mode()
{
    static s32 sMode = -1;
    if (sMode < 0)
    {
        const char* p = getenv("GTA2_RAIN");
        sMode = 0;
        if (p)
        {
            sMode |= strstr(p, "streak") ? kStreaks : 0;
            sMode |= strstr(p, "wet") ? kWet : 0;
            sMode |= strstr(p, "refl") ? kWet | kReflect : 0;
            sMode |= strstr(p, "all") ? kStreaks | kWet | kReflect : 0;
        }
    }
    return static_cast<u32>(sMode);
}

// All in blocks and seconds. Drops fall from kTop blocks under the eye down to the ground, pushed by the wind; a
// streak is where a drop went during kExposure.
// 900 drops on a 1280x720 screen, as many per pixel on larger ones.
const s32 kDrops = 4096;
const f32 kFall = 18.0f;
const f32 kWindX = 1.2f, kWindY = 2.6f;
const f32 kExposure = 0.022f;
const f32 kTop = 1.5f;
const f32 kDepth = 9.0f;
const f32 kSplashLife = 0.18f;
const f32 kWidth = 0.018f;

struct Drop
{
    s32 mCycle;
    f32 mX, mY, mGround;
};
Drop sDrops[kDrops];

u32 Hash(u32 a, u32 b)
{
    u32 h = a * 0x9E3779B1u ^ (b + 0x7F4A7C15u) * 0x85EBCA77u;
    h ^= h >> 15;
    h *= 0x2C1B3C6Du;
    h ^= h >> 12;
    return h;
}

f32 Rand(u32 a, u32 b)
{
    return (Hash(a, b) & 0xFFFFFF) / 16777216.0f;
}

f32 GroundZ(f32 x, f32 y)
{
    x = fminf(fmaxf(x, 0.05f), 255.9f);
    y = fminf(fmaxf(y, 0.05f), 255.9f);
    return gMap_0x370 ? gMap_0x370->FindGroundZForCoord(Fix16(x), Fix16(y)).ToFloat() : 0.0f;
}

// GTA2_RAIN_GLOW scales every glow the rain adds, GTA2_RAIN_PUDDLES is the share of flat ground under water.
f32 EnvFloat(const char* pName, f32 fallback)
{
    const char* p = getenv(pName);
    return p && *p ? static_cast<f32>(atof(p)) : fallback;
}

// 4x32 streak, tail (top) transparent to head; a 32x32 ring for the splashes.
const s32 kStreakW = 4, kStreakH = 32, kRing = 32;
u32 sStreakPixels[kStreakW * kStreakH];
u32 sRingPixels[kRing * kRing];

void MakeTextures()
{
    static const f32 kAcross[kStreakW] = {0.35f, 1.0f, 1.0f, 0.35f};
    for (s32 y = 0; y < kStreakH; y++)
    {
        const f32 t = (y + 0.5f) / kStreakH;
        for (s32 x = 0; x < kStreakW; x++)
        {
            const u32 a = static_cast<u32>(255.0f * t * t * kAcross[x]);
            sStreakPixels[y * kStreakW + x] = 0x00FFFFFFu | a << 24;
        }
    }
    for (s32 y = 0; y < kRing; y++)
    {
        for (s32 x = 0; x < kRing; x++)
        {
            const f32 dx = (x + 0.5f) / kRing * 2.0f - 1.0f, dy = (y + 0.5f) / kRing * 2.0f - 1.0f;
            const f32 r = sqrtf(dx * dx + dy * dy);
            const f32 ring = expf(-(r - 0.78f) * (r - 0.78f) / 0.008f);
            const f32 a = r > 1.0f ? 0.0f : fminf(1.0f, ring + 0.25f * expf(-r * r / 0.05f));
            sRingPixels[y * kRing + x] = 0x00FFFFFFu | static_cast<u32>(255.0f * a) << 24;
        }
    }
}

f32 sStreakCorners[kDrops * 28];
u32 sStreakColours[kDrops];
f32 sSplashCorners[kDrops * 28];
u32 sSplashColours[kDrops];

void Corner(f32* c, f32 x, f32 y, f32 u, f32 v, f32 wx, f32 wy, f32 wz)
{
    c[0] = x, c[1] = y, c[2] = u, c[3] = v, c[4] = wx, c[5] = wy, c[6] = wz;
}

u32 Colour(f32 alpha, u32 rgb)
{
    const u32 a = static_cast<u32>(fminf(fmaxf(alpha, 0.0f), 1.0f) * 255.0f);
    return a << 24 | rgb;
}
} // namespace

void BeginDraw()
{
#if defined(GTA2_PORT_SDL)
    f32 x, y, z;
    PortView::Eye(&x, &y, &z);
    port_RainScene((Mode() & (kWet | kReflect)) | (getenv("GTA2_RAIN_DEBUG") ? 8u : 0u) | (getenv("GTA2_RAIN_NOSPRITE") ? 16u : 0u), x, y, z, static_cast<f32>(fmod(PortFrameLoop::WorldSeconds(), 3600.0)),
                   GroundZ(x, y), EnvFloat("GTA2_RAIN_GLOW", 1.0f), EnvFloat("GTA2_RAIN_PUDDLES", 0.15f));
#endif
}

void Draw()
{
#if defined(GTA2_PORT_SDL)
    if (!gGame_0x40 || gGame_0x40->mNumPlayers != 1)
    {
        return;
    }
    if (Mode() & kReflect)
    {
        port_RainReflect(static_cast<f32>(fmod(PortFrameLoop::WorldSeconds(), 3600.0)));
    }
    if (!(Mode() & kStreaks))
    {
        return;
    }
    static bool sTextures;
    if (!sTextures)
    {
        MakeTextures();
        sTextures = true;
    }
    const f32 now = static_cast<f32>(fmod(PortFrameLoop::WorldSeconds(), 3600.0));
    f32 eyeX, eyeY, eyeZ;
    PortView::Eye(&eyeX, &eyeY, &eyeZ);
    const f32 groundRef = GroundZ(eyeX, eyeY);
    // The ground seen, with a margin for the wind and the tall drops' paths.
    f32 x0, y0, x1, y1, sw, sh;
    PortView::ScreenSize(&sw, &sh);
    PortView::ScreenToGround(-120.0f, -120.0f, groundRef, &x0, &y0);
    PortView::ScreenToGround(sw + 120.0f, sh + 120.0f, groundRef, &x1, &y1);
    const f32 top = eyeZ - kTop;
    const f32 period = kDepth / kFall;
    s32 streaks = 0, splashes = 0;
    const s32 drops = static_cast<s32>(fminf(900.0f * sw * sh / (1280.0f * 720.0f), kDrops));
    for (s32 i = 0; i < drops; i++)
    {
        Drop& d = sDrops[i];
        const f32 t = now / period + Rand(i, 0x51);
        const s32 cycle = static_cast<s32>(floorf(t));
        const f32 age = (t - cycle) * period;
        if (d.mCycle != cycle || d.mGround == 0.0f)
        {
            d.mCycle = cycle;
            // Where the drop lands: it starts kWind * fall time upwind of there.
            d.mX = x0 + (x1 - x0) * Rand(i, cycle * 2 + 1);
            d.mY = y0 + (y1 - y0) * Rand(i, cycle * 2 + 2);
            d.mGround = GroundZ(d.mX, d.mY);
        }
        const f32 fallTime = (top - d.mGround) / kFall;
        if (age < fallTime)
        {
            const f32 left = fallTime - age;
            const f32 hx = d.mX - kWindX * left, hy = d.mY - kWindY * left, hz = d.mGround + kFall * left;
            const f32 tx = hx - kWindX * kExposure, ty = hy - kWindY * kExposure, tz = hz + kFall * kExposure;
            f32 sx0, sy0, sx1, sy1, wx0, wy0, wx1, wy1;
            PortView::GroundToScreen(tx, ty, tz, &sx0, &sy0);
            PortView::GroundToScreen(hx, hy, hz, &sx1, &sy1);
            PortView::GroundToScreen(hx, hy, hz, &wx0, &wy0);
            PortView::GroundToScreen(hx + kWidth, hy, hz, &wx1, &wy1);
            f32 dx = sx1 - sx0, dy = sy1 - sy0;
            const f32 len = sqrtf(dx * dx + dy * dy);
            if (len < 0.5f || sx1 < -50.0f || sy1 < -50.0f || sx1 > sw + 50.0f || sy1 > sh + 50.0f)
            {
                continue;
            }
            const f32 w = fmaxf(0.6f, wx1 - wx0);
            const f32 px = -dy / len * w, py = dx / len * w;
            f32* c = &sStreakCorners[streaks * 28];
            Corner(c, sx0 - px, sy0 - py, 0.0f, 0.0f, tx, ty, tz);
            Corner(c + 7, sx0 + px, sy0 + py, 1.0f, 0.0f, tx, ty, tz);
            Corner(c + 14, sx1 + px, sy1 + py, 1.0f, 1.0f, hx, hy, hz);
            Corner(c + 21, sx1 - px, sy1 - py, 0.0f, 1.0f, hx, hy, hz);
            // Drops close to the eye are out of focus: fainter.
            const f32 nearEye = fminf(fmaxf((hz - d.mGround) / (top - d.mGround), 0.0f), 1.0f);
            sStreakColours[streaks++] = Colour(0.45f - 0.35f * nearEye, 0xC8D4E0);
        }
        // No splashes on the roofs above the street: seen from above they read as bubbles.
        else if (age - fallTime < kSplashLife && d.mGround < groundRef + 0.75f)
        {
            const f32 s = (age - fallTime) / kSplashLife;
            const f32 r = 0.012f + 0.032f * sqrtf(s);
            f32* c = &sSplashCorners[splashes * 28];
            const f32 g = d.mGround + 0.01f;
            f32 sx, sy;
            PortView::GroundToScreen(d.mX - r, d.mY - r, g, &sx, &sy);
            Corner(c, sx, sy, 0.0f, 0.0f, d.mX - r, d.mY - r, g);
            PortView::GroundToScreen(d.mX + r, d.mY - r, g, &sx, &sy);
            Corner(c + 7, sx, sy, 1.0f, 0.0f, d.mX + r, d.mY - r, g);
            PortView::GroundToScreen(d.mX + r, d.mY + r, g, &sx, &sy);
            Corner(c + 14, sx, sy, 1.0f, 1.0f, d.mX + r, d.mY + r, g);
            PortView::GroundToScreen(d.mX - r, d.mY + r, g, &sx, &sy);
            Corner(c + 21, sx, sy, 0.0f, 1.0f, d.mX - r, d.mY + r, g);
            sSplashColours[splashes++] = Colour(0.3f * (1.0f - s) * (1.0f - s), 0xD8E4F0);
        }
    }
    if (splashes)
    {
        port_DrawParticles(3, kRing, kRing, sRingPixels, 1, sSplashCorners, sSplashColours, splashes, false);
    }
    if (streaks)
    {
        port_DrawParticles(2, kStreakW, kStreakH, sStreakPixels, 1, sStreakCorners, sStreakColours, streaks, false);
    }
#endif
}
} // namespace PortRain
