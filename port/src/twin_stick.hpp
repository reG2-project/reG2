// CONTROLS TWIN STICK, on foot: the left stick or the movement keys walk and the right stick or the mouse
// aims, both relative to the screen. The game's controls do the rest: mTurnSpeed turns the ped to the aim
// and mMoveDirection walks it, in a direction of its own (Walk).
#pragma once

#include "ang16.hpp"

class Camera;
class Car;
class Char;
class Fix16_Point;
class Ped;
class Player;
class Sprite;
struct STexture;
struct Vert;

namespace PortTwinStick
{
// At the end of Player::DoPedControlInputs.
void PedControls(Player* pPlayer, Ped* pPed);
// At the start of Char::UpdateWalking: true if this step walks along the move, the facing to end with in *pFacing.
bool Walk(Char* pChar, Ang16* pFacing);
// In Char::Update, after the velocity along the facing: the walk's velocity is along its own direction, as
// the original's backward walk gives it (the shots inherit it).
void WalkVelocity(Char* pChar);
// The ped's velocity the muzzle flash and the machine gun's bullet start ahead by, and a throw inherits. The original's
// walk is along the facing; the TWIN STICK player's adds only its part toward the facing, so a walk off it keeps them
// at the gun and the throw along the aim, and a walk toward it looks as the original's.
Fix16_Point ShotLead(const Char* pChar);
// In Sprite::Draw, for a quad of the sprite (its world corners after it if count is 8): draws the player's walk off the
// facing, its steps shorter toward the side and turned to the move backpedalling. False if the quad is drawn as usual.
bool DrawLegs(const Sprite* pSprite, STexture* pTexture, u32 flags, const Vert* pVerts, s32 count);
// In Car::HandleUserInput, unless SPECIAL turns the roof gun: the player's fire truck, tank or gun jeep
// turns its roof gun toward the aim.
void AimTurret(Car* pCar);
// In Ped::ComputeAimAngle: true if the player's machine guns fire where he faces, without the original's
// lock on a ped in front or on a nearby shooter (Ped::ComputeAimAngle).
bool FreeAim(const Ped* pPed);
// In Ped::UpdateFacingAngle, a ped standing to shoot: its aim at the player walking on foot leads him, a part of the
// way his bullets need, as he can walk off his own aim to sidestep them.
void LeadAim(Ped* pShooter);
// With the HUD.
void DrawReticle();
// AIM CAMERA: after Camera::ApplyPanOffset in Camera::UpdateFollowPedCamera, the target moved along the aim.
void AimCamera(Camera* pCamera, Ped* pPed, Fix16& x, Fix16& y);
} // namespace PortTwinStick
