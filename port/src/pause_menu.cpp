#include "pause_menu.hpp"
#include "autosave.hpp"
#include "retry.hpp"
#include "Camera.hpp"
#include "Draw.hpp"
#include "Frontend.hpp"
#include "Function.hpp"
#include "Game.hpp"
#include "GameSession.hpp"
#include "Globals.hpp"
#include "Hud.hpp"
#include "InputRecorder_67F8B0.hpp"
#include "Ped.hpp"
#include "Player.hpp"
#include "TextTable.hpp"
#include "TextureCache.hpp"
#include "cSampleManager.hpp"
#include "debug.hpp"
#include "gbh_graphics.hpp"
#include "gtx_0x106C.hpp"
#include "menu.hpp"
#include "minimap.hpp"
#include "options_menu.hpp"
#include "registry.hpp"
#include "root_sound.hpp"
#include "settings.hpp"
#include "sound_obj.hpp"
#include "winmain.hpp"
#include <stdio.h>
#include <vector>

EXTERN_GLOBAL(s16, word_7064D8);
EXTERN_GLOBAL(u16, gBriefFont);
EXTERN_GLOBAL(u8, gSoundSwitchRadioCoolDown);

#if defined(GTA2_PORT_SDL)
int port_GetAltBinding(int control);
#endif

