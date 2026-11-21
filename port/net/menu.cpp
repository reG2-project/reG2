// port/net (port/docs/netcode.md, "Playing"): a network page in the game's frontend. The main menu gets
// "Network game" (between options and quit); its page takes a server's address and a name, and joins in the
// background (NetMenu_Join, NetMenu_Poll: the menu runs on meanwhile). Once the server welcomes us the network game is
// set up as the command line's --connect does, and the frontend boots it as it boots a level (Booting_Map_2), with
// the game's own network start filling in the session (Start_NetworkGame). The page is one of the frontend's
// unused ones (13); the game draws it, this file takes its input (gPortFrontendInput) and writes its labels. The
// address and the name are kept in gta2_registry.ini, beside the game's own network settings.
#include "net_main.hpp"
#include "port/src/net_hooks.hpp"
#include "Source/Draw.hpp"
#include "Source/Frontend.hpp"
#include "Source/SoundObject.hpp"
#include "Source/winmain.hpp"
#include <ctype.h>
#include <dinput.h>
#include <string>
#include <string.h>
#include <vector>

EXTERN_GLOBAL(HINSTANCE, gHInstance);
unsigned short port_TypedChar(int dik); // compat/src/dinput.cpp: the character a key's last press typed, with shift
EXTERN_GLOBAL(SoundObject, snd1); // the frontend's sound: 1 up, 2 down, 5 enter, 6 back, 7 a key, 8 a deletion

