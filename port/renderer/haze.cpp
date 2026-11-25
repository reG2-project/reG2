// Heat haze: the flame sprites, as they are drawn, also go into a heat mask at a quarter of the target's size
// (AddHeat); the heat over a point gathered from the flames below it along the way they rise; then the scene drawn
// so far, copied around the flames, drawn back through a smooth noise offset that the heat scales, flowing the way
// the heat rises. The soft shadows' mask is distorted with it.
#include "gl_renderer.hpp"
#include <algorithm>
#include <math.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

namespace
{
const s32 kNoiseSize = 64;
const s32 kNoiseCells = 8; // lattice cells across the noise texture

// Hashed value in 0..1 for lattice point (x, y) of channel c, wrapping at kNoiseCells.
f32 Lattice(s32 x, s32 y, s32 c)
{
    u32 h = (u32)((x & (kNoiseCells - 1)) + kNoiseCells * (y & (kNoiseCells - 1))) * 0x9E3779B1u ^ (u32)c * 0x85EBCA77u;
    h ^= h >> 15;
    h *= 0x2C1B3C6Du;
    h ^= h >> 12;
    return (h & 0xFFFF) / 65535.0f;
}

// Smooth value noise in 0..1 that tiles over the texture.
f32 ValueNoise(f32 x, f32 y, s32 c)
{
    const s32 ix = (s32)floorf(x), iy = (s32)floorf(y);
    f32 fx = x - ix, fy = y - iy;
    fx = fx * fx * (3.0f - 2.0f * fx);
    fy = fy * fy * (3.0f - 2.0f * fy);
    const f32 a = Lattice(ix, iy, c) + (Lattice(ix + 1, iy, c) - Lattice(ix, iy, c)) * fx;
    const f32 b = Lattice(ix, iy + 1, c) + (Lattice(ix + 1, iy + 1, c) - Lattice(ix, iy + 1, c)) * fx;
    return a + (b - a) * fy;
}

const char* kHazeVS =
    "void main()\n"
    "{\n"
    "    vec2 p = vec2((gl_VertexID << 1) & 2, gl_VertexID & 2);\n"
    "    gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0);\n"
    "}\n";

const char* kHazeFS =
    "uniform sampler2D uTex;\n"
    "uniform sampler2D uNoise;\n"
    "uniform vec4 uGrid;\n" // texel size in target pixels, world texel at the target's (0, 0)
    "uniform vec4 uWave;\n" // seconds, amplitude in target pixels, noise cells per world texel, changes per second
    "uniform float uFlow;\n" // world texels a flow cycle carries the ripples up
    "uniform vec4 uClip;\n" // the copied rectangle, right and bottom exclusive
    "uniform sampler2D uHeat;\n" // the heat mask, a quarter of the target's size
    "uniform vec3 uRise;\n" // the target's centre, how far below a point its heat is gathered from, in target pixels
    "uniform int uDebug;\n" // the heat shown as a colour ramp (GTA2_HAZE_DEBUG=1)
    "uniform sampler2D uMask;\n" // the copy of the soft shadow mask
    "layout(location = 0) out vec4 oColor;\n"
    "layout(location = 2) out vec4 oShadow;\n"
    // A tiling two channel noise in -1..1 over world texels (8 cells across the texture), two layers drifting
    // apart about one cell per change: fine ripples that slowly change shape.
    "vec2 Turbulence(vec2 w)\n"
    "{\n"
    "    vec2 uv = w * uWave.z / 8.0;\n"
    "    float t = uWave.x * uWave.w / 8.0;\n"
    "    vec2 a = texture(uNoise, uv + vec2(0.6, 0.35) * t).rg;\n"
    "    vec2 b = texture(uNoise, mat2(0.8, -0.6, 0.6, 0.8) * uv * 2.2 + vec2(-0.45, 0.7) * t).rg;\n"
    "    return (a * 0.65 + b * 0.35) * 2.0 - 1.0;\n"
    "}\n"
    // The heat (kGatherFS) scales a smooth noise offset that flows the way the flames rise; two phases half a cycle
    // apart, each weighted 0 at its reset.
    "void main()\n"
    "{\n"
    "    vec2 p = gl_FragCoord.xy;\n"
    "    vec2 size = vec2(textureSize(uTex, 0));\n"
    "    vec2 dir = normalize(vec2(0.0, -1.0) + 0.5 * (p - uRise.xy) / size.y);\n"
    "    float h = texture(uHeat, p / size).r;\n"
    "    if (h < 0.02) discard;\n"
    "    vec2 world = uGrid.zw + p / uGrid.xy;\n"
    "    vec2 flow = dir * uFlow;\n"
    "    float t = uWave.x * uWave.w * 0.5;\n"
    "    float pa = fract(t);\n"
    "    float pb = fract(t + 0.5);\n"
    "    float wa = 1.0 - abs(1.0 - 2.0 * pa);\n"
    "    vec2 n = Turbulence(world - flow * pa) * wa + Turbulence(world - flow * pb + vec2(23.0, 41.0)) * (1.0 - wa);\n"
    "    vec2 src = clamp(p + n * uWave.y * h, uClip.xy + 0.5, uClip.zw - 0.5);\n"
    "    oColor = texture(uTex, src / size);\n"
    "    oShadow = vec4(texture(uMask, src / size).r);\n"
    "    if (uDebug != 0 && h > 0.01)\n"
    "    {\n"
    "        vec3 ramp = h < 0.5 ? mix(vec3(0.0, 0.3, 1.0), vec3(0.0, 1.0, 0.2), h * 2.0) : mix(vec3(1.0, 1.0, 0.0), vec3(1.0, 0.0, 0.0), h * 2.0 - 1.0);\n"
    "        oColor = vec4(mix(oColor.rgb, ramp, 0.6), 1.0);\n"
    "    }\n"
    "}\n";
// The heat at a point, at the mask's size: the flame below it along the way the flames rise (up the screen,
// leaning out from its centre with the perspective, as walls do), the nearer the more, in close steps so it stays
// one smooth field. So it is weak at the flames' base, strongest at their top, trails off above them and is
// nothing below.
const char* kGatherFS =
    "uniform sampler2D uHeat;\n"
    "uniform vec4 uGather;\n" // the target's size, how far below a point its heat is gathered from (target pixels)
    "out vec4 oColor;\n"
    "void main()\n"
    "{\n"
    "    vec2 size = uGather.xy;\n"
    "    vec2 p = gl_FragCoord.xy * size / vec2(textureSize(uHeat, 0));\n"
    "    vec2 dir = normalize(vec2(0.0, -1.0) + 0.5 * (p - 0.5 * size) / size.y) * uGather.z / 12.0;\n"
    "    vec2 side = vec2(-dir.y, dir.x);\n"
    "    float sum = 0.0;\n"
    "    for (int k = 1; k <= 12; k++)\n"
    "    {\n"
    "        sum += texture(uHeat, (p - dir * float(k) + side * float((k & 1) * 2 - 1)) / size).r * (13.0 - float(k));\n"
    "    }\n"
    "    oColor = vec4(max(min(sum / 39.0, 1.0), 0.2 * texture(uHeat, p / size).r));\n"
    "}\n";

const char* kHeatVS =
    "layout(location = 0) in vec2 aPos;\n"
    "layout(location = 1) in vec4 aCol;\n"
    "layout(location = 2) in vec2 aUV;\n"
    "uniform vec2 uScale;\n"
    "out vec2 vUV;\n"
    "out float vLevel;\n"
    "void main()\n"
    "{\n"
    "    vec2 p = (aPos + 0.5) * uScale;\n"
    "    gl_Position = vec4(p - 1.0, 0.0, 1.0);\n"
    "    vUV = aUV;\n"
    "    vLevel = aCol.a;\n"
    "}\n";

// Hot triangles into the heat mask: their texture's opaque, fiery texels (red well above blue: flames, not smoke
// or the black of added sprites), the hottest kept where they overlap.
const char* kHeatFS =
    "uniform sampler2D uTex;\n"
    "in vec2 vUV;\n"
    "in float vLevel;\n"
    "out vec4 oColor;\n"
    "void main()\n"
    "{\n"
    "    vec4 t = texture(uTex, vUV);\n"
    "    oColor = vec4(t.a * smoothstep(0.2, 0.6, t.r - 0.6 * t.b) * vLevel);\n"
    "}\n";

const s32 kMaxHeatVerts = 6 * 1024;
} // namespace

