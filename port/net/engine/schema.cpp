// port/net (port/docs/netcode.md, "Entities"): players, cars, peds, pickups and the world as plain numbers. Positions are
// Fix16 raw values (14 fraction bits): x and y below 256 blocks fit 22 bits, z below 8 blocks 17 bits; angles are
// Ang16 (0..1439), 11 bits. So what a client gets is exactly the server's value.
#include "internal.hpp"
#include "Source/Player.hpp"
#include <deque>
#include <stdio.h>
#include <string>

void net::engine::PhaseRecordFields(std::vector<net::FieldDesc>& out)
{
    static const net::FieldDesc kFields[net::engine::kPhaseFieldCount] = {
        {"phase", 4, false}, {"phase_timer", 16, false}, {"phase_car", 25, false}, {"throw", 5, false}, {"throw_x", 22, false},
        {"throw_y", 22, false}, {"throw_z", 17, false}, {"throw_rot", 11, false}, {"throw_angle", 11, false},
        {"throw_speed", 18, true}, // Fix16 raw: up to 8 blocks a tick either way
        {"throw_zspeed", 18, true}, {"throw_zaccel", 18, true}, {"throw_friction", 18, true}};
    out.insert(out.end(), kFields, kFields + net::engine::kPhaseFieldCount);
}

