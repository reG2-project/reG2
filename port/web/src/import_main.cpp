// The page's importer (import-worker.js): install.cpp's check and copy of the player's GTA2, in a worker.
// The source is mounted read-only (WORKERFS); the result is written to the working directory.
#include "../../setup/install.hpp"
#include <emscripten.h>
#include <string>

namespace
{
std::string sResult;

void Quote(const std::string& s)
{
    sResult += '"';
    for (char c : s)
    {
        if (c == '"' || c == '\\')
        {
            sResult += '\\';
            sResult += c;
        }
        else if (c == '\n')
        {
            sResult += "\\n";
        }
        else
        {
            sResult += c;
        }
    }
    sResult += '"';
}

// The result as JSON: {ok, cancelled, error, addOnly, check: {missing: [...], music: [found, total], movie,
// languages: {name: bool}}}.
const char* Report(const setup::InstallResult& r)
{
    sResult = "{\"ok\":";
    sResult += r.mOk ? "true" : "false";
    sResult += ",\"cancelled\":";
    sResult += r.mCancelled ? "true" : "false";
    sResult += ",\"error\":";
    Quote(r.mError);
    sResult += ",\"addOnly\":";
    sResult += r.mAddOnly ? "true" : "false";
    sResult += ",\"check\":{\"missing\":[";
    for (size_t i = 0; i < r.mCheck.mMissing.size(); i++)
    {
        sResult += i ? "," : "";
        Quote(r.mCheck.mMissing[i]);
    }
    sResult += "],\"music\":[" + std::to_string(r.mCheck.mMusicFound) + "," + std::to_string(r.mCheck.mMusicTotal) + "]";
    sResult += ",\"movie\":";
    sResult += r.mCheck.mMovie ? "true" : "false";
    sResult += ",\"languages\":{";
    for (size_t i = 0; i < r.mCheck.mLanguages.size(); i++)
    {
        sResult += i ? "," : "";
        Quote(r.mCheck.mLanguages[i].first);
        sResult += r.mCheck.mLanguages[i].second ? ":true" : ":false";
    }
    sResult += "}}}";
    return sResult.c_str();
}

setup::Progress sProgress;
} // namespace

extern "C" {
EMSCRIPTEN_KEEPALIVE const char* import_check(const char* source)
{
    return Report(setup::CheckSource(setup::PathFromUtf8(source), sProgress));
}

EMSCRIPTEN_KEEPALIVE const char* import_install(const char* source)
{
    sProgress.mDone = 0;
    sProgress.mTotal = 0;
    setup::InstallResult r = setup::Install(setup::PathFromUtf8(source), sProgress);
    if (r.mOk)
    {
        r.mCheck = setup::CheckGameData(".");
    }
    return Report(r);
}

// Bytes copied and to copy, for the page's progress bar.
EMSCRIPTEN_KEEPALIVE double import_done()
{
    return (double)sProgress.mDone;
}

EMSCRIPTEN_KEEPALIVE double import_total()
{
    return (double)sProgress.mTotal;
}
}

int main()
{
    return 0;
}
