// Backend interface between the gbh_* layer (render DLL) and the OpenGL core (video DLL).
// Both sides are built by the same compiler, so a plain virtual interface is safe across the DLLs.
#pragma once

#include "gta2_api.hpp"

struct RVertex
{
    f32 x, y;  // logical screen pixels
    f32 rhw;   // 1/w, used for perspective correct interpolation
    u32 rgba;  // bytes in memory: r, g, b, a
    f32 u, v;  // normalised texture coordinates
    // Per pixel lighting only (lit != 0): world position (also for ao), colour before lighting, shade 0..1.
    f32 wx, wy, wz;
    u32 unlit;
    f32 shade;
    f32 lit;
    // Ambient occlusion (SetOcclusion) at the world position, wx, wy, wz: the share of the colour it darkens,
    // 0 for none. With per pixel lighting, any value above 0 lets the shader split the light itself.
    f32 ao;
};

// Optional enhancements; all off is the faithful renderer.
struct RenderOptions
{
    bool mTrueColour = false;       // 8-bit palette colours and an RGBA8 target
    f32 mBloom = 0.0f;              // 0..1, glow from additive draws
    bool mPerPixelLighting = false; // gbh_AddLight lights evaluated per fragment
    s32 mTextureFilter = 0;         // 0 bilinear, 1 trilinear, 2 anisotropic, 3 nearest (pixelated), 4 sharp bilinear
    s32 mMsaa = 0;                  // samples per pixel of the render target, 0 or 1 for none
    bool mSoftShadows = false;      // sprite shadows with soft edges, overlaps darkened once
    u32 mGeneration = 0;            // bumped on every change
    bool Mipmaps() const { return mTextureFilter == 1 || mTextureFilter == 2; }
};

// A light as the original's LightVertex uses it (already filtered to active lights).
struct RLight
{
    f32 x, y, z;
    f32 radius;
    f32 ceiling; // the height its light fades out above
    f32 r, g, b; // colour times brightness
    f32 dx, dy;              // a beam's direction on the ground
    f32 width, beam;         // a beam's gaussian sigma at the lamp, and 1 for a beam, 0 for a point light
};

enum RenderStateBits
{
    RS_ALPHA_TEST = 1 << 0,   // discard texels with alpha < 0.5 (palette index 0)
    RS_BLEND_ALPHA = 1 << 1,  // src_alpha, 1 - src_alpha
    RS_BLEND_ADD = 1 << 2,    // additive
    RS_FILTER_LINEAR = 1 << 3,
    RS_NO_TEXTURE = 1 << 4,
    RS_PIXEL_EXACT = 1 << 5,  // no D3D pixel centre offset: integer quads copy texels 1:1 (DirectDraw blits)
    // Soft shadow (mSoftShadows): texture alpha blurred by the vertex red channel (texels * 32), times the vertex
    // alpha, into a shadow mask that FinishFrame applies; what is drawn later over it lightens the mask.
    RS_SHADOW = 1 << 6,
    RS_TEXELS = 1 << 7, // magnified as blocks of texels, without the menus' smooth edges
};

// Mip levels to build for a texture: none, or averaged with or without regard to transparent texels.
enum RMipmaps
{
    RMIP_NONE,
    RMIP_OPAQUE, // drawn without blending: every texel counts
    RMIP_ALPHA,  // blended: transparent texels lend no colour
};

// HEAT HAZE over the flames drawn with AddHeat: the ground's texel grid in logical pixels (a texel's size and the
// world texel at the screen's top left), seconds, the offset's amplitude and the ripples' size in ground texels,
// how often they change (Hz), and how far the heat rises above the flames (blocks).
struct RHazeParams
{
    f32 texel, worldX, worldY;
    f32 time, amplitude, ripple, speed, rise;
};

struct RTexture;

class IRenderer
{
  public:
    virtual ~IRenderer() {}

    // Texture contents are given as RGBA8, top row first. The backend may defer GL work.
    virtual RTexture* CreateTexture() = 0;
    virtual void UploadTexture(RTexture* pTex, s32 width, s32 height, const u32* pRgba, RMipmaps mipmaps) = 0;
    virtual void FreeTexture(RTexture* pTex) = 0;

    virtual void BeginScene() = 0;
    virtual void EndScene() = 0;

    // Triangles (count multiple of 3).
    virtual void DrawTriangles(const RVertex* pVerts, s32 count, RTexture* pTex, u32 state) = 0;

    virtual void SetClipRect(f32 left, f32 top, f32 right, f32 bottom) = 0;

    virtual const RenderOptions& Options() const = 0;
    // Lights for per pixel lighting; ambient red, green and blue in 0..255 units as gbh_SetAmbient stores it,
    // and how far it takes on the unlit colour (gbh_api.cpp's Ambient).
    virtual void SetLights(const RLight* pLights, s32 count, const f32* pAmbient, f32 unlitAmbient) = 0;
    // Ambient occlusion: the map's blocks, width x height x depth bytes (x fastest, 255 solid) of one world
    // unit each, and how much of the ambient light the most occluded surfaces lose (0 off; NULL cells keep the last).
    virtual void SetOcclusion(const u8* pCells, s32 width, s32 height, s32 depth, f32 strength) = 0;

    // Stats exposed through gbh_GetGlobals.
    virtual u32 GetDrawCallCount() const = 0;

    // Heat haze over what is drawn so far (RHazeParams); a backend without it draws nothing.
    virtual void DrawHaze(const RHazeParams&) {}
    // Triangles just drawn that are hot (flames), level 0..1: their texture's bright, opaque texels go into the heat
    // mask DrawHaze distorts over.
    virtual void AddHeat(const RVertex*, s32, RTexture*, f32) {}
    // RAIN PoC: bit 2 wet ground (tiles drawn with lit 1.25), bit 4 lights reflected on it; the camera's eye.
    virtual void SetRain(u32, f32, f32, f32, f32, f32, f32, f32) {}
    virtual void DrawRainReflect(f32) {}
};

// Lets the render DLL find the backend from the SVideo pointer the game hands to gbh_InitDLL.
static const u32 kGLVideoMagic = 0x4C47324Cu; // "L2GL"

struct GLVideoHeader
{
    SVideo mVideo; // must stay first: the game only sees an SVideo*
    u32 mMagic;
    IRenderer* mRenderer;
    const u32* mFlipCounter; // number of Vid_FlipBuffers calls so far
};
