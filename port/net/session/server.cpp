// port/net (port/docs/netcode.md, "The server"): the authoritative game. Every frame, after the tick: read the clients, set
// every player's input for the next tick, and send each client a snapshot of its area as a delta to the newest one
// it has acknowledged.
#include "server.hpp"
#include "env.hpp"
#include "../maps.hpp"
#include <algorithm>
#include <chrono>
#include <math.h>
#include <random>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <thread>

namespace
{
const uint32_t kArenaPeer = ~0u; // an arena bot's slot (no transport peer has it)

float Distance(const net::EntityState& e, float x, float y)
{
    // Every schema type with a place starts with x and y
    const float dx = net::engine::Blocks(e.f[0]) - x, dy = net::engine::Blocks(e.f[1]) - y;
    return fabsf(dx) > fabsf(dy) ? fabsf(dx) : fabsf(dy);
}

// Its state in the client's baseline (NULL: new to it)
const net::EntityState* InBase(const net::Snapshot* base, const net::EntityState& e)
{
    if (!base)
    {
        return NULL;
    }
    auto it = std::lower_bound(base->entities.begin(), base->entities.end(), e.id, [](const net::EntityState& b, uint32_t id) { return b.id < id; });
    return it != base->entities.end() && it->id == e.id && it->type == e.type ? &*it : NULL;
}

// A client's Escape (the key's code in the input: Source/input_bits.hpp) is its own (Client::ChooseInput: its leave
// prompt): here it would open the server's quit prompt for that player, whose yes ended the server's game
uint32_t WithoutEscape(uint32_t buttons)
{
    const uint32_t kKeyDown = 0x200000, kKeyBits = 0x1FF000, kEscape = 0x01; // DIK_ESCAPE
    return ((buttons & kKeyBits) >> 12) == kEscape ? buttons & ~(kKeyDown | kKeyBits) : buttons;
}

} // namespace

std::string net::Scoreboard(const Snapshot& snap)
{
    std::string out;
    for (const EntityState& e : snap.entities)
    {
        int p = 0;
        uint32_t b = 0;
        if (engine::PlayerOf(e, &p, &b))
        {
            char one[64];
            snprintf(one, sizeof(one), " %d:%d/%d(%d)@%.0f,%.0f", p, engine::PlayerFrags(e), engine::PlayerDeaths(e), engine::PlayerHealth(e),
                     engine::Blocks(e.f[0]), engine::Blocks(e.f[1]));
            out += one;
        }
    }
    return out;
}

bool net::Server::Start(const ServerConfig& config, std::string* error)
{
    mConfig = config;
    mConfig.packets = config.packets < 1 ? 1 : config.packets > kMaxParts ? kMaxParts : config.packets;
    mSlots.assign(config.game.players, Slot());
    mTransport.reset(CreateServerTransport(config.port, (uint32_t)config.game.players + 4, error));
    if (!mTransport)
    {
        return false;
    }
    if (config.link.Any())
    {
        mTransport.reset(CreateConditioner(mTransport.release(), config.link));
    }
    if (config.arena.size() > mSlots.size())
    {
        *error = "more arena bots than player slots";
        return false;
    }
    if (!mArena.Start(config.arena, error))
    {
        return false;
    }
    for (int i = 0; i < mArena.Count(); i++)
    {
        mSlots[i].used = true; // no peer: the arena's
        mSlots[i].peer = kArenaPeer;
        mSlots[i].name = "bot" + std::to_string(i);
    }
    std::vector<std::string> mapNames, titles;
    for (const MapFiles& m : mConfig.voteMaps)
    {
        mapNames.push_back(m.name);
        titles.push_back(MapTitle(m.name));
    }
    mMap = (size_t)(mConfig.currentMap >= 0 ? mConfig.currentMap : 0);
    mRounds.Start(mConfig.rules, mapNames, mConfig.currentMap);
    mRounds.SetMapTitles(titles);
    engine::CurrentRound() = mRounds.State();
    mProbe.Start();
    mSession = std::random_device{}() | 1; // never 0: a welcome's session is always set
    mCarInfo = engine::DataFileCrc("nyc.gci");
    mPrint.gmp = engine::DataFileCrc(mConfig.game.gmp);
    mPrint.sty = engine::DataFileCrc(mConfig.game.sty);
    mPrint.scr = engine::DataFileCrc(mConfig.game.scr);
    printf("net: server on port %u, %d player slots, %s, reG2 %s\n", config.port, config.game.players, config.game.scr.c_str(), config.build.c_str());
    return true;
}

int net::Server::SlotOfPeer(uint32_t peer) const
{
    for (size_t i = 0; i < mSlots.size(); i++)
    {
        if (mSlots[i].used && mSlots[i].peer == peer)
        {
            return (int)i;
        }
    }
    return -1;
}

