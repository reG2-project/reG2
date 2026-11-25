// port/net (port/docs/netcode.md, "Sound on a client"): a client's sound log. A resimulated tick runs the sound service's entities
// without the mixer (SoundLogResimTick): what each would have asked for is noted, and its progress (entity ages, the per-car
// SoundPosEntry, the frame counter) moves on with the world. The live ticks note what they asked for. Once a snapshot
// settles a tick, its one-shot requests that no live tick had (same owner and sound, within 2 ticks and a block) are
// added to the next live frame's, each once. A car's impact on another player, or by another player's car, is a guess at
// their inputs: a live tick doesn't hear it, the settled tick does.
#include "internal.hpp"
#include "Source/Car.hpp"
#include "Source/CollisionSoundQueue.hpp"
#include "Source/Game.hpp"
#include "Source/Object_5C.hpp"
#include "Source/Ped.hpp"
#include "Source/Player.hpp"
#include "Source/SoundObject.hpp"
#include "Source/char.hpp"
#include "Source/rng.hpp"
#include "Source/sound_obj.hpp"
#include "Source/sprite.hpp"
#include "port/src/net_hooks.hpp"
#include <algorithm>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <vector>

namespace
{
struct SoundEvent
{
    const void* mOwner; // what the audio entity follows (its SoundObject's mpAny), or the vocals
    s32 mKind; // a car or a char (by its id, as a resimulation can make it again elsewhere in its pool), an object (by
               // place: a snapshot's blast is made again with another id), or minus the sound object's type (by the pointer)
    s32 mId;
    u32 mSound; // which of its owner's sounds (sound_0x68::mSampleIndex: a footstep, a door, whatever variant it
                // draws; an object's sample), the voice line for the vocals, the collision queue's impacts by place: 40 a
                // car's on a ped, 0 the others
    s32 mX, mY; // in blocks: the collision queue's events share one owner
    uint32_t mTick;
    sound_0x68 mSample;
};
std::vector<SoundEvent> sHeard; // asked for by the live frames, or added as owed
std::vector<SoundEvent> sWould; // asked for by resimulated ticks, the newest resimulation's of each tick
uint32_t sSnapTick; // the last restore's, as the game's cycle
uint32_t sSettled; // the newest tick whose requests were given
bool sRestored;
bool sResimStep; // in a resimulated tick's game step
const uint32_t kKeep = 64; // ticks of the logs

const s32 kCarOwner = 1, kCharOwner = 2, kObjectOwner = 3;

s32 OwnerId(const SoundObject* o, s32* pKind)
{
    Sprite* sprite = o->mObjectType == SoundObjectTypeEnum::Sprite_1 ? o->mpAny.pSprite : NULL;
    *pKind = -o->mObjectType;
    if (!sprite)
    {
        return 0;
    }
    if (Car* c = sprite->AsCar())
    {
        *pKind = kCarOwner;
        return c->mCarId;
    }
    Char* c = sprite->AsCharB4();
    if (c && c->mpPed)
    {
        *pKind = kCharOwner;
        return c->mpPed->mId;
    }
    Object* obj = sprite->AsObject2C();
    *pKind = obj ? kObjectOwner : *pKind;
    return obj ? obj->mId : 0;
}

uint32_t Tick()
{
    return gpRng ? (uint32_t)gpRng->get_cur_rng() : 0;
}

bool Same(const SoundEvent& a, const SoundEvent& b)
{
    const uint32_t dt = a.mTick > b.mTick ? a.mTick - b.mTick : b.mTick - a.mTick;
    return (a.mKind > 0 || a.mOwner == b.mOwner) && a.mKind == b.mKind && (a.mKind == kObjectOwner || a.mId == b.mId) &&
        a.mSound == b.mSound && dt <= 2 && abs(a.mX - b.mX) <= 1 && abs(a.mY - b.mY) <= 1;
}

bool Heard(const SoundEvent& e)
{
    for (const SoundEvent& h : sHeard)
    {
        if (Same(h, e))
        {
            return true;
        }
    }
    return false;
}

// The one-shot requests in the sound's queue (a loop, mLoopCount 0, goes on by the kept channels)
void TakeRequests(std::vector<SoundEvent>& out, uint32_t tick)
{
    sound_obj& s = gSound_obj;
    const u8 q = s.mnActiveSampleQueue;
    for (u8 i = 0; i < s.mSampleRequestQueuesStatus[q]; i++)
    {
        const sound_0x68& r = s.masSamples[q][s.mabSampleQueueIndexTable[q][i]];
        const AudioEntity& e = s.mAudioEntities[r.mEntityIndex];
        if (r.mLoopCount == 0 || r.mpReleaseSource || !e.mbUsed || !e.mpObj)
        {
            continue;
        }
        s32 kind;
        const s32 id = OwnerId(e.mpObj, &kind);
        // The collision queue's impacts are one by place, but a car's on a ped (SelectCarPedCollisionSound, by its
        // strength), which a live tick may not have heard (ImpactHeard) beside the ped's own landing
        const bool impact = e.mpObj->mObjectType == SoundObjectTypeEnum::CollisionSoundQueue_6;
        u32 sound = !impact ? r.mSampleIndex : r.mSampIdx == 40 || r.mSampIdx == 41 ? 40 : 0;
        // An AI driver's insult or beep (HandleAICarHornBeep) takes the next of a shared count, 100 to 199; an object's
        // by its sample, as a small blast and a car's bang there are two
        sound = kind == kCarOwner && sound >= 100 ? 100 : kind == kObjectOwner ? r.mSampIdx : sound;
        out.push_back({e.mpObj->mpAny.pAny, kind, id, sound, r.mObj.mX.ToInt(), r.mObj.mY.ToInt(), tick, r});
    }
    if (s.mVocals.mPendingVoice)
    {
        out.push_back({&s.mVocals, 0, 0, (u32)s.mVocals.mPendingVoice, 0, 0, tick, sound_0x68()});
    }
}

int EntityOf(const SoundEvent& e)
{
    for (int i = 1; i < 1020; i++)
    {
        const AudioEntity& a = gSound_obj.mAudioEntities[i];
        s32 kind;
        if (a.mbUsed && a.mpObj && a.mpObj->mpAny.pAny == e.mOwner && OwnerId(a.mpObj, &kind) == e.mId && kind == e.mKind)
        {
            return i;
        }
    }
    return -1;
}

// A blast's bang (ProcessOtherObjects: a small, a car's or item's, a large one), 0 for another sound
int BangSize(const sound_0x68& r)
{
    return r.mSampIdx >= 186 && r.mSampIdx <= 188 ? (int)r.mSampIdx - 185 : 0;
}

// A bigger bang given where a smaller one plays ends that one, the game's way (AddReleasingSounds no longer carries it):
// the car the server blew up where this client's own blast was
void CutSmallerBangs(const SoundEvent& w)
{
    const uint32_t kBangTicks = 30;
    sound_obj& s = gSound_obj;
    const u8 q = s.mnActiveSampleQueue ? 0 : 1;
    for (const SoundEvent& h : sHeard)
    {
        if (BangSize(h.mSample) == 0 || BangSize(h.mSample) >= BangSize(w.mSample) || h.mTick + kBangTicks < w.mTick ||
            abs(h.mX - w.mX) > 1 || abs(h.mY - w.mY) > 1)
        {
            continue;
        }
        for (u8 i = 0; i < s.mSampleRequestQueuesStatus[q]; i++)
        {
            sound_0x68& r = s.masSamples[q][s.mabSampleQueueIndexTable[q][i]];
            if (r.mSampIdx == h.mSample.mSampIdx && abs(r.mObj.mX.ToInt() - h.mX) <= 1 && abs(r.mObj.mY.ToInt() - h.mY) <= 1)
            {
                r.mReleasingVolumeDivider = 0;
            }
        }
    }
}

// The settled ticks' requests no live frame had, into this frame's queue (the vocal line played at once)
void GiveOwed()
{
    if (!sRestored)
    {
        return;
    }
    const uint32_t upTo = sSnapTick + 1; // the snapshot's next tick runs with the server's state and inputs
    std::vector<SoundEvent> given;
    for (const SoundEvent& w : sWould)
    {
        if (w.mTick <= sSettled || w.mTick > upTo)
        {
            continue;
        }
        net::engine::gSoundStats[0]++;
        if (Heard(w))
        {
            net::engine::gSoundStats[1]++;
            sHeard.push_back(w); // the same sound asked for again (a held one, a death a snapshot carries out again)
            continue;
        }
        sHeard.push_back(w);
        bool again = false; // one owner's sound once a frame
        for (const SoundEvent& g : given)
        {
            again |= g.mOwner == w.mOwner && g.mSound == w.mSound;
        }
        if (again)
        {
            continue;
        }
        given.push_back(w);
        net::engine::gSoundsGiven++;
        if (w.mOwner == &gSound_obj.mVocals)
        {
            // Played now: the next frame's restore and resimulated ticks empty the slot before its vocals read it
            if (!gSound_obj.mVocals.mPendingVoice)
            {
                gSound_obj.mVocals.mPendingVoice = (s32)w.mSound;
                gSound_obj.ProcessType10_Vocals();
            }
            continue;
        }
        const int ent = EntityOf(w);
        net::engine::gSoundStats[2] += ent <= 0;
        if (ent > 0)
        {
            if (BangSize(w.mSample))
            {
                CutSmallerBangs(w);
            }
            memcpy(&gSound_obj.msQueueSample, &w.mSample, sizeof(sound_0x68));
            gSound_obj.msQueueSample.mEntityIndex = ent;
            gSound_obj.AddSampleToRequestedQueue();
        }
    }
    if (upTo > sSettled)
    {
        sSettled = upTo;
    }
}

void Prune(uint32_t now)
{
    auto old = [now](const SoundEvent& e) { return e.mTick + kKeep < now; };
    sHeard.erase(std::remove_if(sHeard.begin(), sHeard.end(), old), sHeard.end());
    sWould.erase(std::remove_if(sWould.begin(), sWould.end(), [](const SoundEvent& e) { return e.mTick <= sSettled; }),
                 sWould.end());
}

// gPortImpactHeard: a car's impact on a ped that another player takes part in, as the ped or the car's driver, waits for
// the tick's settling (GiveOwed); the others are as predictable as the local player's inputs and the game's own AI
bool ImpactHeard(Sprite* a, Sprite* b)
{
    Car* car = a->AsCar() ? a->AsCar() : b->AsCar();
    Char* c = a->AsCharB4() ? a->AsCharB4() : b->AsCharB4();
    if (sResimStep || !car || !c || !c->mpPed || !gGame_0x40)
    {
        return true;
    }
    const Player* self = gGame_0x40->mLocalPlayer;
    static const bool sHitLog = getenv("GTA2_NET_HITLOG") != NULL;
    if (sHitLog && car->mDriver && car->mDriver->mPlayer == self)
    {
        const Sprite* s = c->mSpritePtr;
        printf("net: CARHIT tick %u: ped %d player %d state %d/%d health %d at (%.2f, %.2f)\n", Tick(), (int)c->mpPed->mId,
               c->mpPed->mPlayer ? (int)c->mpPed->mPlayer->mPlayerIdx : -1, (int)c->mpPed->mPedState1, (int)c->mpPed->mPedState2,
               (int)c->mpPed->mHealth, s ? s->mXy.x.ToFloat() : 0.0f, s ? s->mXy.y.ToFloat() : 0.0f);
    }
    auto other = [self](const Ped* p) { return p && p->mPlayer && p->mPlayer != self; };
    return !other(c->mpPed) && !other(car->mDriver);
}

// gPortSoundRequestsHook: a live frame's requests noted, then what is owed added
void LiveRequests(bool requested)
{
    const uint32_t tick = Tick();
    if (!requested)
    {
        // The vocal line is taken while the entities ask for theirs
        if (gSound_obj.mVocals.mPendingVoice)
        {
            sHeard.push_back({&gSound_obj.mVocals, 0, 0, (u32)gSound_obj.mVocals.mPendingVoice, 0, 0, tick, sound_0x68()});
        }
        return;
    }
    const size_t from = sHeard.size();
    TakeRequests(sHeard, tick);
    // the vocal line was noted before
    sHeard.erase(std::remove_if(sHeard.begin() + from, sHeard.end(), [](const SoundEvent& e) { return e.mOwner == &gSound_obj.mVocals; }),
                 sHeard.end());
    GiveOwed();
    Prune(tick);
}
} // namespace

