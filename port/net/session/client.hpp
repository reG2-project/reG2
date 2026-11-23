// port/net (port/docs/netcode.md, "How it works"): the client. It predicts its game ahead of the server's snapshots, sends
// its input, reconciles each snapshot (rolls back, applies it, runs the ticks since again) and shows the result.
#pragma once
#include "../core/protocol.hpp"
#include "../engine/engine.hpp"
#include "../transport/transport.hpp"
#include "../dev/client_probe.hpp"
#include "bot.hpp"
#include "recording.hpp"
#include "round_flow.hpp"
#include "session.hpp"
#include <memory>
#include <string>
#include <vector>

namespace net
{
struct ClientConfig
{
    std::string build; // port_BuildId: the server takes only its own
    std::string host = "127.0.0.1";
    uint16_t port = kDefaultPort;
    std::string name; // empty: the server gives one (PlayerName)
    std::string bot; // a bot brain plays instead of the controls ("simple"); empty: a person
    LinkShape link; // the link conditioner (tests)
};

// Corrections of what is shown, for the log
// The display error of a kind, in blocks
struct ShownError
{
    double sum = 0;
    uint32_t count = 0;
    float max = 0;
    void Add(float e)
    {
        sum += e;
        count++;
        max = e > max ? e : max;
    }
    float Mean() const
    {
        return count ? (float)(sum / count) : 0.0f;
    }
};

struct CorrectionStats
{
    uint32_t count = 0, jumps = 0;
    float sum = 0, max = 0;
    void Add(float blocks, bool jumped)
    {
        count++;
        sum += blocks;
        max = blocks > max ? blocks : max;
        jumps += jumped;
    }
    void Print(uint32_t frame, const char* what) const;
};

class Client
{
  public:
    // Connects and waits for the server's welcome (a few seconds at most): then the game to start is known
    bool Connect(const ClientConfig& config, engine::NetworkGame* pGame, std::string* error);
    // The same without waiting (the frontend's network page): PollConnect each frame, 1 welcomed, 0 not yet, -1 failed
    bool BeginConnect(const ClientConfig& config, std::string* error);
    int PollConnect(engine::NetworkGame* pGame, std::string* error);
    // No server: a recorded session (GTA2_NET_RECORD) played back frame for frame, as fast as the game runs
    bool StartReplay(const char* path, engine::NetworkGame* pGame, std::string* error);
    void Frame();
    void Leave(); // tells the server at once (a quit, a signal), not by its timeout seconds later
    const std::string& LeftWhy() const { return mLeftWhy; } // why we went back to the menu ("": the player left)
    void Idle(); // between frames: a pong timed as it arrives, the rest kept for the frame