// Another build plays another game (its code and its protocol); other car physics too
std::string net::Server::Refusal(const Hello& hello, bool read) const
{
    if (!read || hello.version != kProtocolVersion || hello.build != mConfig.build)
    {
        return "the server runs reG2 " + mConfig.build + ", you run " + (!read || hello.build.empty() ? "another version" : hello.build);
    }
    if (hello.carInfo != mCarInfo)
    {
        return "your data/nyc.gci differs from the server's";
    }
    return "";
}

// The slot this name had (a client back after its connection dropped: the same player), else the first free one
// that nobody left, else the first free one
int net::Server::FreeSlot(const std::string& name) const
{
    int any = (int)mSlots.size(), unclaimed = any;
    for (int i = (int)mSlots.size() - 1; i >= 0; i--)
    {
        if (mSlots[i].used)
        {
            continue;
        }
        if (mSlots[i].name == name)
        {
            return i;
        }
        any = i;
        unclaimed = mSlots[i].name.empty() ? i : unclaimed;
    }
    return unclaimed < (int)mSlots.size() ? unclaimed : any;
}

// A client back after its connection dropped, whose old connection may not have timed out here yet
int net::Server::ReturningSlot(const Hello& hello) const
{
    for (size_t i = 0; hello.token && hello.session == mSession && i < mSlots.size(); i++)
    {
        if (mSlots[i].token == hello.token && mSlots[i].peer != kArenaPeer)
        {
            return (int)i;
        }
    }
    return -1;
}

void net::Server::OnHello(uint32_t peer, BitReader& r)
{
    Hello hello;
    BitWriter w;
    const bool read = ReadHello(r, hello);
    const std::string refusal = Refusal(hello, read);
    if (!refusal.empty())
    {
        Reject reject;
        reject.reason = refusal;
        WriteReject(w, reject);
        printf("net: turned away a client: %s\n", refusal.c_str());
    }
    else if (SlotOfPeer(peer) < 0)
    {
        std::vector<std::string> taken;
        for (const Slot& s : mSlots)
        {
            if (s.used)
            {
                taken.push_back(s.name);
            }
        }
        const int back = ReturningSlot(hello);
        std::string name = back >= 0 ? mSlots[back].name : PlayerName(hello.name, std::vector<std::string>());
        int slot = back >= 0 ? back : FreeSlot(name);
        if (back < 0 && slot < (int)mSlots.size() && mSlots[slot].name != name)
        {
            name = PlayerName(name, taken); // a new player: a name nobody in the game has
        }
        auto ban = mBanned.find(name);
        if (ban != mBanned.end() && mTick < ban->second)
        {
            Reject reject;
            reject.reason = "kicked by vote: try again later";
            WriteReject(w, reject);
        }
        else if (slot == (int)mSlots.size())
        {
            Reject reject;
            reject.reason = "the server is full";
            WriteReject(w, reject);
        }
        else
        {
            Slot& s = mSlots[slot];
            const bool stillIn = back >= 0 && s.used; // its old connection hasn't timed out: dropped, the player stays
            if (stillIn)
            {
                mTransport->Disconnect(s.peer);
            }
            const uint32_t token = back >= 0 ? s.token : std::random_device{}() | 1;
            s = Slot();
            s.used = true;
            s.peer = peer;
            s.name = name;
            s.token = token;
            Welcome welcome;
            welcome.slot = (uint8_t)slot;
            welcome.players = (uint8_t)mSlots.size();
            welcome.gameType = (uint8_t)mConfig.game.gameType;
            welcome.fragLimit = (uint32_t)mConfig.game.fragLimit;
            welcome.timeLimitMin = (uint16_t)mConfig.game.timeLimitMin;
            welcome.tickMs = (uint16_t)mConfig.game.tickMs;
            welcome.police = (uint8_t)mConfig.game.police;
            welcome.session = mSession;
            welcome.name = name;
            welcome.map = mMap < mConfig.voteMaps.size() ? mConfig.voteMaps[mMap].name : "";
            welcome.gmp = mConfig.game.gmp;
            welcome.sty = mConfig.game.sty;
            welcome.scr = mConfig.game.scr;
            welcome.print = mPrint;
            welcome.serverTick = mTick;
            welcome.maps = mRounds.Maps();
            welcome.token = token;
            welcome.game = (uint16_t)(engine::MapChangePending() ? engine::GameGeneration() + 1 : engine::GameGeneration());
            WriteWelcome(w, welcome);
            if (!stillIn)
            {
                engine::PutInGame(slot, back < 0); // at the next restart zone; a new player with an empty score
            }
            engine::SetPlayerName(slot, name.c_str());
            printf("net: %s %s player %d%s\n", name.c_str(), back >= 0 ? "is back as" : "joined as", slot,
                   name != hello.name ? (" (asked for \"" + hello.name + "\")").c_str() : "");
            Notice(-1, name + (back >= 0 ? " is back" : " joined"), slot);
        }
    }
    if (w.ByteCount()) // a second hello from a client already in: nothing to say
    {
        mTransport->Send(peer, kChannelReliable, w.Data().data(), w.ByteCount(), true);
    }
}

