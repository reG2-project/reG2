#include "gl_renderer.hpp"
#include <math.h>
#include <stdlib.h>
#include <string.h>


namespace
{
// Faithful path: kept exactly as validated against the reference (any change of the varyings can
// move a rasterisation tie).
const char* kFaithfulVS =
    "layout(location = 0) in vec3 aPos;\n"
    "layout(location = 1) in vec4 aCol;\n"
    "layout(location = 2) in vec2 aUV;\n"
    "uniform vec2 uScale;\n"
    "uniform vec2 uOffset;\n"
    "out vec4 vCol;\n"
    "out vec2 vUV;\n"
    "void main()\n"
    "{\n"
    "    float w = 1.0 / max(aPos.z, 1e-7);\n"
    "    vec2 p = (aPos.xy + uOffset) * uScale;\n"
    "    gl_Position = vec4((p.x - 1.0) * w, (p.y - 1.0) * w, 0.0, w);\n"
    "    vCol = aCol;\n"
    "    vUV = aUV;\n"
    "}\n";

// Smooth magnification for the menus: the colour is the bilinear mix of the visible texels only
// (transparent texels keep their palette colour), and the edge is one screen pixel wide where the
// bilinear alpha crosses one half, so magnified glyphs are neither blurred nor blocky. Opaque
// texels get the plain bilinear sample.
#define SMOOTH_SAMPLE \
    "uniform int uSmooth;\n" \
    "vec4 SmoothSample(vec2 uv)\n" \
    "{\n" \
    "    vec4 c = texture(uTex, uv);\n" \
    "    vec2 size = vec2(textureSize(uTex, 0));\n" \
    "    vec2 p = uv * size - 0.5;\n" \
    "    float scale = 1.0 / max(max(fwidth(p).x, fwidth(p).y), 1e-6);\n" \
    "    if (uSmooth == 0 || scale <= 1.0 || c.a >= 0.999)\n" \
    "        return c;\n" \
    "    ivec2 i0 = ivec2(floor(p));\n" \
    "    ivec2 hi = ivec2(size) - 1;\n" \
    "    vec2 f = fract(p);\n" \
    "    vec4 t00 = texelFetch(uTex, clamp(i0, ivec2(0), hi), 0);\n" \
    "    vec4 t10 = texelFetch(uTex, clamp(i0 + ivec2(1, 0), ivec2(0), hi), 0);\n" \
    "    vec4 t01 = texelFetch(uTex, clamp(i0 + ivec2(0, 1), ivec2(0), hi), 0);\n" \
    "    vec4 t11 = texelFetch(uTex, clamp(i0 + ivec2(1, 1), ivec2(0), hi), 0);\n" \
    "    vec4 w = vec4((1.0 - f.x) * (1.0 - f.y), f.x * (1.0 - f.y), (1.0 - f.x) * f.y, f.x * f.y);\n" \
    "    vec4 wa = w * vec4(t00.a, t10.a, t01.a, t11.a);\n" \
    "    float a = wa.x + wa.y + wa.z + wa.w;\n" \
    "    vec3 rgb = (t00.rgb * wa.x + t10.rgb * wa.y + t01.rgb * wa.z + t11.rgb * wa.w) / max(a, 1e-5);\n" \
    "    float e = 0.5 / scale;\n" \
    "    return vec4(rgb, smoothstep(0.5 - e, 0.5 + e, a));\n" \
    "}\n"

const char* kFaithfulFS =
    "uniform sampler2D uTex;\n"
    SMOOTH_SAMPLE
    "uniform int uAlphaTest;\n"
    "uniform int uUseTex;\n"
    "uniform float uSampleShift;\n"
    "in vec4 vCol;\n"
    "in vec2 vUV;\n"
    "out vec4 oColor;\n"
    "void main()\n"
    "{\n"
    "    vec2 uv = vUV - (dFdx(vUV) + dFdy(vUV)) * uSampleShift;\n"
    "    vec4 t = uUseTex != 0 ? SmoothSample(uv) : vec4(1.0);\n"
    "    if (uAlphaTest != 0 && t.a < 0.5) discard;\n"
    "    oColor = t * vCol;\n"
    "}\n";

// Enhanced path (bloom output, per pixel lighting).
const char* kSceneVS =
    "layout(location = 0) in vec3 aPos;\n"
    "layout(location = 1) in vec4 aCol;\n"
    "layout(location = 2) in vec2 aUV;\n"
    "layout(location = 3) in vec3 aWorld;\n"
    "layout(location = 4) in vec4 aUnlit;\n"
    "layout(location = 5) in float aShade;\n"
    "layout(location = 6) in float aLit;\n"
    "layout(location = 7) in float aAo;\n"
    "uniform vec2 uScale;\n"
    "uniform vec2 uOffset;\n"
    "out vec4 vCol;\n"
    "out vec2 vUV;\n"
    "out vec3 vWorld;\n"
    "out vec4 vUnlit;\n"
    "out float vShade;\n"
    "out float vLit;\n"
    "out float vAo;\n"
    "void main()\n"
    "{\n"
    "    float w = 1.0 / max(aPos.z, 1e-7);\n"
    "    vec2 p = (aPos.xy + uOffset) * uScale;\n"
    "    gl_Position = vec4((p.x - 1.0) * w, (p.y - 1.0) * w, 0.0, w);\n"
    "    vCol = aCol;\n"
    "    vUV = aUV;\n"
    "    vWorld = aWorld;\n"
    "    vUnlit = aUnlit;\n"
    "    vShade = aShade;\n"
    "    vLit = aLit;\n"
    "    vAo = aAo;\n"
    "}\n";

// Per pixel lighting is the original's LightVertex formula with a real square root.
const char* kSceneFS =
    "uniform sampler2D uTex;\n"
    SMOOTH_SAMPLE
    "uniform int uAlphaTest;\n"
    "uniform int uUseTex;\n"
    "uniform float uSampleShift;\n"
    "uniform int uAdditive;\n"
    "uniform int uPerPixel;\n"
    "uniform int uNumLights;\n"
    "uniform vec3 uAmbient;\n"
    "vec3 gHaze;\n"
    "vec3 gSpots;\n"
    "float gWet = 0.0;\n"
    "uniform float uUnlitAmbient;\n"
    "uniform int uShadow;\n"
    "layout(std140) uniform Lights\n"
    "{\n"
    "    vec4 uLightPos[256];\n" // xyz, radius
    "    vec4 uLightCol[256];\n" // rgb * brightness, the height it fades out above
    "    vec4 uLightDir[256];\n" // beam direction xy, width, 1 for a beam
    "};\n"
    "in vec4 vCol;\n"
    "in vec2 vUV;\n"
    "in vec3 vWorld;\n"
    "in vec4 vUnlit;\n"
    "in float vShade;\n"
    "in float vLit;\n"
    "in float vAo;\n"
    "uniform sampler3D uAoSides;\n"
    "uniform sampler3D uAoUp;\n"
    "uniform vec3 uAoGrid;\n"
    "uniform float uAo;\n"
    "uniform int uAoDebug;\n"
    "layout(location = 0) out vec4 oColor;\n"
    "layout(location = 1) out vec4 oBloom;\n"
    "layout(location = 2) out vec4 oShadow;\n"
    // smoke 1: a particle (FLAG_TINTED in gbh_api.cpp), above the lights' ceilings.
    "vec3 LightSum(float smoke)\n"
    "{\n"
    "    vec3 acc = vec3(0.0);\n"
    "    vec3 spots = vec3(0.0);\n"
    "    gHaze = vec3(0.0);\n"
    "    for (int i = 0; i < uNumLights; i++)\n"
    "    {\n"
    "        vec3 d = vWorld - uLightPos[i].xyz;\n"
    "        float d2 = dot(d, d);\n"
    "        float r = uLightPos[i].w;\n"
    "        if (d2 <= r * r)\n"
    "        {\n"
    "            float dist = sqrt(d2);\n"
    "            float att = (r - dist) / max(r, 1e-6);\n"
    // As BelowCeiling in gbh_api.cpp.
    "            float below = max(1.0 - smoothstep(uLightCol[i].w, uLightCol[i].w + 0.4, vWorld.z), smoke);\n"
    "            vec4 s = uLightDir[i];\n"
    // As Beam in gbh_api.cpp.
    "            float along = dot(d.xy, s.xy);\n"
    "            float ahead = max(along, 0.0);\n"
    "            float sigma = s.z + 0.58 * ahead;\n"
    "            float cone = exp(-ahead / (0.33 * r)) * exp(-max(dot(d.xy, d.xy) - along * along, 0.0) / (2.0 * sigma * sigma));\n"
    "            float glow = exp(-dot(d.xy, d.xy) / 0.0288);\n"
    "            float beam = smoothstep(-0.02, 0.08, along) * (1.0 - smoothstep(0.7 * r, r, along)) * (cone + glow) * below;\n"
    // A little of a beam's light is added as haze, on top of lighting the texture.
    "            if (s.w > 0.0) { spots += uLightCol[i].rgb * beam; gHaze += uLightCol[i].rgb * beam * 0.12; }\n"
    "            else if (att > 0.0) acc += uLightCol[i].rgb * att * below;\n"
    "        }\n"
    "    }\n"
    "    gSpots = min(spots, vec3(0.6));\n"
    "    return acc + min(spots, vec3(0.6));\n" // kSpotCap in gbh_api.cpp
    "}\n"
    // RAIN PoC: wet tiles (lit 1.25) facing up are a little darker and take a sheen from the lights: a soft pool
    // under each lamp, a streak along each headlight's beam, only from lights just above the surface. gWet tells
    // the screen space reflections how much the pixel mirrors.
    "uniform int uRain;\n"
    "uniform vec4 uRainCam;\n" // eye xyz in blocks, seconds
    "uniform vec4 uRainParams;\n" // ground z under the eye, glow scale, puddle cover
    // Dave Hoskins' hash without sine: the same on every GPU at the map's coordinates.
    "float RainHash(vec2 p)\n"
    "{\n"
    "    vec3 q = fract(vec3(p.xyx) * 0.1031);\n"
    "    q += dot(q, q.yzx + 33.33);\n"
    "    return fract((q.x + q.y) * q.z);\n"
    "}\n"
    "float RainNoise(vec2 p)\n"
    "{\n"
    "    vec2 i = floor(p), f = fract(p);\n"
    "    f = f * f * (3.0 - 2.0 * f);\n"
    "    return mix(mix(RainHash(i), RainHash(i + vec2(1.0, 0.0)), f.x), mix(RainHash(i + vec2(0.0, 1.0)), RainHash(i + vec2(1.0, 1.0)), f.x), f.y);\n"
    "}\n"
        // The wet glow round the lamps and neons at vWorld: a soft pool round the point under each, a little longer up
    // and down the screen, only from lights just above. The ground and the sprites standing in it both take it.
    "vec3 RainPools()\n"
    "{\n"
    "    vec3 g = vec3(0.0);\n"
    "    for (int i = 0; i < uNumLights; i++)\n"
    "    {\n"
    "        vec3 L = uLightPos[i].xyz;\n"
    "        float r = uLightPos[i].w;\n"
    "        vec3 dl = vWorld - L;\n"
    "        float dist = length(dl);\n"
    "        if (dist >= r || uLightDir[i].w > 0.0) continue;\n"
    "        float above = L.z - vWorld.z;\n"
    "        float near = smoothstep(-0.3, 0.2, above) * (1.0 - smoothstep(1.2, 2.2, above));\n"
    "        float rr = 0.6 + 0.1 * max(above, 0.0);\n"
    "        float k = exp(-(dl.x * dl.x + dl.y * dl.y * 0.45) / (2.0 * rr * rr));\n"
    "        g += uLightCol[i].rgb * near * ((r - dist) / r * 0.025 + k * 0.07);\n"
    "    }\n"
    "    return mix(g, vec3(dot(g, vec3(0.3, 0.5, 0.2))), 0.4) * uRainParams.y;\n"
    "}\n"
        // Puddles: a still mask in world space (two octaves of value noise, block coordinates), edges as wide as a
    // pixel; and in them rings from the drops, each cell of a quarter block having one at its own place and time.
    "float RainPuddle(vec2 w, float bias)\n"
    "{\n"
    "    vec2 warp = vec2(RainNoise(w * 2.3 + 3.0), RainNoise(w * 2.3 + 41.0)) - 0.5;\n"
    "    float n = RainNoise(w * 1.35 + warp * 0.9) * 0.7 + RainNoise(w * 4.1 + 17.0) * 0.3;\n"
    "    float th = 0.86 - 0.5 * clamp(uRainParams.z, 0.0, 1.0) + bias;\n"
    "    float fw = max(fwidth(n), 1e-4);\n"
    "    return uRainParams.z > 0.0 ? smoothstep(th - fw, th + fw, n) : 0.0;\n"
    "}\n"
    "float RainRings(vec2 w)\n"
    "{\n"
    "    vec2 g = w * 4.0;\n"
    "    vec2 base = floor(g - 0.5);\n"
    "    float ring = 0.0;\n"
    "    for (int j = 0; j < 4; j++)\n"
    "    {\n"
    "        vec2 c = base + vec2(float(j & 1), float(j >> 1));\n"
    "        vec2 at = c + 0.25 + 0.5 * vec2(RainHash(c), RainHash(c + 71.0));\n"
    "        float life = fract(uRainCam.w * 0.9 + RainHash(c + 13.0));\n"
    "        float d = length(g - at);\n"
    "        float rad = 0.05 + 0.4 * life;\n"
    "        ring += exp(-(d - rad) * (d - rad) / 0.0025) * (1.0 - life) * (1.0 - life);\n"
    "    }\n"
    "    return ring;\n"
    "}\n"
    "vec3 RainShade(vec3 col, vec3 tex, vec3 worldDx, vec3 worldDy, float alpha)\n"
    "{\n"
    "    vec3 n = cross(worldDx, worldDy);\n"
    "    if (dot(n, n) < 1e-24) return col;\n"
    "    float up = smoothstep(0.8, 0.98, abs(normalize(n).z));\n"
    "    if (up <= 0.0) return col;\n"
    // Grass and other green ground soaks the rain up: it does not mirror nor hold puddles; nor do the roofs.
    "    float soak = smoothstep(0.015, 0.07, tex.g - max(tex.r, tex.b));\n"
    "    float street = 1.0 - smoothstep(0.4, 0.9, vWorld.z - uRainParams.x);\n"
    // Fewer on the light pavements than on the dark asphalt.
    "    float puddle = RainPuddle(vWorld.xy, 0.12 * smoothstep(0.25, 0.5, dot(tex, vec3(0.3, 0.5, 0.2)))) * up * (1.0 - soak) * street;\n"
    "    gWet = up * (0.6 + 0.4 * puddle) * (1.0 - 0.85 * soak);\n"
    "    col *= 1.0 - up * (0.24 + 0.06 * puddle);\n"
    "    float t = uRainCam.w;\n"
    "    vec3 sheen = gSpots * 0.25 * uRainParams.y + RainPools();\n"
    "    for (int i = 0; i < uNumLights; i++)\n"
    "    {\n"
    "        vec4 s = uLightDir[i];\n"
    "        if (s.w <= 0.0) continue;\n"
    "        vec3 L = uLightPos[i].xyz;\n"
    "        float r = uLightPos[i].w;\n"
    "        vec3 dl = vWorld - L;\n"
    "        float above = L.z - vWorld.z;\n"
    "        float near = smoothstep(-0.3, 0.2, above) * (1.0 - smoothstep(1.2, 2.2, above));\n"
    // A headlight: a glossy streak on the road ahead along its beam, widening and fading out, rippled lengthways.
    "        float along = dot(dl.xy, s.xy);\n"
    "        float across = dot(dl.xy, vec2(-s.y, s.x));\n"
    "        float wid = 0.05 + 0.16 * max(along, 0.0);\n"
    "        float k = exp(-across * across / (2.0 * wid * wid)) * smoothstep(-0.05, 0.12, along) * (1.0 - smoothstep(0.25 * r, 0.75 * r, along));\n"
    "        float wave = 0.75 + 0.5 * RainNoise(vec2(along * 6.0, across * 3.0 + float(i) * 7.3));\n"
    "        sheen += uLightCol[i].rgb * near * k * wave * 0.7 * uRainParams.y;\n"
    "    }\n"
    "    if ((uRain & 8) != 0) return vec3(puddle, gWet * 0.5, 0.0);\n"
    "    float gloss = 1.0 + puddle * (1.6 + 0.8 * RainRings(vWorld.xy));\n"
    // Water mirrors the sky: a faint cool sheen even far from the lights.
    "    vec3 sky = (vec3(0.009, 0.012, 0.018) + uAmbient / 255.0 * 0.04) * uRainParams.y;\n"
    "    return col + (sheen * gloss + sky * puddle) * up * alpha;\n"
    "}\n"
    // The texture's alpha averaged over a square of that radius in texels; outside the texture is transparent.
    "float SoftAlpha(vec2 uv, float radius)\n"
    "{\n"
    "    vec2 texel = 1.0 / vec2(textureSize(uTex, 0));\n"
    "    int n = int(clamp(ceil(radius), 1.0, 4.0));\n"
    "    vec2 stepUV = texel * (radius / float(n));\n"
    "    float sum = 0.0;\n"
    "    for (int y = -n; y <= n; y++)\n"
    "    {\n"
    "        for (int x = -n; x <= n; x++)\n"
    "        {\n"
    "            vec2 p = uv + vec2(float(x), float(y)) * stepUV;\n"
    "            vec2 inside = step(vec2(0.0), p) * step(p, vec2(1.0));\n"
    "            sum += textureLod(uTex, p, 0.0).a * inside.x * inside.y;\n"
    "        }\n"
    "    }\n"
    "    return sum / float((2 * n + 1) * (2 * n + 1));\n"
    "}\n"
    // Ambient occlusion from the baked volumes (kAoBakeFS), per texel of the 64 per block as the tiles are drawn.
    // Slopes blend the axes they face, so that the facing's rounding never picks one axis per pixel.
    "float Occlusion(vec3 dx, vec3 dy)\n"
    "{\n"
    "    vec3 n = cross(dx, dy);\n"
    "    if (dot(n, n) < 1e-24) return 1.0;\n"
    "    vec3 a = abs(normalize(n));\n"
    "    vec3 w = a * a;\n"
    "    w *= w;\n"
    "    w *= w;\n"
    "    w /= w.x + w.y + w.z;\n"
    "    vec3 p = (floor(vWorld * 64.0) + 0.5) / 64.0;\n"
    "    float occ = 0.0;\n"
    "    if (w.z > 0.01 && n.z > 0.0) occ += w.z * textureLod(uAoUp, (vec3(p.xy, vWorld.z) * 2.0 + 0.5) / uAoGrid, 0.0).r;\n"
    "    if (w.x > 0.01)\n"
    "    {\n"
    "        vec4 s = textureLod(uAoSides, (vec3(vWorld.x, p.yz) * 2.0 + 0.5) / uAoGrid, 0.0);\n"
    "        occ += w.x * (n.x > 0.0 ? s.r : s.g);\n"
    "    }\n"
    "    if (w.y > 0.01)\n"
    "    {\n"
    "        vec4 s = textureLod(uAoSides, (vec3(p.x, vWorld.y, p.z) * 2.0 + 0.5) / uAoGrid, 0.0);\n"
    "        occ += w.y * (n.y > 0.0 ? s.b : s.a);\n"
    "    }\n"
    "    return 1.0 - uAo * occ;\n"
    "}\n"
    "void main()\n"
    "{\n"
    "    vec3 worldDx = dFdx(vWorld);\n"
    "    vec3 worldDy = dFdy(vWorld);\n"
    "    vec2 uv = vUV - (dFdx(vUV) + dFdy(vUV)) * uSampleShift;\n"
    "    if (uShadow != 0)\n"
    "    {\n"
    // A shadow takes away the ambient light and half of the lights' (as in gbh_api.cpp).
    "        float a = SoftAlpha(uv, vCol.r * 255.0 / 32.0) * vCol.a;\n"
    "        if (uPerPixel != 0 && vLit > 0.5)\n"
    "        {\n"
    "            vec3 l = min(255.0 * LightSum(0.0) + uAmbient, vec3(255.0));\n"
    "            a *= 0.5 + 0.5 * min((uAmbient.r + uAmbient.g + uAmbient.b) / max(l.r + l.g + l.b, 1.0), 1.0);\n"
    "        }\n"
    "        oColor = vec4(0.0);\n"
    "        oBloom = vec4(0.0);\n"
    "        oShadow = vec4(a);\n"
    "        return;\n"
    "    }\n"
    "    vec4 t = uUseTex != 0 ? SmoothSample(uv) : vec4(1.0);\n"
    "    if (uAlphaTest != 0 && t.a < 0.5) discard;\n"
    "    vec4 c = vCol;\n"
    "    float ao = uAo > 0.0 && vAo > 0.0 ? Occlusion(worldDx, worldDy) : 1.0;\n"
    "    if (uPerPixel != 0 && vLit > 0.5)\n"
    "    {\n"
    // Ambient occlusion takes the ambient light and a quarter of the lights' (kAoOfLights in gbh_api.cpp). A tinted
    // particle (lit 2, FLAG_TINTED) has its colour multiplied by the ambient light too, takes twice the lights (they
    // light it through, not only its surface) and may be lit up to 3 times its colour (overbright, as Quake 3's).
    "        c = vec4(min((vShade * vUnlit.rgb * 255.0 * LightSum(vLit > 1.5 ? 1.0 : 0.0) * (vLit > 1.5 ? 2.0 : 1.0) * mix(1.0, ao, 0.25) + uAmbient * ao * (1.0 - (vLit > 1.5 ? 1.0 : uUnlitAmbient) * (1.0 - vUnlit.rgb))) / 255.0, vec3(vLit > 1.5 ? 3.0 : 1.0)), vUnlit.a);\n"
    "    }\n"
    "    else\n"
    "    {\n"
    "        c.rgb *= 1.0 - (1.0 - ao) * vAo;\n"
    "    }\n"
    "    oColor = t * c;\n"
    "    if (uRain != 0 && uPerPixel != 0 && vLit > 1.1 && vLit < 1.4) oColor.rgb = RainShade(oColor.rgb, t.rgb, worldDx, worldDy, t.a);\n"
    // A sprite standing in a lamp's wet glow is lit by it as the ground round it is.
    "    else if ((uRain & 18) == 2 && uPerPixel != 0 && vLit > 0.9 && vLit < 1.1) oColor.rgb += RainPools() * t.rgb * vUnlit.rgb * 3.0 * t.a;\n"
    "    if (uAoDebug != 0 && vAo > 0.0) oColor.rgb = vec3(ao) * t.a;\n"
    "    if (uPerPixel != 0 && vLit > 0.5) oColor.rgb += min(gHaze, vec3(0.3)) * t.a;\n"
    "    oShadow = vec4(0.0, 0.0, 0.0, oColor.a);\n"
    // With a shadow mask, additive draws blend with (1, 1 - alpha): no alpha keeps them additive, and the
    // mask is lightened by their brightness, as a shadow under them would have been.
    "    if (uAdditive == 2)\n"
    "    {\n"
    "        oShadow.a = max(oColor.r, max(oColor.g, oColor.b));\n"
    "        oColor.a = 0.0;\n"
    "    }\n"
    "    oBloom = uAdditive != 0 ? oColor : vec4(0.0, 0.0, 0.0, oColor.a);\n"
    "    if ((uRain & 4) != 0 && uPerPixel != 0 && vLit > 1.1 && vLit < 1.4) oBloom.a = oColor.a * (1.0 - gWet);\n"
    "}\n";

// Fullscreen triangle; also composites the CPU surface (alpha 0 where the game did not write).
const char* kPresentVS =
    "out vec2 vUV;\n"
    "void main()\n"
    "{\n"
    "    vec2 p = vec2((gl_VertexID << 1) & 2, gl_VertexID & 2);\n"
    "    vUV = p;\n"
    "    gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0);\n"
    "}\n";

const char* kPresentFS =
    "uniform sampler2D uTex;\n"
    "uniform vec3 uGamma;\n"
    "uniform int uFlipY;\n"
    "in vec2 vUV;\n"
    "layout(location = 0) out vec4 oColor;\n"
    "layout(location = 2) out vec4 oShadow;\n"
    "void main()\n"
    "{\n"
    "    vec2 uv = uFlipY != 0 ? vec2(vUV.x, 1.0 - vUV.y) : vUV;\n"
    "    vec4 c = texture(uTex, uv);\n"
    "    oColor = vec4(pow(c.rgb, 1.0 / uGamma), c.a);\n"
    "    oShadow = vec4(0.0, 0.0, 0.0, c.a);\n"
    "}\n";

// Bloom: 4x4 box downsample (four bilinear taps), separable 9 tap gaussian, additive compose.
const char* kDownFS =
    "uniform sampler2D uTex;\n"
    "uniform vec2 uTexel;\n"
    "in vec2 vUV;\n"
    "out vec4 oColor;\n"
    "void main()\n"
    "{\n"
    "    oColor = 0.25 * (texture(uTex, vUV + uTexel * vec2(-1.0, -1.0)) + texture(uTex, vUV + uTexel * vec2(1.0, -1.0)) +\n"
    "                     texture(uTex, vUV + uTexel * vec2(-1.0, 1.0)) + texture(uTex, vUV + uTexel * vec2(1.0, 1.0)));\n"
    "}\n";

const char* kBlurFS =
    "uniform sampler2D uTex;\n"
    "uniform vec2 uStep;\n"
    "in vec2 vUV;\n"
    "out vec4 oColor;\n"
    "void main()\n"
    "{\n"
    "    const float w[5] = float[](0.227027, 0.1945946, 0.1216216, 0.054054, 0.016216);\n"
    "    vec4 c = texture(uTex, vUV) * w[0];\n"
    "    for (int i = 1; i < 5; i++)\n"
    "    {\n"
    "        c += (texture(uTex, vUV + uStep * float(i)) + texture(uTex, vUV - uStep * float(i))) * w[i];\n"
    "    }\n"
    "    oColor = c;\n"
    "}\n";

const char* kComposeFS =
    "uniform sampler2D uTex;\n"
    "uniform sampler2D uBloom;\n"
    "uniform float uIntensity;\n"
    "in vec2 vUV;\n"
    "out vec4 oColor;\n"
    "void main()\n"
    "{\n"
    "    vec3 c = texture(uTex, vUV).rgb + texture(uBloom, vUV).rgb * (uIntensity * 2.0);\n"
    "    oColor = vec4(min(c, vec3(1.0)), 1.0);\n"
    "}\n";

// Ambient occlusion baked on a grid of half blocks, one z slice per draw: for each axis facing, how solid the map is
// on a ring of half a block round the point half a block out (never its own block): 0.5 on a wall's contact line,
// 0.75 in an inner corner, nothing a block away.
const char* kAoBakeFS =
    "uniform sampler3D uOcc;\n"
    "uniform vec3 uOccScale;\n"
    "uniform float uZ;\n"
    "layout(location = 0) out vec4 oSides;\n"
    "layout(location = 1) out vec4 oUp;\n"
    // How solid the map is at a point, trilinear between block centres.
    "float Solid(vec3 p)\n"
    "{\n"
    "    return textureLod(uOcc, p * uOccScale, 0.0).r;\n"
    "}\n"
    "float Occlusion(vec3 p, vec3 n, vec3 t1, vec3 t2)\n"
    "{\n"
    "    const vec2 ring[8] = vec2[](vec2(0.92388, 0.38268), vec2(0.38268, 0.92388), vec2(-0.38268, 0.92388), vec2(-0.92388, 0.38268),\n"
    "                                vec2(-0.92388, -0.38268), vec2(-0.38268, -0.92388), vec2(0.38268, -0.92388), vec2(0.92388, -0.38268));\n"
    "    float sum = 0.0;\n"
    "    for (int k = 0; k < 8; k++)\n"
    "    {\n"
    "        sum += Solid(p + n * 0.5 + (t1 * ring[k].x + t2 * ring[k].y) * 0.5);\n"
    "    }\n"
    "    return sum * 0.125;\n"
    "}\n"
    "void main()\n"
    "{\n"
    "    vec3 p = vec3(floor(gl_FragCoord.xy), uZ) * 0.5;\n"
    "    vec3 x = vec3(1.0, 0.0, 0.0), y = vec3(0.0, 1.0, 0.0), z = vec3(0.0, 0.0, 1.0);\n"
    "    oSides = vec4(Occlusion(p, x, y, z), Occlusion(p, -x, y, z), Occlusion(p, y, x, z), Occlusion(p, -y, x, z));\n"
    "    oUp = vec4(Occlusion(p, z, x, y));\n"
    "}\n";

const s32 kMaxBatchVerts = 6 * 4096;
const s32 kMaxLights = 256;
// Ambient occlusion textures' units (0 is the batch's texture, 1 the bloom's): the baked sides and up volumes, and
// the map's blocks they are baked from.
const s32 kAoSidesUnit = 2;
const s32 kAoUpUnit = 3;
const s32 kOcclusionUnit = 4;
const u32 kStateMipmap = 1u << 31; // the batch's texture has mip levels and is filtered
const GLenum kTextureMaxAnisotropy = 0x84FE;
const GLenum kMaxTextureMaxAnisotropy = 0x84FF;

// A mip level from the one above it: 2x2 boxes, edges clamped. With alpha, transparent texels lend
// no colour, and fully transparent ones take their neighbours' so filtering adds no dark fringe.
void Downsample(const u32* src, s32 sw, s32 sh, u32* dst, s32 dw, s32 dh, bool alpha)
{
    for (s32 y = 0; y < dh; y++)
    {
        const s32 ys[2] = {2 * y < sh ? 2 * y : sh - 1, 2 * y + 1 < sh ? 2 * y + 1 : sh - 1};
        for (s32 x = 0; x < dw; x++)
        {
            const s32 xs[2] = {2 * x < sw ? 2 * x : sw - 1, 2 * x + 1 < sw ? 2 * x + 1 : sw - 1};
            u32 sum[3] = {0, 0, 0}, weighted[3] = {0, 0, 0}, a = 0;
            for (int i = 0; i < 4; i++)
            {
                const u32 c = src[(size_t)ys[i >> 1] * sw + xs[i & 1]];
                const u32 ca = c >> 24;
                for (int k = 0; k < 3; k++)
                {
                    const u32 v = (c >> (8 * k)) & 0xFF;
                    sum[k] += v;
                    weighted[k] += v * ca;
                }
                a += ca;
            }
            u32 out = ((a + 2) / 4) << 24;
            for (int k = 0; k < 3; k++)
            {
                const u32 v = alpha && a ? (weighted[k] + a / 2) / a : (sum[k] + 2) / 4;
                out |= v << (8 * k);
            }
            dst[(size_t)y * dw + x] = out;
        }
    }
    if (!alpha)
    {
        return;
    }
    static std::vector<u32> sCopy;
    sCopy.assign(dst, dst + (size_t)dw * dh);
    for (s32 y = 0; y < dh; y++)
    {
        for (s32 x = 0; x < dw; x++)
        {
            if (sCopy[(size_t)y * dw + x] >> 24)
            {
                continue;
            }
            u32 weighted[3] = {0, 0, 0}, a = 0;
            for (s32 ny = y - 1; ny <= y + 1; ny++)
            {
                for (s32 nx = x - 1; nx <= x + 1; nx++)
                {
                    if (nx < 0 || ny < 0 || nx >= dw || ny >= dh)
                    {
                        continue;
                    }
                    const u32 c = sCopy[(size_t)ny * dw + nx];
                    const u32 ca = c >> 24;
                    for (int k = 0; k < 3; k++)
                    {
                        weighted[k] += ((c >> (8 * k)) & 0xFF) * ca;
                    }
                    a += ca;
                }
            }
            if (a)
            {
                dst[(size_t)y * dw + x] = ((weighted[0] + a / 2) / a) | (((weighted[1] + a / 2) / a) << 8) | (((weighted[2] + a / 2) / a) << 16);
            }
        }
    }
}
} // namespace

