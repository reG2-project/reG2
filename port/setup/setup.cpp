// The game folder, and the setup that fills it: native message boxes and file dialogs, and a small
// SDL window for the progress.
#include "setup.hpp"

#include "install.hpp"

#include <SDL3/SDL.h>
#include <math.h>
#include <algorithm>
#include <stdlib.h>

using namespace setup;

namespace
{
const char kTitle[] = "GTA2 setup";

// Shown for "I don't have the game".
const char kNoGameText[] =
    "This port does not include GTA2's game files. To play, you need your own copy of GTA2 for "
    "Windows: the original CD, a Steam copy (no longer sold since 2013), or the installer Rockstar Games "
    "gave away for free in 2004 in its \"Rockstar Classics\" series.\n\n"
    "Rockstar no longer offers it, but the Internet Archive keeps a copy of that freeware installer. "
    "Download GTA2INSTALLER.exe from there, then choose \"Install from a file\" and pick it.";
const char kNoGameUrl[] = "https://archive.org/details/gta2_rockstar-classics";

// Plain ASCII: SDL's X11 message box, used when zenity is missing, draws Latin-1 only.
const char kCheck[] = "[x]", kCross[] = "[ ]";

const SDL_DialogFileFilter kFileFilters[] = {
    {"GTA2 installer or disc image", "exe;iso;zip;7z"},
    {"All files", "*"},
};

#if !defined(_WIN32) && !defined(__APPLE__)
const unsigned char kLogoPng[] = {
#embed "../resources/reg2_256.png"
};

// The reG2 logo, 256 px.
SDL_Surface* LoadLogo()
{
    return SDL_LoadPNG_IO(SDL_IOFromConstMem(kLogoPng, sizeof(kLogoPng)), true);
}
#endif

#if !defined(_WIN32) && !defined(__APPLE__)
// Lines of at most width characters, cut at spaces.
std::string Wrap(const std::string& text, size_t width)
{
    std::string out, line;
    for (size_t pos = 0; pos <= text.size(); pos++)
    {
        const char c = pos < text.size() ? text[pos] : '\n';
        if (c != '\n')
        {
            line += c;
            if (line.size() <= width)
            {
                continue;
            }
            size_t cut = line.rfind(' ', width);
            cut = cut != std::string::npos && cut > 0 ? cut : line.find(' ', width);
            if (cut == std::string::npos)
            {
                continue;
            }
            out += line.substr(0, cut) + '\n';
            line.erase(0, cut + (line[cut] == ' '));
            continue;
        }
        out += line + (pos < text.size() ? "\n" : "");
        line.clear();
    }
    return out;
}
#endif

bool EnvSet(const char* name)
{
    const char* v = SDL_getenv(name);
    return v && *v && *v != '0';
}

// A window drawn with SDL's renderer and its 8x8 font, in units of font pixels.
class SetupWindow
{
public:
    ~SetupWindow()
    {
        SDL_DestroyRenderer(mRenderer);
        SDL_DestroyWindow(mWindow);
        if (mVideo)
        {
            SDL_QuitSubSystem(SDL_INIT_VIDEO);
        }
    }

    bool Open(int w, int h)
    {
        mVideo = SDL_InitSubSystem(SDL_INIT_VIDEO);
        const float content = mVideo ? SDL_GetDisplayContentScale(SDL_GetPrimaryDisplay()) : 1.0f;
        mScale = fmaxf(2.0f, roundf(2.0f * (content > 0.0f ? content : 1.0f)));
        if (!mVideo || !SDL_CreateWindowAndRenderer(kTitle, (int)(w * mScale), (int)(h * mScale), 0, &mWindow, &mRenderer))
        {
            SDL_Log("Setup window: %s", SDL_GetError());
            return false;
        }
        port_SetWindowIcon(mWindow);
        SDL_SetRenderScale(mRenderer, mScale, mScale);
        SDL_SetRenderVSync(mRenderer, 1);
        return true;
    }

    void Color(Uint32 rgb) { SDL_SetRenderDrawColor(mRenderer, rgb >> 16, (rgb >> 8) & 255, rgb & 255, 255); }

    void Rect(float x, float y, float w, float h, Uint32 fill, Uint32 border)
    {
        const SDL_FRect r = {x, y, w, h};
        Color(fill);
        SDL_RenderFillRect(mRenderer, &r);
        Color(border);
        SDL_RenderRect(mRenderer, &r);
    }