void net::Server::OnCommands(int slot, BitReader& r)
{
    Slot& s = mSlots[slot];
    uint32_t ack = 0;
    InputCmd cmds[kMaxCmdsPerPacket];
    const int n = ReadCommands(r, &ack, cmds, kMaxCmdsPerPacket);
    if (n < 0)
    {
        return;
    }
    if (ack > s.ackTick && ack <= mTick)
    {
        s.ackTick = ack;
    }
    for (int i = 0; i < n; i++)
    {
        // The newest tick a command arrived for, late or not: the client steers its lead by it
        if (cmds[i].tick <= mTick + kRing)
        {
            s.lastCmdTick = cmds[i].tick > s.lastCmdTick ? cmds[i].tick : s.lastCmdTick;
        }
        const uint32_t buttons = WithoutEscape(cmds[i].buttons);
        // Commands for ticks still to come (and not too far ahead) go into the buffer
        if (cmds[i].tick > mTick && cmds[i].tick <= mTick + kRing)
        {
            s.cmdTick[cmds[i].tick % kRing] = cmds[i].tick;
            s.cmdButtons[cmds[i].tick % kRing] = buttons & ~kKeyEventBits;
        }
        // A key event, late or not, once (the packets repeat the commands)
        const uint32_t key = buttons & kKeyEventBits, at = cmds[i].tick % kRing;
        const bool seen = s.keySeenTick[at] == cmds[i].tick && s.keySeenBits[at] == key;
        if (key && !seen && cmds[i].tick + kRing > mTick && cmds[i].tick <= mTick + kRing && s.keys.size() < (size_t)kRing)
        {
            s.keys.push_back({cmds[i].tick, key, cmds[i].typed});
            s.keySeenTick[at] = cmds[i].tick;
            s.keySeenBits[at] = key;
        }
    }
}

void net::Server::OnReceive(const TransportEvent& e)
{
    BitReader r(e.data.data(), e.data.size());
    const uint32_t type = r.Bits(8);
    if (type == kMsgHello)
    {
        OnHello(e.peer, r);
        return;
    }
    if (type == kMsgPing) // polled in a frame, not between them (Idle): late by that much
    {
        std::vector<uint8_t> pong = e.data;
        pong[0] = kMsgPong;
        mTransport->Send(e.peer, kChannelUnreliable, pong.data(), pong.size(), false);
        return;
    }
    const int slot = SlotOfPeer(e.peer);
    std::string why;
    if (slot >= 0 && type == kMsgCommands) // else: not joined, or nothing a client sends
    {
        OnCommands(slot, r);
    }
    else if (slot >= 0 && type == kMsgLeave && ReadText(r, why))
    {
        printf("net: player %d (%s) leaves: %s\n", slot, mSlots[slot].name.c_str(), why.c_str());
    }
    else if (slot >= 0 && type == kMsgCommand)
    {
        OnCommand(slot, r);
    }
}

namespace
{
const float kFullRateRadius = 12.0f; // blocks: beyond, an entity the client has gets new state every kFarAgeTicks
const int kFarAgeTicks = 3;
} // namespace

// Each nearby entity with what it costs new and aged, and its priority: the age plus one, over one plus a sixth of the
// distance (an entity new to the client counts as 8 ticks old: one near comes soon)
std::vector<net::Server::Item> net::Server::PriceArea(const Slot& s, const std::vector<const EntityState*>& area, const Snapshot* base, float x, float y) const
{
    const Schema& schema = engine::NetSchema();
    std::vector<Item> items(area.size());
    for (size_t i = 0; i < area.size(); i++)
    {
        Item& it = items[i];
        it.now = area[i];
        it.had = InBase(base, *area[i]);
        it.freshBits = EntryBits(schema, it.had, *it.now);
        it.pick = 0;
        it.agedBits = 0;
        it.hold = false;
        if (it.had)
        {
            it.aged = *it.had;
            const int f = engine::AgeField(it.had->type);
            const int age = (int)(mTick - base->tick) + engine::EntityAge(*it.had);
            if (f >= 0)
            {
                it.aged.f[f] = age < 1 ? 1 : age > 63 ? 63 : age;
            }
            it.agedBits = EntryBits(schema, it.had, it.aged);
            it.priority = (float)(age + 1) / (1.0f + Distance(*it.now, x, y) / 6.0f);
            // Off screen with a margin (a view is 8 to 11 blocks centre to corner), new state every third tick when it
            // costs more than its age: the client's own simulation keeps it within a pixel meanwhile (netcode.md, "Drift")
            const auto last = s.lastFresh.find(it.now->id);
            it.hold = last != s.lastFresh.end() && mTick - last->second < (uint32_t)kFarAgeTicks &&
                      Distance(*it.now, x, y) > kFullRateRadius && it.freshBits > it.agedBits;
        }
        else
        {
            it.priority = 9.0f / (1.0f + Distance(*it.now, x, y) / 6.0f);
        }
    }
    return items;
}

