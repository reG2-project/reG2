// port/net (port/docs/netcode.md, "Entities"): a ped thrown by a blast, a car or a door, as the server has it. The game
// throws a ped by a moving object (Ped::mpObj2C, ped_pushed_110) that its char follows until it stops, then lands it by
// the throw's outcome (Char::UpdateImmobilized). A client predicts every ped's throws; its snapshots correct them.
#include "internal.hpp"
#include "Source/Object_3C.hpp"
#include "Source/Object_5C.hpp"
#include "Source/Ped.hpp"
#include "Source/PedRefTable.hpp"
#include "Source/char.hpp"
#include "Source/sprite.hpp"

namespace
{
using namespace net::engine;
using net::EntityState;

const int32_t kThrowAirborne = 4;
const int kThrowCollideShift = 3;

int32_t OutcomeCode(s32 state2)
{
    return state2 - ped_state_2::knocked_recover_24 + 1;
}

s32 OutcomeState(int32_t code)
{
    return ped_state_2::knocked_recover_24 + (code & 3) - 1;
}

void PlaceObject(Object* o, const int32_t* t)
{
    o->RemoveFromCollisionBuckets();
    o->mSprite->set_xyz_lazy(Raw(t[kThrowX]), Raw(t[kThrowY]), Raw(t[kThrowZ]));
    o->mSprite->set_ang_lazy(Angle(t[kThrowRot]));
    o->AssignToBucket();
}

// The server's throw started here as Char::HandleGenericImpact starts one, its object at the server's
Object* StartThrow(Ped* ped, Char* c, const int32_t* t)
{
    Object* o = gObject_5C->NewMovingObj(objects::ped_pushed_110,
                                         Raw(t[kThrowX]),
                                         Raw(t[kThrowY]),
                                         Raw(t[kThrowZ]),
                                         Angle(t[kThrowAngle]),
                                         Angle(t[kThrowRot]),
                                         Raw(t[kThrowSpeed]),
                                         Raw(t[kThrowFriction]),
                                         Raw(t[kThrowZSpeed]));
    if (!o || !o->mObj3c || !o->mSprite)
    {
        return NULL;
    }
    c->mStateInitPending = true;
    ped->mpObj2C = o;
    if (!ped->mPedRefIdx)
    {
        ped->mPedRefIdx = gPedRefTable_7F8->AllocForPed(ped->mId);
    }
    o->SetDamageOwner(ped->mPedRefIdx);
    ped->ChangeNextPedState1(ped_state_1::immobilized_8);
    ped->ChangeNextPedState2(OutcomeState(t[kThrow]));
    c->mPedState2 = ped_state_2::knocked_recover_24;
    return o;
}
} // namespace

Object* net::engine::ThrowOf(const Ped* ped)
{
    Object* o = ped->mpObj2C;
    const bool thrown = o && o->mObj3c && o->mSprite && !ped->mCar && ped->mGameObject && ped->mPedState1 == ped_state_1::immobilized_8 &&
        ped->mPedState2 >= ped_state_2::knocked_recover_24 && ped->mPedState2 <= ped_state_2::knocked_fatal_26;
    return thrown ? o : NULL;
}

void net::engine::CaptureThrow(const Ped* ped, EntityState& e, int base)
{
    const Object* o = ThrowOf(ped);
    if (!o)
    {
        return;
    }
    const Object_3C* m = o->mObj3c;
    int32_t* t = &e.f[base];
    t[kThrow] = OutcomeCode(ped->mPedState2) | (m->mbAirborne ? kThrowAirborne : 0) | (m->mSpriteCollideMode & 3) << kThrowCollideShift;
    t[kThrowX] = o->mSprite->mXy.x.mValue;
    t[kThrowY] = o->mSprite->mXy.y.mValue;
    t[kThrowZ] = o->mSprite->mZpos.mValue;
    t[kThrowRot] = o->mSprite->mRotation.rValue;
    t[kThrowAngle] = m->mAngle.rValue;
    t[kThrowSpeed] = m->mSpeed.mValue;
    t[kThrowZSpeed] = m->mZSpeed.mValue;
    t[kThrowZAccel] = m->mZAccel.mValue;
    t[kThrowFriction] = m->mFriction.mValue;
}

// After ApplyPhases ended any other phase here: thrown as the server's, or not
void net::engine::ApplyThrow(Ped* ped, const EntityState& e, int base)
{
    Char* c = ped->mGameObject;
    if (!c || ped->mCar || ped->isDead())
    {
        return;
    }
    const int32_t* t = &e.f[base];
    Object* o = ThrowOf(ped);
    if (!t[kThrow])
    {
        if (o)
        {
            // Not thrown there: on its feet at once (its place the server's ped's), or in the server's phase (ApplyPhases)
            o->RequestRemoval();
            ped->mpObj2C = NULL;
            CancelPhase(ped);
        }
        return;
    }
    if (!o)
    {
        if (ped->mpObj2C || ped->mPedState1 == ped_state_1::immobilized_8 || AtCarDoor(ped) || !c->mSpritePtr)
        {
            return;
        }
        o = StartThrow(ped, c, t);
        if (!o)
        {
            return;
        }
    }
    PlaceObject(o, t);
    Object_3C* m = o->mObj3c;
    m->mAngle = Angle(t[kThrowAngle]);
    m->mSpeed = Raw(t[kThrowSpeed]);
    m->mZSpeed = Raw(t[kThrowZSpeed]);
    m->mZAccel = Raw(t[kThrowZAccel]);
    m->mFriction = Raw(t[kThrowFriction]);
    m->mbAirborne = (t[kThrow] & kThrowAirborne) ? 1 : 0;
    m->mSpriteCollideMode = t[kThrow] >> kThrowCollideShift & 3;
    const s32 outcome = OutcomeState(t[kThrow]);
    ped->mPedState2 = outcome;
    if (!c->mStateInitPending && c->mPedState1 == ped_state_1::immobilized_8)
    {
        c->mPedState2 = outcome;
        if (t[kPhase] & kPhaseInit)
        {
            c->mStateInitPending = true;
        }
        else
        {
            c->mTimeout = (u16)PhaseTimer(t);
        }
    }
}

void net::engine::MoveThrow(Ped* ped, int32_t dx, int32_t dy)
{
    Object* o = ThrowOf(ped);
    if (o && (dx || dy))
    {
        Sprite* s = o->mSprite;
        o->RemoveFromCollisionBuckets();
        s->set_xyz_lazy(Raw(s->mXy.x.mValue + dx), Raw(s->mXy.y.mValue + dy), s->mZpos);
        o->AssignToBucket();
    }
}

void net::engine::GuardThrow(Ped* ped)
{
    // Its timeout, counted twice a tick, makes any landing fatal at 0: a fatal one is the outcome's (its death held, ClientPedMayDie)
    if (ThrowOf(ped))
    {
        Char* c = ped->mGameObject;
        if (c->mTimeout < 3)
        {
            c->mTimeout = 3;
        }
    }
}
