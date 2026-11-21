// port/net (port/docs/netcode.md, "The rollback set"): a client saves the game after every tick and, when a snapshot for an
// older tick arrives, restores that tick, applies the snapshot and resimulates up to now with its own inputs.
#include "internal.hpp"
#include "world.hpp"
#include "dev/dev.hpp"
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
        sBlocks.swap(now);
        gSet++;
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

// The sounds a resimulation drains that the live ticks never played: what a snapshot's Apply set (a death the server
// carried out: the cry, the killer's voice line, a car's explosion), and a player's shot the live prediction didn't
// fire (another player's input arrives after its tick). They are given back after each resimulated save, so the
// live tick's sound plays them and no save holds them
struct OwedVoice
{
    Ped* mpPed;
    s32 mId;
    s32 mVoice;
};
struct OwedCar
{
    Car* mpCar;
    s32 mId;
    s32 mEvent;
};
std::vector<OwedVoice> sOwedVoices;
std::vector<OwedCar> sOwedCars;
s32 sOwedLine; // sound_obj::mVocals.mPendingVoice
u32 sOwedShots[MAX_PLAYERS]; // by Player::mWeapons slot
bool sFirstResim; // the next ResimTick is the first since a restore
bool sResimSaved; // the next save is a resimulated tick's
u32 sResimShots[MAX_PLAYERS]; // the weapons the last ResimTick fired
// The weapons each live tick fired, from their ammo, and the ammo the next tick starts from
const int kShotTicks = 64;
struct LiveShots
{
    uint32_t mTick;
    u32 mShots[MAX_PLAYERS];
} sLiveShots[kShotTicks];
u16 sAmmo[MAX_PLAYERS][net::engine::kPlayerWeapons];
bool sWasDead[MAX_PLAYERS]; // each player's ped at the last save

Weapon* SlotWeapon(int player, int slot)
{
    Game* g = gGame_0x40;
    Player* p = player < (int)g->mNumPlayers ? g->mPlayers[player] : NULL;
    return p && p->mInUse ? p->mWeapons[slot] : NULL;
}

void GiveOwedSounds(bool owed)
{
    for (const OwedVoice& v : sOwedVoices)
    {
        if (v.mpPed->mId == v.mId)
        {
            v.mpPed->mVoiceEvent = owed ? v.mVoice : 0;
        }
    }
    for (const OwedCar& c : sOwedCars)
    {
        if (c.mpCar->mCarId == c.mId)
        {
            c.mpCar->mSoundEvent = owed ? c.mEvent : 0;
        }
    }
    if (sOwedLine)
    {
        gSound_obj.mVocals.mPendingVoice = owed ? sOwedLine : 0;
    }
    for (int i = 0; i < MAX_PLAYERS; i++)
    {
        for (int w = 0; w < net::engine::kPlayerWeapons; w++)
        {
            Weapon* weapon = sOwedShots[i] >> w & 1 ? SlotWeapon(i, w) : NULL;
            if (weapon)
            {
                weapon->mShotFired = owed;
            }
        }
    }
}

void ForgetOwedSounds()
{
    sOwedVoices.clear();
    sOwedCars.clear();
    sOwedLine = 0;
    memset(sOwedShots, 0, sizeof(sOwedShots));
}

// A sound from an Apply, unless it was heard within a second: a death the prediction showed too (its cry), or one a
// late snapshot carries out again. By VoiceKey, car id + kCarKey, or kLineKey
struct GivenVoice
{
    s32 mId;
    s32 mVoice;
    uint32_t mTick;
};
std::vector<GivenVoice> sGivenVoices;
const s32 kCarKey = 1 << 24;
const s32 kPlayerKey = 1 << 23; // a player's ped: by player, as a death gives the player another ped
s32 VoiceKey(const Ped* p)
{
    return p->mPlayer ? kPlayerKey + p->mPlayer->mPlayerIdx : p->mId;
}
const s32 kDeathCry = 4; // Ped::Kill's voice
const s32 kLineKey = -1;
uint32_t sLiveTick;

bool GivenLately(s32 id, s32 voice)
{
    const uint32_t kTicks = 30;
    sGivenVoices.erase(std::remove_if(sGivenVoices.begin(), sGivenVoices.end(),
                                      [](const GivenVoice& g) { return sLiveTick - g.mTick > kTicks; }),
                       sGivenVoices.end());
    for (const GivenVoice& g : sGivenVoices)
    {
        if (g.mId == id && g.mVoice == voice)
        {
            return true;
        }
    }
    sGivenVoices.push_back({id, voice, sLiveTick});
    return false;
}

// What a snapshot's Apply left for the sound: the world was a save's, which holds none
void TakeApplySounds()
{
    for (Ped* p = gPedPool->mPool.mpPrev; p; p = p->mpNext)
    {
        if (p->mVoiceEvent && !GivenLately(VoiceKey(p), p->mVoiceEvent))
        {
            sOwedVoices.push_back({p, p->mId, p->mVoiceEvent});
        }
    }
    for (Car* c = gCar_BC_Pool->mPool.mpPrev; c; c = c->mpNext)
    {
        if (c->mSoundEvent && !GivenLately(kCarKey + c->mCarId, c->mSoundEvent))
        {
            sOwedCars.push_back({c, c->mCarId, c->mSoundEvent});
        }
    }
    const s32 line = gSound_obj.mVocals.mPendingVoice;
    sOwedLine = line && !GivenLately(kLineKey, line) ? line : 0;
}

