// port/net (port/docs/netcode.md, "Environment variables"): the session's GTA2_NET_* switches, read once (env.cpp). Most
// are for the checks and test hooks (port/net/dev).
#pragma once
#include <stdint.h>

namespace net
{
// The session's GTA2_NET_* switches (port/docs/netcode.md), read once
struct SessionEnv
{
    const char* record; // a file to record the client's session to
    bool marker; // the server's position of the local player drawn
    bool corrLog, animLog, inputLog;
    bool compare; // the comparison check: the prediction against each snapshot, field by field
    bool stats; // GTA2_NET_STATS: a client shows its loss, snapshot rate, lead and resimulation with the ping
    bool snapCheck; // GTA2_NET_SNAPCHECK: the server adds each entity's hash to its snapshots, a client checks them
    int replayOn; // GTA2_NET_REPLAYON=<frames>: a replay runs on that long after the recording, nothing arriving
    uint32_t divergeTick, divergeTicks; // the divergence check: from that server tick on this game alone, for that many ticks
    int jerkLog; // 1: the jerk statistics, 2: and each jump
    bool noForward; // GTA2_NET_NOFORWARD=1: a client predicts other players by their held input only, not the server's forwarded controls
    bool holdAll; // GTA2_NET_HOLDALL=1: a client holds other players' last input through a stall (to compare on a replay)
    int resimSaves; // GTA2_NET_RESIMSAVES=<n>: a resimulation saves its first n ticks (4; 0: all of them)
    float blend; // how far a small correction is blended in each tick (0.3; 1: at once)
    float blendFar; // and one near 2 blocks (0.5), linearly between
    float blendSelf; // and the local player's own, on foot or its car (0.5)
    const char* navDump; // the bots' map as a picture
    const char* testKill; // <tick>:<victim>:<killer>[,...]: server test kills
    const char* testArm; // <weapon>:<ammo>: every player kept armed on the server
    const char* testWanted; // <points>: every player kept at least that wanted on the server
    const char* testPowerUp; // <tick>:<player>:<power-up>[,...]: server test power-ups
    const char* testStreak; // <tick>:<player>[,...]: server test kill streaks
    const char* testFire; // <tick>:<player>[,...]: server test players set on fire
    const char* testCar; // <tick>:<player>:<model>[,...]: server test cars beside players (11: a bus)
    const char* testTrain; // <tick>:<player>:<train>[,...]: server players put at the wheel of a train (train + 10: it reverses)
    const char* say; // <frame>:<command>[;...]: a client's test commands to the server ("/kick r_b")
    uint32_t fuzz, fuzzRate; // GTA2_NET_FUZZ=<seed>[:<per mille>]: a client turns that many of each snapshot's fields to random values (20)
    bool botVoteMap2; // GTA2_NET_BOTMAP2: a bot client votes map 2 in the results (a test sees the map change)
};
const SessionEnv& Env();
} // namespace net
