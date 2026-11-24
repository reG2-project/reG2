// port/net (port/docs/netcode.md, "Entities"): a player sinking (Char::DrownPed) or electrocuted (Ped::ManageShocking),
// both ended by its death (Char::UpdateImmobilized), or lying knocked down. A client follows the server's phase, and
// holds sinking and electrocution short of the death.
#include "internal.hpp"
#include "Source/Ped.hpp"
#include "Source/char.hpp"
#include "Source/sprite.hpp"

namespace
{
using namespace net::engine;
using net::EntityState;

bool InPhase(const Ped* ped, s32 state2)
{
    return ped->mPedState1 == ped_state_1::immobilized_8 && ped->mPedState2 == state2;
}

// On its feet as a live electrocuted ped gets up, its sinking's shrink undone (a ped's sprite has no visual shape)
void Cancel(Ped* ped, Char* c)
{
    ped->mPedState1 = ped_state_1::walking_0;
    ped->mPedState2 = ped_state_2::ped2_walking_0;
    ped->mShockCounter = 0;
    c->Set_F8_ped_state_1_433910(ped_state_1::walking_0);
    c->mPedState2 = ped_state_2::ped2_walking_0;
    c->mAnimationState = Char_Anim_state::Walking_0;
    c->mCharState = Char_B4_state::Walking_1;
    Sprite* s = c->mSpritePtr;
    if (s && s->mpVisualShape)
    {
        gSpriteShapePool->Remove(s->mpVisualShape);
        s->mpVisualShape = NULL;
    }
}
} // namespace

void net::engine::CapturePhases(const Ped* ped, EntityState& e)
{
    e.f[kPlayerState] |= (InPhase(ped, ped_state_2::sinking_20) ? kPlayerSinking : 0) | (InPhase(ped, ped_state_2::electrocuted_27) ? kPlayerShocked : 0);
    // An arrest's lying (Ped::mBf21C.bBusted) is the arrest's: its timer runs on below 0
    const Char* c = ped->mGameObject;
    if (c && !c->mStateInitPending && InPhase(ped, ped_state_2::lying_on_floor_22) && !ped->mBf21C.bBusted)
    {
        e.f[kPlayerLying] = (c->mLyingOnFloorTimer > 0 ? (c->mLyingOnFloorTimer < kLyingMax ? c->mLyingOnFloorTimer : kLyingMax) : 0) + 1;
    }
}

void net::engine::GuardPhases(Ped* ped)
{
    Char* c = ped ? ped->mGameObject : NULL;
    if (!c || c->mStateInitPending || c->mPedState1 != ped_state_1::immobilized_8 || c->mPedState2 != ped->mPedState2)
    {
        return;
    }
    // Sinking kills at 0 counting down, electrocution past 100 counting up, once a tick
    if (InPhase(ped, ped_state_2::sinking_20) && c->mTimer < 2)
    {
        c->mTimer = 2;
    }
    if (InPhase(ped, ped_state_2::electrocuted_27) && c->mTimer > 99)
    {
        c->mTimer = 99;
    }
}

void net::engine::ApplyPhases(Ped* ped, const EntityState& e)
{
    Char* c = ped->mGameObject;
    const int32_t state = e.f[kPlayerState];
    if (!c || ped->mCar || ped->isDead() || (state & kPlayerDead))
    {
        return;
    }
    const bool sinking = InPhase(ped, ped_state_2::sinking_20), shocked = InPhase(ped, ped_state_2::electrocuted_27);
    const int32_t lying = e.f[kPlayerLying];
    // Lying here, getting up or thrown again there (throws.cpp starts that one)
    const bool lyingHere = InPhase(ped, ped_state_2::lying_on_floor_22) && !c->mStateInitPending && !ped->mBf21C.bBusted;
    if ((sinking && !(state & kPlayerSinking)) || (shocked && !(state & kPlayerShocked)) || (lyingHere && !lying && !e.f[kPlayerThrow]))
    {
        Cancel(ped, c);
        return;
    }
    if (lyingHere && lying)
    {
        c->mLyingOnFloorTimer = (s8)(lying - 1);
        return;
    }
    // Started as the game starts them, on a ped not already held (thrown, lying, in another phase)
    if (ped->mPedState1 == ped_state_1::immobilized_8 || ped->mpObj2C || InCarTransition(ped) || !c->mSpritePtr)
    {
        return;
    }
    if (state & kPlayerSinking)
    {
        c->DrownPed();
    }
    else if (state & kPlayerShocked)
    {
        ped->ChangeNextPedState1(ped_state_1::immobilized_8);
        ped->ChangeNextPedState2(ped_state_2::electrocuted_27);
        ped->SetStateInitPending();
    }
    else if (lying && !e.f[kPlayerThrow])
    {
        ped->ChangeNextPedState1(ped_state_1::immobilized_8);
        ped->ChangeNextPedState2(ped_state_2::lying_on_floor_22);
        ped->SetStateInitPending(); // its timer the next snapshot's
    }
}