GLRenderer::GLRenderer()
    : mReady(false), mLogicalW(640), mLogicalH(480), mTargetW(640), mTargetH(480), mDrawableW(0), mDrawableH(0), mFbo(0), mFboTex(0), mBloomSrcTex(0), mShadowTex(0), mPostFbo(0), mPostTex(0), mBlurW(0),
      mBlurH(0), mTargetsTrueColour(false), mTargetsBloom(false), mTargetsShadows(false), mPostValid(false), mFrameFence(0), mMsFbo(0), mTargetsSamples(0), mMaxSamples(0), mProgram(0), mPresentProgram(0),
      mDownProgram(0), mBlurProgram(0), mComposeProgram(0), mVao(0), mVbo(0), mLightUbo(0), mSamplerNearest(0), mSamplerLinear(0),
      mSamplerMip(0), mMaxAnisotropy(1.0f), mWhiteTex(0), mSurfaceTex(0), mSurfaceW(0), mSurfaceH(0), mBatchTex(0), mBatchState(0), mAppliedState(0), mStateValid(false),
      mOffsetX(0.5f - 1.0f / 2048.0f), mOffsetY(0.5f - 1.0f / 2048.0f), mSampleShift(1.0f / 1024.0f), mNumLights(0),
      mAmbient{255.0f, 255.0f, 255.0f}, mUnlitAmbient(0.0f), mOccTex(0), mAoTex{0, 0}, mAoFbo(0), mAoGrid{1, 1, 1}, mAoStrength(0.0f),
      mAoBakeProgram(0), mUniBakeOccScale(-1), mUniBakeZ(-1), mUniAoGrid(-1), mUniAo(-1), mUniAoDebug(-1), mDrawCalls(0), mUploads(0), mVertices(0)
{
    memset(mScene, 0, sizeof(mScene));
    mActiveScene = 0;
    memset(&gl, 0, sizeof(gl));
    mGamma[0] = mGamma[1] = mGamma[2] = 1.0f;
    mBlurFbo[0] = mBlurFbo[1] = 0;
    mBlurTex[0] = mBlurTex[1] = 0;
    mMsColour[0] = mMsColour[1] = mMsColour[2] = 0;
}