// The next tick's controls of every other player in the game, then the commands we hold for the ticks after it, as far as
// they follow on
net::ForwardedInputs net::Server::Forwarded(int slot) const
{
    ForwardedInputs f;
    const bool held = InputsHeld(engine::CurrentRound().phase);
    for (int i = 0; i < (int)mSlots.size() && i < kMaxForwardSlots; i++)
    {
        const Slot& o = mSlots[i];
        if (i == slot || !o.used || !engine::SlotInGame(i))
        {
            continue;
        }
        f.controls[i][0] = (uint16_t)(o.next & 0xFFF);
        int n = 1;
        for (uint32_t t = mTick + 2; n < kMaxForwarded && o.peer != kArenaPeer && o.cmdTick[t % kRing] == t; t++)
        {
            f.controls[i][n++] = (uint16_t)(held ? 0 : o.cmdButtons[t % kRing] & 0xFFF);
        }
        f.count[i] = (uint8_t)n;
    }
    return f;
}

// Fit the snapshot into its packets by priority, as Quake 3's successors do. The entities the client has stay in every
// snapshot: one left out would be removed there (and cost a whole new one later). Each gets this tick's state, or, when
// there is no room, the state the client already has with its age (the ticks since it was new) - a few bits; the
// client applies only new state and predicts the rest on. Who gets new state: the highest priority. New entities come
// after, as room allows. False: it fits no budget (nothing to send)
bool net::Server::Fill(Slot& s, const Snapshot& fixed, std::vector<Item>& items, const Snapshot* base, BitWriter& w)
{
    static const Snapshot kEmpty;
    const Schema& schema = engine::NetSchema();
    size_t fixedBits = 0;
    for (const EntityState& e : fixed.entities)
    {
        fixedBits += EntryBits(schema, InBase(base, e), e);
    }
    std::vector<size_t> order(items.size());
    for (size_t i = 0; i < items.size(); i++)
    {
        order[i] = i;
    }
    std::sort(order.begin(), order.end(), [&](size_t a, size_t b) { return items[a].priority > items[b].priority; });
    const size_t room = mConfig.packets == 1 ? MaxPayload() : (size_t)mConfig.packets * (MaxPayload() - kPartHeader); // bytes
    const ForwardedInputs forwarded = Forwarded((int)(&s - &mSlots[0]));
    size_t forwardedBits = 4;
    for (int i = 0; i < kMaxForwardSlots; i++)
    {
        forwardedBits += forwarded.count[i] ? 5 + 13 * forwarded.count[i] : 0;
    }
    size_t budget = room * 8 - 96 - forwardedBits; // the header, the entry count, a margin, the forwarded controls
    for (;;)
    {
        size_t bits = fixedBits;
        for (Item& it : items) // what the client has stays, aged
        {
            it.pick = it.had ? 1 : 0;
            bits += it.had ? it.agedBits : 0;
        }
        for (size_t k = 0; k < order.size(); k++) // then new state, by priority, as room allows
        {
            Item& it = items[order[k]];
            const size_t was = it.had ? it.agedBits : 0;
            if (!it.hold && bits - was + it.freshBits <= budget)
            {
                bits += it.freshBits - was;
                it.pick = 2;
            }
        }
        Snapshot candidate = fixed;
        size_t fresh = 0;
        for (const Item& it : items)
        {
            if (it.pick == 2)
            {
                candidate.entities.push_back(*it.now);
                fresh++;
            }
            else if (it.pick == 1)
            {
                candidate.entities.push_back(it.aged);
            }
        }
        engine::SetSnapshotReach(candidate, mConfig.interestRadius); // nothing it has goes missing
        SortById(candidate);
        w = BitWriter();
        SnapshotHeader h = {mTick, base ? base->tick : mTick, s.lastCmdTick, forwarded};
        WriteSnapshotHeader(w, h);
        WriteDelta(w, schema, base ? *base : kEmpty, candidate, Env().snapCheck);
        if (w.ByteCount() <= room || budget < 2048)
        {
            mStats.inArea += (uint32_t)items.size();
            mStats.fresh += (uint32_t)fresh;
            mStats.cut += fresh < items.size();
            s.sent[mTick % kRing] = candidate;
            for (const Item& it : items)
            {
                if (it.pick == 2)
                {
                    s.lastFresh[it.now->id] = mTick;
                }
            }
            return w.ByteCount() <= room;
        }
        budget -= budget / 8; // the estimate was off (ids' gaps): a little less
    }
}

