// port/net (port/docs/netcode.md, "Security"): a snapshot from the server made safe for the game before anything of it
// is applied. Every field the game uses as an index, a table's entry, an angle into its sine table, a position it looks
// up in the map or a count of a fixed array is put into its valid range; a real server's values are always in it, so
// a field changed here is printed as a CHECK line. The rest of the checks need the ped or the car they go to and are
// where they are applied (replica.cpp, ped_state.cpp, cars.cpp, pickups.cpp, trains.cpp).
#include "internal.hpp"
#include "Source/Object_5C.hpp"
#include "Source/ObjectDefinitions.hpp"
#include "Source/enums.hpp"
#include "Source/gtx_0x106C.hpp"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <vector>

namespace
{
enum Kind
{
    kRange, // lo..hi
    kAngle, // an Ang16: 0..1439, else taken modulo 1440
    kMask, // the bits in lo only
    kZeroOrRange, // 0 (none), else lo..hi
    kPhysicsRange, // lo..hi in a car with physics (all its physics fields 0 without: CarHasPhysics)
};

struct Rule
{
    uint8_t type;
    const char* field; // its name in the schema (schema.cpp, and the member tables' member names)
    Kind kind;
    int32_t lo, hi;
};

// A Fix16 x or y on the map, a block off its edges: the map is looked up around what moves, unchecked (Map_0x370::GetBlock)
const int32_t kMapMin = 1 << 14, kMapMax = 255 << 14;
const int32_t kCarSpeedMax = 1 << 14; // a block a tick: four times the fastest car, more than twice a train
// Half a block a tick: above a train's fastest (kTrainSpeedState0, 0.3 a tick), never below 0 (CarAI::RaiseSpeedTo)
const int32_t kTrainSpeedMax = 1 << 13;
const int32_t kSpeedMax = 16 << 14; // 16 blocks a tick: far above any speed the game gives a ped (4)
// A car's z speed: a step's rise or fall over its time step, which a collision's search cuts to an eighth or less
// (CarPhysics::UpdateZPhysics); 2 blocks a tick measured
const int32_t kCarZSpeedMax = 16 << 14;

using namespace net::engine;
const Rule kRules[] = {
    // Where each thing is (the game's own entities, a block off the map's edges)
    {kEntityPlayer, "x", kRange, kMapMin, kMapMax},
    {kEntityPlayer, "y", kRange, kMapMin, kMapMax},
    {kEntityCar, "x", kRange, kMapMin, kMapMax},
    {kEntityCar, "y", kRange, kMapMin, kMapMax},
    {kEntityPed, "x", kRange, kMapMin, kMapMax},
    {kEntityPed, "y", kRange, kMapMin, kMapMax},
    {kEntityObject, "x", kRange, kMapMin, kMapMax},
    {kEntityObject, "y", kRange, kMapMin, kMapMax},
    {kEntityTrain, "x", kRange, kMapMin, kMapMax},
    {kEntityTrain, "y", kRange, kMapMin, kMapMax},
    // The input's key event: a DirectInput code of 0..255 (KeyboardLayout::mKeys[256], read by the chat), its press bit
    {kEntityPlayer, "buttons", kMask, 0x2FFFFF, 0},
    {kEntityPlayer, "money", kRange, 0, 999999999}, // the HUD's "%09d" into its 10 bytes (RollingDigitCounter)
    {kEntityPlayer, "frags", kRange, -999, 999},
    {kEntityPlayer, "multiplier", kRange, 0, 99},
    {kEntityWorld, "zone_turn", kRange, -1, 39}, // Map_0x370::GetNextZoneOfTypeRoundRobin's zone_stack[40]
    {kEntityWorld, "police_level", kRange, 0, 6},
    {kEntityWorld, "police_crew_type", kRange, 0, 6},
    {kEntityWorld, "pursuit_level0", kRange, 0, 6},
    {kEntityWorld, "pursuit_level1", kRange, 0, 6},
    {kEntityWorld, "pursuit_level2", kRange, 0, 6},
    {kEntityWorld, "pursuit_level3", kRange, 0, 6},
    {kEntityWorld, "pursuit_crews0", kRange, 0, 6}, // Police_38::mCrews[6]
    {kEntityWorld, "pursuit_crews1", kRange, 0, 6},
    {kEntityWorld, "pursuit_crews2", kRange, 0, 6},
    {kEntityWorld, "pursuit_crews3", kRange, 0, 6},
    {kEntityCar, "turret", kAngle, 0, 0},
    // Its physics: the map is looked up at its place, the sine table at its heading, a skid made where its wheels were
    {kEntityCar, "mTheta", kAngle, 0, 0},
    {kEntityCar, "mCm1.x", kPhysicsRange, kMapMin, kMapMax},
    {kEntityCar, "mCm1.y", kPhysicsRange, kMapMin, kMapMax},
    {kEntityCar, "mCp1.x", kPhysicsRange, kMapMin, kMapMax},
    {kEntityCar, "mCp1.y", kPhysicsRange, kMapMin, kMapMax},
    {kEntityCar, "mCp3", kRange, 0, (8 << 14) - 1},
    {kEntityCar, "mVelReadOnly.x", kRange, -kCarSpeedMax, kCarSpeedMax},
    {kEntityCar, "mVelReadOnly.y", kRange, -kCarSpeedMax, kCarSpeedMax},
    {kEntityCar, "mLinvel1.x", kRange, -kCarSpeedMax, kCarSpeedMax},
    {kEntityCar, "mLinvel1.y", kRange, -kCarSpeedMax, kCarSpeedMax},
    {kEntityCar, "mForceAccum.x", kRange, -4 * kCarSpeedMax, 4 * kCarSpeedMax},
    {kEntityCar, "mForceAccum.y", kRange, -4 * kCarSpeedMax, 4 * kCarSpeedMax},
    {kEntityCar, "mLinearAccel.x", kRange, -4 * kCarSpeedMax, 4 * kCarSpeedMax},
    {kEntityCar, "mLinearAccel.y", kRange, -4 * kCarSpeedMax, 4 * kCarSpeedMax},
    {kEntityCar, "mZVel", kRange, -kCarZSpeedMax, kCarZSpeedMax},
    {kEntityCar, "mAirZVel", kRange, -kCarZSpeedMax, kCarZSpeedMax},
    {kEntityCar, "mLastSkidPos[0].x", kZeroOrRange, kMapMin, kMapMax},
    {kEntityCar, "mLastSkidPos[0].y", kZeroOrRange, kMapMin, kMapMax},
    {kEntityCar, "mLastSkidPos[1].x", kZeroOrRange, kMapMin, kMapMax},
    {kEntityCar, "mLastSkidPos[1].y", kZeroOrRange, kMapMin, kMapMax},
    {kEntityCar, "mLastSkidPos[2].x", kZeroOrRange, kMapMin, kMapMax},
    {kEntityCar, "mLastSkidPos[2].y", kZeroOrRange, kMapMin, kMapMax},
    {kEntityCar, "mLastSkidPos[3].x", kZeroOrRange, kMapMin, kMapMax},
    {kEntityCar, "mLastSkidPos[3].y", kZeroOrRange, kMapMin, kMapMax},
    {kEntityCar, "mState", kRange, 0, 4},
    {kEntityCar, "mOilSpinDir", kRange, 0, 2},
    {kEntityCar, "mIsCoasting", kRange, 0, 1},
    // Its AI: RouteFinder::mRoutes[50] and a route's mJunctions[256] (and the one after)
    {kEntityCar, "mRouteIdx", kRange, -1, 49},
    {kEntityCar, "mRoutePos", kRange, 0, 254},
    {kEntityCar, "mAngle", kAngle, 0, 0},
    {kEntityCar, "mCurrDirection", kRange, 0, 4},
    // A ped: its angles, its speeds (the char moves by them, the map is looked up where it gets to), the door of
    // Car::mDoors[4] it goes to, its weapons (weapon_type + 1: hand weapons only, a car's need the car), its sprite
    {kEntityPed, "mFacingAngle", kAngle, 0, 0},
    {kEntityPed, "mAimAngle", kAngle, 0, 0},
    {kEntityPed, "mTargetFacingAngle", kAngle, 0, 0},
    {kEntityPed, "mFollowCarOffsetAngle", kAngle, 0, 0},
    {kEntityPed, "mRotation", kAngle, 0, 0},
    {kEntityPed, "mMaxSpeed", kRange, -kSpeedMax, kSpeedMax},
    {kEntityPed, "mWalkSpeed", kRange, -kSpeedMax, kSpeedMax},
    {kEntityPed, "mRunSpeed", kRange, -kSpeedMax, kSpeedMax},
    {kEntityPed, "mEnterCarAsPassenger", kRange, 0, 1},
    {kEntityPed, "mTargetCarDoor", kRange, 0, 3},
    {kEntityPed, "weapon", kRange, 0, weapon_type::weapon_14 + 1},
    {kEntityPed, "weapon2", kRange, 0, weapon_type::weapon_14 + 1},
    {kEntityPed, "mTargetRotation", kAngle, 0, 0},
    {kEntityPed, "mRotationJitter", kAngle, 0, 0},
    {kEntityPed, "mRepulsionRotation", kAngle, 0, 0},
    {kEntityPed, "mVelocity", kRange, -kSpeedMax, kSpeedMax},
    {kEntityPed, "mRunOrJumpSpeed", kRange, -kSpeedMax, kSpeedMax},
    {kEntityPed, "mVelocityVector.x", kRange, -kSpeedMax, kSpeedMax},
    {kEntityPed, "mVelocityVector.y", kRange, -kSpeedMax, kSpeedMax},
    {kEntityPed, "mAnimationFrame", kRange, 0, 31}, // added to the animation's first sprite: within a ped's 158
    {kEntityTrain, "speed", kRange, 0, kTrainSpeedMax},
    {kEntityTrain, "state", kRange, 0, 5},
    {kEntityTrain, "stop_phase", kRange, 0, 11},
};

struct Resolved
{
    Rule rule;
    int field;
};

int FieldOf(uint8_t type, const char* name)
{
    const std::vector<net::FieldDesc>& fields = NetSchema().types[type].fields;
    for (size_t i = 0; i < fields.size(); i++)
    {
        if (!strcmp(fields[i].name, name))
        {
            return (int)i;
        }
    }
    return -1;
}

const std::vector<Resolved>& Rules()
{
    static std::vector<Resolved> sRules;
    if (sRules.empty())
    {
        for (const Rule& r : kRules)
        {
            const int f = FieldOf(r.type, r.field);
            if (f < 0)
            {
                fprintf(stderr, "net: the snapshot check names a field the schema doesn't have: %s\n", r.field);
                abort(); // a member renamed: the rule must follow it
            }
            sRules.push_back(Resolved{r, f});
        }
    }
    return sRules;
}

int gClamped;

void Report(const net::EntityState& e, const char* field, int32_t was, int32_t now)
{
    if (gClamped++ < 20)
    {
        printf("net: CHECK snapshot field %s.%s of %u: %d outside its range, now %d\n", NetSchema().types[e.type].name, field, e.id, was, now);
    }
}

void Set(net::EntityState& e, int field, int32_t v)
{
    if (e.f[field] != v)
    {
        Report(e, NetSchema().types[e.type].fields[field].name, e.f[field], v);
        e.f[field] = v;
    }
}

const ObjectDefinition* Definition(int model)
{
    return model >= 0 && model < ObjectDefinitions::k_max_definitions && gObjectDefinitions ? gObjectDefinitions->mDefinitionByIdx[model] : NULL;
}

bool ObjectIsPickup(int model)
{
    const ObjectDefinition* d = Definition(model);
    return d && (d->mBehaviorType == object_behavior_type::pickup_6 || d->mBehaviorType == object_behavior_type::pickup_physics_7 ||
                 d->mBehaviorType == object_behavior_type::pickup_animated_8 || d->mBehaviorType == object_behavior_type::pickup_physics_animated_9);
}

// What the server sends (pickups.cpp's Usable): a pickup, a roadblock's barrier, a car's mine or oil
bool ObjectModelKnown(int model)
{
    return Definition(model) && (ObjectIsPickup(model) || model == objects::roadblock_21 || model == objects::mine_10 ||
                                 model == objects::animating_oil_8 || model == objects::oil_9);
}
} // namespace