  private:
    struct Parts
    {
        uint32_t tick = 0;
        int count = 0, have = 0;
        std::vector<std::vector<uint8_t>> pieces;
    };
    // What this client did and saw at a tick (by tick % kRing)
    struct TickRecord
    {
        uint32_t tick = 0, buttons = 0; // the input used for the tick (tick: 0 until there is one)
        uint16_t typed = 0; // what its key press typed here (InputCmd::typed)
        uint32_t shownTick = 0; // the tick <predX>, <predY> and <shown> are for (0: none)
        float predX = 0, predY = 0; // where the local player was predicted to be
        std::vector<engine::ReplicaPos> shown; // where the mapped cars and peds were shown (the display error)
        std::vector<engine::ReplicaPos> players; // and the players (id: player + 1)
    };
    // For the log, every 300 frames
    struct Stats
    {
        CorrectionStats players, cars;
        uint32_t rollbacks = 0, resimTicks = 0;
        float maxError = 0; // the prediction error
        double shownErrorSum = 0;
        uint32_t shownErrorCount = 0;
        float shownErrorMax = 0;
        ShownError others, self; // the display error of the other players, and of the local player
        double restoreUs = 0, resimUs = 0, saveUs = 0, reconcileWorstUs = 0; // the rollbacks' time
        // The same, and the cars' corrections, split: within the local player's view and a block (InView), and beyond
        double shownViewSum[2] = {};
        uint32_t shownViewCount[2] = {};
        float shownViewMax[2] = {};
        CorrectionStats carsView[2];
        uint32_t carFrames = 0, carMismatch = 0; // frames x players, and those where in-car differs from the snapshot
        uint32_t jumps = 0, fastForwards = 0, plainApplies = 0; // Resync, FastForward, a snapshot taken without its save
        uint32_t staleRestores = 0; // a snapshot's save from before the last correction (Reconcile saved its tick no more)
    };
    void Step(); // the frame's work: what arrived, the input, the snapshot, the commands
    int InView(float x, float y) const; // 0: within the local player's view and a block, 1: beyond
    void Welcomed(const Welcome& welcome, engine::NetworkGame* pGame);
    bool OnWelcome(const TransportEvent& e, engine::NetworkGame* pGame, std::string* error);
    void OnSnapshot(BitReader& r);
    void ReportBadEntities(const SnapshotHeader& h, const Snapshot& base, const Snapshot& snap, const DeltaCheck& check);
    void OnSnapshotPart(BitReader& r);
    bool TakeEvents(std::vector<TransportEvent>& events, uint32_t* pRecordedInput);
    uint32_t ChooseInput(uint32_t recordedInput);
    void StartBot();
    void CountTestStats();
    void RecordTick();
    void HandleEvents(const std::vector<TransportEvent>& events);
    void TakeSnapshot();
    bool Saved(uint32_t tick) const; // the save of that tick, to reconcile a snapshot of it
    void FastForward(int ticks); // the ticks a jump forward skips, run at once, saved
    void AnnounceRound();
    void OnMapChange(BitReader& r);
    // The server's game on <scr> (number <game>, of <players>, ours <slot>): this one ends after <pauseTicks> and that
    // one starts; false, and we left to the menu, if this machine hasn't the map's files as the server has them
    bool ChangeGame(const std::string& map, const std::string& gmp, const std::string& sty, const std::string& scr, const MapPrint& print,
                    int game, int players, int slot, int pauseTicks);
    void StartOver(); // a new game (the next map): synced again from its first snapshot
    void Resync(const Snapshot& snap);
    void Reconcile(const Snapshot& snap);
    void MeasureShownError(const Snapshot& snap);
    void TakePlayerStates(const Snapshot& snap);
    void Blend(const std::vector<engine::Pose>& shown);
    void RecordShown(); // what this tick shows, once its snapshot's correction is blended in (the display error)
    void PrintStats(const Snapshot& snap);
    int PacedTickMs() const;
    void SetNextInputs(uint32_t local);
    void SendCommands();
    uint32_t Lead() const; // ticks to run ahead of the newest snapshot
    void Reconnect(); // the connection dropped: a new one, now and then, until the server has us back
    void Rejoined(const Welcome& welcome);
    void LeaveToMenu(const char* why); // off the server, and the game back to the frontend's network page
    void SendLeave(const std::string& why); // tells the server why we go (before Leave)
    void ShowStatus(); // the ping, and an outage, over the frame
    void ActOnMenu(); // what the pause menu chose: leave, ready, a vote
    std::string NoAnswer() const; // a join's error when the server never answered
    // The round flow (session/round_flow.cpp), a client's part: its commands to the server, its keys, the panel
    void SendCommand(const std::string& text);
    void RoundKeys(uint32_t& local, uint32_t key); // F1 ready in the warmup, Y / N on an open vote
    void CompleteChat(uint32_t& local, uint32_t key); // Tab in a command line: completed (CompleteCommand)
    void WatchChat(); // a chat line typed to its end that starts with '/' goes as a command
    void KeepChatLines(); // the lines players ended, kept over the frame for a while (the game shows them while typed)
    void ShowRound(); // the warmup, countdown, results and an open vote over the frame
    // The scoreboard over the frame if <shown> (SCORES or the gamepad's Back held, the pause menu's PLAYERS <fromMenu>,
    // GTA2_NET_SCOREBOARD=1), else hidden
    void ShowScoreboard(const std::vector<engine::PlayerRow>& players, bool shown, bool fromMenu);
    std::string RoundCorner() const;
    void BotRound(); // a bot readies up and votes
    uint32_t BotMapChoice() const; // the map a bot votes for in the results (1-based)
    bool TakePong(const TransportEvent& e); // a pong (the ping's round trip measured): not for the frame
    void SendPing();

    ClientConfig mConfig;
    std::unique_ptr<Transport> mTransport;
    bool mConnected = false;
    int mSlot = 0;
    int mNextSlot = 0; // ours in the next game, during a map change's pause (the slots are renumbered); else mSlot
    int mTickMs = 33;
    int mGameType = 1; // the welcome's: 1 frags, 2 points, 3 tag
    int mFragLimit = 0; // the welcome's: frags, or a points game's money (0: none)
    uint32_t mFrame = 0;

