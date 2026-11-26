// port/net: see recording.hpp. The file: "GTA2NETR", a version, the welcome as the protocol writes it (size first),
// then per frame: frame, round trip, input, event count, and per event its type, channel, size and bytes.
// A server's: "GTA2NETS", a version, the protocol, the secret, the command line and the switches (a count, then each as
// its size and bytes), then per frame: tick, checksum, event count, and per event its type, peer, channel, size and bytes.
// Little-endian 32-bit numbers throughout.
#include "recording.hpp"
#include <stdlib.h>
#include <string.h>
#if defined(_WIN32)
#include <direct.h>
#define environ _environ
#else
#include <sys/stat.h>
extern char** environ;
#endif

namespace
{
const char kMagic[8] = {'G', 'T', 'A', '2', 'N', 'E', 'T', 'R'};
const uint32_t kVersion = 1;
const char kServerMagic[8] = {'G', 'T', 'A', '2', 'N', 'E', 'T', 'S'};
const uint32_t kServerVersion = 1;

void Put(FILE* f, uint32_t v)
{
    const uint8_t b[4] = {(uint8_t)v, (uint8_t)(v >> 8), (uint8_t)(v >> 16), (uint8_t)(v >> 24)};
    fwrite(b, 1, 4, f);
}

bool Get(FILE* f, uint32_t* v)
{
    uint8_t b[4];
    if (fread(b, 1, 4, f) != 4)
    {
        return false;
    }
    *v = b[0] | (b[1] << 8) | (b[2] << 16) | ((uint32_t)b[3] << 24);
    return true;
}

// Each event's type, its peer (a server's), channel, size and bytes; the bytes written
uint64_t PutEvents(FILE* f, const std::vector<net::TransportEvent>& events, bool peers)
{
    uint64_t bytes = 4;
    Put(f, (uint32_t)events.size());
    for (const net::TransportEvent& e : events)
    {
        Put(f, (uint32_t)e.type);
        if (peers)
        {
            Put(f, e.peer);
        }
        Put(f, e.channel);
        Put(f, (uint32_t)e.data.size());
        fwrite(e.data.data(), 1, e.data.size(), f);
        bytes += (peers ? 16 : 12) + e.data.size();
    }
    return bytes;
}

bool GetEvents(FILE* f, std::vector<net::TransportEvent>& events, bool peers)
{
    events.clear();
    uint32_t count = 0;
    if (!Get(f, &count))
    {
        return false;
    }
    for (uint32_t i = 0; i < count; i++)
    {
        uint32_t type = 0, peer = 0, channel = 0, size = 0;
        if (!Get(f, &type) || (peers && !Get(f, &peer)) || !Get(f, &channel) || !Get(f, &size) || size > (1u << 20))
        {
            return false;
        }
        net::TransportEvent e;
        e.type = (net::TransportEvent::Type)type;
        e.peer = peer;
        e.channel = (uint8_t)channel;
        e.data.resize(size);
        if (fread(e.data.data(), 1, size, f) != size)
        {
            return false;
        }
        events.push_back(e);
    }
    return true;
}

void PutText(FILE* f, const std::string& s)
{
    Put(f, (uint32_t)s.size());
    fwrite(s.data(), 1, s.size(), f);
}

bool GetText(FILE* f, std::string* s)
{
    uint32_t size = 0;
    if (!Get(f, &size) || size > 4096)
    {
        return false;
    }
    s->resize(size);
    return fread(&(*s)[0], 1, size, f) == size;
}

void PutTexts(FILE* f, const std::vector<std::string>& list)
{
    Put(f, (uint32_t)list.size());
    for (const std::string& s : list)
    {
        PutText(f, s);
    }
}

bool GetTexts(FILE* f, std::vector<std::string>* list)
{
    uint32_t count = 0;
    if (!Get(f, &count) || count > 4096)
    {
        return false;
    }
    list->resize(count);
    for (std::string& s : *list)
    {
        if (!GetText(f, &s))
        {
            return false;
        }
    }
    return true;
}
} // namespace

std::string net::NextRecordingPath(const char* kind)
{
    const std::string counter = strcmp(kind, "session") ? std::string("recordings/") + kind + "-next" : "recordings/next";
#if defined(_WIN32)
    _mkdir("recordings");
#else
    mkdir("recordings", 0755);
#endif
    unsigned next = 0;
    if (FILE* f = fopen(counter.c_str(), "r"))
    {
        if (fscanf(f, "%u", &next) != 1)
        {
            next = 0;
        }
        fclose(f);
    }
    if (FILE* f = fopen(counter.c_str(), "w"))
    {
        fprintf(f, "%u\n", next + 1);
        fclose(f);
    }
    char path[64];
    snprintf(path, sizeof(path), "recordings/%s-%u.rec", kind, next % 10);
    return path;
}

net::ClientRecorder::~ClientRecorder()
{
    if (mFile)
    {
        fclose(mFile);
    }
}

bool net::ClientRecorder::Open(const char* path, const Welcome& welcome)
{
    mFile = fopen(path, "wb");
    if (!mFile)
    {
        return false;
    }
    fwrite(kMagic, 1, sizeof(kMagic), mFile);
    Put(mFile, kVersion);
    BitWriter w;
    WriteWelcome(w, welcome);
    Put(mFile, (uint32_t)w.ByteCount());
    fwrite(w.Data().data(), 1, w.ByteCount(), mFile);
    return true;
}