// One packet, or parts of at most MaxPayload() each: the client puts them together before reading it
void net::Server::Transmit(const Slot& s, const BitWriter& w)
{
    if (w.ByteCount() > MaxPayload())
    {
        std::vector<uint8_t> message(w.Data().begin(), w.Data().begin() + w.ByteCount());
        std::vector<std::vector<uint8_t>> parts;
        WriteSnapshotParts(message, mTick, parts);
        for (size_t k = 0; k < parts.size(); k++)
        {
            mTransport->Send(s.peer, kChannelUnreliable, parts[k].data(), parts[k].size(), false);
        }
        mStats.parts += (uint32_t)parts.size();
    }
    else
    {
        mTransport->Send(s.peer, kChannelUnreliable, w.Data().data(), w.ByteCount(), false);
        mStats.parts++;
    }
    mStats.bytes += (uint32_t)w.ByteCount();
    mStats.snapshots++;
    mStats.maxBytes = std::max(mStats.maxBytes, (uint32_t)w.ByteCount());
}

// The client's area: every player and the cars they sit in (wherever they are: a player far off isn't a ped without its
// car), every train (a few, and a client's own moves on wherever it is), and the cars, peds and pickups near its own,
// nearest first
void net::Server::SendSnapshot(int slot, Slot& s, const Snapshot& world)
{
    float x = 0, y = 0;
    const bool located = engine::PlayerPosition(slot, &x, &y);
    std::vector<uint32_t> playersCars;
    for (const EntityState& e : world.entities)
    {
        if (engine::PlayerCarId(e))
        {
            playersCars.push_back(engine::PlayerCarId(e));
        }
    }
    Snapshot fixed;
    fixed.tick = mTick;
    std::vector<const EntityState*> area;
    for (const EntityState& e : world.entities)
    {
        const bool playersCar = std::find(playersCars.begin(), playersCars.end(), e.id) != playersCars.end();
        if (e.type == engine::kEntityPlayer || e.type == engine::kEntityWorld || e.type == engine::kEntityTrain || playersCar)
        {
            fixed.entities.push_back(e);
        }
        else if (located && Distance(e, x, y) <= mConfig.interestRadius)
        {
            area.push_back(&e);
        }
    }
    std::sort(area.begin(), area.end(), [&](const EntityState* a, const EntityState* b) { return Distance(*a, x, y) < Distance(*b, x, y); });
    const Snapshot& acked = s.sent[s.ackTick % kRing];
    const Snapshot* base = s.ackTick && acked.tick == s.ackTick && mTick - s.ackTick < (uint32_t)kRing ? &acked : NULL;
    std::vector<Item> items = PriceArea(s, area, base, x, y);
    BitWriter w;
    if (Fill(s, fixed, items, base, w))
    {
        Transmit(s, w);
    }
    else
    {
        mStats.unsent++;
    }
}

void net::Server::StartGameOnce()
{
    if (mStarted || !engine::GameRunning())
    {
        return;
    }
    // The map script made a ped for every slot: those without a client are out, ped and all, until one joins
    for (size_t i = 0; i < mSlots.size(); i++)
    {
        // Every slot starts a game with a fresh score and HUD, a map change's new game too: one away comes back to
        // this game's, not the last one's
        engine::FreshScores((int)i);
        if (!mSlots[i].used)
        {
            engine::NeverInGame((int)i); // and its ped
        }
        else
        {
            if (mSlots[i].peer == kArenaPeer)
            {
                engine::PutInGame((int)i, true);
            }
            engine::SetPlayerName((int)i, mSlots[i].name.c_str());
        }
    }
    if (mConfig.seed)
    {
        engine::SeedRandom(mConfig.seed);
    }
    mGameStartTick = mTick;
    if (mGeneration > 0)
    {
        Carry(mRounds.NewGame(mTick, (int)mMap, RoundPlayers(Snapshot())));
    }
    mStarted = true;
}

void net::Server::PollTransport()
{
    for (const TransportEvent& e : mPending)
    {
        OnEvent(e);
    }
    mPending.clear();
    TransportEvent e;
    while (mTransport->Poll(e))
    {
        OnEvent(e);
    }
}

// Between frames: a client's ping goes back at once (its round trip is then the network's alone, not our tick's
// wait); anything else waits for the frame, as a join or a leave changes the game
void net::Server::Idle()
{
    TransportEvent e;
    while (mTransport && mTransport->Poll(e))
    {
        if (e.type == TransportEvent::kReceive && e.channel == kChannelUnreliable && !e.data.empty() && e.data[0] == kMsgPing)
        {
            e.data[0] = kMsgPong;
            mTransport->Send(e.peer, kChannelUnreliable, e.data.data(), e.data.size(), false);
            mTransport->Flush();
        }
        else
        {
            mPending.push_back(e);
        }
    }
}

void net::Server::OnEvent(const TransportEvent& e)
{
    if (e.type == TransportEvent::kReceive)
    {
        OnReceive(e);
    }
    else if (e.type == TransportEvent::kDisconnect)
    {
        const int slot = SlotOfPeer(e.peer);
        if (slot >= 0)
        {
            printf("net: player %d (%s) left\n", slot, mSlots[slot].name.c_str());
            const std::string name = mSlots[slot].name;
            const uint32_t token = mSlots[slot].token;
            mSlots[slot] = Slot();
            mSlots[slot].name = name; // kept for its return
            mSlots[slot].token = token;
            engine::TakeOutOfGame(slot);
            mRounds.Left(slot);
            Notice(-1, name + " left");
        }
    }
}