namespace
{
const u16 kPage = MENUPAGE_13; // 12 is the options's (port/src/options_menu.cpp)
const u16 kNoTarget = 268; // an ENTERABLE option's target the game does nothing for (UpdatePageFromUserInput)
const int kStatusLines = 6; // the status's lines, under the options down to the bottom (a refusal names two builds)
const s16 kStatusY = 350;
enum
{
    kAddress,
    kName,
    kJoin,
    kOptions
};

std::string gAddress, gName, gStatus; // the address: the last one joined, none the first time
int gEditing = -1; // the field being typed into (kAddress, kName), or -1
std::string gBefore; // the field as it was: Esc gives it back
bool gJoining;
unsigned gBlink;
char gPrevKeys[256];

const char* const kSettings = "SOFTWARE\\DMA Design Ltd\\GTA2\\Network";

std::string Load(const char* name)
{
    HKEY key;
    char text[64] = {};
    DWORD size = sizeof(text) - 1;
    if (RegOpenKeyExA(HKEY_LOCAL_MACHINE, kSettings, 0, KEY_ALL_ACCESS, &key) != ERROR_SUCCESS)
    {
        return "";
    }
    if (RegQueryValueExA(key, name, 0, 0, (LPBYTE)text, &size) != ERROR_SUCCESS)
    {
        text[0] = 0;
    }
    RegCloseKey(key);
    return text;
}

void Save()
{
    HKEY key;
    DWORD disposition;
    if (RegCreateKeyExA(HKEY_LOCAL_MACHINE, kSettings, 0, 0, 0, KEY_ALL_ACCESS, 0, &key, &disposition) != ERROR_SUCCESS)
    {
        return;
    }
    RegSetValueExA(key, "ServerAddress", 0, REG_SZ, (const BYTE*)gAddress.c_str(), (DWORD)gAddress.size() + 1);
    RegSetValueExA(key, "PlayerName", 0, REG_SZ, (const BYTE*)gName.c_str(), (DWORD)gName.size() + 1); // the game's own
    RegCloseKey(key);
}

// Into one of the frontend's 50-character labels. By hand, not with the C library's wide functions or
// strings: the game's wchar_t is 2 bytes (-fshort-wchar), the library's 4
void SetText(wchar_t* out, const std::string& text)
{
    size_t n = 0;
    for (; n < text.size() && n < 49; n++)
    {
        out[n] = (wchar_t)(unsigned char)toupper((unsigned char)text[n]);
    }
    out[n] = 0;
}

// The status in lines that fit right of the picture, and left of the Rockstar logo near the bottom; what doesn't fit
// is left out
std::vector<std::string> StatusLines(const std::string& text, u16 font)
{
    std::vector<std::string> lines(1);
    const s32 height = GetLineSpacingFromFontType(font);
    size_t at = 0;
    while (at < text.size())
    {
        const size_t space = text.find(' ', at);
        const std::string word = text.substr(at, space == std::string::npos ? std::string::npos : space - at);
        at = space == std::string::npos ? text.size() : space + 1;
        const std::string longer = lines.back().empty() ? word : lines.back() + " " + word;
        wchar_t wide[50];
        SetText(wide, longer);
        const s32 y = kStatusY + (s32)(lines.size() - 1) * height;
        const s32 room = y + height > 400 ? 250 : 330; // the logo: x 560 and on, from y 400
        if (lines.back().empty() || Frontend::GetMaxTextWidth(wide, font) <= room)
        {
            lines.back() = longer;
        }
        else if ((int)lines.size() < kStatusLines)
        {
            lines.push_back(word);
        }
        else
        {
            break;
        }
    }
    return lines;
}

// The character a key types into an address or a name, uppercase as the frontend's fonts draw them: the keyboard
// layout's (the game's own keyboard table has no shift), the keypad's where the layout gives none
char KeyChar(int key)
{
    const unsigned short typed = port_TypedChar(key);
    if (typed && typed < 128)
    {
        return (char)toupper(typed);
    }
    if (key >= DIK_NUMPAD7 && key <= DIK_NUMPAD9)
    {
        return (char)('7' + key - DIK_NUMPAD7);
    }
    if (key >= DIK_NUMPAD4 && key <= DIK_NUMPAD6)
    {
        return (char)('4' + key - DIK_NUMPAD4);
    }
    if (key >= DIK_NUMPAD1 && key <= DIK_NUMPAD3)
    {
        return (char)('1' + key - DIK_NUMPAD1);
    }
    return key == DIK_NUMPAD0 ? '0' : key == DIK_DECIMAL ? '.' : 0;
}

std::string& Field(int f)
{
    return f == kAddress ? gAddress : gName;
}

// A key pressed this frame into the field being typed into; Enter keeps it, Esc gives back what it was
void Type(const bool* pressed)
{
    std::string& text = Field(gEditing);
    const size_t maxLength = gEditing == kAddress ? 40 : 15;
    for (int k = 0; k < 256; k++)
    {
        if (!pressed[k])
        {
            continue;
        }
        if (k == DIK_RETURN || k == DIK_NUMPADENTER)
        {
            gEditing = -1;
            Save();
            snd1.mObjectType = 5;
            return;
        }
        if (k == DIK_ESCAPE)
        {
            text = gBefore;
            gEditing = -1;
            snd1.mObjectType = 6;
            return;
        }
        if (k == DIK_BACK)
        {
            if (!text.empty())
            {
                text.erase(text.size() - 1);
                snd1.mObjectType = 8;
            }
            continue;
        }
        // The name entry drops what its font has no width for
        const char c = KeyChar(k);
        const bool common = (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-';
        const bool allowed = common || (gEditing == kAddress ? c == '.' || c == ':' : c == '_');
        if (allowed && text.size() < maxLength)
        {
            text += c;
            snd1.mObjectType = 7;
        }
    }
}

// The page's labels for this frame: the fields' text (and the cursor where one is typed into), the join's state
void Refresh(Frontend* pFrontend)
{
    MenuPage& page = pFrontend->mMenuPagesArray[kPage];
    const bool cursor = (++gBlink / 8) % 2;
    SetText(page.mOptionsArray[kAddress].mOptionNameStr, "Address   " + gAddress + (gEditing == kAddress && cursor ? "_" : ""));
    SetText(page.mOptionsArray[kName].mOptionNameStr, "Name   " + gName + (gEditing == kName && cursor ? "_" : ""));
    SetText(page.mOptionsArray[kJoin].mOptionNameStr, gJoining ? "Connecting..." : "Join game");
    const std::vector<std::string> lines = StatusLines(gStatus, page.mElementsArray[0].mFontType);
    for (int i = 0; i < kStatusLines; i++)
    {
        SetText(page.mElementsArray[i].mElementNameStr, i < (int)lines.size() ? lines[i] : "");
    }
}

// The server welcomed us: the game's network start (as -N does before the frontend), then the frontend boots it
void StartGame(Frontend* pFrontend)
{
    bStartNetworkGame = 1;
    if (!Start_NetworkGame(gHInstance))
    {
        bStartNetworkGame = 0;
        NetMenu_Cancel();
        gStatus = "the network game didn't start";
        return;
    }
    pFrontend->mMenuScreen = RedBar_16;
    pFrontend->mState = FrontendState::Booting_Map_2;
}

void Setup(Frontend* pFrontend)
{
    // The main menu: "Network game" after options, quit moved down a line
    MenuPage& main = pFrontend->mMenuPagesArray[MENUPAGE_START_MENU];
    main.mOptionsArray[3] = main.mOptionsArray[2];
    main.mOptionsArray[3].mYPos += 20;
    main.mOptionStates[3] = main.mOptionStates[2];
    MenuItem& item = main.mOptionsArray[2];
    SetText(item.mOptionNameStr, "Network game");
    item.mMenuPageTarget = kPage;
    main.mNumberOfOptions = 4;

    MenuPage& page = pFrontend->mMenuPagesArray[kPage];
    page.mNumberOfOptions = kOptions;
    // Where the main menu's and the play page's are: right of the picture, on the black
    const s16 ys[kOptions] = {250, 270, 310};
    for (int i = 0; i < kOptions; i++)
    {
        page.mOptionsArray[i].mOptionType = ENTERABLE_1;
        page.mOptionsArray[i].mXPos = 300;
        page.mOptionsArray[i].mYPos = ys[i];
        page.mOptionsArray[i].mMenuPageTarget = kNoTarget;
    }
    page.mCurrentOptionIdx = kJoin;
    page.mDefaultOptionIdx = kJoin;
    page.mNumberOfElements = kStatusLines;
    for (int i = 0; i < kStatusLines; i++)
    {
        MenuElement& status = page.mElementsArray[i];
        status.mElementType = STRING_TEXT_1;
        status.mXpos = 300;
        status.mYpos = (s16)(kStatusY + i * GetLineSpacingFromFontType(pFrontend->mNormalFont));
        status.mFontType = pFrontend->mNormalFont;
        status.mFontPalette = 0xFFFF;
    }
    static bool loaded;
    if (!loaded)
    {
        loaded = true;
        const std::string address = Load("ServerAddress");
        gAddress = address.empty() ? gAddress : address;
        gName = NetMenu_DefaultName().empty() ? Load("PlayerName") : NetMenu_DefaultName(); // --name, else the saved one
    }
    gName = gName.empty() ? "PLAYER" : gName;
    for (char& c : gName)
    {
        c = (char)toupper((unsigned char)c);
    }
    Refresh(pFrontend);
}

bool Input(Frontend* pFrontend)
{
    if (pFrontend->mCurrentPageIdx == MENUPAGE_START_MENU)
    {
        // The picture beside the main menu (UpdateMenuScreen knows three options): ours shows the options' one
        const u16 option = pFrontend->mMenuPagesArray[MENUPAGE_START_MENU].mCurrentOptionIdx;
        pFrontend->mMenuScreen = option == 2 ? Options_0 : option == 3 ? Quit_2 : pFrontend->mMenuScreen;
        if (option == 2 && pFrontend->mReturnPressed)
        {
            // Ours too: the game's page change draws a level's picture for a frame (UpdateMenuScreen)
            pFrontend->ChangeMenuPage(kPage);
            pFrontend->mMenuPagesArray[kPage].mCurrentOptionIdx = gAddress.empty() ? kAddress : kJoin;
            pFrontend->mMenuScreen = Options_0;
            snd1.mObjectType = 5;
            return true;
        }
        return false;
    }
    if (pFrontend->mCurrentPageIdx != kPage)
    {
        return false;
    }
    pFrontend->mMenuScreen = Options_0; // the main menu's picture for "Network game" (the game's: a level's)
    bool pressed[256];
    for (int k = 0; k < 256; k++)
    {
        const bool down = (pFrontend->mKeys[k] & 0x80) != 0;
        pressed[k] = down && !(gPrevKeys[k] & 0x80);
        gPrevKeys[k] = pFrontend->mKeys[k];
    }
    MenuPage& page = pFrontend->mMenuPagesArray[kPage];
    bool handled = true;
    if (gJoining)
    {
        std::string error;
        const int result = NetMenu_Poll(&error);
        if (result > 0)
        {
            gJoining = false;
            gStatus = "";
            StartGame(pFrontend);
        }
        else if (result < 0)
        {
            gJoining = false;
            gStatus = error;
        }
        else if (pFrontend->mEscapePressed)
        {
            NetMenu_Cancel();
            gJoining = false;
            gStatus = "cancelled";
            snd1.mObjectType = 6;
        }
    }
    else if (gEditing >= 0)
    {
        Type(pressed);
    }
    else if (pFrontend->mReturnPressed)
    {
        const u16 option = page.mCurrentOptionIdx;
        if (option == kAddress || option == kName)
        {
            gEditing = option;
            gBefore = Field(option);
        }
        else if (gAddress.empty() || gName.empty())
        {
            gStatus = gAddress.empty() ? "type the server's address" : "type a name";
        }
        else
        {
            std::string error;
            Save();
            gJoining = NetMenu_Join(gAddress, gName, &error);
            gStatus = gJoining ? "" : error;
        }
        snd1.mObjectType = 5;
    }
    else if (pFrontend->mEscapePressed)
    {
        pFrontend->ChangeMenuPage(MENUPAGE_START_MENU);
        pFrontend->mMenuScreen = Options_0; // "Network game" still chosen there
        snd1.mObjectType = 6;
    }
    else
    {
        handled = false; // up and down: the game's
    }
    Refresh(pFrontend);
    return handled;
}
} // namespace

void NetMenu_SetStatus(const std::string& status)
{
    gStatus = status;
}

int NetMenu_Page()
{
    return kPage;
}

void NetMenu_Install()
{
    gPortFrontendSetup = Setup;
    gPortFrontendInput = Input;
}
