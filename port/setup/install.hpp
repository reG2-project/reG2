// Copying the game's data into the game folder (the working directory) from an install folder,
// a disc, an archive or an installer.
#pragma once

#include <stdint.h>
#include <stdio.h>
#include <atomic>
#include <filesystem>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

namespace setup
{
namespace fs = std::filesystem;

fs::path PathFromUtf8(const std::string& s);
std::string PathToUtf8(const fs::path& p);
FILE* OpenFile(const fs::path& p, const char* mode);
// rel uses '/'; each component that does not exist as written is matched without regard to case.
bool FindNoCase(const fs::path& root, const std::string& rel, fs::path* pFound);

struct DataCheck
{
    std::vector<std::string> mMissing; // essential files
    int mMusicFound = 0;
    int mMusicTotal = 0;
    bool mMovie = false; // data/Movie/intro.bik
    std::vector<std::pair<const char*, bool>> mLanguages; // the game's five, and whether data/<code>.gxt is there
};
DataCheck CheckGameData(const fs::path& root, bool server = false); // server: a dedicated server's files only

struct Progress
{
    std::atomic<uint64_t> mDone{0}, mTotal{0}; // bytes
    std::atomic<bool> mCancel{false};
    std::mutex mMutex;
    std::string mStatus; // under mMutex

    void SetStatus(const std::string& s)
    {
        std::lock_guard<std::mutex> lock(mMutex);
        mStatus = s;
    }
    std::string Status()
    {
        std::lock_guard<std::mutex> lock(mMutex);
        return mStatus;
    }
};

struct InstallResult
{
    bool mOk = false;
    bool mCancelled = false;
    std::string mError;
    DataCheck mCheck; // CheckSource: what the source holds
    bool mAddOnly = false; // CheckSource: no game data, but music or the movie for the installed game
};

// What the setup reads, for its messages.
inline constexpr char kSourceKinds[] = "a GTA2 game folder, the game's CD (or a copy of it as a folder), its .iso, "
                                       "the 2004 Rockstar Classics freeware installer (GTA2INSTALLER.exe), "
                                       "or a .zip or .7z of an installed game";

// What source (a folder, or a file: InstallShield setup, disc image, archive) holds, from its list
// of files; of InstallShield cabinets only data1.hdr is extracted.
InstallResult CheckSource(const fs::path& source, Progress& progress);
InstallResult Install(const fs::path& source, Progress& progress);
} // namespace setup