void net::engine::SanitizeSnapshot(Snapshot& snap)
{
    static const int kCarRemapField = FieldOf(kEntityCar, "remap"), kPedRemapField = FieldOf(kEntityPed, "remap"),
                     kPedTypeField = FieldOf(kEntityPed, "mPedType"), kGangCarField = FieldOf(kEntityPed, "mGangCarModel"),
                     kOccupationField = FieldOf(kEntityPed, "mOccupation"), kCarThiefField = FieldOf(kEntityPed, "car_thief"),
                     kStolenCarField = FieldOf(kEntityPed, "stolen_car");
    const int carRemaps = gGtx_0x106C ? gGtx_0x106C->GetPaletteBaseOfType(palette_types_enum::car_remaps_3) : 0;
    const int pedRemaps = gGtx_0x106C ? gGtx_0x106C->GetPaletteBaseOfType(palette_types_enum::ped_remaps_4) : 0;
    for (EntityState& e : snap.entities)
    {
        for (const Resolved& r : Rules())
        {
            if (r.rule.type != e.type)
            {
                continue;
            }
            const int32_t v = e.f[r.field];
            switch (r.rule.kind)
            {
                case kRange:
                    Set(e, r.field, v < r.rule.lo ? r.rule.lo : v > r.rule.hi ? r.rule.hi : v);
                    break;
                case kAngle:
                    Set(e, r.field, ((v % 1440) + 1440) % 1440);
                    break;
                case kMask:
                    Set(e, r.field, v & r.rule.lo);
                    break;
                case kPhysicsRange:
                    if (CarHasPhysics(e))
                    {
                        Set(e, r.field, v < r.rule.lo ? r.rule.lo : v > r.rule.hi ? r.rule.hi : v);
                    }
                    break;
                case kZeroOrRange:
                    Set(e, r.field, !v ? 0 : v < r.rule.lo ? r.rule.lo : v > r.rule.hi ? r.rule.hi : v);
                    break;
            }
        }
        // Remaps index the texture cache's remapped sprites: the style's count of them, or none (-1, a ped's 0xFF)
        if (e.type == kEntityCar && (e.f[kCarRemapField] < -1 || e.f[kCarRemapField] >= carRemaps))
        {
            Set(e, kCarRemapField, -1);
        }
        if (e.type == kEntityPed && e.f[kPedRemapField] != 0xFF && e.f[kPedRemapField] >= pedRemaps)
        {
            Set(e, kPedRemapField, 0xFF);
        }
        if (e.type == kEntityPed)
        {
            // A player's ped is a player entity: a ped of player type here has no Player, which the game reads
            const int32_t t = e.f[kPedTypeField];
            if (t < ped_type::none_0 || t > ped_type::dummy_with_occupation_6 || t == ped_type::player_2)
            {
                Set(e, kPedTypeField, ped_type::dummy_3);
            }
            // A robbed driver goes after its thief and its car (Ped::RobbedDriver_AI reads them unchecked); a player's
            // occupations (player, empty: Ped::IsPedAThreat reads its Player) and the traffic's shared spare driver's are
            // never a ped entity's
            const int32_t o = e.f[kOccupationField];
            const bool robbed = o == ped_ocupation_enum::robbed_driver_10 || o == ped_ocupation_enum::angry_armed_robbed_driver_12 ||
                                o == ped_ocupation_enum::very_angry_armed_robbed_driver_13;
            if (o == ped_ocupation_enum::player || o == ped_ocupation_enum::empty || o == ped_ocupation_enum::dummy_driver_4 ||
                (robbed && (!e.f[kCarThiefField] || !e.f[kStolenCarField])))
            {
                Set(e, kOccupationField, ped_ocupation_enum::dummy);
            }
            // The car its gang calls: a model of the style's (Ped's own default otherwise)
            const int32_t m = e.f[kGangCarField];
            if (m < 0 || m > 255 || !gGtx_0x106C || !gGtx_0x106C->does_car_exist((u8)m))
            {
                Set(e, kGangCarField, car_model_enum::MERC);
            }
        }
        // The PedRefTable entry (255 of them) of an object that isn't a pickup (Object::UsesParamAsPedRef: a mine, oil,
        // a barrier); an object without a definition isn't made (pickups.cpp)
        if (e.type == kEntityObject && e.f[kObjIdx] > 254 && !ObjectIsPickup(e.f[kObjModel]))
        {
            Set(e, kObjIdx, 254);
        }
    }
}

bool net::engine::ObjectModelUsable(int model)
{
    return ObjectModelKnown(model);
}

int net::engine::SnapshotClamps()
{
    return gClamped;
}
