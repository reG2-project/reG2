#include "frame_loop.hpp"
#include "Car.hpp"
#include "Draw.hpp"
#include "ExplodingScore_100.hpp"
#include "Object_5C.hpp"
#include "Frontend.hpp"
#include "Game.hpp"
#include "Hud.hpp"
#include "Player.hpp"
#include "Ped.hpp"
#include "interpolation.hpp"
#include "net_hooks.hpp"
#include "settings.hpp"
#include "sprite.hpp"
#include "view.hpp"
#include "winmain.hpp"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#if defined(GTA2_PORT_SDL)
unsigned long long port_NowNs();
void port_DelayNs(unsigned long long ns);
void port_CountGameStep();
void port_SetGameFlip(bool gameFlip);
void port_PresentContent(unsigned long long gameNs);
unsigned long long port_SwapRefreshNs();
void port_PresentAgain();
#else
namespace
{
unsigned long long port_NowNs()
{
    LARGE_INTEGER freq;
    LARGE_INTEGER now;
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&now);
    return static_cast<unsigned long long>(static_cast<f64>(now.QuadPart) * 1e9 / static_cast<f64>(freq.QuadPart));
}

void port_DelayNs(unsigned long long ns)
{
    if (ns >= 2000000)
    {
        Sleep(static_cast<DWORD>(ns / 1000000) - 1);
    }
}

void port_CountGameStep()
{
}

void port_SetGameFlip(bool)
{
}

void port_PresentContent(unsigned long long)
{
}

unsigned long long port_SwapRefreshNs()
{
    return 0;
}

void port_PresentAgain()
{
}
} // namespace
#endif