    void Text(float x, float y, const std::string& s, Uint32 rgb)
    {
        Color(rgb);
        SDL_RenderDebugText(mRenderer, x, y, s.c_str());
    }

    // GTA2_SETUP_CAPTURE=<folder>: frames written there as BMP, for tests without a screen.
    void Present(bool capture)
    {
        static int sCaptures;
        const char* dir = SDL_getenv("GTA2_SETUP_CAPTURE");
        SDL_Surface* s = capture && dir ? SDL_RenderReadPixels(mRenderer, NULL) : NULL;
        if (s)
        {
            SDL_SaveBMP(s, (std::string(dir) + "/setup_" + std::to_string(sCaptures++) + ".bmp").c_str());
            SDL_DestroySurface(s);
        }
        SDL_RenderPresent(mRenderer);
        Color(0xEEEEEE);
        SDL_RenderClear(mRenderer);
    }

private:
    float mScale = 2.0f;
    bool mVideo = false;
    SDL_Window* mWindow = NULL;
    SDL_Renderer* mRenderer = NULL;
};

// A native message box; buttons are numbered from 0, closing it (or no message box) gives escapeButton.
int Ask(SDL_MessageBoxFlags flags, const std::string& message, const std::vector<const char*>& buttons,
        int returnButton, int escapeButton)
{
    if (!EnvSet("GTA2_NO_MESSAGEBOX"))
    {
        std::vector<SDL_MessageBoxButtonData> data;
        for (int i = 0; i < (int)buttons.size(); i++)
        {
            data.push_back({(i == returnButton ? SDL_MESSAGEBOX_BUTTON_RETURNKEY_DEFAULT : 0u) |
                                (i == escapeButton ? SDL_MESSAGEBOX_BUTTON_ESCAPEKEY_DEFAULT : 0u),
                            i, buttons[i]});
        }
#if defined(_WIN32) || defined(__APPLE__)
        const std::string text = message;
#else
        // zenity and SDL's X11 box do not wrap lines.
        const std::string text = Wrap(message, 72);
#endif
        const SDL_MessageBoxData box = {flags | SDL_MESSAGEBOX_BUTTONS_LEFT_TO_RIGHT, NULL, kTitle, text.c_str(),
                                        (int)data.size(), data.data(), NULL};
        int id = -1;
        if (SDL_ShowMessageBox(&box, &id))
        {
            return id < 0 ? escapeButton : id;
        }
        SDL_Log("No message box (%s); GTA2_SETUP_PATH can name the folder or file to install from", SDL_GetError());
    }
    SDL_Log("%s: %s", kTitle, message.c_str());
    return escapeButton;
}

struct Pick
{
    std::atomic<int> mState{0}; // 1 done (mPath empty if cancelled), 2 no dialog
    std::string mPath, mError;
};

// A folder or a file from the system's dialog; empty if cancelled.
std::string PickPath(bool folder)
{
    Pick pick;
    const SDL_DialogFileCallback done = [](void* pUser, const char* const* pList, int) {
        Pick* p = static_cast<Pick*>(pUser);
        p->mError = pList ? "" : SDL_GetError();
        p->mPath = pList && pList[0] ? pList[0] : "";
        p->mState = pList ? 1 : 2;
    };
    if (folder)
    {
        SDL_ShowOpenFolderDialog(done, &pick, NULL, NULL, false);
    }
    else
    {
        SDL_ShowOpenFileDialog(done, &pick, NULL, kFileFilters, SDL_arraysize(kFileFilters), NULL, false);
    }
    while (pick.mState == 0)
    {
        SDL_PumpEvents();
        SDL_Delay(15);
    }
    if (pick.mState == 2)
    {
        Ask(SDL_MESSAGEBOX_ERROR,
            "No file dialog could be opened (" + pick.mError +
                ").\n\nOn Linux this needs xdg-desktop-portal or zenity. The path can also be given in the "
                "GTA2_SETUP_PATH environment variable.",
            {"OK"}, 0, 0);
    }
    return pick.mPath;
}

using JobFunction = InstallResult (*)(const fs::path&, Progress&);

struct Job
{
    JobFunction mFunction;
    fs::path mSource;
    Progress mProgress;
    InstallResult mResult;
    std::atomic<bool> mDone{false};
};

// The job runs in a thread; after a moment a window shows its progress, and closing it cancels the job.
InstallResult RunJob(JobFunction function, const fs::path& source, const char* heading)
{
    Job job;
    job.mFunction = function;
    job.mSource = source;
    SDL_Thread* thread = SDL_CreateThread(
        [](void* p) {
            Job* job = static_cast<Job*>(p);
            job->mResult = job->mFunction(job->mSource, job->mProgress);
            job->mDone = true;
            return 0;
        },
        "gta2 setup", &job);
    const int w = 300, h = 66;
    SetupWindow win;
    bool shown = false;
    for (Uint64 lastCapture = 0, start = SDL_GetTicks(); thread && !job.mDone;)
    {
        if (!lastCapture && SDL_GetTicks() - start >= 300)
        {
            shown = win.Open(w, h);
            lastCapture = 1;
        }
        SDL_Event e;
        while (SDL_PollEvent(&e))
        {
            job.mProgress.mCancel = job.mProgress.mCancel || e.type == SDL_EVENT_QUIT ||
                                    e.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED ||
                                    (e.type == SDL_EVENT_KEY_DOWN && e.key.key == SDLK_ESCAPE);
        }
        const uint64_t done = job.mProgress.mDone >> 20, total = job.mProgress.mTotal >> 20;
        const std::string amount = total ? std::to_string(done) + " / " + std::to_string(total) + " MB" : "";
        std::string status = job.mProgress.Status();
        const size_t room = (w - 16) / 8 - amount.size() - 2;
        status = status.size() > room ? "..." + status.substr(status.size() - room + 3) : status;
        if (shown)
        {
            win.Text(8, 8, job.mProgress.mCancel ? "Cancelling..." : heading, 0x202020);
            win.Rect(8, 24, w - 16, 14, 0xFFFFFF, 0x9A9A9A);
            win.Rect(9, 25, total ? (w - 18) * (float)done / total : 0, 12, 0x3584E4, 0x3584E4);
            win.Text(8, 48, status, 0x505050);
            win.Text(w - 8 - amount.size() * 8.0f, 48, amount, 0x505050);
            const bool capture = lastCapture == 1 || SDL_GetTicks() - lastCapture >= 1000;
            lastCapture = capture ? SDL_GetTicks() : lastCapture;
            win.Present(capture);
        }
        SDL_Delay(10);
    }
    if (thread)
    {
        SDL_WaitThread(thread, NULL);
    }
    else
    {
        job.mResult = function(source, job.mProgress);
    }
    return job.mResult;
}

// What a game folder or a source holds, one item a line with a check mark or a cross.
std::string Report(const DataCheck& check, bool gameFiles)
{
    const auto item = [](bool ok, const char* name) { return std::string(ok ? kCheck : kCross) + " " + name + "\n"; };
    std::string s = gameFiles ? item(check.mMissing.empty(), "Game files") : "";
    s += item(check.mMusicFound >= check.mMusicTotal, "Music") + item(check.mMovie, "Intro movie");
    for (size_t i = 0; gameFiles && i < check.mLanguages.size(); i++)
    {
        s += (i ? "  " : "Languages:\n  ") + item(check.mLanguages[i].second, check.mLanguages[i].first);
    }
    return s;
}

std::string MusicMissing(const DataCheck& check)
{
    return "The music was not found (" + std::to_string(check.mMusicFound) + " of " + std::to_string(check.mMusicTotal) +
           " tracks): the game runs without music and radio. The music is in the GTAudio folder of the original CD, "
           "or in the installer: choose one to add it.";
}

// The source's report: 0 install, 1 choose another, 2 quit.
int AskSource(const InstallResult& source, const std::string& gameDir)
{
    const DataCheck& check = source.mCheck;
    const std::string text = "This copy contains:\n\n" + Report(check, !source.mAddOnly) + "\n";
    if (!check.mMissing.empty() && !source.mAddOnly)
    {
        return 1 + Ask(SDL_MESSAGEBOX_ERROR,
                       text + "Some of GTA2's essential game files are missing: the game cannot be installed from it.",
                       {"Choose another...", "Quit"}, 0, 1);
    }
    const bool music = check.mMusicFound >= check.mMusicTotal;
    const std::string warning = !music ? MusicMissing(check) + "\n\n"
                                : !check.mMovie ? "The intro movie was not found: the game starts without it.\n\n"
                                                : "";
    return Ask(warning.empty() ? SDL_MESSAGEBOX_INFORMATION : SDL_MESSAGEBOX_WARNING,
               text + warning + (source.mAddOnly ? "Install adds it to the game in:\n" : "Install copies it to:\n") + gameDir,
               {"Install", "Choose another...", "Quit"}, 0, 2);
}

void LogDataCheck(const char* what, const DataCheck& check)
{
    std::string missing;
    for (size_t i = 0; i < check.mMissing.size() && i < 10; i++)
    {
        missing += (i ? ", " : "") + check.mMissing[i];
    }
    if (check.mMissing.size() > 10)
    {
        missing += ", ...";
    }
    const std::string state =
        check.mMissing.empty() ? "complete" : "missing " + std::to_string(check.mMissing.size()) + " files: " + missing;
    const std::string music = !check.mMusicTotal ? "" // a server's check: no music
                                                  : ", music " + std::to_string(check.mMusicFound) + " of " +
                                                        std::to_string(check.mMusicTotal) + " tracks";
    SDL_Log("%s: %s%s", what, state.c_str(), music.c_str());
}
// The questions until the game has its data: false to quit.
bool SetupLoop(const std::string& gameDir)
{
    // GTA2_SETUP_PATH: the folder or file to install from, instead of the first question and the dialog.
    const char* envPath = SDL_getenv("GTA2_SETUP_PATH");
    std::string path = envPath ? envPath : "";
    for (bool fromEnv = !path.empty();; fromEnv = false, path.clear())
    {
        const DataCheck check = CheckGameData(".");
        fs::path found;
        if (path.empty() && !check.mMissing.empty())
        {
            const int choice = Ask(SDL_MESSAGEBOX_INFORMATION,
                                   std::string(FindNoCase(".", "data", &found)
                                                   ? "Some of GTA2's essential game files are missing."
                                                   : "GTA2's game files were not found.") +
                                       "\n\nThis port does not include them. Give it your own copy: " + kSourceKinds +
                                       ". The files are copied to:\n" + gameDir,
                                   {"Use an existing install...", "Install from a file...", "I don't have the game",
                                    "Quit"},
                                   0, 3);
            if (choice < 0 || choice == 3)
            {
                return false;
            }
            if (choice == 2)
            {
                if (Ask(SDL_MESSAGEBOX_INFORMATION, kNoGameText, {"Download the GTA2 freeware from archive.org", "Back"}, 1, 1) == 0)
                {
                    SDL_OpenURL(kNoGameUrl);
                }
                continue;
            }
            path = PickPath(choice == 0);
        }
        else if (path.empty())
        {
            const bool music = check.mMusicFound >= check.mMusicTotal;
            const int choice = Ask(
                music ? SDL_MESSAGEBOX_INFORMATION : SDL_MESSAGEBOX_WARNING,
                "GTA2's game files are installed in:\n" + gameDir + "\n\n" + Report(check, true) + "\n" +
                    (music ? "Choose a folder or a file to replace them." : MusicMissing(check)),
                {"Use a folder...", "Use a file...", "Play"}, 2, 2);
            if (choice == 2)
            {
                return true;
            }
            path = PickPath(choice == 0);
        }
        if (path.empty())
        {
            continue;
        }
        const InstallResult source = RunJob(CheckSource, PathFromUtf8(path), "Reading the source...");
        int choice = 1;
        if (!source.mOk && !source.mCancelled)
        {
            Ask(SDL_MESSAGEBOX_ERROR, source.mError, {"OK"}, 0, 0);
        }
        else if (source.mOk)
        {
            LogDataCheck("Source", source.mCheck);
            // GTA2_SETUP_PATH installs without asking, when it can.
            choice = fromEnv && (source.mCheck.mMissing.empty() || source.mAddOnly) ? 0 : AskSource(source, gameDir);
        }
        if (choice == 2 || (fromEnv && !source.mOk))
        {
            return false;
        }
        if (choice != 0)
        {
            continue;
        }
        const InstallResult result = RunJob(Install, PathFromUtf8(path), "Copying GTA2's game files...");
        if (result.mOk)
        {
            return true;
        }
        if (!result.mCancelled)
        {
            Ask(SDL_MESSAGEBOX_ERROR, result.mError, {"OK"}, 0, 0);
        }
        if (fromEnv)
        {
            return false;
        }
    }
}

bool HasGameData(const fs::path& dir)
{
    fs::path found;
    return FindNoCase(dir, "data/fstyle.sty", &found);
}

// --data, else $GTA2_DIR, else the current folder or the executable's when it holds the game's data, else the
// per-user folder (empty if none). pWhy: which one, for the log.
fs::path GameFolder(const char* dataDir, bool* pPerUser, const char** pWhy)
{
    *pPerUser = false;
    fs::path found;
    if (dataDir)
    {
        *pWhy = "--data";
        const fs::path dir = PathFromUtf8(dataDir);
        // The data folder itself, or the game folder holding it
        const bool dataItself = !FindNoCase(dir, "data", &found) && FindNoCase(dir, "nyc.gci", &found);
        return dataItself ? (dir / "..").lexically_normal() : dir;
    }
    if (const char* dir = SDL_getenv("GTA2_DIR"))
    {
        *pWhy = "GTA2_DIR";
        return PathFromUtf8(dir);
    }
    std::error_code ec;
    const char* base = SDL_GetBasePath();
    if (HasGameData(fs::current_path(ec)) || !base)
    {
        *pWhy = base ? "current folder, has data" : "current folder";
        return fs::current_path(ec);
    }
    if (HasGameData(PathFromUtf8(base)))
    {
        *pWhy = "executable's folder, has data";
        return PathFromUtf8(base);
    }
    char* pref = SDL_GetPrefPath("", "reG2");
    const fs::path dir = PathFromUtf8(pref ? pref : "");
    SDL_free(pref);
    *pPerUser = true;
    *pWhy = "per-user folder";
    return dir;
}

} // namespace