GLRenderer::~GLRenderer()
{
    Shutdown();
}

// The shaders are GLSL ES 3.00 (OpenGL ES 3.0, WebGL2) and GLSL 3.30 alike; only the first line differs.
GLuint GLRenderer::CompileProgram(const char* vs, const char* fs, const char** ppError)
{
    static char log[1024];
    const char* version = reinterpret_cast<const char*>(gl.glGetString(GL_VERSION));
    const char* header = (version && strncmp(version, "OpenGL ES", 9) == 0)
        ? "#version 300 es\nprecision highp float;\nprecision highp int;\nprecision highp sampler3D;\n"
        : "#version 330 core\n";
    GLuint shaders[2];
    const char* srcs[2] = {vs, fs};
    const GLenum types[2] = {GL_VERTEX_SHADER, GL_FRAGMENT_SHADER};
    for (int i = 0; i < 2; i++)
    {
        shaders[i] = gl.glCreateShader(types[i]);
        const char* parts[2] = {header, srcs[i]};
        gl.glShaderSource(shaders[i], 2, parts, NULL);
        gl.glCompileShader(shaders[i]);
        GLint ok = 0;
        gl.glGetShaderiv(shaders[i], GL_COMPILE_STATUS, &ok);
        if (!ok)
        {
            gl.glGetShaderInfoLog(shaders[i], sizeof(log), NULL, log);
            *ppError = log;
            return 0;
        }
    }
    GLuint prog = gl.glCreateProgram();
    gl.glAttachShader(prog, shaders[0]);
    gl.glAttachShader(prog, shaders[1]);
    gl.glLinkProgram(prog);
    gl.glDeleteShader(shaders[0]);
    gl.glDeleteShader(shaders[1]);
    GLint ok = 0;
    gl.glGetProgramiv(prog, GL_LINK_STATUS, &ok);
    if (!ok)
    {
        gl.glGetProgramInfoLog(prog, sizeof(log), NULL, log);
        *ppError = log;
        return 0;
    }
    return prog;
}

