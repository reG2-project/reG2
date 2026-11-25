// The port's game loop pacing: logic in whole steps of Game::GetFrameDurationMs() on a monotonic clock,
// as the original paces it, and frames drawn in between at the chosen rate, interpolated (interpolation.hpp).
#pragma once

#include "types.hpp"

namespace PortFrameLoop
{
enum Action
{
    kIdle,
    kStep,
    kDraw,
};

// The step duration and whether the game asks for a step now (screen size change, no-draw debug mode).
Action Next(s32 stepMs, bool forceStep, bool noDraw);

// Around ExecuteGame and around DrawAndFlip.
void BeforeStep();
void AfterStep();
void BeginDraw();
void EndDraw();

// Logic steps since the previous draw: what the original advanced once per drawn frame advances that often.
u32 StepsBeforeDraw();

// Logic steps run so far, and where the current draw sits between the last two (1: on the last).
u32 StepCount();
f32 DrawAlpha();
// The part of a step the current draw is behind the last one (0 outside a draw or without interpolation): what a value
// stepped once per logic step is drawn less of, as a fraction of its step.
f32 StepNotElapsed();

// The world's time as drawn, in seconds of logic steps: it stands still while the game is paused (its PAUSE, or the
// port's pause menu), at the last step's moment as the game's sprites do, and goes on from there.
f64 WorldSeconds();

// Two values a draw changes, put back at the end of the scope unless a logic step ran before this draw.
template <typename T>
class StepLocked
{
  public:
    StepLocked(T& a, T& b) : mA(a), mB(b), mKeepA(a), mKeepB(b)
    {
    }
    ~StepLocked()
    {
        if (!StepsBeforeDraw())
        {
            mA = mKeepA;
            mB = mKeepB;
        }
    }

  private:
    T& mA;
    T& mB;
    T mKeepA;
    T mKeepB;
};

// After a load or a new screen size: no backlog, nothing to interpolate from.
void Reset();

// SHOW FPS: the frames drawn per second, at the top right, under the health.
void DrawFps();

// The FRAME LIMIT setting changed.
void ApplySettings();
} // namespace PortFrameLoop
