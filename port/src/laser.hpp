// LASER RIFLE: a weapon of the port's in weapon slot 11, which the game leaves unused. An instant red beam fired
// in bursts, then a cooldown; given by the REDLASER cheat or get_all_weapons, outside network games.
#pragma once

#include "types.hpp"

class Weapon;

namespace PortLaser
{
const s32 kWeapon = 11;

// REDLASER, toggled in the frontend as the game's cheats are.
extern bool gCheat;

// Whether a player spawns with the laser rifle (Player::InitPlayerPed).
bool Granted();

// Weapon::pull_trigger, every step the trigger is held.
void Fire(Weapon* pWeapon);

// A beam being drawn, from the shooter's muzzle as drawn; fade from 1 down to 0.
struct Beam
{
    f32 mX0, mY0, mX1, mY1, mZ, mFade;
};
s32 GetBeams(Beam* pOut, s32 max);

// In the map's draw after the sprites of each layer from 1 to 7, as FIRE SMOKE (MapRenderer::Draw).
void DrawLayer(s32 layer);

// The HUD's weapon icon, right aligned at x and from y down in the 640x480 layout; false for another weapon.
bool DrawHudIcon(s16 weapon_idx, s32 right, s32 top);
} // namespace PortLaser
