// VC6's rand()/srand(), see rand.hpp.
#include "rand.hpp"
#include "sound.hpp"
#include "state.hpp"
#include "owner.hpp"
#include "../port_globals.hpp"

// 10.5.exe keeps it at 0x626FE0 (a single-threaded CRT): registered, so the network game's rollback keeps it
static unsigned int sState = 1;
PORT_GLOBAL(sState, 0x626FE0);

void port_srand_5EDED7(unsigned int seed)
{
    sState = seed;
}

unsigned int PortRand::State()
{
    return sState;
}

void PortRand::SetState(unsigned int state)
{
    sState = state;
}

unsigned int PortRand::gCalls;
void (*PortRand::gLog)(void* caller, bool owned);

// The owners' draws (owner.hpp)
unsigned int (*PortRand::gPedKey)(const Ped* p);
unsigned int (*PortRand::gCarKey)(const Car* c);
unsigned int (*PortRand::gObjectKey)(const Object* o);
bool PortRand::gOwned;
void (*PortRand::gTickHook)();
static unsigned int sOwner, sOwnerDraws, sSeed, sTick;
static unsigned int sCreator, sCreatorMade; // who makes objects now, and how many keyed ones this update

void PortRand::Scope::Create(unsigned int key)
{
    mCreating = key != 0;
    mSavedCreator = sCreator, mSavedMade = sCreatorMade;
    if (mCreating)
    {
        sCreator = key, sCreatorMade = 0;
    }
}

void PortRand::Scope::Enter(unsigned int key)
{
    mActive = key != 0;
    mSavedKey = sOwner, mSavedDraws = sOwnerDraws;
    if (mActive)
    {
        sOwner = key, sOwnerDraws = 0;
    }
    Create(key);
}

PortRand::Scope::Scope(unsigned int serviceKey)
{
    Enter(gOwned ? serviceKey : 0);
}

const Ped* PortRand::gLastPed;
const Car* PortRand::gLastCar;

PortRand::Scope::Scope(const Ped* p)
{
    gLastPed = p;
    Enter(gPedKey ? gPedKey(p) : 0);
}

PortRand::Scope::Scope(const Car* c)
{
    gLastCar = c;
    Enter(gCarKey ? gCarKey(c) : 0);
}

PortRand::Scope::Scope(const Object* o)
{
    const unsigned int key = gObjectKey ? gObjectKey(o) : 0;
    mActive = key != 0;
    mSavedKey = sOwner, mSavedDraws = sOwnerDraws;
    if (mActive)
    {
        sOwner = 0x05000000u | key, sOwnerDraws = 0; // 24-bit keys, apart from the ped and car owners (net ids)
    }
    Create(key);
}

PortRand::Scope::~Scope()
{
    if (mActive)
    {
        sOwner = mSavedKey, sOwnerDraws = mSavedDraws;
    }
    if (mCreating)
    {
        sCreator = mSavedCreator, sCreatorMade = mSavedMade;
    }
}

void PortRand::TickStart(unsigned int cycle)
{
    sTick = cycle;
    sCreator = 0, sCreatorMade = 0;
    if (gTickHook)
    {
        gTickHook();
    }
}

void PortRand::SetSeed(unsigned int seed)
{
    sSeed = seed;
}

unsigned int PortRand::Seed()
{
    return sSeed;
}

// 15 bits, as rand()'s, from (seed, owner, tick, draw): splitmix64's finalizer
static int OwnedDraw()
{
    unsigned long long x = sSeed * 0x9E3779B97F4A7C15ull ^ sOwner * 0xC2B2AE3D27D4EB4Full ^ sTick * 0x165667B19E3779F9ull ^
                           (sOwnerDraws++ + 1) * 0xD6E8FEB86659FD93ull;
    x = (x ^ (x >> 30)) * 0xBF58476D1CE4E5B9ull;
    x = (x ^ (x >> 27)) * 0x94D049BB133111EBull;
    x ^= x >> 31;
    return (int)(x & 0x7FFF);
}

unsigned int PortRand::NextObjectKey()
{
    unsigned long long x = sSeed * 0x9E3779B97F4A7C15ull ^ sCreator * 0xC2B2AE3D27D4EB4Full ^ sTick * 0x165667B19E3779F9ull ^
                           (++sCreatorMade) * 0xD6E8FEB86659FD93ull ^ 0x5851F42D4C957F2Dull;
    x = (x ^ (x >> 30)) * 0xBF58476D1CE4E5B9ull;
    x = (x ^ (x >> 27)) * 0x94D049BB133111EBull;
    x ^= x >> 31;
    const unsigned int key = (unsigned int)(x & 0xFFFFFF);
    return key ? key : 1;
}

int port_rand_5EDEE1()
{
    PortRand::gCalls++;
    if (PortRand::gLog)
    {
        PortRand::gLog(__builtin_return_address(0), sOwner != 0); // the divergence check's log of who draws (port/net/engine/dev/compare.cpp)
    }
    if (sOwner)
    {
        return OwnedDraw();
    }
    return PortRand::Step(sState);
}

// sound_obj's own stream (sound.hpp), the same generator
static bool sSoundSeparate;
static unsigned int sSoundState = 1;

void PortRand::SetSoundSeparate(bool separate)
{
    sSoundSeparate = separate;
}

int PortRand::SoundDraw()
{
    if (!sSoundSeparate)
    {
        return port_rand_5EDEE1();
    }
    return Step(sSoundState);
}

