// The port's lights among the game's, with the dusk lighting: car headlights (CAR HEADLIGHTS), and brake
// lights, explosions, fires, burning peds, flamethrowers, electro gun arcs, rockets and muzzle flashes
// (LIGHT EFFECTS), where the game has none of its own.
#pragma once

#include "types.hpp"

namespace PortLights
{
// With the level's lights.
void Submit(s32 min_x, s32 min_y, s32 max_x, s32 max_y);
// FIRE SMOKE's lights: the fires' and blasts' (LIGHT EFFECTS) for the smoke only, their intensity times scale, so
// that it is lit by them where the world is not (NOON, the cycle's day).
void SmokeLights(f32 scale);
} // namespace PortLights
