// port/net core (port/docs/netcode.md, "Wire format"): constants and the messages client and server exchange. Every
// message starts with its type (one byte); readers return false on anything malformed.
#pragma once
#include <vector>
#include "bitstream.hpp"
#include "player_limit.hpp"
#include <stdint.h>
#include <string>

namespace net
{
const uint32_t kProtocolVersion = 31;
const uint16_t kDefaultPort = 27920;
const size_t kMaxPayload = 1372; // the largest unreliable ENet packet at any link MTU (1428: ENet's mtu 1400)
// The link's MTU, IP packets (--mtu, 576 to 1428): 1280 by default, the smallest of the VPNs people play LAN games over
// (Tailscale; WireGuard 1420, Hamachi 1404). Its unreliable packets are at most MaxPayload: that less IP and UDP's 28
// bytes and ENet's 28, so neither ENet nor IP fragments them (port/docs/netcode.md, "Wire format")
void SetLinkMtu(int mtu);
int LinkMtu();
size_t MaxPayload();
const uint8_t kChannelUnreliable = 0; // snapshots, commands
const uint8_t kChannelReliable = 1; // join, events, chat
const int kChannelCount = 2;
// A command's input: the controls in the low 12 bits, a key event (its DirectInput code and the key-down bit, for the
// chat) above them (Source/input_bits.hpp)
const uint32_t kKeyEventBits = 0x3FF000;
const int kMaxCmdsPerPacket = 8; // each command packet repeats the last ones, so a lost packet loses no input

enum MsgType : uint8_t
{
    kMsgHello = 1, // client -> server, reliable
    kMsgWelcome = 2, // server -> client, reliable
    kMsgReject = 3, // server -> client, reliable, then disconnect
    kMsgCommands = 4, // client -> server, unreliable
    kMsgSnapshot = 5, // server -> client, unreliable
    kMsgSnapshotPart = 6, // server -> client, unreliable: one part of a snapshot too big for one packet
    kMsgMapChange = 7, // server -> client, reliable: the next map, after a round (engine/rounds.cpp)
    kMsgPing = 8, // client -> server, unreliable: the client's clock (ms, 32 bits) and ping (16 bits), answered at once with
    kMsgPong = 9, // server -> client, unreliable: the same clock back (the ping: the network's round trip alone)
    kMsgCommand = 10, // client -> server, reliable: a player's command ("/ready", "/yes", "/map 2"...: session/round_flow.cpp)
    kMsgNotice = 11, // server -> client, reliable: a line to show (a vote started or decided, a player ready...)
    kMsgLeave = 12, // client -> server, reliable, then disconnect: why it leaves (its copy of the map differs...)
    kMsgPings = 13, // server -> client, unreliable, once a second: every player's ping, for the scoreboard
};

// CRC-32s of a map's files (core/crc32.hpp): a client with other files would play another game
struct MapPrint
{
    uint32_t gmp = 0, sty = 0, scr = 0;
};

// The build first after the version, so that a server of any version can say what it runs and what the client does
struct Hello
{
    uint32_t version = kProtocolVersion;
    std::string build; // port_BuildId (port/src/build_id.hpp): a server takes only its own
    std::string name; // empty, too long or taken: the server gives one
    uint32_t carInfo = 0; // data/nyc.gci's CRC-32 (the cars' physics, every map's)
    uint32_t session = 0, token = 0; // a reconnect: the welcome's, so the server gives the slot back as it was (0: a join)
};

// What a client needs to start the same network game as the server
struct Welcome
{
    uint32_t version = kProtocolVersion;
    uint8_t slot = 0; // the client's player index
    uint8_t players = 0; // player slots in the game
    uint8_t gameType = 1; // 1 frags, 2 points, 3 tag (MultiplayerGameType)
    uint32_t fragLimit = 10; // frags, or a points game's money
    uint16_t timeLimitMin = 10;
    uint16_t tickMs = 33;
    uint8_t police = 0;
    uint32_t session = 0; // the server's run, drawn at its start: a client back after a drop finds another one if it restarted
    std::string name; // the player's name, as the server has it (another than the hello's if that was taken)
    std::string map; // the map's name (data/<map>.mmp)
    std::string gmp, sty, scr; // file names in data/
    MapPrint print; // the server's copy of gmp, sty and scr
    uint32_t serverTick = 0;
    std::vector<std::string> maps; // the maps a vote can choose, by number (the round's state in the world entity counts them)
    uint32_t token = 0; // this slot's: a reconnect's hello shows it to have the slot back with its score
    uint16_t game = 0; // the server's game to take snapshots of (its snapshots' world.game): the next one during a map change
};

struct Reject
{
    std::string reason;
};

struct MapChange
{
    std::string map; // the map's name (data/<map>.mmp)
    std::string gmp, sty, scr; // file names in data/
    MapPrint print; // the server's copy of gmp, sty and scr
    uint16_t game = 0; // the server's game on it (its snapshots' world.game)
    uint8_t players = 0; // that game's (the map's, at most the server's slots)
    uint8_t slot = 0; // the client's player index in it: the slots are renumbered from 0 (netcode.md, "Maps and players")
};

struct InputCmd
{
    uint32_t tick; // the server tick it is for
    uint32_t buttons; // the game's input bits (Source/input_bits.hpp), 22 bits
    uint16_t typed; // with a key press: the character it typed in the client's keyboard layout (0: none), for the chat
};
const uint32_t kKeyDownBit = 0x200000; // a key event's press (Source/input_bits.hpp)

const size_t kMaxName = 15; // the game's player names (Player::mPlayerName)
// <asked> as a player's name: printable ASCII, at most kMaxName, "PLAYER" if that leaves nothing, and with a number
// ("PLAYER 2") if a name in <taken> is the same but for case
std::string PlayerName(const std::string& asked, const std::vector<std::string>& taken);

// <s> to print to a terminal or the log: each byte outside printable ASCII a '?', so that a peer's text sends no
// escape sequences
std::string Printable(const std::string& s);

// A file name a server sends or a map's .mmp gives: only a bare name in data/, [A-Za-z0-9_-]{1,20} then <ext> (".gmp",
// any case; "" for a map's name, whose .mmp the client opens), so that nothing from the network names a path
const size_t kMaxFileName = 20;
bool BareFileName(const std::string& name, const char* ext);
// The map's name and its files' ("" if all are bare names, else the first that isn't: "map", "gmp", "sty", "scr")
std::string BadMapName(const std::string& map, const std::string& gmp, const std::string& sty, const std::string& scr);
// A welcome's: its map's, then the maps a vote chooses from ("vote map")
std::string BadMapName(const Welcome& m);

void WriteHello(BitWriter& w, const Hello& m);
bool ReadHello(BitReader& r, Hello& m);
void WriteWelcome(BitWriter& w, const Welcome& m);
bool ReadWelcome(BitReader& r, Welcome& m);
void WriteReject(BitWriter& w, const Reject& m);
bool ReadReject(BitReader& r, Reject& m);
// A line of text: a player's command (kMsgCommand) or a notice to show (kMsgNotice), at most 120 characters
void WriteText(BitWriter& w, MsgType type, const std::string& text);
bool ReadText(BitReader& r, std::string& text);
// Every slot's ping in ms (kMsgPings): the one its client measures and reports in its kMsgPing
const uint16_t kPingNone = 0xFFFF; // no player, or no ping known yet
const uint16_t kPingBot = 0xFFFE; // an arena bot: no connection
const uint16_t kPingMax = 4095;
struct Pings
{
    uint16_t ms[kMaxPlayers];
    Pings()
    {
        for (uint16_t& m : ms)
        {
            m = kPingNone;
        }
    }
};
void WritePings(BitWriter& w, const Pings& m);
bool ReadPings(BitReader& r, Pings& m);
void WriteMapChange(BitWriter& w, const MapChange& m);
bool ReadMapChange(BitReader& r, MapChange& m);

// Commands: the newest snapshot tick the client has, then up to kMaxCmdsPerPacket commands for consecutive ticks,
// oldest first
void WriteCommands(BitWriter& w, uint32_t ackTick, const InputCmd* cmds, int count);
int ReadCommands(BitReader& r, uint32_t* pAckTick, InputCmd* cmds, int max); // -1 if malformed

// Snapshot header: its tick, the tick of the baseline it is a delta to (its own tick: none, a full snapshot), the newest command tick
// the server has applied for this client
// and, per other player, the controls the server has for the ticks after it (the next one's as it will run, then the
// commands it holds): what a client predicts that player with before it repeats the last
const int kMaxForwarded = 4;
const int kMaxForwardSlots = kMaxPlayers;
struct ForwardedInputs
{
    uint8_t count[kMaxForwardSlots] = {}; // ticks known after the snapshot's, per slot
    uint16_t controls[kMaxForwardSlots][kMaxForwarded] = {}; // the low 12 bits of the input, for tick + 1 + k
};
struct SnapshotHeader
{
    uint32_t tick;
    uint32_t baseTick;
    uint32_t lastCmdTick;
    ForwardedInputs forwarded;
};
// A snapshot message bigger than one packet goes in parts (up to kMaxParts): its tick, the part's index and how many,
// then a piece of the message's bytes. A client puts them together, in order, once it has all of them
const int kMaxParts = 8;
const size_t kPartHeader = 8; // the most a part's header takes, in bytes
void WriteSnapshotParts(const std::vector<uint8_t>& message, uint32_t tick, std::vector<std::vector<uint8_t>>& parts);
bool ReadSnapshotPart(BitReader& r, uint32_t* pTick, int* pIndex, int* pCount, std::vector<uint8_t>& piece);
void WriteSnapshotHeader(BitWriter& w, const SnapshotHeader& h); // then the delta (snapshot.hpp)
bool ReadSnapshotHeader(BitReader& r, SnapshotHeader& h);
} // namespace net