void GLRenderer::AddHeat(const RVertex* pVerts, s32 count, RTexture* pTex, f32 level)
{
    if (!mReady || count <= 0 || level <= 0.0f || !pTex || !pTex->mId || (s32)mHeatVerts.size() + count > kMaxHeatVerts)
    {
        return;
    }
    // Screen position, texture position and the level: small, so uploading them never waits on the driver. The
    // level rides in the vertices, so the mask is drawn in one call per texture.
    const u32 heat = (u32)((level > 1.0f ? 1.0f : level) * 255.0f + 0.5f) << 24;
    const s32 first = (s32)mHeatVerts.size();
    for (s32 i = 0; i < count; i++)
    {
        const HeatVertex v = {pVerts[i].x, pVerts[i].y, pVerts[i].u, pVerts[i].v, heat};
        mHeatVerts.push_back(v);
    }
    if (!mHeatDraws.empty() && mHeatDraws.back().mpTex == pTex)
    {
        mHeatDraws.back().mCount += count;
        return;
    }
    const HeatDraw draw = {pTex, first, count};
    mHeatDraws.push_back(draw);
}

void GLRenderer::DrawHaze(const RHazeParams& params)
{
    if (!mReady || mHeatDraws.empty() || mHazeFailed || params.texel <= 0.0f || params.amplitude <= 0.0f || params.speed <= 0.0f)
    {
        mHeatVerts.clear();
        mHeatDraws.clear();
        return;
    }
    const f32 sx = (f32)mTargetW / mLogicalW;
    const f32 sy = (f32)mTargetH / mLogicalH;
    const f32 tx = params.texel * sx;
    const f32 ty = params.texel * sy;
    const f32 px = fmaxf(fmaxf(tx, ty), 1.0f);
    const f32 rise = params.rise * 64.0f * px;
    // The flames' box, grown by the rise and as far as a pixel can be fetched from.
    f32 minX = 1e9f, minY = 1e9f, maxX = -1e9f, maxY = -1e9f;
    for (size_t i = 0; i < mHeatVerts.size(); i++)
    {
        minX = fminf(minX, mHeatVerts[i].x * sx), maxX = fmaxf(maxX, mHeatVerts[i].x * sx);
        minY = fminf(minY, mHeatVerts[i].y * sy), maxY = fmaxf(maxY, mHeatVerts[i].y * sy);
    }
    // The heat trails up (smaller y) and leans out at most half as far sideways; below, only the offsets reach.
    const f32 margin = params.amplitude * px + 4.0f;
    const s32 x0 = (s32)fmaxf(0.0f, floorf(minX - margin - 0.5f * rise)), x1 = (s32)fminf((f32)mTargetW, ceilf(maxX + margin + 0.5f * rise));
    const s32 y0 = (s32)fmaxf(0.0f, floorf(minY - margin - rise)), y1 = (s32)fminf((f32)mTargetH, ceilf(maxY + margin));
    Flush();
    if (x0 >= x1 || y0 >= y1 || !HazeProgram())
    {
        mHeatVerts.clear();
        mHeatDraws.clear();
        return;
    }
    if (!mHeatProgram)
    {
        const char* pError = NULL;
        mHeatProgram = CompileProgram(kHeatVS, kHeatFS, &pError);
        if (!mHeatProgram)
        {
            mHazeFailed = true;
            return;
        }
        mUniHeatScale = gl.glGetUniformLocation(mHeatProgram, "uScale");
        gl.glUseProgram(mHeatProgram);
        gl.glUniform1i(gl.glGetUniformLocation(mHeatProgram, "uTex"), 0);
        mGatherProgram = CompileProgram(kHazeVS, kGatherFS, &pError);
        if (!mGatherProgram)
        {
            mHazeFailed = true;
            return;
        }
        mUniGather = gl.glGetUniformLocation(mGatherProgram, "uGather");
        gl.glUseProgram(mGatherProgram);
        gl.glUniform1i(gl.glGetUniformLocation(mGatherProgram, "uHeat"), 0);
        gl.glGenFramebuffers(2, mHeatFbo);
        gl.glGenTextures(2, mHeatTex);
        // Its own buffer, the same size every frame, so the driver recycles it instead of allocating.
        gl.glGenVertexArrays(1, &mHeatVao);
        gl.glBindVertexArray(mHeatVao);
        gl.glGenBuffers(1, &mHeatVbo);
        gl.glBindBuffer(GL_ARRAY_BUFFER, mHeatVbo);
        gl.glBufferData(GL_ARRAY_BUFFER, kMaxHeatVerts * sizeof(HeatVertex), NULL, GL_STREAM_DRAW);
        gl.glEnableVertexAttribArray(0);
        gl.glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, sizeof(HeatVertex), (void*)offsetof(HeatVertex, x));
        gl.glEnableVertexAttribArray(1);
        gl.glVertexAttribPointer(1, 4, GL_UNSIGNED_BYTE, GL_TRUE, sizeof(HeatVertex), (void*)offsetof(HeatVertex, level));
        gl.glEnableVertexAttribArray(2);
        gl.glVertexAttribPointer(2, 2, GL_FLOAT, GL_FALSE, sizeof(HeatVertex), (void*)offsetof(HeatVertex, u));
    }
    const s32 heatW = (mTargetW + 3) / 4, heatH = (mTargetH + 3) / 4;
    if (heatW != mHeatW || heatH != mHeatH)
    {
        for (int i = 0; i < 2; i++)
        {
            gl.glBindTexture(GL_TEXTURE_2D, mHeatTex[i]);
            gl.glTexImage2D(GL_TEXTURE_2D, 0, GL_R8, heatW, heatH, 0, GL_RED, GL_UNSIGNED_BYTE, NULL);
            gl.glBindFramebuffer(GL_FRAMEBUFFER, mHeatFbo[i]);
            gl.glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, mHeatTex[i], 0);
        }
        mHeatW = heatW;
        mHeatH = heatH;
    }

    // The heat mask.
    const GLenum colour[1] = {GL_COLOR_ATTACHMENT0};
    gl.glDisable(GL_SCISSOR_TEST);
    gl.glBindFramebuffer(GL_FRAMEBUFFER, mHeatFbo[0]);
    gl.glDrawBuffers(1, colour);
    gl.glViewport(0, 0, mHeatW, mHeatH);
    const GLfloat zero[4] = {0.0f, 0.0f, 0.0f, 0.0f};
    gl.glClearBufferfv(GL_COLOR, 0, zero);
    gl.glEnable(GL_BLEND);
    gl.glBlendEquation(GL_MAX);
    gl.glBlendFunc(GL_ONE, GL_ONE);
    gl.glUseProgram(mHeatProgram);
    gl.glUniform2f(mUniHeatScale, 2.0f / mLogicalW, 2.0f / mLogicalH);
    gl.glBindVertexArray(mHeatVao);
    gl.glBindBuffer(GL_ARRAY_BUFFER, mHeatVbo);
    // Grouped by texture: the flames share a few animation frames.
    std::stable_sort(mHeatDraws.begin(), mHeatDraws.end(), [](const HeatDraw& a, const HeatDraw& b) { return a.mpTex < b.mpTex; });
    mHeatSorted.clear();
    for (size_t i = 0; i < mHeatDraws.size(); i++)
    {
        const HeatDraw& d = mHeatDraws[i];
        mHeatSorted.insert(mHeatSorted.end(), mHeatVerts.begin() + d.mFirst, mHeatVerts.begin() + d.mFirst + d.mCount);
    }
    gl.glBufferData(GL_ARRAY_BUFFER, kMaxHeatVerts * sizeof(HeatVertex), NULL, GL_STREAM_DRAW);
    // In pieces of 4 KB: a threaded GL driver copies small uploads along with the commands, a large one waits for it.
    const size_t bytes = mHeatSorted.size() * sizeof(HeatVertex);
    for (size_t at = 0; at < bytes; at += 4096)
    {
        gl.glBufferSubData(GL_ARRAY_BUFFER, (GLintptr)at, (GLsizeiptr)(bytes - at < 4096 ? bytes - at : 4096), (const u8*)&mHeatSorted[0] + at);
    }
    gl.glActiveTexture(GL_TEXTURE0);
    gl.glBindSampler(0, mSamplerLinear);
    for (size_t i = 0, first = 0; i < mHeatDraws.size();)
    {
        RTexture* pTex = mHeatDraws[i].mpTex;
        size_t count = 0;
        for (; i < mHeatDraws.size() && mHeatDraws[i].mpTex == pTex; i++)
        {
            count += mHeatDraws[i].mCount;
        }
        gl.glBindTexture(GL_TEXTURE_2D, pTex->mId);
        gl.glDrawArrays(GL_TRIANGLES, (GLint)first, (GLsizei)count);
        first += count;
    }
    gl.glBlendEquation(GL_FUNC_ADD);
    gl.glDisable(GL_BLEND);
    mHeatVerts.clear();
    mHeatDraws.clear();

    // The heat field, at the mask's size too, only where the haze is drawn.
    gl.glBindFramebuffer(GL_FRAMEBUFFER, mHeatFbo[1]);
    gl.glDrawBuffers(1, colour);
    gl.glEnable(GL_SCISSOR_TEST);
    gl.glScissor(x0 / 4 - 1, y0 / 4 - 1, (x1 - x0) / 4 + 3, (y1 - y0) / 4 + 3);
    gl.glUseProgram(mGatherProgram);
    gl.glUniform4f(mUniGather, (f32)mTargetW, (f32)mTargetH, rise, 0.0f);
    gl.glBindTexture(GL_TEXTURE_2D, mHeatTex[0]);
    DrawFullscreen();
    gl.glDisable(GL_SCISSOR_TEST);

    // The scene under it, as far as the offsets reach.
    const s32 reach = (s32)ceilf(params.amplitude * px) + 2;
    const s32 c0 = x0 - reach > 0 ? x0 - reach : 0, c1 = x1 + reach < mTargetW ? x1 + reach : mTargetW;
    const s32 r0 = y0 - reach > 0 ? y0 - reach : 0, r1 = y1 + reach < mTargetH ? y1 + reach : mTargetH;
    const bool ms = mTargetsSamples != 0;
    BlitColour(ms ? mMsColour[0] : mFboTex, ms, mHazeTex, c0, r0, c1, r1);
    if (mTargetsShadows)
    {
        // The soft shadows are applied at the end of the frame (ApplyShadows): their mask is distorted too.
        BlitColour(ms ? mMsColour[2] : mShadowTex, ms, mHazeMaskTex, c0, r0, c1, r1);
    }

    static const char* spDebug = getenv("GTA2_HAZE_DEBUG");
    BindTarget();
    const GLenum both[3] = {GL_COLOR_ATTACHMENT0, GL_NONE, mTargetsShadows ? (GLenum)GL_COLOR_ATTACHMENT2 : (GLenum)GL_NONE};
    gl.glDrawBuffers(3, both);
    gl.glEnable(GL_SCISSOR_TEST);
    gl.glScissor(x0, y0, x1 - x0, y1 - y0);
    gl.glUseProgram(mHazeProgram);
    gl.glUniform4f(mUniHazeGrid, tx, ty, params.worldX, params.worldY);
    const f32 ripple = params.ripple * px;
    gl.glUniform4f(mUniHazeWave, params.time, params.amplitude * px, tx / ripple, params.speed);
    gl.glUniform1f(mUniHazeFlow, 0.5f * ripple / tx);
    gl.glUniform4f(mUniHazeClip, (f32)c0, (f32)r0, (f32)c1, (f32)r1);
    gl.glUniform3f(mUniHazeRise, 0.5f * mTargetW, 0.5f * mTargetH, rise);
    gl.glUniform1i(mUniHazeDebug, spDebug && atoi(spDebug) ? 1 : 0);
    gl.glActiveTexture(GL_TEXTURE3);
    gl.glBindTexture(GL_TEXTURE_2D, mHazeMaskTex);
    gl.glBindSampler(3, mSamplerLinear);
    gl.glActiveTexture(GL_TEXTURE2);
    gl.glBindTexture(GL_TEXTURE_2D, mHeatTex[1]);
    gl.glBindSampler(2, mSamplerLinear);
    gl.glActiveTexture(GL_TEXTURE1);
    gl.glBindTexture(GL_TEXTURE_2D, mHazeNoise);
    gl.glBindSampler(1, mHazeNoiseSampler);
    gl.glActiveTexture(GL_TEXTURE0);
    gl.glBindTexture(GL_TEXTURE_2D, mHazeTex);
    gl.glBindSampler(0, mSamplerLinear);
    DrawFullscreen();
    gl.glBindSampler(1, 0);
    gl.glBindSampler(2, 0);
    gl.glBindSampler(3, 0);
    gl.glDisable(GL_SCISSOR_TEST);
    SetDrawBuffers();
    mStateValid = false;
}

