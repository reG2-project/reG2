// Portable OpenGL 3.3 core renderer: logical size offscreen target, batched triangles,
// a CPU 16-bit surface overlay for direct pixel writes, gamma, present and frame capture.
// The offscreen target is stored top row first (y down) so edge ties follow D3D's top-left fill rule.
// Optional enhancements (RenderOptions): true colour target, bloom, per pixel lighting, mipmaps, MSAA, soft shadows.
#pragma once

#include "gl_funcs.hpp"
#include "renderer.hpp"
#include <vector>

struct RTexture
{
    GLuint mId;
    s32 mWidth;
    s32 mHeight;
    s32 mLevels; // mip levels uploaded, 1 for the base level only
};

class GLRenderer : public IRenderer
{
  public:
    GLRenderer();
    ~GLRenderer();

    // Loader comes from the platform context, which must be current.
    bool Init(GLFuncs::Loader loader, const char** ppError);
    void Shutdown();

    void SetLogicalSize(s32 width, s32 height);
    // Menus: magnified textures with transparency get smooth edges (SmoothSample in the shaders).
    void SetSmoothMagnify(bool smooth) { mSmoothMagnify = smooth; }
    s32 LogicalWidth() const { return mLogicalW; }
    s32 LogicalHeight() const { return mLogicalH; }
    // The render target: the logical frame at the size it is shown (sharp scaling), or the logical
    // size itself with GTA2GL_NATIVE=0.
    s32 TargetWidth() const { return mTargetW; }
    s32 TargetHeight() const { return mTargetH; }

    // Rectangle in logical pixels, right/bottom exclusive. Empty rect means everything.
    void Clear(u8 r, u8 g, u8 b, s32 left, s32 top, s32 right, s32 bottom);

    // 16-bit RGB565 pixels written by the game; key marks pixels that were not written.
    void CompositeSurface(const u16* pPixels, s32 pitchBytes, s32 width, s32 height, u16 key);

    // Post effects of the finished frame (bloom); call once before capture and Present.
    void FinishFrame();
    // Draws the finished frame into the default framebuffer, letterboxed.
    void Present(s32 drawableW, s32 drawableH);
    // Draws RGBA rows, top first, into the default framebuffer instead of the frame: as large as fits
    // with their shape, the rest black. ReadPresented reads it back, top row first.
    void PresentImage(const u8* pRgba, s32 width, s32 height, s32 drawableW, s32 drawableH);
    void ReadPresented(std::vector<u8>& out, s32 drawableW, s32 drawableH);
    // Call after the swap: waits for the previous frame, so at most one frame is queued ahead of the display.
    void ThrottleFrames();
    void SetGamma(f32 r, f32 g, f32 b);

    // RGBA8 rows of the finished frame, top row first.
    void ReadLogical(std::vector<u8>& out);

    // Pixel offset added to every vertex. D3D puts pixel centres on integers, GL on halves. The extra
    // 1/2048 resolves exact ties like the reference without catching the original's "w - 0.001" quad edges.
    void SetVertexOffset(f32 x, f32 y) { mOffsetX = x; mOffsetY = y; }

    // Texture sampling moved by this many pixels towards the top left of the pixel centre.
    void SetSampleShift(f32 pixels) { mSampleShift = pixels; }

    void SetOptions(const RenderOptions& options);
    // The most samples per pixel the render target can have (MSAA).
    s32 MaxSamples() const { return mMaxSamples; }

    // IRenderer
    RTexture* CreateTexture();
    void UploadTexture(RTexture* pTex, s32 width, s32 height, const u32* pRgba, RMipmaps mipmaps);
    void FreeTexture(RTexture* pTex);
    void BeginScene();
    void EndScene();
    void DrawTriangles(const RVertex* pVerts, s32 count, RTexture* pTex, u32 state);
    void SetClipRect(f32 left, f32 top, f32 right, f32 bottom);
    // The clip SetClipRect asked for, set again after Clear's own.
    void ApplyClip();
    u32 GetDrawCallCount() const { return mDrawCalls; }
    u32 GetUploadCount() const { return mUploads; }
    u32 GetVertexCount() const { return mVertices; }
    const RenderOptions& Options() const { return mOptions; }
    void SetLights(const RLight* pLights, s32 count, const f32* pAmbient, f32 unlitAmbient);
    void SetOcclusion(const u8* pCells, s32 width, s32 height, s32 depth, f32 strength);
    void DrawHaze(const RHazeParams& params);
    void AddHeat(const RVertex* pVerts, s32 count, RTexture* pTex, f32 level);

  private:
    void FreeHaze();
    bool HazeProgram();
    void Flush();
    void BindTarget();
    void ApplyState(u32 state);
    void CreateTargets();
    void CreateMultisampleTarget(GLenum colourFormat);
    void ResolveMultisample();
    void BlitColour(GLuint src, bool srcRenderbuffer, GLuint dstTex, s32 x0, s32 y0, s32 x1, s32 y1);
    void SetDrawBuffers();
    void ApplyShadows();
    void ApplyTextureFilter();
    void DrawFullscreen();
    GLuint CompileProgram(const char* vs, const char* fs, const char** ppError);

    GLFuncs gl;
    bool mReady;
    bool mClipping;
    f32 mClip[4];
    s32 mLogicalW;
    s32 mLogicalH;
    s32 mTargetW;
    s32 mTargetH;
    s32 mDrawableW;
    s32 mDrawableH;
    void UpdateTargetSize();