namespace PortFrameLoop
{
namespace
{
// After a stall the game catches up this many steps before a frame is drawn; the rest of the backlog is dropped.
const u32 kMaxStepsPerDraw = 4;
// Nearer than this, the next step or frame is waited for by polling the message loop.
const unsigned long long kMinDelayNs = 200000;
// The longest wait while the netcode polls the network from the message loop (gPortIdleHook), so pings stay timed.
const unsigned long long kMaxIdleDelayNs = 1000000;

bool sStarted;
unsigned long long sLast;
unsigned long long sAccumulated;
unsigned long long sStepNs = 33000000;
u32 sStepsSinceDraw;
unsigned long long sNextDraw; // timer pacing
unsigned long long sLastPresent; // vsync pacing
unsigned long long sDrawDueNs; // when a draw came due that steps still to run were put before; 0: none
u32 sRepeatsLeft;
u32 sRefreshesPerFrame;
f32 sAlpha;
bool sInDraw;
u32 sDrawSteps;
u32 sMaxFps = 30;

// GTA2_LOOP_TRACE=<file>: the player's position after each step, the drawn camera and player each frame.
// GTA2_LOOP_STATS=1: steps and frames per second on stderr.
FILE* spTrace;
bool sStats;
u32 sStepCount;
u32 sStatsSteps;
u32 sStatsDraws;
unsigned long long sStatsStart;
// SHOW FPS: the draws since sFpsStart, and sFps, their rate over the last half second.
u32 sFpsDraws;
unsigned long long sFpsStart;
u32 sFps;

bool Original()
{
    return sMaxFps == 30;
}

// The frame limit as a whole number of refreshes: the nearest one, at least 1.
u32 RefreshesPerFrame(unsigned long long refreshNs)
{
    const u32 n = sMaxFps ? static_cast<u32>(1e9 / sMaxFps / refreshNs + 0.5) : 1;
    return n ? n : 1;
}

Sprite* PlayerSprite(const Player* pPlayer)
{
    Ped* pPed = pPlayer ? pPlayer->mPlayerPed : NULL;
    return pPed && (pPed->mCar || pPed->mGameObject) ? pPed->GetSprite() : NULL;
}

void Trace(char kind)
{
    if (!spTrace)
    {
        return;
    }
    const Sprite* pSprite = PlayerSprite(gGame_0x40 ? gGame_0x40->mLocalPlayer : NULL);
    const Camera* pCamera = gGame_0x40 && gGame_0x40->mDrawPlayer ? &gGame_0x40->mDrawPlayer->mViewCamera : NULL;
    fprintf(spTrace, "%c %u %.4f cam %d %d %d player %d %d %d %d", kind, sStepCount, kind == 'D' ? sAlpha : 1.0f,
            pCamera ? pCamera->mCurPos.mX.mValue : 0, pCamera ? pCamera->mCurPos.mY.mValue : 0,
            pCamera ? pCamera->mCurPos.mZ.mValue : 0, pSprite ? pSprite->mXy.x.mValue : 0,
            pSprite ? pSprite->mXy.y.mValue : 0, pSprite ? pSprite->mZpos.mValue : 0,
            pSprite ? pSprite->mRotation.rValue : 0);
    // A network game's other players, by slot.
    for (s32 i = 0; gGame_0x40 && bStartNetworkGame && i < MAX_PLAYERS; i++)
    {
        const Player* pPlayer = gGame_0x40->GetPlayer(static_cast<u8>(i));
        const Sprite* pOther = pPlayer != gGame_0x40->mLocalPlayer ? PlayerSprite(pPlayer) : NULL;
        if (pOther)
        {
            fprintf(spTrace, " p%d %d %d %d %d", i, pOther->mXy.x.mValue, pOther->mXy.y.mValue, pOther->mZpos.mValue,
                    pOther->mRotation.rValue);
        }
    }
    // The local player's place its name and markers are drawn at (Ped::mCam) against its sprite, the exploding scores'
    // size, and how far a car's attached objects are drawn off where its drawn place puts them (blocks)
    Ped* pPed = gGame_0x40 && gGame_0x40->mLocalPlayer ? gGame_0x40->mLocalPlayer->mPlayerPed : NULL;
    if (pSprite && pPed)
    {
        fprintf(spTrace, " pedcam %d %d", pPed->mCam.x.mValue - pSprite->mXy.x.mValue, pPed->mCam.y.mValue - pSprite->mXy.y.mValue);
    }
    for (ExplodingScore* pScore = gExplodingScorePool ? gExplodingScorePool->mPool.GetFirstUsed() : NULL; pScore; pScore = pScore->mpNext)
    {
        const f32 back = kind == 'D' && pScore->mExplodeCounter <= -2 ? StepNotElapsed() : 0.0f;
        fprintf(spTrace, " score %.3f", pScore->mExplodeCounter < 0 ? -pScore->mExplodeCounter - back : 1.0f);
    }
    f32 attachedOff = 0;
    for (Car* pCar = gCar_BC_Pool ? gCar_BC_Pool->mPool.mpPrev : NULL; pCar; pCar = pCar->mpNext)
    {
        const Sprite* pCarSprite = pCar->mCarSprite;
        if (pCarSprite && kind == 'D' && !PortInterp::Steered(pCarSprite))
        {
            continue; // a train's carriage turning a corner: its lights stay put
        }
        for (AttachedSprite* a = pCarSprite ? pCar->mAttachments.mP18 : NULL; a; a = a->mpNext)
        {
            if (!a->mpSprite || !a->mpSprite->AsObject2C())
            {
                continue;
            }
            Fix16 x = pCarSprite->mXy.x, y = pCarSprite->mXy.y;
            if (a->mOffsetX != Fix16(0) || a->mOffsetY != Fix16(0))
            {
                x = a->mOffsetX;
                y = a->mOffsetY;
                Ang16 rot = pCarSprite->mRotation;
                Ang16::RotateVector(x, y, rot);
                x += pCarSprite->mXy.x;
                y += pCarSprite->mXy.y;
            }
            const f32 dx = (a->mpSprite->mXy.x.mValue - x.mValue) / 16384.0f, dy = (a->mpSprite->mXy.y.mValue - y.mValue) / 16384.0f;
            const f32 off = dx * dx + dy * dy;
            attachedOff = off > attachedOff ? off : attachedOff;
        }
    }
    fprintf(spTrace, " attached %.4f", attachedOff > 0 ? sqrtf(attachedOff) : 0.0f);
    // Each burning car's flames: the car's sprite, then each flame's sprite and its place on the car
    for (Car* pCar = gCar_BC_Pool ? gCar_BC_Pool->mPool.mpPrev : NULL; pCar; pCar = pCar->mpNext)
    {
        bool any = false;
        for (AttachedSprite* a = pCar->mCarSprite ? pCar->mAttachments.mP18 : NULL; a; a = a->mpNext)
        {
            Object* pObject = a->mpSprite ? a->mpSprite->AsObject2C() : NULL;
            if (pObject && pObject->IsFlameObject())
            {
                if (!any)
                {
                    fprintf(spTrace, " flames %d", pCar->mCarSprite->mInstanceId);
                    any = true;
                }
                fprintf(spTrace, " %d@%.2f,%.2f", a->mpSprite->mInstanceId, a->mOffsetX.mValue / 16384.0f, a->mOffsetY.mValue / 16384.0f);
            }
        }
    }
    fputc('\n', spTrace);
}

// Whether a new frame is due now, by the pacing Next uses.
bool DrawDue(unsigned long long now)
{
    if (Original())
    {
        return false;
    }
    const unsigned long long refreshNs = port_SwapRefreshNs();
    return refreshNs ? !sRepeatsLeft && now >= sLastPresent + refreshNs / 2 : now >= sNextDraw;
}

// Where a draw now sits between the last two steps. One that came due while steps ran shows the moment it came due:
// otherwise the steps' time, a network rollback's few milliseconds, would put it that far ahead of the frames around it.
f32 AlphaNow(unsigned long long now)
{
    const unsigned long long late = sDrawDueNs ? now - sDrawDueNs : 0;
    sDrawDueNs = 0;
    return sAccumulated > late ? static_cast<f32>(static_cast<f64>(sAccumulated - late) / sStepNs) : 0.0f;
}

void Stats(unsigned long long now)
{
    if (!sStats)
    {
        return;
    }
    if (!sStatsStart)
    {
        sStatsStart = now;
        return;
    }
    const f64 seconds = (now - sStatsStart) / 1e9;
    if (seconds >= 5.0)
    {
        u32 moved;
        u32 jumped;
        u32 snapped;
        PortInterp::TakeCounts(&moved, &jumped, &snapped);
        fprintf(stderr, "loop: %.2f steps/s, %.1f frames/s (frame limit %u, %u refreshes per frame), sprites per frame: %.1f in between, %.2f jumped, %.2f snapped round\n",
                sStatsSteps / seconds, sStatsDraws / seconds, sMaxFps, sRefreshesPerFrame, moved / (f64)(sStatsDraws ? sStatsDraws : 1),
                jumped / (f64)(sStatsDraws ? sStatsDraws : 1), snapped / (f64)(sStatsDraws ? sStatsDraws : 1));
        sStatsStart = now;
        sStatsSteps = 0;
        sStatsDraws = 0;
    }
}
} // namespace

void ApplySettings()
{
    sMaxFps = static_cast<u32>(PortSettings::MaxFps());
    static bool sInit;
    if (!sInit)
    {
        sInit = true;
        const char* pTrace = getenv("GTA2_LOOP_TRACE");
        spTrace = pTrace && *pTrace ? fopen(pTrace, "w") : NULL;
        sStats = getenv("GTA2_LOOP_STATS") != NULL;
        // Otherwise only the frontend's options menu hands them to the renderer, and skip_frontend never builds it.
        PortSettings::ApplyRenderOptions();
    }
}

void Reset()
{
    sStarted = false;
    sFpsStart = 0;
    sFps = 0;
    sAccumulated = 0;
    sStepsSinceDraw = 0;
    sRepeatsLeft = 0;
    sDrawDueNs = 0;
    PortInterp::Clear();
    ApplySettings();
}

Action Next(s32 stepMs, bool forceStep, bool noDraw)
{
    const unsigned long long now = port_NowNs();
    if (!sStarted)
    {
        sStarted = true;
        sLast = now;
        sNextDraw = now;
    }
    sAccumulated += now - sLast;
    sLast = now;
    sStepNs = static_cast<unsigned long long>(stepMs) * 1000000;
    Stats(now);

    if (forceStep || noDraw)
    {
        sAccumulated = 0;
        return kStep;
    }
    if (sAccumulated >= sStepNs)
    {
        if (sStepsSinceDraw < kMaxStepsPerDraw)
        {
            if (!sDrawDueNs && DrawDue(now))
            {
                sDrawDueNs = now;
            }
            sAccumulated -= sStepNs;
            return kStep;
        }
        sAccumulated %= sStepNs;
    }

    unsigned long long wake = now + (sStepNs - sAccumulated);
    const unsigned long long refreshNs = Original() ? 0 : port_SwapRefreshNs();
    sRefreshesPerFrame = refreshNs ? RefreshesPerFrame(refreshNs) : 0;
    if (Original())
    {
        if (sStepsSinceDraw)
        {
            sAlpha = 1.0f;
            return kDraw;
        }
    }
    else if (refreshNs)
    {
        // Vsync: present on every refresh, since the swap waits for it, so frames follow the screen's clock.
        // A new frame every sRefreshesPerFrame, the last one again in between.
        const unsigned long long due = sLastPresent + refreshNs / 2; // only binds when the swap does not wait (hidden window)
        if (now >= due)
        {
            sLastPresent = now;
            if (sRepeatsLeft)
            {
                sRepeatsLeft--;
                port_PresentAgain();
                return kIdle;
            }
            sRepeatsLeft = sRefreshesPerFrame - 1;
            sAlpha = AlphaNow(now);
            return kDraw;
        }
        if (due < wake)
        {
            wake = due;
        }
    }
    else
    {
        // No vsync, or no known refresh rate: a timer at the frame limit.
        if (now >= sNextDraw)
        {
            const unsigned long long interval = sMaxFps ? 1000000000ull / sMaxFps : 0;
            sNextDraw = now - sNextDraw > interval ? now + interval : sNextDraw + interval;
            sAlpha = AlphaNow(now);
            return kDraw;
        }
        if (sNextDraw < wake)
        {
            wake = sNextDraw;
        }
    }
    if (wake > now + kMinDelayNs)
    {
        const unsigned long long delay = wake - now - kMinDelayNs / 2;
        port_DelayNs(gPortIdleHook && delay > kMaxIdleDelayNs ? kMaxIdleDelayNs : delay);
    }
    return kIdle;
}

void BeforeStep()
{
    PortInterp::Snapshot();
}

void AfterStep()
{
    // GTA2_TEST_SCORE=<step>,...: an exploding score over the local player at those steps (the trace's test)
    static const char* sTestScore = getenv("GTA2_TEST_SCORE");
    for (const char* p = sTestScore; p && *p; p = strchr(p, ',') ? strchr(p, ',') + 1 : "")
    {
        Ped* pPed = gGame_0x40 && gGame_0x40->mLocalPlayer ? gGame_0x40->mLocalPlayer->mPlayerPed : NULL;
        if (pPed && gExplodingScorePool && static_cast<u32>(atoi(p)) == sStepCount)
        {
            gExplodingScorePool->PushScore(pPed->mCam.x, pPed->mCam.y, pPed->mCam.z, 1000);
        }
    }
    sStepsSinceDraw++;
    sStepCount++;
    sStatsSteps++;
    port_CountGameStep();
    Trace('S');
}

void BeginDraw()
{
    sInDraw = true;
    sDrawSteps = sStepsSinceDraw;
    if (!Original())
    {
        PortInterp::Apply(sAlpha);
    }
    Trace('D');
    port_PresentContent(static_cast<unsigned long long>((sStepCount + static_cast<f64>(sAlpha)) * sStepNs));
    PortView::BeginDraw();
    port_SetGameFlip(true);
}

void EndDraw()
{
    port_SetGameFlip(false);
    PortInterp::Restore();
    sStepsSinceDraw = 0;
    sInDraw = false;
    sStatsDraws++;
    sFpsDraws++;
    const unsigned long long now = port_NowNs();
    if (now - sFpsStart >= 500000000ull)
    {
        sFps = sFpsStart ? static_cast<u32>(sFpsDraws * 1e9 / (now - sFpsStart) + 0.5) : 0;
        sFpsStart = now;
        sFpsDraws = 0;
    }
}

void DrawFps()
{
    if (!sFps || !PortSettings::ShowFps())
    {
        return;
    }
    wchar_t text[16];
    swprintf(text, L"%u FPS", sFps);
    // Clear of the briefs along the bottom and of the power-ups' icons lower on the right.
    DrawStringScaled(text, Fix16(636 - Frontend::GetMaxTextWidth(text, gDebugFont)), Fix16(48), gDebugFont);
}

u32 StepsBeforeDraw()
{
    return sInDraw ? sDrawSteps : 1;
}

u32 StepCount()
{
    return sStepCount;
}

f32 DrawAlpha()
{
    return sInDraw && !Original() ? sAlpha : 1.0f;
}

f32 StepNotElapsed()
{
    return 1.0f - DrawAlpha();
}
} // namespace PortFrameLoop