void GLRenderer::FreeHaze()
{
    gl.glDeleteProgram(mHazeProgram);
    gl.glDeleteTextures(1, &mHazeTex);
    gl.glDeleteTextures(1, &mHazeMaskTex);
    mHazeMaskTex = 0;
    gl.glDeleteTextures(1, &mHazeNoise);
    gl.glDeleteSamplers(1, &mHazeNoiseSampler);
    gl.glDeleteProgram(mHeatProgram);
    gl.glDeleteBuffers(1, &mHeatVbo);
    gl.glDeleteVertexArrays(1, &mHeatVao);
    mHeatVbo = 0;
    mHeatVao = 0;
    gl.glDeleteProgram(mGatherProgram);
    gl.glDeleteFramebuffers(2, mHeatFbo);
    gl.glDeleteTextures(2, mHeatTex);
    mGatherProgram = 0;
    mHeatProgram = 0;
    mHeatFbo[0] = mHeatFbo[1] = 0;
    mHeatTex[0] = mHeatTex[1] = 0;
    mHeatW = mHeatH = 0;
    mHazeNoise = 0;
    mHazeNoiseSampler = 0;
    mHazeProgram = 0;
    mHazeTex = 0;
    mHazeW = mHazeH = 0;
    mHazeFormat = 0;
}

