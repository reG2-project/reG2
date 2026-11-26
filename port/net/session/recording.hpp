// port/net (port/docs/netcode.md, "Recordings"): a client's session as a file, to replay without a server. A client
// does nothing but react to what arrives each frame and to its own input, so a recording of exactly that (the
// server's welcome, then per frame the packets, the round trip it saw and the input it used) replays its game
// tick for tick: as fast as the game runs, and the same every time. GTA2_NET_RECORD=<file> records, --replay <file>
// plays it back. Without GTA2_NET_RECORD, a person's session is recorded anyway (NextRecordingPath), and F12 (or the key under Esc) in play
// marks a moment to look at: the client and its replay print "net: mark N at frame F" (the key is in the input).
#pragma once
#include "../core/protocol.hpp"
#include "../transport/transport.hpp"
#include <stdint.h>
#include <stdio.h>
#include <string>
#include <vector>

namespace net
{
// recordings/<kind>-<n>.rec, n counting up over the sessions (recordings/next, or <kind>-next) and kept for the newest 10
std::string NextRecordingPath(const char* kind = "session");
// A recording ends here (an evening in a busy area is hundreds of MB, and ten are kept): its replay ends there too
const uint64_t kMaxRecordingBytes = 200ull << 20;

class ClientRecorder
{
  public:
    ~ClientRecorder();
    bool Open(const char* path, const Welcome& welcome);
    void Frame(uint32_t frame, uint32_t rttMs, uint32_t input, const std::vector<TransportEvent>& events);

  private:
    FILE* mFile = nullptr;
    uint64_t mBytes = 0; // written so far: a recording stops at kMaxRecordingBytes
};

class ClientReplay
{
  public:
    ~ClientReplay();
    bool Open(const char* path, Welcome* pWelcome, std::string* error); // false: why in <error>
    // The next frame's record; false at the end of the recording
    bool Frame(uint32_t* pFrame, uint32_t* pRttMs, uint32_t* pInput, std::vector<TransportEvent>& events);

  private:
    FILE* mFile = nullptr;
};

// A server's session: what its world is made from, so that it replays without the network. The server's world is its
// start (the command line, the GTA2_NET_* switches, the secret its session number and tokens are drawn from) and, per
// frame, the transport events it took (the clients' hellos, commands, chat and votes, joins and leaves). Each frame's
// record has a checksum of the world after it, so a replay finds the first tick where it differs.
struct ServerStart
{
    std::vector<std::string> args; // the netcode's command line
    std::vector<std::string> env; // "GTA2_NET_<name>=<value>"
    uint32_t secret[8] = {}; // 256 bits: a client sees a few draws, which must not give the others away
};

class ServerRecorder
{
  public:
    ~ServerRecorder();
    bool Open(const char* path, const ServerStart& start);
    void Frame(uint32_t tick, uint32_t check, const std::vector<TransportEvent>& events);

  private:
    FILE* mFile = nullptr;
    uint64_t mBytes = 0;
};

class ServerReplay
{
  public:
    ~ServerReplay();
    static bool IsServerRecording(const char* path);
    bool Open(const char* path, ServerStart* pStart, std::string* error);
    bool Frame(uint32_t* pTick, uint32_t* pCheck, std::vector<TransportEvent>& events); // false at the end

  private:
    FILE* mFile = nullptr;
};
// The GTA2_NET_* variables set now (the server's start), but GTA2_NET_RECORD
std::vector<std::string> NetEnvironment();
} // namespace net