    GLuint mFbo;
    GLuint mFboTex;      // scene colour (attachment 0)
    GLuint mBloomSrcTex; // additive contributions (attachment 1, bloom only)
    GLuint mShadowTex;   // shadow mask (attachment 2, soft shadows only)
    GLuint mPostFbo;     // scene + bloom
    GLuint mPostTex;
    GLuint mBlurFbo[2];
    GLuint mBlurTex[2];
    s32 mBlurW;
    s32 mBlurH;
    bool mTargetsTrueColour;
    bool mTargetsBloom;
    bool mTargetsShadows;
    bool mPostValid;
    GLsync mFrameFence;
    // MSAA: the scene is drawn into these and resolved into mFbo by FinishFrame.
    GLuint mMsFbo;
    GLuint mMsColour[3];
    GLuint mBlitFbo[2]; // read and draw, for BlitColour
    s32 mTargetsSamples; // 0 without MSAA
    s32 mMaxSamples;

    // Scene programs: 0 faithful, 1 enhanced (bloom output, per pixel lighting).
    struct SceneProgram
    {
        GLuint mId;
        GLint mScale, mOffset, mAlphaTest, mUseTex, mSampleShift, mSmooth, mAdditive, mPerPixel, mNumLights, mAmbient, mUnlitAmbient, mShadow;
    };
    SceneProgram mScene[2];
    int mActiveScene;
    GLuint mProgram; // enhanced scene program
    GLuint mPresentProgram;
    GLint mUniGamma;
    GLint mUniFlipY;
    GLint mUniShadowMask;
    GLuint mDownProgram;
    GLint mUniDownTexel;
    GLuint mBlurProgram;
    GLint mUniBlurStep;
    GLuint mComposeProgram;
    GLint mUniBloomIntensity;
    GLuint mVao;
    GLuint mVbo;
    GLuint mLightUbo;
    GLuint mSamplerNearest;
    GLuint mSamplerLinear;
    GLuint mSamplerMip;
    f32 mMaxAnisotropy; // 1 without EXT_texture_filter_anisotropic
    std::vector<u32> mMipBuffer;
    GLuint mWhiteTex;
    GLuint mSurfaceTex;
    s32 mSurfaceW;
    s32 mSurfaceH;
    std::vector<u32> mSurfaceRgba;

    std::vector<RVertex> mBatch;
    RTexture* mBatchTex;
    u32 mBatchState;
    u32 mAppliedState;
    bool mStateValid;
    // While mStateValid, what the last Flush set that the next may keep (each call costs in a browser).
    f32 mFlushScale[2] = {0.0f, 0.0f};
    int mFlushSmooth = -1;

    f32 mOffsetX;
    f32 mOffsetY;
    f32 mSampleShift;
    bool mSmoothMagnify = false;
    RenderOptions mOptions;
    s32 mNumLights;
    f32 mAmbient[3];
    f32 mUnlitAmbient;
    // Ambient occlusion (SetOcclusion): the map's blocks as a 3D texture, the occlusion baked from them for the four
    // sides and for up (kAoBakeFS), the grid of the baked volumes, and the strength, 0 when off.
    GLuint mOccTex;
    GLuint mAoTex[2];
    GLuint mAoFbo;
    s32 mAoGrid[3];
    f32 mAoStrength;
    GLuint mAoBakeProgram;
    GLint mUniBakeOccScale;
    GLint mUniBakeZ;
    GLint mUniAoGrid;
    GLint mUniAo;
    GLint mUniAoDebug;
    f32 mGamma[3];
    u32 mDrawCalls;
    u32 mUploads;
    u32 mVertices;

    // Heat haze (haze.cpp): the copy of the scene colour it displaces.
    GLenum mColourFormat = GL_RGB565;
    GLuint mHazeProgram = 0;
    bool mHazeFailed = false;
    GLuint mHazeTex = 0;
    GLuint mHazeMaskTex = 0; // the soft shadow mask's copy, distorted with the scene by the heat mask
    GLuint mHazeNoise = 0;
    GLuint mHazeNoiseSampler = 0;
    s32 mHazeW = 0;
    s32 mHazeH = 0;
    GLenum mHazeFormat = 0;
    GLint mUniHazeGrid = -1, mUniHazeWave = -1, mUniHazeFlow = -1, mUniHazeClip = -1, mUniHazeRise = -1, mUniHazeDebug = -1;
    // The heat mask: hot triangles of the frame (AddHeat), drawn at a quarter of the target's size.
    struct HeatDraw
    {
        RTexture* mpTex;
        s32 mFirst, mCount;
    };
    struct HeatVertex
    {
        f32 x, y, u, v;
        u32 level; // in the top byte
    };
    std::vector<HeatVertex> mHeatVerts;
    std::vector<HeatVertex> mHeatSorted;
    std::vector<HeatDraw> mHeatDraws;
    GLuint mHeatProgram = 0;
    GLuint mHeatVao = 0;
    GLuint mHeatVbo = 0;
    GLuint mHeatFbo[2] = {0, 0}; // the flames, then the heat field gathered from them
    GLuint mHeatTex[2] = {0, 0};
    GLuint mGatherProgram = 0;
    GLint mUniGather = -1;
    s32 mHeatW = 0;
    s32 mHeatH = 0;
    GLint mUniHeatScale = -1;
};
