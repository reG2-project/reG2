// The settings menu, the same in the frontend (options_menu.cpp) and in game (pause_menu.cpp): its
// pages, navigation, settings, key and pad bindings, and its drawing with the frontend's fonts.
#pragma once

#include "prompts.hpp"
#include "saves.hpp"
#include "types.hpp"

namespace PortMenu
{
enum Input
{
    kUp,
    kDown,
    kLeft,
    kRight,
    kSelect,
    kCancel,
};

// What an input did, numbered as the frontend's sounds up to kWentBack.
enum Result
{
    kNothing,
    kMovedUp,
    kMovedDown,
    kLowered,
    kRaised,
    kEntered,
    kWentBack,
    kClosed, // Back on the main page, BACK there, or RESUME
    kQuitGame,
    kLoadSave,
    kRetryCheckpoint,
    kSaveAndQuitGame,
    kPreviousStation,
    kNextStation,
    kReadyChosen,
    kVotedYes,
    kVotedNo,
};

// The gamepad gave the last input.
bool PadMode();

// On the main page; in game it starts with RESUME and ends with QUIT GAME.
void Open();
// After Open in game: RETRY above RESUME, or RESTART while the job or frenzy runs, with the job's title and giver,
// the checkpoint's money and file time, selected or not.
void OfferRetry(PortSaves::Checkpoint kind, s32 money, long long time, const PortSaves::JobName& job, bool selected, bool restart);
// After Open in game: STATISTICS under RESUME, a page with that many rows of the original's pause screen.
void OfferStatistics(s32 rows);
// On that page: true, with the layout's top of its first row and the distance between rows.
bool OnStatistics(f32* pTop, f32* pStep);
// After Open in game: MISSION TRANSCRIPT under STATISTICS, a page with briefs of that height (0: NO MESSAGES YET).
void OfferTranscript(f32 height);
// On that page: true; either way the layout's top of the briefs and the height they may take.
bool OnTranscript(f32* pTop, f32* pRoom);
// After Open in game, again as the station changes: RADIO under RESUME with that station's name, left and right (or the
// line's arrows) giving kPreviousStation and kNextStation, Enter kNextStation; no line if NULL.
void OfferRadio(const wchar_t* pStation);
// A network game's player as PLAYERS lists it; a ping below 0 if unknown.
struct NetworkPlayer
{
    wchar_t mName[24];
    s32 mScore;
    s32 mDeaths;
    s32 mPing;
    bool mYou;
};
const s32 kMaxNetworkPlayers = 6;
// After Open in a network game, again as they change: no GAME page, PLAYERS with those players and the score's heading,
// READY (1) or NOT READY (2) giving kReadyChosen (0: no line), the open vote's title with VOTE YES and NO (NULL: none),
// LEAVE GAME with a question giving kQuitGame, and the settings a network game turns off greyed.
void OfferNetwork(const NetworkPlayer* pPlayers, s32 count, const wchar_t* pScoreHeading, s32 ready, const wchar_t* pVote);
// On LEAVE GAME's question.
bool OnLeaveQuestion();
// The menu is gone: the settings changed in it are saved now rather than at each change.
void Close();

// The frontend's LOAD GAME page: the saves under a tab per city (unreached cities dim unless saved), on the newest
// save. Left and right change the city; Back closes it; a save chosen gives kLoadSave.
void OpenSaves(const PortSaves::Entry* pEntries, s32 count, const bool* pCities);
// After Open in game, with AUTOSAVE: QUIT GAME offers SAVE AND QUIT (kSaveAndQuitGame), greyed with that reason (English)
// if not NULL, QUIT and CANCEL.
void OfferQuitChoices(const wchar_t* pBlocked);
// After Open in game: LOAD GAME under STATISTICS if there is a save, a question then that page; Back goes back.
void OfferSaves(const PortSaves::Entry* pEntries, s32 count, const bool* pCities);
bool OnSaves();
const PortSaves::Entry& SelectedSave();

Result HandleInput(Input input);

// While waiting for a key or pad input to bind: every frame, as no input goes to HandleInput.
bool IsCapturing();
Result UpdateCapture();

// The mouse's moves, clicks and wheel since the last call, with the layout placed as Draw places it.
// Needs the frontend's style.
Result HandleMouse(f32 scale, f32 left, f32 top);

const wchar_t* Title();

// The 640x480 layout at that scale and screen position, on a black panel sized to the page if asked.
// Needs the frontend's style.
void Draw(f32 scale, f32 left, f32 top, bool panel);

// The frontend's hints for a question's choices, SELECT and BACK, where its pages have theirs.
void DrawChoiceHints();
// A key's or button's picture in the frontend at a list's size, for the original pages' smaller hints, at x with its
// middle on a line of text at y; the width it takes with the space before a label.
f32 DrawHintGlyph(PortPrompts::Glyph glyph, f32 x, f32 y);
} // namespace PortMenu
