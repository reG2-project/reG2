#include "shot_trails.hpp"
#include "interpolation.hpp"
#include "sprite.hpp"
#include "view.hpp"
#include <math.h>

#if defined(GTA2_PORT_SDL)
void port_DrawParticles(s32 slot, s32 width, s32 height, const u32* pRgba, u32 version, const f32* pCorners, const u32* pColours,
                        s32 count, bool additive);
#endif

namespace PortShotTrails
{
namespace
{
const f64 kTrailSteps = 1.0; // 2 frames at 60 fps, the frame it starts at below that
const f32 kFlashSize = 0.42f; // blocks
const f32 kTracerWidth = 0.05f;
const s32 kMaxTrails = 32;
const s32 kTex = 32; // the flash's glow left, the tracer's cross-section right

struct Trail
{
    const Sprite* mpShooter;
    s16 mShooterId;
    f32 mMuzzle, mX, mY, mZ, mEx, mEy, mEz;
    f64 mStart; // the time of its first frame, -1 before it
};
Trail sTrails[kMaxTrails];
s32 sNumTrails;
u32 sPixels[kTex * 2 * kTex];

void MakeTexture()
{
    for (s32 y = 0; y < kTex; y++)
    {
        for (s32 x = 0; x < kTex * 2; x++)
        {
            const f32 v = (y + 0.5f) / kTex * 2.0f - 1.0f;
            f32 a;
            if (x < kTex)
            {
                const f32 u = (x + 0.5f) / kTex * 2.0f - 1.0f, r = sqrtf(u * u + v * v);
                a = r >= 1.0f ? 0.0f : (1.0f - r) * (1.0f - r);
            }
            else
            {
                a = 1.0f - fabsf(v);
                a *= a;
            }
            const u32 c = static_cast<u32>(a * 255.0f + 0.5f);
            sPixels[y * kTex * 2 + x] = c | c << 8 | c << 16 | c << 24; // RGBA8, little endian
        }
    }
}

// The quad's corner i (0..3: u then v increasing) at screen (sx, sy) and world (x, y, z)
void Corner(f32* pOut, s32 i, f32 sx, f32 sy, f32 u, f32 v, f32 x, f32 y, f32 z)
{
    f32* o = &pOut[i * 7];
    o[0] = sx, o[1] = sy, o[2] = u, o[3] = v, o[4] = x, o[5] = y, o[6] = z;
}

// Additive (ONE/ONE): faded by its colour
u32 Faded(u32 rgb, f32 fade)
{
    const f32 k = fade < 0.0f ? 0.0f : (fade > 1.0f ? 1.0f : fade);
    const u32 r = static_cast<u32>(((rgb >> 16) & 0xFF) * k), g = static_cast<u32>(((rgb >> 8) & 0xFF) * k), b = static_cast<u32>((rgb & 0xFF) * k);
    return 0xFF000000u | r << 16 | g << 8 | b;
}

s32 LayerOf(f32 z)
{
    const s32 layer = static_cast<s32>(floorf(z));
    return layer < 1 ? 1 : (layer > 7 ? 7 : layer);
}
} // namespace

void Add(const Sprite* pShooter, f32 muzzle, f32 x, f32 y, f32 z, f32 ex, f32 ey, f32 ez)
{
    if (sNumTrails == kMaxTrails)
    {
        sTrails[0] = sTrails[--sNumTrails];
    }
    Trail t = {pShooter, pShooter ? pShooter->mInstanceId : static_cast<s16>(0), muzzle, x, y, z, ex, ey, ez, -1.0};
    sTrails[sNumTrails++] = t;
}

void DrawLayer(s32 layer)
{
    const f64 now = PortInterp::DrawTime();
    if (layer == 1)
    {
        for (s32 i = 0; i < sNumTrails;)
        {
            Trail& t = sTrails[i];
            t.mStart = t.mStart < 0 || t.mStart > now ? now : t.mStart;
            if (now - t.mStart >= kTrailSteps)
            {
                sTrails[i] = sTrails[--sNumTrails];
                continue;
            }
            // The gun where the shooter is drawn
            if (t.mpShooter && t.mpShooter->mInstanceId == t.mShooterId)
            {
                t.mX = t.mpShooter->mXy.x.ToFloat() + t.mMuzzle * Ang16::sine(t.mpShooter->mRotation).ToFloat();
                t.mY = t.mpShooter->mXy.y.ToFloat() + t.mMuzzle * Ang16::cosine(t.mpShooter->mRotation).ToFloat();
                t.mZ = t.mpShooter->mZpos.ToFloat();
            }
            i++;
        }
    }
#if defined(GTA2_PORT_SDL)
    static bool sTexture;
    f32 corners[kMaxTrails * 2 * 28];
    u32 colours[kMaxTrails * 2];
    s32 n = 0;
    for (s32 i = 0; i < sNumTrails; i++)
    {
        const Trail& t = sTrails[i];
        if (LayerOf(t.mZ) != layer)
        {
            continue;
        }
        const f32 fade = 1.0f - static_cast<f32>((now - t.mStart) / kTrailSteps);
        f32 sx, sy, ex, ey, px, py;
        PortView::GroundToScreen(t.mX, t.mY, t.mZ, &sx, &sy);
        PortView::GroundToScreen(t.mX + 1.0f, t.mY, t.mZ, &px, &py);
        const f32 block = px - sx; // pixels a block
        if (block <= 0.0f || block > 2000.0f)
        {
            continue;
        }
        PortView::GroundToScreen(t.mEx, t.mEy, t.mEz, &ex, &ey);
        // The tracer: the right half's cross-section along the gun to the end
        const f32 dx = ex - sx, dy = ey - sy, len = sqrtf(dx * dx + dy * dy);
        if (len > 1.0f)
        {
            const f32 w = kTracerWidth * 0.5f * block, nx = -dy / len * w, ny = dx / len * w;
            f32* c = &corners[n * 28];
            Corner(c, 0, sx + nx, sy + ny, 0.5f, 0.0f, t.mX, t.mY, t.mZ);
            Corner(c, 1, ex + nx, ey + ny, 1.0f, 0.0f, t.mEx, t.mEy, t.mEz);
            Corner(c, 2, ex - nx, ey - ny, 1.0f, 1.0f, t.mEx, t.mEy, t.mEz);
            Corner(c, 3, sx - nx, sy - ny, 0.5f, 1.0f, t.mX, t.mY, t.mZ);
            colours[n++] = Faded(0xFFE8A0, fade);
        }
        const f32 h = kFlashSize * 0.5f * block * (0.6f + 0.4f * fade);
        f32* c = &corners[n * 28];
        Corner(c, 0, sx - h, sy - h, 0.0f, 0.0f, t.mX, t.mY, t.mZ);
        Corner(c, 1, sx + h, sy - h, 0.5f, 0.0f, t.mX, t.mY, t.mZ);
        Corner(c, 2, sx + h, sy + h, 0.5f, 1.0f, t.mX, t.mY, t.mZ);
        Corner(c, 3, sx - h, sy + h, 0.0f, 1.0f, t.mX, t.mY, t.mZ);
        colours[n++] = Faded(0xFFD070, fade);
    }
    if (n)
    {
        if (!sTexture)
        {
            MakeTexture();
            sTexture = true;
        }
        port_DrawParticles(2, kTex * 2, kTex, sPixels, 1, corners, colours, n, true);
    }
#endif
}
} // namespace PortShotTrails
