// Drawing between two logic steps: every live sprite (cars, peds, objects, particles), the view camera, the HUD's
// arrows and the players' places their names are drawn at are drawn at prev + (cur - prev) * alpha, objects on a car
// or a ped where it is drawn, then all put back exactly, so the logic only ever sees whole steps.
#pragma once

#include "types.hpp"

class Sprite;

namespace PortInterp
{
// Before a logic step: the positions it starts from.
void Snapshot();
// A network client's rollback, before the resimulation's last tick: the positions the step starts from in the
// corrected game
void SnapshotResim();
// GTA2_LOOP_TRACE: the last draw's objects drawn off their way between the step before and the step (after a rollback,
// in the corrected game), and the worst of them (blocks)
void ObjectCounts(u32* pOff, f32* pWorst);
// Nothing to interpolate from (load, new game).
void Clear();
// A sprite laid this step by something that moved (dx, dy) in it, a skid mark under its wheel: drawn moving
// with it, so it ends where that is drawn.
void LaidMoving(Sprite* pSprite, s32 dx, s32 dy);
// A network client's correction is about to move this car's or ped's sprite off the place the step put the objects
// on it at: they are still drawn on it.
void Correcting(const Sprite* pSprite);
// Around a draw; alpha in [0, 1] is the fraction of the next step already elapsed.
void Apply(f32 alpha);
void Restore();
// Whether a sprite turned no further this step than a car steers (its attached objects are then drawn on it)
bool Steered(const Sprite* pSprite);
// Sprites drawn in between, drawn where they are after a jump, drawn unturned after a snap round; then zeroed.
void TakeCounts(u32* pMoved, u32* pJumped, u32* pSnapped);
} // namespace PortInterp
