#include "options_menu.hpp"
#include "Function.hpp"
#include "GameSession.hpp"
#include "Draw.hpp"
#include "Globals.hpp"
#include "PlyDat.hpp"
#include "SoundObject.hpp"
#include "TextTable.hpp"
#include "menu.hpp"
#include "reg2_version.h"
#include "retry.hpp"
#include "settings.hpp"
#include "welcome.hpp"
#include <ctype.h>
#include <stdio.h>
#include <string.h>
#if defined(GTA2_UPDATE_CHECK)
#include "registry.hpp"
#include <SDL3/SDL.h>
#endif
#include "net_hooks.hpp"

// No std::wstring: with -fshort-wchar its traits would call the C library's 4-byte wide functions.

EXTERN_GLOBAL(SoundObject, snd1);

#if defined(GTA2_PORT_SDL)
bool port_TakeMouse(float* pX, float* pY, int* pLeft, int* pRight, int* pWheel);
#endif

namespace PortOptionsMenu
{
namespace
{
const u16 kMenuPage = 12;
bool sShown;
// The page and option the list of saves goes back to, and the save chosen in it.
u16 sSavesReturnPage;
u16 sSavesReturnOption;
Frontend* sListing;
char_type sResumeFile[64];
// The frontend's levels, for the pause menu's LOAD GAME, when the frontend is gone.
MainBlockStrings sLevels[PortSaves::kNumCities][4];
u8 sNumCities;

// The delete player dialog's type for a new game that replaces an autosave, past the original's 1, and its
// choices, CANCEL first.
const u8 kReplaceDialog = 2;
const u16 kContinueY = 210;
const u16 kCancelY = 230;
// YOUR <CITY> AUTOSAVE WILL BE REPLACED on the screen's right half under the logo, then CONTINUE and CANCEL.
const s32 kQuestionLeft = 300;
const s32 kQuestionTop = 160;
const s32 kQuestionWidth = 320;
const s32 kChoiceStep = 20;
// Before the original pages took the input, put back if the new game is not confirmed; and the boot's screen.
GameSession sSessionBefore;
char_type sScreenBefore;
char_type sBootScreen;

s32 Wrap(s32 i, s32 n)
{
    return (i % n + n) % n;
}

// The frontend fonts have no hyphen and no lower case.
void Widen(const char* pText, wchar_t* pOut)
{
    for (; *pText; pText++)
    {
        *pOut++ = static_cast<wchar_t>(*pText == '-' ? ' ' : toupper(static_cast<unsigned char>(*pText)));
    }
    *pOut = 0;
}

#if defined(GTA2_UPDATE_CHECK)
// x.y.z as one number, 0 if the text does not start with one.
s32 VersionNumber(const char* pText)
{
    s32 x, y, z;
    return sscanf(pText, "%d.%d.%d", &x, &y, &z) == 3 ? x * 1000000 + y * 1000 + z : 0;
}

// The latest release's version number, -1 while it is fetched, and the day it is fetched on.
SDL_AtomicInt sLatest;
s32 sFetchDay = -1;

// With curl, else wget. GTA2_UPDATE_URL replaces GitHub's API in tests.
int SDLCALL FetchLatest(void*)
{
    const char* url = SDL_getenv("GTA2_UPDATE_URL") ? SDL_getenv("GTA2_UPDATE_URL") : "https://api.github.com/repos/reG2-project/reG2/releases/latest";
    const char* commands[][6] = {{"curl", "-fsSL", "--max-time", "10", url, NULL}, {"wget", "-qO-", "--timeout=10", url, NULL}};
    char* pOut = NULL;
    for (s32 i = 0; i < 2 && !pOut; i++)
    {
        const SDL_PropertiesID props = SDL_CreateProperties();
        SDL_SetPointerProperty(props, SDL_PROP_PROCESS_CREATE_ARGS_POINTER, commands[i]);
        SDL_SetNumberProperty(props, SDL_PROP_PROCESS_CREATE_STDOUT_NUMBER, SDL_PROCESS_STDIO_APP);
#if defined(SDL_PLATFORM_WINDOWS)
        // Else Windows opens a console window for curl.
        SDL_SetBooleanProperty(props, SDL_PROP_PROCESS_CREATE_BACKGROUND_BOOLEAN, true);
#endif
        SDL_Process* pProcess = SDL_CreateProcessWithProperties(props);
        SDL_DestroyProperties(props);
        pOut = pProcess ? static_cast<char*>(SDL_ReadProcess(pProcess, NULL, NULL)) : NULL;
        SDL_DestroyProcess(pProcess);
    }
    const char* pTag = pOut ? SDL_strstr(pOut, "\"tag_name\"") : NULL;
    pTag = pTag ? SDL_strstr(pTag + 10, "\"v") : NULL;
    SDL_SetAtomicInt(&sLatest, pTag ? VersionNumber(pTag + 2) : 0);
    SDL_free(pOut);
    return 0;
}

// At most once a day: the last check's day and result are kept in the registry.
void StartUpdateCheck()
{
    SDL_Time now = 0;
    SDL_GetCurrentTime(&now);
    const s32 today = static_cast<s32>(now / (86400 * SDL_NS_PER_SECOND));
    if (PortSettings::UpdateCheck() && gRegistry.Get_Screen_Setting("update_day", 0) == today)
    {
        SDL_SetAtomicInt(&sLatest, gRegistry.Get_Screen_Setting("update_latest", 0));
    }
    else if (PortSettings::UpdateCheck())
    {
        SDL_SetAtomicInt(&sLatest, -1);
        sFetchDay = today;
        SDL_DetachThread(SDL_CreateThread(FetchLatest, "update check", NULL));
    }
}

// A newer release's version number, else 0.
s32 NewerRelease()
{
    const s32 latest = SDL_GetAtomicInt(&sLatest);
    if (sFetchDay >= 0 && latest >= 0)
    {
        gRegistry.Set_Screen_Setting("update_day", sFetchDay);
        gRegistry.Set_Screen_Setting("update_latest", latest);
        sFetchDay = -1;
    }
    return latest > VersionNumber(GTA2_VERSION) ? latest : 0;
}
#endif

PortMenu::Result HandleInput(Frontend* pFrontend)
{
    if (PortMenu::IsCapturing())
    {
        const PortMenu::Result result = PortMenu::UpdateCapture();
        return result != PortMenu::kNothing ? result : PortMenu::HandleMouse(1.0f, 0.0f, 0.0f);
    }
    const struct
    {
        bool Frontend::*mPressed;
        PortMenu::Input mInput;
    } kInputs[] = {{&Frontend::mReturnPressed, PortMenu::kSelect}, {&Frontend::mEscapePressed, PortMenu::kCancel},
                   {&Frontend::mUpPressed, PortMenu::kUp},          {&Frontend::mDownPressed, PortMenu::kDown},
                   {&Frontend::mLeftPressed, PortMenu::kLeft},      {&Frontend::mRightPressed, PortMenu::kRight}};
    for (const auto& input : kInputs)
    {
        if (pFrontend->*input.mPressed)
        {
            return PortMenu::HandleInput(input.mInput);
        }
    }
    return PortMenu::HandleMouse(1.0f, 0.0f, 0.0f);
}

#if defined(GTA2_PORT_SDL)
bool Inside(f32 x, f32 y, f32 left, f32 top, f32 right, f32 bottom)
{
    return x >= left && x < right && y >= top && y < bottom;
}

// Whether the pointer is on a line of text drawn at left, top in font: 20 high, the pages' line step, centred on the
// font's line, where its letters sit, so the gap between two lines is split halfway.
bool OnText(f32 x, f32 y, f32 left, f32 top, f32 width, u16 font)
{
    const f32 middle = top + GetLineSpacingFromFontType(font) / 2.0f;
    return Inside(x, y, left - 4.0f, middle - 10.0f, left + width + 4.0f, middle + 10.0f);
}

// The original pages with the mouse, as their keys: pointing at an option selects it, a click enters
// it, the right button goes back, the wheel and the arrows beside an option change it.
void OriginalPageMouse(Frontend* pFrontend)
{
    float x;
    float y;
    int left;
    int right;
    int wheel;
    const bool moved = port_TakeMouse(&x, &y, &left, &right, &wheel);
    if (right)
    {
        pFrontend->mEscapePressed = true;
        return;
    }
#if defined(GTA2_UPDATE_CHECK)
    if (left && pFrontend->mCurrentPageIdx == MENUPAGE_START_MENU && NewerRelease() && Inside(x, y, 300.0f, 450.0f, 640.0f, 480.0f))
    {
        SDL_OpenURL("https://github.com/reG2-project/reG2/releases/latest");
        return;
    }
#endif
    MenuPage* pPage = &pFrontend->mMenuPagesArray[pFrontend->mCurrentPageIdx];
    for (s32 i = 0; i < pPage->mNumberOfElements && left; i++)
    {
        const MenuElement& element = pPage->mElementsArray[i];
        const u16 shape = element.mGeometricShapeType;
        if (element.mElementType == GEOMETRIC_SHAPE_3 && element.mIsItDisplayed && (shape == 1 || shape == 2) &&
            Inside(x, y, element.mXpos - 12.0f, element.mYpos - 12.0f, element.mXpos + 12.0f, element.mYpos + 12.0f))
        {
            (shape == 1 ? pFrontend->mLeftPressed : pFrontend->mRightPressed) = true;
            return;
        }
    }
    s32 option = -1;
    for (s32 i = 0; i < pPage->mNumberOfOptions && option < 0; i++)
    {
        MenuItem& item = pPage->mOptionsArray[i];
        if (!item.mIsUnlocked)
        {
            continue;
        }
        wchar_t* pText = item.mOptionNameStr;
        if (item.mOptionType == HORIZONTAL_SELECTION_2)
        {
            pFrontend->GetOptionText(pFrontend->mCurrentPageIdx, static_cast<u16>(i), &pText);
        }
        const u16 font = i == pPage->mCurrentOptionIdx ? pFrontend->mSelectedFont
                                                        : (item.mFontType != 0xFFFF ? item.mFontType : pFrontend->mNormalFont);
        const f32 width = static_cast<f32>(Frontend::GetMaxTextWidth(pText, font));
        if (OnText(x, y, item.mXPos, item.mYPos, width, font))
        {
            option = i;
        }
    }
    if (option < 0 || !(moved || left || wheel))
    {
        return;
    }
    if (option != pPage->mCurrentOptionIdx)
    {
        snd1.mObjectType = option < pPage->mCurrentOptionIdx ? PortMenu::kMovedUp : PortMenu::kMovedDown;
        pPage->mCurrentOptionIdx = static_cast<u16>(option);
    }
    if (wheel)
    {
        (wheel < 0 ? pFrontend->mLeftPressed : pFrontend->mRightPressed) = true;
    }
    else if (left)
    {
        pFrontend->mReturnPressed = true;
    }
}
#endif
// gFrontend is not set yet while the frontend sets its pages up.
bool IsListingLevel(const PortSaves::Header& header)
{
    return IsLevel(sListing, header);
}

void Boot(Frontend* pFrontend, const char_type* pFileName)
{
    snprintf(sResumeFile, sizeof(sResumeFile), "%s", pFileName);
    pFrontend->LoadMapFilenamesFromSvg();
    pFrontend->mMenuScreen = RedBar_16;
    pFrontend->mState = FrontendState::Booting_Map_2;
}
} // namespace

void Setup(Frontend* pFrontend)
{
    MenuPage* pPage = &pFrontend->mMenuPagesArray[kMenuPage];
    // The menu draws its own lines; ChangeMenuPage only needs option 0 to be selectable.
    pPage->mNumberOfOptions = 0;
    pPage->mOptionsArray[0].mIsUnlocked = 1;
    pPage->mCurrentOptionIdx = 0;
    pPage->mDefaultOptionIdx = 0;

    MenuElement& title = pPage->mElementsArray[0];
    pPage->mNumberOfElements = 1;
    title.mElementType = STRING_TEXT_1;
    title.mXpos = 35;
    title.mYpos = 11;
    title.mIsItDisplayed = 1;
    title.mFontType = pFrontend->mTitleFont;
    title.mFontPalette = 5;

    PortSettings::ApplyRenderOptions();
#if defined(GTA2_UPDATE_CHECK)
    StartUpdateCheck();
#endif

    // The start menu's "Options" opened GTA2 Manager.
    pFrontend->mMenuPagesArray[MENUPAGE_START_MENU].mOptionsArray[1].mMenuPageTarget = kMenuPage;

#if !defined(GTA2_NET_PLAY)
    // Port: without network play built in (GTA2_NET_PLAY), MULTIPLAYER above QUIT, greyed and passed over, with SOON.
    // Last in the list, so that QUIT keeps the third option's background picture.
    MenuPage& start = pFrontend->mMenuPagesArray[MENUPAGE_START_MENU];
    MenuItem& multiplayer = start.mOptionsArray[3];
    multiplayer = start.mOptionsArray[2];
    wcsncpy(multiplayer.mOptionNameStr, PortSettings::Translate(L"MULTIPLAYER"), 50);
    multiplayer.mOptionNameStr[49] = 0;
    multiplayer.mIsUnlocked = 0;
    multiplayer.mMenuPageTarget = MENUPAGE_START_MENU;
    start.mOptionsArray[2].mYPos = multiplayer.mYPos + 20;
    start.mOptionStates[3] = start.mOptionStates[2];
    start.mOptionStates[2].mUnused2 += 20;
    start.mNumberOfOptions = 4;
    // The frontend leaves a locked option out: its text drawn as an element, in the greyed options' palette.
    MenuElement& name = start.mElementsArray[start.mNumberOfElements++];
    name.mElementType = STRING_TEXT_1;
    wcsncpy(name.mElementNameStr, multiplayer.mOptionNameStr, 50);
    name.mXpos = multiplayer.mXPos;
    name.mYpos = multiplayer.mYPos;
    name.mIsItDisplayed = 1;
    name.mFontType = 0xFFFF;
    name.mFontPalette = 8;
    MenuElement& soon = start.mElementsArray[start.mNumberOfElements++];
    soon.mElementType = STRING_TEXT_1;
    wcsncpy(soon.mElementNameStr, PortSettings::Translate(L"SOON"), 50);
    soon.mElementNameStr[49] = 0;
    soon.mXpos = multiplayer.mXPos + Frontend::GetMaxTextWidth(multiplayer.mOptionNameStr, pFrontend->mNormalFont) + 10;
    soon.mYpos = multiplayer.mYPos + 3;
    soon.mIsItDisplayed = 1;
    soon.mFontType = gFrontendFont7;
    soon.mFontPalette = 0xFFFF;
#endif

    // RESUME SAVED STATUS named as the page it opens; the Japanese text stays, the port has no translation for it.
    const struct
    {
        s32 mPage;
        s32 mOption;
    } kLoadGame[] = {{MENUPAGE_PLAY, 1}, {MENUPAGE_RESULTS_PLAYER_QUIT, 0}, {MENUPAGE_DEAD, 0}, {MENUPAGE_AREA_COMPLETE, 1}};
    for (s32 i = 0; i < 4 && !gText_0x14->LangIsJapanese(); i++)
    {
        MenuItem& option = pFrontend->mMenuPagesArray[kLoadGame[i].mPage].mOptionsArray[kLoadGame[i].mOption];
        wcsncpy(option.mOptionNameStr, PortSettings::Translate(L"LOAD GAME"), 50);
        option.mOptionNameStr[49] = 0;
        if (kLoadGame[i].mPage != MENUPAGE_PLAY)
        {
            option.mXPos = pFrontend->GetCenteredXPos(option.mOptionNameStr, option.mFontType, 320);
        }
    }
    if (gPortFrontendSetup)
    {
        gPortFrontendSetup(pFrontend); // the network game's page (port/net/menu.cpp)
    }
}

bool Update(Frontend* pFrontend)
{
    if (PortWelcome::Update(pFrontend))
    {
        return true;
    }
    if (pFrontend->mCurrentPageIdx != kMenuPage)
    {
        if (sShown)
        {
            PortMenu::Close();
        }
        sShown = false;
#if defined(GTA2_PORT_SDL)
        OriginalPageMouse(pFrontend);
#endif
        sSessionBefore = gGameSession;
        sScreenBefore = pFrontend->mMenuScreen;
        // The network game's page, and its line in the start menu, after the mouse: a click is its Enter, the right
        // button its Escape (port/net/menu.cpp)
        return gPortFrontendInput && gPortFrontendInput(pFrontend);
    }
    if (!sShown)
    {
        PortMenu::Open();
        sShown = true;
    }
    pFrontend->mMenuScreen = RedBar_16;
    PortMenu::Result result = HandleInput(pFrontend);
    // LOAD GAME's cities: also Page Up and Page Down (the pad's LB and RB), Q and E.
    static bool sPrevPageKeys[4];
    const u8 pageKeys[4] = {DIK_PRIOR, DIK_NEXT, DIK_Q, DIK_E};
    for (s32 i = 0; i < 4; i++)
    {
        const bool down = (pFrontend->mKeys[pageKeys[i]] & 0x80) != 0;
        if (down && !sPrevPageKeys[i] && result == PortMenu::kNothing && PortMenu::OnSaves())
        {
            result = PortMenu::HandleInput(i % 2 ? PortMenu::kRight : PortMenu::kLeft);
        }
        sPrevPageKeys[i] = down;
    }
    if (result == PortMenu::kLoadSave)
    {
        PortMenu::Close();
        sShown = false;
        Boot(pFrontend, PortMenu::SelectedSave().mFileName);
        snd1.mObjectType = PortMenu::kEntered;
        return true;
    }
    if (result == PortMenu::kClosed && PortMenu::OnSaves())
    {
        pFrontend->ChangeMenuPage(sSavesReturnPage);
        pFrontend->mMenuPagesArray[sSavesReturnPage].mCurrentOptionIdx = sSavesReturnOption;
        snd1.mObjectType = PortMenu::kWentBack;
    }
    else if (result == PortMenu::kClosed)
    {
        pFrontend->ChangeMenuPage(MENUPAGE_START_MENU);
        snd1.mObjectType = PortMenu::kWentBack;
    }
    else if (result != PortMenu::kNothing)
    {
        snd1.mObjectType = result;
    }
    wcsncpy(pFrontend->mMenuPagesArray[kMenuPage].mElementsArray[0].mElementNameStr, PortMenu::Title(), 50);
    return true;
}

// The warning in lines; where CONTINUE goes under it.
s32 DialogQuestion(Frontend* pFrontend, wchar_t* pLines)
{
    static const wchar_t* const kCities[PortSaves::kNumCities] = {L"DOWNTOWN", L"RESIDENTIAL", L"INDUSTRIAL"};
    const u8 city = static_cast<u8>(gGameSession.GetMainStageIdx());
    wchar_t text[128];
    swprintf(text, PortSettings::Translate(L"YOUR %ls AUTOSAVE WILL BE REPLACED"),
             PortSettings::Translate(kCities[city < PortSaves::kNumCities ? city : 0]));
    const s32 count = TextTable::InsertLineBreaksAndGetNumLines(pLines, text, kQuestionWidth, pFrontend->mNormalFont);
    return kQuestionTop + count * GetLineSpacingFromFontType(pFrontend->mNormalFont) + 10;
}

void AfterPageInput(Frontend* pFrontend)
{
    if (pFrontend->mState != FrontendState::Booting_Map_2 || gGameSession.GetSaveName()[0] || gGameSession.IsBonusStage() ||
        !PortSettings::Autosave() ||
        !PortSaves::HasAutosave(gGameSession.GetPlySlotIdx(), static_cast<u8>(gGameSession.GetMainStageIdx())))
    {
        return;
    }
    sBootScreen = pFrontend->mMenuScreen;
    pFrontend->mMenuScreen = sScreenBefore;
    pFrontend->mState = FrontendState::Delete_Player_Dialog_4;
    pFrontend->mDialogType = kReplaceDialog;
    pFrontend->mDialogCursorYpos = kCancelY;
}

bool UpdateDialog(Frontend* pFrontend)
{
    if (pFrontend->mDialogType != kReplaceDialog)
    {
        return false;
    }
    u16& cursor = pFrontend->mDialogCursorYpos;
#if defined(GTA2_PORT_SDL)
    // CONTINUE and CANCEL with the mouse, as the original pages' options.
    float x;
    float y;
    int left;
    int right;
    int wheel;
    const bool moved = port_TakeMouse(&x, &y, &left, &right, &wheel);
    pFrontend->mEscapePressed = pFrontend->mEscapePressed || right;
    wchar_t lines[160];
    const s32 continueTop = DialogQuestion(pFrontend, lines);
    const u16 choices[2] = {kContinueY, kCancelY};
    const wchar_t* const names[2] = {PortSettings::Translate(L"CONTINUE"), PortSettings::Translate(L"CANCEL")};
    for (s32 i = 0; i < 2 && (moved || left); i++)
    {
        const f32 width = static_cast<f32>(Frontend::GetMaxTextWidth(const_cast<wchar_t*>(names[i]), pFrontend->mSelectedFont));
        const f32 top = static_cast<f32>(continueTop + i * kChoiceStep);
        if (OnText(x, y, kQuestionLeft, top, width, pFrontend->mSelectedFont))
        {
            if (cursor != choices[i])
            {
                snd1.mObjectType = i ? PortMenu::kMovedDown : PortMenu::kMovedUp;
                cursor = choices[i];
            }
            pFrontend->mReturnPressed = pFrontend->mReturnPressed || left;
        }
    }
#endif
    if (pFrontend->mReturnPressed && cursor == kContinueY)
    {
        pFrontend->mMenuScreen = sBootScreen;
        pFrontend->mState = FrontendState::Booting_Map_2;
        snd1.mObjectType = PortMenu::kEntered;
    }
    else if (pFrontend->mEscapePressed || pFrontend->mReturnPressed)
    {
        gGameSession = sSessionBefore;
        pFrontend->mMenuScreen = sScreenBefore;
        pFrontend->mState = FrontendState::Menu_Navigation_1;
        snd1.mObjectType = PortMenu::kWentBack;
    }
    else if (pFrontend->mUpPressed || pFrontend->mDownPressed)
    {
        cursor = cursor == kContinueY ? kCancelY : kContinueY;
        snd1.mObjectType = pFrontend->mUpPressed ? PortMenu::kMovedUp : PortMenu::kMovedDown;
    }
    return true;
}

bool DrawDialog(Frontend* pFrontend)
{
    if (pFrontend->mDialogType != kReplaceDialog)
    {
        return false;
    }
    // In the title bar as the original dialog's DELETE PLAYER, then the question and its choices.
    DrawStringDefaultPalette(PortSettings::Translate(L"AUTOSAVE"), Fix16(275), Fix16(gText_0x14->LangIsJapanese() ? 16 : 12),
                             pFrontend->mHeadingFont, Fix16(1));
    wchar_t lines[160];
    const s32 continueTop = DialogQuestion(pFrontend, lines);
    DrawStringDefaultPalette(lines, Fix16(kQuestionLeft), Fix16(kQuestionTop), static_cast<s16>(pFrontend->mNormalFont), Fix16(1));
    const u16 cursor = pFrontend->mDialogCursorYpos;
    DrawStringDefaultPalette(PortSettings::Translate(L"CONTINUE"), Fix16(kQuestionLeft), Fix16(continueTop),
                             cursor == kContinueY ? pFrontend->mSelectedFont : static_cast<s16>(pFrontend->mNormalFont), Fix16(1));
    DrawStringDefaultPalette(PortSettings::Translate(L"CANCEL"), Fix16(kQuestionLeft), Fix16(continueTop + kChoiceStep),
                             cursor == kCancelY ? pFrontend->mSelectedFont : static_cast<s16>(pFrontend->mNormalFont), Fix16(1));
    PortMenu::DrawChoiceHints();
    return true;
}

void Draw(Frontend* pFrontend)
{
    if (pFrontend->mCurrentPageIdx == kMenuPage)
    {
        PortMenu::Draw(1.0f, 0.0f, 0.0f, false);
    }
}

void VersionText(wchar_t* pOut)
{
    char text[32];
    snprintf(text, sizeof(text), "REG2 %s", GTA2_VERSION);
    Widen(text, pOut);
}

void DrawUpdate(wchar_t* pVersion, s16 font)
{
#if defined(GTA2_UPDATE_CHECK)
    if (const s32 newer = NewerRelease())
    {
        char text[32];
        snprintf(text, sizeof(text), "  %d.%d.%d AVAILABLE!", newer / 1000000, newer / 1000 % 1000, newer % 1000);
        wchar_t wide[32];
        Widen(text, wide);
        // Remap 10 gives this font the red of the selected menu option.
        DrawString(wide, 300 + Frontend::GetMaxTextWidth(pVersion, font), 460, font, 1, palette_types_enum::font_remaps_8, 10, 0, 0);
    }
#endif
}

bool UpdateNameEntry(Frontend* pFrontend)
{
    if (!PortMenu::PadMode())
    {
        return false;
    }
    static const wchar_t kLetters[] = L"ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789 ";
    const s32 numLetters = static_cast<s32>(wcslen(kLetters));
    wchar_t* pName = pFrontend->mCurrPlyrName;
    u8& len = pFrontend->mCurrPlyrNameLength;
    if (len == 0)
    {
        pName[len++] = L'A';
    }
    if (pFrontend->mReturnPressed || pFrontend->mEscapePressed)
    {
        pFrontend->mState = FrontendState::Menu_Navigation_1;
        pFrontend->StripPlayerNameToCurrLength();
        if (pFrontend->mReturnPressed)
        {
            pFrontend->SaveAndUpdatePlayerName();
        }
        pFrontend->mMenuPagesArray[MENUPAGE_PLAY].mCurrentOptionIdx = 0;
        snd1.mObjectType = pFrontend->mReturnPressed ? 5 : 6;
        return true;
    }
    if (pFrontend->mUpPressed || pFrontend->mDownPressed)
    {
        s32 i = 0;
        while (i < numLetters && kLetters[i] != pName[len - 1])
        {
            i++;
        }
        i = Wrap((i < numLetters ? i : 0) + (pFrontend->mUpPressed ? 1 : -1), numLetters);
        pName[len - 1] = kLetters[i];
        snd1.mObjectType = 7;
    }
    else if (pFrontend->mRightPressed && len < 8)
    {
        pName[len] = pName[len - 1] == L' ' ? L'A' : pName[len - 1];
        len++;
        snd1.mObjectType = 7;
    }
    else if (pFrontend->mLeftPressed && len > 1)
    {
        len--;
        snd1.mObjectType = 8;
    }
    pFrontend->StripPlayerNameToCurrLength();
    return true;
}

s32 DrawKeyHint(u8 hint, wchar_t* pText, u16 x, u16 y, u16 font, s32 palette)
{
    using namespace PortPrompts;
    // Whether the last key hint was a picture; its label follows it.
    static bool sPicture;
    const bool pad = PortMenu::PadMode();
    // ESC, DEL, ENTER and BACKSPACE; the pad deletes a letter with the d-pad's left.
    const Glyph kKeys[4] = {kKeyEscape, kKeyDelete, kKeyEnter, kKeyBackspace};
    const Glyph kButtons[4] = {kPadEast, kPadWest, kPadSouth, kPadDpadLeft};
    if (hint < 4)
    {
        const Glyph glyph = pad ? kButtons[hint] : kKeys[hint];
        sPicture = glyph != kNoGlyph && Available();
        if (sPicture)
        {
            return static_cast<s32>(PortMenu::DrawHintGlyph(glyph, x, y));
        }
        if (pad)
        {
            static const wchar_t* const kButtonNames[4] = {L"B", L"X", L"A", NULL};
            if (kButtonNames[hint])
            {
                wcscpy(pText, kButtonNames[hint]);
            }
            else
            {
                wcscpy(pText, gText_0x14->Find("left_i"));
            }
        }
    }
    // The labels start with the font's colon and two spaces.
    else if (sPicture && pText[0] == L';')
    {
        const wchar_t* pLabel = pText + 1;
        while (*pLabel == L' ')
        {
            pLabel++;
        }
        memmove(pText, pLabel, (wcslen(pLabel) + 1) * sizeof(wchar_t));
    }
    if (static_cast<u16>(palette) == 0xFFFF)
    {
        DrawStringDefaultPalette(pText, x, y, font, 1);
    }
    else
    {
        DrawString(pText, x, y, font, 1, palette_types_enum::font_remaps_8, palette, 0, 0);
    }
    return Frontend::GetMaxTextWidth(pText, font);
}

bool IsLevel(Frontend* pFrontend, const PortSaves::Header& header)
{
    const MainBlockStrings& level = (pFrontend ? pFrontend->mBlocks : sLevels)[header.mMainStage][header.mBonusStage];
    return !strncmp(header.mMapName, "data\\", 5) && !strcmp(header.mMapName + 5, level.mGmpName) &&
        !strncmp(header.mStyleName, "data\\", 5) && !strcmp(header.mStyleName + 5, level.mStyName) &&
        !strncmp(header.mScriptName, "data\\", 5) && !strcmp(header.mScriptName + 5, level.mScrName);
}

s32 ListSaves(Frontend* pFrontend, PortSaves::Entry* pEntries, bool* pCities)
{
    sListing = pFrontend;
    const PlySlot& stats = gPlyDat->mPlyrStats[gGameSession.GetPlySlotIdx()];
    for (u8 city = 0; city < PortSaves::kNumCities && pCities; city++)
    {
        pCities[city] = city < (pFrontend ? pFrontend->mNumMainStages : sNumCities) &&
            stats.mPlyrStageStats[city][0].mIsStageUnlocked && FreeLoader::CheckCityInstalled(city);
    }
    return PortSaves::List(gGameSession.GetPlySlotIdx(), IsListingLevel, pEntries);
}

bool HasSaves(Frontend* pFrontend)
{
    PortSaves::Entry saves[PortSaves::kMaxSaves];
    return ListSaves(pFrontend, saves, NULL) > 0;
}

bool HasSave(Frontend* pFrontend, u8 mainStage, u8 bonusStage)
{
    PortSaves::Entry saves[PortSaves::kMaxSaves];
    const s32 count = ListSaves(pFrontend, saves, NULL);
    for (s32 i = 0; i < count; i++)
    {
        if (saves[i].mMainStage == mainStage && saves[i].mBonusStage == bonusStage)
        {
            return true;
        }
    }
    return false;
}

bool Resume(Frontend* pFrontend)
{
    PortSaves::Entry saves[PortSaves::kMaxSaves];
    bool cities[PortSaves::kNumCities];
    const s32 count = ListSaves(pFrontend, saves, cities);
    if (!count)
    {
        return false;
    }
    sSavesReturnPage = pFrontend->mCurrentPageIdx;
    sSavesReturnOption = pFrontend->mMenuPagesArray[sSavesReturnPage].mCurrentOptionIdx;
    PortMenu::OpenSaves(saves, count, cities);
    sShown = true;
    pFrontend->ChangeMenuPage(kMenuPage);
    return true;
}

void AfterGame(Frontend* pFrontend)
{
    memcpy(sLevels, pFrontend->mBlocks, sizeof(sLevels));
    sNumCities = pFrontend->mNumMainStages;
    char_type name[64];
    PortSaves::Header header;
    if (PortRetry::TakeRetry(name, sizeof(name)) && PortSaves::IsValid(name, &header) && IsLevel(pFrontend, header))
    {
        Boot(pFrontend, name);
    }
}

void ResumeFileName(char_type* pFileName)
{
    if (sResumeFile[0])
    {
        strcpy(pFileName, sResumeFile);
        sResumeFile[0] = 0;
    }
}
} // namespace PortOptionsMenu