namespace PortPauseMenu
{
namespace
{
bool sOpen;

// The frontend's style, for the menu's fonts and sprites, loaded with the city (Preload).
gtx_0x106C* sStyle;
TextureCache* sTextures;

// Its palettes go after the game's in the renderer's one palette table.
s32 sPaletteBase;
T_gbh_RegisterPalette sRegisterPalette;
T_gbh_FreePalette sFreePalette;
T_gbh_RegisterTexture sRegisterTexture;

u32 CC RegisterPalette(s32 palId, DWORD* pData)
{
    return sRegisterPalette(sPaletteBase + palId, pData);
}

void CC FreePalette(s32 palId)
{
    sFreePalette(sPaletteBase + palId);
}

STexture* CC RegisterTexture(s32 width, s32 height, BYTE* pData, s32 palId, char_type flag)
{
    return sRegisterTexture(width, height, pData, sPaletteBase + palId, flag);
}

gtx_0x106C* sGameStyle;
TextureCache* sGameTextures;

void UseFrontendStyle(bool registering)
{
    sGameStyle = gGtx_0x106C;
    sGameTextures = gTextureCache_15D8;
    gGtx_0x106C = sStyle;
    gTextureCache_15D8 = sTextures;
    if (registering)
    {
        sPaletteBase = sGameTextures->mPalCount;
        sRegisterPalette = pgbh_RegisterPalette;
        sFreePalette = pgbh_FreePalette;
        sRegisterTexture = pgbh_RegisterTexture;
        pgbh_RegisterPalette = RegisterPalette;
        pgbh_FreePalette = FreePalette;
        pgbh_RegisterTexture = RegisterTexture;
    }
}

void UseGameStyle()
{
    gGtx_0x106C = sGameStyle;
    gTextureCache_15D8 = sGameTextures;
    if (pgbh_RegisterPalette == RegisterPalette)
    {
        pgbh_RegisterPalette = sRegisterPalette;
        pgbh_FreePalette = sFreePalette;
        pgbh_RegisterTexture = sRegisterTexture;
    }
}

void LoadStyle()
{
    // Loading a style sets the font globals; the game's must stay.
    s16* const pFonts[] = {&gStyleMessageFont, &gStyleTextFont,  &gStyleCarNameFont, &gStyleZoneNameFont,
                           &gStylePauseFont,   &gKanjiSmallFont, &gKanjiLargeFont};
    s16 fonts[7];
    for (s32 i = 0; i < 7; i++)
    {
        fonts[i] = *pFonts[i];
    }
    sStyle = new gtx_0x106C();
    sTextures = new TextureCache();
    UseFrontendStyle(true);
    sStyle->LoadSty("data\\fstyle.sty");
    sTextures->LoadStyleTextures();
    ConvertColourBanks();
    UseGameStyle();
    for (s32 i = 0; i < 7; i++)
    {
        *pFonts[i] = fonts[i];
    }
}

void FreeStyle()
{
    if (!sStyle)
    {
        return;
    }
    UseFrontendStyle(true);
    GTA2_DELETE_AND_NULL(sTextures);
    GTA2_DELETE_AND_NULL(sStyle);
    UseGameStyle();
}

// The frontend's menu sounds, fstyle's samples 0 to 11: a left and a right one for each result up to
// kWentBack. The game's sound bank is the level's, so they play on sample handles of their own.
SdtEntry sSoundEntries[12];
std::vector<u8> sSoundData;
s32 sSoundFirst;
// Pairs used in turn: like the frontend's, a sound plays to its end over the next ones (about a
// second), as cutting it short clicks.
const s32 kSoundPairs = 8;
HSAMPLE sSoundHandles[kSoundPairs * 2];
s32 sNextPair;

bool LoadSoundData()
{
    FILE* pSdt = fopen("data\\audio\\FSTYLE.SDT", "rb");
    FILE* pRaw = fopen("data\\audio\\FSTYLE.RAW", "rb");
    bool ok = pSdt && pRaw && fread(sSoundEntries, sizeof(SdtEntry), 12, pSdt) == 12;
    if (ok)
    {
        sSoundFirst = sSoundEntries[0].mOffset;
        s32 end = 0;
        for (const SdtEntry& entry : sSoundEntries)
        {
            sSoundFirst = entry.mOffset < sSoundFirst ? entry.mOffset : sSoundFirst;
            end = entry.mOffset + entry.mSampleLength > end ? entry.mOffset + entry.mSampleLength : end;
        }
        sSoundData.resize(end - sSoundFirst);
        ok = fseek(pRaw, sSoundFirst, SEEK_SET) == 0 && fread(sSoundData.data(), 1, sSoundData.size(), pRaw) == sSoundData.size();
    }
    if (pSdt)
    {
        fclose(pSdt);
    }
    if (pRaw)
    {
        fclose(pRaw);
    }
    if (!ok)
    {
        sSoundData.clear();
    }
    return ok;
}

void OpenSounds()
{
    if (!gSampManager.mhDriver || (sSoundData.empty() && !LoadSoundData()))
    {
        return;
    }
    for (HSAMPLE& handle : sSoundHandles)
    {
        if (!handle)
        {
            handle = AIL_allocate_sample_handle(gSampManager.mhDriver);
        }
    }
}

// Once the menu closed, as each sound ends.
void ReleaseSounds()
{
    for (HSAMPLE& handle : sSoundHandles)
    {
        if (handle && AIL_sample_status(handle) != SMP_PLAYING)
        {
            AIL_release_sample_handle(handle);
            handle = NULL;
        }
    }
}

// As the frontend plays them, at the SOUND EFFECTS volume.
void PlaySound(PortMenu::Result result)
{
    if (result < PortMenu::kMovedUp || result > PortMenu::kWentBack)
    {
        return;
    }
    const s32 volume = gRegistry.Get_Sound_Setting("SFXVol", 127);
    for (s32 i = 0; i < 2; i++)
    {
        HSAMPLE handle = sSoundHandles[sNextPair * 2 + i];
        if (!handle)
        {
            continue;
        }
        const SdtEntry& entry = sSoundEntries[(result - PortMenu::kMovedUp) * 2 + i];
        AIL_end_sample(handle);
        AIL_init_sample(handle);
        AIL_set_sample_type(handle, DIG_F_MONO_16, DIG_PCM_SIGN);
        AIL_set_sample_address(handle, &sSoundData[entry.mOffset - sSoundFirst], entry.mSampleLength);
        AIL_set_sample_playback_rate(handle, entry.mPlayBackRate);
        AIL_set_sample_volume(handle, volume);
        AIL_set_sample_pan(handle, i ? 127 : 0);
        AIL_set_sample_loop_count(handle, 1);
        AIL_start_sample(handle);
    }
    sNextPair = (sNextPair + 1) % kSoundPairs;
}

// The stations by number, as GTAudio\<n>.wav: the game's data has no names.
const wchar_t* const kStations[] = {NULL, L"HEAD RADIO", L"ROCKSTAR RADIO", L"KREZ", L"LO-FI FM", L"FUTURO FM", L"FUNAMI FM",
                                    L"LITHIUM FM", L"KING 130.7", L"OSMOSIS FM", L"HEAVENLY RADIO", L"KGBH"};

const wchar_t* StationName()
{
    const s32 station = gSound_obj.RadioStation();
    return station > 0 && station < GTA2_COUNTOF_S(kStations) ? kStations[station] : NULL;
}

void Close()
{
    sOpen = false;
    PortMenu::Close();
    if (gGame_0x40->IsPaused())
    {
        gGame_0x40->Unpause();
    }
}

bool Closes(PortMenu::Result result)
{
    return result == PortMenu::kClosed || result == PortMenu::kQuitGame || result == PortMenu::kRetryCheckpoint ||
        result == PortMenu::kLoadSave || result == PortMenu::kSaveAndQuitGame;
}

void Apply(PortMenu::Result result)
{
    // As the original's F1 and F2, with their wait between changes; the sound takes the change on its next frame.
    if (result == PortMenu::kPreviousStation || result == PortMenu::kNextStation)
    {
        const bool next = result == PortMenu::kNextStation;
        result = gSoundSwitchRadioCoolDown > 0 ? PortMenu::kNothing : (next ? PortMenu::kRaised : PortMenu::kLowered);
        gRoot_sound.CycleRadioStation(next);
    }
    PlaySound(result);
    if (Closes(result))
    {
        Close();
    }
    // A failed save leaves the player in game, SAVE FAILED on the HUD.
    if (result == PortMenu::kQuitGame || (result == PortMenu::kSaveAndQuitGame && PortAutosave::SaveToQuit()))
    {
        gGame_0x40->ExitGameNoBonus(1, GameExitType::PlayerQuit_2);
    }
    if (result == PortMenu::kRetryCheckpoint)
    {
        PortRetry::Retry();
    }
    if (result == PortMenu::kLoadSave)
    {
        PortRetry::Retry(PortMenu::SelectedSave().mFileName);
    }
}

// The original pause screen's sections, in UpdatePauseSection's order and with its checks; none in a bonus
// stage, where it shows none.
s32 StatSections(u8* pSections)
{
    if (gGameSession.IsBonusStage())
    {
        return 0;
    }
    Hud_PauseScreen& pause = gHud_2B00->mPauseScreen;
    const Hud_PauseScreen saved = pause;
    s32 count = 0;
    pause.mCurrentPauseSection = HudPauseSection::tokens_collected_6;
    do
    {
        pause.mTimer = 1;
        pause.UpdatePauseSection();
        pSections[count++] = pause.mCurrentPauseSection;
    } while (pause.mCurrentPauseSection != HudPauseSection::tokens_collected_6);
    pause = saved;
    return count;
}
} // namespace

bool IsOpen()
{
    return sOpen;
}

void Preload()
{
    if (!sStyle)
    {
        LoadStyle();
    }
    if (gSampManager.mhDriver && sSoundData.empty())
    {
        LoadSoundData();
    }
}

void Release()
{
    sOpen = false;
    PortMenu::Close();
    FreeStyle();
}

static f32 Transcript(f32 top, f32 room, f32 scale = 0, f32 left = 0, f32 menuTop = 0);

// A key one of the game's controls is bound to, first or second binding.
static bool KeyBound(s32 dik)
{
    for (s32 i = 0; i < 12; i++)
    {
        if (InputDevices::sControlDeviceType[i] == 0 && InputDevices::sPlayerControlsBinding[i] == dik)
        {
            return true;
        }
#if defined(GTA2_PORT_SDL)
        if (port_GetAltBinding(i) == dik)
        {
            return true;
        }
#endif
    }
    return false;
}

#if defined(__EMSCRIPTEN__)
// In a browser, which takes Escape to leave pointer lock or full screen, Pause and P (unless a control uses it) act as
// Escape does here.
s32 WebEscape(s32 action)
{
    if (action == DIK_PAUSE)
    {
        return DIK_ESCAPE;
    }
    return action == DIK_P && !KeyBound(DIK_P) ? DIK_ESCAPE : action;
}
#endif

// The MAP opened with its key, M: going back from it resumes the game.
static bool sMapDirect;

static void LeaveMap()
{
    if (sMapDirect)
    {
        PlaySound(PortMenu::kWentBack);
        Close();
    }
    else
    {
        Apply(PortMenu::HandleInput(PortMenu::kCancel));
    }
}

bool HandleKey(s32 action, Player* pPlayer)
{
#if defined(__EMSCRIPTEN__)
    if (!PortMenu::IsCapturing())
    {
        action = WebEscape(action);
    }
#endif
    if (!sOpen)
    {
        // M opens the MAP at once, unless a control uses it or the player is typing a chat message. The MAP needs the SDL
        // renderer.
#if defined(GTA2_PORT_SDL)
        const bool map = action == DIK_M && !KeyBound(DIK_M) && !pPlayer->mIsChatting;
#else
        const bool map = false;
#endif
        if ((action != DIK_ESCAPE && !map) || pPlayer->mShowQuitMessage || bSkip_quit_confirm || !pPlayer->IsUser())
        {
            return false;
        }
        sOpen = true;
        sMapDirect = map;
        if (!sStyle)
        {
            LoadStyle();
        }
        OpenSounds();
        PortMenu::Open();
        if (PortSettings::Retry())
        {
            s32 money;
            long long time;
            PortSaves::JobName job;
            const PortSaves::Checkpoint kind = PortSaves::GetCheckpoint(&money, &time, &job);
            PortMenu::OfferRetry(kind, money, time, job, PortRetry::JustFailed(), PortRetry::Running());
        }
        PortMenu::OfferRadio(StationName());
#if defined(GTA2_PORT_SDL)
        PortMenu::OfferMap();
#endif
        u8 sections[7];
        PortMenu::OfferStatistics(StatSections(sections));
        f32 top;
        f32 room;
        PortMenu::OnTranscript(&top, &room);
        PortMenu::OfferTranscript(Transcript(top, room));
        if (!PortSaves::Replaying() && !IsNetworkGame() && !gGameSession.IsBonusStage())
        {
            PortSaves::Entry saves[PortSaves::kMaxSaves];
            bool cities[PortSaves::kNumCities];
            PortMenu::OfferSaves(saves, PortOptionsMenu::ListSaves(NULL, saves, cities), cities);
            if (PortSettings::Autosave())
            {
                PortMenu::OfferQuitChoices(PortAutosave::QuitSaveBlocked());
            }
        }
        if (!gGame_0x40->IsPaused())
        {
            gGame_0x40->Pause();
        }
        if (map)
        {
            PortMenu::ShowMap();
        }
        return true;
    }
    if (PortMenu::IsCapturing())
    {
        return true;
    }
    // On the MAP the arrows, WASD and the sticks move it while held (PortMinimap::DrawCityBase); Page Up and Down, Q and E,
    // - and + zoom; Enter (the pad's A) sets or clears the waypoint at its middle; Space, C or Delete (X) goes back to the
    // player.
    if (PortMenu::OnMap())
    {
        switch (action)
        {
            case DIK_ESCAPE:
            case DIK_BACK:
            case DIK_M:
                LeaveMap();
                break;
            case DIK_RETURN:
                PortMinimap::ToggleCityWaypoint();
                break;
            case DIK_SPACE:
            case DIK_C:
            case DIK_DELETE:
                PortMinimap::CentreCityMap();
                break;
            case DIK_PRIOR:
            case DIK_Q:
            case DIK_MINUS:
            case DIK_SUBTRACT:
                PortMinimap::ZoomCityMap(-1);
                break;
            case DIK_NEXT:
            case DIK_E:
            case DIK_EQUALS:
            case DIK_ADD:
                PortMinimap::ZoomCityMap(1);
                break;
        }
        return true;
    }
    // W, A, S and D by position too, whatever the controls: the game's own bindings are off while the menu is open.
    const struct
    {
        s32 mDik;
        PortMenu::Input mInput;
    } kInputs[] = {{DIK_UP, PortMenu::kUp},       {DIK_DOWN, PortMenu::kDown},     {DIK_LEFT, PortMenu::kLeft},
                   {DIK_RIGHT, PortMenu::kRight}, {DIK_RETURN, PortMenu::kSelect}, {DIK_ESCAPE, PortMenu::kCancel},
                   {DIK_W, PortMenu::kUp},        {DIK_S, PortMenu::kDown},        {DIK_A, PortMenu::kLeft},
                   {DIK_D, PortMenu::kRight}};
    const s32 count = sizeof(kInputs) / sizeof(kInputs[0]);
    s32 i = 0;
    while (i < count && kInputs[i].mDik != action)
    {
        i++;
    }
    // LOAD GAME's cities: also Page Up and Page Down (the pad's LB and RB), Q and E, as in the frontend.
    const bool previousCity = action == DIK_PRIOR || action == DIK_Q;
    if (PortMenu::OnSaves() && (previousCity || action == DIK_NEXT || action == DIK_E))
    {
        Apply(PortMenu::HandleInput(previousCity ? PortMenu::kLeft : PortMenu::kRight));
        return true;
    }
    if (i == count)
    {
        return true;
    }
    Apply(PortMenu::HandleInput(kInputs[i].mInput));
    return true;
}

Fix16 Fx(f32 v)
{
    return Fix16(static_cast<s32>(v * 16384.0f), 0);
}

static bool sDrawingStats;
static f32 sStatScale;
static f32 sStatLeft;
static f32 sStatTop;

bool IsDrawingStats()
{
    return sDrawingStats;
}

f32 PlaceUi(f32 pos, bool y)
{
    return (y ? sStatTop : sStatLeft) + pos * sStatScale;
}

// One row per section from rowTop in the menu's layout, each centred as the original's pause screen draws it.
static void DrawStats(f32 scale, f32 left, f32 top, f32 rowTop, f32 rowStep)
{
    Hud_PauseScreen& pause = gHud_2B00->mPauseScreen;
    const Hud_PauseScreen saved = pause;
    u8 sections[7];
    const s32 count = StatSections(sections);
    const Fix16 hudScale = gViewCamera->mUiScale;
    gViewCamera->mUiScale = Fx(scale);
    sDrawingStats = true;
    sStatScale = scale;
    sStatLeft = left;
    for (s32 i = 0; i < count; i++)
    {
        // DrawPause puts its text at y 220.
        sStatTop = top + (rowTop + i * rowStep - 220) * scale;
        pause.mCurrentPauseSection = sections[i];
        pause.DrawPause();
    }
    sDrawingStats = false;
    gViewCamera->mUiScale = hudScale;
    pause = saved;
}

// MISSION TRANSCRIPT's height: the brief shown, then those F7 shows again (one per run of the same), stacked while they fit
// in room; with a scale, drawn by the HUD a little smaller.
static f32 Transcript(f32 top, f32 room, f32 scale, f32 left, f32 menuTop)
{
    const f32 kSize = 0.85f;
    Hud_Brief& brief = gHud_2B00->mBrief;
    const Hud_Brief saved = brief;
    const Fix16 hudScale = gViewCamera->mUiScale;
    gViewCamera->mUiScale = Fx(scale * kSize);
    sDrawingStats = scale > 0;
    sStatScale = scale * kSize;
    // DrawBrief's face spans about x 8 to 56 and y 412 to 474, its last line ends at y 480: the face's left at x 50.
    sStatLeft = left + (50 - 8 * kSize) * scale;
    f32 height = 0;
    const Hud_BriefEntry* pLast = NULL;
    Hud_BriefEntry* pEntry = saved.mCurrBriefs && saved.mCurrBriefs->mWasDisplayed ? saved.mCurrBriefs : saved.mPrevBriefs;
    for (s32 count = 0; pEntry && count < 5; pEntry = pEntry == saved.mCurrBriefs ? saved.mPrevBriefs : pEntry->mpNext)
    {
        if (pLast && !strcmp(pEntry->mBriefIdStr, pLast->mBriefIdStr) && pEntry->mCostParam == pLast->mCostParam)
        {
            continue;
        }
        brief.mCurrBriefs = pEntry;
        brief.FormatCurrentBrief();
        const f32 textTop = 480.0f - GetLineSpacingFromFontType_5D7700_inlined(gBriefFont) * brief.mNumLines;
        const f32 bottom = height + (pLast ? 14 : 0) + (480 - (textTop < 412 ? textTop : 412)) * kSize;
        if (bottom > room)
        {
            break;
        }
        sStatTop = menuTop + (top + bottom - 480 * kSize) * scale;
        if (scale > 0)
        {
            brief.mFaceVariant = 0;
            brief.DrawBrief();
        }
        height = bottom;
        pLast = pEntry;
        count++;
    }
    sDrawingStats = false;
    gViewCamera->mUiScale = hudScale;
    brief = saved;
    return height;
}

// The HUD brief's text: its right end and top on the screen; a right end of 0 without one.
static void BriefPlace(f32* pRight, f32* pTop)
{
    Hud_Brief& brief = gHud_2B00->mBrief;
    *pRight = 0;
    *pTop = 0;
    if (brief.mCurrBriefs)
    {
        PortUiCentred centred;
        *pRight = PortUiStringX(brief.mStr, Fix16(64), gBriefFont).ToFloat() +
            Frontend::GetMaxTextWidth(brief.mStr, gBriefFont) * gViewCamera->mUiScale.ToFloat();
        *pTop = PortUiY(Fix16(480 - GetLineSpacingFromFontType_5D7700_inlined(gBriefFont) * brief.mNumLines)).ToFloat();
    }
}

// The local player's position, where the camera follows it (its car's in one), in the frontend's small font in the
// safe area's bottom right corner, above the brief if it reaches there. Needs the frontend's style.
static void DrawPosition(f32 scale, f32 briefRight, f32 briefTop)
{
    const Ped* pPed = gGame_0x40->mLocalPlayer ? gGame_0x40->mLocalPlayer->GetActivePed() : NULL;
    if (!pPed)
    {
        return;
    }
    wchar_t text[48];
    swprintf(text, L"X %.2f  Y %.2f  Z %.2f", pPed->get_cam_x().ToFloat(), pPed->get_cam_y().ToFloat(), pPed->get_cam_z().ToFloat());
    const f32 margin = PortSettings::SafeArea();
    const f32 inset = 6 * scale;
    const f32 x = gViewCamera->mScreenPxWidth * (1 - margin) - inset - Frontend::GetMaxTextWidth(text, gFrontendFont7) * scale;
    f32 bottom = gViewCamera->mScreenPxHeight * (1 - margin);
    bottom = x < briefRight && briefTop < bottom ? briefTop : bottom;
    const f32 y = bottom - inset - GetLineSpacingFromFontType(gFrontendFont7) * scale;
    DrawString(text, Fx(x), Fx(y), gFrontendFont7, Fx(scale), palette_types_enum::sprites_2, 0, 0, 0);
}

// The MAP page: the city on the left, and a column on the right with the title, the key, the buttons (for touch) and the
// controls, so that nothing covers the map.
static bool sOnMap;

static void DrawMapPage(f32 scale)
{
    const f32 width = static_cast<f32>(gViewCamera->mScreenPxWidth), height = static_cast<f32>(gViewCamera->mScreenPxHeight);
    const f32 margin = PortSettings::SafeArea();
    const f32 pad = 6 * scale, iconColumn = 27 * scale, lineHeight = 14 * scale, buttonHeight = 22 * scale;
    // Every name the key may show, for a column as wide whatever the level has.
    static const wchar_t* const kKeyNames[] = {L"YOU",           L"WAYPOINT",          L"JOB TARGET", L"EASY JOB",
                                               L"MEDIUM JOB",    L"HARD JOB",          L"NEEDS MORE RESPECT",
                                               L"GANG INFORMATION", L"CHURCH - SAVE",  L"GARAGE",     L"CRANE",
                                               L"HOSPITAL",      L"POLICE STATION",    L"TRAIN STATION", L"TERRITORY"};
    const bool padMode = PortMenu::PadMode();
    const s32 kNumHints = 5;
    const wchar_t* const kHints[2][kNumHints] = {
        {L"ARROWS OR DRAG - MOVE", L"WHEEL OR Q AND E - ZOOM", L"ENTER OR CLICK - WAYPOINT", L"SPACE - BACK TO YOU", L"ESC OR M - BACK"},
        {L"LEFT STICK - MOVE", L"RIGHT STICK - ZOOM", L"A - WAYPOINT", L"X - BACK TO YOU", L"B - BACK"},
    };
    UseFrontendStyle(false);
    f32 inner = PortMenu::ScreenText(L"MAP", 0, 0, scale, false, false, false);
    for (const wchar_t* pName : kKeyNames)
    {
        const f32 w = iconColumn + PortMenu::ScreenText(pName, 0, 0, scale, true, false, false);
        inner = w > inner ? w : inner;
    }
    for (const wchar_t* pHint : kHints[padMode])
    {
        const f32 w = PortMenu::ScreenText(pHint, 0, 0, scale, true, false, false);
        inner = w > inner ? w : inner;
    }
    UseGameStyle();
    const f32 keyRight = width * (1 - margin) - 8 * scale, keyLeft = keyRight - inner;
    const f32 columnLeft = keyLeft - pad - 4 * scale;
    const f32 columnTop = height * margin + 8 * scale, columnBottom = height * (1 - margin) - 8 * scale;
    const f32 hintsTop = columnBottom - kNumHints * lineHeight;
    const f32 buttons[4] = {keyLeft, hintsTop - pad - buttonHeight, keyRight, hintsTop - pad};
    const f32 keyTop = columnTop + 24 * scale;

    if (PortMinimap::DrawCityBase(0, 0, columnLeft, height, scale * 1.5f, buttons))
    {
        LeaveMap();
        return;
    }
    PortMinimap::DrawShade(columnLeft, 0, width, height, 1.0f);

    // The districts' names on black, as the printed maps have them, under the pins and the player.
    const s32 numLabels = PortMinimap::CityMapLabels();
    f32 labelWidth[64];
    UseFrontendStyle(false);
    for (s32 i = 0; i < numLabels && i < 64; i++)
    {
        f32 x, y;
        labelWidth[i] = PortMenu::ScreenText(PortMinimap::CityMapLabel(i, &x, &y), 0, 0, scale * 0.85f, true, false, false);
    }
    UseGameStyle();
    for (s32 i = 0; i < numLabels && i < 64; i++)
    {
        f32 x, y;
        PortMinimap::CityMapLabel(i, &x, &y);
        PortMinimap::DrawShade(x - labelWidth[i] / 2 - 4 * scale, y - 7 * scale, x + labelWidth[i] / 2 + 4 * scale, y + 6 * scale, 0.85f);
    }
    UseFrontendStyle(false);
    for (s32 i = 0; i < numLabels && i < 64; i++)
    {
        f32 x, y;
        const wchar_t* pText = PortMinimap::CityMapLabel(i, &x, &y);
        PortMenu::ScreenText(pText, x - labelWidth[i] / 2, y - 5 * scale, scale * 0.85f, true);
    }
    UseGameStyle();
    PortMinimap::DrawCityMarkers();

    // The key, its rows closer together where they would reach the buttons.
    s32 rows = 0;
    while (PortMinimap::LegendName(rows))
    {
        rows++;
    }
    const f32 room = buttons[1] - pad - keyTop;
    const f32 row = rows && room / rows < 16 * scale ? room / rows : 16 * scale;
    for (s32 i = 0; i < rows; i++)
    {
        PortMinimap::DrawLegendIcon(i, keyLeft + iconColumn / 2, keyTop + i * row + row / 2, scale * 1.25f);
    }
    UseFrontendStyle(false);
    PortMenu::ScreenText(L"MAP", keyLeft, columnTop, scale, false);
    for (s32 i = 0; i < rows; i++)
    {
        PortMenu::ScreenText(PortMinimap::LegendName(i), keyLeft + iconColumn, keyTop + i * row + row / 2 - 5 * scale, scale, true);
    }
    for (s32 i = 0; i < kNumHints; i++)
    {
        PortMenu::ScreenText(kHints[padMode][i], keyLeft, hintsTop + i * lineHeight, scale, true, true);
    }
    UseGameStyle();
}

void Draw()
{
    if (!sOpen)
    {
        ReleaseSounds();
        sOnMap = false;
        return;
    }
    PlaySound(PortMenu::UpdateCapture());

    // The 640x480 layout centred on the screen, inside the safe area but without the HUD scale, 10 lower.
    const f32 scale = PortScreenUiScale().ToFloat() * (1 - 2 * PortSettings::SafeArea());
    const f32 left = (gViewCamera->mScreenPxWidth - 640.0f * scale) / 2;
    const f32 top = (gViewCamera->mScreenPxHeight - 480.0f * scale) / 2;
    const f32 menuTop = top + 10 * scale;
    PortMenu::OfferRadio(StationName());
    if (PortMenu::OnMap())
    {
        if (!sOnMap)
        {
            PortMinimap::OpenCityMap();
        }
        sOnMap = true;
        DrawMapPage(scale);
        return;
    }
    sOnMap = false;
    f32 briefRight;
    f32 briefTop;
    BriefPlace(&briefRight, &briefTop);
    UseFrontendStyle(false);
    const PortMenu::Result result = PortMenu::HandleMouse(scale, left, menuTop);
    if (Closes(result))
    {
        UseGameStyle();
        Apply(result);
        return;
    }
    Apply(result);
    PortMenu::Draw(scale, left, menuTop, true);
    DrawPosition(scale, briefRight, briefTop);
    UseGameStyle();
    f32 rowTop;
    f32 rowStep;
    if (PortMenu::OnStatistics(&rowTop, &rowStep))
    {
        DrawStats(scale, left, menuTop, rowTop, rowStep);
    }
    f32 room;
    if (PortMenu::OnTranscript(&rowTop, &room))
    {
        Transcript(rowTop, room, scale, left, menuTop);
    }
}
} // namespace PortPauseMenu
