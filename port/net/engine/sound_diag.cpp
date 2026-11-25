// port/net (port/docs/netcode.md, "Sound on a client"): a client's sound in one log line, from the game's requests to
// the mixer's output, so that a client that hears nothing can tell where its sounds stop.
#include "Source/Camera.hpp"
#include "Source/Car.hpp"
#include "Source/Game.hpp"
#include "Source/GameSession.hpp"
#include "Source/Ped.hpp"
#include "Source/Player.hpp"
#include "Source/SoundObject.hpp"
#include "Source/cSampleManager.hpp"
#include "Source/sound_obj.hpp"
#include "internal.hpp"
#include "port/src/net_hooks.hpp"
#include <math.h>
#include <stdio.h>
#include <string.h>
#include <string>

namespace
{
std::string Kinds(const u32* r)
{
    u32 all = 0;
    for (int k = 0; k < PortSoundStats::kKinds; k++)
    {
        all += r[k];
    }
    char buf[128];
    snprintf(buf,
             sizeof(buf),
             "%u (spr %u wpn %u hit %u oth %u fade %u)",
             all,
             r[PortSoundStats::kSprite],
             r[PortSoundStats::kWeapon],
             r[PortSoundStats::kCollision],
             r[PortSoundStats::kOther],
             r[PortSoundStats::kReleasing]);
    return buf;
}

// Whose camera the listener's entity follows: "p4 view", "p4 game", "p4 aux", else what it is
std::string ListenerCamera()
{
    const sound_obj& s = gSound_obj;
    if (!s.mType5Idx)
    {
        return "none";
    }
    const AudioEntity& e = s.mAudioEntities[s.mType5Idx];
    if (!e.mbUsed || !e.mpObj || e.mpObj->mObjectType != SoundObjectTypeEnum::Camera_0xBC_5)
    {
        return "entity " + std::to_string(s.mType5Idx) + " not a camera's (used " + std::to_string(e.mbUsed) + ")";
    }
    const Camera* c = e.mpObj->mpAny.pCamera_0xBC;
    Game* g = gGame_0x40;
    for (u32 i = 0; g && c && i < g->mNumPlayers; i++)
    {
        const Player* p = g->mPlayers[i];
        const char* which = !p      ? NULL :
            c == &p->mViewCamera    ? "view" :
            c == &p->mGameCamera    ? "game" :
            c == &p->mAuxGameCamera ? "aux" :
                                      NULL;
        if (which)
        {
            return "p" + std::to_string(i) + " " + which;
        }
    }
    return c ? "another camera" : "no camera";
}
// The audio entities in the order the sound processes them, by kind; muted (their object's mbStatus: skipped) and
// broken ones (unused, no object, an unknown kind) can't ask for anything
std::string Entities()
{
    const sound_obj& s = gSound_obj;
    u32 spr = 0, wpn = 0, other = 0, muted = 0, broken = 0;
    for (u32 j = 0; j < s.m444CNAudioEntitiesCount && j < 1020; j++)
    {
        const u32 idx = s.mAudioEntityOrderList[j];
        const SoundObject* o = idx < 1020 && s.mAudioEntities[idx].mbUsed ? s.mAudioEntities[idx].mpObj : NULL;
        const s32 t = o ? o->mObjectType : 0;
        if (!o || t < SoundObjectTypeEnum::Sprite_1 || t > SoundObjectTypeEnum::Hud_Pager_C_11)
        {
            broken++;
            continue;
        }
        muted += o->mbStatus;
        spr += t == SoundObjectTypeEnum::Sprite_1;
        wpn += t == SoundObjectTypeEnum::Weapon_30_7;
        other += t != SoundObjectTypeEnum::Sprite_1 && t != SoundObjectTypeEnum::Weapon_30_7;
    }
    char buf[128];
    snprintf(buf, sizeof(buf), "%u (spr %u wpn %u oth %u, muted %u, broken %u)", s.m444CNAudioEntitiesCount, spr, wpn, other, muted, broken);
    return buf;
}
} // namespace

void net::engine::PrintSoundDiag(const char* when)
{
    if (gPortNoAudio)
    {
        return;
    }
    PortSoundStats& st = gPortSoundStats;
    const sound_obj& s = gSound_obj;
    s32 samples = 0;
    for (s32 i = 0; i < 320; i++)
    {
        samples += gSampManager.GetSampleLength(i) != 0;
    }
    const char* style = gGameSession.GetStyleName();
    const char* slash = style ? strrchr(style, '\\') : NULL;
    // The listener against the local player's ped (or its car), in blocks
    float lx = s.mListenerX.ToFloat(), ly = s.mListenerY.ToFloat(), px = 0, py = 0;
    Game* g = gGame_0x40;
    const Player* local = g ? g->mLocalPlayer : NULL;
    const Ped* ped = local ? local->mPlayerPed : NULL;
    if (ped)
    {
        Car* car = ped->get_car();
        Fix16 x, y, z;
        if (car)
        {
            car->GetXYZ(&x, &y, &z);
        }
        else
        {
            x = ped->get_cam_x();
            y = ped->get_cam_y();
        }
        px = x.ToFloat();
        py = y.ToFloat();
    }
    PortMixerStats m;
    port_MixerStats(&m);
    printf("net: %s, sound: asked %s live, %s resim; unheard full %u/%u far %u/%u vol0 %u/%u; started %u (refused %u bad %u rate0 %u), "
           "busy <=%u of %u; %u frames (paused %u, no listener %u); entities %s (+%u -%u, full %u, 2nd camera %u); on %d svc %d 3d %d "
           "vol %u, bank %s %s %d; listener %.1f,%.1f player %.1f,%.1f (%.1f) camera %s (local p%d); mixer %s %s%s: %u callbacks, "
           "%u starts (%u empty), voices <=%u, streams %u, peak %.2f\n",
           when,
           Kinds(st.requests[0]).c_str(),
           Kinds(st.requests[1]).c_str(),
           st.queueFull[0],
           st.queueFull[1],
           st.far[0],
           st.far[1],
           st.silent[0],
           st.silent[1],
           st.started,
           st.refused,
           st.badSample,
           st.noRate,
           st.busyMax,
           s.mnActiveSamples,
           st.frames,
           st.pausedFrames,
           st.noListenerFrames,
           Entities().c_str(),
           st.entityAdds,
           st.entityFrees,
           st.addFails,
           st.cameraRefused,
           s.mbSoundInitialized,
           s.mServiceEnabled,
           s.mB3dSound,
           s.mSfxVol,
           slash     ? slash + 1 :
               style ? style :
                       "-",
           gSampManager.PortBankLoaded() ? "loaded" : "NOT loaded",
           samples,
           lx,
           ly,
           px,
           py,
           ped ? hypotf(lx - px, ly - py) : -1.0f,
           ListenerCamera().c_str(),
           g ? (int)g->mLocalPlayerIdx : -1,
           m.driver ? m.driver : "-",
           m.open ? "open" : "CLOSED",
           m.paused ? " PAUSED" : "",
           m.callbacks,
           m.starts,
           m.emptyStarts,
           m.voicesMax,
           m.streams,
           m.peak);
    st = PortSoundStats();
}
