// HEAT HAZE: the air shimmers over the flames of fires, blasts, burning cars and peds and the flamethrower. The flame
// sprites go into a heat mask as the game draws them; once the world is drawn, the scene is displaced over it.
#pragma once

#include "types.hpp"

class Sprite;

namespace PortHeatHaze
{
// Once the world is drawn, before the HUD.
void Draw();
// Around the game's draw of a sprite, then with NULL: flames go into the heat mask.
void HeatSprite(const Sprite* pSprite);
} // namespace PortHeatHaze
