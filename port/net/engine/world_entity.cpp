// port/net (port/docs/netcode.md, "Entities"): the world entity, one per snapshot: the game's random cycle and seed, the
// restart zones' turn, the round's state (round_state.hpp), the game's generation, the traffic lights, the tag game's
// fugitive, the map's light level and the police's state.
#include "internal.hpp"
#include "port/src/rand/owner.hpp"
#include "port/src/net_hooks.hpp"
#include "port/src/rand/state.hpp"
#include "Source/Car.hpp"
#include "Source/Char_Pool.hpp"
#include "Source/Game.hpp"
#include "Source/Ped.hpp"
#include "Source/Player.hpp"
#include "Source/Police.hpp"
#include "Source/SpriteGrid.hpp"
#include "Source/TrafficLights.hpp"
#include "Source/char.hpp"
#include "Source/network_data.hpp"
#include "Source/rng.hpp"
#include "Source/sprite.hpp"
#include "Source/GameSession.hpp"
#include "Source/MapRenderer.hpp"
#include <math.h>
#include <string.h>
#include <unordered_map>

void net::engine::CaptureWorld(Snapshot& out)
{
    EntityState w = Entity(kWorldId, kEntityWorld);
    w.f[kWorldCycle] = gpRng->get_cur_rng();
    w.f[kWorldZoneTurn] = ZoneTurn();
    w.f[kWorldRound] = CurrentRound().number;
    w.f[kWorldWinner] = CurrentRound().winner;
    w.f[kWorldSecondsLeft] = CurrentRound().secondsLeft;
    const Round& r = CurrentRound();
    w.f[kWorldPhase] = r.phase;
    w.f[kWorldPhaseLeft] = r.phaseLeft < 1023 ? r.phaseLeft : 1023;
    w.f[kWorldMinPlayers] = r.minPlayers;
    w.f[kWorldReady] = r.ready;
    w.f[kWorldVote] = r.voteKind;
    w.f[kWorldVoteArg] = r.voteArg;
    w.f[kWorldVoteYes] = r.voteYes;
    w.f[kWorldVoteNo] = r.voteNo;
    w.f[kWorldVoteLeft] = r.voteLeft;
    for (int i = 0; i < kRoundSlots; i++)
    {
        w.f[kWorldMapChoice + i] = r.mapChoice[i];
    }
    w.f[kWorldGame] = GameGeneration() & 0xFFFF;
    w.f[kWorldRand] = (int32_t)PortRand::State();
    w.f[kWorldSeed] = (int32_t)PortRand::Seed();
    if (gTrafficLights_194 && gCarManager_6C)
    {
        w.f[kWorldLights] = gTrafficLights_194->mPhase | gTrafficLights_194->mTimer << 4 |
                            (gCarManager_6C->mLightStateA & 0xF) << 12;
    }
    if (gpMapRenderer)
    {
        w.f[kWorldAmbient] = gpMapRenderer->mAmbient.mValue;
        w.f[kWorldTargetAmbient] = gpMapRenderer->mTargetAmbient.mValue;
        w.f[kWorldAmbientStep] = gpMapRenderer->mAmbientStep.mValue;
    }
    if (const Police* police = gPolice_7B8)
    {
        w.f[kWorldPoliceLevel] = police->mMaxWantedLevel;
        w.f[kWorldPoliceCrewType] = police->mHighestCrewTypeInPursuit;
        w.f[kWorldPoliceCrewOnscreen] = police->mCrewPedOnscreen;
        w.f[kWorldPoliceFiring] = (int32_t)PedNetId(police->mLastFiringEmergencyPed);
        for (int i = 0; i < MAX_PURSUIT_TARGETS; i++)
        {
            const PolicePursuitTarget& t = police->mPursuitTargets[i];
            w.f[kWorldPursuitCriminal + i] = (int32_t)PedNetId(t.mCriminalPed);
            w.f[kWorldPursuitLevel + i] = t.mWantedLevel;
            w.f[kWorldPursuitState + i] = t.mState;
            w.f[kWorldPursuitTimer + i] = t.mTimer;
            w.f[kWorldPursuitCrews + i] = t.mNumCrews;
            w.f[kWorldPursuitChased + i] = t.mIsActivelyChased;
        }
    }
    w.f[kWorldIt] = gTagGame.mFugitive ? gTagGame.mFugitive->mPlayerIdx + 1 : 0;
    for (int i = 0; i < MAX_PLAYERS; i++)
    {
        w.f[kWorldItTime + i] = gTagGame.mItTimeSecs[i];
    }
    out.entities.push_back(w);
}

