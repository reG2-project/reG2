// port/net transport over ENet 1.3.18 (port/net/enet), with the settings the probe found (port/docs/netcode.md,
// "Wire format"): the link's MTU (--mtu), two channels (unreliable and reliable never mixed), no packet throttle, a flush per
// tick, and unreliable packets capped at MaxPayload() so ENet never fragments them.
#include "transport.hpp"
#include "../core/protocol.hpp"
#include <enet/enet.h>
#include <unordered_map>

namespace
{
bool gInitialised;

bool Init(std::string* error)
{
    if (!gInitialised && enet_initialize() != 0)
    {
        *error = "enet_initialize failed";
        return false;
    }
    gInitialised = true;
    return true;
}

class EnetTransport : public net::Transport
{
  public:
    explicit EnetTransport(ENetHost* host) : mHost(host)
    {
        mHost->mtu = (enet_uint32)(net::LinkMtu() - 28); // UDP's payload: the link's IP packets less IP and UDP
    }
    ~EnetTransport() override
    {
        for (size_t i = 0; i < mPeers.size(); i++)
        {
            if (mPeers[i])
            {
                enet_peer_disconnect_now(mPeers[i], 0);
            }
        }
        enet_host_destroy(mHost);
    }

    bool Poll(net::TransportEvent& event) override
    {
        return Wait(event, 0);
    }

    bool Wait(net::TransportEvent& event, int ms) override
    {
        ENetEvent e;
        for (int wait = ms; enet_host_service(mHost, &e, (enet_uint32)wait) > 0; wait = 0)
        {
            event.data.clear();
            switch (e.type)
            {
                case ENET_EVENT_TYPE_CONNECT:
                    Configure(e.peer);
                    event.type = net::TransportEvent::kConnect;
                    event.peer = Add(e.peer);
                    return true;
                case ENET_EVENT_TYPE_DISCONNECT:
                    event.type = net::TransportEvent::kDisconnect;
                    event.peer = Remove(e.peer);
                    return true;
                case ENET_EVENT_TYPE_RECEIVE:
                    event.type = net::TransportEvent::kReceive;
                    event.peer = (uint32_t)(uintptr_t)e.peer->data - 1;
                    event.channel = e.channelID;
                    event.data.assign(e.packet->data, e.packet->data + e.packet->dataLength);
                    enet_packet_destroy(e.packet);
                    return true;
                default:
                    break;
            }
        }
        return false;
    }

    bool Send(uint32_t peer, uint8_t channel, const void* data, size_t size, bool reliable) override
    {
        if (peer >= mPeers.size() || !mPeers[peer] || (!reliable && size > net::MaxPayload()))
        {
            return false;
        }
        ENetPacket* p = enet_packet_create(data, size, reliable ? ENET_PACKET_FLAG_RELIABLE : 0);
        if (enet_peer_send(mPeers[peer], channel, p) != 0)
        {
            enet_packet_destroy(p);
            return false;
        }
        return true;
    }

    void Flush() override
    {
        enet_host_flush(mHost);
    }

    void Disconnect(uint32_t peer) override
    {
        if (peer < mPeers.size() && mPeers[peer])
        {
            enet_peer_disconnect_later(mPeers[peer], 0);
        }
    }

    uint32_t RoundTripMs(uint32_t peer) const override
    {
        return peer < mPeers.size() && mPeers[peer] ? mPeers[peer]->roundTripTime : 0;
    }

    // A client's connection attempt: its peer becomes 0 once connected
    void Connecting(ENetPeer* peer)
    {
        mPending = peer;
        enet_peer_timeout(peer, 0, 5000, 10000); // the attempt too: give up after 5-10 s, not ENet's 30 (the throttle
                                                 // setting is a message to the peer: only once connected)
    }

  private:
    void Configure(ENetPeer* peer)
    {
        enet_peer_throttle_configure(peer, 5000, ENET_PEER_PACKET_THROTTLE_SCALE, 0); // never drop unreliable sends
        enet_peer_timeout(peer, 0, 5000, 10000);
    }

    uint32_t Add(ENetPeer* peer)
    {
        uint32_t id = 0;
        while (id < mPeers.size() && mPeers[id])
        {
            id++;
        }
        if (id == mPeers.size())
        {
            mPeers.push_back(NULL);
        }
        mPeers[id] = peer;
        peer->data = (void*)(uintptr_t)(id + 1);
        return id;
    }

    uint32_t Remove(ENetPeer* peer)
    {
        const uint32_t id = (uint32_t)(uintptr_t)peer->data - 1; // data is id + 1; 0: never connected
        if (peer->data && id < mPeers.size() && mPeers[id] == peer)
        {
            mPeers[id] = NULL;
        }
        peer->data = NULL;
        return peer == mPending ? 0 : id;
    }

    ENetHost* mHost;
    ENetPeer* mPending = NULL;
    std::vector<ENetPeer*> mPeers;
};
} // namespace

namespace
{
// ENet's defaults take 32 MB packets and 32 MB waiting per peer: a message here is at most a few kB (a welcome, a
// snapshot's parts), so a peer sending more is refused before it costs memory
void Limit(ENetHost* host)
{
    host->maximumPacketSize = net::kMaxPacket;
    host->maximumWaitingData = 4 * net::kMaxPacket;
}

// Before ENet reads a packet: one address gets at most 4 of a server's peers, the loopback any number (local tests' bots)
int ENET_CALLBACK PeersPerAddress(ENetHost* host, ENetEvent*)
{
    const bool loopback = ENET_NET_TO_HOST_32(host->receivedAddress.host) >> 24 == 127;
    host->duplicatePeers = loopback ? ENET_PROTOCOL_MAXIMUM_PEER_ID : 4;
    return 0; // not intercepted: ENet goes on with the packet
}
} // namespace

net::Transport* net::CreateServerTransport(uint16_t port, uint32_t maxPeers, std::string* error)
{
    if (!Init(error))
    {
        return NULL;
    }
    ENetAddress address;
    address.host = ENET_HOST_ANY;
    address.port = port;
    ENetHost* host = enet_host_create(&address, maxPeers, kChannelCount, 0, 0);
    if (!host)
    {
        *error = "can't listen on port " + std::to_string(port);
        return NULL;
    }
    Limit(host);
    host->intercept = PeersPerAddress;
    return new EnetTransport(host);
}

net::Transport* net::CreateClientTransport(const char* hostName, uint16_t port, std::string* error)
{
    if (!Init(error))
    {
        return NULL;
    }
    ENetHost* host = enet_host_create(NULL, 1, kChannelCount, 0, 0);
    if (!host)
    {
        *error = "can't create a client host";
        return NULL;
    }
    Limit(host);
    // A name is looked up once: a lookup blocks the frame (up to the resolver's timeout while the network is down), and
    // a client reconnects every 3 s during an outage (Client::Reconnect)
    static std::unordered_map<std::string, enet_uint32> sResolved;
    ENetAddress address;
    auto known = sResolved.find(hostName);
    if (known != sResolved.end())
    {
        address.host = known->second;
    }
    else if (enet_address_set_host(&address, hostName) == 0)
    {
        sResolved[hostName] = address.host;
    }
    else
    {
        enet_host_destroy(host);
        *error = std::string("unknown host ") + hostName;
        return NULL;
    }
    address.port = port;
    EnetTransport* t = new EnetTransport(host);
    ENetPeer* peer = enet_host_connect(host, &address, kChannelCount, 0);
    if (!peer)
    {
        delete t;
        *error = "can't connect";
        return NULL;
    }
    t->Connecting(peer);
    return t;
}
