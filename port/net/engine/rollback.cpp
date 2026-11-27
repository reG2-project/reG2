// port/net (port/docs/netcode.md, "The rollback set"): a client saves the game after every tick and, when a snapshot for an
// older tick arrives, restores that tick, applies the snapshot and resimulates up to now with its own inputs.
#include "internal.hpp"
#include "world.hpp"
#include "dev/dev.hpp"
#include "port/src/interpolation.hpp"
#include "Source/Game.hpp"
#include "Source/Player.hpp"
#include "Source/Car.hpp"
#include "Source/Char_Pool.hpp"
#include "Source/CollisionSoundQueue.hpp"
#include "Source/CrimeReportQueue.hpp"
#include "Source/Weapon.hpp"
#include "Source/WeaponManager.hpp"
#include "Source/sound_obj.hpp"
#include "Source/Ped.hpp"
#include "Source/rng.hpp"
#include "Source/SoundObject.hpp"
#include "Source/TagGame.hpp"
#include "port/src/net_hooks.hpp"
#include <algorithm>
#include <stddef.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <string>

struct net::engine::SavedWorld::Impl
{
    std::vector<unsigned char> bytes;
    ReplicaState* replicas = NULL;
    unsigned set = 0; // the rollback set it was saved with (gSet)
};

namespace
{
unsigned gSet; // which rollback set: taken again, its blocks lie elsewhere in a saved world (Restorable)
unsigned gSetBase; // the oldest set whose blocks start every set since (only blocks added at the end since then)

// The first block that differs between two sets (none: -1)
long FirstDifference(const std::vector<net::engine::Block>& a, const std::vector<net::engine::Block>& b)
{
    for (size_t i = 0; i < a.size() || i < b.size(); i++)
    {
        if (i >= a.size() || i >= b.size() || a[i].addr != b[i].addr || a[i].size != b[i].size)
        {
            return (long)i;
        }
    }
    return -1;
}

// The rollback set, taken again whenever it would be another: a new game (a map change's new Game can come at the old
// one's address, with its pools elsewhere: the old blocks wrote over freed memory), a function's static registered, a
// pointee a global gained (NULL when the set was taken) or that moved. A world saved with an older set isn't restored
// (Restorable)
const std::vector<net::engine::Block>& RollbackSet()
{
    static std::vector<net::engine::Block> sBlocks;
    std::vector<net::engine::Block> now = net::engine::WorldBlocks();
    const long diff = FirstDifference(sBlocks, now);
    if (diff >= 0)
    {
        const bool appended = (size_t)diff == sBlocks.size() && now.size() > sBlocks.size();
        sBlocks.swap(now);
        gSet++;
        gSetBase = appended ? gSetBase : gSet;
        // A block that keeps changing (a scratch pointer with a pointee, for kNoPointee in world.cpp) would leave no save
        // to restore: each retake throws the saves away
        static int sGeneration = -1, sTakes;
        sTakes = sGeneration == net::engine::GameGeneration() ? sTakes + 1 : 0;
        sGeneration = net::engine::GameGeneration();
        if (sTakes == 50)
        {
            printf("net: CHECK the rollback set taken 50 times in this game, the last time from block %ld (%s)\n", diff,
                   (size_t)diff < sBlocks.size() ? sBlocks[diff].name : "-");
        }
        if (net::engine::Debug().checkGrid)
        {
            size_t bytes = 0;
            for (const net::engine::Block& b : sBlocks)
            {
                bytes += b.size;
            }
            printf("net: the rollback set taken (set %u): %zu blocks, %zu bytes, from block %ld (%s)\n", gSet, sBlocks.size(), bytes, diff,
                   (size_t)diff < sBlocks.size() ? sBlocks[diff].name : "-");
            // GTA2_NET_CHECKGRID: a block that runs past the start of the next into memory beyond it (one inside another,
            // a camera inside its player, is fine). A pointee bigger than what its pointer points to is restored over
            // whatever lies after it
            std::vector<net::engine::Block> by = sBlocks;
            std::sort(by.begin(), by.end(), [](const net::engine::Block& a, const net::engine::Block& b)
                      { return a.addr != b.addr ? a.addr < b.addr : a.size > b.size; }); // at one address, the outer first
            for (size_t i = 1; i < by.size(); i++)
            {
                const unsigned char* end = by[i - 1].addr + by[i - 1].size;
                if (by[i].addr < end && by[i].addr + by[i].size > end)
                {
                    printf("net: CHECK the rollback block %s (%p, %zu bytes) runs into %s (%p, %zu bytes)\n", by[i - 1].name, (void*)by[i - 1].addr,
                           by[i - 1].size, by[i].name, (void*)by[i].addr, by[i].size);
                }
            }
        }
    }
    return sBlocks;
}

bool sFirstResim; // the next ResimTick is the first since a restore
} // namespace

