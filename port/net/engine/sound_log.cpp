// port/net (port/docs/netcode.md, "Sound on a client"): a client's sound log. A resimulated tick runs the sound service's entities
// without the mixer (SoundLogResimTick): what each would have asked for is noted, and its progress (entity ages, the per-car
// SoundPosEntry, the frame counter) moves on with the world. The live ticks note what they asked for. Once a snapshot
// settles a tick, its one-shot requests that no live tick had (same owner and sound, within 2 ticks and a block) are
// added to the next live frame's, each once.
#include "internal.hpp"
#include "Source/Car.hpp"
#include "Source/CollisionSoundQueue.hpp"
#include "Source/Object_5C.hpp"
#include "Source/Ped.hpp"
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
                // draws; an object's sample), the voice line for the vocals, 0 for the collision queue's impacts (one by place)
    s32 mX, mY; // in blocks: the collision queue's events share one owner
    uint32_t mTick;
    sound_0x68 mSample;
};
std::vector<SoundEvent> sHeard; // asked for by the live frames, or added as owed
std::vector<SoundEvent> sWould; // asked for by resimulated ticks, the newest resimulation's of each tick
uint32_t sSnapTick; // the last restore's, as the game's cycle
uint32_t sCycleOffset; // a snapshot's tick less the game's cycle at it
uint32_t sSettled; // the newest tick whose requests were given
bool sRestored;
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
        u32 sound = e.mpObj->mObjectType == SoundObjectTypeEnum::CollisionSoundQueue_6 ? 0 : r.mSampleIndex;
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

// The settled ticks' requests no live frame had, into this frame's queue (the vocal line into its slot)
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
        if (Heard(w))
        {
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
            if (!gSound_obj.mVocals.mPendingVoice)
            {
                gSound_obj.mVocals.mPendingVoice = (s32)w.mSound;
            }
            continue;
        }
        const int ent = EntityOf(w);
        if (ent > 0)
        {
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

void net::engine::SoundLogWorldAt(uint32_t tick)
{
    sCycleOffset = tick - Tick();
}

void net::engine::SoundLogRestored(uint32_t tick)
{
    tick -= sCycleOffset;
    gPortSoundRequestsHook = LiveRequests;
    sSnapTick = tick;
    sRestored = true;
    if (sSettled > tick + kKeep || sSettled + kKeep < tick) // a new game, or a jump
    {
        sSettled = tick;
    }
    // This restore's resimulation asks again for the ticks after it
    sWould.erase(std::remove_if(sWould.begin(), sWould.end(), [tick](const SoundEvent& e) { return e.mTick > tick; }),
                 sWould.end());
}

void net::engine::SoundLogResimTick(bool applied)
{
    sound_obj& s = gSound_obj;
    if (!s.mbSoundInitialized || !s.mServiceEnabled || !s.mType5Idx || !gPortSoundRequestsHook)
    {
        return;
    }
    // The channels and queues mirror the mixer (RestoreGame keeps them over a restore): as they were
    const size_t from = offsetof(sound_obj, msQueueSample), to = offsetof(sound_obj, mUnused1440);
    static unsigned char sChannels[offsetof(sound_obj, mUnused1440) - offsetof(sound_obj, msQueueSample)];
    memcpy(sChannels, (unsigned char*)&s + from, to - from);
    if (!applied)
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
    s.InterrogateAudioEntities();
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

void net::engine::SoundLogStop()
{
    gPortSoundRequestsHook = NULL;
    sHeard.clear();
    sWould.clear();
    sRestored = false;
    sSettled = 0;
    sCycleOffset = 0;
}