const uint32_t kHeldTicks = 6; // a late client's input is repeated this long at most

// Inputs for the next tick: its command, or the last one again if it is late, or the arena bot's; nobody's slot
// stands still
void net::Server::SetNextInputs(const Snapshot& world)
{
    const uint32_t next = mTick + 1;
    if (mArena.Count() && mStarted)
    {
        mArena.Think(world, mTick);
    }
    for (size_t i = 0; i < mSlots.size(); i++)
    {
        Slot& s = mSlots[i];
        if (s.peer == kArenaPeer)
        {
            s.buttons = mStarted ? mArena.Buttons((int)i) : 0;
        }
        else if (s.used)
        {
            const bool came = s.cmdTick[next % kRing] == next;
            if (came)
            {
                s.buttons = s.cmdButtons[next % kRing];
                if (s.silent > kHeldTicks)
                {
                    float x = 0, y = 0;
                    engine::PlayerPosition((int)i, &x, &y);
                    printf("net: tick %u, player %zu back after %u ticks without commands, at (%.2f, %.2f)\n", mTick, i, s.silent, x, y);
                }
                s.silent = 0;
            }
            else
            {
                s.late++;
                // The last controls again (a key event, the chat, comes from the key queue below); a client silent
                // longer than a late command's few ticks plays nothing: its player stands, rather than running on blind
                s.buttons = ++s.silent > kHeldTicks ? 0 : s.buttons & ~kKeyEventBits;
                if (s.silent == kHeldTicks + 1)
                {
                    float x = 0, y = 0;
                    engine::PlayerPosition((int)i, &x, &y);
                    printf("net: tick %u, player %zu without commands for %u ticks: its input let go, at (%.2f, %.2f)\n", mTick, i, kHeldTicks, x, y);
                }
            }
            s.typed = 0;
            if (!s.keys.empty() && s.keys.front().tick <= next)
            {
                s.buttons |= s.keys.front().bits;
                s.typed = s.keys.front().typed;
                s.keys.erase(s.keys.begin());
            }
        }
        // The results and the countdown: a player's key events (the chat) are all it has (round_state.hpp)
        const uint32_t buttons = InputsHeld(engine::CurrentRound().phase) ? s.buttons & kKeyEventBits : s.buttons;
        s.next = s.used ? buttons : 0;
        engine::SetPlayerInput((int)i, s.next);
        engine::SetPlayerTyped((int)i, s.next, s.typed);
    }
}

// GTA2_NET_JERKLOG: the same measure as the clients', on the server's own state
// The round flow's view of the slots: who is in, a person or an arena bot, and the score that counts (frags, points
// in a points game, the seconds "it" in a tag game)
std::vector<net::RoundPlayer> net::Server::RoundPlayers(const Snapshot& world) const
{
    std::vector<RoundPlayer> players(mSlots.size());
    for (size_t i = 0; i < mSlots.size(); i++)
    {
        players[i].in = mSlots[i].used && engine::SlotInGame((int)i);
        players[i].voter = mSlots[i].used && mSlots[i].peer != kArenaPeer;
        players[i].alwaysReady = mSlots[i].peer == kArenaPeer;
        players[i].name = mSlots[i].name;
        // A client has the game once it acknowledged one of its snapshots (it was loading the map till then)
        players[i].loaded = mSlots[i].peer == kArenaPeer || (mSlots[i].ackTick && mSlots[i].ackTick >= mGameStartTick);
    }
    for (const EntityState& e : world.entities)
    {
        int p = 0;
        uint32_t b = 0;
        if (engine::PlayerOf(e, &p, &b) && p < (int)players.size())
        {
            // As the game's results: a tag game's winner was "it" the longest (Frontend::ChangeMenuPage)
            const int type = mConfig.game.gameType;
            players[p].score = type == 2 ? engine::PlayerPoints(e) : type == 3 ? engine::TagSeconds(p) : engine::PlayerFrags(e);
        }
    }
    return players;
}

// The round flow's tick (session/round_flow.cpp), its state for the snapshots, and what it asks for
void net::Server::RunRounds(const Snapshot& world)
{
    mRoundPlayers = RoundPlayers(world);
    if (!mStarted || engine::MapChangePending())
    {
        engine::CurrentRound() = mRounds.State();
        return;
    }
    const int before = mRounds.State().number;
    const RoundActions actions = mRounds.Tick(mTick, mRoundPlayers);
    if (mRounds.State().number != before)
    {
        printf("net: round %d's scores:%s\n", before, Scoreboard(world).c_str());
    }
    Carry(actions);
    engine::CurrentRound() = mRounds.State();
}

void net::Server::Carry(const RoundActions& actions)
{
    for (const auto& n : actions.notices)
    {
        Notice(n.first, n.second);
    }
    if (actions.newRound)
    {
        engine::NewRound();
    }
    if (actions.kick >= 0)
    {
        Kick(actions.kick);
    }
    if (actions.changeMap >= 0)
    {
        ChangeToMap(actions.changeMap);
    }
}

