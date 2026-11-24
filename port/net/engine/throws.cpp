// port/net (port/docs/netcode.md, "Entities"): a player thrown by a blast, a car or a door, as the server has it. The
// game throws a ped by a moving object (Ped::mpObj2C, ped_pushed_110) that its char follows until it stops, then lands
// it by the throw's outcome (Char::UpdateImmobilized). A client predicts its players' throws; its snapshots correct them.
#include "internal.hpp"
#include "Source/Object_3C.hpp"
#include "Source/Object_5C.hpp"
#include "Source/Ped.hpp"
#include "Source/PedRefTable.hpp"
#include "Source/char.hpp"
#include "Source/rng.hpp"
#include "Source/sprite.hpp"

namespace
{
using namespace net::engine;
using net::EntityState;

// kPlayerThrow: 0 not thrown, else the outcome (1 back on its feet, 2 knocked down, 3 dead) | airborne << 2 | the
// object's collision mode (Object_3C::mSpriteCollideMode) << 3
const int32_t kThrowAirborne = 4;
const int kThrowCollideShift = 3;
// A throw the client landed this many ticks before the server's ends isn't started again
const uint32_t kLandedGrace = 10;
uint32_t gThrownTick[MAX_PLAYERS]; // the last tick each player was thrown here (GuardThrow)

uint32_t Tick()
{
    return gpRng ? (uint32_t)gpRng->get_cur_rng() : 0;
}

// Only the server's death is fatal here (KillNow): its Ped::Kill leaves dead_9 for any landing to restore
s32 ClientOutcome(s32 state2)
{
    return state2 == ped_state_2::knocked_fatal_26 ? ped_state_2::knocked_down_25 : state2;
}

int32_t OutcomeCode(s32 state2)
{
    return state2 - ped_state_2::knocked_recover_24 + 1;
}

s32 OutcomeState(int32_t code)
{
    return ClientOutcome(ped_state_2::knocked_recover_24 + (code & 3) - 1);
}

void PlaceObject(Object* o, const EntityState& e)
{
    o->RemoveFromCollisionBuckets();
    o->mSprite->set_xyz_lazy(Raw(e.f[kPlayerThrowX]), Raw(e.f[kPlayerThrowY]), Raw(e.f[kPlayerThrowZ]));
    o->mSprite->set_ang_lazy(Angle(e.f[kPlayerThrowRot]));
    o->AssignToBucket();
}

// The server's throw started here as Char::HandleGenericImpact starts one, its object at the server's
Object* StartThrow(Ped* ped, Char* c, const EntityState& e)
{
    const int32_t t = e.f[kPlayerThrow];
    const s32 outcome = OutcomeState(t);
    Object* o = gObject_5C->NewMovingObj(objects::ped_pushed_110,
                                         Raw(e.f[kPlayerThrowX]),
                                         Raw(e.f[kPlayerThrowY]),
                                         Raw(e.f[kPlayerThrowZ]),
                                         Angle(e.f[kPlayerThrowAngle]),
                                         Angle(e.f[kPlayerThrowRot]),
                                         Raw(e.f[kPlayerThrowSpeed]),
                                         Raw(e.f[kPlayerThrowFriction]),
                                         Raw(e.f[kPlayerThrowZSpeed]));
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
    ped->ChangeNextPedState2(outcome);
    c->mPedState2 = ped_state_2::knocked_recover_24;
    return o; // its blood is the server's hit's (ShowServerHit)
}
} // namespace

Object* net::engine::ThrowOf(Ped* ped)
{
    Object* o = ped->mpObj2C;
    const bool thrown = o && o->mObj3c && o->mSprite && !ped->mCar && ped->mGameObject && ped->mPedState1 == ped_state_1::immobilized_8 &&
        ped->mPedState2 >= ped_state_2::knocked_recover_24 && ped->mPedState2 <= ped_state_2::knocked_fatal_26;
    return thrown ? o : NULL;
}

