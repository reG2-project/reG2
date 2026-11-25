// port/net (port/docs/netcode.md, "Entities"): a ped's phase, as the server has it: thrown (throws.cpp), lying knocked
// down, sinking (Char::DrownPed), electrocuted (Ped::ManageShocking), pulled out at a car door. A client follows the
// server's at every snapshot: starts it the game's way, writes its timer, or cancels its own; and holds the fatal ends of
// its own (GuardPhases, ClientPedMayDie) for the server's death (KillNow).
#include "internal.hpp"
#include "Source/Car.hpp"
#include "Source/Object_5C.hpp"
#include "Source/char_anim_state_more.hpp"
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
} // namespace

int32_t net::engine::PhaseOf(const Ped* ped)
{
    if (ThrowOf(ped))
    {
        return kPhaseThrown;
    }
    if (ped->mPedState1 != ped_state_1::immobilized_8 || !ped->mGameObject || ped->mCar)
    {
        return kPhaseNone;
    }
    switch (ped->mPedState2)
    {
        case ped_state_2::lying_on_floor_22:
            return ped->mBf21C.bBusted ? kPhaseNone : kPhaseLying; // an arrest's lying is the arrest's
        case ped_state_2::sinking_20:
            return kPhaseSinking;
        case ped_state_2::electrocuted_27:
            return kPhaseShocked;
        case ped_state_2::ped2_at_car_door_17:
            return kPhaseDoor;
    }
    return kPhaseNone;
}

int32_t net::engine::PhaseTimer(const int32_t* f)
{
    // As the game starts them: a char's timeout 500 (knocked down 300), lying 60, sinking 10, electrocuted 0 up to 100
    static const int32_t kMost[] = {0, 500, 60, 10, 100, 0x7F, 0, 0};
    const int32_t timer = f[kPhaseTimer];
    const int32_t most = kMost[f[kPhase] & 7];
    return timer < 0 ? 0 : timer > most ? most : timer;
}

// On its feet as a live electrocuted ped gets up, its sinking's shrink undone (a ped's sprite has no visual shape)
void net::engine::CancelPhase(Ped* ped)
{
    Char* c = ped->mGameObject;
    ped->mPedState1 = ped_state_1::walking_0;
    ped->mPedState2 = ped_state_2::ped2_walking_0;
    ped->mShockCounter = 0;
    c->Set_F8_ped_state_1_433910(ped_state_1::walking_0);
    c->mPedState2 = ped_state_2::ped2_walking_0;
    c->mAnimationState = Char_Anim_state::Walking_0;
    c->mCharState = Char_B4_state::Walking_1;
    c->mStateInitPending = false;
    c->mTargetCar = NULL; // a door's car, read at the door only (Char::UpdateImmobilized)
    Sprite* s = c->mSpritePtr;
    if (s && s->mpVisualShape)
    {
        gSpriteShapePool->Remove(s->mpVisualShape);
        s->mpVisualShape = NULL;
    }
}

void net::engine::CapturePhases(const Ped* ped, EntityState& e, int base)
{
    const int32_t phase = PhaseOf(ped);
    if (!phase)
    {
        return;
    }
    const Char* c = ped->mGameObject;
    int32_t* f = &e.f[base];
    f[kPhase] = phase | (c->mStateInitPending ? kPhaseInit : 0);
    switch (phase)
    {
        case kPhaseThrown:
            f[kPhaseTimer] = c->mTimeout;
            break;
        case kPhaseLying:
            f[kPhaseTimer] = c->mLyingOnFloorTimer < 0 ? 0 : c->mLyingOnFloorTimer;
            break;
        case kPhaseSinking:
        case kPhaseShocked:
            f[kPhaseTimer] = c->mTimer;
            break;
        case kPhaseDoor:
            f[kPhaseTimer] = (c->mAnimationFrame & 3) | (ped->mTargetCarDoor & 3) << 2 | (c->mFrameTimer & 7) << 4;
            f[kPhaseCar] = CarRef(c->mTargetCar);
            break;
    }
    CaptureThrow(ped, e, base);
}

void net::engine::GuardPhases(Ped* ped)
{
    GuardThrow(ped);
    Char* c = ped->mGameObject;
    // Thrown without its object (gone: GuardPeds), its char kills it where it lies (Char::UpdateImmobilized): lying instead
    const s32 s2 = ped->mPedState2;
    if (c && !ped->mCar && ped->mPedState1 == ped_state_1::immobilized_8 && s2 >= ped_state_2::knocked_recover_24 &&
        s2 <= ped_state_2::knocked_fatal_26 && !ThrowOf(ped))
    {
        ped->mpObj2C = NULL;
        ped->mPedState2 = ped_state_2::lying_on_floor_22;
        c->mPedState2 = ped_state_2::lying_on_floor_22;
        c->mStateInitPending = true;
        return;
    }
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
    // A player lying at 0 health dies as it gets up (Char::UpdateImmobilized): down until the server's death, not up ahead of it
    if (InPhase(ped, ped_state_2::lying_on_floor_22) && c->mLyingOnFloorTimer < 2 && PlayerDyingThere(ped))
    {
        c->mLyingOnFloorTimer = 2;
    }
}