void net::Server::OnCommand(int slot, BitReader& r)
{
    std::string text;
    if (!ReadText(r, text) || !mStarted)
    {
        return;
    }
    if (Env().snapCheck && text.compare(0, 9, "/snapbad ") == 0)
    {
        ReportSent(slot, text);
        return;
    }
    printf("net: player %d (%s): %s\n", slot, mSlots[slot].name.c_str(), text.c_str());
    RoundActions actions;
    mRounds.Command(mTick, slot, text, mRoundPlayers, actions);
    Carry(actions);
    engine::CurrentRound() = mRounds.State();
}

// GTA2_NET_SNAPCHECK: a client read an entity wrong ("/snapbad <tick> <baseline> <id>"): what we sent it then, and
// the baseline we sent it against, from the snapshots we keep
void net::Server::ReportSent(int slot, const std::string& text)
{
    unsigned tick = 0, baseTick = 0, id = 0;
    if (sscanf(text.c_str(), "/snapbad %u %u %u", &tick, &baseTick, &id) != 3)
    {
        return;
    }
    const Slot& s = mSlots[slot];
    const Snapshot& sent = s.sent[tick % kRing];
    const Snapshot& base = s.sent[baseTick % kRing];
    const EntityState* e = NULL;
    const EntityState* was = NULL;
    for (const EntityState& x : sent.entities)
    {
        e = x.id == id ? &x : e;
    }
    for (const EntityState& x : base.entities)
    {
        was = x.id == id ? &x : was;
    }
    if (sent.tick != tick || !e)
    {
        printf("net: CHECK player %d read entity %u of snapshot %u wrong: no longer kept\n", slot, id, tick);
        return;
    }
    printf("net: CHECK player %d read entity %u of snapshot %u wrong; we sent (baseline %u%s):%s\n", slot, id, tick, baseTick,
           baseTick == tick ? ", none" : base.tick != baseTick ? ", no longer kept" : was ? "" : ", not in it",
           DescribeFields(engine::NetSchema(), *e, base.tick == baseTick ? was : NULL).c_str());
}

void net::Server::Notice(int slot, const std::string& text, int except)
{
    BitWriter w;
    WriteText(w, kMsgNotice, text);
    for (size_t i = 0; i < mSlots.size(); i++)
    {
        if (mSlots[i].used && mSlots[i].peer != kArenaPeer && (slot < 0 || slot == (int)i) && (int)i != except)
        {
            mTransport->Send(mSlots[i].peer, kChannelReliable, w.Data().data(), w.ByteCount(), true);
        }
    }
    printf("net: notice%s: %s\n", slot < 0 ? "" : (" to player " + std::to_string(slot)).c_str(), text.c_str());
}

// Out by vote: told why, disconnected, and its name turned away for 10 minutes
void net::Server::Kick(int slot)
{
    Slot& s = mSlots[slot];
    if (!s.used || s.peer == kArenaPeer)
    {
        return;
    }
    const uint32_t tps = 1000 / (mConfig.game.tickMs > 0 ? mConfig.game.tickMs : 33);
    mBanned[s.name] = mTick + 600 * tps;
    BitWriter w;
    Reject reject;
    reject.reason = "kicked by vote";
    WriteReject(w, reject);
    mTransport->Send(s.peer, kChannelReliable, w.Data().data(), w.ByteCount(), true);
    mTransport->Flush();
    mTransport->Disconnect(s.peer);
    printf("net: player %d (%s) kicked\n", slot, s.name.c_str());
    mRounds.Left(slot);
    const std::string name = s.name;
    s = Slot(); // the name isn't kept for a return
    engine::TakeOutOfGame(slot);
    Notice(-1, name + " was kicked");
}

// A map for everyone: after a pause (the round's winner on the HUD), the game ends here and on the clients and the
// next starts at once on it
void net::Server::ChangeToMap(int map)
{
    mMap = (size_t)map;
    const MapFiles& m = mConfig.voteMaps[mMap];
    mConfig.game.gmp = m.gmp; // for the welcome of those joining
    mConfig.game.sty = m.sty;
    mConfig.game.scr = m.scr;
    mPrint.gmp = engine::DataFileCrc(m.gmp);
    mPrint.sty = engine::DataFileCrc(m.sty);
    mPrint.scr = engine::DataFileCrc(m.scr);
    printf("net: the next map: %s\n", m.name.c_str());
    MapChange change;
    change.map = m.name;
    change.print = mPrint;
    change.gmp = m.gmp;
    change.sty = m.sty;
    change.scr = m.scr;
    change.game = (uint16_t)(engine::GameGeneration() + 1);
    BitWriter w;
    WriteMapChange(w, change);
    for (const Slot& s : mSlots)
    {
        if (s.used && s.peer != kArenaPeer)
        {
            mTransport->Send(s.peer, kChannelReliable, w.Data().data(), w.ByteCount(), true);
        }
    }
    // The round flow showed the results already: half a second for the clients to hear of it. Without it (an arena's
    // rotation) the pause shows the winner
    engine::ChangeMap(m.gmp, m.sty, m.scr, mConfig.rules.enabled ? 15 : 90);
}

