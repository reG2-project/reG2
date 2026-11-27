// port/net (port/docs/netcode.md, "Entities"): a ped's AI state as the server has it. A client runs every ped's AI in its
// resimulations; with only a position and a heading to go on, a ped made here (or one adopted) decided its own way and
// every snapshot pulled it back. Now a ped entity carries what the AI runs on: the plain values (objectives and their
// status, targets, speeds, timers, states, occupation, type, flags, health) and the references (the peds and cars it
// is after, its gang), as net ids. Local to each side and left alone: group, patrol route, pathfinder list, the ped
// reference table, weapons and objects (not synced).
#include "internal.hpp"
#include "members.hpp"
#include "Source/Car.hpp"
#include "Source/Char_Pool.hpp"
#include "Source/Gang.hpp"
#include "Source/Ped.hpp"
#include "Source/Player.hpp"
#include "Source/char.hpp"
#include "Source/Weapon.hpp"
#include "Source/PedGroup.hpp"
#include "Source/Game.hpp"
#include "Source/PublicTransport.hpp"
#include <stddef.h>
#include <string.h>
#include <algorithm>
#include <map>
#include <set>
#include <vector>

namespace
{
using net::engine::Member;
#define PM(member, sz, sg) NET_MEMBER(Ped, member, sz, sg)
const Member kValues[] = {
    PM(mFacingAngle, 2, true), PM(mAimAngle, 2, true), PM(mTargetFacingAngle, 2, true),
    PM(mFollowCarOffsetAngle, 2, true), PM(mRotation, 2, true), PM(mTargetX, 4, true),
    PM(mTargetY, 4, true), PM(mTargetZ, 4, true), PM(mMoveTargetX, 4, true),
    PM(mMoveTargetY, 4, true), PM(mMoveTargetZ, 4, true), PM(mInternalTargetX, 4, true),
    PM(mInternalTargetY, 4, true), PM(mInternalTargetZ, 4, true), PM(mObjectiveTargetX, 4, true),
    PM(mObjectiveTargetY, 4, true), PM(mObjectiveTargetZ, 4, true), PM(mTargetDistanceLimit, 4, true),
    PM(mMaxSpeed, 4, true), PM(mWalkSpeed, 4, true), PM(mRunSpeed, 4, true),
    PM(mFollowCarOffsetDistance, 4, true), PM(mInvulnerability, 2, false), PM(mWantedPoints, 2, true),
    PM(mTargetUnseenCounter, 2, false), PM(mShockCounter, 2, false), PM(mElectrocutionThreshold, 2, false),
    PM(mObjectiveTimer, 2, false), PM(mInternalObjectiveTimer, 2, false), PM(mLastVoiceRng, 4, true),
    PM(mBf224, 1, false), PM(mObjectiveStatus, 1, false), PM(mInternalObjectiveStatus, 1, true),
    PM(mHitCount, 1, true), PM(mMugCount, 1, false), PM(mPedClass, 4, true), PM(mJumpOverMode, 4, true),
    PM(mLifetimeTimer, 1, true), PM(mPedType, 4, true), PM(mOccupation, 4, true),
    PM(mEnterCarAsPassenger, 4, true), PM(mTargetCarDoor, 1, false), PM(mObjective, 4, true),
    PM(mInternalObjective, 4, true), PM(mWaitCounter, 1, true), PM(mAttackersCount, 1, false),
    PM(mPrevAttackersCount, 1, false), PM(mKillerIdTimer, 1, false), PM(mElectrocutionTimer, 1, true),
    PM(mRecentCrimeTimer, 1, false), PM(mGraphicType, 4, true), PM(mFireMode, 4, true),
    PM(mGangCarModel, 4, true), PM(mThreatSearch, 4, true), PM(mThreatReaction, 4, true),
    PM(mDeathCause, 4, true)};
#undef PM
const size_t kValueCount = sizeof(kValues) / sizeof(kValues[0]);

const int kObjective = net::engine::MemberIndex(kValues, kValueCount, offsetof(Ped, mObjective));
const int kInternalObjective = net::engine::MemberIndex(kValues, kValueCount, offsetof(Ped, mInternalObjective));
const int kPassenger = net::engine::MemberIndex(kValues, kValueCount, offsetof(Ped, mEnterCarAsPassenger));
const int kDoor = net::engine::MemberIndex(kValues, kValueCount, offsetof(Ped, mTargetCarDoor));
const int kType = net::engine::MemberIndex(kValues, kValueCount, offsetof(Ped, mPedType));
const int kOccupation = net::engine::MemberIndex(kValues, kValueCount, offsetof(Ped, mOccupation));

// The flags that are the ped's own behaviour; the others say where it is in this game's structures (active, scheduled for
// removal, in the pathfinder's list, on fire with a flame object of this game's: sent, and applied as the game sets it)
const u32 kOnFire = 1u << 24;
const u32 kFlagsSent = ~((1u << 0) | (1u << 10) | (1u << 14) | kOnFire);

// Its states (Ped::mPedState1 ..): set when both sides have it on foot and alive, in the same phase; in, into and out of
// cars and deaths go their own ways (replica.cpp, below)
enum
{
    kState1,
    kState2,
    kStored1,
    kStored2,
    kStateCount
};

// References, as net ids (0: none)
enum
{
    kRefAttacker,
    kRefObjectivePed,
    kRefInternalPed,
    kRefCarThief,
    kRefLastPunched,
    kRefHitTarget,
    kRefKiller,
    kPedRefCount
};
enum
{
    kRefStolenCar,
    kRefObjectiveCar,
    kRefTargetToEnter,
    kRefEscapeCar,
    kCarRefCount
};

// The field layout after kPedState: values, flags, health, states, ped refs, car refs, gangs
const int kFirst = net::engine::kPedAi;
const int kFlags = kFirst + (int)kValueCount;
const int kHealth = kFlags + 1;
const int kStates = kHealth + 1;
const int kPedRefs = kStates + kStateCount;
const int kCarRefs = kPedRefs + kPedRefCount;
const int kGangs = kCarRefs + kCarRefCount; // its gang, a dummy's gang
const int kChar = kGangs + 2; // its char's movement and animation (0 without a char)
#define CM(member, sz, sg) NET_MEMBER(Char, member, sz, sg)
const Member kCharValues[] = {CM(mCharState, 4, true),      CM(mTargetRotation, 2, true),  CM(mVelocity, 4, true),
                              CM(mRunOrJumpSpeed, 4, true), CM(mVelocityVector.x, 4, true), CM(mVelocityVector.y, 4, true),
                              CM(mAnimationState, 4, true), CM(mAnimationFrame, 1, false), CM(mFrameTimer, 1, true),
                              CM(mFrameDelay, 1, true),
                              // its wander: the next idle behaviour's timer and the turn it takes (Char::SelectRandomIdleBehavior,
                              // ApplyRandomRotationJitter), its jumps, and the push off another ped (Char::UpdateFleeOrRunning)
                              CM(mTimer, 2, false), CM(mRotationJitter, 2, true), CM(mJumpScaleCounter, 1, true),
                              CM(mRepulsionTimer, 1, false), CM(mRepulsionRotation, 2, true)};
#undef CM
const size_t kCharCount = sizeof(kCharValues) / sizeof(kCharValues[0]);
const int kCharAnim = net::engine::MemberIndex(kCharValues, kCharCount, offsetof(Char, mAnimationState));
const int kGroup = kChar + (int)kCharCount; // its group: leader (net id), its index (99: the leader), type, counts
const int kWeapons = kGroup + 5; // its weapon and its second one: weapon_type + 1, 0 none
const int kEnd = kWeapons + 2; // then its phase (PhaseField)
static_assert(kEnd + net::engine::kPhaseFieldCount + 1 <= net::kMaxFields, "a ped's fields, its phase and its age (snapshot.cpp)");

int32_t WeaponRef(const Weapon* w) // weapon_type + 1, 0 none
{
    return w ? w->mWeaponType + 1 : 0;
}

uint32_t PedId(const Ped* p) // the server's side
{
    if (!p)
    {
        return 0;
    }
    if (p->mPlayer)
    {
        return 1 + p->mPlayer->mPlayerIdx; // a player entity
    }
    return net::engine::kPedIdBase + p->mId;
}

uint32_t CarId(const Car* c)
{
    return c ? net::engine::kCarIdBase + c->mCarId : 0;
}

int GangIdx(const Gang* g)
{
    const Gang* first = gGangPool_CA8 ? &gGangPool_CA8->mGangList[0] : NULL;
    return g && first && g >= first && g < first + 10 ? (int)(g - first) + 1 : 0;
}

Gang* GangAt(int idx)
{
    return idx >= 1 && idx <= 10 && gGangPool_CA8 ? &gGangPool_CA8->mGangList[idx - 1] : NULL;
}

Ped* const* PedRefs(const Ped* p, Ped** out) // in kRef order
{
    out[kRefAttacker] = p->mAttacker;
    out[kRefObjectivePed] = p->mObjectiveTargetPed;
    out[kRefInternalPed] = p->mInternalTargetPed;
    out[kRefCarThief] = p->mCarThief;
    out[kRefLastPunched] = p->mLastCharPunched;
    out[kRefHitTarget] = p->mHitTargetPed;
    out[kRefKiller] = p->mPedKiller;
    return out;
}
} // namespace