net::engine::SavedWorld::SavedWorld() : mImpl(new Impl())
{
}

net::engine::SavedWorld::~SavedWorld()
{
    FreeReplicas(mImpl->replicas);
    delete mImpl;
}

void net::engine::SaveGame(SavedWorld& out, uint32_t tick)
{
    SaveWorld(RollbackSet(), out.mImpl->bytes);
    out.mImpl->set = gSet;
    out.mImpl->replicas = SaveReplicas(out.mImpl->replicas);
    out.tick = tick;
    out.valid = true;
}

// A world saved with another set (a pool a global points to allocated puts a block in the middle) would be copied into
// the wrong blocks: a pool's list from another's bytes. One saved before blocks came at the end is the start of this one
bool net::engine::Restorable(const SavedWorld& in)
{
    RollbackSet();
    return in.mImpl->set >= gSetBase && in.mImpl->set <= gSet;
}

void net::engine::RestoreGame(const SavedWorld& in)
{
    // The sound's channel bookkeeping (its sample queues and the channels' samples) mirrors the mixer, which isn't
    // rolled back (world.cpp): it stays as it is. Restored, a loop started since the snapshot's tick (a machine gun)
    // played on with nothing left to stop it. Its audio entities, which follow game objects, are restored.
    const size_t from = offsetof(sound_obj, msQueueSample), to = offsetof(sound_obj, mUnused1440);
    static unsigned char sLive[offsetof(sound_obj, mUnused1440) - offsetof(sound_obj, msQueueSample)];
    unsigned char* channels = (unsigned char*)&gSound_obj + from;
    memcpy(sLive, channels, to - from);
    const u8 active = gSound_obj.mnActiveSamples;
    // So do the radios, from the car radio's stations (mRadioEmitters) on: a resimulation runs no sound, so the saves
    // would hold the radio's state from before the player got in, and the car radio would start again on another station
    // every tick; and every correction would start the police radio's call again over itself
    const size_t radio = offsetof(sound_obj, mRadioEmitters);
    static unsigned char sRadio[sizeof(sound_obj) - offsetof(sound_obj, mRadioEmitters)];
    memcpy(sRadio, (unsigned char*)&gSound_obj + radio, sizeof(sRadio));
    // The window's size is this machine's, not the world's: a resize since the save (its cameras' screen size, set only
    // on a change) would otherwise go back to the old one for good
    const Camera* view = gGame_0x40 && gGame_0x40->mLocalPlayer ? &gGame_0x40->mLocalPlayer->mViewCamera : NULL;
    const s32 screenW = view ? view->mScreenPxWidth : 0, screenH = view ? view->mScreenPxHeight : 0;
    RestoreWorld(RollbackSet(), in.mImpl->bytes);
    if (view && (view->mScreenPxWidth != screenW || view->mScreenPxHeight != screenH))
    {
        gGame_0x40->SetScreenSize(screenW, screenH);
    }
    memcpy(channels, sLive, to - from);
    memcpy((unsigned char*)&gSound_obj + radio, sRadio, sizeof(sRadio));
    gSound_obj.mnActiveSamples = active;
    RestoreReplicas(in.mImpl->replicas);
    ResimFrom(in.tick);
}

