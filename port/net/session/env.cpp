// port/net: the session's GTA2_NET_* switches (env.hpp), read once.
#include "env.hpp"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

const net::SessionEnv& net::Env()
{
    static const SessionEnv sEnv = [] {
        SessionEnv e;
        e.record = getenv("GTA2_NET_RECORD");
        e.dumpWorld = getenv("GTA2_NET_DUMPWORLD");
        e.marker = getenv("GTA2_NET_MARKER") != NULL;
        e.corrLog = getenv("GTA2_NET_CORRLOG") != NULL;
        e.animLog = getenv("GTA2_NET_ANIMLOG") != NULL;
        e.inputLog = getenv("GTA2_NET_INPUTLOG") != NULL;
        e.jerkLog = getenv("GTA2_NET_JERKLOG") ? (atoi(getenv("GTA2_NET_JERKLOG")) >= 2 ? 2 : 1) : 0;
        e.noForward = getenv("GTA2_NET_NOFORWARD") && atoi(getenv("GTA2_NET_NOFORWARD"));
        e.holdFire = getenv("GTA2_NET_HOLDFIRE") && atoi(getenv("GTA2_NET_HOLDFIRE"));
        e.holdAll = getenv("GTA2_NET_HOLDALL") && atoi(getenv("GTA2_NET_HOLDALL"));
        // Tuned on recordings (port/docs/netcode.md, "The client"): the other players shown a third nearer the server's
        e.resimSaves = getenv("GTA2_NET_RESIMSAVES") ? atoi(getenv("GTA2_NET_RESIMSAVES")) : 4;
        e.blend = getenv("GTA2_NET_BLEND") ? (float)atof(getenv("GTA2_NET_BLEND")) : 0.3f;
        e.blendFar = getenv("GTA2_NET_BLEND_FAR") ? (float)atof(getenv("GTA2_NET_BLEND_FAR")) : 0.5f;
        e.blendSelf = getenv("GTA2_NET_BLEND_SELF") ? (float)atof(getenv("GTA2_NET_BLEND_SELF")) : 0.5f;
        e.navDump = getenv("GTA2_NET_NAVDUMP");
        e.testKill = getenv("GTA2_NET_TESTKILL");
        e.testArm = getenv("GTA2_NET_TESTARM");
        e.compare = getenv("GTA2_NET_COMPARE") != NULL;
        e.botIdleFrom = e.botIdleTo = 0;
        if (const char* d = getenv("GTA2_NET_BOTIDLE"))
        {
            sscanf(d, "%u:%u", &e.botIdleFrom, &e.botIdleTo);
        }
        e.divergeTick = 0, e.divergeTicks = 300;
        if (const char* d = getenv("GTA2_NET_DIVERGE")) // <server tick>[:<ticks>]
        {
            sscanf(d, "%u:%u", &e.divergeTick, &e.divergeTicks);
        }
        e.testPowerUp = getenv("GTA2_NET_TESTPOWERUP");
        e.testStreak = getenv("GTA2_NET_TESTSTREAK");
        e.testFire = getenv("GTA2_NET_TESTFIRE");
        e.testBlast = getenv("GTA2_NET_TESTBLAST");
        e.testPhase = getenv("GTA2_NET_TESTPHASE");
        e.testPatients = getenv("GTA2_NET_TESTPATIENTS");
        e.testShot = getenv("GTA2_NET_TESTSHOT");
        e.testCar = getenv("GTA2_NET_TESTCAR");
        e.testTrain = getenv("GTA2_NET_TESTTRAIN");
        e.snapCheck = getenv("GTA2_NET_SNAPCHECK") != NULL;
        e.stats = getenv("GTA2_NET_STATS") != NULL;
        e.replayOn = getenv("GTA2_NET_REPLAYON") ? atoi(getenv("GTA2_NET_REPLAYON")) : 0;
        e.say = getenv("GTA2_NET_SAY");
        e.botVoteMap2 = getenv("GTA2_NET_BOTMAP2") != NULL;
        e.scoreboard = getenv("GTA2_NET_SCOREBOARD") && !strcmp(getenv("GTA2_NET_SCOREBOARD"), "1");
        e.testWanted = getenv("GTA2_NET_TESTWANTED");
        e.fuzz = 0, e.fuzzRate = 20;
        if (const char* f = getenv("GTA2_NET_FUZZ"))
        {
            sscanf(f, "%u:%u", &e.fuzz, &e.fuzzRate);
        }
        return e;
    }();
    return sEnv;
}
