// LIGHTING's DAY-NIGHT CYCLE: the hour from the level's ticks, the NOON look by day and the level's own
// (DUSK) ambient by night, the lights fading in at dusk and out at dawn, and a slight tint.
#pragma once

#include "types.hpp"

namespace PortDayNight
{
// In place of gbh_SetAmbient for the level's ambient, before anything of the frame is drawn.
void SetAmbient(f32 ambient);

// CLOCK: the hour on the HUD, by the money.
void DrawClock();

// How much of their light the level's lights give this frame: 0 at NOON, 1 with LIGHTING, 0 to 1 in the cycle.
f32 LightsShare();

// gLighting, but false by day in the cycle: cars keep their lights in their sprite, as at NOON.
bool Lit();
} // namespace PortDayNight