void net::engine::ResimFrom(uint32_t)
{
    sFirstResim = true;
}

void net::engine::ResimStepStart()
{
    PortInterp::SnapshotResim();
}

void net::engine::ResimTick(const uint32_t* buttons, int count)
{
    Game* g = gGame_0x40;
    if (sFirstResim)
    {
        // By the game's cycle, as the logs stamp their ticks: a map change starts it again, not the network's tick
        const uint32_t cycle = (uint32_t)gpRng->get_cur_rng();
        SoundLogRestored(cycle);
        ShotVisRollback(cycle);
        SoundLogResimTick(true); // what the snapshot's Apply left for the sound
        sFirstResim = false;
    }
    for (int i = 0; i < count && i < (int)g->mNumPlayers; i++)
    {
        Player* p = g->mPlayers[i];
        if (p && p->mInUse)
        {
            p->SetInputs(buttons[i] & 0xFFF); // the controls only: key events (chat) happen once, not per resim
        }
    }
    ProtectPlayers();
    GuardPeds();
    KeepPlayerArrows();
    const bool noRumble = gPortNoRumble;
    gPortNoRumble = true; // felt when the tick was predicted, as its sounds were heard
    SoundLogResimStep(true);
    g->UpdateAllPlayersControls();
    if (g->mGameState == GameState::Running_1)
    {
        g->UpdateGame(); // the tick without the sound service (Game::ExecuteGame without sound)
    }
    gTagGame.ExecuteGamemodeTick(); // a network game's frame runs it after the tick (do_network_and_local_inputs)
    gPortNoRumble = noRumble;
    SoundLogResimStep(false);
    GuardPeds(); // after it too: the live tick follows the last one without a frame in between
    SoundLogResimTick(false);
    // What the sound log's pass left (what no audio entity follows, the police radio's crimes) is taken too: a save holds
    // no sound of its tick for a restore to play again. The sim's word to the sound: a weapon's shot, the impacts of the
    // tick (a landing, a crash) and a voice line.
    if (gCollisionSoundQueue)
    {
        gCollisionSoundQueue->Reset();
    }
    gSound_obj.mVocals.mPendingVoice = 0; // sound_obj::PlayVocal's slot (0: none)
    if (gCrimeReportQueue) // the crimes the police radio calls out
    {
        gCrimeReportQueue->mReadIdx = gCrimeReportQueue->mWriteIdx;
    }
    // ... and what it takes from peds and cars (all the Take* accessors sound_obj.cpp uses)
    for (Ped* p = gPedPool->mPool.mpPrev; p; p = p->mpNext)
    {
        p->TakeVoiceEvent(); // what it says next (a landing from a roof, a hit)
        // Its weapons, which aren't on the weapon pool's list (an instant gang's dual pistols heard for ever)
        for (Weapon* w : {p->mSelectedWeapon, p->mpWeapon})
        {
            if (w)
            {
                w->TakeShotFired();
            }
        }
    }
    for (Car* c = gCar_BC_Pool->mPool.mpPrev; c; c = c->mpNext)
    {
        c->TakeSoundEvent();
        c->TakeShopEvent();
    }
    for (Weapon* w = gWeapon_30_Pool->mPool.mpPrev; w; w = w->mpNext)
    {
        w->TakeShotFired();
    }
    for (int i = 0; i < (int)g->mNumPlayers; i++) // the players' own weapons aren't on the pool's list
    {
        if (Player* p = g->mPlayers[i])
        {
            for (int w = 0; w < kPlayerWeapons; w++)
            {
                if (p->mWeapons[w])
                {
                    p->mWeapons[w]->TakeShotFired();
                }
            }
        }
    }
}