void net::engine::ApplyWorld(const EntityState& w)
{
    // The game's tick counter: timers are stamps of it (a car's damage, CarPhysics::mDamagedUntilCycle), so a
    // client's own count would put them elsewhere
    gpRng->mRng = w.f[kWorldCycle];
    ZoneTurn() = (short)w.f[kWorldZoneTurn];
    Round& r = CurrentRound();
    r.number = w.f[kWorldRound];
    r.winner = w.f[kWorldWinner];
    r.secondsLeft = w.f[kWorldSecondsLeft];
    r.phase = (uint8_t)w.f[kWorldPhase];
    r.phaseLeft = w.f[kWorldPhaseLeft];
    r.minPlayers = (uint8_t)w.f[kWorldMinPlayers];
    r.ready = (uint8_t)w.f[kWorldReady];
    r.voteKind = (uint8_t)w.f[kWorldVote];
    r.voteArg = (uint8_t)w.f[kWorldVoteArg];
    r.voteYes = (uint8_t)w.f[kWorldVoteYes];
    r.voteNo = (uint8_t)w.f[kWorldVoteNo];
    r.voteLeft = (uint8_t)w.f[kWorldVoteLeft];
    for (int i = 0; i < kRoundSlots; i++)
    {
        r.mapChoice[i] = (uint8_t)w.f[kWorldMapChoice + i];
    }
    // The tag game's "it" is the server's, made the game's way (its weapons off, the arrow, "you're it")
    Player* it = GetPlayer(w.f[kWorldIt] - 1);
    if (it && it != gTagGame.mFugitive && gTagGame.IsTagGame())
    {
        gTagGame.SetNewFugitive(it);
    }
    for (int i = 0; i < MAX_PLAYERS; i++)
    {
        gTagGame.mItTimeSecs[i] = w.f[kWorldItTime + i];
    }
    if (gpMapRenderer)
    {
        gpMapRenderer->mAmbient.mValue = w.f[kWorldAmbient];
        gpMapRenderer->mTargetAmbient.mValue = w.f[kWorldTargetAmbient];
        gpMapRenderer->mAmbientStep.mValue = w.f[kWorldAmbientStep];
    }
    // The police's state as the server's, for what this game's cops, cop cars and HUD read; its crews stay this
    // game's (they are serviced in Police::Service, which runs on the server only)
    if (Police* police = gPolice_7B8)
    {
        police->mMaxWantedLevel = w.f[kWorldPoliceLevel];
        police->mHighestCrewTypeInPursuit = w.f[kWorldPoliceCrewType];
        police->mCrewPedOnscreen = w.f[kWorldPoliceCrewOnscreen] != 0;
        police->mLastFiringEmergencyPed = NetIdPed((uint32_t)w.f[kWorldPoliceFiring]);
        for (int i = 0; i < MAX_PURSUIT_TARGETS; i++)
        {
            PolicePursuitTarget& t = police->mPursuitTargets[i];
            t.mCriminalPed = NetIdPed((uint32_t)w.f[kWorldPursuitCriminal + i]);
            t.mWantedLevel = w.f[kWorldPursuitLevel + i];
            t.mState = w.f[kWorldPursuitState + i];
            t.mTimer = (u16)w.f[kWorldPursuitTimer + i];
            t.mNumCrews = (u8)w.f[kWorldPursuitCrews + i];
            t.mIsActivelyChased = w.f[kWorldPursuitChased + i] != 0;
        }
    }
    PortRand::SetState((unsigned int)w.f[kWorldRand]);
    PortRand::SetSeed((unsigned int)w.f[kWorldSeed]);
    // The traffic lights as the server's: each game ran its own from its start, so a client's cars waited at a red
    // the server's had as green (and were put back behind it by the next snapshot)
    TrafficLights* lights = gTrafficLights_194;
    if (lights && gCarManager_6C && w.f[kWorldLights])
    {
        const u8 phase = (u8)(w.f[kWorldLights] & 0xF);
        lights->mTimer = (u8)(w.f[kWorldLights] >> 4);
        gCarManager_6C->mLightStateA = (w.f[kWorldLights] >> 12) & 0xF;
        if (lights->mPhase != phase)
        {
            lights->mPhase = phase;
            for (u32 i = 0; i < lights->mArrayUsedCount; i++)
            {
                lights->mTrafficLights[i]->UpdateLightsFromPhase(phase);
            }
        }
    }
}

int net::engine::SnapshotGame(const Snapshot& snap)
{
    const EntityState* w = FindWorld(snap);
    return w ? w->f[kWorldGame] : -1;
}

int net::engine::TagSeconds(int slot)
{
    return slot >= 0 && slot < MAX_PLAYERS ? gTagGame.mItTimeSecs[slot] : 0;
}
