#include "menu.hpp"
#include "Draw.hpp"
#include "Frontend.hpp"
#include "Function.hpp"
#include "Globals.hpp"
#include "InputRecorder_67F8B0.hpp"
#include "KeyboardLayout.hpp"
#include "input.hpp"
#include "prompts.hpp"
#include "registry.hpp"
#include "settings.hpp"
#include "TextTable.hpp"
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <vector>

// No std::wstring: with -fshort-wchar its traits would call the C library's 4-byte wide functions.

bool port_GameRunning();
#if defined(GTA2_PORT_SDL)
bool port_PadMode();
int port_GetPadBinding(int control);
void port_SetPadBinding(int control, int code);
int port_DefaultPadBinding(int control);
int port_GetAltBinding(int control);
void port_SetAltBinding(int control, int code);
int port_DefaultAltBinding(int control);
int port_KeyLetter(int dik);
void port_CaptureStart();
int port_CapturedKey();
int port_CapturedPad();
bool port_MouseMode();
bool port_TakeMouse(float* pX, float* pY, int* pLeft, int* pRight, int* pWheel);
bool port_MouseLeftDown();
void port_DeferRegistrySaves(bool defer);
#endif

namespace PortMenu
{
namespace
{
using PortSettings::Translate;

enum Page
{
    kMainPage,
    kGamePage,
    kGraphicsPage,
    kDisplayPage,
    kSoundPage,
    kControlsPage,
    kStatsPage,
    kTranscriptPage,
    kSavesPage,
    kLoadPage,
    kQuitPage,
    kPlayersPage,
    kNumPages,
};

enum Kind
{
    kGoTo,
    kSetting,
    kBinding,
    kDefaultKeys,
    kBack,
    kResume,
    kQuit,
    kSave,
    kRetry,
    kContinue,
    kSaveAndQuit,
    kRadio,
    kReady,
    kVoteYes,
    kVoteNo,
};

struct Item
{
    Kind mKind;
    s32 mArg; // page, setting or control; 1 for a BACK named CANCEL, or a QUIT GAME named QUIT
    bool mGap; // set apart from the item above
    const wchar_t* mTitle; // its group's title above it, in English, NULL if none
};

const s32 kMaxItems = 20;
const wchar_t* const kTitles[kNumPages] = {L"OPTIONS", L"GAME", L"GRAPHICS", L"DISPLAY", L"SOUND", L"CONTROLS", L"STATISTICS",
                                           L"MISSION TRANSCRIPT", L"LOAD GAME", L"LOAD GAME", L"QUIT GAME", L"PLAYERS"};

// The game's 12 controls, then SCORES: held, a network game's scoreboard (compat/src/pad.cpp); its pad input is Back.
// The DirectX build has no network game.
#if defined(GTA2_PORT_SDL)
const s32 kScoresControl = 12;
const s32 kNumControls = 13;
#else
const s32 kNumControls = 12;
#endif
// SPECIAL 3 does nothing in the game: it keeps its bindings but is not shown.
const s32 kSpecial3Control = 11;
const s32 kShownControls = kNumControls - 1;
const s32 kDefaultControls[13] = {DIK_UP, DIK_DOWN, DIK_LEFT, DIK_RIGHT, DIK_LCONTROL, DIK_RETURN, DIK_SPACE,
                                  DIK_Z, DIK_X, DIK_C, DIK_LMENU, DIK_RSHIFT, DIK_Q};
const wchar_t* const kControlNames[13] = {L"FORWARD", L"BACKWARD", L"LEFT", L"RIGHT", L"ATTACK",
                                          L"ENTER OR EXIT", L"JUMP OR BRAKE", L"PREVIOUS WEAPON",
                                          L"NEXT WEAPON", L"HORN OR TAUNT", L"LOOK AROUND", L"SPECIAL 3", L"SCORES"};
// Pad binding codes (compat/src/pad.cpp): buttons 0 to 11, then the stick and d-pad directions.
const wchar_t* const kPadButtonNames[12] = {L"A", L"B", L"X", L"Y", L"LB", L"RB", L"BACK", L"START", L"L3", L"R3", L"LT", L"RT"};
const s32 kPadB = 1;
const s32 kPadBack = 6;
const s32 kPadLeft = 224;
const s32 kPadRight = 225;
const s32 kPadUp = 226;
const s32 kPadDown = 227;
// Second key codes (compat/src/port_platform.hpp): a DIK code, a mouse button, a wheel notch.
const s32 kMouseCode = 0x100;
const s32 kWheelUpCode = 0x110;

// A binding's columns, KEYBOARD alone without SDL.
enum Column
{
    kKeyColumn,
    kAltColumn,
    kPadColumn,
    kNumColumns,
};
#if defined(GTA2_PORT_SDL)
const s32 kColumns = kNumColumns;
#else
const s32 kColumns = 1;
#endif
const wchar_t* const kColumnNames[kNumColumns] = {L"KEYBOARD", L"ALTERNATE", L"GAMEPAD"};
// A save's line: its kind, then its money, jobs and date under their headings.
const s32 kSaveColumns = 3;
const wchar_t* const kSaveHeadings[kSaveColumns] = {L"MONEY", L"JOBS", L"DATE"};
// What the kind's column leaves of the page, a job's title cut to fit.
f32 sSaveNameWidth = 200;
const s32 kMaxTexts = 4;
const s32 kTextLength = 64;

// The frontend's user sprites: arrows.
const s16 kLeftArrowSprite = 37;
const s16 kArrowSprite = 38;

Page sPage;
s32 sSelected[kNumPages];
PortSaves::Entry sSaves[PortSaves::kMaxSaves];
s32 sNumSaves;
// The saves page's city tab, the one under the mouse, and the cities that can be chosen.
s32 sTab;
s32 sTabHover = -1;
bool sTabEnabled[PortSaves::kNumCities];
const wchar_t* const kCities[PortSaves::kNumCities] = {L"DOWNTOWN", L"RESIDENTIAL", L"INDUSTRIAL"};
// The retry checkpoint the pause menu offers.
PortSaves::Checkpoint sRetry;
bool sRestart; // the job or frenzy still runs
s32 sRetryMoney;
long long sRetryTime;
PortSaves::JobName sRetryJob;
s32 sStatRows;
f32 sTranscript = -1; // MISSION TRANSCRIPT's height, -1 if not offered
// The station RADIO shows, NULL without the line.
const wchar_t* sRadio;
// QUIT GAME's choices offered, and why SAVE AND QUIT is greyed, NULL if it is not.
bool sQuitChoices;
const wchar_t* sQuitBlocked;
// A network game's menu (OfferNetwork): its players, the score's heading, READY (1) or NOT READY (2), the open vote.
bool sNetwork;
NetworkPlayer sPlayers[kMaxNetworkPlayers];
s32 sNumPlayers;
const wchar_t* sScoreHeading = L"FRAGS";
s32 sReady;
wchar_t sVote[kTextLength];
s32 sColumn; // a Column
s32 sCapturing = -1; // control waiting for a key or pad input
bool sDragging; // a slider's knob, with the left button held
#if !defined(GTA2_PORT_SDL)
char_type sPrevKeys[256];
#endif

struct KeyName
{
    wchar_t mName[24];
};
std::vector<KeyName> sKeyNames;

// A page of settings, each group's title over its first one, then BACK. A group without settings shows nothing.
s32 SettingsPage(Item* pItems, const PortSettings::Id* pIds, s32 count)
{
    s32 n = 0;
    const wchar_t* pTitle = NULL;
    for (s32 i = 0; i < count; i++)
    {
        if (PortSettings::IsGroup(pIds[i]))
        {
            pTitle = PortSettings::Name(pIds[i]);
        }
        else
        {
            pItems[n++] = Item{kSetting, pIds[i], false, pTitle};
            pTitle = NULL;
        }
    }
    pItems[n++] = Item{kBack, 0, true};
    return n;
}

s32 Items(Page page, Item* pItems)
{
    const bool inGame = port_GameRunning();
    s32 n = 0;
    switch (page)
    {
        case kMainPage:
            if (inGame && sRetry != PortSaves::kNoCheckpoint)
            {
                pItems[n++] = Item{kRetry, 0, false};
            }
            if (inGame)
            {
                pItems[n++] = Item{kResume, 0, sRetry != PortSaves::kNoCheckpoint};
            }
            if (inGame && sNetwork && sReady)
            {
                pItems[n++] = Item{kReady, 0, false};
            }
            if (inGame && sNetwork && sVote[0])
            {
                pItems[n++] = Item{kVoteYes, 0, false, sVote};
                pItems[n++] = Item{kVoteNo, 0, false};
            }
            if (inGame && sNetwork)
            {
                pItems[n++] = Item{kGoTo, kPlayersPage, !!sVote[0]};
            }
            if (inGame && sRadio)
            {
                pItems[n++] = Item{kRadio, 0, false};
            }
            if (inGame && sStatRows)
            {
                pItems[n++] = Item{kGoTo, kStatsPage, false};
            }
            if (inGame && sTranscript >= 0)
            {
                pItems[n++] = Item{kGoTo, kTranscriptPage, false};
            }
            if (inGame && sNumSaves)
            {
                pItems[n++] = Item{kGoTo, kLoadPage, false};
            }
            for (s32 page = sNetwork ? kGraphicsPage : kGamePage; page <= kControlsPage; page++)
            {
                pItems[n++] = Item{kGoTo, page, inGame && (page == kGamePage || (sNetwork && page == kGraphicsPage))};
            }
            pItems[n++] = Item{inGame ? kQuit : kBack, 0, true};
            break;
        case kGamePage:
            return SettingsPage(pItems, PortSettings::kGameSettings, PortSettings::kNumGameSettings);
        case kGraphicsPage:
            return SettingsPage(pItems, PortSettings::kGraphicsSettings, PortSettings::kNumGraphicsSettings);
        case kDisplayPage:
            return SettingsPage(pItems, PortSettings::kDisplaySettings, PortSettings::kNumDisplaySettings);
        case kSoundPage:
            return SettingsPage(pItems, PortSettings::kSoundSettings, PortSettings::kNumSoundSettings);
        case kStatsPage:
        case kTranscriptPage:
        case kPlayersPage:
            pItems[n++] = Item{kBack, 0, false};
            break;
        case kSavesPage:
            for (s32 i = 0; i < sNumSaves; i++)
            {
                if (sSaves[i].mMainStage == sTab)
                {
                    pItems[n++] = Item{kSave, i, false};
                }
            }
            pItems[n++] = Item{kBack, 0, true};
            break;
        case kLoadPage:
            pItems[n++] = Item{kContinue, 0, false};
            pItems[n++] = Item{kBack, 1, false};
            break;
        case kQuitPage:
            if (!sNetwork)
            {
                pItems[n++] = Item{kSaveAndQuit, 0, false};
            }
            pItems[n++] = Item{kQuit, 1, false};
            pItems[n++] = Item{kBack, 1, false};
            break;
        default:
            for (s32 i = 0; i < kShownControls; i++)
            {
                pItems[n++] = Item{kBinding, i < kSpecial3Control ? i : i + 1, false};
            }
#if defined(GTA2_PORT_SDL)
            pItems[n++] = Item{kSetting, PortSettings::kControls, true};
            pItems[n++] = Item{kSetting, PortSettings::kAimCamera, false};
            pItems[n++] = Item{kSetting, PortSettings::kStrafeAnimation, false};
            pItems[n++] = Item{kSetting, PortSettings::kCrosshair, false};
            pItems[n++] = Item{kSetting, PortSettings::kVibration, false};
#endif
            // Under the TWIN STICK settings, or set apart from the bindings.
            const bool gap = n == kShownControls;
            pItems[n++] = Item{kDefaultKeys, 0, gap};
            pItems[n++] = Item{kBack, 0, false};
            break;
    }
    return n;
}

const wchar_t* EnglishName(const Item& item)
{
    switch (item.mKind)
    {
        case kGoTo:
            return kTitles[item.mArg];
        case kSetting:
            return PortSettings::Name(static_cast<PortSettings::Id>(item.mArg));
        case kBinding:
            return kControlNames[item.mArg];
        case kDefaultKeys:
            return L"DEFAULT KEYS";
        case kResume:
            return L"RESUME";
        case kQuit:
            if (sNetwork)
            {
                return item.mArg ? L"LEAVE" : L"LEAVE GAME";
            }
            return item.mArg ? L"QUIT" : L"QUIT GAME";
        case kSaveAndQuit:
            return L"SAVE AND QUIT";
        case kSave:
            switch (sSaves[item.mArg].mKind)
            {
                case PortSaves::kChurch:
                    return L"CHURCH";
                case PortSaves::kAutosave:
                    return L"AUTOSAVE";
                default:
                    return sSaves[item.mArg].mCheckpoint == PortSaves::kBeforeFrenzy ? L"BEFORE LAST FRENZY" : L"BEFORE LAST JOB";
            }
        case kRetry:
            if (sRestart)
            {
                return sRetry == PortSaves::kBeforeFrenzy ? L"RESTART FRENZY" : L"RESTART JOB";
            }
            return sRetry == PortSaves::kBeforeFrenzy ? L"RETRY FRENZY" : L"RETRY JOB";
        case kContinue:
            return L"CONTINUE";
        case kRadio:
            return L"RADIO";
        case kReady:
            return sReady == 2 ? L"NOT READY" : L"READY";
        case kVoteYes:
            return L"VOTE YES";
        case kVoteNo:
            return L"VOTE NO";
        default:
            return item.mArg ? L"CANCEL" : L"BACK";
    }
}

bool JobTitle(const PortSaves::JobName& job, wchar_t* pOut, size_t size);
void FitText(wchar_t* pText, f32 width);

// An item's name; a job checkpoint's is BEFORE: <title>.
const wchar_t* ItemName(const Item& item)
{
    static wchar_t sName[kTextLength];
    wchar_t title[kTextLength - 16];
    const PortSaves::Entry* pSave = item.mKind == kSave ? &sSaves[item.mArg] : NULL;
    if (pSave && pSave->mKind == PortSaves::kRetry && pSave->mCheckpoint == PortSaves::kBeforeJob &&
        JobTitle(pSave->mJob, title, GTA2_COUNTOF(title)))
    {
        swprintf(sName, Translate(L"BEFORE: %ls"), title);
        FitText(sName, sSaveNameWidth);
        return sName;
    }
    return Translate(EnglishName(item));
}

// The key names GTA2 Manager showed: data\keyboard\<lang>_kb.cfg, one name per DirectInput code.
void LoadKeyNames()
{
    static const char* const kFiles[] = {"eng", "fre", "ger", "ita", "spa", "por", "eng"};
    const s32 layout = KeyboardLayout::GetLayout();
    char path[64];
    sprintf(path, "data\\keyboard\\%s_kb.cfg", kFiles[layout >= 0 && layout < 7 ? layout : 0]);
    sKeyNames.clear();
    FILE* f = fopen(path, "rb");
    if (!f)
    {
        return;
    }
    KeyName line = {};
    size_t len = 0;
    for (int c; (c = fgetc(f)) != EOF;)
    {
        if (c == '\n')
        {
            sKeyNames.push_back(line);
            line = KeyName();
            len = 0;
        }
        else if (c != '\r' && c != '`' && len + 1 < 24)
        {
            line.mName[len++] = static_cast<wchar_t>(static_cast<u8>(c));
        }
    }
    fclose(f);
}

void KeyName(s32 dik, wchar_t* pOut)
{
    dik &= 0xFF;
#if defined(GTA2_PORT_SDL)
    // A letter as the keyboard's layout has it, whatever the language.
    if (const int letter = port_KeyLetter(dik))
    {
        pOut[0] = static_cast<wchar_t>(letter);
        pOut[1] = 0;
        return;
    }
#endif
    if (dik < static_cast<s32>(sKeyNames.size()) && sKeyNames[dik].mName[0])
    {
        wcscpy(pOut, sKeyNames[dik].mName);
    }
    else
    {
        swprintf(pOut, Translate(L"KEY %d"), dik);
    }
}

void PadName(s32 code, wchar_t* pOut)
{
    static const wchar_t* const kDirections[] = {L"LEFT", L"RIGHT", L"UP", L"DOWN"};
    if (code >= kPadLeft && code <= kPadDown)
    {
        wcscpy(pOut, Translate(kDirections[code - kPadLeft]));
    }
    else
    {
        wcscpy(pOut, code >= 0 && code < 12 ? kPadButtonNames[code] : L"-");
    }
}

// The second key column's names, a key's or the mouse's; "-" for none.
void AltName(s32 code, wchar_t* pOut)
{
    if (code >= kWheelUpCode)
    {
        swprintf(pOut, L"%ls %ls", Translate(L"WHEEL"), code == kWheelUpCode ? L"+" : L"-");
    }
    else if (code > kMouseCode)
    {
        // SDL's buttons 1 to 5 are left, middle, right, back and forward: mouse 1, 3, 2, 4, 5.
        const s32 button = code - kMouseCode;
        swprintf(pOut, L"%ls %d", Translate(L"MOUSE"), button == 2 ? 3 : (button == 3 ? 2 : button));
    }
    else if (code > 0)
    {
        KeyName(code, pOut);
    }
    else
    {
        wcscpy(pOut, L"-");
    }
}

s32 GetKey(s32 control)
{
    return gRegistry.Get_Control_Setting(control, kDefaultControls[control]) & 0xFF;
}

void SetKey(s32 control, s32 key)
{
    HKEY hKey;
    if (RegCreateKeyExA(HKEY_LOCAL_MACHINE, "SOFTWARE\\DMA Design Ltd\\GTA2\\Control", 0, NULL, 0, KEY_ALL_ACCESS, NULL,
                        &hKey, NULL) != ERROR_SUCCESS)
    {
        return;
    }
    char name[4];
    sprintf(name, "%d", control);
    const DWORD data = static_cast<DWORD>(key);
    RegSetValueExA(hKey, name, 0, REG_DWORD, reinterpret_cast<const BYTE*>(&data), sizeof(data));
    RegCloseKey(hKey);
}

s32 GetBinding(s32 column, s32 control)
{
#if defined(GTA2_PORT_SDL)
    if (column != kKeyColumn)
    {
        return column == kAltColumn ? port_GetAltBinding(control) : port_GetPadBinding(control);
    }
#endif
    return GetKey(control);
}

void SetBinding(s32 column, s32 control, s32 binding)
{
#if defined(GTA2_PORT_SDL)
    if (column != kKeyColumn)
    {
        column == kAltColumn ? port_SetAltBinding(control, binding) : port_SetPadBinding(control, binding);
        return;
    }
#endif
    SetKey(control, binding);
}

// The game reads the keyboard's bindings when it starts, the pad's at each press.
void ReloadKeys()
{
    if (gInputDevices)
    {
        gInputDevices->get_registry_controls();
    }
}

// A key or pad input already used by another binding moves to the one being changed, keys between both
// keyboard columns too, but a KEYBOARD binding never gets none or a mouse input.
void Bind(s32 column, s32 control, s32 binding)
{
    const s32 old = GetBinding(column, control);
    for (s32 c = 0; c < kColumns; c++)
    {
        for (s32 i = 0; i < kNumControls; i++)
        {
            const bool sameDevice = (c == kPadColumn) == (column == kPadColumn);
            if (sameDevice && (c != column || i != control) && GetBinding(c, i) == binding &&
                (c != kKeyColumn || (old > 0 && old < kMouseCode)))
            {
                SetBinding(c, i, old);
            }
        }
    }
    SetBinding(column, control, binding);
    ReloadKeys();
}

void DefaultKeys()
{
    for (s32 i = 0; i < kNumControls; i++)
    {
        SetKey(i, kDefaultControls[i]);
#if defined(GTA2_PORT_SDL)
        port_SetAltBinding(i, port_DefaultAltBinding(i));
        port_SetPadBinding(i, port_DefaultPadBinding(i));
#endif
    }
    ReloadKeys();
}

#if !defined(GTA2_PORT_SDL)
// The DirectX build has no SDL layer: the first key pressed in the frontend's key state.
s32 FrontendKeyPressed()
{
    s32 result = -1;
    for (s32 i = 0; i < 256 && result < 0; i++)
    {
        if ((gFrontend->mKeys[i] & 0x80) && !(sPrevKeys[i] & 0x80))
        {
            result = i;
        }
    }
    memcpy(sPrevKeys, gFrontend->mKeys, sizeof(sPrevKeys));
    return result;
}
#endif

void GoTo(Page page)
{
    if (page == kControlsPage)
    {
        sColumn = PadMode() ? kPadColumn : kKeyColumn;
    }
    sPage = page;
    // LOAD GAME's question on CANCEL, as the frontend's; QUIT GAME's choices on CANCEL if SAVE AND QUIT is greyed.
    sSelected[page] = page == kLoadPage ? 1 : (page == kQuitPage && sQuitBlocked ? 2 : 0);
}

// In game the saves page goes back to the pause menu.
Result Back()
{
    if (sPage == kMainPage || (sPage == kSavesPage && !port_GameRunning()))
    {
        return kClosed;
    }
    sPage = kMainPage;
    return kWentBack;
}

// Drawing: the layout's coordinates to the screen's.
f32 sScale;
f32 sLeft;
f32 sTop;

Fix16 Fx(f32 v)
{
    return Fix16(static_cast<s32>(v * 16384.0f), 0);
}

// colour: 0xRRGGBB.
void Rect(f32 left, f32 top, f32 right, f32 bottom, s32 colour)
{
    Vert verts[4] = {};
    verts[0].x = verts[3].x = sLeft + left * sScale;
    verts[1].x = verts[2].x = sLeft + right * sScale;
    verts[0].y = verts[1].y = sTop + top * sScale;
    verts[2].y = verts[3].y = sTop + bottom * sScale;
    pgbh_DrawFlatRect(verts, colour);
}

// The text colours (white, the red remap); dim, darker.
s32 TextColour(bool selected, bool dim)
{
    return dim ? (selected ? 0x5C1212 : 0x707070) : (selected ? 0xB72323 : 0xFFFFFF);
}

// The pages' text: the frontend's plain white score font, smaller than its menus'; a selected line in its
// red-rimmed one, reddened by a remap. Neither has a '-', drawn as a bar.
s16 MenuFont()
{
    return gFrontendScoreFont;
}

const u16 kRedRemap = 4;
const f32 kDashWidth = 7;
const f32 kDashY = 8;

// Draws the text if asked; its width. Dim: half transparent, grey on the panel or the frontend's black.
f32 TextAt(const wchar_t* pText, f32 x, f32 y, s16 font, bool selected, bool dim, bool draw)
{
    const bool bars = GetCharWidthFromFontType(font, L'-') < 3;
    font = selected && font == MenuFont() ? gFrontendPlayMenuScoreFont : font;
    f32 width = 0;
    while (*pText)
    {
        wchar_t run[64];
        size_t n = 0;
        for (; pText[n] && n < 63 && !(bars && pText[n] == L'-'); n++)
        {
            run[n] = pText[n];
        }
        run[n] = 0;
        pText += n;
        if (n && draw)
        {
            DrawString(run, Fx(sLeft + (x + width) * sScale), Fx(sTop + y * sScale), font, Fx(sScale),
                       selected ? palette_types_enum::font_remaps_8 : palette_types_enum::sprites_2, selected ? kRedRemap : 0,
                       dim ? 1 : 0, dim ? 12 : 0);
        }
        width += n ? Frontend::GetMaxTextWidth(run, font) : 0;
        if (bars && *pText == L'-')
        {
            if (draw)
            {
                Rect(x + width + 1, y + kDashY, x + width + kDashWidth - 1, y + kDashY + 2, TextColour(selected, dim));
            }
            width += kDashWidth;
            pText++;
        }
    }
    return width;
}

f32 Width(const wchar_t* pText, s16 font)
{
    return TextAt(pText, 0, 0, font, false, false, false);
}

void Text(const wchar_t* pText, f32 x, f32 y, s16 font, bool selected = false, bool dim = false)
{
    TextAt(pText, x, y, font, selected, dim, true);
}

void Sprite(s16 sprite, f32 x, f32 y)
{
    DrawFigure(sprite_types_enum::user_6, sprite, Fx(sLeft + x * sScale), Fx(sTop + y * sScale), Ang16(0), Fx(sScale),
               palette_types_enum::sprites_2, 0, 0, 0, false);
}

f32 Max(f32 a, f32 b)
{
    return a > b ? a : b;
}

// The numeric settings' sliders: a track, then the value's text.
const f32 kSliderWidth = 100;
const f32 kSliderGap = 14;

struct Slider
{
    s32 mMin;
    s32 mMax;
    s32 mValue;
};

bool GetSlider(const Item& item, Slider* pSlider)
{
    return item.mKind == kSetting &&
        PortSettings::Range(static_cast<PortSettings::Id>(item.mArg), &pSlider->mMin, &pSlider->mMax, &pSlider->mValue);
}

// From the value column to the value's text.
f32 TextOffset(const Item& item)
{
    Slider slider;
    return GetSlider(item, &slider) ? kSliderWidth + kSliderGap : 0;
}

// The text's colours, a darker grey for the empty part of the track.
void DrawSlider(const Slider& slider, f32 x, f32 y, bool selected, bool dim)
{
    const s32 colour = TextColour(selected, dim);
    const f32 middle = y + 7;
    const f32 knob = x + kSliderWidth * (slider.mValue - slider.mMin) / (slider.mMax - slider.mMin);
    Rect(x, middle - 1.5f, kSliderWidth + x, middle + 1.5f, 0x606060);
    Rect(x, middle - 1.5f, knob, middle + 1.5f, colour);
    Rect(knob - 3, middle - 7, knob + 3, middle + 7, colour);
}

// What a network game turns off (port/docs/netcode.md): TWIN STICK with its settings, and ZOOM.
bool OffInNetworkGame(const Item& item)
{
    const PortSettings::Id id = static_cast<PortSettings::Id>(item.mArg);
    return sNetwork && item.mKind == kSetting &&
        (id == PortSettings::kControls || id == PortSettings::kZoom || PortSettings::Requirement(id) == PortSettings::kControls);
}

bool IsEnabled(const Item& item)
{
    if (OffInNetworkGame(item))
    {
        return false;
    }
    if (item.mKind == kSaveAndQuit)
    {
        return !sQuitBlocked;
    }
    return item.mKind != kSetting || PortSettings::Enabled(static_cast<PortSettings::Id>(item.mArg));
}

// Under the page, for a greyed setting: the one it needs.
void DrawRequirement(const Item& item, f32 x, f32 y)
{
    if (OffInNetworkGame(item))
    {
        Text(Translate(L"NOT IN NETWORK GAMES"), x, y, MenuFont());
        return;
    }
    const PortSettings::Id requirement = PortSettings::Requirement(static_cast<PortSettings::Id>(item.mArg));
    wchar_t text[64];
    const wchar_t* pName = requirement == PortSettings::kLighting ? L"DAY-NIGHT CYCLE"
                                                                  : (requirement == PortSettings::kControls ? L"TWIN STICK" : PortSettings::Name(requirement));
    swprintf(text, L"%ls %ls", Translate(L"NEEDS"), Translate(pName));
    Text(text, x, y, MenuFont());
}

// The value under the mouse on a slider's track, or nearest to it.
Result SetSliderAt(const Item& item, f32 x, f32 trackX)
{
    Slider slider;
    if (!GetSlider(item, &slider))
    {
        return kNothing;
    }
    const f32 steps = (x - trackX) * (slider.mMax - slider.mMin) / kSliderWidth;
    s32 value = slider.mMin + static_cast<s32>(steps < 0 ? steps - 0.5f : steps + 0.5f);
    value = value < slider.mMin ? slider.mMin : (value > slider.mMax ? slider.mMax : value);
    if (value == slider.mValue)
    {
        return kNothing;
    }
    PortSettings::Change(static_cast<PortSettings::Id>(item.mArg), value - slider.mValue);
    return value < slider.mValue ? kLowered : kRaised;
}

// $184000 in every language, as the HUD and the original frontend's bonus results show money.
void MoneyText(s32 money, wchar_t* pOut)
{
    swprintf(pOut, money < 0 ? L"-$%d" : L"$%d", money < 0 ? -money : money);
}

time_t Midnight(tm day)
{
    day.tm_hour = day.tm_min = day.tm_sec = 0;
    day.tm_isdst = -1;
    return mktime(&day);
}

// TODAY 21:40, or 21:40 under a DATE heading; YESTERDAY 18:05, 3 DAYS AGO.
void DateText(long long fileTime, wchar_t* pOut, bool underHeading = false)
{
    const time_t t = static_cast<time_t>(fileTime);
    const time_t now = time(NULL);
    const tm* pLocal = fileTime >= 0 ? localtime(&t) : NULL;
    if (!pLocal)
    {
        pOut[0] = 0;
        return;
    }
    const tm local = *pLocal;
    const tm today = *localtime(&now);
    // Half a day more: a day of a daylight saving change is 23 or 25 hours long.
    const long long days = (static_cast<long long>(Midnight(today)) - Midnight(local) + 43200) / 86400;
    if (days >= 2)
    {
        swprintf(pOut, Translate(L"%d DAYS AGO"), static_cast<int>(days));
    }
    else if (days == 0 && underHeading)
    {
        swprintf(pOut, L"%d:%02d", local.tm_hour, local.tm_min);
    }
    else
    {
        swprintf(pOut, L"%ls %d:%02d", Translate(days == 1 ? L"YESTERDAY" : L"TODAY"), local.tm_hour, local.tm_min);
    }
}

// JUST NOW, 8 MIN AGO, then as DateText.
void AgeText(long long fileTime, wchar_t* pOut)
{
    const long long age = static_cast<long long>(time(NULL)) - fileTime;
    if (fileTime >= 0 && age < 60)
    {
        wcscpy(pOut, Translate(L"JUST NOW"));
    }
    else if (fileTime >= 0 && age < 3600)
    {
        swprintf(pOut, Translate(L"%d MIN AGO"), static_cast<int>(age / 60));
    }
    else
    {
        DateText(fileTime, pOut);
    }
}

// A text of the game's text table, upper case as the menu's fonts have it; false if there is none.
bool GameText(const char* pKey, wchar_t* pOut, size_t size)
{
    if (!gText_0x14 || !gText_0x14->KeyExists(pKey))
    {
        return false;
    }
    wcsncpy(pOut, gText_0x14->Find(pKey), size - 1);
    pOut[size - 1] = 0;
    gText_0x14->StrToUpper(pOut);
    return pOut[0] != 0;
}

bool JobTitle(const PortSaves::JobName& job, wchar_t* pOut, size_t size)
{
    char key[16];
    snprintf(key, sizeof(key), "%u", job.mTitle);
    return job.mTitle && GameText(key, pOut, size);
}

// Cut after a word, with "...", to fit that width.
void FitText(wchar_t* pText, f32 width)
{
    size_t len = wcslen(pText);
    while (Width(pText, MenuFont()) > width)
    {
        while (len && pText[len - 1] != L' ')
        {
            len--;
        }
        while (len && pText[len - 1] == L' ')
        {
            len--;
        }
        if (!len)
        {
            return;
        }
        wcscpy(pText + len, L"...");
    }
}

// The space from index from on that cuts the text in two rows of the most even widths, the first at most maxFirst
// wide; -1 if there is none.
s32 BalancedSplit(wchar_t* pText, s32 from, f32 maxFirst = 100000)
{
    s32 split = -1;
    f32 best = 0;
    for (s32 i = from; pText[i]; i++)
    {
        if (pText[i] == L' ')
        {
            pText[i] = 0;
            const f32 first = Width(pText, MenuFont());
            const f32 longest = Max(first, Width(pText + i + 1, MenuFont()));
            pText[i] = L' ';
            if (first <= maxFirst && (split < 0 || longest < best))
            {
                split = i;
                best = longest;
            }
        }
    }
    return split;
}

// The main page's width, from its names to the panel's right edge.
const f32 kRetryWidth = 450;

// RETRY's rows: RETRY: <title> (RESTART while it runs), on two rows if it is too wide, RETRY JOB without one, or
// RETRY FRENZY; then the gang that gave the job, the money then and the checkpoint's age.
s32 RetryRows(wchar_t (*pTexts)[kTextLength])
{
    wchar_t title[kTextLength - 16];
    s32 rows = 1;
    if (sRetry == PortSaves::kBeforeFrenzy || !JobTitle(sRetryJob, title, GTA2_COUNTOF(title)))
    {
        wcscpy(pTexts[0], ItemName(Item{kRetry, 0, false}));
    }
    else
    {
        swprintf(pTexts[0], Translate(sRestart ? L"RESTART: %ls" : L"RETRY: %ls"), title);
        // Too wide: two rows, split in the title.
        const s32 titleStart = static_cast<s32>(wcslen(pTexts[0]) - wcslen(title));
        const s32 from = titleStart > 0 ? titleStart - 1 : 0;
        const s32 split = Width(pTexts[0], MenuFont()) > kRetryWidth ? BalancedSplit(pTexts[0], from, kRetryWidth) : -1;
        if (split > 0)
        {
            wcscpy(pTexts[1], pTexts[0] + split + 1);
            pTexts[0][split] = 0;
            rows = 2;
        }
    }
    wchar_t* pDetails = pTexts[rows];
    pDetails[0] = 0;
    char key[16];
    snprintf(key, sizeof(key), "ganga%u", sRetryJob.mGang);
    if (sRetry == PortSaves::kBeforeJob && sRetryJob.mGang && GameText(key, pDetails, 16))
    {
        wcscat(pDetails, L"   ");
    }
    wchar_t text[kTextLength];
    MoneyText(sRetryMoney, text);
    wcscat(pDetails, text);
    wcscat(pDetails, L"   ");
    AgeText(sRetryTime, text);
    wcscat(pDetails, text);
    return rows + 1;
}

// A setting's value, a binding's inputs by column, a save's details by column then on the second row, or
// RETRY's rows; empty for other items.
void ItemValues(const Item& item, wchar_t (*pTexts)[kTextLength])
{
    for (s32 c = 0; c < kMaxTexts; c++)
    {
        pTexts[c][0] = 0;
    }
    if (item.mKind == kRetry)
    {
        RetryRows(pTexts);
    }
    else if (item.mKind == kSave)
    {
        const PortSaves::Entry& save = sSaves[item.mArg];
        MoneyText(save.mMoney, pTexts[0]);
        if (save.mJobsPassed >= 0)
        {
            swprintf(pTexts[1], L"%d/%d", save.mJobsPassed, save.mJobsTotal);
        }
        DateText(save.mTime, pTexts[2], true);
    }
    else if (item.mKind == kSetting)
    {
        PortSettings::Value(static_cast<PortSettings::Id>(item.mArg), pTexts[0]);
    }
    else if (item.mKind == kRadio)
    {
        wcsncpy(pTexts[0], sRadio, kTextLength - 1);
        pTexts[0][kTextLength - 1] = 0;
    }
    else if (item.mKind == kBinding)
    {
        KeyName(GetKey(item.mArg), pTexts[kKeyColumn]);
        for (s32 c = kAltColumn; c < kColumns; c++)
        {
            const s32 binding = GetBinding(c, item.mArg);
            c == kAltColumn ? AltName(binding, pTexts[c]) : PadName(binding, pTexts[c]);
        }
        if (sCapturing == item.mArg)
        {
            wcscpy(pTexts[sColumn], Translate(L"PRESS"));
        }
    }
}

// Where Draw puts the page in the 640x480 layout.
struct Layout
{
    Item mItems[kMaxItems];
    f32 mY[kMaxItems]; // each line's top
    s32 mCount;
    bool mControls;
    s32 mColumns;
    f32 mX;
    f32 mColumnX[kNumColumns]; // the first, a setting's value
    f32 mLineStep;
    f32 mPanelLeft;
    f32 mPanelRight;
    f32 mUnderPage; // the help's or requirement's first line
    f32 mBottom; // the page's, with the two lines of help it has room for
};

// The first line's top: under the frontend's logo, under its title bar on the pages that hide the logo, or near the
// top of the pause menu's panel, which starts under the HUD's gang bars on a 4:3 screen.
const f32 kListTop = 160;
const f32 kNoLogoListTop = 70;
const f32 kGameListTop = 110;
const f32 kGamePanelTop = 80;
// The GTA2 logo in the frontend background's picture (LevelComplete.tga), on black.
const f32 kGta2LogoLeft = 147;
const f32 kGta2LogoTop = 53;
const f32 kGta2LogoRight = 494;
const f32 kGta2LogoBottom = 144;
// The statistics' rows, from one's top to the next.
const f32 kStatStep = 24;

// The frontend's settings and CONTROLS pages cover the logo, for room.
bool HidesLogo()
{
    return !port_GameRunning() && sPage >= kGamePage && sPage <= kControlsPage;
}

f32 ListTop()
{
    return port_GameRunning() ? kGameListTop : (HidesLogo() ? kNoLogoListTop : kListTop);
}

// From a line's top to the next, or to one set apart; a save's line, and the space after it; from the saves page's
// tabs to its first line. A page that would reach the hints has its lines closer, down to kMinLineStep.
const f32 kLineStep = 16;
const f32 kMinLineStep = 14;
// The space a group's title takes above its first line, half a line more unless it tops the page, and its rule's height.
const f32 kTitleStep = 14;
const f32 kTitleRuleY = 9;
// The frontend's hints, and the lowest the pause menu's go.
const f32 kHintsY = 440;
// The frontend's logos in the bottom corners, and the bottom of a line's letters.
const f32 kLogosTop = 402;
const f32 kLeftLogoRight = 66;
const f32 kRightLogoLeft = 556;
const f32 kTextBottom = 14;
const f32 kGapStep = 24;
const f32 kSaveSpace = 4;
const f32 kTabsStep = 46;
const f32 kHeadingsStep = 20;

f32 Height(const Item& item, f32 lineStep)
{
    wchar_t texts[kMaxTexts][kTextLength];
    if (item.mKind == kRetry)
    {
        return lineStep * RetryRows(texts);
    }
    return item.mKind == kSave ? lineStep + kSaveSpace : lineStep;
}

// From a setting's value to the centre of the arrows the mouse changes it with.
const f32 kArrowGap = 14;

// A group's title in the frontend's small red and white font, a third larger as on the welcome screen, with a dark red
// rule after it up to right.
void DrawTitle(const wchar_t* pTitle, f32 x, f32 y, f32 right)
{
    const f32 size = 4.0f / 3;
    wchar_t text[kTextLength];
    wcsncpy(text, pTitle, kTextLength - 1);
    text[kTextLength - 1] = 0;
    DrawString(text, Fx(sLeft + x * sScale), Fx(sTop + y * sScale), gFrontendFont6, Fx(sScale * size), palette_types_enum::sprites_2,
               0, 0, 0);
    const f32 width = Frontend::GetMaxTextWidth(text, gFrontendFont6) * size;
    Rect(x + width + 6, y + kTitleRuleY, right, y + kTitleRuleY + 1, 0x7A1A1A);
}

// Dim unless asked, on two lines of about the same length if it is wider than right - x; further left, down to left, if a line
// still is.
void DrawHelp(const wchar_t* pText, f32 x, f32 y, f32 left, f32 right, bool dim = true)
{
    const s16 font = MenuFont();
    wchar_t first[96];
    wcsncpy(first, pText, 95);
    first[95] = 0;
    const wchar_t* pSecond = L"";
    const s32 split = Width(pText, font) > right - x ? BalancedSplit(first, 0) : -1;
    if (split >= 0)
    {
        first[split] = 0;
        pSecond = first + split + 1;
    }
    const f32 longest = Max(Width(first, font), Width(pSecond, font));
    x = Max(left, x < right - longest ? x : right - longest);
    Text(first, x, y, font, false, dim);
    Text(pSecond, x, y + kLineStep, font, false, dim);
}

void GetLayout(Layout* pLayout)
{
    if (sKeyNames.empty())
    {
        LoadKeyNames();
    }
    Item* pItems = pLayout->mItems;
    const s32 count = Items(sPage, pItems);
    const bool controls = sPage == kControlsPage;
    const bool saves = sPage == kSavesPage;
    const s32 columns = controls ? kColumns : (saves ? kSaveColumns : 1);

    // The first column at its place, or further right for longer names, each a gap after the longest
    // text of the one before.
    const s16 normal = MenuFont();
    f32 nameWidth = 0;
    f32 widths[kNumColumns] = {};
    for (s32 c = 0; c < columns && controls; c++)
    {
        widths[c] = Width(Translate(kColumnNames[c]), normal);
    }
    wchar_t texts[kMaxTexts][kTextLength];
    for (s32 c = 0; c < columns && saves; c++)
    {
        widths[c] = Width(Translate(kSaveHeadings[c]), normal);
    }
    // The saves' columns where they are in every city, their kinds in what the others leave of the page from x 20.
    for (s32 i = 0; i < sNumSaves && saves; i++)
    {
        ItemValues(Item{kSave, i, false}, texts);
        for (s32 c = 0; c < columns; c++)
        {
            widths[c] = Max(widths[c], Width(texts[c], normal));
        }
    }
    sSaveNameWidth = 610 - widths[0] - widths[1] - widths[2] - 12 * columns;
    for (s32 i = 0; i < (saves ? sNumSaves : count); i++)
    {
        const Item item = saves ? Item{kSave, i, false} : pItems[i];
        if (item.mKind == kSetting || item.mKind == kBinding || item.mKind == kSave || item.mKind == kRadio)
        {
            ItemValues(item, texts);
            nameWidth = Max(nameWidth, Width(ItemName(item), normal));
            for (s32 c = 0; c < columns; c++)
            {
                widths[c] = Max(widths[c], (c ? 0 : TextOffset(item)) + Width(texts[c], normal));
            }
        }
    }
    // The statistics' and the transcript's BACK centred as their rows.
    const bool centred = sPage == kStatsPage || sPage == kTranscriptPage || sPage == kPlayersPage;
    const f32 x = controls ? 80.0f : (saves ? 40.0f : (centred ? 320 - Width(ItemName(pItems[0]), normal) / 2 : 140.0f));
    f32 columnX[kNumColumns];
    f32 right;
    // Narrower gaps for a page wider than the screen even moved left.
    for (f32 gap = controls ? 16.0f : 24.0f; gap >= 8; gap -= 4)
    {
        columnX[0] = Max(controls ? 260.0f : (saves ? 0.0f : 360.0f), x + nameWidth + gap);
        for (s32 c = 1; c < columns; c++)
        {
            columnX[c] = columnX[c - 1] + widths[c - 1] + gap;
        }
        right = columnX[columns - 1] + widths[columns - 1];
        if (right - (x - 20) <= 630)
        {
            break;
        }
    }
    // Inside the frontend's screen: the page moves left, down to x 20.
    f32 shift = right - 630;
    shift = shift < 0 ? 0 : (shift > x - 20 ? x - 20 : shift);

    pLayout->mCount = count;
    pLayout->mControls = controls;
    pLayout->mColumns = columns;
    pLayout->mX = x - shift;
    for (s32 c = 0; c < columns; c++)
    {
        pLayout->mColumnX[c] = columnX[c] - shift;
    }
    pLayout->mPanelLeft = pLayout->mX < 60 ? pLayout->mX - 20 : 40;
    pLayout->mPanelRight = Max(600, right - shift + (controls || saves ? 20 : kArrowGap + 16));
    // In game the hints go under the page, on a panel that ends with them, and two lines are kept under the page for
    // a setting's help or requirement.
    bool help = sPage == kQuitPage && !sNetwork;
    for (s32 i = 0; i < count; i++)
    {
        const PortSettings::Id id = static_cast<PortSettings::Id>(pItems[i].mArg);
        help = help || (pItems[i].mKind == kSetting && (PortSettings::Help(id) || PortSettings::Requirement(id) != PortSettings::kCount));
    }
    const f32 lowest = port_GameRunning() ? kHintsY - kLineStep : kHintsY;
    // The controls page closer, its last line above the frontend's left logo.
    for (f32 step = controls ? kLineStep - 1 : kLineStep;; step--)
    {
        pLayout->mLineStep = step;
        // BACK under the statistics, the briefs or NO MESSAGES YET; the saves under the tabs, BACK under NO SAVE; LOAD GAME's
        // choices under its two lines.
        f32 y = ListTop() + (sPage == kStatsPage ? sStatRows * kStatStep + kGapStep - kLineStep : 0);
        y += sPage == kTranscriptPage ? Max(sTranscript, kLineStep) + kGapStep - kLineStep : 0;
        y += sPage == kLoadPage ? kLineStep + kGapStep : 0;
        y += sPage == kPlayersPage ? sNumPlayers * kLineStep + kGapStep : 0;
        y += saves ? (count == 1 ? kTabsStep - kHeadingsStep + kGapStep : kTabsStep) : 0;
        for (s32 i = 0; i < count; i++)
        {
            if (i)
            {
                y += (pItems[i].mGap ? (controls ? kGapStep - 4 : kGapStep) : step) + Height(pItems[i - 1], step) - step;
            }
            y += pItems[i].mTitle ? kTitleStep + (i ? step / 2 : 0) : 0;
            pLayout->mY[i] = y;
        }
        pLayout->mUnderPage = y + kGapStep + 4;
        pLayout->mBottom = Max(y + Height(pItems[count - 1], step), help ? pLayout->mUnderPage + kLineStep * 2 : 0);
        if (pLayout->mBottom <= lowest || step <= kMinLineStep)
        {
            break;
        }
    }
}

// The line under that layout position, -1 if none.
s32 LineAt(const Layout& layout, f32 x, f32 y)
{
    if (x < layout.mPanelLeft || x >= layout.mPanelRight)
    {
        return -1;
    }
    for (s32 i = 0; i < layout.mCount; i++)
    {
        if (y >= layout.mY[i] - 3 && y < layout.mY[i] + Height(layout.mItems[i], layout.mLineStep) - 3)
        {
            return i;
        }
    }
    return -1;
}

bool MouseMode()
{
#if defined(GTA2_PORT_SDL)
    return port_MouseMode();
#else
    return false;
#endif
}

// A key's or button's picture at x, its middle on that of a line of text at y, if drawn; its width. Smaller in a list:
// the whole multiple of the cell whose button, 14 of its 16 texels, is nearest the letters' height of about 10.
const f32 kGlyphHeight = 16;
const f32 kListGlyphHeight = 10.0f * 16 / 14;
const f32 kTextMiddle = 10;
f32 GlyphAt(PortPrompts::Glyph glyph, f32 x, f32 y, bool draw, bool selected = false, f32 height = kGlyphHeight)
{
    const f32 size = PortPrompts::Size(height * sScale);
    if (draw)
    {
        PortPrompts::Draw(glyph, sLeft + x * sScale, sTop + (y + kTextMiddle) * sScale, size, TextColour(selected, false), false);
    }
    return PortPrompts::Width(glyph, size) / sScale;
}

// The pictures of one or two keys or buttons, or the keys' names without them, then what they do.
struct Hint
{
    PortPrompts::Glyph mGlyphs[2];
    const wchar_t* mKeys;
    const wchar_t* mLabel;
};

// The hint at x if drawn; its width.
f32 HintAt(const Hint& hint, f32 x, f32 y, bool draw)
{
    const s16 font = MenuFont();
    const f32 start = x;
    if (hint.mGlyphs[0] != PortPrompts::kNoGlyph && PortPrompts::Available())
    {
        x += GlyphAt(hint.mGlyphs[0], x, y, draw);
        x += hint.mGlyphs[1] != PortPrompts::kNoGlyph ? 2 + GlyphAt(hint.mGlyphs[1], x + 2, y, draw) : 0;
        x += 6;
    }
    else
    {
        x += TextAt(Translate(hint.mKeys), x, y, font, false, false, draw) + 10;
    }
    return x - start + TextAt(Translate(hint.mLabel), x, y, font, false, false, draw);
}

// The hints at y, between the frontend's two logos (x 70 to 550): longer translations start further left, then
// come closer together.
void DrawHintRow(const Hint* pHints, s32 count, f32 y)
{
    const f32 kLeft = 70;
    const f32 kRight = 550;
    f32 width = 0;
    for (s32 i = 0; i < count; i++)
    {
        width += HintAt(pHints[i], 0, y, false);
    }
    f32 gap = 40;
    f32 x = kRight - width - gap * (count - 1);
    x = x > 120 ? 120 : (x < kLeft ? kLeft : x);
    if (count > 1 && x + width + gap > kRight)
    {
        gap = kRight - x - width > 10 ? kRight - x - width : 10;
    }
    for (s32 i = 0; i < count; i++)
    {
        x += HintAt(pHints[i], x, y, true) + gap;
    }
}

// The keys, or the pad's buttons once the pad was used last. Back on the pause menu's main page resumes the game.
void DrawHints(f32 y)
{
    using PortPrompts::kNoGlyph;
    const bool resume = port_GameRunning() && sPage == kMainPage;
    const bool pad = PadMode();
    // LOAD GAME's other keys for the city: Q and E where they are on this keyboard.
    wchar_t cityKeys[8] = L"Q E";
    PortPrompts::Glyph cityGlyphs[2] = {kNoGlyph, kNoGlyph};
#if defined(GTA2_PORT_SDL)
    const s32 q = port_KeyLetter(DIK_Q);
    const s32 e = port_KeyLetter(DIK_E);
    if (q && e)
    {
        swprintf(cityKeys, L"%lc %lc", static_cast<wchar_t>(q), static_cast<wchar_t>(e));
        cityGlyphs[0] = PortPrompts::Letter(q);
        cityGlyphs[1] = PortPrompts::Letter(e);
    }
#endif
    const Hint kNavigate[] = {
        {{pad ? PortPrompts::kPadSouth : PortPrompts::kKeyEnter, kNoGlyph}, L"ENTER", L"SELECT"},
        {{pad ? PortPrompts::kPadEast : PortPrompts::kKeyEscape, kNoGlyph}, L"ESC", resume ? L"RESUME" : L"BACK"},
        {{pad ? PortPrompts::kPadLeftShoulder : cityGlyphs[0], pad ? PortPrompts::kPadRightShoulder : cityGlyphs[1]}, cityKeys, L"CITY"}};
    // Capture: B cancels on the keyboard columns, Back on the pad column (B can be bound there); Delete
    // clears a second key or a pad input.
    const bool padColumn = sColumn == kPadColumn;
    const PortPrompts::Glyph cancel = pad ? (padColumn ? PortPrompts::kPadBack : PortPrompts::kPadEast) : PortPrompts::kKeyEscape;
    const Hint kCapture[] = {{{cancel, kNoGlyph}, pad && padColumn ? L"BACK" : L"ESC", L"CANCEL"},
                             {{PortPrompts::kKeyDelete, kNoGlyph}, L"DEL", L"CLEAR"}};
    const bool capturing = sCapturing >= 0;
    const s32 count = capturing && sColumn == kKeyColumn ? 1 : (!capturing && sPage == kSavesPage ? 3 : 2);
    DrawHintRow(capturing ? kCapture : kNavigate, count, y);
}

// The saves page's tabs, centred on a row: where each starts and its width.
void TabsLayout(f32* pX, f32* pWidths)
{
    f32 total = 0;
    for (s32 i = 0; i < PortSaves::kNumCities; i++)
    {
        pWidths[i] = Width(Translate(kCities[i]), MenuFont());
        total += pWidths[i];
    }
    const f32 room = (600 - total) / (PortSaves::kNumCities - 1);
    const f32 gap = room < 48 ? room : 48;
    f32 x = 320 - (total + gap * (PortSaves::kNumCities - 1)) / 2;
    for (s32 i = 0; i < PortSaves::kNumCities; i++)
    {
        pX[i] = x;
        x += pWidths[i] + gap;
    }
}

// From a tab row's end to the centre of the marker beside it.
const f32 kTabMarkerGap = 20;

// The city chosen underlined in red, the one under the mouse red, the cities not reached dim; LB and RB, or arrows,
// at the row's ends; NO SAVE in a city without one.
void DrawTabs(const Layout& layout)
{
    f32 x[PortSaves::kNumCities];
    f32 widths[PortSaves::kNumCities];
    TabsLayout(x, widths);
    const f32 y = ListTop();
    const f32 left = x[0] - kTabMarkerGap;
    const f32 right = x[PortSaves::kNumCities - 1] + widths[PortSaves::kNumCities - 1] + kTabMarkerGap;
    if (PadMode())
    {
        GlyphAt(PortPrompts::kPadLeftShoulder, left - GlyphAt(PortPrompts::kPadLeftShoulder, 0, y, false) / 2, y, true);
        GlyphAt(PortPrompts::kPadRightShoulder, right - GlyphAt(PortPrompts::kPadRightShoulder, 0, y, false) / 2, y, true);
    }
    else
    {
        Sprite(kLeftArrowSprite, left, y + 6);
        Sprite(kArrowSprite, right, y + 6);
    }
    for (s32 i = 0; i < PortSaves::kNumCities; i++)
    {
        Text(Translate(kCities[i]), x[i], y, MenuFont(), i == sTab || (i == sTabHover && sTabEnabled[i]), !sTabEnabled[i]);
        if (i == sTab)
        {
            Rect(x[i], y + 17, x[i] + widths[i], y + 19, TextColour(true, false));
        }
    }
    for (s32 c = 0; c < kSaveColumns && layout.mCount > 1; c++)
    {
        Text(Translate(kSaveHeadings[c]), layout.mColumnX[c], y + kTabsStep - kHeadingsStep, MenuFont(), false, true);
    }
    if (layout.mCount == 1)
    {
        Text(Translate(L"NO SAVE"), layout.mX, y + kTabsStep - kHeadingsStep, MenuFont(), false, true);
    }
}

// The tab under that layout position, -1 if none; kNumCities for the marker before the row, kNumCities + 1 after it.
s32 TabAt(f32 x, f32 y)
{
    f32 tabX[PortSaves::kNumCities];
    f32 widths[PortSaves::kNumCities];
    TabsLayout(tabX, widths);
    if (y < ListTop() - 4 || y >= ListTop() + 20)
    {
        return -1;
    }
    const f32 right = tabX[PortSaves::kNumCities - 1] + widths[PortSaves::kNumCities - 1];
    if (x >= tabX[0] - kTabMarkerGap - 14 && x < tabX[0] - 8)
    {
        return PortSaves::kNumCities;
    }
    if (x >= right + 8 && x < right + kTabMarkerGap + 14)
    {
        return PortSaves::kNumCities + 1;
    }
    for (s32 i = 0; i < PortSaves::kNumCities; i++)
    {
        if (x >= tabX[i] - 8 && x < tabX[i] + widths[i] + 8)
        {
            return i;
        }
    }
    return -1;
}

// That city, or with a step the next one that can be chosen that way, round the row; its newest save selected.
Result ChangeTab(s32 tab, s32 step)
{
    const s32 n = PortSaves::kNumCities;
    tab = (tab % n + n) % n;
    for (s32 i = 0; i < n && step && !sTabEnabled[tab]; i++)
    {
        tab = (tab + step + n) % n;
    }
    if (!sTabEnabled[tab] || tab == sTab)
    {
        return kNothing;
    }
    const Result result = (step ? step < 0 : tab < sTab) ? kLowered : kRaised;
    sTab = tab;
    sSelected[kSavesPage] = 0;
    return result;
}

// Drops the mouse's moves and clicks so far.
void DiscardMouse()
{
#if defined(GTA2_PORT_SDL)
    float x;
    float y;
    int left;
    int right;
    int wheel;
    port_TakeMouse(&x, &y, &left, &right, &wheel);
#endif
}
} // namespace

bool PadMode()
{
#if defined(GTA2_PORT_SDL)
    return port_PadMode();
#else
    return false;
#endif
}

void Open()
{
    sPage = kMainPage;
    sSelected[kMainPage] = 0;
    sRetry = PortSaves::kNoCheckpoint;
    sStatRows = 0;
    sTranscript = -1;
    sNumSaves = 0;
    sQuitChoices = false;
    sQuitBlocked = NULL;
    sRadio = NULL;
    sNetwork = false;
    sNumPlayers = 0;
    sReady = 0;
    sVote[0] = 0;
    sCapturing = -1;
    sDragging = false;
    DiscardMouse();
#if defined(GTA2_PORT_SDL)
    port_DeferRegistrySaves(true);
#endif
}

void OfferRetry(PortSaves::Checkpoint kind, s32 money, long long time, const PortSaves::JobName& job, bool selected, bool restart)
{
    sRetry = kind;
    sRestart = restart;
    sRetryMoney = money;
    sRetryTime = time;
    sRetryJob = job;
    sSelected[kMainPage] = kind != PortSaves::kNoCheckpoint && !selected ? 1 : 0;
}

void OfferStatistics(s32 rows)
{
    sStatRows = rows;
}

void OfferTranscript(f32 height)
{
    sTranscript = height;
}

bool OnTranscript(f32* pTop, f32* pRoom)
{
    *pTop = ListTop();
    *pRoom = kHintsY - kLineStep - kGapStep - ListTop();
    return sPage == kTranscriptPage;
}

bool OnStatistics(f32* pTop, f32* pStep)
{
    *pTop = ListTop();
    *pStep = kStatStep;
    return sPage == kStatsPage;
}

void OfferRadio(const wchar_t* pStation)
{
    sRadio = pStation;
}

void OfferQuitChoices(const wchar_t* pBlocked)
{
    sQuitChoices = true;
    sQuitBlocked = pBlocked;
}

void OfferSaves(const PortSaves::Entry* pEntries, s32 count, const bool* pCities)
{
    sNumSaves = count < PortSaves::kMaxSaves ? count : PortSaves::kMaxSaves;
    memcpy(sSaves, pEntries, sNumSaves * sizeof(sSaves[0]));
    sTab = -1;
    for (s32 c = 0; c < PortSaves::kNumCities; c++)
    {
        sTabEnabled[c] = pCities[c];
        for (s32 i = 0; i < sNumSaves; i++)
        {
            sTabEnabled[c] = sTabEnabled[c] || sSaves[i].mMainStage == c;
        }
        sTab = sTab < 0 && sTabEnabled[c] ? c : sTab;
    }
    sTab = sNumSaves ? sSaves[0].mMainStage : (sTab < 0 ? 0 : sTab);
}

void OpenSaves(const PortSaves::Entry* pEntries, s32 count, const bool* pCities)
{
    Open();
    OfferSaves(pEntries, count, pCities);
    GoTo(kSavesPage);
}

void OfferNetwork(const NetworkPlayer* pPlayers, s32 count, const wchar_t* pScoreHeading, s32 ready, const wchar_t* pVote)
{
    Item items[kMaxItems];
    s32 n = Items(kMainPage, items);
    s32& selected = sSelected[kMainPage];
    const Item was = items[selected < n ? selected : n - 1];
    sNetwork = true;
    sNumPlayers = count < kMaxNetworkPlayers ? count : kMaxNetworkPlayers;
    memcpy(sPlayers, pPlayers, sNumPlayers * sizeof(sPlayers[0]));
    sScoreHeading = pScoreHeading;
    sReady = ready;
    wcsncpy(sVote, pVote ? pVote : L"", kTextLength - 1);
    sVote[kTextLength - 1] = 0;
    n = Items(kMainPage, items);
    s32 i = 0;
    while (i < n && !(items[i].mKind == was.mKind && items[i].mArg == was.mArg))
    {
        i++;
    }
    // A line gone (a vote ended): the one in its place.
    selected = i < n ? i : (selected < n ? selected : n - 1);
}

bool OnLeaveQuestion()
{
    return sNetwork && sPage == kQuitPage;
}

bool OnSaves()
{
    return sPage == kSavesPage;
}

const PortSaves::Entry& SelectedSave()
{
    Item items[kMaxItems];
    const s32 count = Items(kSavesPage, items);
    const s32 selected = sSelected[kSavesPage];
    return sSaves[selected < count && items[selected].mKind == kSave ? items[selected].mArg : 0];
}

void Close()
{
#if defined(GTA2_PORT_SDL)
    port_DeferRegistrySaves(false);
#endif
}

Result HandleInput(Input input)
{
    Item items[kMaxItems];
    const s32 count = Items(sPage, items);
    s32& selected = sSelected[sPage];
    const Item& item = items[selected];
    switch (input)
    {
        case kUp:
            selected = (selected + count - 1) % count;
            return kMovedUp;
        case kDown:
            selected = (selected + 1) % count;
            return kMovedDown;
        case kLeft:
        case kRight:
            if (sPage == kSavesPage)
            {
                return ChangeTab(sTab + (input == kLeft ? -1 : 1), input == kLeft ? -1 : 1);
            }
            if (item.mKind == kRadio)
            {
                return input == kLeft ? kPreviousStation : kNextStation;
            }
            if (!IsEnabled(item))
            {
                return kNothing;
            }
            if (item.mKind == kSetting)
            {
                PortSettings::Change(static_cast<PortSettings::Id>(item.mArg), input == kLeft ? -1 : 1);
                return input == kLeft ? kLowered : kRaised;
            }
#if defined(GTA2_PORT_SDL)
            if (item.mKind == kBinding)
            {
                const s32 column = sColumn + (input == kLeft ? -1 : 1);
                if (column < 0 || column >= kColumns)
                {
                    return kNothing;
                }
                sColumn = column;
                return input == kLeft ? kLowered : kRaised;
            }
#endif
            return kNothing;
        case kSelect:
            switch (item.mKind)
            {
                case kGoTo:
                    GoTo(static_cast<Page>(item.mArg));
                    return kEntered;
                case kSetting:
                    if (!IsEnabled(item))
                    {
                        return kNothing;
                    }
                    PortSettings::Change(static_cast<PortSettings::Id>(item.mArg), 1);
                    return kRaised;
                case kBinding:
#if defined(GTA2_PORT_SDL)
                    if (item.mArg == kScoresControl && sColumn == kPadColumn)
                    {
                        return kNothing; // Back, whose short press is the network game's ready and yes
                    }
                    port_CaptureStart();
#else
                    if (!gFrontend)
                    {
                        return kNothing;
                    }
                    memcpy(sPrevKeys, gFrontend->mKeys, sizeof(sPrevKeys));
#endif
                    sCapturing = item.mArg;
                    return kEntered;
                case kDefaultKeys:
                    DefaultKeys();
                    return kEntered;
                case kBack:
                    return Back();
                case kResume:
                    return kClosed;
                case kRadio:
                    return kNextStation;
                case kReady:
                    return kReadyChosen;
                case kVoteYes:
                    return kVotedYes;
                case kVoteNo:
                    return kVotedNo;
                case kSave:
                    return kLoadSave;
                case kRetry:
                    return kRetryCheckpoint;
                case kContinue:
                    GoTo(kSavesPage);
                    sTab = sSaves[0].mMainStage;
                    return kEntered;
                case kSaveAndQuit:
                    return IsEnabled(item) ? kSaveAndQuitGame : kNothing;
                default:
                    if ((sQuitChoices || sNetwork) && !item.mArg)
                    {
                        GoTo(kQuitPage);
                        return kEntered;
                    }
                    return kQuitGame;
            }
        default:
            return Back();
    }
}

bool IsCapturing()
{
    return sCapturing >= 0;
}

// F1 to F12 stay the game's own keys (brief, zone name, chat, debug cameras), never a control.
bool IsFunctionKey(s32 key)
{
    return (key >= DIK_F1 && key <= DIK_F10) || key == DIK_F11 || key == DIK_F12;
}

Result UpdateCapture()
{
    if (sCapturing < 0)
    {
        return kNothing;
    }
    // Escape cancels; B cancels on the keyboard columns, Back on the pad column.
#if defined(GTA2_PORT_SDL)
    const s32 key = port_CapturedKey();
    const s32 pad = port_CapturedPad();
#else
    const s32 key = FrontendKeyPressed();
    const s32 pad = -1;
#endif
    const bool padColumn = sColumn == kPadColumn;
    if (key == DIK_ESCAPE || (!padColumn && pad == kPadB) || (padColumn && pad == kPadBack))
    {
        sCapturing = -1;
        return kWentBack;
    }
    if (sColumn != kKeyColumn && (key == DIK_DELETE || key == DIK_BACK))
    {
        SetBinding(sColumn, sCapturing, padColumn ? -1 : 0);
        sCapturing = -1;
        return kLowered;
    }
    const s32 binding = padColumn ? pad : key;
    if (binding < 0 || (!padColumn && (binding == 0 || IsFunctionKey(binding))) ||
        (sColumn == kKeyColumn && binding >= kMouseCode))
    {
        return kNothing;
    }
    Bind(sColumn, sCapturing, binding);
    sCapturing = -1;
    // A click bound stays out of the menu.
    DiscardMouse();
    return kEntered;
}

Result HandleMouse(f32 scale, f32 left, f32 top)
{
#if defined(GTA2_PORT_SDL)
    float frameX;
    float frameY;
    int leftClicks;
    int rightClicks;
    int wheel;
    const bool moved = port_TakeMouse(&frameX, &frameY, &leftClicks, &rightClicks, &wheel);
    if (sCapturing >= 0)
    {
        // The right button cancels, but binds as a second key.
        if (rightClicks && sColumn != kAltColumn)
        {
            sCapturing = -1;
            return kWentBack;
        }
        return kNothing;
    }
    if (rightClicks)
    {
        sDragging = false;
        return Back();
    }
    const f32 x = (frameX - left) / scale;
    const f32 y = (frameY - top) / scale;
    const s32 tab = sPage == kSavesPage ? TabAt(x, y) : -1;
    sTabHover = moved ? (tab < PortSaves::kNumCities ? tab : -1) : sTabHover;
    if (tab >= 0 && leftClicks)
    {
        const s32 step = tab == PortSaves::kNumCities ? -1 : 1;
        return tab < PortSaves::kNumCities ? ChangeTab(tab, 0) : ChangeTab(sTab + step, step);
    }
    Layout layout;
    GetLayout(&layout);
    s32& selected = sSelected[sPage];
    sDragging = sDragging && port_MouseLeftDown();
    if (sDragging)
    {
        return SetSliderAt(layout.mItems[selected], x, layout.mColumnX[0]);
    }
    const s32 line = LineAt(layout, x, y);
    if (line < 0)
    {
        return kNothing;
    }
    const Item& item = layout.mItems[line];
    Result result = kNothing;
    if (moved || leftClicks || wheel)
    {
        if (line != selected)
        {
            result = line < selected ? kMovedUp : kMovedDown;
            selected = line;
        }
        // Over a binding's input, that column.
        for (s32 c = 0; c < layout.mColumns && item.mKind == kBinding; c++)
        {
            sColumn = x >= layout.mColumnX[c] - kArrowGap - 6 ? c : sColumn;
        }
    }
    if (!IsEnabled(item))
    {
        return result;
    }
    Slider slider;
    if (leftClicks && GetSlider(item, &slider) && x >= layout.mColumnX[0] - 6 && x < layout.mColumnX[0] + kSliderWidth + 6)
    {
        sDragging = true;
        const Result changed = SetSliderAt(item, x, layout.mColumnX[0]);
        return changed != kNothing ? changed : result;
    }
    if ((item.mKind == kSetting || item.mKind == kRadio) && (wheel || leftClicks))
    {
        // The left arrow lowers, the rest of the line raises.
        const bool lower = wheel ? wheel < 0 : x >= layout.mColumnX[0] - kArrowGap * 2 && x < layout.mColumnX[0] - 2;
        if (item.mKind == kRadio)
        {
            return lower ? kPreviousStation : kNextStation;
        }
        PortSettings::Change(static_cast<PortSettings::Id>(item.mArg), lower ? -1 : 1);
        return lower ? kLowered : kRaised;
    }
    if (leftClicks)
    {
        return HandleInput(kSelect);
    }
    return result;
#else
    return kNothing;
#endif
}

const wchar_t* Title()
{
    return Translate(sNetwork && sPage == kQuitPage ? L"LEAVE GAME" : kTitles[sPage]);
}

// PLAYERS: a heading, then a line per player, the local one in red.
static void DrawPlayers()
{
    const s16 font = MenuFont();
    const f32 kNameX = 80;
    const f32 kRights[3] = {350, 460, 560};
    const auto row = [&](wchar_t (*pTexts)[kTextLength], f32 y, bool selected, bool dim) {
        Text(pTexts[0], kNameX, y, font, selected, dim);
        for (s32 c = 1; c < 4; c++)
        {
            Text(pTexts[c], kRights[c - 1] - Width(pTexts[c], font), y, font, selected, dim);
        }
    };
    wchar_t texts[4][kTextLength];
    const wchar_t* const headings[4] = {L"PLAYER", sScoreHeading, L"DEATHS", L"PING"};
    for (s32 c = 0; c < 4; c++)
    {
        swprintf(texts[c], L"%ls", Translate(headings[c]));
    }
    row(texts, ListTop(), false, true);
    for (s32 i = 0; i < sNumPlayers; i++)
    {
        const NetworkPlayer& player = sPlayers[i];
        swprintf(texts[0], L"%ls", player.mName);
        FitText(texts[0], kRights[0] - kNameX - 60);
        swprintf(texts[1], L"%d", player.mScore);
        swprintf(texts[2], L"%d", player.mDeaths);
        swprintf(texts[3], player.mPing < 0 ? L"-" : L"%d", player.mPing);
        row(texts, ListTop() + (i + 1) * kLineStep, player.mYou, false);
    }
}

void Draw(f32 scale, f32 left, f32 top, bool panel)
{
    sScale = scale;
    sLeft = left;
    sTop = top;
    Layout layout;
    GetLayout(&layout);
    const s16 normal = MenuFont();
    const f32 underPage = layout.mUnderPage;
    const bool inGame = port_GameRunning();
    const f32 hintsY = inGame ? layout.mBottom + kLineStep : kHintsY;
    // The help and requirement lines inside the panel, or between the frontend's logos once that low.
    const bool logos = !inGame && underPage + kLineStep + kTextBottom > kLogosTop;
    const f32 helpLeft = inGame ? layout.mPanelLeft + 10 : (logos ? kLeftLogoRight : 20);
    const f32 helpRight = logos ? kRightLogoLeft : layout.mPanelRight - 10;
    if (panel)
    {
        Rect(layout.mPanelLeft, kGamePanelTop, layout.mPanelRight, hintsY + kGameListTop - kGamePanelTop, 0);
    }
    if (HidesLogo())
    {
        Rect(kGta2LogoLeft, kGta2LogoTop, kGta2LogoRight, kGta2LogoBottom, 0);
    }
    for (s32 c = 0; c < layout.mColumns && layout.mControls; c++)
    {
        Text(Translate(kColumnNames[c]), layout.mColumnX[c], ListTop() - 20, normal);
    }
    if (sPage == kSavesPage)
    {
        DrawTabs(layout);
    }
    if (sPage == kTranscriptPage && !sTranscript)
    {
        const wchar_t* pEmpty = Translate(L"NO MESSAGES YET");
        Text(pEmpty, 320 - Width(pEmpty, normal) / 2, ListTop(), normal, false, true);
    }
    if (sPage == kPlayersPage)
    {
        DrawPlayers();
    }
    if (sPage == kLoadPage)
    {
        const wchar_t* pQuestion = Translate(L"PROGRESS SINCE YOUR LAST SAVE WILL BE LOST");
        DrawHelp(pQuestion, layout.mX, ListTop(), layout.mX, layout.mPanelRight - 10, false);
    }
    wchar_t texts[kMaxTexts][kTextLength];
    for (s32 i = 0; i < layout.mCount; i++)
    {
        const Item& item = layout.mItems[i];
        const f32 y = layout.mY[i];
        const bool selected = i == sSelected[sPage];
        const bool dim = !IsEnabled(item);
        ItemValues(item, texts);
        if (item.mTitle)
        {
            DrawTitle(Translate(item.mTitle), layout.mX, y - kTitleStep, layout.mPanelRight - 30);
        }
        if (item.mKind == kRetry)
        {
            const s32 rows = RetryRows(texts);
            for (s32 r = 0; r < rows; r++)
            {
                Text(texts[r], layout.mX, y + r * layout.mLineStep, normal, selected, r == rows - 1 && !selected);
            }
            continue;
        }
        Text(ItemName(item), layout.mX, y, normal, selected, dim);
        Slider slider;
        if (GetSlider(item, &slider))
        {
            DrawSlider(slider, layout.mColumnX[0], y, selected, dim);
        }
        const f32 valueX = layout.mColumnX[0] + TextOffset(item);
        Text(texts[0], valueX, y, normal, selected, dim);
        if (selected && dim && item.mKind == kSetting)
        {
            DrawRequirement(item, Max(layout.mX, helpLeft), underPage);
        }
        const wchar_t* pHelp = item.mKind == kSetting ? PortSettings::Help(static_cast<PortSettings::Id>(item.mArg)) : NULL;
        // QUIT GAME's choices: why SAVE AND QUIT is greyed, what QUIT loses.
        const wchar_t* pLost = L"PROGRESS SINCE YOUR LAST SAVE WILL BE LOST";
        pHelp = item.mKind == kSaveAndQuit ? sQuitBlocked : (item.mKind == kQuit && item.mArg && !sNetwork ? pLost : pHelp);
        if (selected && (!dim || item.mKind != kSetting) && pHelp)
        {
            DrawHelp(Translate(pHelp), Max(layout.mX, helpLeft), underPage, helpLeft, helpRight);
        }
        for (s32 c = 1; c < layout.mColumns && (item.mKind == kBinding || item.mKind == kSave); c++)
        {
            // A pad button's picture, as printed on the pad used last; the directions in words.
            const s32 code = item.mKind == kBinding && c == kPadColumn && sCapturing != item.mArg ? GetBinding(c, item.mArg) : -1;
            if (code >= 0 && code < 12 && PortPrompts::Available())
            {
                GlyphAt(static_cast<PortPrompts::Glyph>(code), layout.mColumnX[c], y, true, selected, kListGlyphHeight);
            }
            else
            {
                Text(texts[c], layout.mColumnX[c], y, normal, selected);
            }
        }
        if (selected && item.mKind == kBinding && layout.mColumns > 1)
        {
            Sprite(kArrowSprite, layout.mColumnX[sColumn] - 12, y + 6);
        }
        // RADIO's arrows whatever the input, as its previous and next.
        if (selected && ((item.mKind == kSetting && !dim && MouseMode()) || item.mKind == kRadio))
        {
            Sprite(kLeftArrowSprite, layout.mColumnX[0] - kArrowGap, y + 6);
            Sprite(kArrowSprite, valueX + Width(texts[0], normal) + kArrowGap, y + 6);
        }
    }
    DrawHints(hintsY);
}

void DrawChoiceHints()
{
    using PortPrompts::kNoGlyph;
    sScale = 1;
    sLeft = 0;
    sTop = 0;
    const bool pad = PadMode();
    const Hint kHints[] = {{{pad ? PortPrompts::kPadSouth : PortPrompts::kKeyEnter, kNoGlyph}, L"ENTER", L"SELECT"},
                           {{pad ? PortPrompts::kPadEast : PortPrompts::kKeyEscape, kNoGlyph}, L"ESC", L"BACK"}};
    DrawHintRow(kHints, 2, kHintsY);
}

f32 DrawHintGlyph(PortPrompts::Glyph glyph, f32 x, f32 y)
{
    sScale = 1;
    sLeft = 0;
    sTop = 0;
    return GlyphAt(glyph, x, y, true, false, kListGlyphHeight) + 6;
}
} // namespace PortMenu