// After a save: a resimulated tick's shots no live tick fired are owed; a live tick's shots and deaths are noted
void NoteLiveSounds(uint32_t tick)
{
    LiveShots& live = sLiveShots[tick % kShotTicks];
    if (live.mTick != tick)
    {
        live.mTick = tick;
        memset(live.mShots, 0, sizeof(live.mShots));
    }
    for (int i = 0; i < MAX_PLAYERS; i++)
    {
        // A player's death in a live tick: its cry was heard
        Player* player = i < (int)gGame_0x40->mNumPlayers ? gGame_0x40->mPlayers[i] : NULL;
        Ped* ped = player && player->mInUse ? player->mPlayerPed : NULL;
        const bool dead = ped && ped->isDead();
        if (!sResimSaved && dead && !sWasDead[i])
        {
            GivenLately(kPlayerKey + i, kDeathCry);
        }
        sWasDead[i] = dead;
        for (int w = 0; w < net::engine::kPlayerWeapons; w++)
        {
            const Weapon* weapon = SlotWeapon(i, w);
            const u16 ammo = weapon ? weapon->mAmmo : 0;
            if (!sResimSaved && weapon && ammo < sAmmo[i][w])
            {
                live.mShots[i] |= 1u << w;
            }
            sAmmo[i][w] = ammo;
        }
        if (sResimSaved)
        {
            // Within a tick of a live shot it is that shot, a tick early or late
            u32 heard = 0;
            for (uint32_t t = tick - 1; t != tick + 2; t++)
            {
                heard |= sLiveShots[t % kShotTicks].mTick == t ? sLiveShots[t % kShotTicks].mShots[i] : 0;
            }
            sOwedShots[i] |= sResimShots[i] & ~heard;
            live.mShots[i] |= sResimShots[i];
        }
    }
}
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
    NoteLiveSounds(tick);
    if (sResimSaved)
    {
        GiveOwedSounds(true);
        sResimSaved = false;
    }
    else
    {
        ForgetOwedSounds(); // the live tick played them
        sLiveTick++;
    }
}

// A world saved with another set (taken again since: a function's static registered, or a pool a global points to
// allocated, puts a block in the middle) would be copied into the wrong blocks: a pool's list from another's bytes
bool net::engine::Restorable(const SavedWorld& in)
{
    RollbackSet();
    return in.mImpl->set == gSet;
}

void net::engine::RestoreGame(const SavedWorld& in)
{
    // The sound's channel bookkeeping (its sample queues and the 16 channels' samples) mirrors the mixer, which isn't
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
    ForgetOwedSounds(); // the live tick played them, or the restore took them back
    sFirstResim = true;
}

void net::engine::ResimTick(const uint32_t* buttons, int count)
{
    Game* g = gGame_0x40;
    if (sFirstResim)
    {
        TakeApplySounds();
        sFirstResim = false;
    }
    GiveOwedSounds(false); // given back after the last save, not into this tick's
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
    SeenByAll();
    const bool noRumble = gPortNoRumble;
    gPortNoRumble = true; // felt when the tick was predicted, as its sounds were heard
    g->UpdateAllPlayersControls();
    if (g->mGameState == GameState::Running_1)
    {
        g->UpdateGame(); // the tick without the sound service (Game::ExecuteGame without sound)
    }
    gTagGame.ExecuteGamemodeTick(); // a network game's frame runs it after the tick (do_network_and_local_inputs)
    gPortNoRumble = noRumble;
    GuardPeds(); // after it too: the live tick follows the last one without a frame in between
    for (int i = 0; i < MAX_PLAYERS; i++)
    {
        sResimShots[i] = 0;
        for (int w = 0; w < kPlayerWeapons; w++)
        {
            const Weapon* weapon = SlotWeapon(i, w);
            sResimShots[i] |= weapon && weapon->mShotFired ? 1u << w : 0;
        }
    }
    sResimSaved = true;
    // What the sound service takes from the sim each tick, taken here as it doesn't run: a resimulated sound was heard
    // when it was predicted. Left there, it went into the saves and every restore played it again (a gun that went on
    // firing, a laugh or a step heard over and over). The sim's word to the sound: a weapon's shot, the impacts of the
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
        const s32 voice = p->TakeVoiceEvent(); // what it says next (a landing from a roof, a hit)
        // A player's death cry no live tick had is owed; any other voice was heard when the tick was predicted. Either
        // way an Apply's same voice isn't owed
        if (voice && !GivenLately(VoiceKey(p), voice) && voice == kDeathCry && p->mPlayer)
        {
            sOwedVoices.push_back({p, p->mId, voice});
        }
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
