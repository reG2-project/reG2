// The save files (.svg): written whole or not at all, and checked before the game reads one.
#pragma once

#include "ang16.hpp"
#include "fix16.hpp"
#include "types.hpp"
#include <stddef.h>

class Player;
class SaveData;

namespace PortSaves
{
// ScriptManager::SaveGame's save, its parts in turn: written to a temporary file then moved over pFileName, the
// previous file kept if that fails. A failure does not end the game as the original's: SAVE FAILED shows instead.
struct Part
{
    const void* mData;
    u32 mSize;
};
bool Write(const char_type* pFileName, const Part* pParts, s32 count);
// The save failures so far.
s32 Failures();

// What a retry checkpoint was taken before.
enum Checkpoint
{
    kNoCheckpoint,
    kBeforeJob,
    kBeforeFrenzy,
};

// A job's title (a text id) and the gang that gave it (Gang's arrow colour: the text "ganga<colour>"), 0 if
// none, shown in the language of the moment; for a job no phone gave, the LAUNCH_MISSION command that started it.
struct JobName
{
    u16 mTitle;
    u8 mGang;
    u16 mLaunch;
};

// What the game reads first from a save: its level's files and stage.
struct Header
{
    char_type mMapName[25];
    char_type mStyleName[25];
    char_type mScriptName[25];
    u8 mMainStage;
    u8 mBonusStage; // 0 for a main stage
    s32 mMoney;
    u8 mCheckpoint; // a Checkpoint
    JobName mJob; // a checkpoint's
};

// An attract demo or a recorded input replay plays: no save, autosave, checkpoint or retry.
bool Replaying();

// The file is a whole save (sizes, header, map changes) whose saved counters are those of its
// level's script.
bool IsValid(const char_type* pFileName, Header* pHeader = NULL);

// A profile's saves in player\, per city: the church save (plyslotN.svg, the latest as the original writes it, and
// a copy) and the autosave with a backup of the one before; and one retry checkpoint for all cities.
const s32 kNumCities = 3;

// The profile has an autosave of that city, or its backup.
bool HasAutosave(u8 slot, u8 mainStage);

// SaveGame, as it fills the header: the exact position, power-ups and car of an autosave or a checkpoint,
// or none of them in a church save, as the original.
void FillSave(SaveData* pSave);
// Where the player is, and the car he drives if a save can restore it.
struct Spot
{
    Fix16 mX;
    Fix16 mY;
    Fix16 mZ;
    Ang16 mRotation;
    bool mDriving;
    u16 mCarModel;
    s16 mCarColour; // the remap + 1, 0 for none
    s16 mCarDamage;
    u32 mCarDamagedAreas;
};
void GetSpot(Spot* pSpot);

// The autosave of the current city, written now with the player where he is or at pSpot; false if it failed.
bool Autosave(const Spot* pSpot = NULL);

// The retry checkpoint, written now with the player put back at that spot, or deleted.
void CheckpointName(u8 slot, char_type* pOut, size_t size);
void WriteCheckpoint(Checkpoint kind, const Spot& spot);
void DeleteCheckpoint();
// What it was taken before, kNoCheckpoint if there is none for the current level; its money, file time and job.
Checkpoint GetCheckpoint(s32* pMoney = NULL, long long* pTime = NULL, JobName* pJob = NULL);
// The save was written before the checkpoint.
bool OlderThanCheckpoint(const char_type* pFileName);
// The job, known once its script is loaded, after the checkpoint was written.
void SetCheckpointJob(const JobName& job);
// Read again from its file, as a level starts.
void RefreshCheckpoint();
// The level was loaded from an autosave or a checkpoint: the player spawns where he was.
bool ExactSpawn();
// What the checkpoint the level was loaded from was taken before, kNoCheckpoint for another save.
Checkpoint LoadedCheckpoint();
// Its job's LAUNCH_MISSION command, 0 if a phone gave the job or for another save.
u16 LoadedLaunch();
// Player::UpdateGameFromSave restores the cars: a car of the saved one's model already on its spot is used instead.
void BeforeRestore();
// Player::UpdateGameFromSave restored the cars: the player back in the one he drove.
void AfterRestore(Player* pPlayer);

// The saves RESUME SAVED STATUS lists, newest first.
enum Kind
{
    kChurch,
    kAutosave,
    kRetry,
};
// With plyslotN.svg of a bonus stage and the checkpoint.
const s32 kMaxSaves = kNumCities * 2 + 2;

struct Entry
{
    char_type mFileName[64];
    Kind mKind;
    Checkpoint mCheckpoint;
    u8 mMainStage;
    u8 mBonusStage;
    s32 mMoney;
    s32 mJobsPassed; // -1 if unknown
    s32 mJobsTotal;
    long long mTime; // the file's, in seconds since 1970
    JobName mJob;
};

// A save the frontend can boot: its level's files are those of its stage.
typedef bool (*LevelCheck)(const Header& header);

s32 List(u8 slot, LevelCheck pCheck, Entry* pEntries);
} // namespace PortSaves