// The haze program, its noise and the copy of the scene at the target's size; false if it cannot be had.
bool GLRenderer::HazeProgram()
{
    if (!mHazeProgram)
    {
        const char* pError = NULL;
        mHazeProgram = CompileProgram(kHazeVS, kHazeFS, &pError);
        if (!mHazeProgram)
        {
            mHazeFailed = true;
            return false;
        }
        mUniHazeGrid = gl.glGetUniformLocation(mHazeProgram, "uGrid");
        mUniHazeWave = gl.glGetUniformLocation(mHazeProgram, "uWave");
        mUniHazeFlow = gl.glGetUniformLocation(mHazeProgram, "uFlow");
        mUniHazeClip = gl.glGetUniformLocation(mHazeProgram, "uClip");
        gl.glUseProgram(mHazeProgram);
        gl.glUniform1i(gl.glGetUniformLocation(mHazeProgram, "uTex"), 0);
        gl.glUniform1i(gl.glGetUniformLocation(mHazeProgram, "uNoise"), 1);
        gl.glUniform1i(gl.glGetUniformLocation(mHazeProgram, "uHeat"), 2);
        gl.glUniform1i(gl.glGetUniformLocation(mHazeProgram, "uMask"), 3);
        mUniHazeRise = gl.glGetUniformLocation(mHazeProgram, "uRise");
        mUniHazeDebug = gl.glGetUniformLocation(mHazeProgram, "uDebug");
        gl.glGenTextures(1, &mHazeTex);
        gl.glGenTextures(1, &mHazeMaskTex);
        static u8 noise[kNoiseSize * kNoiseSize * 2];
        for (s32 y = 0; y < kNoiseSize; y++)
        {
            for (s32 x = 0; x < kNoiseSize; x++)
            {
                for (s32 c = 0; c < 2; c++)
                {
                    const f32 v = ValueNoise((x + 0.5f) * kNoiseCells / kNoiseSize, (y + 0.5f) * kNoiseCells / kNoiseSize, c);
                    noise[(y * kNoiseSize + x) * 2 + c] = (u8)(v * 255.0f + 0.5f);
                }
            }
        }
        gl.glGenTextures(1, &mHazeNoise);
        gl.glBindTexture(GL_TEXTURE_2D, mHazeNoise);
        gl.glTexImage2D(GL_TEXTURE_2D, 0, GL_RG8, kNoiseSize, kNoiseSize, 0, GL_RG, GL_UNSIGNED_BYTE, noise);
        gl.glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, 0);
        gl.glGenSamplers(1, &mHazeNoiseSampler);
        gl.glSamplerParameteri(mHazeNoiseSampler, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        gl.glSamplerParameteri(mHazeNoiseSampler, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        gl.glSamplerParameteri(mHazeNoiseSampler, GL_TEXTURE_WRAP_S, GL_REPEAT);
        gl.glSamplerParameteri(mHazeNoiseSampler, GL_TEXTURE_WRAP_T, GL_REPEAT);
    }
    if (mHazeW != mTargetW || mHazeH != mTargetH || mHazeFormat != mColourFormat)
    {
        // The scene's format, as a multisample resolve needs.
        const bool rgb565 = mColourFormat == GL_RGB565;
        gl.glBindTexture(GL_TEXTURE_2D, mHazeTex);
        gl.glTexImage2D(GL_TEXTURE_2D, 0, mColourFormat, mTargetW, mTargetH, 0, rgb565 ? GL_RGB : GL_RGBA,
                        rgb565 ? GL_UNSIGNED_SHORT_5_6_5 : GL_UNSIGNED_BYTE, NULL);
        gl.glBindTexture(GL_TEXTURE_2D, mHazeMaskTex);
        gl.glTexImage2D(GL_TEXTURE_2D, 0, GL_R8, mTargetW, mTargetH, 0, GL_RED, GL_UNSIGNED_BYTE, NULL);
        mHazeW = mTargetW;
        mHazeH = mTargetH;
        mHazeFormat = mColourFormat;
    }
    return true;
}

// RAIN PoC: the bright things of the scene drawn so far glow softly on the wet ground round them. At half size, a
// wet pixel gathers its surroundings with jittered samples; blurred, then added where the scene shader left the
// ground wet (the bloom target's alpha, 1 - wet).
namespace
{
const char* kRainFS =
    "uniform sampler2D uTex;\n"
    "uniform sampler2D uMask;\n"
    "uniform vec4 uRain;\n" // centre in target pixels, seconds, target pixels per logical pixel
    "layout(location = 0) out vec4 oColor;\n"
    "void main()\n"
    "{\n"
    "    vec2 size = vec2(textureSize(uTex, 0));\n"
    "    vec2 p = gl_FragCoord.xy * 2.0;\n"
    "    if (texture(uMask, p / size).a > 0.97) { oColor = vec4(0.0); return; }\n"
    "    vec2 d = p - uRain.xy;\n"
    // A soft round gather, a little taller than wide: bright things glow on the wet ground round them, with no
    // direction of their own (the headlights' streaks along their beams are the scene shader's).
    "    float rad = 26.0 * uRain.w;\n"
    // Interleaved gradient noise: every pixel samples between the others' samples, the blur evens it out.
    "    float ign = fract(52.9829189 * fract(dot(gl_FragCoord.xy, vec2(0.06711056, 0.00583715))));\n"
    "    vec3 acc = vec3(0.0);\n"
    "    float wsum = 0.0;\n"
    "    for (int i = 0; i < 24; i++)\n"
    "    {\n"
    "        float s = sqrt((float(i) + ign) / 24.0);\n"
    "        float a = (float(i) + ign) * 2.39996;\n"
    "        vec2 q = (p + vec2(cos(a), sin(a) * 1.3) * rad * s) / size;\n"
    "        vec4 m = texture(uMask, q);\n"
    "        vec3 c = texture(uTex, q).rgb;\n"
    "        float l = max(c.r, max(c.g, c.b));\n"
    "        float solid = smoothstep(0.9, 0.99, m.a);\n"
    // What stands out of the water (lamps, lit walls, the cars' lights), and faintly the brightest lit ground.
    "        vec3 src = c * mix(0.15 * smoothstep(0.6, 1.0, l), 0.1 + 2.5 * smoothstep(0.6, 1.0, l), solid) + m.rgb;\n"
    "        float wgt = 1.0 - s;\n"
    "        acc += src * wgt;\n"
    "        wsum += wgt;\n"
    "    }\n"
"    oColor = vec4(min(acc / wsum, vec3(0.5)), 1.0);\n"
    "}\n";

const char* kRainBlurFS =
    "uniform sampler2D uTex;\n"
    "layout(location = 0) out vec4 oColor;\n"
    "void main()\n"
    "{\n"
    "    vec2 t = 1.0 / vec2(textureSize(uTex, 0));\n"
    "    vec2 uv = gl_FragCoord.xy * t;\n"
    "    vec4 c = texture(uTex, uv) * 0.2;\n"
    "    c += (texture(uTex, uv + t * vec2(1.5, 0.5)) + texture(uTex, uv + t * vec2(-0.5, 1.5)) + texture(uTex, uv + t * vec2(-1.5, -0.5)) + texture(uTex, uv + t * vec2(0.5, -1.5))) * 0.2;\n"
    "    oColor = c;\n"
    "}\n";

const char* kRainComposeFS =
    "uniform sampler2D uTex;\n"
    "uniform sampler2D uMask;\n"
    "uniform float uGain;\n"
    "layout(location = 0) out vec4 oColor;\n"
    "void main()\n"
    "{\n"
    "    vec2 size = vec2(textureSize(uMask, 0));\n"
    "    float wet = 1.0 - texture(uMask, gl_FragCoord.xy / size).a;\n"
    "    if (wet < 0.03) discard;\n"
    "    oColor = vec4(texture(uTex, gl_FragCoord.xy / size).rgb * wet * uGain, 1.0);\n"
    "}\n";

GLuint sRainProgram, sRainBlur, sRainCompose, sRainFbo, sRainTex[2], sHalfFbo[2], sHalfTex[2];
GLint sUniRain = -1, sUniGain = -1;
s32 sRainW, sRainH;
GLenum sRainFormat;
} // namespace

void GLRenderer::DrawRainReflect(f32 time)
{
    if (!mReady || !mTargetsBloom)
    {
        return;
    }
    Flush();
    if (!sRainProgram)
    {
        const char* pError = NULL;
        sRainProgram = CompileProgram(kHazeVS, kRainFS, &pError);
        sRainBlur = CompileProgram(kHazeVS, kRainBlurFS, &pError);
        sRainCompose = CompileProgram(kHazeVS, kRainComposeFS, &pError);
        if (!sRainProgram || !sRainBlur || !sRainCompose)
        {
            return;
        }
        sUniRain = gl.glGetUniformLocation(sRainProgram, "uRain");
        gl.glUseProgram(sRainProgram);
        gl.glUniform1i(gl.glGetUniformLocation(sRainProgram, "uTex"), 0);
        gl.glUniform1i(gl.glGetUniformLocation(sRainProgram, "uMask"), 1);
        gl.glUseProgram(sRainBlur);
        gl.glUniform1i(gl.glGetUniformLocation(sRainBlur, "uTex"), 0);
        gl.glUseProgram(sRainCompose);
        gl.glUniform1i(gl.glGetUniformLocation(sRainCompose, "uTex"), 0);
        gl.glUniform1i(gl.glGetUniformLocation(sRainCompose, "uMask"), 1);
        sUniGain = gl.glGetUniformLocation(sRainCompose, "uGain");
        gl.glGenFramebuffers(1, &sRainFbo);
        gl.glGenTextures(2, sRainTex);
        gl.glGenFramebuffers(2, sHalfFbo);
        gl.glGenTextures(2, sHalfTex);
    }
    const s32 halfW = (mTargetW + 1) / 2, halfH = (mTargetH + 1) / 2;
    if (sRainW != mTargetW || sRainH != mTargetH || sRainFormat != mColourFormat)
    {
        const bool rgb565 = mColourFormat == GL_RGB565;
        gl.glBindTexture(GL_TEXTURE_2D, sRainTex[0]);
        gl.glTexImage2D(GL_TEXTURE_2D, 0, mColourFormat, mTargetW, mTargetH, 0, rgb565 ? GL_RGB : GL_RGBA,
                        rgb565 ? GL_UNSIGNED_SHORT_5_6_5 : GL_UNSIGNED_BYTE, NULL);
        gl.glBindTexture(GL_TEXTURE_2D, sRainTex[1]);
        gl.glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, mTargetW, mTargetH, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
        gl.glBindFramebuffer(GL_FRAMEBUFFER, sRainFbo);
        gl.glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, sRainTex[0], 0);
        gl.glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT1, GL_TEXTURE_2D, sRainTex[1], 0);
        for (int i = 0; i < 2; i++)
        {
            gl.glBindTexture(GL_TEXTURE_2D, sHalfTex[i]);
            gl.glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, halfW, halfH, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
            gl.glBindFramebuffer(GL_FRAMEBUFFER, sHalfFbo[i]);
            gl.glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, sHalfTex[i], 0);
        }
        sRainW = mTargetW;
        sRainH = mTargetH;
        sRainFormat = mColourFormat;
    }
    gl.glDisable(GL_SCISSOR_TEST);
    gl.glDisable(GL_BLEND);
    gl.glBindFramebuffer(GL_READ_FRAMEBUFFER, mTargetsSamples ? mMsFbo : mFbo);
    gl.glBindFramebuffer(GL_DRAW_FRAMEBUFFER, sRainFbo);
    for (int i = 0; i < 2; i++)
    {
        const GLenum bufs[2] = {i == 0 ? (GLenum)GL_COLOR_ATTACHMENT0 : (GLenum)GL_NONE, i == 1 ? (GLenum)GL_COLOR_ATTACHMENT1 : (GLenum)GL_NONE};
        gl.glReadBuffer(GL_COLOR_ATTACHMENT0 + i);
        gl.glDrawBuffers(2, bufs);
        gl.glBlitFramebuffer(0, 0, mTargetW, mTargetH, 0, 0, mTargetW, mTargetH, GL_COLOR_BUFFER_BIT, GL_NEAREST);
    }
    gl.glReadBuffer(GL_COLOR_ATTACHMENT0);
    const GLenum one[1] = {GL_COLOR_ATTACHMENT0};

    // The gather, at half size.
    gl.glBindFramebuffer(GL_FRAMEBUFFER, sHalfFbo[0]);
    gl.glDrawBuffers(1, one);
    gl.glViewport(0, 0, halfW, halfH);
    gl.glUseProgram(sRainProgram);
    gl.glUniform4f(sUniRain, 0.5f * mTargetW, 0.5f * mTargetH, time, (f32)mTargetW / mLogicalW);
    gl.glActiveTexture(GL_TEXTURE1);
    gl.glBindTexture(GL_TEXTURE_2D, sRainTex[1]);
    gl.glBindSampler(1, mSamplerLinear);
    gl.glActiveTexture(GL_TEXTURE0);
    gl.glBindTexture(GL_TEXTURE_2D, sRainTex[0]);
    gl.glBindSampler(0, mSamplerLinear);
    DrawFullscreen();

    // Two blurs.
    gl.glUseProgram(sRainBlur);
    for (int i = 0; i < 2; i++)
    {
        gl.glBindFramebuffer(GL_FRAMEBUFFER, sHalfFbo[1 - i]);
        gl.glDrawBuffers(1, one);
        gl.glBindTexture(GL_TEXTURE_2D, sHalfTex[i]);
        DrawFullscreen();
    }

    // Added over the wet ground.
    BindTarget();
    gl.glViewport(0, 0, mTargetW, mTargetH);
    const GLenum colour[3] = {GL_COLOR_ATTACHMENT0, GL_NONE, GL_NONE};
    gl.glDrawBuffers(3, colour);
    gl.glEnable(GL_BLEND);
    gl.glBlendEquation(GL_FUNC_ADD);
    gl.glBlendFunc(GL_ONE, GL_ONE);
    gl.glUseProgram(sRainCompose);
    gl.glUniform1f(sUniGain, 0.8f * mRainParams[1]);
    gl.glBindTexture(GL_TEXTURE_2D, sHalfTex[0]);
    DrawFullscreen();
    gl.glBindSampler(1, 0);
    gl.glDisable(GL_BLEND);
    SetDrawBuffers();
    mStateValid = false;
}