bool GLRenderer::Init(GLFuncs::Loader loader, const char** ppError)
{
    const char* missing = gl.Load(loader);
    if (missing)
    {
        *ppError = missing;
        return false;
    }

    mScene[0].mId = CompileProgram(kFaithfulVS, kFaithfulFS, ppError);
    mScene[1].mId = mScene[0].mId ? CompileProgram(kSceneVS, kSceneFS, ppError) : 0;
    mProgram = mScene[1].mId;
    mPresentProgram = mProgram ? CompileProgram(kPresentVS, kPresentFS, ppError) : 0;
    mDownProgram = mPresentProgram ? CompileProgram(kPresentVS, kDownFS, ppError) : 0;
    mBlurProgram = mDownProgram ? CompileProgram(kPresentVS, kBlurFS, ppError) : 0;
    mComposeProgram = mBlurProgram ? CompileProgram(kPresentVS, kComposeFS, ppError) : 0;
    mAoBakeProgram = mComposeProgram ? CompileProgram(kPresentVS, kAoBakeFS, ppError) : 0;
    if (!mAoBakeProgram)
    {
        return false;
    }
    for (int i = 0; i < 2; i++)
    {
        SceneProgram& sp = mScene[i];
        sp.mScale = gl.glGetUniformLocation(sp.mId, "uScale");
        sp.mOffset = gl.glGetUniformLocation(sp.mId, "uOffset");
        sp.mAlphaTest = gl.glGetUniformLocation(sp.mId, "uAlphaTest");
        sp.mUseTex = gl.glGetUniformLocation(sp.mId, "uUseTex");
        sp.mSampleShift = gl.glGetUniformLocation(sp.mId, "uSampleShift");
        sp.mSmooth = gl.glGetUniformLocation(sp.mId, "uSmooth");
        sp.mAdditive = gl.glGetUniformLocation(sp.mId, "uAdditive");
        sp.mPerPixel = gl.glGetUniformLocation(sp.mId, "uPerPixel");
        sp.mNumLights = gl.glGetUniformLocation(sp.mId, "uNumLights");
        sp.mAmbient = gl.glGetUniformLocation(sp.mId, "uAmbient");
        sp.mUnlitAmbient = gl.glGetUniformLocation(sp.mId, "uUnlitAmbient");
        sp.mShadow = gl.glGetUniformLocation(sp.mId, "uShadow");
        gl.glUseProgram(sp.mId);
        gl.glUniform1i(gl.glGetUniformLocation(sp.mId, "uTex"), 0);
    }
    mUniGamma = gl.glGetUniformLocation(mPresentProgram, "uGamma");
    mUniFlipY = gl.glGetUniformLocation(mPresentProgram, "uFlipY");
    mUniDownTexel = gl.glGetUniformLocation(mDownProgram, "uTexel");
    mUniBlurStep = gl.glGetUniformLocation(mBlurProgram, "uStep");
    mUniBloomIntensity = gl.glGetUniformLocation(mComposeProgram, "uIntensity");
    gl.glUseProgram(mProgram);
    gl.glUniformBlockBinding(mProgram, gl.glGetUniformBlockIndex(mProgram, "Lights"), 0);
    gl.glUniform1i(gl.glGetUniformLocation(mProgram, "uAoSides"), kAoSidesUnit);
    gl.glUniform1i(gl.glGetUniformLocation(mProgram, "uAoUp"), kAoUpUnit);
    mUniAoGrid = gl.glGetUniformLocation(mProgram, "uAoGrid");
    mUniAo = gl.glGetUniformLocation(mProgram, "uAo");
    mUniAoDebug = gl.glGetUniformLocation(mProgram, "uAoDebug");
    mUniRain = gl.glGetUniformLocation(mProgram, "uRain");
    mUniRainCam = gl.glGetUniformLocation(mProgram, "uRainCam");
    mUniRainParams = gl.glGetUniformLocation(mProgram, "uRainParams");
    GLuint posts[4] = {mPresentProgram, mDownProgram, mBlurProgram, mComposeProgram};
    for (int i = 0; i < 4; i++)
    {
        gl.glUseProgram(posts[i]);
        gl.glUniform1i(gl.glGetUniformLocation(posts[i], "uTex"), 0);
    }
    gl.glUniform1i(gl.glGetUniformLocation(mComposeProgram, "uBloom"), 1);
    gl.glUseProgram(mAoBakeProgram);
    gl.glUniform1i(gl.glGetUniformLocation(mAoBakeProgram, "uOcc"), kOcclusionUnit);
    mUniBakeOccScale = gl.glGetUniformLocation(mAoBakeProgram, "uOccScale");
    mUniBakeZ = gl.glGetUniformLocation(mAoBakeProgram, "uZ");

    gl.glGenBuffers(1, &mLightUbo);
    gl.glBindBuffer(GL_UNIFORM_BUFFER, mLightUbo);
    gl.glBufferData(GL_UNIFORM_BUFFER, kMaxLights * 3 * 16, NULL, GL_DYNAMIC_DRAW);
    gl.glBindBufferBase(GL_UNIFORM_BUFFER, 0, mLightUbo);

    gl.glGenVertexArrays(1, &mVao);
    gl.glBindVertexArray(mVao);
    gl.glGenBuffers(1, &mVbo);
    gl.glBindBuffer(GL_ARRAY_BUFFER, mVbo);
    gl.glBufferData(GL_ARRAY_BUFFER, kMaxBatchVerts * sizeof(RVertex), NULL, GL_STREAM_DRAW);
    gl.glEnableVertexAttribArray(0);
    gl.glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, sizeof(RVertex), (void*)offsetof(RVertex, x));
    gl.glEnableVertexAttribArray(1);
    gl.glVertexAttribPointer(1, 4, GL_UNSIGNED_BYTE, GL_TRUE, sizeof(RVertex), (void*)offsetof(RVertex, rgba));
    gl.glEnableVertexAttribArray(2);
    gl.glVertexAttribPointer(2, 2, GL_FLOAT, GL_FALSE, sizeof(RVertex), (void*)offsetof(RVertex, u));
    gl.glEnableVertexAttribArray(3);
    gl.glVertexAttribPointer(3, 3, GL_FLOAT, GL_FALSE, sizeof(RVertex), (void*)offsetof(RVertex, wx));
    gl.glEnableVertexAttribArray(4);
    gl.glVertexAttribPointer(4, 4, GL_UNSIGNED_BYTE, GL_TRUE, sizeof(RVertex), (void*)offsetof(RVertex, unlit));
    gl.glEnableVertexAttribArray(5);
    gl.glVertexAttribPointer(5, 1, GL_FLOAT, GL_FALSE, sizeof(RVertex), (void*)offsetof(RVertex, shade));
    gl.glEnableVertexAttribArray(6);
    gl.glVertexAttribPointer(6, 1, GL_FLOAT, GL_FALSE, sizeof(RVertex), (void*)offsetof(RVertex, lit));
    gl.glEnableVertexAttribArray(7);
    gl.glVertexAttribPointer(7, 1, GL_FLOAT, GL_FALSE, sizeof(RVertex), (void*)offsetof(RVertex, ao));

    gl.glGenSamplers(1, &mSamplerNearest);
    gl.glGenSamplers(1, &mSamplerLinear);
    GLuint samplers[2] = {mSamplerNearest, mSamplerLinear};
    for (int i = 0; i < 2; i++)
    {
        const GLint f = i ? GL_LINEAR : GL_NEAREST;
        gl.glSamplerParameteri(samplers[i], GL_TEXTURE_MIN_FILTER, f);
        gl.glSamplerParameteri(samplers[i], GL_TEXTURE_MAG_FILTER, f);
        gl.glSamplerParameteri(samplers[i], GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        gl.glSamplerParameteri(samplers[i], GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    }
    gl.glGenSamplers(1, &mSamplerMip);
    gl.glSamplerParameteri(mSamplerMip, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
    gl.glSamplerParameteri(mSamplerMip, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    gl.glSamplerParameteri(mSamplerMip, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    gl.glSamplerParameteri(mSamplerMip, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    GLint numExtensions = 0;
    gl.glGetIntegerv(GL_NUM_EXTENSIONS, &numExtensions);
    for (GLint i = 0; i < numExtensions; i++)
    {
        const char* pName = reinterpret_cast<const char*>(gl.glGetStringi(GL_EXTENSIONS, i));
        if (pName && (!strcmp(pName, "GL_EXT_texture_filter_anisotropic") || !strcmp(pName, "GL_ARB_texture_filter_anisotropic")))
        {
            GLint maxAnisotropy = 1;
            gl.glGetIntegerv(kMaxTextureMaxAnisotropy, &maxAnisotropy);
            mMaxAnisotropy = (f32)(maxAnisotropy < 16 ? maxAnisotropy : 16);
        }
    }
    ApplyTextureFilter();
    gl.glGetIntegerv(GL_MAX_SAMPLES, &mMaxSamples);

    const u32 white = 0xFFFFFFFFu;
    gl.glGenTextures(1, &mWhiteTex);
    gl.glBindTexture(GL_TEXTURE_2D, mWhiteTex);
    gl.glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, 1, 1, 0, GL_RGBA, GL_UNSIGNED_BYTE, &white);

    gl.glGenTextures(1, &mSurfaceTex);
    gl.glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    gl.glPixelStorei(GL_PACK_ALIGNMENT, 1);

    gl.glGenFramebuffers(1, &mFbo);
    gl.glGenTextures(1, &mFboTex);
    gl.glGenTextures(1, &mBloomSrcTex);
    gl.glGenTextures(1, &mShadowTex);
    gl.glGenFramebuffers(1, &mPostFbo);
    gl.glGenTextures(1, &mPostTex);
    gl.glGenFramebuffers(2, mBlurFbo);
    gl.glGenTextures(2, mBlurTex);
    gl.glGenFramebuffers(1, &mMsFbo);
    gl.glGenRenderbuffers(3, mMsColour);

    mBatch.reserve(kMaxBatchVerts);
    mReady = true;
    CreateTargets();
    Clear(0, 0, 0, 0, 0, 0, 0);
    return true;
}

void GLRenderer::ThrottleFrames()
{
    if (mFrameFence)
    {
        gl.glClientWaitSync(mFrameFence, GL_SYNC_FLUSH_COMMANDS_BIT, 1000000000ull);
        gl.glDeleteSync(mFrameFence);
    }
    mFrameFence = gl.glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
}

void GLRenderer::Shutdown()
{
    if (!mReady)
    {
        return;
    }
    if (mFrameFence)
    {
        gl.glDeleteSync(mFrameFence);
        mFrameFence = 0;
    }
    gl.glDeleteFramebuffers(1, &mFbo);
    gl.glDeleteFramebuffers(1, &mPostFbo);
    gl.glDeleteFramebuffers(2, mBlurFbo);
    gl.glDeleteTextures(1, &mFboTex);
    gl.glDeleteTextures(1, &mBloomSrcTex);
    gl.glDeleteTextures(1, &mShadowTex);
    gl.glDeleteTextures(1, &mPostTex);
    gl.glDeleteTextures(2, mBlurTex);
    gl.glDeleteFramebuffers(1, &mMsFbo);
    gl.glDeleteRenderbuffers(3, mMsColour);
    gl.glDeleteSamplers(1, &mSamplerMip);
    gl.glDeleteTextures(1, &mWhiteTex);
    gl.glDeleteTextures(1, &mOccTex);
    gl.glDeleteTextures(2, mAoTex);
    gl.glDeleteFramebuffers(1, &mAoFbo);
    gl.glDeleteProgram(mAoBakeProgram);
    mOccTex = 0;
    mAoTex[0] = mAoTex[1] = 0;
    mAoFbo = 0;
    mAoGrid[0] = mAoGrid[1] = mAoGrid[2] = 1;
    gl.glDeleteTextures(1, &mSurfaceTex);
    gl.glDeleteSamplers(1, &mSamplerNearest);
    gl.glDeleteSamplers(1, &mSamplerLinear);
    gl.glDeleteBuffers(1, &mVbo);
    gl.glDeleteBuffers(1, &mLightUbo);
    gl.glDeleteVertexArrays(1, &mVao);
    gl.glDeleteProgram(mScene[0].mId);
    gl.glDeleteProgram(mScene[1].mId);
    gl.glDeleteProgram(mPresentProgram);
    gl.glDeleteProgram(mDownProgram);
    gl.glDeleteProgram(mBlurProgram);
    gl.glDeleteProgram(mComposeProgram);
    FreeHaze();
    mReady = false;
}

void GLRenderer::SetOptions(const RenderOptions& options)
{
    const bool targetsChanged = options.mTrueColour != mOptions.mTrueColour || (options.mBloom > 0.0f) != (mOptions.mBloom > 0.0f) ||
                                options.mMsaa != mOptions.mMsaa || options.mSoftShadows != mOptions.mSoftShadows;
    Flush();
    mOptions = options;
    if (mReady)
    {
        ApplyTextureFilter();
    }
    if (targetsChanged && mReady)
    {
        CreateTargets();
        Clear(0, 0, 0, 0, 0, 0, 0);
    }
    mStateValid = false;
}

void GLRenderer::SetLogicalSize(s32 width, s32 height)
{
    if (width <= 0 || height <= 0)
    {
        return;
    }
    Flush();
    mLogicalW = width;
    mLogicalH = height;
    UpdateTargetSize();
}

void GLRenderer::UpdateTargetSize()
{
    static const char* sNative = getenv("GTA2GL_NATIVE");
    s32 w = mLogicalW;
    s32 h = mLogicalH;
    if ((!sNative || atoi(sNative)) && mDrawableW > 0 && mDrawableH > 0)
    {
        // The logical frame fitted into the window, keeping its shape; never smaller than logical.
        w = mDrawableW;
        h = (s32)((long long)mDrawableW * mLogicalH / mLogicalW);
        if (h > mDrawableH)
        {
            h = mDrawableH;
            w = (s32)((long long)mDrawableH * mLogicalW / mLogicalH);
        }
        if (w < mLogicalW || h < mLogicalH)
        {
            w = mLogicalW;
            h = mLogicalH;
        }
    }
    const bool changed = w != mTargetW || h != mTargetH;
    mTargetW = w;
    mTargetH = h;
    if (mReady && changed)
    {
        CreateTargets();
    }
}

void GLRenderer::CreateTargets()
{
    const s32 width = mTargetW, height = mTargetH;
    gl.glBindTexture(GL_TEXTURE_2D, mFboTex);
    gl.glBindFramebuffer(GL_FRAMEBUFFER, mFbo);
    bool complete = false;
    GLenum colourFormat = GL_RGB565;
    if (!mOptions.mTrueColour)
    {
        // The original's 16-bit back buffer.
        gl.glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB565, width, height, 0, GL_RGB, GL_UNSIGNED_SHORT_5_6_5, NULL);
        gl.glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, mFboTex, 0);
        complete = gl.glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE;
    }
    if (!complete)
    {
        colourFormat = GL_RGBA8;
        gl.glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
        gl.glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, mFboTex, 0);
    }
    mColourFormat = colourFormat;
    mTargetsTrueColour = mOptions.mTrueColour;
    mTargetsBloom = mOptions.mBloom > 0.0f;
    const GLenum one[1] = {GL_COLOR_ATTACHMENT0};
    if (mTargetsBloom)
    {
        gl.glBindTexture(GL_TEXTURE_2D, mBloomSrcTex);
        gl.glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
        gl.glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT1, GL_TEXTURE_2D, mBloomSrcTex, 0);
        const GLenum two[2] = {GL_COLOR_ATTACHMENT0, GL_COLOR_ATTACHMENT1};
        gl.glDrawBuffers(2, two);

        gl.glBindTexture(GL_TEXTURE_2D, mPostTex);
        gl.glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
        gl.glBindFramebuffer(GL_FRAMEBUFFER, mPostFbo);
        gl.glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, mPostTex, 0);
        mBlurW = (width + 3) / 4;
        mBlurH = (height + 3) / 4;
        for (int i = 0; i < 2; i++)
        {
            gl.glBindTexture(GL_TEXTURE_2D, mBlurTex[i]);
            gl.glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, mBlurW, mBlurH, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
            gl.glBindFramebuffer(GL_FRAMEBUFFER, mBlurFbo[i]);
            gl.glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, mBlurTex[i], 0);
        }
    }
    else
    {
        gl.glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT1, GL_TEXTURE_2D, 0, 0);
        gl.glDrawBuffers(1, one);
    }
    mTargetsShadows = mOptions.mSoftShadows;
    gl.glBindFramebuffer(GL_FRAMEBUFFER, mFbo);
    if (mTargetsShadows)
    {
        // Sampled as (0, 0, 0, mask) to darken the scene with alpha blending.
        gl.glBindTexture(GL_TEXTURE_2D, mShadowTex);
        gl.glTexImage2D(GL_TEXTURE_2D, 0, GL_R8, width, height, 0, GL_RED, GL_UNSIGNED_BYTE, NULL);
        gl.glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_SWIZZLE_R, GL_ZERO);
        gl.glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_SWIZZLE_G, GL_ZERO);
        gl.glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_SWIZZLE_B, GL_ZERO);
        gl.glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_SWIZZLE_A, GL_RED);
    }
    gl.glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT2, GL_TEXTURE_2D, mTargetsShadows ? mShadowTex : 0, 0);
    SetDrawBuffers();
    CreateMultisampleTarget(colourFormat);
    mPostValid = false;
    mStateValid = false;
}