void port_SetWindowIcon(SDL_Window* pWindow)
{
#if !defined(_WIN32) && !defined(__APPLE__)
    if (SDL_Surface* pLogo = LoadLogo())
    {
        SDL_SetWindowIcon(pWindow, pLogo);
        SDL_DestroySurface(pLogo);
    }
#endif
}

bool port_PrepareGameFolder(bool forceSetup, bool server, const char* dataDir)
{
    bool perUser;
    const char* why = "";
    const fs::path dir = GameFolder(dataDir, &perUser, &why);
    SDL_Log("Game folder (%s): %s", why, PathToUtf8(dir).c_str());
    std::error_code ec;
    fs::current_path(dir, ec);
    if (ec || dir.empty())
    {
        SDL_Log("Cannot enter the game folder %s: %s", PathToUtf8(dir).c_str(), ec ? ec.message().c_str() : SDL_GetError());
        return false;
    }
    // Folders the game writes to and does not create.
    fs::path found;
    for (const char* sub : {"player", "test"})
    {
        if (!FindNoCase(".", sub, &found))
        {
            fs::create_directories(sub, ec);
        }
    }
    const DataCheck check = CheckGameData(".", server);
    LogDataCheck("Game data", check);
    if (server)
    {
        // No setup: it asks questions in windows, and a server has nobody to answer them
        for (const std::string& rel : check.mMissing)
        {
            SDL_Log("Server: no %s in %s", rel.c_str(), PathToUtf8(fs::absolute(dir, ec)).c_str());
        }
        if (!check.mMissing.empty())
        {
            SDL_Log("Server: that is the %s; --data <folder> or GTA2_DIR=<folder> for another (the one holding data/)", why);
        }
        return check.mMissing.empty();
    }
    if (!perUser || (!forceSetup && check.mMissing.empty()))
    {
        SDL_Log("Setup skipped: %s", perUser ? "the game data is there" : "not the per-user folder");
        return true;
    }
    SDL_Log("Setup: %s", forceSetup ? "--setup" : "no game data");
    SDL_InitSubSystem(SDL_INIT_EVENTS);
    const bool ok = SetupLoop(PathToUtf8(dir));
    SDL_QuitSubSystem(SDL_INIT_EVENTS);
    SDL_Log("Setup %s", ok ? "done" : "left: quitting");
    if (ok)
    {
        LogDataCheck("Game data", CheckGameData("."));
    }
    return ok;
}