void net::engine::PedStateFields(std::vector<FieldDesc>& out)
{
    AppendFields(kValues, kValueCount, out);
    out.push_back(FieldDesc{"flags", 32, false});
    out.push_back(FieldDesc{"health", 16, true});
    static const char* const kStateNames[kStateCount] = {"state1", "state2", "stored1", "stored2"};
    for (int i = 0; i < kStateCount; i++)
    {
        out.push_back(FieldDesc{kStateNames[i], 5, false});
    }
    static const char* const kPedRefNames[kPedRefCount] = {"attacker", "objective_ped", "internal_ped", "car_thief", "last_punched", "hit_target", "killer"};
    for (int i = 0; i < kPedRefCount; i++)
    {
        out.push_back(FieldDesc{kPedRefNames[i], 32, false});
    }
    static const char* const kCarRefNames[kCarRefCount] = {"stolen_car", "objective_car", "target_to_enter", "escape_car"};
    for (int i = 0; i < kCarRefCount; i++)
    {
        out.push_back(FieldDesc{kCarRefNames[i], 32, false});
    }
    out.push_back(FieldDesc{"gang", 4, false});
    out.push_back(FieldDesc{"dummy_gang", 4, false});
    AppendFields(kCharValues, kCharCount, out);
    out.push_back(FieldDesc{"group_leader", 32, false});
    out.push_back(FieldDesc{"group_idx", 8, false});
    out.push_back(FieldDesc{"group_type", 8, true});
    out.push_back(FieldDesc{"group_count", 8, false});
    out.push_back(FieldDesc{"group_count2", 8, false});
    out.push_back(FieldDesc{"weapon", 6, false});
    out.push_back(FieldDesc{"weapon2", 6, false});
}