void GLRenderer::CreateMultisampleTarget(GLenum colourFormat)
{
    gl.glBindFramebuffer(GL_FRAMEBUFFER, mMsFbo);
    // New names free the old storage; deleting detaches them from the bound framebuffer.
    gl.glDeleteRenderbuffers(3, mMsColour);
    gl.glGenRenderbuffers(3, mMsColour);
    mTargetsSamples = 0;
    // The resolve blit needs the same formats as the single sample target's.
    for (s32 samples = mOptions.mMsaa < mMaxSamples ? mOptions.mMsaa : mMaxSamples; samples > 1; samples /= 2)
    {
        for (int i = 0; i < 16 && gl.glGetError() != GL_NO_ERROR; i++)
        {
        }
        const GLenum formats[3] = {colourFormat, GL_RGBA8, GL_R8};
        const bool used[3] = {true, mTargetsBloom, mTargetsShadows};
        for (s32 i = 0; i < 3; i++)
        {
            gl.glBindRenderbuffer(GL_RENDERBUFFER, mMsColour[i]);
            if (used[i])
            {
                gl.glRenderbufferStorageMultisample(GL_RENDERBUFFER, samples, formats[i], mTargetW, mTargetH);
            }
            gl.glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0 + i, GL_RENDERBUFFER, used[i] ? mMsColour[i] : 0);
        }
        SetDrawBuffers();
        if (gl.glGetError() == GL_NO_ERROR && gl.glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE)
        {
            mTargetsSamples = samples;
            break;
        }
    }
    gl.glBindRenderbuffer(GL_RENDERBUFFER, 0);
}

