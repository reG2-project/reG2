#include "retry.hpp"
#include "Draw.hpp"
#include "Frontend.hpp"
#include "Game.hpp"
#include "Gang.hpp"
#include "GameSession.hpp"
#include "Globals.hpp"
#include "Camera.hpp"
#include "Hud.hpp"
#include "Ped.hpp"
#include "Player.hpp"
#include "ScriptManager.hpp"
#include "ScriptThread.hpp"
#include "gtx_0x106C.hpp"
#include "map_0x370.hpp"
#include "menu.hpp"
#include "pause_menu.hpp"
#include "prompts.hpp"
#include "settings.hpp"
#include "winmain.hpp"
#include <math.h>
#include <stdio.h>
#include <string.h>

#if defined(GTA2_PORT_SDL)
#include <SDL3/SDL_log.h>
#endif

namespace PortRetry
{
namespace
{
// At 30 steps a second, not counting the time the player is wasted or busted.
const s32 kShowSteps = 240;

// The job or frenzy the checkpoint was taken before, while it runs, and its passed counter then; the last one over.
PortSaves::Checkpoint sRunning;
PortSaves::Checkpoint sEnded;
s32 sPassedBefore;
s32 sLastMission;
bool sLastFrenzy;
s32 sShown;
bool sRetrying;
char_type sRetryFile[64];
// A checkpoint was just written before a job, whose script is not loaded yet.
bool sJobToName;
// LoadMissionScript loads that many base pointers of a job's script.
const u32 kJobBasePointers = 0xC00 / sizeof(u16);
// A checkpoint before a job the script launched itself (a final job, as the job before it ended) loads with the
// script past that point: its LAUNCH_MISSION runs again once the level's script is in its main loop (Residential's
// and Industrial's set up a command a step, for about 130 and 100 steps).
u16 sRelaunch;
s32 sRelaunchIn;
const s32 kRelaunchSteps = 150;

// The player's last steady spots, one a second over the last 12 seconds or so. A checkpoint puts him back
// at one a few steps from what started the job or frenzy, so that it starts again only if he goes back to it.
struct Sample
{
    PortSaves::Spot mSpot;
    u32 mStep;
};
const s32 kNumSamples = 12;
const u32 kSampleSteps = 30;
const f32 kMinTriggerDistance = 2;
// FRENZY RESPAWN: a failed frenzy's car, parked again once no car is that close to its spot and the spot is out of view,
// as creating it moves the cars it overlaps to a road and removes the peds (Sprite::ResolveCollisionWithCarPedOrObject).
const f32 kClearCarSpot = 3;
// The START_BASIC_KF of each failed car frenzy waiting for that, by its mBasicKf slot.
u16 sCarToPark[GTA2_COUNTOF(gScriptManager->mBasicKf)];
// Further in a second: a teleport, the spots before it are of no use.
const f32 kMaxSampleDistance = 20;
// Cars catch fire from there (Car::AccumulateDamage).
const s16 kBurningDamage = 16000;
Sample sSamples[kNumSamples];
s32 sNumSamples;
u32 sSteps;

void Log(const char* pWhat)
{
#if defined(GTA2_PORT_SDL)
    SDL_Log("Retry: %s", pWhat);
#else
    (void)pWhat;
#endif
}

bool InGame()
{
    return !PortSaves::Replaying() && gGame_0x40 && gScriptManager && !IsNetworkGame() && !gGameSession.IsBonusStage();
}

bool Enabled()
{
    return PortSettings::Retry() && InGame();
}

s32 PassedCount(PortSaves::Checkpoint kind)
{
    const s32* pCounter = kind == PortSaves::kBeforeJob ? gScriptManager->mPassedFlag : gScriptManager->mSecretsPassed;
    return pCounter ? *pCounter : 0;
}

void End(bool passed)
{
    sEnded = sRunning;
    sRunning = PortSaves::kNoCheckpoint;
    if (passed)
    {
        Log("passed");
        PortSaves::DeleteCheckpoint();
    }
    else if (PortSaves::GetCheckpoint() != PortSaves::kNoCheckpoint)
    {
        Log("failed");
        sShown = kShowSteps;
    }
}

f32 Distance(const PortSaves::Spot& a, const PortSaves::Spot& b)
{
    const f32 dx = a.mX.ToFloat() - b.mX.ToFloat();
    const f32 dy = a.mY.ToFloat() - b.mY.ToFloat();
    return sqrtf(dx * dx + dy * dy);
}

// On the top of the map's ground there, not water; a road for a car.
bool OnGround(const PortSaves::Spot& spot)
{
    s32 z;
    const gmp_block_info* pBlock = gMap_0x370->FindHighestBlockForCoord(spot.mX.ToInt(), spot.mY.ToInt(), &z);
    if (!pBlock || IS_AIR_TYPE(pBlock->mSlopeType) || (spot.mDriving && GET_BLOCK_TYPE(pBlock->mSlopeType) != ROAD) ||
        gGtx_0x106C->GetSpecIgnoringRoadJunction(pBlock->mLid & 0x3FF) == tile_spec::water)
    {
        return false;
    }
    return fabsf(gMap_0x370->FindGroundZForCoord(spot.mX, spot.mY).ToFloat() - spot.mZ.ToFloat()) < 0.125f;
}

// Alive, not burning, walking or standing on the ground, or driving a car that is not burning on a road.
bool Steady(const PortSaves::Spot& spot)
{
    Player* pPlayer = gGame_0x40->mLocalPlayer;
    Ped* pPed = pPlayer ? pPlayer->mPlayerPed : NULL;
    if (!pPed || pPlayer->mWastedOrBusted || pPed->get_health() <= 0 || pPed->IsOnFire() || !OnGround(spot))
    {
        return false;
    }
    if (spot.mDriving)
    {
        return spot.mCarDamage < kBurningDamage;
    }
    const s32 state1 = pPed->mPedState1;
    const s32 state2 = pPed->mPedState2;
    return !pPed->mCar && (state1 == ped_state_1::walking_0 || state1 == ped_state_1::standing_still_7) &&
        (state2 == ped_state_2::ped2_walking_0 || state2 == ped_state_2::ped2_staying_14);
}

void TakeSample()
{
    Player* pPlayer = gGame_0x40->mLocalPlayer;
    if (!pPlayer || pPlayer->mWastedOrBusted)
    {
        sNumSamples = 0;
        return;
    }
    PortSaves::Spot spot;
    PortSaves::GetSpot(&spot);
    if (!Steady(spot))
    {
        return;
    }
    if (sNumSamples && Distance(sSamples[(sNumSamples - 1) % kNumSamples].mSpot, spot) > kMaxSampleDistance)
    {
        sNumSamples = 0;
    }
    sSamples[sNumSamples % kNumSamples] = {spot, sSteps};
    sNumSamples++;
}

// SpotAway, else where the player is.
PortSaves::Spot RetrySpot()
{
    PortSaves::Spot spot;
    if (SpotAway(&spot))
    {
        Log("put back a few steps away");
        return spot;
    }
    PortSaves::GetSpot(&spot);
    return spot;
}

// As the script runs it, in a new thread and from a copy of the command with no next one: the thread ends with the
// job instead of going on with what launched it (Residential's and Industrial's main loops).
void Relaunch()
{
    const SCR_POINTER* pCmd = static_cast<const SCR_POINTER*>(gScriptManager->GetBasePointer(sRelaunch));
    str_table_entry* pName = pCmd && pCmd->mType == SCRCMD_LAUNCH_MISSION ? gScriptManager->FindStringById(pCmd->mIndex) : NULL;
    sRelaunch = 0;
    if (!pName || !gScriptManager->mMissionFlag || *gScriptManager->mMissionFlag)
    {
        return;
    }
    Log("the job launched again");
    SCR_CMD_HEADER launch = *pCmd;
    launch.mCmdNext = -1;
    gScriptManager->SpawnThread(launch.mCmdThis, 0)->launch_mission(&launch, pName->get_name());
}

void ParkFrenzyCars()
{
    for (u32 i = 0; i < GTA2_COUNTOF(sCarToPark); i++)
    {
        if (!sCarToPark[i])
        {
            continue;
        }
        const SCR_START_BASIC_KF_TEMPLATE* pFrenzy =
            static_cast<const SCR_START_BASIC_KF_TEMPLATE*>(gScriptManager->GetBasePointer(sCarToPark[i]));
        SCR_POINTER* pIcon = static_cast<SCR_POINTER*>(gScriptManager->GetBasePointer(pFrenzy->mObjname));
        const Fix16_Vec& spot = reinterpret_cast<SCR_CAR_DATA_DEC*>(pIcon)->mPos;
        Fix16_Rect view;
        view.ComputeCollisionPrism(spot.x, spot.y, Fix16(2 * kClearCarSpot), spot.z);
        bool clear = !gGame_0x40->IsRectVisibleToAnyPlayer(&view);
        for (Car* pCar = gCar_BC_Pool->GetFirstCar_420E50(); pCar && clear; pCar = pCar->mpNext)
        {
            if (pCar->mCarSprite)
            {
                const f32 dx = pCar->mCarSprite->GetXPos().ToFloat() - spot.x.ToFloat();
                const f32 dy = pCar->mCarSprite->GetYPos().ToFloat() - spot.y.ToFloat();
                clear = dx * dx + dy * dy > kClearCarSpot * kClearCarSpot;
            }
        }
        if (clear)
        {
            ScriptThread thread;
            thread.SCRCMD_PARKED_CAR_DECSET(pIcon);
            thread.EnableThread(pFrenzy->mTriggername);
            sCarToPark[i] = 0;
            Log("frenzy car back");
        }
    }
}
} // namespace

bool SpotAway(PortSaves::Spot* pSpot)
{
    PortSaves::Spot here;
    PortSaves::GetSpot(&here);
    for (s32 i = sNumSamples - 1; i >= 0 && i >= sNumSamples - kNumSamples; i--)
    {
        const Sample& sample = sSamples[i % kNumSamples];
        if (sSteps - sample.mStep <= kNumSamples * kSampleSteps && Distance(sample.mSpot, here) >= kMinTriggerDistance)
        {
            *pSpot = sample.mSpot;
            return true;
        }
    }
    return false;
}

void Start()
{
    sRunning = PortSaves::kNoCheckpoint;
    sLastMission = -1;
    sLastFrenzy = false;
    sShown = 0;
    sRetrying = false;
    sNumSamples = 0;
    sRelaunch = PortSaves::LoadedLaunch();
    sRelaunchIn = kRelaunchSteps;
    memset(sCarToPark, 0, sizeof(sCarToPark));
    PortSaves::RefreshCheckpoint();
    if (gScriptManager && !PortSaves::Replaying() &&
        (!gScriptManager->IsLoadedFromSave() ||
         (PortSaves::LoadedCheckpoint() == PortSaves::kNoCheckpoint && PortSaves::OlderThanCheckpoint(gGameSession.GetSaveName()))) &&
        !IsNetworkGame() && !gGameSession.IsBonusStage())
    {
        Log("a new game or an older save: no checkpoint");
        PortSaves::DeleteCheckpoint();
    }
}

void BeforeStart(PortSaves::Checkpoint kind, const SCR_CMD_HEADER* pCmd)
{
    // The script launches the Downtown tutorial itself before any job is passed: it is no job to retry.
    if (!Enabled() || !gScriptManager->mMissionFlag || *gScriptManager->mMissionFlag || gScriptManager->IsKillFrenzyActive() ||
        (pCmd && pCmd->mType == SCRCMD_LAUNCH_MISSION && !PassedCount(kind)))
    {
        return;
    }
    Player* pPlayer = gGame_0x40->mLocalPlayer;
    Ped* pPed = pPlayer ? pPlayer->mPlayerPed : NULL;
    if (!pPed || pPlayer->mWastedOrBusted || pPed->get_health() <= 0)
    {
        return;
    }
    Log(kind == PortSaves::kBeforeJob ? "checkpoint before a job" : "checkpoint before a frenzy");
    PortSaves::WriteCheckpoint(kind, RetrySpot());
    sJobToName = kind == PortSaves::kBeforeJob;
    sRunning = kind;
    sPassedBefore = PassedCount(kind);
    sShown = 0;
}

void JobLoaded(const SCR_CMD_HEADER* pCmd, u16 base)
{
    if (!sJobToName)
    {
        return;
    }
    sJobToName = false;
    PortSaves::JobName job = {};
    // Jobs from a phone: the gang the phone's command names. The others (tutorial, final jobs) have none.
    if (pCmd->mType == SCRCMD_PHONE_TEMPLATE || pCmd->mType == SCRCMD_DO_EASY_PHONE)
    {
        const SCR_PHONE_TEMPLATE* pPhone = static_cast<const SCR_PHONE_TEMPLATE*>(pCmd);
        str_table_entry* pName = gScriptManager->FindStringById(pPhone->mGangname);
        const Gang* pGang = pName ? gGangPool_CA8->FindGangByName(pName->get_name()) : NULL;
        job.mGang = pGang ? pGang->mArrowColour : 0;
    }
    job.mLaunch = pCmd->mType == SCRCMD_LAUNCH_MISSION ? pCmd->mCmdThis : 0;
    // The title: in the game's scripts, the job's first DISPLAY_MESSAGE of type 0 (1 and 2 are JOB COMPLETE and
    // JOB FAILED).
    const u32 end = base + kJobBasePointers;
    for (u32 i = base; i < end && i < GTA2_COUNTOF(gScriptManager->mBasePointers) && !job.mTitle; i++)
    {
        const SCR_TWO_PARAMS* pMessage = static_cast<const SCR_TWO_PARAMS*>(gScriptManager->GetBasePointer(static_cast<u16>(i)));
        if (pMessage && pMessage->mType == SCRCMD_DISPLAY_MESSAGE && pMessage->mUnsigned2 == 0)
        {
            job.mTitle = pMessage->mUnsigned1;
        }
    }
#if defined(GTA2_PORT_SDL)
    SDL_Log("Retry: job title %u, gang %u", job.mTitle, job.mGang);
#endif
    PortSaves::SetCheckpointJob(job);
}

void AfterStep()
{
    if (!gGame_0x40 || !gScriptManager || PortSaves::Replaying())
    {
        return;
    }
    Player* pPlayer = gGame_0x40->mLocalPlayer;
    if (sShown > 0 && pPlayer && !pPlayer->mWastedOrBusted)
    {
        sShown--;
    }
    const s32* pMission = gScriptManager->mMissionFlag;
    const s32 mission = pMission ? (*pMission != 0) : -1;
    const bool frenzy = gScriptManager->IsKillFrenzyActive();
    if ((sRunning == PortSaves::kBeforeJob && sLastMission == 1 && mission == 0) ||
        (sRunning == PortSaves::kBeforeFrenzy && sLastFrenzy && !frenzy))
    {
        End(PassedCount(sRunning) > sPassedBefore);
    }
    // Residential's and Industrial's final jobs count the pass a few steps after clearing the mission flag.
    if (sShown > 0 && PassedCount(sEnded) > sPassedBefore)
    {
        sShown = 0;
        End(true);
    }
    sLastMission = mission;
    sLastFrenzy = frenzy;
    if (InGame() && ++sSteps % kSampleSteps == 0)
    {
        if (PortSettings::Retry() || PortSettings::Autosave())
        {
            TakeSample();
        }
        if (PortSettings::KfRespawn())
        {
            ParkFrenzyCars();
        }
    }
    if (sRelaunch && --sRelaunchIn <= 0)
    {
        Relaunch();
    }
}

bool FrenzyFailed(u16 doCmd)
{
    // The template's START_BASIC_KF and START_BONUS come just before its DO_BASIC_KF in the three cities' scripts.
    const u16 start = doCmd - 2;
    const SCR_START_BASIC_KF_TEMPLATE* pFrenzy = static_cast<const SCR_START_BASIC_KF_TEMPLATE*>(gScriptManager->GetBasePointer(start));
    if (!PortSettings::KfRespawn() || !InGame() || pFrenzy->mType != SCRCMD_START_BASIC_KF)
    {
        return false;
    }
    // A new skull as the level's start creates it (ScriptThread::ExecOpCode). The old car, while it is still the frenzy's,
    // locks the player out and despawns as traffic does; ParkFrenzyCars parks a new one.
    SCR_POINTER* pIcon = static_cast<SCR_POINTER*>(gScriptManager->GetBasePointer(pFrenzy->mObjname));
    for (u32 i = 0; i < GTA2_COUNTOF(gScriptManager->mBasicKf); i++)
    {
        if (gScriptManager->mBasicKf[i] == start)
        {
            gGameSave.mCarAndScriptData.mStartedBasicKfMask &= ~(1 << i);
            sCarToPark[i] = pIcon->mType == SCRCMD_PARKED_CAR_DECSET_3D ? start : 0;
        }
    }
    ScriptThread thread;
    if (pIcon->mType != SCRCMD_PARKED_CAR_DECSET_3D)
    {
        thread.SCRCMD_OBJ_DECSET_2D_3D(reinterpret_cast<SCR_OBJ_DATA*>(pIcon), pIcon);
        thread.EnableThread(pFrenzy->mTriggername);
    }
    else
    {
        // A burnt out car is freed (Car::PoolDeallocate) and its slot may hold another car since.
        Car* pOld = pIcon->mCar;
        if (!pOld || !pOld->mCarSprite || pOld->mCarInfoIdx != reinterpret_cast<SCR_CAR_DATA_DEC*>(pIcon)->mCarId)
        {
            pIcon->mCar = NULL;
        }
        else if (pOld->mDoorLock != Car_Door_Lock::locked_permanently_4)
        {
            pOld->mDoorLock = Car_Door_Lock::lockout_player_5;
        }
        thread.DeallocOrDeleteItem(pFrenzy->mObjname);
    }
    Log("frenzy back");
    return true;
}

bool LastSteadySpot(PortSaves::Spot* pSpot)
{
    if (!sNumSamples)
    {
        return false;
    }
    *pSpot = sSamples[(sNumSamples - 1) % kNumSamples].mSpot;
    return true;
}

void Draw()
{
    if (!sShown || !PortSettings::Retry() || PortPauseMenu::IsOpen() || PortSaves::GetCheckpoint() == PortSaves::kNoCheckpoint)
    {
        return;
    }
    // Under SAVING, above a job's titles.
    const s32 kTop = 78;
    const PortPrompts::Glyph glyph = PortMenu::PadMode() ? PortPrompts::kPadStart : PortPrompts::kKeyEscape;
    wchar_t text[64];
    if (!PortPrompts::Available())
    {
        swprintf(text, PortSettings::Translate(L"%ls: RETRY"), PortSettings::Translate(L"ESC"));
        DrawStringScaled(text, Fix16(320 - Frontend::GetMaxTextWidth(text, gDebugFont) / 2), Fix16(kTop), gDebugFont);
        return;
    }
    // Escape's or Start's picture, then RETRY, centred; the picture placed as the HUD places its layout.
    wcscpy(text, PortSettings::Translate(L"RETRY"));
    const f32 ui = gViewCamera->mUiScale.ToFloat();
    const f32 size = PortPrompts::Size(16 * ui);
    const f32 glyphWidth = PortPrompts::Width(glyph, size) / ui;
    const f32 kGap = 5;
    const s32 kYellow = 0xAD9A52; // the text's, for a white picture
    const s32 left = static_cast<s32>(320 - (glyphWidth + kGap + Frontend::GetMaxTextWidth(text, gDebugFont)) / 2);
    PortPrompts::Draw(glyph, PortUiX(Fix16(left)).ToFloat(), PortUiY(Fix16(kTop)).ToFloat() + 11 * ui, size, kYellow, false);
    DrawStringScaled(text, Fix16(static_cast<s32>(left + glyphWidth + kGap)), Fix16(kTop), gDebugFont);
}

bool JustFailed()
{
    return sShown > 0;
}

void Retry(const char_type* pFileName)
{
    Log(pFileName ? pFileName : "chosen");
    if (pFileName)
    {
        snprintf(sRetryFile, sizeof(sRetryFile), "%s", pFileName);
    }
    else
    {
        PortSaves::CheckpointName(gGameSession.GetPlySlotIdx(), sRetryFile, sizeof(sRetryFile));
    }
    sRetrying = true;
    gGame_0x40->ExitGameNoBonus(1, GameExitType::PlayerQuit_2);
}

bool Running()
{
    return sRunning != PortSaves::kNoCheckpoint;
}

bool Retrying()
{
    return sRetrying;
}

bool TakeRetry(char_type* pFileName, size_t size)
{
    snprintf(pFileName, size, "%s", sRetryFile);
    const bool retrying = sRetrying;
    sRetrying = false;
    return retrying;
}
} // namespace PortRetry
