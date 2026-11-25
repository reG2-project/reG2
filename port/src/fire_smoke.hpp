// FIRE SMOKE: a ragged plume of wisps (the game's own smoke frames) and a few embers over burning cars, wrecks and
// fires, and a plume after a blast, as billboard particles. Purely visual.
#pragma once

#include "types.hpp"

namespace PortFireSmoke
{
// In the map's draw, after the sprites of each layer from 1 to 7 (MapRenderer::Draw): the smoke and embers whose
// height is in that layer (7: and above), so that the blocks drawn next hide them as they hide the game's sprites.
void DrawLayer(s32 layer);
} // namespace PortFireSmoke
