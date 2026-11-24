// A network client's shot of another player's that came and went between two drawn frames: a muzzle flash at the
// shooter's gun and a short tracer to where the shot ended, for a frame or two. Purely visual.
#pragma once

#include "types.hpp"

class Sprite;

namespace PortShotTrails
{
// From pShooter's drawn gun (muzzle blocks ahead of it; the gun at x, y, z once it moved off) to the end at ex, ey, ez
void Add(const Sprite* pShooter, f32 muzzle, f32 x, f32 y, f32 z, f32 ex, f32 ey, f32 ez);
// In the map's draw, with the sprites of each layer (PortFireSmoke::DrawLayer)
void DrawLayer(s32 layer);
} // namespace PortShotTrails