void GLRenderer::ResolveMultisample()
{
    if (!mTargetsSamples)
    {
        return;
    }
    gl.glBindFramebuffer(GL_READ_FRAMEBUFFER, mMsFbo);
    gl.glBindFramebuffer(GL_DRAW_FRAMEBUFFER, mFbo);
    const bool used[3] = {true, mTargetsBloom, mTargetsShadows};
    for (s32 i = 0; i < 3; i++)
    {
        if (!used[i])
        {
            continue;
        }
        const GLenum buffers[3] = {i == 0 ? (GLenum)GL_COLOR_ATTACHMENT0 : (GLenum)GL_NONE,
                                   i == 1 ? (GLenum)GL_COLOR_ATTACHMENT1 : (GLenum)GL_NONE, GL_COLOR_ATTACHMENT2};
        gl.glReadBuffer(GL_COLOR_ATTACHMENT0 + i);
        gl.glDrawBuffers(i + 1, buffers);
        gl.glBlitFramebuffer(0, 0, mTargetW, mTargetH, 0, 0, mTargetW, mTargetH, GL_COLOR_BUFFER_BIT, GL_NEAREST);
    }
    SetDrawBuffers();
    gl.glReadBuffer(GL_COLOR_ATTACHMENT0);
    gl.glBindFramebuffer(GL_FRAMEBUFFER, mFbo);
}

// For the bound scene target: colour, bloom source and shadow mask, as the options have them.
void GLRenderer::SetDrawBuffers()
{
    const GLenum buffers[3] = {GL_COLOR_ATTACHMENT0, mTargetsBloom ? (GLenum)GL_COLOR_ATTACHMENT1 : (GLenum)GL_NONE, GL_COLOR_ATTACHMENT2};
    gl.glDrawBuffers(mTargetsShadows ? 3 : (mTargetsBloom ? 2 : 1), buffers);
}

// Darkens the resolved scene by the shadow mask, then clears the mask.
void GLRenderer::ApplyShadows()
{
    if (!mTargetsShadows)
    {
        return;
    }
    gl.glBindFramebuffer(GL_FRAMEBUFFER, mFbo);
    gl.glViewport(0, 0, mTargetW, mTargetH);
    const GLenum one[1] = {GL_COLOR_ATTACHMENT0};
    gl.glDrawBuffers(1, one);
    gl.glUseProgram(mPresentProgram);
    gl.glUniform3f(mUniGamma, 1.0f, 1.0f, 1.0f);
    gl.glUniform1i(mUniFlipY, 0);
    gl.glActiveTexture(GL_TEXTURE0);
    gl.glBindTexture(GL_TEXTURE_2D, mShadowTex);
    gl.glBindSampler(0, mSamplerNearest);
    gl.glEnable(GL_BLEND);
    gl.glBlendEquation(GL_FUNC_ADD);
    gl.glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    DrawFullscreen();
    SetDrawBuffers();
    const GLfloat zero[4] = {0.0f, 0.0f, 0.0f, 0.0f};
    gl.glClearBufferfv(GL_COLOR, 2, zero);
    if (mTargetsSamples)
    {
        BindTarget();
        gl.glClearBufferfv(GL_COLOR, 2, zero);
    }
    mStateValid = false;
}

void GLRenderer::ApplyTextureFilter()
{
    if (mMaxAnisotropy > 1.0f)
    {
        gl.glSamplerParameterf(mSamplerMip, kTextureMaxAnisotropy, mOptions.mTextureFilter >= 2 ? mMaxAnisotropy : 1.0f);
    }
}

void GLRenderer::BindTarget()
{
    gl.glBindFramebuffer(GL_FRAMEBUFFER, mTargetsSamples ? mMsFbo : mFbo);
    gl.glViewport(0, 0, mTargetW, mTargetH);
}

void GLRenderer::Clear(u8 r, u8 g, u8 b, s32 left, s32 top, s32 right, s32 bottom)
{
    if (!mReady)
    {
        return;
    }
    Flush();
    BindTarget();
    const bool full = (left == 0 && top == 0 && right == 0 && bottom == 0) ||
                      (left <= 0 && top <= 0 && right >= mLogicalW && bottom >= mLogicalH);
    if (!full)
    {
        gl.glEnable(GL_SCISSOR_TEST);
        const f32 sx = (f32)mTargetW / mLogicalW;
        const f32 sy = (f32)mTargetH / mLogicalH;
        gl.glScissor((GLint)(left * sx), (GLint)(top * sy), (GLsizei)((right - left) * sx), (GLsizei)((bottom - top) * sy));
    }
    const GLfloat colour[4] = {r / 255.0f, g / 255.0f, b / 255.0f, 1.0f};
    gl.glClearBufferfv(GL_COLOR, 0, colour);
    const GLfloat zero[4] = {0.0f, 0.0f, 0.0f, 0.0f};
    if (mTargetsBloom)
    {
        gl.glClearBufferfv(GL_COLOR, 1, zero);
    }
    if (mTargetsShadows)
    {
        gl.glClearBufferfv(GL_COLOR, 2, zero);
    }
    gl.glDisable(GL_SCISSOR_TEST);
}

RTexture* GLRenderer::CreateTexture()
{
    RTexture* pTex = new RTexture;
    pTex->mId = 0;
    pTex->mWidth = 0;
    pTex->mHeight = 0;
    pTex->mLevels = 1;
    return pTex;
}

void GLRenderer::UploadTexture(RTexture* pTex, s32 width, s32 height, const u32* pRgba, RMipmaps mipmaps)
{
    if (!mReady || !pTex || width <= 0 || height <= 0)
    {
        return;
    }
    if (pTex == mBatchTex)
    {
        Flush();
    }
    if (!pTex->mId)
    {
        gl.glGenTextures(1, &pTex->mId);
    }
    mUploads++;
    gl.glBindTexture(GL_TEXTURE_2D, pTex->mId);
    if (pTex->mWidth != width || pTex->mHeight != height)
    {
        gl.glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, pRgba);
        pTex->mWidth = width;
        pTex->mHeight = height;
    }
    else
    {
        gl.glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, width, height, GL_RGBA, GL_UNSIGNED_BYTE, pRgba);
    }
    s32 levels = 1;
    if (mipmaps != RMIP_NONE && mOptions.mTextureFilter > 0)
    {
        // The levels below the base hold about a third of its texels; the margin covers thin sizes.
        mMipBuffer.resize((size_t)width * height / 2 + width + height + 8);
        const u32* pSrc = pRgba;
        u32* pDst = &mMipBuffer[0];
        for (s32 w = width, h = height; w > 1 || h > 1; levels++)
        {
            const s32 dw = w > 1 ? w / 2 : 1;
            const s32 dh = h > 1 ? h / 2 : 1;
            Downsample(pSrc, w, h, pDst, dw, dh, mipmaps == RMIP_ALPHA);
            gl.glTexImage2D(GL_TEXTURE_2D, levels, GL_RGBA8, dw, dh, 0, GL_RGBA, GL_UNSIGNED_BYTE, pDst);
            pSrc = pDst;
            pDst += (size_t)dw * dh;
            w = dw;
            h = dh;
        }
    }
    gl.glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, levels - 1);
    pTex->mLevels = levels;
}

void GLRenderer::FreeTexture(RTexture* pTex)
{
    if (!pTex)
    {
        return;
    }
    if (pTex == mBatchTex)
    {
        Flush();
        mBatchTex = 0;
    }
    if (mReady && pTex->mId)
    {
        gl.glDeleteTextures(1, &pTex->mId);
    }
    delete pTex;
}

