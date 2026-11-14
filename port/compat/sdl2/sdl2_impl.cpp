#ifdef GTA2_PORT_SDL2

#include "SDL3/SDL.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#ifdef _WIN32
#include <windows.h>
#include <commdlg.h>
#include <shlobj.h>
#else
#include <fcntl.h>
#include <unistd.h>
#include <sys/wait.h>
#endif

SDL_DisplayMode** SDL_GetFullscreenDisplayModes(int displayID, int* pCount)
{
    *pCount = 0;
    const int total = SDL_GetNumDisplayModes(displayID);
    if (total < 0)
    {
        return NULL;
    }
    const size_t ptrBytes = (size_t)(total + 1) * sizeof(SDL_DisplayMode*);
    const size_t modeBytes = (size_t)total * sizeof(SDL_DisplayMode);
    SDL_DisplayMode** ppModes = static_cast<SDL_DisplayMode**>(SDL_malloc(ptrBytes + modeBytes));
    if (!ppModes)
    {
        return NULL;
    }
    SDL_DisplayMode* pModes = reinterpret_cast<SDL_DisplayMode*>(reinterpret_cast<char*>(ppModes) + ptrBytes);
    int count = 0;
    for (int i = 0; i < total; i++)
    {
        if (SDL_GetDisplayMode(displayID, i, &pModes[count]) == 0)
        {
            ppModes[count] = &pModes[count];
            count++;
        }
    }
    ppModes[count] = NULL;
    *pCount = count;
    return ppModes;
}

SDL_JoystickID* SDL_GetGamepads(int* pCount)
{
    *pCount = 0;
    const int n = SDL_NumJoysticks();
    if (n < 0)
    {
        return NULL;
    }
    SDL_JoystickID* pIds = static_cast<SDL_JoystickID*>(SDL_malloc(sizeof(SDL_JoystickID) * (size_t)n));
    if (!pIds)
    {
        return NULL;
    }
    int count = 0;
    for (int i = 0; i < n; i++)
    {
        if (SDL_IsGameController(i))
        {
            pIds[count++] = SDL_JoystickGetDeviceInstanceID(i);
        }
    }
    *pCount = count;
    return pIds;
}

const char* SDL_GetGamepadNameForID(SDL_JoystickID id)
{
    const int n = SDL_NumJoysticks();
    for (int i = 0; i < n; i++)
    {
        if (SDL_JoystickGetDeviceInstanceID(i) == id)
        {
            return SDL_GameControllerNameForIndex(i);
        }
    }
    return NULL;
}

SDL_Gamepad* SDL_OpenGamepad(SDL_JoystickID id)
{
    const int n = SDL_NumJoysticks();
    for (int i = 0; i < n; i++)
    {
        if (SDL_JoystickGetDeviceInstanceID(i) == id)
        {
            return SDL_GameControllerOpen(i);
        }
    }
    return NULL;
}

SDL_JoystickID SDL_GetGamepadID(SDL_Gamepad* pPad)
{
    SDL_Joystick* pJoy = pPad ? SDL_GameControllerGetJoystick(pPad) : NULL;
    return pJoy ? SDL_JoystickInstanceID(pJoy) : 0;
}

SDL_GamepadButton SDL_GetGamepadButtonFromString(const char* pName)
{
    SDL_GamepadButton button = SDL_GameControllerGetButtonFromString(pName);
    if (button != SDL_CONTROLLER_BUTTON_INVALID)
    {
        return button;
    }
    struct Alias
    {
        const char* mFrom;
        const char* mTo;
    };
    const Alias aliases[] = {{"south", "a"},
                             {"east", "b"},
                             {"west", "x"},
                             {"north", "y"},
                             {"rightpaddle1", "paddle1"},
                             {"leftpaddle1", "paddle2"},
                             {"rightpaddle2", "paddle3"},
                             {"leftpaddle2", "paddle4"}};
    for (const Alias& alias : aliases)
    {
        if (SDL_strcasecmp(pName, alias.mFrom) == 0)
        {
            return SDL_GameControllerGetButtonFromString(alias.mTo);
        }
    }
    return SDL_CONTROLLER_BUTTON_INVALID;
}