void net::Server::Shutdown()
{
    if (!mTransport)
    {
        return;
    }
    BitWriter w;
    Reject reject;
    reject.reason = "the server closed";
    WriteReject(w, reject);
    int left = 0;
    for (const Slot& s : mSlots)
    {
        if (s.used && s.peer != kArenaPeer)
        {
            mTransport->Send(s.peer, kChannelReliable, w.Data().data(), w.ByteCount(), true);
            mTransport->Disconnect(s.peer); // once the reject is through: an abrupt one drops what came with it
            left++;
        }
    }
    mTransport->Flush();
    TransportEvent e;
    for (const auto until = std::chrono::steady_clock::now() + std::chrono::milliseconds(500); left > 0 && std::chrono::steady_clock::now() < until;)
    {
        while (left > 0 && mTransport->Poll(e))
        {
            left -= e.type == TransportEvent::kDisconnect;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    mTransport.reset();
    printf("net: server closed\n");
}

void net::Server::StartOver()
{
    mGeneration = engine::GameGeneration();
    mStarted = false;
    for (Slot& s : mSlots)
    {
        s.ackTick = 0; // the old game's snapshots are no baseline: full ones again
        s.lastFresh.clear();
        for (Snapshot& sent : s.sent)
        {
            sent = Snapshot();
        }
    }
    mArena.NewMap();
}

void net::Server::PrintStats(const Snapshot& world)
{
    int used = 0;
    uint32_t late = 0;
    for (Slot& s : mSlots)
    {
        used += s.used;
        late += s.late;
        s.late = 0;
    }
    int sent = 0, driven = 0, same = 0;
    engine::DriverStats(world, &sent, &driven, &same);
    const double snapshots = mStats.snapshots ? mStats.snapshots : 1;
    printf("net: tick %u, %d players, %zu entities (%d drivers), %.0f bytes per snapshot (up to %u), %u late commands; frags/deaths(health)%s\n",
           mTick, used, world.entities.size(), sent, mStats.bytes / snapshots, mStats.maxBytes, late, Scoreboard(world).c_str());
    printf("net: tick %u, %.2f packets per snapshot; %s\n", mTick, mStats.parts / snapshots, engine::SpawnStats().c_str());
    for (size_t i = 0; i < mSlots.size(); i++)
    {
        if (mSlots[i].used)
        {
            printf("net: tick %u, player %zu sees %.1f blocks (centre to corner), spawned for %d ticks ahead\n", mTick, i, engine::PlayerViewRadius((int)i),
                   engine::SpawnLead((int)i));
        }
    }
    printf("net: tick %u, snapshots had new state for %.0f%% of the entities in the clients' areas (%u of %u not for all)%s\n", mTick,
           mStats.inArea ? 100.0 * mStats.fresh / mStats.inArea : 100.0, mStats.cut, used * 300, mStats.unsent ? "; some didn't fit at all" : "");
    mStats = Stats();
}

void net::Server::Frame()
{
    mTick++; // the tick that just ran
    engine::KeepMapChange();
    if (engine::GameGeneration() != mGeneration)
    {
        StartOver();
    }
    engine::GuardPeds();
    StartGameOnce();
    PollTransport();
    mProbe.Tests(mTick, (int)mSlots.size());
    Snapshot world;
    engine::Capture(world);
    world.tick = mTick;
    mProbe.Captured(mTick, world);
    SetNextInputs(world);
    RunRounds(world);
    if (mConfig.endTick && mTick >= mConfig.endTick)
    {
        mArena.Finish(world);
        fflush(stdout);
        _Exit(0); // No game shutdown; the trainer only reads the result line
    }
    int players = 0;
    for (size_t i = 0; i < mSlots.size(); i++)
    {
        const Slot& s = mSlots[i];
        players += s.used;
        // A client shows its player ahead of here by how early its commands come and the way back of the snapshots
        // (half the round trip, from what it acknowledged); the next tick spawns for there
        const bool remote = s.used && s.peer != kArenaPeer && s.ackTick && s.lastCmdTick;
        engine::SetSpawnLead((int)i, remote ? (int)(s.lastCmdTick - mTick) + (int)(mTick - s.ackTick) / 2 : 0);
    }
    engine::SetSpawnPlayers(players);
    for (size_t i = 0; i < mSlots.size(); i++)
    {
        if (mSlots[i].used && mSlots[i].peer != kArenaPeer)
        {
            SendSnapshot((int)i, mSlots[i], world);
        }
    }
    mTransport->Flush();
    if (mTick % 300 == 0)
    {
        PrintStats(world);
    }
}