void net::engine::PedStateRefFields(std::vector<int>* peds, std::vector<int>* cars)
{
    for (int i = 0; i < kPedRefCount; i++)
    {
        peds->push_back(kPedRefs + i);
    }
    peds->push_back(kGroup);
    for (int i = 0; i < kCarRefCount; i++)
    {
        cars->push_back(kCarRefs + i);
    }
}

const int net::engine::kPedPhase = kEnd;

uint32_t net::engine::PedNetId(const Ped* p)
{
    return PedId(p);
}

void net::engine::CapturePedState(const Ped* p, EntityState& e)
{
    CaptureMembers(p, kValues, kValueCount, e, kFirst);
    u32 flags;
    memcpy(&flags, &p->mBf21C, sizeof(flags));
    e.f[kFlags] = (int32_t)(flags & (kFlagsSent | kOnFire));
    e.f[kHealth] = p->mHealth;
    e.f[kStates + kState1] = p->mPedState1;
    e.f[kStates + kState2] = p->mPedState2;
    e.f[kStates + kStored1] = p->mStoredPedState1;
    e.f[kStates + kStored2] = p->mStoredPedState2;
    Ped* refs[kPedRefCount];
    PedRefs(p, refs);
    for (int i = 0; i < kPedRefCount; i++)
    {
        e.f[kPedRefs + i] = (int32_t)PedId(refs[i]);
    }
    e.f[kCarRefs + kRefStolenCar] = (int32_t)CarId(p->mStolenCar);
    e.f[kCarRefs + kRefObjectiveCar] = (int32_t)CarId(p->mTargetObjectiveCar);
    e.f[kCarRefs + kRefTargetToEnter] = (int32_t)CarId(p->mTargetToEnter);
    e.f[kCarRefs + kRefEscapeCar] = (int32_t)CarId(p->mEscapeCar);
    e.f[kGangs] = GangIdx(p->mpGang);
    e.f[kGangs + 1] = GangIdx(p->mDummyGang);
    if (const Char* c = p->mGameObject)
    {
        CaptureMembers(c, kCharValues, kCharCount, e, kChar);
    }
    e.f[kWeapons] = WeaponRef(p->mSelectedWeapon);
    e.f[kWeapons + 1] = WeaponRef(p->mpWeapon);
    if (const PedGroup* g = p->mPedGroup)
    {
        e.f[kGroup] = (int32_t)PedId(g->mPedLeader);
        e.f[kGroup + 1] = p->mGroupIdx;
        e.f[kGroup + 2] = g->mGroupType;
        e.f[kGroup + 3] = g->mCount34;
        e.f[kGroup + 4] = (u8)g->mCount36;
    }
    CapturePhases(p, e, kEnd);
}

