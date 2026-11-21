// port/net transport test (ctest): a server and a client in one process over loopback.
#include "check.hpp"
#include "../core/protocol.hpp"
#include "../transport/transport.hpp"
#include <enet/enet.h>
#include <chrono>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <thread>
#include <vector>


namespace
{
// Polls both until <want> arrives on <on> (or 3 s pass)
bool WaitFor(net::Transport* a, net::Transport* b, net::Transport* on, net::TransportEvent::Type want, net::TransportEvent* pOut)
{
    for (int i = 0; i < 3000; i++)
    {
        net::TransportEvent e;
        net::Transport* both[2] = {a, b};
        for (int k = 0; k < 2; k++)
        {
            while (both[k]->Poll(e))
            {
                if (both[k] == on && e.type == want)
                {
                    *pOut = e;
                    return true;
                }
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return false;
}

// Counts what gets through, for the conditioner's losses
class Counter : public net::Transport
{
  public:
    std::vector<int> mSeen; // the first byte of each send that got through
    bool Poll(net::TransportEvent&) override
    {
        return false;
    }
    bool Send(uint32_t, uint8_t, const void* data, size_t, bool) override
    {
        mSeen.push_back(*(const int*)data);
        return true;
    }
    void Flush() override
    {
    }
    void Disconnect(uint32_t) override
    {
    }
    uint32_t RoundTripMs(uint32_t) const override
    {
        return 0;
    }
};

// 20000 unreliable sends through a conditioner without delay: the rate lost and the mean length of a run of losses
void Losses(int lossPercent, int lossBurst, double* pRate, double* pBurst)
{
    Counter* counter = new Counter();
    net::LinkShape shape;
    shape.lossPercent = lossPercent;
    shape.lossBurst = lossBurst;
    net::Transport* t = net::CreateConditioner(counter, shape);
    const int n = 20000;
    for (int i = 0; i < n; i++)
    {
        t->Send(0, net::kChannelUnreliable, &i, sizeof(i), false);
    }
    t->Flush();
    int lost = 0, runs = 0, prev = -1;
    for (int seen : counter->mSeen)
    {
        if (seen != prev + 1)
        {
            lost += seen - prev - 1;
            runs++;
        }
        prev = seen;
    }
    lost += n - 1 - prev;
    *pRate = 100.0 * lost / n;
    *pBurst = runs ? (double)lost / runs : 0;
    delete t;
}
} // namespace

int main()
{
    std::string error;
    const uint16_t port = 27999;
    net::Transport* server = net::CreateServerTransport(port, 4, &error);
    CHECK(server);
    net::Transport* client = net::CreateClientTransport("127.0.0.1", port, &error);
    CHECK(client);
    net::TransportEvent e;
    CHECK(WaitFor(server, client, client, net::TransportEvent::kConnect, &e) && e.peer == 0);
    net::TransportEvent se;
    CHECK(WaitFor(server, client, server, net::TransportEvent::kConnect, &se));
    const uint32_t clientOnServer = se.peer;

    const char hello[] = "hello server";
    CHECK(client->Send(0, net::kChannelReliable, hello, sizeof(hello), true));
    client->Flush();
    CHECK(WaitFor(server, client, server, net::TransportEvent::kReceive, &se));
    CHECK(se.channel == net::kChannelReliable && se.data.size() == sizeof(hello) && !memcmp(se.data.data(), hello, sizeof(hello)));

    std::vector<uint8_t> snap(net::MaxPayload(), 0x5A);
    CHECK(server->Send(clientOnServer, net::kChannelUnreliable, snap.data(), snap.size(), false));
    std::vector<uint8_t> tooBig(net::MaxPayload() + 1, 1);
    CHECK(!server->Send(clientOnServer, net::kChannelUnreliable, tooBig.data(), tooBig.size(), false)); // would fragment
    server->Flush();
    CHECK(WaitFor(server, client, client, net::TransportEvent::kReceive, &e));
    CHECK(e.channel == net::kChannelUnreliable && e.data == snap);
    CHECK(server->RoundTripMs(clientOnServer) < 1000);

    // The packet cap: our hosts send nothing bigger, and a server drops what a host without the cap sends
    std::vector<uint8_t> big(net::kMaxPacket, 7), huge(net::kMaxPacket + 1, 9);
    CHECK(client->Send(0, net::kChannelReliable, big.data(), big.size(), true));
    CHECK(!client->Send(0, net::kChannelReliable, huge.data(), huge.size(), true));
    client->Flush();
    CHECK(WaitFor(server, client, server, net::TransportEvent::kReceive, &se) && se.data == big);
    {
        ENetHost* raw = enet_host_create(NULL, 1, net::kChannelCount, 0, 0);
        CHECK(raw);
        ENetAddress address;
        enet_address_set_host(&address, "127.0.0.1");
        address.port = port;
        ENetPeer* peer = enet_host_connect(raw, &address, net::kChannelCount, 0);
        bool connected = false, received = false;
        for (int i = 0; i < 3000 && !received; i++)
        {
            ENetEvent ev;
            while (enet_host_service(raw, &ev, 0) > 0)
            {
                if (ev.type == ENET_EVENT_TYPE_CONNECT)
                {
                    connected = true;
                    enet_peer_send(peer, net::kChannelReliable, enet_packet_create(huge.data(), huge.size(), ENET_PACKET_FLAG_RELIABLE));
                    enet_host_flush(raw);
                }
            }
            while (server->Poll(se))
            {
                received = received || (se.type == net::TransportEvent::kReceive && se.data.size() > net::kMaxPacket);
            }
            if (connected && i > 1500)
            {
                break; // 1.5 s after: never came
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        CHECK(connected && !received);
        enet_host_destroy(raw);
    }

    client->Disconnect(0);
    client->Flush();
    CHECK(WaitFor(server, client, server, net::TransportEvent::kDisconnect, &se) && se.peer == clientOnServer);
    delete client;
    delete server;

    // Nobody listening: the attempt ends in a disconnect
    net::Transport* lone = net::CreateClientTransport("127.0.0.1", port + 1, &error);
    CHECK(lone);
    bool failed = false;
    for (int i = 0; i < 15000 && !failed; i++)
    {
        while (lone->Poll(e))
        {
            failed |= e.type == net::TransportEvent::kDisconnect;
            CHECK(e.type != net::TransportEvent::kConnect);
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    CHECK(failed);
    delete lone;
    // The conditioner: independent losses come one at a time, bursts at the same mean rate in runs of about N
    double rate = 0, burst = 0;
    Losses(10, 0, &rate, &burst);
    CHECK(rate > 8.5 && rate < 11.5 && burst < 1.3);
    Losses(10, 5, &rate, &burst);
    printf("conditioner: 10%% in bursts of 5: %.1f%% lost, runs of %.1f\n", rate, burst);
    CHECK(rate > 8 && rate < 12 && burst > 4 && burst < 6);
    printf("net transport tests passed\n");
    return 0;
}