void GLRenderer::BeginScene()
{
    mStateValid = false;
    mHeatVerts.clear();
    mHeatDraws.clear();
}

void GLRenderer::EndScene()
{
    Flush();
}

void GLRenderer::SetClipRect(f32, f32, f32, f32)
{
}

void GLRenderer::SetLights(const RLight* pLights, s32 count, const f32* pAmbient, f32 unlitAmbient)
{
    if (!mReady)
    {
        return;
    }
    Flush();
    if (count > kMaxLights)
    {
        count = kMaxLights;
    }
    static f32 data[kMaxLights * 12];
    memset(data, 0, sizeof(data));
    for (s32 i = 0; i < count; i++)
    {
        const RLight& l = pLights[i];
        f32* pos = &data[i * 4];
        f32* col = &data[kMaxLights * 4 + i * 4];
        pos[0] = l.x, pos[1] = l.y, pos[2] = l.z, pos[3] = l.radius;
        col[0] = l.r, col[1] = l.g, col[2] = l.b, col[3] = l.ceiling;
        f32* dir = &data[kMaxLights * 8 + i * 4];
        dir[0] = l.dx, dir[1] = l.dy, dir[2] = l.width, dir[3] = l.beam;
    }
    gl.glBindBuffer(GL_UNIFORM_BUFFER, mLightUbo);
    gl.glBufferSubData(GL_UNIFORM_BUFFER, 0, sizeof(data), data);
    mNumLights = count;
    mAmbient[0] = pAmbient[0], mAmbient[1] = pAmbient[1], mAmbient[2] = pAmbient[2];
    mUnlitAmbient = unlitAmbient;
    mStateValid = false;
}

namespace
{
void SetVolumeParameters(const GLFuncs& gl)
{
    gl.glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    gl.glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    gl.glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    gl.glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    gl.glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_WRAP_R, GL_CLAMP_TO_EDGE);
    gl.glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_MAX_LEVEL, 0);
}
} // namespace

void GLRenderer::SetOcclusion(const u8* pCells, s32 width, s32 height, s32 depth, f32 strength)
{
    if (!mReady)
    {
        return;
    }
    Flush();
    mAoStrength = strength;
    mStateValid = false;
    if (!pCells)
    {
        return;
    }
    if (!mOccTex)
    {
        gl.glGenTextures(1, &mOccTex);
        gl.glGenTextures(2, mAoTex);
        gl.glGenFramebuffers(1, &mAoFbo);
    }
    gl.glActiveTexture(GL_TEXTURE0 + kOcclusionUnit);
    gl.glBindTexture(GL_TEXTURE_3D, mOccTex);
    gl.glTexImage3D(GL_TEXTURE_3D, 0, GL_R8, width, height, depth, 0, GL_RED, GL_UNSIGNED_BYTE, pCells);
    SetVolumeParameters(gl);

    // The baked volumes: a point every half block, the map's edges and the top of its levels included.
    const s32 grid[3] = {width * 2 + 1, height * 2 + 1, depth + 1};
    const bool resized = grid[0] != mAoGrid[0] || grid[1] != mAoGrid[1] || grid[2] != mAoGrid[2];
    const GLenum formats[2] = {GL_RGBA8, GL_R8};
    const GLenum layouts[2] = {GL_RGBA, GL_RED};
    const s32 units[2] = {kAoSidesUnit, kAoUpUnit};
    for (s32 i = 0; i < 2; i++)
    {
        gl.glActiveTexture(GL_TEXTURE0 + units[i]);
        gl.glBindTexture(GL_TEXTURE_3D, mAoTex[i]);
        if (resized)
        {
            gl.glTexImage3D(GL_TEXTURE_3D, 0, formats[i], grid[0], grid[1], grid[2], 0, layouts[i], GL_UNSIGNED_BYTE, NULL);
            SetVolumeParameters(gl);
        }
    }
    gl.glActiveTexture(GL_TEXTURE0);
    mAoGrid[0] = grid[0], mAoGrid[1] = grid[1], mAoGrid[2] = grid[2];

    gl.glBindFramebuffer(GL_FRAMEBUFFER, mAoFbo);
    const GLenum two[2] = {GL_COLOR_ATTACHMENT0, GL_COLOR_ATTACHMENT1};
    gl.glDrawBuffers(2, two);
    gl.glViewport(0, 0, grid[0], grid[1]);
    gl.glDisable(GL_BLEND);
    gl.glUseProgram(mAoBakeProgram);
    gl.glUniform3f(mUniBakeOccScale, 1.0f / width, 1.0f / height, 1.0f / depth);
    for (s32 z = 0; z < grid[2]; z++)
    {
        gl.glFramebufferTextureLayer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, mAoTex[0], 0, z);
        gl.glFramebufferTextureLayer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT1, mAoTex[1], 0, z);
        gl.glUniform1f(mUniBakeZ, (f32)z);
        DrawFullscreen();
    }
    BindTarget();
}

void GLRenderer::SetRain(u32 bits, f32 eyeX, f32 eyeY, f32 eyeZ, f32 time, f32 ground, f32 glow, f32 puddles)
{
    if (!mReady)
    {
        return;
    }
    Flush();
    mRainBits = bits;
    mRainCam[0] = eyeX, mRainCam[1] = eyeY, mRainCam[2] = eyeZ, mRainCam[3] = time;
    mRainParams[0] = ground, mRainParams[1] = glow, mRainParams[2] = puddles;
    mStateValid = false;
}

void GLRenderer::ApplyState(u32 state)
{
    if (mStateValid && state == mAppliedState)
    {
        return;
    }
    // A shadow keeps the colour (it writes zero) and takes the larger of the shadow masks.
    gl.glBlendEquation((state & RS_SHADOW) ? GL_MAX : GL_FUNC_ADD);
    if (state & RS_BLEND_ADD)
    {
        gl.glEnable(GL_BLEND);
        gl.glBlendFunc(GL_ONE, mTargetsShadows ? GL_ONE_MINUS_SRC_ALPHA : GL_ONE);
    }
    else if (state & RS_BLEND_ALPHA)
    {
        gl.glEnable(GL_BLEND);
        gl.glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    }
    else
    {
        gl.glDisable(GL_BLEND);
    }
    const SceneProgram& sp = mScene[mActiveScene];
    gl.glUniform1i(sp.mAlphaTest, (state & RS_ALPHA_TEST) ? 1 : 0);
    gl.glUniform1i(sp.mUseTex, (state & RS_NO_TEXTURE) ? 0 : 1);
    if (mActiveScene == 1)
    {
        gl.glUniform1i(sp.mAdditive, (state & RS_BLEND_ADD) ? (mTargetsShadows ? 2 : 1) : 0);
        gl.glUniform1i(sp.mPerPixel, mOptions.mPerPixelLighting ? 1 : 0);
        gl.glUniform1i(sp.mNumLights, mNumLights);
        gl.glUniform3f(sp.mAmbient, mAmbient[0], mAmbient[1], mAmbient[2]);
        gl.glUniform1f(sp.mUnlitAmbient, mUnlitAmbient);
        gl.glUniform1i(sp.mShadow, (state & RS_SHADOW) ? 1 : 0);
        static const char* sAoDebug = getenv("GTA2_AO_DEBUG");
        gl.glUniform1f(mUniAo, mAoStrength);
        gl.glUniform1i(mUniAoDebug, sAoDebug && atoi(sAoDebug) ? 1 : 0);
        gl.glUniform3f(mUniAoGrid, (f32)mAoGrid[0], (f32)mAoGrid[1], (f32)mAoGrid[2]);
        gl.glUniform1i(mUniRain, (GLint)mRainBits);
        gl.glUniform4f(mUniRainCam, mRainCam[0], mRainCam[1], mRainCam[2], mRainCam[3]);
        gl.glUniform4f(mUniRainParams, mRainParams[0], mRainParams[1], mRainParams[2], mRainParams[3]);
    }
    gl.glBindSampler(0, (state & kStateMipmap) ? mSamplerMip : (state & RS_FILTER_LINEAR) ? mSamplerLinear : mSamplerNearest);
    if (state & RS_PIXEL_EXACT)
    {
        gl.glUniform2f(sp.mOffset, 0.0f, 0.0f);
        gl.glUniform1f(sp.mSampleShift, 0.0f);
    }
    else if (mTargetsSamples)
    {
        // Edges of integer quads on pixel boundaries, so MSAA leaves them sharp; texels sampled
        // where D3D's pixel centres would see them.
        gl.glUniform2f(sp.mOffset, mOffsetX - 0.5f, mOffsetY - 0.5f);
        gl.glUniform1f(sp.mSampleShift, mSampleShift + 0.5f * mTargetW / mLogicalW);
    }
    else
    {
        gl.glUniform2f(sp.mOffset, mOffsetX, mOffsetY);
        gl.glUniform1f(sp.mSampleShift, mSampleShift);
    }
    mAppliedState = state;
    mStateValid = true;
}

void GLRenderer::DrawTriangles(const RVertex* pVerts, s32 count, RTexture* pTex, u32 state)
{
    if (!mReady || count <= 0)
    {
        return;
    }
    if (!pTex || !pTex->mId)
    {
        state |= RS_NO_TEXTURE;
        pTex = 0;
    }
    if (!mBatch.empty() && (pTex != mBatchTex || state != mBatchState || (s32)mBatch.size() + count > kMaxBatchVerts))
    {
        Flush();
    }
    mBatchTex = pTex;
    mBatchState = state;
    mBatch.insert(mBatch.end(), pVerts, pVerts + count);
}

