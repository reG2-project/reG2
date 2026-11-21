// RAIN proof of concept, GTA2_RAIN=streaks|wet|reflect|all (a list of them works too): falling streaks and
// splashes over the world, wet ground and the lights reflected on it in the scene shader.
#pragma once

#include "types.hpp"

namespace PortRain
{
// Before the map draw: the wet ground's camera and time for the scene shader.
void BeginDraw();
// Once the world is drawn, before the HUD: the drops and their splashes.
void Draw();
} // namespace PortRain