namespace
{
Ped* LocalPed(uint32_t id, bool* pResolved) // the client's side: a net id to this game's ped
{
    *pResolved = true;
    if (!id)
    {
        return NULL;
    }
    if (id < net::engine::kCarIdBase)
    {
        Ped* p = net::engine::InGamePed((int)id - 1);
        *pResolved = p != NULL;
        return p;
    }
    Ped* p = net::engine::ReplicaPed(id);
    *pResolved = p != NULL;
    return p;
}

Car* LocalCar(uint32_t id, bool* pResolved)
{
    *pResolved = true;
    if (!id)
    {
        return NULL;
    }
    Car* c = net::engine::ReplicaCar(id);
    *pResolved = c && c->mCarSprite;
    return *pResolved ? c : NULL;
}

// The targets the objectives' code reads without a check (Ped::KillCharOnFoot, FollowCarInCurrCar, FleeFromPedTillSafe,
// WaitForTrain...): the server's ped always has them, so a state that lacks one isn't taken
bool ObjectiveTargetsThere(const Ped* p, const net::EntityState& e)
{
    using namespace objectives_enum;
    switch (e.f[kFirst + kObjective])
    {
        case flee_char_on_foot_till_safe_2:
        case flee_char_on_foot_always_3:
        case goto_char_on_foot_16:
        case kill_char_any_means_19:
        case kill_char_on_foot_20:
        case punch_char_23:
        case pull_driver_out_32:
            if (!e.f[kPedRefs + kRefObjectivePed])
            {
                return false;
            }
            break;
        case enter_car_as_driver_35:
        case enter_train_37:
        case leave_train_38:
        case follow_car_on_foot_with_offset_56:
        case destroy_car_59:
            if (!e.f[kCarRefs + kRefObjectiveCar])
            {
                return false;
            }
            break;
        case follow_car_in_car_55:
        case turret_put_out_car_fire_60:
            if (!e.f[kCarRefs + kRefObjectiveCar] || !p->mCar)
            {
                return false;
            }
            break;
        case follow_char_in_car_10:
        case patrol_in_car_43:
        case chase_char_in_car_52:
        case fire_at_object_from_vehicle_57:
        case tank_fire_at_player_61:
            if (!p->mCar)
            {
                return false;
            }
            break;
    }
    switch (e.f[kFirst + kInternalObjective])
    {
        case flee_char_on_foot_till_safe_2:
        case flee_char_on_foot_always_3:
        case run_to_char_7:
        case follow_char_9:
        case stay_close_to_char_11:
        case punch_char_23:
        case pull_driver_out_32:
            if (!e.f[kPedRefs + kRefInternalPed])
            {
                return false;
            }
            break;
        case wait_for_train_29:
        {
            bool ok;
            Car* car = LocalCar((uint32_t)e.f[kCarRefs + kRefTargetToEnter], &ok);
            return car && gPublicTransport_181C->GetTrainFromCarExcludingLeadCar(car);
        }
    }
    return true;
}

bool RobbedDriver(int32_t occupation) // Ped::RobbedDriver_AI's: it reads its car thief and its car unchecked
{
    return occupation == ped_ocupation_enum::robbed_driver_10 || occupation == ped_ocupation_enum::angry_armed_robbed_driver_12 ||
           occupation == ped_ocupation_enum::very_angry_armed_robbed_driver_13;
}

// Its weapons, the game's way of arming a ped (Ped::ForceWeapon drops the one it had): a gang member here had
// none and never showed the shots the server's fired
void ApplyWeapons(Ped* p, const net::EntityState& e)
{
    if (WeaponRef(p->mSelectedWeapon) != e.f[kWeapons])
    {
        if (e.f[kWeapons])
        {
            p->ForceWeapon(e.f[kWeapons] - 1);
        }
        else
        {
            p->RemovePedWeapons();
        }
    }
    if (WeaponRef(p->mpWeapon) != e.f[kWeapons + 1])
    {
        if (e.f[kWeapons + 1])
        {
            p->GiveWeapon(e.f[kWeapons + 1] - 1);
        }
        else
        {
            p->RemoveSecondaryWeapon();
        }
    }
}
} // namespace