SDL_JoystickID SDL_AttachVirtualJoystick(const SDL_VirtualJoystickDesc* pDesc)
{
    SDL_VirtualJoystickDesc desc;
    SDL_zero(desc);
    desc.version = SDL_VIRTUAL_JOYSTICK_DESC_VERSION;
    desc.type = SDL_JOYSTICK_TYPE_GAMECONTROLLER;
    desc.naxes = pDesc->naxes;
    desc.nbuttons = pDesc->nbuttons;
    desc.name = pDesc->name;
    desc.button_mask = pDesc->nbuttons >= 32 ? 0xFFFFFFFFu : (1u << pDesc->nbuttons) - 1;
    desc.axis_mask = pDesc->naxes >= 32 ? 0xFFFFFFFFu : (1u << pDesc->naxes) - 1;
    return SDL_JoystickAttachVirtualEx(&desc);
}

float SDL_GetDisplayContentScale(int displayID)
{
    float ddpi = 0.0f;
    if (SDL_GetDisplayDPI(displayID, &ddpi, NULL, NULL) == 0 && ddpi > 0.0f)
    {
        return ddpi / 96.0f;
    }
    return 1.0f;
}

int SDL_GetCurrentTime(SDL_Time* pNow)
{
    *pNow = (SDL_Time)time(NULL) * 1000000000ull;
    return *pNow ? 1 : 0;
}

int SDL2_VersionNumber()
{
    SDL_version version;
    SDL_VERSION(&version);
    return SDL_VERSIONNUM(version.major, version.minor, version.patch);
}

SDL_Locale* SDL_GetPreferredLocales_I(...)
{
    return (SDL_GetPreferredLocales)();
}

char* SDL2_ProcessCapture(const char* const* args)
{
#ifdef _WIN32
    SECURITY_ATTRIBUTES inherit = {sizeof(inherit), NULL, TRUE};
    HANDLE readPipe = NULL;
    HANDLE writePipe = NULL;
    if (!CreatePipe(&readPipe, &writePipe, &inherit, 0))
    {
        return NULL;
    }
    SetHandleInformation(writePipe, HANDLE_FLAG_INHERIT, HANDLE_FLAG_INHERIT);
    std::string cmd;
    for (int i = 0; args[i]; i++)
    {
        if (i)
        {
            cmd += " ";
        }
        cmd += args[i];
    }
    char* pCmd = _strdup(cmd.c_str());
    STARTUPINFOA start = {};
    start.cb = sizeof(start);
    start.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
    start.wShowWindow = SW_HIDE;
    start.hStdOutput = writePipe;
    start.hStdError = GetStdHandle(STD_ERROR_HANDLE);
    PROCESS_INFORMATION process = {};
    const BOOL created =
        pCmd && CreateProcessA(NULL, pCmd, NULL, NULL, TRUE, CREATE_NO_WINDOW, NULL, NULL, &start, &process);
    free(pCmd);
    CloseHandle(writePipe);
    std::string out;
    if (created)
    {
        CloseHandle(process.hThread);
        char buffer[4096];
        DWORD read = 0;
        while (ReadFile(readPipe, buffer, sizeof(buffer), &read, NULL) && read)
        {
            out.append(buffer, read);
        }
        WaitForSingleObject(process.hProcess, 10000);
        CloseHandle(process.hProcess);
    }
    CloseHandle(readPipe);
    return out.empty() ? NULL : _strdup(out.c_str());
#else
    int fds[2];
    if (pipe(fds) != 0)
    {
        return NULL;
    }
    const pid_t pid = fork();
    if (pid < 0)
    {
        close(fds[0]);
        close(fds[1]);
        return NULL;
    }
    if (pid == 0)
    {
        close(fds[0]);
        dup2(fds[1], STDOUT_FILENO);
        close(fds[1]);
        const int devnull = open("/dev/null", O_WRONLY);
        if (devnull >= 0)
        {
            dup2(devnull, STDERR_FILENO);
            close(devnull);
        }
        execvp(args[0], const_cast<char* const*>(args));
        _exit(127);
    }
    close(fds[1]);
    std::string out;
    char buffer[4096];
    for (ssize_t n; (n = read(fds[0], buffer, sizeof(buffer))) > 0;)
    {
        out.append(buffer, (size_t)n);
    }
    close(fds[0]);
    waitpid(pid, NULL, 0);
    return out.empty() ? NULL : strdup(out.c_str());
#endif
}