void net::ClientRecorder::Frame(uint32_t frame, uint32_t rttMs, uint32_t input, const std::vector<TransportEvent>& events)
{
    if (!mFile)
    {
        return;
    }
    if (mBytes >= kMaxRecordingBytes)
    {
        printf("net: the recording stops at frame %u (%llu MB)\n", frame, (unsigned long long)(mBytes >> 20));
        fclose(mFile);
        mFile = nullptr;
        return;
    }
    Put(mFile, frame);
    Put(mFile, rttMs);
    Put(mFile, input);
    mBytes += 12 + PutEvents(mFile, events, false);
    fflush(mFile); // a crash keeps everything up to it
}

net::ClientReplay::~ClientReplay()
{
    if (mFile)
    {
        fclose(mFile);
    }
}

bool net::ClientReplay::Open(const char* path, Welcome* pWelcome, std::string* error)
{
    *error = std::string("not a recording: ") + path;
    mFile = fopen(path, "rb");
    char magic[8];
    uint32_t version = 0, size = 0;
    if (!mFile || fread(magic, 1, 8, mFile) != 8 || memcmp(magic, kMagic, 8) || !Get(mFile, &version) || version != kVersion ||
        !Get(mFile, &size) || size > 4096)
    {
        return false;
    }
    std::vector<uint8_t> bytes(size);
    if (fread(bytes.data(), 1, size, mFile) != size)
    {
        return false;
    }
    // Its messages are as the recording build's protocol wrote them: only the same protocol reads them
    BitReader peek(bytes.data(), bytes.size());
    const uint32_t protocol = peek.Bits(8) == kMsgWelcome ? peek.VarU() : 0;
    if (!peek.Overflow() && protocol && protocol != kProtocolVersion)
    {
        *error = std::string(path) + " is a recording of protocol " + std::to_string(protocol) + ", this build plays protocol " +
                 std::to_string(kProtocolVersion) + "'s";
        return false;
    }
    BitReader r(bytes.data(), bytes.size());
    return r.Bits(8) == kMsgWelcome && ReadWelcome(r, *pWelcome);
}

bool net::ClientReplay::Frame(uint32_t* pFrame, uint32_t* pRttMs, uint32_t* pInput, std::vector<TransportEvent>& events)
{
    events.clear();
    return mFile && Get(mFile, pFrame) && Get(mFile, pRttMs) && Get(mFile, pInput) && GetEvents(mFile, events, false);
}

std::vector<std::string> net::NetEnvironment()
{
    std::vector<std::string> env;
    for (char** v = environ; v && *v; v++)
    {
        if (!strncmp(*v, "GTA2_NET_", 9) && strncmp(*v, "GTA2_NET_RECORD=", 16))
        {
            env.push_back(*v);
        }
    }
    return env;
}

net::ServerRecorder::~ServerRecorder()
{
    if (mFile)
    {
        fclose(mFile);
    }
}

bool net::ServerRecorder::Open(const char* path, const ServerStart& start)
{
    mFile = fopen(path, "wb");
    if (!mFile)
    {
        return false;
    }
    fwrite(kServerMagic, 1, sizeof(kServerMagic), mFile);
    Put(mFile, kServerVersion);
    Put(mFile, kProtocolVersion);
    for (uint32_t word : start.secret)
    {
        Put(mFile, word);
    }
    PutTexts(mFile, start.args);
    PutTexts(mFile, start.env);
    fflush(mFile);
    return true;
}

void net::ServerRecorder::Frame(uint32_t tick, uint32_t check, const std::vector<TransportEvent>& events)
{
    if (!mFile)
    {
        return;
    }
    if (mBytes >= kMaxRecordingBytes)
    {
        printf("net: the recording stops at tick %u (%llu MB)\n", tick, (unsigned long long)(mBytes >> 20));
        fclose(mFile);
        mFile = nullptr;
        return;
    }
    Put(mFile, tick);
    Put(mFile, check);
    mBytes += 8 + PutEvents(mFile, events, true);
    fflush(mFile);
}

net::ServerReplay::~ServerReplay()
{
    if (mFile)
    {
        fclose(mFile);
    }
}

bool net::ServerReplay::IsServerRecording(const char* path)
{
    char magic[8];
    FILE* f = fopen(path, "rb");
    const bool is = f && fread(magic, 1, 8, f) == 8 && !memcmp(magic, kServerMagic, 8);
    if (f)
    {
        fclose(f);
    }
    return is;
}

bool net::ServerReplay::Open(const char* path, ServerStart* pStart, std::string* error)
{
    *error = std::string("not a server recording: ") + path;
    mFile = fopen(path, "rb");
    char magic[8];
    uint32_t version = 0, protocol = 0;
    if (!mFile || fread(magic, 1, 8, mFile) != 8 || memcmp(magic, kServerMagic, 8) || !Get(mFile, &version) ||
        version != kServerVersion || !Get(mFile, &protocol))
    {
        return false;
    }
    if (protocol != kProtocolVersion)
    {
        *error = std::string(path) + " is a recording of protocol " + std::to_string(protocol) + ", this build plays protocol " +
                 std::to_string(kProtocolVersion) + "'s";
        return false;
    }
    for (uint32_t& word : pStart->secret)
    {
        if (!Get(mFile, &word))
        {
            return false;
        }
    }
    return GetTexts(mFile, &pStart->args) && GetTexts(mFile, &pStart->env);
}

bool net::ServerReplay::Frame(uint32_t* pTick, uint32_t* pCheck, std::vector<TransportEvent>& events)
{
    events.clear();
    return mFile && Get(mFile, pTick) && Get(mFile, pCheck) && GetEvents(mFile, events, true);
}