void net::engine::ApplyPhases(Ped* ped, const EntityState& e, int base, bool deadThere, Car* seat)
{
    const int32_t* f = &e.f[base];
    const int32_t there = deadThere ? kPhaseNone : f[kPhase] & 7;
    if (ped->mCar)
    {
        // Pulled out there of the car it sits in here: before the seat is settled (that would put it out walking)
        Car* car = RefCar(f[kPhaseCar]);
        if (there == kPhaseDoor && car && car == ped->mCar)
        {
            StartPulledOut(ped, car, f[kPhaseTimer] >> 2 & 3, Raw(e.f[kPedX]), Raw(e.f[kPedY]), Raw(e.f[kPedZ]));
        }
        return;
    }
    Char* c = ped->mGameObject;
    if (!c || ped->isDead())
    {
        return;
    }
    if (deadThere)
    {
        // Thrown there out of another phase here (lying, sinking): that one ends for the throw, as the server's did
        const int32_t here = PhaseOf(ped);
        if ((f[kPhase] & 7) == kPhaseThrown && here > kPhaseThrown && here != kPhaseDoor)
        {
            CancelPhase(ped);
        }
        ApplyThrow(ped, e, base); // a thrown ped's death takes at its landing; the others' end as KillNow ends them
        return;
    }
    int32_t here = PhaseOf(ped);
    // Held otherwise here (a fall, a throw's state without its object) while the server's has a phase; at a car door here
    // and back in that car there: the seat's (ForceIntoCar)
    const bool held = here == kPhaseNone && there && ped->mPedState1 == ped_state_1::immobilized_8;
    if (here != there && (here > kPhaseThrown || held) && !(here == kPhaseDoor && seat && seat == c->mTargetCar))
    {
        if (ped->mpObj2C && !ThrowOf(ped))
        {
            ped->mpObj2C->RequestRemoval();
            ped->mpObj2C = NULL;
        }
        CancelPhase(ped);
    }
    // Getting in or out at a car door here while the server's ped is thrown, down, sinking or electrocuted: called off
    // here as CancelJack does, then the server's phase started below
    if (there && there != kPhaseDoor && here == kPhaseNone && AtCarDoor(ped))
    {
        ped->SetObjective(objectives_enum::no_obj_0, 9999);
        ped->SetObjective2(objectives_enum::no_obj_0);
        CancelPhase(ped);
    }
    if (there == kPhaseDoor && here != kPhaseDoor)
    {
        // On foot here (a pull predicted ahead of the server's): at the server's door, its frame the server's (below)
        StartPulledOut(ped, RefCar(f[kPhaseCar]), f[kPhaseTimer] >> 2 & 3, Raw(e.f[kPedX]), Raw(e.f[kPedY]), Raw(e.f[kPedZ]));
        if (PhaseOf(ped) != kPhaseDoor)
        {
            return;
        }
        c->mStateInitPending = false;
        here = kPhaseDoor;
    }
    ApplyThrow(ped, e, base);
    if (here == there && there > kPhaseThrown)
    {
        if (f[kPhase] & kPhaseInit)
        {
            c->mStateInitPending = true; // started there this tick: its timers are the start's
            return;
        }
        if (c->mStateInitPending)
        {
            return;
        }
        const int32_t timer = PhaseTimer(f);
        switch (there)
        {
            case kPhaseLying:
                c->mLyingOnFloorTimer = (s8)timer;
                break;
            case kPhaseSinking:
            case kPhaseShocked:
                c->mTimer = (u16)timer;
                break;
            case kPhaseDoor:
                if (Car* car = RefCar(f[kPhaseCar]))
                {
                    c->mAnimationFrame = (u8)(timer & 3);
                    c->mFrameTimer = (s8)(timer >> 4 & 7);
                    ped->mTargetCarDoor = (u8)(timer >> 2 & 3);
                    c->mTargetCar = car;
                }
                break;
        }
        return;
    }
    // Started as the game starts them, on a ped not held otherwise (thrown, at a car door)
    if (there <= kPhaseThrown || ped->mPedState1 == ped_state_1::immobilized_8 || ped->mpObj2C || AtCarDoor(ped) || !c->mSpritePtr)
    {
        return;
    }
    if (there == kPhaseSinking)
    {
        c->DrownPed();
        return;
    }
    ped->ChangeNextPedState1(ped_state_1::immobilized_8);
    ped->ChangeNextPedState2(there == kPhaseShocked ? ped_state_2::electrocuted_27 : ped_state_2::lying_on_floor_22);
    ped->SetStateInitPending();
}