void net::engine::ApplyPedGroups(const std::vector<std::pair<Ped*, const EntityState*>>& peds)
{
    // The members each leader has here, in the server's order: the game walks a group's list up to its count
    // (mCount34) without looking for gaps, so it is made dense with only the members that are here
    std::map<Ped*, std::vector<std::pair<int, Ped*>>> byLeader;
    std::set<Ped*> placed;
    for (size_t k = 0; k < peds.size(); k++)
    {
        Ped* p = peds[k].first;
        const EntityState& e = *peds[k].second;
        bool ok;
        Ped* leader = e.f[kGroup] ? LocalPed((uint32_t)e.f[kGroup], &ok) : NULL;
        const int idx = e.f[kGroup + 1];
        // In a car too: the game keeps a passenger in its group (an instant gang in its leader's car); left out, the
        // rebuild below took it out of the group here
        if (leader && leader != p && idx != 99 && !p->isDead() && !leader->isDead())
        {
            byLeader[leader].push_back(std::make_pair(idx, p));
            placed.insert(p);
        }
        else if (p->mPedGroup && p->mPedGroup->mPedLeader != p && !p->mCar)
        {
            p->mPedGroup->RemovePed(p); // in no group there (or its leader isn't here)
        }
    }
    for (auto& g : byLeader)
    {
        Ped* leader = g.first;
        std::vector<std::pair<int, Ped*>>& members = g.second;
        std::sort(members.begin(), members.end());
        PedGroup* group = leader->mPedGroup;
        if (!group)
        {
            group = PedGroup::New(); // as the game makes one (SpawnPedChainGroupAt)
            if (!group)
            {
                continue;
            }
            group->add_ped_leader(leader);
        }
        // Out of any other group first, then the list as it should be
        for (size_t m = 0; m < members.size(); m++)
        {
            PedGroup* other = members[m].second->mPedGroup;
            if (other && other != group && other->mPedLeader != members[m].second)
            {
                other->RemovePed(members[m].second);
            }
        }
        std::set<Ped*> now;
        for (size_t m = 0; m < members.size(); m++)
        {
            now.insert(members[m].second);
        }
        for (int m = 0; m < 9; m++)
        {
            Ped* was = group->mPedList[m];
            if (was && !now.count(was) && was->mPedGroup == group)
            {
                was->reset_ped_group(); // listed here before, not any more
            }
            group->mPedList[m] = NULL;
        }
        const size_t n = members.size() < 9 ? members.size() : 9;
        for (size_t m = 0; m < n; m++)
        {
            group->add_ped_to_list(members[m].second, (u8)m);
        }
        group->mCount34 = (u8)n;
        group->mCount36 = (char_type)n;
    }
    // A member whose leader isn't here is in no group here: its objective to follow the group goes too (the server's,
    // copied with the AI state; Ped::UpdateGroupMemberFollow assumes a group)
    for (Ped* p = gPedPool->mPool.mpPrev; p; p = p->mpNext) // every ped: one taken out above too
    {
        if (!p->mPedGroup &&
            (p->mObjective == objectives_enum::left_behind_by_leader_8 || p->mInternalObjective == objectives_enum::left_behind_by_leader_8))
        {
            p->mObjective = objectives_enum::no_obj_0;
            p->mInternalObjective = objectives_enum::no_obj_0;
        }
    }
}


