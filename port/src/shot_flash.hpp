// A network client's view of another player's shot that it learned of late: the game's own muzzle flash
// (ParticleManager::GunMuzzelFlash) played from its start at the shooter's drawn gun. Purely visual.
#pragma once

#include "types.hpp"

class Sprite;

namespace PortShotFlash
{
// A flash from the game tick <tick> on
void Add(const Sprite* pShooter, u32 tick);
void Clear();
// The game tick the frame shows
void SetTick(u32 tick);
// In the map's draw, with the sprites of each layer (PortFireSmoke::DrawLayer)
void DrawLayer(s32 layer);
} // namespace PortShotFlash
