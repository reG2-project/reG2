// Ambient occlusion from the map's blocks: how much of each block is solid, handed to the renderer, which
// darkens the ambient light in corners and at the foot of walls (AMBIENT OCCLUSION setting).
#pragma once

#include "types.hpp"

namespace PortAmbientOcclusion
{
// Once per drawn frame, before the map: builds the volume for a new map and keeps it in step with block changes.
void Update();
} // namespace PortAmbientOcclusion
