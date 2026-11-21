// port/net transport (port/docs/netcode.md, "Code layout"): hosts, peers and channels, independent of the library
// underneath (enet_transport.cpp). A server host has many peers, a client host one, the server.
#pragma once
#include <stddef.h>
#include <stdint.h>
#include <string>
#include <vector>

namespace net
{
struct TransportEvent
{
    enum Type
    {
        kNone,
        kConnect,
        kDisconnect,
        kReceive,
    };
    Type type = kNone;
    uint32_t peer = 0; // stable while connected; reused after a disconnect
    uint8_t channel = 0;
    std::vector<uint8_t> data;
};

class Transport
{
  public:
    virtual ~Transport()
    {
    }
    // The next event, without waiting; false when there is none
    virtual bool Poll(TransportEvent& event) = 0;
    // Queued until Flush; false if the peer is gone or the packet is too big for an unreliable send
    virtual bool Send(uint32_t peer, uint8_t channel, const void* data, size_t size, bool reliable) = 0;
    virtual void Flush() = 0;
    virtual void Disconnect(uint32_t peer) = 0;
    virtual uint32_t RoundTripMs(uint32_t peer) const = 0;
};

// The largest packet a host sends or takes (64 KB): ENet drops a bigger one it receives, and a peer's data waiting to be
// read is capped at four of them
const size_t kMaxPacket = 64 * 1024;

// Listens on <port>; nullptr and <error> on failure
Transport* CreateServerTransport(uint16_t port, uint32_t maxPeers, std::string* error);
// A link's conditions for tests (--lag, --jitter, --loss, --lossburst, --spike): a delay with jitter, a part of the
// unreliable sends lost (in bursts of lossBurst on average: Gilbert-Elliott, at the same mean rate), and every
// spikeEveryS seconds spikeMs more delay for spikeOnS seconds (ramped in and out over a second)
struct LinkShape
{
    int lagMs = 0, jitterMs = 0, lossPercent = 0, lossBurst = 0;
    int spikeMs = 0, spikeOnS = 0, spikeEveryS = 0;
    bool Any() const
    {
        return lagMs || jitterMs || lossPercent || spikeMs;
    }
};
// A link conditioner around <inner> (taking it over), on this end's sends; use it on both ends.
Transport* CreateConditioner(Transport* inner, const LinkShape& shape);
// Starts connecting to host:port (a kConnect event follows, or kDisconnect on failure); its peer is 0
Transport* CreateClientTransport(const char* host, uint16_t port, std::string* error);
} // namespace net
