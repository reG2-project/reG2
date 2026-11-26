// port/net (port/docs/netcode.md, "How it works"): the server. It runs the game, takes the clients' commands and sends each
// client snapshots of what is around it; it runs the round flow (round_flow.hpp).
#pragma once
#include "../core/protocol.hpp"
#include "../engine/engine.hpp"
#include "../transport/transport.hpp"
#include "../dev/arena.hpp"
#include "../dev/server_probe.hpp"
#include "recording.hpp"
#include "round_flow.hpp"
#include "session.hpp"
#include <map>
#include <memory>
#include <random>
#include <string>
#include <unordered_map>
#include <vector>

namespace net
{
struct MapFiles // a multiplayer map, from its data/<name>.mmp
{
    std::string name, gmp, sty, scr;
    int players = 0; // its own players (core/map_players.hpp): a map has the server's slots, more start at its restart zones
};

struct ServerConfig
{
    std::string build; // port_BuildId: a client of another build is turned away
    uint16_t port = kDefaultPort;
    engine::NetworkGame game; // game.players: <slots>
    int slots = 10; // the players on every map (--players, else NET_MAX_PLAYERS)
    float interestRadius = 24.0f; // at zoom 0 (ParseOptions scales it), blocks around a client's player and its view that its snapshots cover (players: always)
    int packets = 4; // a snapshot's packets at most (--packets, 1..8): a busy area's goes in parts
    LinkShape link; // the link conditioner (tests)
    std::vector<std::string> arena; // bot brains playing the first slots on the server itself (--arena)
    uint32_t endTick = 0; // the arena's result printed and the server ended at this tick (--ticks; 0: never)
    uint32_t seed = 0; // the game's random state at the start (--seed; 0: the game's own)
    std::vector<MapFiles> maps; // played in turn, a round each (--maps); fewer than two: the rounds stay on the map
    RoundRules rules; // the round flow (session/round_flow.cpp): warmup, countdown, results, votes
    std::vector<MapFiles> voteMaps; // what a vote chooses from: the rotation, else every multiplayer map there is
    int currentMap = 0; // the first map's number in voteMaps
    std::vector<std::string> args; // the command line, for the recording
    std::string replay; // a server recording played instead of the network (--replay)
};

class Server
{
  public:
    bool Start(const ServerConfig& config, std::string* error);
    void Frame();
    void Idle(); // between frames: a ping answered at once, the rest kept for the frame
    void Shutdown(); // the server ends: its clients told so (else they wait for the timeout, then try to reconnect)