namespace
{
// The server's ped on its way into or out of a car and this game's settled: started here the way the game starts it
// (as a player's, players.cpp's StartTransition), else it went on with what it did before until both had settled
// (a cop at a police car's door on the server walked off here). Into the server's target car, with its seat and
// door; out of the car both have it in
bool StartCarTransition(Ped* p, const net::EntityState& e)
{
    using namespace net::engine;
    if (p->mPlayer || !e.f[kPedState] || InCarTransition(p) || !p->mGameObject == !p->mCar)
    {
        return false; // a player's (players.cpp), the server's settled, ours on its way already, or neither on foot nor in a car
    }
    const int32_t objective = e.f[kFirst + kObjective];
    if (!p->mCar && !e.f[kPedCar] && objective == objectives_enum::enter_car_as_driver_35)
    {
        bool ok;
        Car* car = LocalCar((uint32_t)e.f[kCarRefs + kRefObjectiveCar], &ok);
        if (!car || !car->mCarSprite)
        {
            return false; // not here (yet)
        }
        StartEntering(p, car);
        p->set_enter_car_as_passenger((char_type)e.f[kFirst + kPassenger]);
        p->set_target_car_door((char_type)e.f[kFirst + kDoor]);
        return true;
    }
    const bool leaving = objective == objectives_enum::leave_car_36 || e.f[kStates + kState1] == ped_state_1::exiting_car_4 ||
                         e.f[kStates + kState2] == ped_state_2::ped2_getting_out_a_car_7; // as LeavingCar (car_entry.cpp)
    if (p->mCar && RefCar(e.f[kPedCar]) == p->mCar && leaving && p->mCar->CanExitCar())
    {
        StartLeaving(p);
        return true;
    }
    return false;
}
} // namespace

int net::engine::gPedsRevived;

bool net::engine::PedDeadThere(const EntityState& e)
{
    return e.f[kStates + kState1] == ped_state_1::dead_9;
}

