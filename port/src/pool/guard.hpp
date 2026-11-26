// A full fixed pool (Source/Pool.hpp) refuses the allocation instead of reading through its empty free list: its
// caller skips the creation (Source/ sites under GTA2_PORT), with a warning line at most once a minute per pool.
// Ambient creations leave a few entries for the essential ones (players, scripts, police, ped bodies).
// GTA2_POOL_LIMIT=Name=N,... caps a pool's use at N for tests (Name as in the warning, for example Object_3C=30).
#pragma once

namespace PortPool
{
struct Info
{
    char name[32];
    int cap;
    int limit; // GTA2_POOL_LIMIT's cap, 0: the pool's own
    int used; // at the last refusal
    unsigned int skips; // since the last warning line
    double lastLine; // seconds, wall clock
};

Info* Register(const char* prettyFunction, int cap); // from a template's __PRETTY_FUNCTION__ ("PoolType = X")
void Refused(Info* info, int used); // the pool had no room: full, or short of a reserve the creation leaves
void Skipped(const char* what); // the caller skipped <what> after its pool refused; logs at most once a minute per pool

// A room check's result, false logged as <what> skipped
inline bool RoomOrSkip(bool room, const char* what)
{
    if (!room)
    {
        Skipped(what);
    }
    return room;
}

// While one exists (the scripts run), creations are essential: they may use the reserves the others leave
extern int gEssential;
struct Essential
{
    Essential()
    {
        ++gEssential;
    }
    ~Essential()
    {
        --gEssential;
    }
};
} // namespace PortPool