  private:
    struct Slot
    {
        bool used = false;
        uint32_t peer = 0;
        std::string name;
        bool bot = false; // a bot client's (its hello): it plays but doesn't vote; kept with the name of one that left
        uint32_t token = 0; // the welcome's: a reconnect shows it to have this slot back as it was
        uint32_t joined = 0; // mJoins at its welcome
        uint32_t leftTick = 0; // the tick its connection went (a map change keeps the slot of one that may come back)
        uint32_t lastCmdTick = 0; // the newest command tick received (late ones too)
        uint32_t cmdTick[kRing] = {}; // received commands by tick % kRing
        uint32_t cmdButtons[kRing] = {}; // their controls: the key events go by <keys>
        // The key events (the chat) in the order typed, one a tick, from its own tick on: a late command's is applied
        // late, not lost. <keySeen>: the event taken for each tick, by tick % kRing (a client that steps its lead back
        // sends the same ticks again, with other key presses)
        struct KeyEvent
        {
            uint32_t tick, bits;
            uint16_t typed; // InputCmd::typed
        };
        std::vector<KeyEvent> keys;
        uint32_t keySeenTick[kRing] = {}, keySeenBits[kRing] = {};
        uint16_t typed = 0; // what the key event applied last typed
        uint32_t buttons = 0; // the input applied last (repeated while commands are late)
        uint32_t next = 0; // what the next tick runs with, as the game has it (the other clients get it forwarded)
        uint32_t late = 0; // ticks without their command (counted for the log)
        uint32_t silent = 0; // ticks in a row without their command
        uint16_t ping = 0; // the ping its client reports in its kMsgPing (0: none yet)
        uint32_t ackTick = 0; // the newest snapshot the client has
        Snapshot sent[kRing]; // what was sent, by tick % kRing: the baselines
        std::unordered_map<uint32_t, uint32_t> lastFresh; // entity -> the tick its new state was last sent (PriceArea)
    };
    // For the log, every 300 ticks
    struct Stats
    {
        uint32_t bytes = 0, parts = 0, snapshots = 0, maxBytes = 0; // what went to the clients (arena bots get none)
        uint32_t inArea = 0, fresh = 0, cut = 0; // entities in the areas, with new state, snapshots without it for all
        uint32_t unsent = 0; // snapshots that fit no budget
    };
    // A nearby entity, for filling a snapshot by priority
    struct Item
    {
        const EntityState* now;
        const EntityState* had; // in the client's baseline
        EntityState aged; // what it has, with its age
        size_t freshBits, agedBits;
        float priority, distance;
        int pick; // 0: not sent (new, no room), 1: aged, 2: this tick's
        bool hold; // far off screen and fresh lately: aged this tick even with room (PriceArea)
    };
    void OnReceive(const TransportEvent& e);
    void AnswerPing(const TransportEvent& e); // the pong at once, and the ping it reports kept for the others
    void SendPings();
    void OnHello(uint32_t peer, BitReader& r);
    std::string Refusal(const Hello& hello, bool read) const; // why a hello's client can't play here ("": it can)
    bool KeptForReturn(const Slot& s) const;
    int FreeSlot(const std::string& name, bool bot, bool kept) const; // the slot a joining <name> gets (mSlots.size(): none)
    int BotSlotForPerson() const;
    void MakeRoom(int slot);
    void RecallArenaBots();
    int ReturningSlot(const Hello& hello) const; // the slot a reconnect's token is for, in use or not (-1: none)
    void OnCommands(int slot, BitReader& r);
    void StartGameOnce();
    void PollTransport();
    bool Deserted() const; // nobody to play for: no client connected or connecting, no arena
    void OnEvent(const TransportEvent& e);
    void SetNextInputs(const Snapshot& world);
    void RunRounds(const Snapshot& world); // the round flow's tick and what it asks for
    void OnCommand(int slot, BitReader& r);
    void ReportSent(int slot, const std::string& text);
    void Carry(const RoundActions& actions);
    void ChangeToMap(int map); // voteMaps' <map>, for everyone
    void Kick(int slot);
    void Notice(int slot, const std::string& text, int except = -1); // to one client (slot) or all (-1) but <except>
    std::vector<RoundPlayer> RoundPlayers(const Snapshot& world) const;
    void StartOver(); // a new game (the next map): its start, and every client's baseline, again
    void PrintStats(const Snapshot& world);
    void SendSnapshot(int slot, Slot& s, const Snapshot& world);
    std::vector<Item> PriceArea(const Slot& s, const std::vector<const EntityState*>& area, const Snapshot* base, const engine::Foci& foci) const;
    ForwardedInputs Forwarded(int slot) const; // the other players' controls after this tick, for <slot>'s snapshot
    bool Fill(Slot& s, const Snapshot& fixed, std::vector<Item>& items, const Snapshot* base, BitWriter& w);
    void Transmit(const Slot& s, const BitWriter& w);
    int SlotOfPeer(uint32_t peer) const;

    ServerConfig mConfig;
    std::unique_ptr<Transport> mTransport;
    std::vector<Slot> mSlots;
    std::vector<TransportEvent> mPending; // what Idle polled, for the next frame
    std::unordered_map<uint32_t, long long> mHelloWait; // peers connected that haven't said hello: since when (ms)
    uint32_t mTick = 0;
    uint32_t mJoins = 0; // welcomes so far
    bool mStarted = false;
    uint32_t mGameStartTick = 0; // the tick the running game started at: its snapshots' first
    Stats mStats;
    ServerProbe mProbe; // the test hooks and logs (port/net/dev)
    Arena mArena;
    RoundFlow mRounds;
    std::vector<RoundPlayer> mRoundPlayers; // as of the last frame, for commands between frames
    std::map<std::string, uint32_t> mBanned; // kicked names, until this tick
    size_t mMap = 0; // in mConfig.voteMaps
    uint32_t mSession = 0; // this run's (the welcome's)
    uint32_t mCarInfo = 0; // data/nyc.gci's CRC-32
    MapPrint mPrint; // the map being played's files
    int mGeneration = 0; // engine::GameGeneration's, of the game running
    std::mt19937 mRandom; // the session number and the tokens, from the recording's secret
    std::unique_ptr<ServerRecorder> mRecorder;
    std::unique_ptr<ServerReplay> mReplay;
    std::vector<TransportEvent> mFrameEvents; // what this frame took, for the recording
    uint32_t mReplayCheck = 0; // the recording's checksum of this tick
    uint32_t mFirstDiff = 0; // the first tick a replay's world differs from the recording's
    void CheckWorld(const Snapshot& world);
    void EndReplay(uint32_t ticks); // the replay's verdict, and the end
};
} // namespace net