bool net::engine::ApplyPedState(Ped* p, const EntityState& e)
{
    // Knocked down, a ped the server killed dies as it gets up (Ped::Kill stored its dead_9): thrown, it is carried out
    // here at once to die at its landing; lying or sinking, the server's death comes then
    const bool dyingThere = e.f[kStates + kState1] == ped_state_1::immobilized_8 && e.f[kStates + kStored1] == ped_state_1::dead_9;
    const bool deadThere = e.f[kStates + kState1] == ped_state_1::dead_9 || (dyingThere && (e.f[kEnd + kPhase] & 7) == kPhaseThrown);
    const bool deadHere = p->isDead();
    const int shownAge = ShownDeathAge(p);
    if (deadThere && (!deadHere || shownAge >= 0))
    {
        ApplyPhases(p, e, kEnd, true, NULL); // a throw on to its landing, or over
        if (!ThrowOf(p) || p->mStoredPedState1 != ped_state_1::dead_9)
        {
            if (p->mCar && !p->mEnterCarAsPassenger)
            {
                ForceOutOfCar(p, Raw(e.f[kPedX]), Raw(e.f[kPedY]), Raw(e.f[kPedZ]), Angle(e.f[kPedRot]));
            }
            KillNow(p); // the server's death, the game's own way (as a player's, snapshot.cpp)
        }
        if (ThrowOf(p))
        {
            p->mStoredPedState2 = e.f[kStates + kStored2]; // its landing restores it: thrown while sinking, the server keeps sinking_20
        }
        return true;
    }
    if (deadHere && !deadThere)
    {
        if (dyingThere)
        {
            return true; // the server's death to come, carried out a tick early here
        }
        if (shownAge >= 0 && shownAge < kShownDeathTicks)
        {
            return true; // shown dead here (ShowDeath), the server's may yet come
        }
        ForgetShownDeath(p);
        if (!p->mGameObject || !p->IsActive())
        {
            return false; // gone here: the ped is made again
        }
        // Alive there (revived by the paramedics, AmbulanceService, or a death of this game's own): revived here as the
        // paramedics do it, then the server's state below
        p->mPedState1 = ped_state_1::walking_0;
        p->mPedState2 = ped_state_2::ped2_walking_0;
        p->mHealth = (s16)(e.f[kHealth] > 0 ? e.f[kHealth] : 1);
        gPedsRevived++;
    }
    else if (deadHere)
    {
        return true; // a corpse on both sides
    }
    ApplyWeapons(p, e);
    ApplyFire(p, (e.f[kFlags] & kOnFire) != 0);

    // Sitting in the same car on both sides (a driver), its AI goes too, but not its states (in and out of cars are
    // replica.cpp's): a chasing driver followed its own objectives here
    const bool driving = p->mCar && e.f[kPedCar] && !e.f[kPedState] && !InCarTransition(p) &&
                         RefCar(e.f[kPedCar]) == p->mCar &&
                         p->mPedState1 == ped_state_1::in_car_10 &&
                         p->mCar->mAI && // its objectives drive through the car's AI (ApplyCarAi gives it one)
                         e.f[kStates + kState1] == ped_state_1::in_car_10 && // driving there too, not on its way out
                         e.f[kFirst + kObjective] != objectives_enum::pull_driver_out_32; // an on-foot one
    if (StartCarTransition(p, e))
    {
        return true;
    }
    // On its way to a car here only, still walking up to it, while the server's ped is on foot with something else to do
    // (an ambulance's crew sent back to a patient: the service that changes their objectives runs on the server only):
    // called off the game's way, and the server's state taken below
    const int32_t objectiveThere = e.f[kFirst + kObjective];
    if (!p->mCar && p->mObjective == objectives_enum::enter_car_as_driver_35 && !AtCarDoor(p) && !e.f[kPedCar] && !e.f[kPedState] &&
        objectiveThere != objectives_enum::enter_car_as_driver_35 && objectiveThere != objectives_enum::enter_train_37)
    {
        p->CancelEnterCarObjective();
    }
    // On foot on both sides: its phase as the server's, whether it goes to a car or not (thrown on its way, it goes on
    // after its landing)
    if (!p->mCar && !e.f[kPedCar] && p->mGameObject)
    {
        ApplyPhases(p, e, kEnd, false, NULL);
    }
    if (!driving && (p->mCar || InCarTransition(p) || e.f[kPedCar] || e.f[kPedState]))
    {
        // In a car, or on its way into or out of one, on either side: that is replica.cpp's (ForceIntoCar, ForceOutOfCar
        // when both have settled). The AI's objectives and targets then belong to the entering and leaving: set from the
        // server's, the char's walk to a door read a car the client had no part in (Char::UpdateImmobilized)
        return true;
    }
    // Then its states and its char's movement and animation, whatever they are, in the same phase: one this game couldn't
    // take (a thrown ped getting up here) or a state of no phase on one side only (a fall) is left for a later snapshot
    if (p->mGameObject)
    {
        const int32_t there = e.f[kEnd + kPhase] & 7;
        // Jacking a driver (Ped::PullDriverOutOfCarStateMachine): its char reads the victim's car, unchecked
        Car* jacked = NULL;
        if (e.f[kChar + kCharAnim] == Char_Anim_state::Carjacking_9)
        {
            bool ok;
            Ped* victim = LocalPed((uint32_t)e.f[kPedRefs + kRefInternalPed], &ok);
            jacked = victim && victim->mCar && victim->mCar->mCarSprite ? victim->mCar : NULL;
        }
        if (PhaseOf(p) == there && (jacked || e.f[kChar + kCharAnim] != Char_Anim_state::Carjacking_9) &&
            (there || (e.f[kStates + kState1] != ped_state_1::immobilized_8 && p->mPedState1 != ped_state_1::immobilized_8)))
        {
            if (jacked)
            {
                p->mGameObject->mTargetCar = jacked;
                p->mTargetCarDoor = (u8)e.f[kFirst + kDoor];
            }
            p->mPedState1 = e.f[kStates + kState1];
            p->mPedState2 = e.f[kStates + kState2];
            if (e.f[kStates + kStored1] != ped_state_1::dead_9)
            {
                p->mStoredPedState1 = e.f[kStates + kStored1]; // a death to come is the server's to carry out (KillNow)
            }
            p->mStoredPedState2 = e.f[kStates + kStored2];
            GuardThrow(p);
            ApplyMembers(p->mGameObject, kCharValues, kCharCount, e, kChar);
        }
    }
    // The references first: one its objective acts on that this game doesn't have (out of its area) keeps the ped as it
    // is here for this snapshot, whole. Its AI runs by its occupation as much as by its objective (a robbed driver reads
    // its car thief: Ped::RobbedDriver_AI), so a part of the server's state with a target missing isn't safe. The others
    // (who attacked, punched, was hit or killed it, the car it stole) the game rarely clears and often name what is gone
    // or far: one this game doesn't have stays as it is here, the rest of the state goes
    static const bool kPedRefBlocks[kPedRefCount] = {false, true, true, true, false, false, false};
    static const bool kCarRefBlocks[kCarRefCount] = {false, true, true, true};
    bool ok;
    Ped* pedRefs[kPedRefCount];
    bool pedKept[kPedRefCount];
    for (int i = 0; i < kPedRefCount; i++)
    {
        pedRefs[i] = LocalPed((uint32_t)e.f[kPedRefs + i], &ok);
        pedKept[i] = !ok;
        if (!ok && kPedRefBlocks[i])
        {
            return true;
        }
    }
    Car* carRefs[kCarRefCount];
    bool carKept[kCarRefCount];
    for (int i = 0; i < kCarRefCount; i++)
    {
        carRefs[i] = LocalCar((uint32_t)e.f[kCarRefs + i], &ok);
        carKept[i] = !ok;
        if (!ok && kCarRefBlocks[i])
        {
            return true;
        }
    }
    if (!ObjectiveTargetsThere(p, e) ||
        (RobbedDriver(e.f[kFirst + kOccupation]) && (!(pedKept[kRefCarThief] ? p->mCarThief : pedRefs[kRefCarThief]) ||
                                                     !(carKept[kRefStolenCar] ? p->mStolenCar : carRefs[kRefStolenCar]))))
    {
        return true;
    }
    ApplyMembers(p, kValues, kValueCount, e, kFirst);
    u32 flags;
    memcpy(&flags, &p->mBf21C, sizeof(flags));
    flags = (flags & ~kFlagsSent) | ((u32)e.f[kFlags] & kFlagsSent);
    memcpy(&p->mBf21C, &flags, sizeof(flags));
    // Ped::PoolUpdate electrocutes a ped whose shock counter is at its threshold (the game's are 100), and lays a busted
    // one down, both through its char: none in a car (this apply may still seat it), and a network game busts nobody
    p->mElectrocutionThreshold = p->mElectrocutionThreshold ? p->mElectrocutionThreshold : 100;
    if ((!p->mGameObject || p->mCar || e.f[kPedCar]) && p->mShockCounter >= p->mElectrocutionThreshold)
    {
        p->mShockCounter = 0;
    }
    p->mBf21C.bBusted = false;
    p->mHealth = (s16)(e.f[kHealth] > 0 ? e.f[kHealth] : 1); // a death comes from the server's state, whole
    Ped** pedMembers[kPedRefCount] = {&p->mAttacker,         &p->mObjectiveTargetPed, &p->mInternalTargetPed,
                                      &p->mCarThief,        &p->mLastCharPunched,    &p->mHitTargetPed,
                                      &p->mPedKiller};
    for (int i = 0; i < kPedRefCount; i++)
    {
        if (!pedKept[i])
        {
            *pedMembers[i] = pedRefs[i];
        }
    }
    Car** carMembers[kCarRefCount] = {&p->mStolenCar, &p->mTargetObjectiveCar, &p->mTargetToEnter,
                                      &p->mEscapeCar};
    for (int i = 0; i < kCarRefCount; i++)
    {
        if (i == kRefTargetToEnter && p->mCar)
        {
            continue; // in a car: what it got into is its car, and leaving it reads this (Ped::ExitCarStateMachine)
        }
        if (!carKept[i])
        {
            *carMembers[i] = carRefs[i];
        }
    }
    p->mpGang = GangAt(e.f[kGangs]);
    p->mDummyGang = GangAt(e.f[kGangs + 1]);
    return true;
}