int net::engine::gSoundsGiven;
int net::engine::gSoundStats[4];

void net::engine::SoundLogRestored(uint32_t tick)
{
    gPortSoundRequestsHook = LiveRequests;
    gPortImpactHeard = ImpactHeard;
    sSnapTick = tick;
    sRestored = true;
    if (sSettled > tick + kKeep || sSettled + kKeep < tick) // a jump
    {
        sSettled = tick;
    }
    // This restore's resimulation asks again for the ticks after it
    sWould.erase(std::remove_if(sWould.begin(), sWould.end(), [tick](const SoundEvent& e) { return e.mTick > tick; }),
                 sWould.end());
}

void net::engine::SoundLogResimStep(bool on)
{
    sResimStep = on;
}

void net::engine::SoundLogResimTick(bool applied)
{
    sound_obj& s = gSound_obj;
    gPortSoundStats.resimTicks++;
    if (!s.mbSoundInitialized || !s.mServiceEnabled || !s.mType5Idx || !gPortSoundRequestsHook)
    {
        net::engine::gSoundStats[3] += gPortSoundRequestsHook != NULL;
        gPortSoundStats.resimOff++;
        return;
    }
    // The pause as sound_obj::Service has it for this tick's world: a save holds the flag of the frame that took it, and a
    // resimulation saves its ticks again, which would carry a paused one (the results) on to every later tick
    s.mIsPaused = gGame_0x40 && (gGame_0x40->mGameState == GameState::Paused_2 || (gPortWorldStill && gPortWorldStill()));
    gPortSoundStats.resimPaused += s.mIsPaused;
    // The channels and queues mirror the mixer (RestoreGame keeps them over a restore): as they were
    const size_t from = offsetof(sound_obj, msQueueSample), to = offsetof(sound_obj, mUnused1440);
    static unsigned char sChannels[offsetof(sound_obj, mUnused1440) - offsetof(sound_obj, msQueueSample)];
    memcpy(sChannels, (unsigned char*)&s + from, to - from);
    if (!applied && !s.mIsPaused)
    {
        s.GenerateIntegerRandomNumberTable();
        s.mVocals.mFifthFrameFlag = !(s.mMFrameCounter++ % 5);
    }
    s.ClearRequestedQueue();
    // No radio, police radio, vocal line, frontend or pager: they play streams of their own, kept as they are
    std::vector<SoundObject*> off;
    for (u32 j = 0; j < s.m444CNAudioEntitiesCount; j++)
    {
        SoundObject* o = s.mAudioEntities[s.mAudioEntityOrderList[j]].mpObj;
        const s32 t = o ? o->mObjectType : 0;
        if (o && !o->mbStatus &&
            (t == SoundObjectTypeEnum::Radio_3 || t == SoundObjectTypeEnum::Vocals_10 || t == SoundObjectTypeEnum::Frontend_2 ||
             t == SoundObjectTypeEnum::Hud_Pager_C_11))
        {
            o->mbStatus = true;
            off.push_back(o);
        }
    }
    gPortSoundSide = 1;
    s.InterrogateAudioEntities();
    gPortSoundSide = 0;
    for (SoundObject* o : off)
    {
        o->mbStatus = false;
    }
    // An Apply's (a death the server carried out) is the snapshot's next tick's
    TakeRequests(sWould, applied ? Tick() + 1 : Tick());
    if (!applied)
    {
        s.IncrementAudioEntitiesAge();
    }
    memcpy((unsigned char*)&s + from, sChannels, to - from);
}

void net::engine::SoundLogNewGame()
{
    sHeard.clear();
    sWould.clear();
    sRestored = false;
    sSettled = 0;
}

void net::engine::SoundLogStop()
{
    gPortSoundRequestsHook = NULL;
    gPortImpactHeard = NULL;
    SoundLogNewGame();
}