void net::engine::CaptureThrow(Ped* ped, EntityState& e)
{
    const Object* o = ThrowOf(ped);
    if (!o)
    {
        return;
    }
    const Object_3C* m = o->mObj3c;
    e.f[kPlayerThrow] =
        OutcomeCode(ped->mPedState2) | (m->mbAirborne ? kThrowAirborne : 0) | (m->mSpriteCollideMode & 3) << kThrowCollideShift;
    e.f[kPlayerThrowX] = o->mSprite->mXy.x.mValue;
    e.f[kPlayerThrowY] = o->mSprite->mXy.y.mValue;
    e.f[kPlayerThrowZ] = o->mSprite->mZpos.mValue;
    e.f[kPlayerThrowRot] = o->mSprite->mRotation.rValue;
    e.f[kPlayerThrowAngle] = m->mAngle.rValue;
    e.f[kPlayerThrowSpeed] = m->mSpeed.mValue;
    e.f[kPlayerThrowZSpeed] = m->mZSpeed.mValue;
    e.f[kPlayerThrowZAccel] = m->mZAccel.mValue;
    e.f[kPlayerThrowFriction] = m->mFriction.mValue;
}

void net::engine::ApplyThrow(Ped* ped, const EntityState& e)
{
    Char* c = ped->mGameObject;
    if (!c || ped->mCar || ped->isDead())
    {
        return;
    }
    const int32_t t = e.f[kPlayerThrow];
    Object* o = ThrowOf(ped);
    if (!t)
    {
        if (o)
        {
            // Not thrown on the server: stopped at its place, its char lands it in its next update (peds update first)
            o->RemoveFromCollisionBuckets();
            o->mSprite->set_xyz_lazy(Raw(e.f[kPlayerX]), Raw(e.f[kPlayerY]), Raw(e.f[kPlayerZ]));
            o->AssignToBucket();
            o->mObj3c->mSpeed = Fix16(0);
            o->mObj3c->mZSpeed = Fix16(0);
            o->mObj3c->mbAirborne = 0;
            ped->mPedState2 = ped_state_2::knocked_recover_24;
        }
        return;
    }
    if (!o)
    {
        const int i = (int)e.id - 1;
        const uint32_t since = i >= 0 && i < MAX_PLAYERS ? Tick() - gThrownTick[i] : kLandedGrace + 1;
        // Knocked down and lying (no object), a new throw takes; one the client just landed stays landed
        const bool held = ped->mPedState1 == ped_state_1::immobilized_8 && ped->mPedState2 != ped_state_2::lying_on_floor_22;
        if (ped->mpObj2C || held || since <= kLandedGrace || InCarTransition(ped) || !c->mSpritePtr)
        {
            return;
        }
        o = StartThrow(ped, c, e);
        if (!o)
        {
            return;
        }
    }
    PlaceObject(o, e);
    Object_3C* m = o->mObj3c;
    m->mAngle = Angle(e.f[kPlayerThrowAngle]);
    m->mSpeed = Raw(e.f[kPlayerThrowSpeed]);
    m->mZSpeed = Raw(e.f[kPlayerThrowZSpeed]);
    m->mZAccel = Raw(e.f[kPlayerThrowZAccel]);
    m->mFriction = Raw(e.f[kPlayerThrowFriction]);
    m->mbAirborne = (t & kThrowAirborne) ? 1 : 0;
    m->mSpriteCollideMode = t >> kThrowCollideShift & 3;
    const s32 outcome = OutcomeState(t);
    ped->mPedState2 = outcome;
    if (!c->mStateInitPending && c->mPedState1 == ped_state_1::immobilized_8)
    {
        c->mPedState2 = outcome;
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

void net::engine::GuardThrow(int i, Ped* ped)
{
    if (ped && i >= 0 && i < MAX_PLAYERS && ThrowOf(ped))
    {
        gThrownTick[i] = Tick();
        ped->mPedState2 = ClientOutcome(ped->mPedState2);
        // Its timeout, counted twice a tick, makes the landing fatal at 0: only a snapshot ends a long throw
        Char* c = ped->mGameObject;
        if (c->mTimeout < 3)
        {
            c->mTimeout = 3;
        }
    }
}