void GLRenderer::Flush()
{
    if (mBatch.empty() || !mReady)
    {
        mBatch.clear();
        return;
    }
    BindTarget();
    const int scene = (mOptions.mBloom > 0.0f || mOptions.mPerPixelLighting || mTargetsShadows || mAoStrength > 0.0f) ? 1 : 0;
    if (scene != mActiveScene)
    {
        mActiveScene = scene;
        mStateValid = false;
    }
    gl.glUseProgram(mScene[scene].mId);
    gl.glUniform2f(mScene[scene].mScale, 2.0f / mLogicalW, 2.0f / mLogicalH);
    gl.glUniform1i(mScene[scene].mSmooth, mSmoothMagnify && !(mBatchState & RS_TEXELS) ? 1 : 0);
    gl.glBindVertexArray(mVao);
    gl.glBindBuffer(GL_ARRAY_BUFFER, mVbo);
    gl.glBufferData(GL_ARRAY_BUFFER, kMaxBatchVerts * sizeof(RVertex), NULL, GL_STREAM_DRAW);
    gl.glBufferSubData(GL_ARRAY_BUFFER, 0, mBatch.size() * sizeof(RVertex), &mBatch[0]);
    gl.glActiveTexture(GL_TEXTURE0);
    gl.glBindTexture(GL_TEXTURE_2D, mBatchTex ? mBatchTex->mId : mWhiteTex);
    const bool mipmap = mOptions.mTextureFilter > 0 && mBatchTex && mBatchTex->mLevels > 1 &&
                        (mBatchState & (RS_FILTER_LINEAR | RS_PIXEL_EXACT)) == RS_FILTER_LINEAR;
    ApplyState(mipmap ? mBatchState | kStateMipmap : mBatchState);
    gl.glDrawArrays(GL_TRIANGLES, 0, (GLsizei)mBatch.size());
    mDrawCalls++;
    mVertices += (u32)mBatch.size();
    mBatch.clear();
}

void GLRenderer::DrawFullscreen()
{
    gl.glBindVertexArray(mVao);
    gl.glDrawArrays(GL_TRIANGLES, 0, 3);
}

void GLRenderer::CompositeSurface(const u16* pPixels, s32 pitchBytes, s32 width, s32 height, u16 key)
{
    if (!mReady || !pPixels)
    {
        return;
    }
    Flush();
    mSurfaceRgba.resize((size_t)width * height);
    for (s32 y = 0; y < height; y++)
    {
        const u16* row = (const u16*)((const u8*)pPixels + (size_t)y * pitchBytes);
        u32* out = &mSurfaceRgba[(size_t)y * width];
        for (s32 x = 0; x < width; x++)
        {
            const u16 p = row[x];
            if (p == key)
            {
                out[x] = 0;
                continue;
            }
            const u32 r = (p >> 11) & 31, g = (p >> 5) & 63, b = p & 31;
            out[x] = ((r << 3) | (r >> 2)) | (((g << 2) | (g >> 4)) << 8) | (((b << 3) | (b >> 2)) << 16) | 0xFF000000u;
        }
    }
    gl.glBindTexture(GL_TEXTURE_2D, mSurfaceTex);
    if (mSurfaceW != width || mSurfaceH != height)
    {
        gl.glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, &mSurfaceRgba[0]);
        mSurfaceW = width;
        mSurfaceH = height;
    }
    else
    {
        gl.glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, width, height, GL_RGBA, GL_UNSIGNED_BYTE, &mSurfaceRgba[0]);
    }

    BindTarget();
    const GLenum buffers[3] = {GL_COLOR_ATTACHMENT0, GL_NONE, GL_COLOR_ATTACHMENT2};
    gl.glDrawBuffers(mTargetsShadows ? 3 : 1, buffers);
    gl.glUseProgram(mPresentProgram);
    gl.glUniform3f(mUniGamma, 1.0f, 1.0f, 1.0f);
    gl.glUniform1i(mUniFlipY, 0);
    gl.glActiveTexture(GL_TEXTURE0);
    gl.glBindSampler(0, mSamplerNearest);
    gl.glEnable(GL_BLEND);
    gl.glBlendEquation(GL_FUNC_ADD);
    gl.glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    DrawFullscreen();
    SetDrawBuffers();
    mStateValid = false;
}

void GLRenderer::FinishFrame()
{
    if (!mReady)
    {
        return;
    }
    Flush();
    ResolveMultisample();
    ApplyShadows();
    mPostValid = false;
    if (!mTargetsBloom || mOptions.mBloom <= 0.0f)
    {
        return;
    }
    gl.glDisable(GL_BLEND);
    gl.glActiveTexture(GL_TEXTURE0);
    gl.glBindSampler(0, mSamplerLinear);

    // Additive contributions, quarter size.
    gl.glBindFramebuffer(GL_FRAMEBUFFER, mBlurFbo[0]);
    gl.glViewport(0, 0, mBlurW, mBlurH);
    gl.glUseProgram(mDownProgram);
    gl.glUniform2f(mUniDownTexel, 1.0f / mTargetW, 1.0f / mTargetH);
    gl.glBindTexture(GL_TEXTURE_2D, mBloomSrcTex);
    DrawFullscreen();

    gl.glUseProgram(mBlurProgram);
    for (int pass = 0; pass < 2; pass++)
    {
        gl.glBindFramebuffer(GL_FRAMEBUFFER, mBlurFbo[1 - pass]);
        gl.glUniform2f(mUniBlurStep, pass == 0 ? 1.0f / mBlurW : 0.0f, pass == 0 ? 0.0f : 1.0f / mBlurH);
        gl.glBindTexture(GL_TEXTURE_2D, mBlurTex[pass]);
        DrawFullscreen();
    }

    gl.glBindFramebuffer(GL_FRAMEBUFFER, mPostFbo);
    gl.glViewport(0, 0, mTargetW, mTargetH);
    gl.glUseProgram(mComposeProgram);
    gl.glUniform1f(mUniBloomIntensity, mOptions.mBloom);
    gl.glBindTexture(GL_TEXTURE_2D, mFboTex);
    gl.glBindSampler(0, mSamplerNearest);
    gl.glActiveTexture(GL_TEXTURE1);
    gl.glBindTexture(GL_TEXTURE_2D, mBlurTex[0]);
    gl.glBindSampler(1, mSamplerLinear);
    DrawFullscreen();
    gl.glBindSampler(1, 0);
    gl.glActiveTexture(GL_TEXTURE0);
    mPostValid = true;
    mStateValid = false;
}

void GLRenderer::SetGamma(f32 r, f32 g, f32 b)
{
    mGamma[0] = r > 0.05f ? r : 0.05f;
    mGamma[1] = g > 0.05f ? g : 0.05f;
    mGamma[2] = b > 0.05f ? b : 0.05f;
}

void GLRenderer::Present(s32 drawableW, s32 drawableH)
{
    if (!mReady)
    {
        return;
    }
    Flush();
    gl.glBindFramebuffer(GL_FRAMEBUFFER, 0);
    gl.glViewport(0, 0, drawableW, drawableH);
    gl.glClearColor(0, 0, 0, 1);
    gl.glClear(GL_COLOR_BUFFER_BIT);

    // Keep the aspect ratio of the logical target.
    s32 w = drawableW;
    s32 h = (s32)((long long)drawableW * mLogicalH / mLogicalW);
    if (h > drawableH)
    {
        h = drawableH;
        w = (s32)((long long)drawableH * mLogicalW / mLogicalH);
    }
    gl.glViewport((drawableW - w) / 2, (drawableH - h) / 2, w, h);
    gl.glUseProgram(mPresentProgram);
    gl.glUniform3f(mUniGamma, mGamma[0], mGamma[1], mGamma[2]);
    gl.glUniform1i(mUniFlipY, 1);
    gl.glActiveTexture(GL_TEXTURE0);
    gl.glBindTexture(GL_TEXTURE_2D, mPostValid ? mPostTex : mFboTex);
    const bool exact = (w == mTargetW && h == mTargetH) || (w % mTargetW == 0 && h % mTargetH == 0);
    gl.glBindSampler(0, exact ? mSamplerNearest : mSamplerLinear);
    gl.glDisable(GL_BLEND);
    DrawFullscreen();
    mStateValid = false;
    if (drawableW != mDrawableW || drawableH != mDrawableH)
    {
        mDrawableW = drawableW;
        mDrawableH = drawableH;
        UpdateTargetSize();
    }
}

void GLRenderer::PresentImage(const u8* pRgba, s32 width, s32 height, s32 drawableW, s32 drawableH)
{
    if (!mReady || !pRgba || width <= 0 || height <= 0)
    {
        return;
    }
    Flush();
    gl.glBindTexture(GL_TEXTURE_2D, mSurfaceTex);
    if (mSurfaceW != width || mSurfaceH != height)
    {
        gl.glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, pRgba);
        mSurfaceW = width;
        mSurfaceH = height;
    }
    else
    {
        gl.glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, width, height, GL_RGBA, GL_UNSIGNED_BYTE, pRgba);
    }
    gl.glBindFramebuffer(GL_FRAMEBUFFER, 0);
    gl.glViewport(0, 0, drawableW, drawableH);
    gl.glClearColor(0, 0, 0, 1);
    gl.glClear(GL_COLOR_BUFFER_BIT);
    s32 w = drawableW;
    s32 h = (s32)((long long)drawableW * height / width);
    if (h > drawableH)
    {
        h = drawableH;
        w = (s32)((long long)drawableH * width / height);
    }
    gl.glViewport((drawableW - w) / 2, (drawableH - h) / 2, w, h);
    gl.glUseProgram(mPresentProgram);
    gl.glUniform3f(mUniGamma, mGamma[0], mGamma[1], mGamma[2]);
    gl.glUniform1i(mUniFlipY, 1);
    gl.glActiveTexture(GL_TEXTURE0);
    gl.glBindSampler(0, w % width == 0 && h % height == 0 ? mSamplerNearest : mSamplerLinear);
    gl.glDisable(GL_BLEND);
    DrawFullscreen();
    mStateValid = false;
}

void GLRenderer::ReadPresented(std::vector<u8>& out, s32 drawableW, s32 drawableH)
{
    out.resize((size_t)drawableW * drawableH * 4);
    if (!mReady)
    {
        return;
    }
    gl.glBindFramebuffer(GL_FRAMEBUFFER, 0);
    gl.glReadPixels(0, 0, drawableW, drawableH, GL_RGBA, GL_UNSIGNED_BYTE, &out[0]);
    const size_t row = (size_t)drawableW * 4;
    std::vector<u8> tmp(row);
    for (s32 y = 0; y < drawableH / 2; y++)
    {
        u8* a = &out[(size_t)y * row];
        u8* b = &out[(size_t)(drawableH - 1 - y) * row];
        memcpy(&tmp[0], a, row);
        memcpy(a, b, row);
        memcpy(b, &tmp[0], row);
    }
}

void GLRenderer::ReadLogical(std::vector<u8>& out)
{
    out.resize((size_t)mTargetW * mTargetH * 4);
    if (!mReady)
    {
        return;
    }
    Flush();
    gl.glBindFramebuffer(GL_FRAMEBUFFER, mPostValid ? mPostFbo : mFbo);
    gl.glReadBuffer(GL_COLOR_ATTACHMENT0);
    gl.glReadPixels(0, 0, mTargetW, mTargetH, GL_RGBA, GL_UNSIGNED_BYTE, &out[0]);
}