    // Snapshots by tick % kRing: the baselines
    Snapshot mReceived[kRing];
    Parts mParts[4]; // snapshots coming in parts, by tick % 4
    uint32_t mLatestTick = 0;
    uint32_t mTakenTick = 0; // the newest snapshot this game took (our ack: the server counts us in the game by it)
    int mSlack = 0; // how many ticks early our commands reach the server (newest command - next tick)
    bool mSlackKnown = false;
    uint32_t mJumpTick = 0; // our tick at the last jump
    bool mNewSnapshot = false;

    // Prediction: this game's tick (the server tick it is predicting) and the game saved after each tick
    bool mSynced = false;
    uint32_t mTick = 0;
    std::vector<std::unique_ptr<engine::SavedWorld>> mSaves;
    std::vector<bool> mStale; // by mSaves' index: a resimulation went past its tick without saving it
    TickRecord mHist[kRing];
    std::vector<uint32_t> mRemote; // other players' last inputs, from snapshots
    std::vector<uint32_t> mRemoteSeen; // and as the newest snapshot had them
    ForwardedInputs mForwarded[kRing]; // the other players' controls after each snapshot's tick (its header's)
    ForwardedInputs mKnown; // those of the snapshot the game took last, for the ticks after <mKnownTick>
    uint32_t mKnownTick = 0;
    uint32_t mTakenAt = 0; // our tick when the game took the newest snapshot (a stall: the ticks since)
    uint32_t RemoteInput(int player, uint32_t tick) const; // what we predict another player's tick with

    std::unique_ptr<BotBrain> mBot;
    std::unique_ptr<ClientRecorder> mRecorder;
    std::unique_ptr<ClientReplay> mReplay;
    uint32_t mRtt = 0; // the round trip as the transport sees it (or the recording says)
    uint32_t mPingMs = 0; // the network's round trip: a ping to its pong, both answered between frames (a link conditioner's delay too)
    uint32_t mPingShown = 0;
    // GTA2_NET_STATS: the connection over the last second, shown with the ping
    struct LinkStats
    {
        uint32_t fromTick = 0, snapshots = 0, rollbacks = 0, resimTicks = 0;
    };
    LinkStats mLink;
    std::string mLinkShown;
    long long mPingShownMs = 0, mLastSnapshotMs = 0, mReconnectMs = 0, mPingSentMs = 0;
    std::vector<TransportEvent> mPending; // what Idle polled, for the next frame
    int mReconnects = 0; // attempts since the connection dropped
    long long mLostMs = 0; // when it dropped
    bool mHelloSent = false; // joining: connected, waiting for the welcome
    long long mJoinWaitMs = 0; // joining: since when we wait (6 s each for the connection and the welcome)
    std::string mScr; // the map's script (a rejoin must find the same)
    uint32_t mSession = 0; // the server's run (a rejoin must find the same: a restarted server has another game)
    uint32_t mToken = 0; // our slot's (the welcome's): a rejoin's hello shows it to have the slot back as it was
    std::unique_ptr<engine::NavMap> mNavMap; // built once the game has its map
    BotView mBotView;
    int mMarks = 0; // mark key presses (F12 or the key under Esc) (ChooseInput)
    engine::Round mRound; // the last one announced
    int mGeneration = 0; // engine::GameGeneration's, of the game running
    int mServerGame = -1; // the server's game we take snapshots of (the welcome's, the next after a map change)
    uint32_t mMarkFrame = 0;
    bool mLeft = false; // left the server (the pause menu's LEAVE GAME, a kick, a lost server)
    std::vector<std::string> mMaps; // what a map vote chooses from (the welcome's)
    std::vector<std::string> mMapTitles; // their names as players know them (MapTitle)
    std::vector<int> mMapPlayers; // the players each is made for (MapOwnPlayers), shown with it
    std::string mMapTitle; // the map being played's (MapTitle)
    struct ScoreRow
    {
        int slot, frags, deaths, tag, points; // tag: the seconds "it" in a tag game
    };
    std::vector<ScoreRow> mResultsRows; // the scores as the results began
    bool mWasChatting = false;
    uint32_t mChatTick = 0; // the snapshot KeepChatLines last looked at
    uint32_t mChatting = 0; // bit per player: typing a line in that snapshot
    uint32_t mRoundKeyFrame = 0, mBotSentFrame = 0;
    uint32_t mMapCursor = 0; // the results' map under the cursor (0-based): Enter, or a gamepad's A, chooses it
    std::string mLeftWhy;
    uint32_t mMenuKeyFrame = 0, mMenuKey = 0; // the pause menu's last key and its frame (held: once)
    std::vector<bool> mWasWasted; // the test log: deaths this game carried out per player
    std::vector<int> mLocalDeaths;
    Stats mStats;
    ClientProbe mProbe; // the checks and test logs (port/net/dev)
};
} // namespace net