bool SDL2_PickPath(bool folder, const char* pFilter, std::string* pOut)
{
#ifdef _WIN32
    if (!folder)
    {
        OPENFILENAMEA ofn = {};
        char file[MAX_PATH] = "";
        ofn.lStructSize = sizeof(ofn);
        ofn.lpstrFile = file;
        ofn.nMaxFile = sizeof(file);
        ofn.lpstrFilter = "All files (*.*)\0*.*\0";
        if (GetOpenFileNameA(&ofn))
        {
            *pOut = file;
            return true;
        }
        return false;
    }
    CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);
    BROWSEINFOA browse = {};
    browse.lpszTitle = "Choose the game folder";
    browse.ulFlags = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE;
    LPITEMIDLIST pList = SHBrowseForFolderA(&browse);
    if (pList)
    {
        char path[MAX_PATH] = "";
        const bool ok = SHGetPathFromIDListA(pList, path) != FALSE;
        CoTaskMemFree(pList);
        if (ok)
        {
            *pOut = path;
            return true;
        }
    }
    return false;
#elif defined(__APPLE__)
    (void)folder;
    (void)pFilter;
    return false;
#else
    const char* pCommands[8];
    std::string zenityFilter;
    std::string zenityAll;
    if (folder)
    {
        pCommands[0] = "zenity";
        pCommands[1] = "--file-selection";
        pCommands[2] = "--directory";
        pCommands[3] = NULL;
    }
    else
    {
        zenityFilter = std::string("--file-filter=GTA2 files | ") + (pFilter && *pFilter ? pFilter : "*");
        zenityAll = "--file-filter=All files | *";
        pCommands[0] = "zenity";
        pCommands[1] = "--file-selection";
        pCommands[2] = zenityFilter.c_str();
        pCommands[3] = zenityAll.c_str();
        pCommands[4] = NULL;
    }
    if (char* pCaptured = SDL2_ProcessCapture(pCommands))
    {
        std::string path = pCaptured;
        free(pCaptured);
        while (!path.empty() && (path.back() == '\n' || path.back() == '\r'))
        {
            path.pop_back();
        }
        if (!path.empty())
        {
            *pOut = path;
        }
        return true;
    }
    std::string kdialogFilter;
    pCommands[0] = "kdialog";
    if (folder)
    {
        pCommands[1] = "--getexistingdirectory";
        pCommands[2] = ".";
        pCommands[3] = NULL;
    }
    else
    {
        pCommands[1] = "--getopenfilename";
        pCommands[2] = ".";
        if (pFilter && *pFilter)
        {
            kdialogFilter = std::string("*|") + pFilter;
            pCommands[3] = kdialogFilter.c_str();
        }
        else
        {
            pCommands[3] = NULL;
        }
        pCommands[4] = NULL;
    }
    if (char* pCaptured = SDL2_ProcessCapture(pCommands))
    {
        std::string path = pCaptured;
        free(pCaptured);
        while (!path.empty() && (path.back() == '\n' || path.back() == '\r'))
        {
            path.pop_back();
        }
        if (!path.empty())
        {
            *pOut = path;
        }
        return true;
    }
    return false;
#endif
}

#endif