namespace
{
using net::FieldDesc;

// Fields named <prefix>0 .. <prefix>n-1
void AppendNumbered(std::vector<FieldDesc>& out, const char* prefix, int count, uint8_t bits)
{
    static std::deque<std::string> sNames; // the schema keeps the pointers (a deque doesn't move them)
    for (int i = 0; i < count; i++)
    {
        char name[32];
        snprintf(name, sizeof(name), "%s%d", prefix, i);
        sNames.push_back(name);
        out.push_back(FieldDesc{sNames.back().c_str(), bits, false});
    }
}

net::Schema BuildSchema()
{
    using namespace net::engine;
    net::Schema s;
    net::EntityType player = {"player", {{"x", 22, false}, {"y", 22, false}, {"z", 17, false}, {"rot", 11, false}, {"in_car", 1, false}, {"buttons", 22, false},
                                         {"health", 10, true}, {"deaths", 8, false}, {"killer", 6, false}, {"weapon", 5, false},
                                         {"frags", 12, true}, {"points", 32, true}}};
    AppendNumbered(player.fields, "ammo", kPlayerWeapons, 16);
    player.fields.push_back(FieldDesc{"car", 25, false}); // kPlayerCar
    player.fields.push_back(FieldDesc{"state", 6, false}); // kPlayerState
    player.fields.push_back(FieldDesc{"target", 25, false}); // kPlayerTarget
    AppendNumbered(player.fields, "name", kPlayerNameEnd - kPlayerName, 32);
    AppendNumbered(player.fields, "power", kPowerUps, 16);
    player.fields.push_back(FieldDesc{"chatting", 1, false});
    AppendNumbered(player.fields, "chat", kPlayerChatEnd - kPlayerChat, 32);
    player.fields.push_back(FieldDesc{"wanted", 16, true}); // kPlayerWanted
    player.fields.push_back(FieldDesc{"money", 32, true}); // kPlayerMoney
    player.fields.push_back(FieldDesc{"multiplier", 8, false}); // kPlayerMultiplier
    player.fields.push_back(FieldDesc{"death_events", 8, false}); // kPlayerDeathEvents
    player.fields.push_back(FieldDesc{"bonus", 12, false}); // kPlayerBonus
    player.fields.push_back(FieldDesc{"invulnerable", 16, false}); // kPlayerInvulnerable
    PhaseRecordFields(player.fields); // kPlayerPhase
    net::EntityType car = {"car", {{"x", 22, false}, {"y", 22, false}, {"z", 17, false}, {"rot", 11, false}, {"model", 8, false},
                                   {"driver", 2, false}, {"damage", 16, false}, {"areas", 32, false}, {"wrecked", 2, false},
                                   {"lights", 8, false}, {"flash", 8, false}, {"horn", 8, false}, {"horn_cooldown", 8, false}, {"remap", 16, true},
                                   {"turret", 11, false}, {"trailer", 25, false},
                                   {"flags", 16, false}}};
    CarPhysicsFields(car.fields);
    CarAiFields(car.fields);
    net::EntityType ped = {"ped", {{"x", 22, false}, {"y", 22, false}, {"z", 17, false}, {"rot", 11, false}, {"remap", 8, false},
                                   {"car", 25, false}, {"state", 1, false}}};
    PedStateFields(ped.fields);
    PhaseRecordFields(ped.fields); // kPedPhase
    // The last field of cars, peds and pickups: how many ticks old its state is (0: this snapshot's; a busy area's far
    // entities get theirs less often, AgeField)
    car.fields.push_back(FieldDesc{"age", 6, false});
    ped.fields.push_back(FieldDesc{"age", 6, false});
    net::EntityType world = {"world", {{"cycle", 32, false}, {"rand", 32, false}, {"reach", 10, false}, {"it", (uint8_t)net::BitsFor(MAX_PLAYERS), false}}};
    AppendNumbered(world.fields, "it_time", MAX_PLAYERS, 16);
    world.fields.push_back(FieldDesc{"zone_turn", 16, true});
    world.fields.push_back(FieldDesc{"round", 16, false});
    world.fields.push_back(FieldDesc{"winner", (uint8_t)net::BitsFor(MAX_PLAYERS), false});
    world.fields.push_back(FieldDesc{"seconds_left", 13, false});
    world.fields.push_back(FieldDesc{"game", 16, false});
    world.fields.push_back(FieldDesc{"seed", 32, false}); // kWorldSeed
    world.fields.push_back(FieldDesc{"lights", 16, false}); // kWorldLights: phase 4 bits, timer 8, the cars' light state 4
    // The round flow (core/round_state.hpp): kWorldPhase ..
    world.fields.push_back(FieldDesc{"phase", 2, false});
    world.fields.push_back(FieldDesc{"phase_left", 10, false});
    world.fields.push_back(FieldDesc{"min_players", (uint8_t)net::BitsFor(net::kRoundSlots), false});
    world.fields.push_back(FieldDesc{"ready", net::kRoundSlots, false});
    world.fields.push_back(FieldDesc{"vote", 2, false});
    world.fields.push_back(FieldDesc{"vote_arg", 8, false});
    world.fields.push_back(FieldDesc{"vote_yes", net::kRoundSlots, false});
    world.fields.push_back(FieldDesc{"vote_no", net::kRoundSlots, false});
    world.fields.push_back(FieldDesc{"vote_left", 7, false});
    AppendNumbered(world.fields, "map_choice", net::kRoundSlots, 4);
    world.fields.push_back(FieldDesc{"ambient", 32, true}); // kWorldAmbient
    world.fields.push_back(FieldDesc{"target_ambient", 32, true});
    world.fields.push_back(FieldDesc{"ambient_step", 32, true});
    world.fields.push_back(FieldDesc{"police_level", 3, false}); // kWorldPoliceLevel ..
    world.fields.push_back(FieldDesc{"police_crew_type", 3, false});
    world.fields.push_back(FieldDesc{"police_crew_onscreen", 1, false});
    world.fields.push_back(FieldDesc{"police_firing", 32, false});
    AppendNumbered(world.fields, "pursuit_criminal", 4, 32); // kWorldPursuitCriminal ..
    AppendNumbered(world.fields, "pursuit_level", 4, 3);
    AppendNumbered(world.fields, "pursuit_state", 4, 3);
    AppendNumbered(world.fields, "pursuit_timer", 4, 16);
    AppendNumbered(world.fields, "pursuit_crews", 4, 3);
    AppendNumbered(world.fields, "pursuit_chased", 4, 1);
    AppendNumbered(world.fields, "lead", MAX_PLAYERS, 5); // kWorldLead
    net::EntityType object = {"object", {{"x", 22, false}, {"y", 22, false}, {"z", 17, false}, {"rot", 11, false}, {"model", 9, false}, {"idx", 8, false},
                                         {"age", 6, false}}};
    net::EntityType train = {"train", {{"x", 22, false}, {"y", 22, false}, {"z", 17, false}, {"rot", 11, false}, {"wagons", 4, false},
                                       {"reversed", 1, false}, {"state", 3, false}, {"stop_phase", 4, false}, {"wait", 16, true},
                                       {"hijacked", 1, false}, {"fleeing", 1, false}, {"passenger_timer", 8, true},
                                       {"passengers", 8, true}, {"speed", 32, true}, {"driver", 4, false}, {"station", 7, false}, {"platform", 3, false},
                                       {"stopped", 4, false}}};
    AppendNumbered(train.fields, "carriage", kTrainCarriages, 25);
    AppendNumbered(train.fields, "damage", kTrainCarriages, 16);
    AppendNumbered(train.fields, "explosion", kTrainCarriages, 8);
    // Fixed at its first tick: both ends simulate it on from there (shots.cpp)
    // (z 18 bits: thrown from a roof's top, a grenade starts above 8 blocks)
    net::EntityType shot = {"shot", {{"x", 22, false}, {"y", 22, false}, {"z", 18, false}, {"rot", 11, false}, {"model", 9, false},
                                     {"kind", 6, false}, {"timer", 16, false}, {"owner", 32, false}, {"born", 16, false}}};
    ShotMotionFields(shot.fields);
    s.types.push_back(player); // kEntityPlayer
    s.types.push_back(car); // kEntityCar
    s.types.push_back(ped); // kEntityPed
    s.types.push_back(world); // kEntityWorld
    s.types.push_back(object); // kEntityObject
    s.types.push_back(train); // kEntityTrain
    s.types.push_back(shot); // kEntityShot
    return s;
}
} // namespace

const net::Schema& net::engine::NetSchema()
{
    static const Schema s = BuildSchema();
    return s;
}

int net::engine::AgeField(uint8_t type)
{
    if (type != kEntityCar && type != kEntityPed && type != kEntityObject)
    {
        return -1;
    }
    return (int)NetSchema().types[type].fields.size() - 1;
}

int net::engine::EntityAge(const EntityState& e)
{
    const int f = AgeField(e.type);
    return f < 0 ? 0 : e.f[f];
}

const net::EntityState* net::engine::FindWorld(const Snapshot& snap)
{
    // Sorted by id, the world's (0) comes first
    for (const EntityState& e : snap.entities)
    {
        if (e.type == kEntityWorld)
        {
            return &e;
        }
    }
    return NULL;
}

float net::engine::SnapshotReach(const Snapshot& snap)
{
    const EntityState* w = FindWorld(snap);
    return w ? w->f[kWorldReach] / 16.0f : 0;
}

void net::engine::SetSnapshotReach(Snapshot& snap, float blocks)
{
    if (EntityState* w = const_cast<EntityState*>(FindWorld(snap)))
    {
        const int v = (int)(blocks * 16);
        w->f[kWorldReach] = v < 0 ? 0 : v > 1023 ? 1023 : v;
    }
}
