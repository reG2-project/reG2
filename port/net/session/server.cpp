// port/net (port/docs/netcode.md, "The server"): the authoritative game. Every frame, after the tick: read the clients, set
// every player's input for the next tick, and send each client a snapshot of its area as a delta to the newest one
// it has acknowledged.
#include "server.hpp"
#include "env.hpp"
#include "../core/crc32.hpp"
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
const int kHelloWaitMs = 10000;
const int kDesertedWaitMs = 100;
const int kComeBackSec = 40; // a client tries to reconnect for 30 s (client.cpp, kGiveUpMs) after its own timeout

long long NowMs()
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

float Distance(const net::EntityState& e, float x, float y)
{
    // Every schema type with a place starts with x and y
    const float dx = net::engine::Blocks(e.f[0]) - x, dy = net::engine::Blocks(e.f[1]) - y;
    return fabsf(dx) > fabsf(dy) ? fabsf(dx) : fabsf(dy);
}

float Distance(const net::EntityState& e, const net::engine::Foci& f) // to the nearer
{
    float d = Distance(e, f.x[0], f.y[0]);
    for (int k = 1; k < f.count; k++)
    {
        d = std::min(d, Distance(e, f.x[k], f.y[k]));
    }
    return d;
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

// A replay's: the recording has what came in, and nothing goes out
class NoTransport : public net::Transport
{
  public:
    bool Poll(net::TransportEvent&) override
    {
        return false;
    }
    bool Send(uint32_t, uint8_t, const void*, size_t, bool) override
    {
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

// The world's checksum: every entity's id, type and fields
uint32_t WorldCheck(const net::Snapshot& world)
{
    const net::Schema& schema = net::engine::NetSchema();
    uint32_t crc = 0;
    for (const net::EntityState& e : world.entities)
    {
        const uint32_t head[2] = {e.id, e.type};
        crc = net::Crc32(head, sizeof(head), crc);
        const size_t fields = e.type < schema.types.size() ? schema.types[e.type].fields.size() : 0;
        crc = net::Crc32(e.f, fields * sizeof(e.f[0]), crc);
    }
    return crc;
}

// GTA2_NET_DUMPWORLD=<tick>[,<tick>...]: every entity of the server's world at those ticks, by the snapshots' ids
void DumpWorld(const net::Snapshot& world)
{
    const char* ticks = net::Env().dumpWorld;
    const std::string at = std::to_string(world.tick);
    bool listed = false;
    for (const char* p = ticks; p && *p && !listed; p = strchr(p, ',') ? strchr(p, ',') + 1 : "")
    {
        listed = !strncmp(p, at.c_str(), at.size()) && (p[at.size()] == ',' || !p[at.size()]);
    }
    if (!listed)
    {
        return;
    }
    const net::Schema& schema = net::engine::NetSchema();
    printf("net: world at tick %u, %zu entities\n", world.tick, world.entities.size());
    for (const net::EntityState& e : world.entities)
    {
        if (e.type >= schema.types.size())
        {
            continue;
        }
        const net::EntityType& type = schema.types[e.type];
        std::string line = "net: " + std::to_string(world.tick) + " " + type.name + " " + std::to_string(e.id);
        for (size_t i = 0; i < type.fields.size(); i++)
        {
            line += std::string(" ") + type.fields[i].name + "=" + std::to_string(e.f[i]);
        }
        puts(line.c_str());
    }
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
    ServerStart start;
    start.args = config.args;
    start.env = NetEnvironment();
    std::random_device device;
    for (uint32_t& word : start.secret)
    {
        word = device();
    }
    if (!config.replay.empty())
    {
        mReplay.reset(new ServerReplay());
        if (!mReplay->Open(config.replay.c_str(), &start, error))
        {
            return false;
        }
        mTransport.reset(new NoTransport());
    }
    else
    {
        mTransport.reset(CreateServerTransport(config.port, (uint32_t)config.slots + 4, error));
    }
    if (!mTransport)
    {
        return false;
    }
    std::seed_seq seed(start.secret, start.secret + 8);
    mRandom.seed(seed);
    // GTA2_NET_RECORD=<file>, or off; recorded anyway unless the server only plays its own bots (tests, training)
    const char* record = Env().record;
    const std::string path = mReplay ? "" : record ? (strcmp(record, "off") ? record : "") : config.arena.empty() ? NextRecordingPath("server") : "";
    if (!path.empty())
    {
        mRecorder.reset(new ServerRecorder());
        if (!mRecorder->Open(path.c_str(), start))
        {
            printf("net: can't record to %s\n", path.c_str());
            mRecorder.reset();
        }
        else
        {
            printf("net: recording to %s\n", path.c_str());
        }
    }
    if (config.link.Any() && !mReplay)
    {
        mTransport.reset(CreateConditioner(mTransport.release(), config.link));
    }
    if (config.arena.size() > mSlots.size())
    {
        *error = "more arena bots than player slots (" + std::to_string(mSlots.size()) + ")";
        return false;
    }
    if (!mArena.Start(config.arena, start.secret[1], error))
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
    mRounds.Seed(start.secret[0]);
    mRounds.SetMapTitles(titles);
    engine::CurrentRound() = mRounds.State();
    mProbe.Start();
    mSession = mRandom() | 1; // never 0: a welcome's session is always set
    mCarInfo = engine::DataFileCrc("nyc.gci");
    mPrint.gmp = engine::DataFileCrc(mConfig.game.gmp);
    mPrint.sty = engine::DataFileCrc(mConfig.game.sty);
    mPrint.scr = engine::DataFileCrc(mConfig.game.scr);
    printf("net: server on port %u, %d player slots, %s, reG2 %s\n", config.port, config.slots, config.game.scr.c_str(), config.build.c_str());
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

// A slot whose person dropped lately: kept for a reconnect
bool net::Server::KeptForReturn(const Slot& s) const
{
    const uint32_t tps = 1000 / (mConfig.game.tickMs > 0 ? mConfig.game.tickMs : 33);
    return !s.used && !s.bot && !s.name.empty() && mTick - s.leftTick < kComeBackSec * tps;
}

// The slot this name had (a client back after its connection dropped: the same player), else the first free one
// that nobody left, else the first free one. A slot kept for a person's reconnect only with <kept>, never for a bot
int net::Server::FreeSlot(const std::string& name, bool bot, bool kept) const
{
    const int none = (int)mSlots.size();
    int any = none, unclaimed = none, held = none;
    for (int i = (int)mSlots.size() - 1; i >= 0; i--)
    {
        const bool forReturn = KeptForReturn(mSlots[i]);
        if (mSlots[i].used || (bot && forReturn))
        {
            continue;
        }
        if (mSlots[i].name == name)
        {
            return i;
        }
        held = forReturn ? i : held;
        any = forReturn ? any : i;
        unclaimed = mSlots[i].name.empty() ? i : unclaimed;
    }
    return unclaimed < none ? unclaimed : any < none ? any : kept ? held : none;
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
        printf("net: turned away a client: %s\n", Printable(refusal).c_str());
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
        int slot = back >= 0 ? back : FreeSlot(name, hello.bot, true);
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
            printf("net: turned away a client: %s\n", reject.reason.c_str());
        }
        else
        {
            Slot& s = mSlots[slot];
            const bool stillIn = back >= 0 && s.used; // its old connection hasn't timed out: dropped, the player stays
            if (stillIn)
            {
                mTransport->Disconnect(s.peer);
            }
            const uint32_t token = back >= 0 ? s.token : mRandom() | 1;
            s = Slot();
            s.used = true;
            s.peer = peer;
            s.name = name;
            s.token = token;
            s.bot = hello.bot;
            Welcome welcome;
            welcome.slot = (uint8_t)slot;
            welcome.players = (uint8_t)mSlots.size();
            welcome.gameType = (uint8_t)mConfig.game.gameType;
            welcome.fragLimit = (uint32_t)mConfig.game.fragLimit;
            welcome.timeLimitMin = (uint16_t)mConfig.game.timeLimitMin;
            welcome.tickMs = (uint16_t)mConfig.game.tickMs;
            welcome.police = (uint8_t)mConfig.game.police;
            welcome.kit = (uint8_t)mConfig.game.kit;
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
            // During a map change's pause the slots are the next game's already (ChangeToMap): its start sets them up
            if (!stillIn && !engine::MapChangePending())
            {
                engine::PutInGame(slot, back < 0); // at the next restart zone; a new player with an empty score
            }
            if (!engine::MapChangePending())
            {
                engine::SetPlayerName(slot, name.c_str());
            }
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

void net::Server::AnswerPing(const TransportEvent& e)
{
    std::vector<uint8_t> pong = e.data;
    pong[0] = kMsgPong;
    mTransport->Send(e.peer, kChannelUnreliable, pong.data(), pong.size(), false);
    BitReader r(e.data.data(), e.data.size());
    r.Bits(8 + 32);
    const uint32_t ping = r.Bits(16);
    const int slot = SlotOfPeer(e.peer);
    if (slot >= 0 && !r.Overflow())
    {
        mSlots[slot].ping = (uint16_t)ping;
    }
}

// Once a second, to every client: each player's ping as its own client measures it (ENet's until it says)
void net::Server::SendPings()
{
    Pings pings;
    for (size_t i = 0; i < (size_t)kMaxPlayers; i++)
    {
        const Slot* s = i < mSlots.size() && mSlots[i].used ? &mSlots[i] : NULL;
        if (s && s->peer == kArenaPeer)
        {
            pings.ms[i] = kPingBot;
        }
        else if (s)
        {
            pings.ms[i] = s->ping ? s->ping : (uint16_t)std::min<uint32_t>(mTransport->RoundTripMs(s->peer), kPingMax);
        }
    }
    BitWriter w;
    WritePings(w, pings);
    for (const Slot& s : mSlots)
    {
        if (s.used && s.peer != kArenaPeer)
        {
            mTransport->Send(s.peer, kChannelUnreliable, w.Data().data(), w.ByteCount(), false);
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
        AnswerPing(e);
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
        printf("net: player %d (%s) leaves: %s\n", slot, mSlots[slot].name.c_str(), Printable(why).c_str());
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
std::vector<net::Server::Item> net::Server::PriceArea(const Slot& s, const std::vector<const EntityState*>& area, const Snapshot* base, const engine::Foci& foci) const
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
        it.distance = Distance(*it.now, foci);
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
            it.priority = (float)(age + 1) / (1.0f + Distance(*it.now, foci) / 6.0f);
            // Off screen with a margin (a view is 8 to 11 blocks centre to corner), new state every third tick when it
            // costs more than its age: the client's own simulation keeps it within a pixel meanwhile (netcode.md, "Drift")
            const auto last = s.lastFresh.find(it.now->id);
            it.hold = last != s.lastFresh.end() && mTick - last->second < (uint32_t)kFarAgeTicks &&
                      Distance(*it.now, foci) > kFullRateRadius && it.freshBits > it.agedBits;
        }
        else
        {
            // A new shot first: the client predicts its own, and one left out it removes within the reach
            it.priority = (it.now->type == engine::kEntityShot ? 1000.0f : 9.0f) / (1.0f + Distance(*it.now, foci) / 6.0f);
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
        candidate.entities.reserve(fixed.entities.size() + items.size());
        size_t fresh = 0;
        float reach = mConfig.interestRadius;
        for (const Item& it : items)
        {
            if (it.pick == 0 && it.now->type == engine::kEntityShot)
            {
                reach = std::min(reach, it.distance - 1.0f); // a new shot left out: the client keeps what it has there
            }
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
        engine::SetSnapshotReach(candidate, reach); // nothing it has goes missing
        SortById(candidate);
        w.Clear();
        SnapshotHeader h = {mTick, base ? base->tick : mTick, s.lastCmdTick, forwarded};
        WriteSnapshotHeader(w, h);
        WriteDelta(w, schema, base ? *base : kEmpty, candidate, Env().snapCheck);
        if (w.ByteCount() <= room || budget < 2048)
        {
            mStats.inArea += (uint32_t)items.size();
            mStats.fresh += (uint32_t)fresh;
            mStats.cut += fresh < items.size();
            s.sent[mTick % kRing] = std::move(candidate);
            for (const Item& it : items)
            {
                if (it.pick == 2)
                {
                    s.lastFresh[it.now->id] = mTick;
                }
            }
            return w.ByteCount() <= room || w.ByteCount() <= (size_t)kMaxParts * (MaxPayload() - kPartHeader); // in more parts then
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

// The client's area: every player and the cars they sit in, get into or are pulled out of (wherever they are: a player
// far off isn't a ped without its car), every train (a few, and a client's own moves on wherever it is), the burning
// debris near it (a shot whose state changes: an aged one would be applied as it was), and the cars, peds, pickups and
// other shots near what its view follows and near its ped (PlayerFoci: apart while it is wasted), nearest first
void net::Server::SendSnapshot(int slot, Slot& s, const Snapshot& world)
{
    const engine::Foci foci = engine::PlayerFoci(slot);
    std::vector<uint32_t> playersCars;
    for (const EntityState& e : world.entities)
    {
        for (uint32_t car : {engine::PlayerCarId(e), engine::PlayerDoorCarId(e)})
        {
            if (car)
            {
                playersCars.push_back(car);
            }
        }
    }
    Snapshot fixed;
    fixed.tick = mTick;
    std::vector<const EntityState*> area;
    for (const EntityState& e : world.entities)
    {
        const bool playersCar = std::find(playersCars.begin(), playersCars.end(), e.id) != playersCars.end();
        const bool near = foci.count && Distance(e, foci) <= mConfig.interestRadius;
        if (e.type == engine::kEntityPlayer || e.type == engine::kEntityWorld || e.type == engine::kEntityTrain || playersCar ||
            (near && engine::ShotNow(e)))
        {
            fixed.entities.push_back(e);
        }
        else if (near)
        {
            area.push_back(&e);
        }
    }
    std::sort(area.begin(), area.end(), [&](const EntityState* a, const EntityState* b) { return Distance(*a, foci) < Distance(*b, foci); });
    const Snapshot& acked = s.sent[s.ackTick % kRing];
    const Snapshot* base = s.ackTick && acked.tick == s.ackTick && mTick - s.ackTick < (uint32_t)kRing ? &acked : NULL;
    std::vector<Item> items = PriceArea(s, area, base, foci);
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
    mFrameEvents.clear();
    if (mReplay)
    {
        uint32_t tick = 0;
        if (!mReplay->Frame(&tick, &mReplayCheck, mFrameEvents))
        {
            EndReplay(mTick - 1);
        }
        if (tick != mTick)
        {
            printf("net: the recording's frame is tick %u's, the replay's %u's\n", tick, mTick);
            mFirstDiff = mFirstDiff ? mFirstDiff : mTick;
            EndReplay(mTick - 1);
        }
        for (const TransportEvent& e : mFrameEvents)
        {
            OnEvent(e);
        }
        return;
    }
    mFrameEvents.swap(mPending);
    TransportEvent e;
    while (mTransport->Poll(e))
    {
        mFrameEvents.push_back(e);
    }
    for (const TransportEvent& e : mFrameEvents)
    {
        OnEvent(e);
    }
    // A connection holds a slot of the host's: one that says no hello in 10 s is dropped (a client says it at once)
    const long long now = NowMs();
    for (auto it = mHelloWait.begin(); it != mHelloWait.end();)
    {
        if (now - it->second > kHelloWaitMs)
        {
            printf("net: dropped a connection that said no hello in %d s\n", kHelloWaitMs / 1000);
            mTransport->Disconnect(it->first);
            it = mHelloWait.erase(it);
        }
        else
        {
            ++it;
        }
    }
}

// Between frames: a client's ping goes back at once (its round trip is then the network's alone, not our tick's
// wait); anything else waits for the frame, as a join or a leave changes the game
void net::Server::Idle()
{
    TransportEvent e;
    if (!mTransport)
    {
        return;
    }
    // Nobody connected and no arena bot: the game stands still (no step, its tick kept) until the network has news
    bool waited = false;
    if (Deserted())
    {
        if (!mTransport->Wait(e, kDesertedWaitMs))
        {
            engine::HoldStep();
            return;
        }
        waited = true;
    }
    // Then whatever came, and while none comes, a wait on the network until shortly before the next step is due: a ping
    // is answered as it comes, and the loop wakes about once a step rather than every millisecond
    for (;;)
    {
        if (!waited && !mTransport->Poll(e))
        {
            const int ms = engine::NetworkWaitMs();
            if (ms <= 0 || !mTransport->Wait(e, ms))
            {
                break;
            }
        }
        waited = false;
        if (e.type == TransportEvent::kReceive && e.channel == kChannelUnreliable && !e.data.empty() && e.data[0] == kMsgPing)
        {
            AnswerPing(e);
            mTransport->Flush();
        }
        else
        {
            mPending.push_back(e);
        }
    }
}

bool net::Server::Deserted() const
{
    if (!mStarted || !mPending.empty() || !mHelloWait.empty())
    {
        return false;
    }
    for (const Slot& slot : mSlots)
    {
        if (slot.used) // a client's or an arena bot's
        {
            return false;
        }
    }
    return true;
}

void net::Server::OnEvent(const TransportEvent& e)
{
    if (e.type == TransportEvent::kConnect)
    {
        mHelloWait[e.peer] = NowMs();
    }
    else if (e.type == TransportEvent::kReceive)
    {
        if (!e.data.empty() && e.data[0] == kMsgHello)
        {
            mHelloWait.erase(e.peer); // welcomed, or refused and disconnected
        }
        OnReceive(e);
    }
    else if (e.type == TransportEvent::kDisconnect)
    {
        mHelloWait.erase(e.peer);
        const int slot = SlotOfPeer(e.peer);
        if (slot >= 0)
        {
            printf("net: player %d (%s) left\n", slot, mSlots[slot].name.c_str());
            const std::string name = mSlots[slot].name;
            const uint32_t token = mSlots[slot].token;
            const bool bot = mSlots[slot].bot;
            mSlots[slot] = Slot();
            mSlots[slot].name = name; // kept for its return
            mSlots[slot].token = token;
            mSlots[slot].bot = bot;
            mSlots[slot].leftTick = mTick;
            if (!engine::MapChangePending()) // else the next game's start leaves the slot out
            {
                engine::TakeOutOfGame(slot);
            }
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
        if (engine::MapChangePending())
        {
            // The slots are the next game's (ChangeToMap): nobody's input goes to the old one's peds, the key events wait
            s.next = 0;
            engine::SetPlayerInput((int)i, 0);
            engine::SetPlayerTyped((int)i, 0, 0);
            continue;
        }
        if (s.peer == kArenaPeer)
        {
            const bool idle = next >= Env().botIdleFrom && next < Env().botIdleTo;
            s.buttons = mStarted && !idle ? mArena.Buttons((int)i) : 0;
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
        players[i].in = mSlots[i].used && (engine::MapChangePending() || engine::SlotInGame((int)i)); // the pause: the next game's slots
        players[i].voter = mSlots[i].used && mSlots[i].peer != kArenaPeer && !mSlots[i].bot;
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
    printf("net: player %d (%s): %s\n", slot, mSlots[slot].name.c_str(), Printable(text).c_str());
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
    if (!engine::MapChangePending())
    {
        engine::TakeOutOfGame(slot);
    }
    Notice(-1, name + " was kicked");
}

// A map for everyone: after a pause (the round's winner on the HUD), the game ends here and on the clients and the
// next starts at once on it
void net::Server::ChangeToMap(int map)
{
    // Its files may have changed since the start: one without a restart zone would end the game here and on the clients
    const size_t n = mConfig.voteMaps.size();
    size_t pick = (size_t)map;
    while (!Playable(mConfig.voteMaps[pick].name, mConfig.voteMaps[pick].gmp, "the next playable map instead"))
    {
        pick = (pick + 1) % n;
        if (pick == (size_t)map)
        {
            fprintf(stderr, "net: no map here is playable any more: the server ends\n");
            Shutdown(); // LeaveServer's work for a server (atexit, skipped by _Exit): the clients hear of it
            fflush(NULL);
            _Exit(1); // not exit(): the game's destructors would run under its threads still going (as in Client)
        }
    }
    mMap = pick;
    const MapFiles& m = mConfig.voteMaps[mMap];
    // The slots renumbered from 0 for it (CompactSlots): every player stays, as every map has the server's slots
    std::vector<bool> taken(mSlots.size()), kept(mSlots.size());
    const uint32_t tps = 1000 / (mConfig.game.tickMs > 0 ? mConfig.game.tickMs : 33);
    for (size_t i = 0; i < mSlots.size(); i++)
    {
        taken[i] = mSlots[i].used;
        kept[i] = !mSlots[i].used && !mSlots[i].name.empty() && mTick - mSlots[i].leftTick < kComeBackSec * tps;
    }
    const int open = (int)mSlots.size();
    const std::vector<int> to = CompactSlots(taken, kept, open);
    mRounds.Renumber(to);
    std::vector<Slot> slots((size_t)open);
    for (size_t i = 0; i < mSlots.size(); i++)
    {
        if (to[i] < 0)
        {
            continue;
        }
        if (to[i] != (int)i)
        {
            printf("net: player %zu (%s) is player %d on %s\n", i, mSlots[i].name.c_str(), to[i], m.name.c_str());
        }
        slots[(size_t)to[i]] = std::move(mSlots[i]);
    }
    mSlots = std::move(slots);
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
    change.players = (uint8_t)mSlots.size();
    for (size_t i = 0; i < mSlots.size(); i++)
    {
        if (mSlots[i].used && mSlots[i].peer != kArenaPeer)
        {
            change.slot = (uint8_t)i;
            BitWriter w;
            WriteMapChange(w, change);
            mTransport->Send(mSlots[i].peer, kChannelReliable, w.Data().data(), w.ByteCount(), true);
        }
    }
    // A fresh game of the round flow's (its countdown next) comes after its results: half a second for the clients to
    // hear of it. Else (an arena's rotation, bots only) the pause shows the winner
    engine::ChangeMap(m.gmp, m.sty, m.scr, (int)mSlots.size(), 0, mRounds.State().phase == kPhaseCountdown ? 15 : 90);
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
    CheckWorld(world);
    SetNextInputs(world);
    RunRounds(world);
    if (mConfig.endTick && mTick >= mConfig.endTick)
    {
        if (mReplay)
        {
            EndReplay(mTick);
        }
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
    if (mTick % (1000 / (mConfig.game.tickMs > 0 ? mConfig.game.tickMs : 33)) == 0)
    {
        SendPings();
    }
    mTransport->Flush();
    if (mTick % 300 == 0)
    {
        PrintStats(world);
    }
}

void net::Server::CheckWorld(const Snapshot& world)
{
    DumpWorld(world);
    const uint32_t check = WorldCheck(world);
    if (mRecorder)
    {
        mRecorder->Frame(mTick, check, mFrameEvents);
    }
    if (mReplay && check != mReplayCheck && !mFirstDiff)
    {
        mFirstDiff = mTick;
        printf("net: the replay's world differs from the recording's at tick %u\n", mTick);
    }
}

// The recording played to its end: the replay's verdict, and its exit code
void net::Server::EndReplay(uint32_t ticks)
{
    if (mFirstDiff)
    {
        printf("net: replayed %u ticks: the world differs from tick %u on\n", ticks, mFirstDiff);
    }
    else
    {
        printf("net: replayed %u ticks: the world matches the recording's at every tick\n", ticks);
    }
    fflush(stdout);
    _Exit(mFirstDiff ? 1 : 0);
}