void net::engine::SetPedKind(Ped* p, const EntityState& e)
{
    p->SetPedType(e.f[kFirst + kType]);
    // A robbed driver whose thief or car this game doesn't have (yet)
    int32_t occupation = e.f[kFirst + kOccupation];
    bool ok;
    if (RobbedDriver(occupation) && (!LocalPed((uint32_t)e.f[kPedRefs + kRefCarThief], &ok) || !LocalCar((uint32_t)e.f[kCarRefs + kRefStolenCar], &ok)))
    {
        occupation = ped_ocupation_enum::dummy;
    }
    p->set_occupation(occupation);
}

bool net::engine::PassengerThere(const EntityState& e)
{
    return e.f[kPedCar] && e.f[kFirst + kPassenger];
}

Ped* net::engine::NetIdPed(uint32_t id)
{
    bool resolved;
    return LocalPed(id, &resolved);
}

// On fire as the server has it, the game's own way (flames on its char, a scream): on foot only, burning in a car blows
// the car up (Ped::ManageBurning)
void net::engine::ApplyFire(Ped* p, bool onFire)
{
    if (onFire && !p->mBf21C.bOnFire && p->mGameObject && !p->mCar)
    {
        p->SetOnFire();
    }
    else if (!onFire && p->mBf21C.bOnFire)
    {
        p->PutOutFire();
    }
}
