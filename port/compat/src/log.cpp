// The log: SDL's log lines kept in memory, written to reG2.log only for a crash or an error box,
// next to the executable when that folder is writable, else in the per-user folder.
// $GTA2_LOG_FILE: every line written to that file as it comes.
#include "port_platform.hpp"
#include "reg2_version.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <deque>
#include <string>
#include <vector>

#if !defined(_WIN32)
#include <fcntl.h>
#include <sys/utsname.h>
#include <unistd.h>
#endif

namespace
{
#if defined(_WIN32)
typedef HANDLE File;
File NoFile()
{
    return INVALID_HANDLE_VALUE;
}
const char kEol[] = "\r\n";
#else
typedef int File;
File NoFile()
{
    return -1;
}
const char kEol[] = "\n";
#endif

const size_t kFirstLines = 200;
const size_t kLastLines = 300;

struct State
{
    // In memory until the file is written: the first lines (the start-up) and the latest.
    std::vector<std::string> mFirst;
    std::deque<std::string> mLast;
    size_t mDropped = 0;
    File mFile = NoFile();
    std::string mPath;
};

// Built on first use: the log starts in a constructor that may run before this file's.
State& S()
{
    static State* pState = new State;
    return *pState;
}

bool sWriteTried;
SDL_LogOutputFunction sDefaultOutput;
void* sDefaultUserData;
bool sMainReturned;

#if defined(_WIN32)
std::string Utf8(const wchar_t* s)
{
    const int n = WideCharToMultiByte(CP_UTF8, 0, s, -1, NULL, 0, NULL, NULL);
    std::string out(n > 0 ? n - 1 : 0, '\0');
    if (n > 1)
    {
        WideCharToMultiByte(CP_UTF8, 0, s, -1, &out[0], n, NULL, NULL);
    }
    return out;
}
#endif

// No allocation: also used from the crash handler.
File Create(const char* path)
{
#if defined(_WIN32)
    wchar_t wide[2048];
    if (!MultiByteToWideChar(CP_UTF8, 0, path, -1, wide, 2048))
    {
        return NoFile();
    }
    return CreateFileW(wide, GENERIC_WRITE, FILE_SHARE_READ, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
#else
    return open(path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
#endif
}

void Write(const char* text, size_t size)
{
    const File file = S().mFile;
    if (file == NoFile())
    {
        return;
    }
#if defined(_WIN32)
    DWORD written;
    WriteFile(file, text, (DWORD)size, &written, NULL);
#else
    while (size > 0)
    {
        const ssize_t n = write(file, text, size);
        if (n <= 0)
        {
            return;
        }
        text += n;
        size -= (size_t)n;
    }
#endif
}

void WriteLine(const std::string& line)
{
    Write(line.data(), line.size());
    Write(kEol, sizeof(kEol) - 1);
}

void Keep(const std::string& line)
{
    State& s = S();
    if (s.mFirst.size() < kFirstLines)
    {
        s.mFirst.push_back(line);
        return;
    }
    if (s.mLast.size() == kLastLines)
    {
        s.mLast.pop_front();
        s.mDropped++;
    }
    s.mLast.push_back(line);
}

void SDLCALL LogOutput(void*, int category, SDL_LogPriority priority, const char* message)
{
    const Uint64 ms = SDL_GetTicks();
    char stamp[32];
    snprintf(stamp, sizeof(stamp), "[%4u.%03u] ", (unsigned)(ms / 1000), (unsigned)(ms % 1000));
    const char* level = priority >= SDL_LOG_PRIORITY_ERROR ? "ERROR: " : priority == SDL_LOG_PRIORITY_WARN ? "WARN: " : "";
    const std::string line = std::string(stamp) + level + message;
    if (S().mFile != NoFile())
    {
        WriteLine(line);
    }
    else
    {
        Keep(line);
    }
    if (sDefaultOutput)
    {
        sDefaultOutput(sDefaultUserData, category, priority, message);
    }
}

bool CreateLog(const std::string& path)
{
    S().mFile = Create(path.c_str());
    if (S().mFile == NoFile())
    {
        return false;
    }
    S().mPath = path;
    return true;
}

std::string OsVersion()
{
#if defined(_WIN32)
    // GetVersionEx answers 6.2 to programs whose manifest does not list Windows 10.
    char version[96] = "Windows";
    SDL_SharedObject* ntdll = reinterpret_cast<SDL_SharedObject*>(GetModuleHandleA("ntdll.dll"));
    typedef LONG(WINAPI * RtlGetVersionFn)(OSVERSIONINFOW*);
    RtlGetVersionFn getVersion = ntdll ? reinterpret_cast<RtlGetVersionFn>(SDL_LoadFunction(ntdll, "RtlGetVersion")) : NULL;
    OSVERSIONINFOW info = {};
    info.dwOSVersionInfoSize = sizeof(info);
    if (getVersion && getVersion(&info) == 0)
    {
        snprintf(version, sizeof(version), "Windows %lu.%lu.%lu", info.dwMajorVersion, info.dwMinorVersion, info.dwBuildNumber);
    }
    typedef const char*(__cdecl * WineVersionFn)();
    WineVersionFn wine = ntdll ? reinterpret_cast<WineVersionFn>(SDL_LoadFunction(ntdll, "wine_get_version")) : NULL;
    return wine ? std::string(version) + " (wine " + wine() + ")" : version;
#else
    struct utsname u;
    return uname(&u) == 0 ? std::string(u.sysname) + " " + u.release + " " + u.machine : "?";
#endif
}

const char* Arch()
{
#if defined(__x86_64__)
    return "x86_64";
#elif defined(__i386__)
    return "x86";
#elif defined(__aarch64__)
    return "arm64";
#else
    return "?";
#endif
}

void LogExitWithoutReturn()
{
    if (!sMainReturned)
    {
        SDL_Log("exit() called before the game returned");
    }
}
} // namespace

void port_StartLog(int argc, char** argv)
{
#if defined(_WIN32)
    const wchar_t* pEnvPath = _wgetenv(L"GTA2_LOG_FILE");
    if (pEnvPath)
    {
        CreateLog(Utf8(pEnvPath));
    }
#else
    if (const char* pEnvPath = getenv("GTA2_LOG_FILE"))
    {
        CreateLog(pEnvPath);
    }
#endif
    sWriteTried = S().mFile != NoFile();
    SDL_GetLogOutputFunction(&sDefaultOutput, &sDefaultUserData);
    SDL_SetLogOutputFunction(LogOutput, NULL);

#if defined(_WIN32)
    wchar_t exe[MAX_PATH] = L"";
    GetModuleFileNameW(NULL, exe, MAX_PATH);
    const std::string exePath = Utf8(exe);
    const std::string cmdLine = Utf8(GetCommandLineW());
#else
    const std::string exePath = argc > 0 ? argv[0] : "?";
    std::string cmdLine;
    for (int i = 0; i < argc; i++)
    {
        cmdLine += (i ? " " : "") + std::string(argv[i]);
    }
#endif
    SDL_Log("reG2 %s (%s), %s, exe %s, command line: %s", GTA2_VERSION, Arch(), OsVersion().c_str(), exePath.c_str(), cmdLine.c_str());
    char date[32] = "";
    const time_t now = time(NULL);
    strftime(date, sizeof(date), "%Y-%m-%d %H:%M:%S", localtime(&now));
    SDL_Log("Started %s", date);
    char klid[KL_NAMELENGTH] = "";
    GetKeyboardLayoutNameA(klid);
    std::string locales;
    if (SDL_Locale** ppLocales = SDL_GetPreferredLocales(NULL))
    {
        for (int i = 0; ppLocales[i] && i < 4; i++)
        {
            locales += (i ? ", " : "") + std::string(ppLocales[i]->language) + (ppLocales[i]->country ? std::string("-") + ppLocales[i]->country : "");
        }
        SDL_free(ppLocales);
    }
    SDL_Log("Keyboard layout %s, locales %s", klid, locales.c_str());
    atexit(LogExitWithoutReturn);
}

const char* port_WriteLog()
{
    if (!sWriteTried)
    {
        sWriteTried = true;
        bool created = false;
#if !defined(__APPLE__)
        // A macOS bundle must not change: its signature covers the folder.
        if (const char* base = SDL_GetBasePath())
        {
            created = CreateLog(std::string(base) + "reG2.log");
        }
#endif
        if (!created)
        {
            if (char* pref = SDL_GetPrefPath("", "reG2"))
            {
                created = CreateLog(std::string(pref) + "reG2.log");
                SDL_free(pref);
            }
        }
        if (created)
        {
            State& s = S();
            for (const std::string& line : s.mFirst)
            {
                WriteLine(line);
            }
            if (s.mDropped)
            {
                WriteLine("(" + std::to_string(s.mDropped) + " lines not kept)");
            }
            for (const std::string& line : s.mLast)
            {
                WriteLine(line);
            }
            s.mFirst.clear();
            s.mLast.clear();
        }
    }
    return S().mPath.c_str();
}

std::string port_WithLogPath(const char* text)
{
    const char* path = port_WriteLog();
    return *path ? std::string(text) + "\n\nThe log: " + path : text;
}

void port_LogWrite(const char* text, size_t size)
{
    Write(text, size);
}

void port_LogExit(int code)
{
    sMainReturned = true;
    SDL_Log("Exit code %d", code);
}
