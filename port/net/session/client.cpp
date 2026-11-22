// port/net (port/docs/netcode.md, "The client"): joins a server, starts the same network game, sends its input every frame
// (repeating the last few commands), and puts the server's snapshots into its game.
#include "client.hpp"
#include "env.hpp"
#include "../maps.hpp"
#include <algorithm>
#include <ctype.h>
#include <chrono>
#include <math.h>
#include <set>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <thread>
#include <unordered_map>

namespace
{
const int kSaves = 32; // ticks a snapshot may be behind and still be reconciled (about 1 s)
const int kMaxFastForward = 8; // ticks our commands may come late and be caught up at once (FastForward); more: a jump
const int kMaxAhead = 20; // ticks our commands may be early beyond two before a jump back; from 12 on, slower pacing
const int kStallMs = 700; // no snapshot for this long: the connection is out (ENet drops it after 5 to 10 s)
const int kReconnectMs = 3000; // between attempts to connect again
const int kGiveUpMs = 30000; // reconnecting this long: the server is gone, back to the menu

long long NowMs()
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

} // namespace

// The names a server (or a recording) gives its map's files: bare names in data/ only (BareFileName), else <error> says
// which, and nothing is opened by them
static bool PlainNames(const std::string& bad, const char* from, std::string* error)
{
    if (bad.empty())
    {
        return true;
    }
    *error = std::string(from) + "'s " + bad + " name isn't a plain file name in data/: refused";
    return false;
}

void net::CorrectionStats::Print(uint32_t frame, const char* what) const
{
    printf("net: frame %u, %s' corrections: %u, %.2f blocks on average, up to %.2f, %u over 2 blocks (jumped)\n", frame, what, count,
           count ? sum / count : 0.0f, max, jumps);
}

void net::Client::Welcomed(const Welcome& welcome, engine::NetworkGame* pGame)
{
    mSlot = welcome.slot;
    mNextSlot = welcome.slot;
    mSession = welcome.session;
    mToken = welcome.token;
    mServerGame = welcome.game; // during a map change's pause, the next game: the old one's snapshots aren't for us
    mConfig.name = welcome.name; // a reconnect asks for it: the server keeps our slot under it
    mMaps = welcome.maps;
    mMapTitles.clear();
    mMapPlayers.clear();
    for (const std::string& m : mMaps)
    {
        mMapTitles.push_back(MapTitle(m)); // every client has the maps (the data check)
        mMapPlayers.push_back(MapOwnPlayers(m));
    }
    mConnected = true;
    pGame->players = welcome.players;
    pGame->localPlayer = welcome.slot;
    pGame->gameType = welcome.gameType;
    pGame->fragLimit = welcome.fragLimit;
    pGame->timeLimitMin = welcome.timeLimitMin;
    pGame->tickMs = welcome.tickMs;
    mTickMs = welcome.tickMs ? welcome.tickMs : 33;
    mGameType = welcome.gameType;
    mFragLimit = (int)welcome.fragLimit;
    pGame->police = welcome.police;
    pGame->gmp = welcome.gmp;
    pGame->sty = welcome.sty;
    pGame->scr = welcome.scr;
    mScr = welcome.scr;
    mProbe.Start();
}

// A map the game can start: one without a restart zone ends it with a fatal error (a server's checks leave such a map out)
static bool PlayableMap(const std::string& map, const std::string& gmp, std::string* error)
{
    if (net::Playable(map, gmp, NULL))
    {
        return true;
    }
    *error = "the server's map " + map + " has no restart zone (data/" + gmp + "): not playable";
    return false;
}

bool net::Client::StartReplay(const char* path, engine::NetworkGame* pGame, std::string* error)
{
    Welcome welcome;
    mReplay.reset(new ClientReplay());
    if (!mReplay->Open(path, &welcome, error))
    {
        return false;
    }
    if (!PlainNames(BadMapName(welcome), "the recording", error) || !PlayableMap(welcome.map, welcome.gmp, error))
    {
        return false;
    }
    Welcomed(welcome, pGame);
    printf("net: replaying %s as player %d of %d, %s\n", path, mSlot, welcome.players, welcome.scr.c_str());
    return true;
}

bool net::Client::BeginConnect(const ClientConfig& config, std::string* error)
{
    mConfig = config;
    mTransport.reset(CreateClientTransport(config.host.c_str(), config.port, error));
    if (!mTransport)
    {
        return false;
    }
    if (config.link.Any())
    {
        mTransport.reset(CreateConditioner(mTransport.release(), config.link));
    }
    mHelloSent = false;
    mJoinWaitMs = NowMs();
    return true;
}

// With what to check: the port is written out, as a forgotten ":port" is a common cause
std::string net::Client::NoAnswer() const
{
    const std::string port = std::to_string(mConfig.port);
    return "no answer from " + mConfig.host + ":" + port + ": is the server running, is UDP " + port + " open?";
}

// The connection, our hello, the server's welcome: 1 welcomed (the game to start is in <pGame>), 0 not yet, -1 failed
int net::Client::PollConnect(engine::NetworkGame* pGame, std::string* error)
{
    TransportEvent e;
    while (mTransport->Poll(e))
    {
        if (e.type == TransportEvent::kDisconnect)
        {
            *error = mHelloSent ? "the server closed the connection" : NoAnswer();
            return -1;
        }
        if (e.type == TransportEvent::kConnect && !mHelloSent)
        {
            Hello hello;
            hello.build = mConfig.build;
            hello.name = mConfig.name;
            hello.carInfo = engine::DataFileCrc("nyc.gci");
            BitWriter w;
            WriteHello(w, hello);
            mTransport->Send(0, kChannelReliable, w.Data().data(), w.ByteCount(), true);
            mTransport->Flush();
            mHelloSent = true;
            mJoinWaitMs = NowMs(); // and as long again for the welcome
        }
        else if (e.type == TransportEvent::kReceive && mHelloSent && e.channel == kChannelReliable)
        {
            return OnWelcome(e, pGame, error) ? 1 : -1;
        }
    }
    if (NowMs() - mJoinWaitMs > 6000)
    {
        *error = mHelloSent ? "the server didn't welcome us" : NoAnswer();
        return -1;
    }
    return 0;
}

bool net::Client::Connect(const ClientConfig& config, engine::NetworkGame* pGame, std::string* error)
{
    if (!BeginConnect(config, error))
    {
        return false;
    }
    int result = 0;
    while (!(result = PollConnect(pGame, error)))
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return result > 0;
}

// A map this machine lacks ends the game with its fatal error at the first file it opens: <error> names it instead
static bool HaveMap(const std::string& gmp, const std::string& sty, const std::string& scr, std::string* error)
{
    const std::string missing = net::engine::MissingMapFile(gmp, sty, scr);
    if (missing.empty())
    {
        return true;
    }
    const std::string name = scr.substr(0, scr.rfind('.'));
    *error = "no map " + name + " here (data/" + missing + ")";
    return false;
}

// The same map as the server's: its files as the server has them, else <error> says which differs
static bool SameMap(const std::string& map, const std::string& gmp, const std::string& sty, const std::string& scr, const net::MapPrint& print,
                    std::string* error)
{
    const std::string* differs = net::engine::DataFileCrc(gmp) != print.gmp   ? &gmp
                                 : net::engine::DataFileCrc(sty) != print.sty ? &sty
                                 : net::engine::DataFileCrc(scr) != print.scr ? &scr
                                                                              : NULL;
    if (!differs)
    {
        return true;
    }
    *error = "your copy of " + map + " differs from the server's (data/" + *differs + ")";
    return false;
}

bool net::Client::OnWelcome(const TransportEvent& e, engine::NetworkGame* pGame, std::string* error)
{
    BitReader r(e.data.data(), e.data.size());
    const uint32_t type = r.Bits(8);
    if (type == kMsgReject)
    {
        Reject reject;
        ReadReject(r, reject);
        *error = Printable(reject.reason);
        return false;
    }
    Welcome welcome;
    if (type != kMsgWelcome || !ReadWelcome(r, welcome) || welcome.version != kProtocolVersion)
    {
        *error = "a welcome we don't understand";
        return false;
    }
    if (!PlainNames(BadMapName(welcome), "the server", error) || !HaveMap(welcome.gmp, welcome.sty, welcome.scr, error) || !SameMap(welcome.map, welcome.gmp, welcome.sty, welcome.scr, welcome.print, error) ||
        !PlayableMap(welcome.map, welcome.gmp, error))
    {
        SendLeave(*error);
        Leave();
        return false;
    }
    Welcomed(welcome, pGame);
    // GTA2_NET_RECORD=<file>, or off; a person's session is recorded anyway, a bot's only when asked
    const char* record = Env().record;
    const std::string path = record ? (strcmp(record, "off") ? record : "") : mConfig.bot.empty() ? NextRecordingPath() : "";
    if (!path.empty())
    {
        mRecorder.reset(new ClientRecorder());
        if (!mRecorder->Open(path.c_str(), welcome))
        {
            printf("net: can't record to %s\n", path.c_str());
            mRecorder.reset();
        }
        else
        {
            printf("net: recording to %s (F12 or the key under Esc marks a moment)\n", path.c_str());
        }
    }
    printf("net: joined %s:%u as player %d of %d, %s\n", mConfig.host.c_str(), mConfig.port, mSlot, welcome.players, welcome.scr.c_str());
    return true;
}

void net::Client::OnSnapshotPart(BitReader& r)
{
    uint32_t tick = 0;
    int index = 0, count = 0;
    std::vector<uint8_t> piece;
    if (!ReadSnapshotPart(r, &tick, &index, &count, piece) || tick <= mLatestTick)
    {
        return;
    }
    Parts& p = mParts[tick % 4];
    if (p.tick != tick || p.count != count)
    {
        p.tick = tick; // a newer snapshot's: one that never came whole is dropped
        p.count = count;
        p.have = 0;
        p.pieces.assign((size_t)count, std::vector<uint8_t>());
    }
    if (!p.pieces[(size_t)index].empty())
    {
        return; // had it
    }
    p.pieces[(size_t)index] = piece;
    if (++p.have < p.count)
    {
        return;
    }
    std::vector<uint8_t> message;
    for (int i = 0; i < count; i++)
    {
        message.insert(message.end(), p.pieces[(size_t)i].begin(), p.pieces[(size_t)i].end());
    }
    p.tick = 0;
    BitReader whole(message.data(), message.size());
    if (whole.Bits(8) == kMsgSnapshot)
    {
        OnSnapshot(whole);
    }
}

// GTA2_NET_SNAPCHECK on the server: the entities this client read wrong, each with how it came and its fields against
// the baseline's, each entity's first; the server prints what it sent (ReportSent): port/net/scripts/snapdiff.py pairs them
void net::Client::ReportBadEntities(const SnapshotHeader& h, const Snapshot& base, const Snapshot& snap, const DeltaCheck& check)
{
    // Each entity's first: it stays wrong after (an unchanged field carries the wrong value on), and the first is how
    static std::set<uint32_t> sReported;
    static const char* const kHow[] = {"carried from the baseline", "updated", "created"};
    const Schema& schema = engine::NetSchema();
    for (size_t k : check.bad)
    {
        const EntityState& e = snap.entities[k];
        if (sReported.size() >= 20 || !sReported.insert(e.id).second)
        {
            continue;
        }
        const EntityState* was = NULL;
        for (const EntityState& b : base.entities)
        {
            was = b.id == e.id ? &b : was;
        }
        printf("net: CHECK frame %u: snapshot %u (baseline %u%s): entity %u (%s) is wrong, %s:%s\n", mFrame, h.tick, h.baseTick,
               h.baseTick == h.tick ? ", none" : "", e.id, schema.types[e.type].name, kHow[check.how[k]], DescribeFields(schema, e, was).c_str());
        // The server prints its own (session/server.cpp, OnCommand): the two side by side say which field
        SendCommand("/snapbad " + std::to_string(h.tick) + " " + std::to_string(h.baseTick) + " " + std::to_string(e.id));
    }
}

// The fuzz test (GTA2_NET_FUZZ): what a hostile server could send in each field, its width's every value, the edges
// most often
static void Fuzz(net::Snapshot& snap)
{
    using namespace net;
    static uint32_t sRng = Env().fuzz;
    auto next = [] {
        sRng ^= sRng << 13;
        sRng ^= sRng >> 17;
        sRng ^= sRng << 5;
        return sRng;
    };
    for (EntityState& e : snap.entities)
    {
        const EntityType& t = engine::NetSchema().types[e.type];
        for (size_t i = 0; i < t.fields.size(); i++)
        {
            if (next() % 1000 >= Env().fuzzRate)
            {
                continue;
            }
            const FieldDesc& d = t.fields[i];
            const uint32_t mask = d.bits >= 32 ? 0xFFFFFFFFu : (1u << d.bits) - 1;
            const uint32_t pick = next() % 6;
            uint32_t v = pick == 0 ? 0 : pick == 1 ? mask : pick == 2 ? mask >> 1 : pick == 3 ? (mask >> 1) + 1 : pick == 4 ? e.f[i] + 1 : next();
            v &= mask;
            if (d.isSigned && d.bits < 32 && (v >> (d.bits - 1)) & 1)
            {
                v |= ~mask; // as the reader sign-extends
            }
            e.f[i] = (int32_t)v;
        }
    }
}

void net::Client::OnSnapshot(BitReader& r)
{
    SnapshotHeader h;
    if (!ReadSnapshotHeader(r, h) || h.tick <= mLatestTick)
    {
        return; // malformed, or older than what we have
    }
    static const Snapshot kEmpty;
    const Snapshot* base = &kEmpty;
    if (h.baseTick != h.tick)
    {
        base = &mReceived[h.baseTick % kRing];
        if (base->tick != h.baseTick)
        {
            return; // a baseline we no longer have: the server sends a full one once our ack is old
        }
    }
    Snapshot snap;
    DeltaCheck check;
    if (!ReadDelta(r, engine::NetSchema(), *base, snap, &check))
    {
        return;
    }
    if (!check.bad.empty())
    {
        ReportBadEntities(h, *base, snap, check);
    }
    if (Env().fuzz)
    {
        Fuzz(snap);
    }
    engine::SanitizeSnapshot(snap);
    snap.tick = h.tick;
    mReceived[h.tick % kRing] = snap;
    mForwarded[h.tick % kRing] = h.forwarded;
    mLatestTick = h.tick;
    mLink.snapshots++;
    mSlack = (int)h.lastCmdTick - (int)(h.tick + 1);
    mSlackKnown = mSynced && h.lastCmdTick > mJumpTick; // it has heard of the ticks since our last jump
    mNewSnapshot = true;
    mLastSnapshotMs = NowMs();
}

uint32_t net::Client::Lead() const
{
    // Half the round trip in ticks, and two to spare: our command for a tick should arrive just before it runs
    return (mRtt / 2 + mTickMs - 1) / mTickMs + 2;
}

int net::Client::InView(float x, float y) const
{
    float px, py;
    if (!engine::PlayerPosition(mSlot, &px, &py))
    {
        return 1;
    }
    const float dx = x - px, dy = y - py;
    const float r = engine::PlayerViewRadius(mSlot) + 1.0f;
    return dx * dx + dy * dy <= r * r ? 0 : 1;
}

// The display error: what tick S showed of the cars, peds and players against the server's S
void net::Client::MeasureShownError(const Snapshot& snap)
{
    const TickRecord& rec = mHist[snap.tick % kRing];
    if (rec.shownTick != snap.tick)
    {
        return;
    }
    auto error = [&](const engine::ReplicaPos& shown, float* pError) {
        for (const EntityState& e : snap.entities) // sorted by id
        {
            if (e.id == shown.id)
            {
                if (engine::EntityAge(e))
                {
                    return false; // only against new state: an aged entry carries what the client had, from ticks before
                }
                const float dx = engine::Blocks(e.f[0]) - shown.x, dy = engine::Blocks(e.f[1]) - shown.y;
                *pError = sqrtf(dx * dx + dy * dy);
                return true;
            }
            if (e.id > shown.id)
            {
                break;
            }
        }
        return false;
    };
    float e = 0;
    for (const engine::ReplicaPos& shown : rec.shown)
    {
        if (error(shown, &e))
        {
            mStats.shownErrorSum += e;
            mStats.shownErrorCount++;
            mStats.shownErrorMax = e > mStats.shownErrorMax ? e : mStats.shownErrorMax;
            const int v = InView(shown.x, shown.y);
            mStats.shownViewSum[v] += e;
            mStats.shownViewCount[v]++;
            mStats.shownViewMax[v] = e > mStats.shownViewMax[v] ? e : mStats.shownViewMax[v];
        }
    }
    for (const engine::ReplicaPos& shown : rec.players)
    {
        if (error(shown, &e) && e <= 2) // further: a respawn or a car entered, which jumps
        {
            (shown.id == (uint32_t)mSlot + 1 ? mStats.self : mStats.others).Add(e);
        }
    }
}

void net::Client::RecordShown()
{
    TickRecord& rec = mHist[mTick % kRing];
    if (rec.tick != mTick)
    {
        return;
    }
    engine::ReplicaPositions(rec.shown);
    rec.players.clear();
    for (int i = 0; i < engine::PlayerCount(); i++)
    {
        float x, y;
        if (engine::PlayerPosition(i, &x, &y))
        {
            rec.players.push_back(engine::ReplicaPos{(uint32_t)i + 1, x, y});
        }
    }
    rec.shownTick = mTick;
}

// The other players' inputs (we predict them with these), the marker, and the prediction error: where we had ourselves
// at S against where the server has us
void net::Client::TakePlayerStates(const Snapshot& snap)
{
    const TickRecord& rec = mHist[snap.tick % kRing];
    mKnown = Env().noForward ? ForwardedInputs() : mForwarded[snap.tick % kRing];
    mKnownTick = snap.tick;
    for (const EntityState& e : snap.entities)
    {
        int player = 0;
        uint32_t buttons = 0;
        if (!engine::PlayerOf(e, &player, &buttons) || player < 0 || player >= (int)mRemote.size())
        {
            continue;
        }
        // Past the controls the server forwarded, the last of them is held, a turn only if the one before had it too
        const uint32_t turn = kBtnLeft | kBtnRight;
        const int n = player < kMaxForwardSlots ? mKnown.count[player] : 0;
        const uint32_t last = n ? mKnown.controls[player][n - 1] : buttons;
        const uint32_t before = n > 1 ? mKnown.controls[player][n - 2] : n ? buttons & 0xFFF : mRemoteSeen[player];
        mRemote[player] = (last & ~turn) | (last & before & turn);
        mRemoteSeen[player] = buttons;
        if (player == mSlot)
        {
            mProbe.Marker(e);
        }
        if (player == mSlot && rec.tick == snap.tick)
        {
            const float dx = engine::Blocks(e.f[0]) - rec.predX, dy = engine::Blocks(e.f[1]) - rec.predY;
            const float err = sqrtf(dx * dx + dy * dy);
            mStats.maxError = err > mStats.maxError ? err : mStats.maxError;
        }
    }
}

// The server's forwarded controls for that tick if it sent them, else the held input. With no snapshot for a while the
// held one is let go: the fire after 5 ticks, the rest after 10 (nobody runs on or shoots on through a stall)
uint32_t net::Client::RemoteInput(int player, uint32_t tick) const
{
    const uint32_t k = tick - mKnownTick - 1;
    if (player < kMaxForwardSlots && tick > mKnownTick && k < mKnown.count[player])
    {
        return mKnown.controls[player][k];
    }
    const int stalled = mSynced && !Env().holdAll ? (int)(tick - mTakenAt) : 0;
    if (stalled == 11 && tick == mTick + 1 && mRemote[player])
    {
        printf("net: frame %u, no snapshot for 10 ticks: player %d's held input 0x%x let go\n", mFrame, player, mRemote[player]);
    }
    return stalled > 10 ? 0 : stalled > 5 ? mRemote[player] & ~(uint32_t)kBtnAttack : mRemote[player];
}

// A snapshot of tick S: back to the game as it was after S, the server's state on top, then the ticks since then
// again with the inputs we used (other players: their last known input)
void net::Client::Reconcile(const Snapshot& snap)
{
    const uint32_t s = snap.tick;
    MeasureShownError(snap);
    TakePlayerStates(snap);
    engine::SavedWorld* saved = s <= mTick && s + kSaves > mTick ? mSaves[s % kSaves].get() : NULL;
    if (mProbe.Alone())
    {
        mProbe.CompareAlone(snap, saved, *mSaves[mTick % kSaves]);
        return;
    }
    if (!saved || !saved->valid || saved->tick != s || !engine::Restorable(*saved))
    {
        engine::Apply(snap); // too old (or too new, or saved with another rollback set) to reconcile: take it as it is
        mProbe.AfterPlainApply(snap);
        mStats.plainApplies += mSynced;
        engine::ApplyScores(snap);
        return;
    }
    std::vector<engine::Pose> before; // what this frame would have shown, to blend the correction in
    engine::Poses(before);
    const engine::LocalChat chat = engine::TakeLocalChat();
    using Clock = std::chrono::steady_clock;
    const auto us = [](Clock::time_point a, Clock::time_point b) {
        return (double)std::chrono::duration_cast<std::chrono::microseconds>(b - a).count();
    };
    const Clock::time_point start = Clock::now();
    mStats.staleRestores += mStale[s % kSaves];
    engine::RestoreGame(*saved);
    mProbe.AfterRestore(snap);
    engine::Apply(snap);
    engine::ApplyScores(snap); // the resimulated ticks and their saves carry the server's scores
    mProbe.AfterApply(snap, mTick);
    Clock::time_point mark = Clock::now();
    mStats.restoreUs += us(start, mark);
    std::vector<uint32_t> buttons(mRemote);
    for (uint32_t t = s + 1; t <= mTick; t++)
    {
        for (size_t i = 0; i < buttons.size(); i++)
        {
            buttons[i] = RemoteInput((int)i, t);
        }
        buttons[mSlot] = mHist[t % kRing].tick == t ? mHist[t % kRing].buttons : 0;
        if (t == mTick)
        {
            engine::ResimStepStart();
        }
        engine::ResimTick(buttons.data(), (int)buttons.size());
        mProbe.AfterResim();
        const Clock::time_point ticked = Clock::now();
        mStats.resimUs += us(mark, ticked);
        // Only the first ticks: the next snapshots restore those (a save is 3 MB, most of a rollback's time); the later
        // ones keep their save of before, still a world of that tick, which a snapshot after a few lost ones restores
        const int keep = Env().resimSaves;
        if (keep <= 0 || t <= s + (uint32_t)keep)
        {
            engine::SaveGame(*mSaves[t % kSaves], t);
            mStale[t % kSaves] = false;
        }
        else
        {
            mStale[t % kSaves] = true;
        }
        mark = Clock::now();
        mStats.saveUs += us(ticked, mark);
        mStats.resimTicks++;
        mLink.resimTicks++;
    }
    const double total = us(start, mark);
    mStats.reconcileWorstUs = total > mStats.reconcileWorstUs ? total : mStats.reconcileWorstUs;
    engine::PutLocalChat(chat);
    mStats.rollbacks++;
    mLink.rollbacks++;
    engine::ApplyScores(snap);
    Blend(before);
}

// The correction in steps: what is shown moves from where it was a part of the way to the corrected state each frame
// (GTA2_NET_BLEND, 0.2; 1: at once). The saves keep the corrected state, so the blend only touches the live tick
// before the next snapshot's resimulation; more than 2 blocks off (a respawn, a car entered) jumps.
void net::Client::Blend(const std::vector<engine::Pose>& shown)
{
    const SessionEnv& env = Env();
    if ((env.blend >= 1 || env.blend <= 0) && (env.blendSelf >= 1 || env.blendSelf <= 0))
    {
        return;
    }
    std::vector<engine::Pose> now;
    engine::Poses(now);
    std::unordered_map<uint32_t, const engine::Pose*> before;
    for (const engine::Pose& p : shown)
    {
        before[p.id] = &p;
    }
    const int32_t kFar = 2 << 14;
    uint32_t selfCar = 0; // the car the local player drives, as the newest snapshot has it
    for (const EntityState& e : mReceived[mLatestTick % kRing].entities)
    {
        if (e.id == (uint32_t)mSlot + 1)
        {
            selfCar = engine::PlayerCarId(e);
        }
    }
    for (const engine::Pose& n : now)
    {
        auto it = before.find(n.id);
        if (it == before.end())
        {
            continue;
        }
        const engine::Pose& b = *it->second;
        const int32_t dx = n.x - b.x, dy = n.y - b.y, dz = n.z - b.z;
        const int32_t dr = ((n.rot - b.rot) % 1440 + 1440 + 720) % 1440 - 720; // the short way round
        const bool jumped = abs(dx) > kFar || abs(dy) > kFar;
        if (dx || dy)
        {
            CorrectionStats* stats = engine::IsPlayerId(n.id) ? &mStats.players : engine::IsCarId(n.id) ? &mStats.cars : NULL;
            if (stats)
            {
                stats->Add(sqrtf((float)dx * dx + (float)dy * dy) / 16384.0f, jumped);
            }
            if (stats == &mStats.cars)
            {
                mStats.carsView[InView(engine::Blocks(n.x), engine::Blocks(n.y))].Add(sqrtf((float)dx * dx + (float)dy * dy) / 16384.0f, jumped);
            }
        }
        mProbe.Correction(mFrame, n, dx, dy, dr, mReceived[mLatestTick % kRing]);
        if (jumped || abs(dz) > kFar || (!dx && !dy && !dz && !dr))
        {
            continue;
        }
        // A larger part of a larger error (it shows the wrong place longer), and its own part for the local player
        const float error = sqrtf((float)dx * dx + (float)dy * dy) / (float)kFar;
        const float alpha = n.id == (uint32_t)mSlot + 1 || (selfCar && n.id == selfCar) ? env.blendSelf : env.blend + (env.blendFar - env.blend) * (error < 1 ? error : 1);
        if (alpha >= 1 || alpha <= 0)
        {
            continue; // the corrected state as it is
        }
        const engine::Pose p = {n.id, b.x + (int32_t)(dx * alpha), b.y + (int32_t)(dy * alpha), b.z + (int32_t)(dz * alpha),
                                ((b.rot + (int32_t)(dr * alpha)) % 1440 + 1440) % 1440};
        engine::SetPose(p);
    }
}

// What arrives this frame: from the network, or the recording (false: the recording is over)
bool net::Client::TakeEvents(std::vector<TransportEvent>& events, uint32_t* pRecordedInput)
{
    if (mReplay)
    {
        uint32_t recordedFrame = 0;
        return mReplay->Frame(&recordedFrame, &mRtt, pRecordedInput, events);
    }
    events.swap(mPending);
    TransportEvent e;
    while (mTransport && mTransport->Poll(e))
    {
        if (!TakePong(e))
        {
            events.push_back(e);
        }
    }
    mRtt = mTransport ? mTransport->RoundTripMs(0) : 0;
    return true;
}

// Between frames: polled every millisecond or so, so a pong is timed when it comes, not at the next frame
void net::Client::Idle()
{
    TransportEvent e;
    while (!mReplay && mTransport && mTransport->Poll(e))
    {
        if (!TakePong(e))
        {
            mPending.push_back(e);
        }
    }
}

bool net::Client::TakePong(const TransportEvent& e)
{
    if (e.type != TransportEvent::kReceive || e.channel != kChannelUnreliable || e.data.empty() || e.data[0] != kMsgPong)
    {
        return false;
    }
    BitReader r(e.data.data(), e.data.size());
    r.Bits(8);
    const uint32_t sent = r.Bits(32);
    const uint32_t sample = (uint32_t)NowMs() - sent;
    if (sample < 10000) // A pong from before a reconnect or a clock wrap, dropped
    {
        mPingMs = mPingMs ? (mPingMs * 3 + sample) / 4 : sample;
    }
    return true;
}

// Twice a second: our clock to the server, which sends it straight back
void net::Client::SendPing()
{
    const long long now = NowMs();
    if (!mTransport || now - mPingSentMs < 500)
    {
        return;
    }
    mPingSentMs = now;
    BitWriter w;
    w.Bits(kMsgPing, 8);
    w.Bits((uint32_t)now, 32);
    mTransport->Send(0, kChannelUnreliable, w.Data().data(), w.ByteCount(), false);
    mTransport->Flush();
}

void net::Client::StartBot()
{
    mBot = CreateBotBrain(mConfig.bot);
    mNavMap.reset(new engine::NavMap());
    engine::BuildNavMap(*mNavMap);
    if (Env().navDump)
    {
        engine::DumpNavMap(*mNavMap, Env().navDump);
    }
}

// This frame's input: the controls', the recording's, or the bot's. First thing: a restore would overwrite the game's
// input buffers
uint32_t net::Client::ChooseInput(uint32_t recordedInput)
{
    uint32_t local = mReplay ? recordedInput : engine::LocalInput();
    if (!mConfig.bot.empty() && engine::GameRunning())
    {
        if (!mBot)
        {
            StartBot();
        }
        FillBotView(mBotView, mReceived[mLatestTick % kRing], mSlot, mFrame, mNavMap.get());
        // The keyboard's key events go with a bot's controls: Escape and the leave prompt, the chat (GTA2_INPUT in tests)
        local = (mBot ? mBot->Think(mBotView) : 0) | (local & kKeyEventBits);
    }
    local &= 0x3FFFFF;
    // F12, or the key under Esc (` or §: keyboards without F12), keys the game doesn't use (here as their DirectInput
    // codes and the key-down bit: Source/input_bits.hpp), mark the moment, in play and in its replay alike
    const uint32_t kKeyDown = 0x200000, kMarkF12 = 0x58, kMarkGrave = 0x29; // DIK_F12, DIK_GRAVE
    const uint32_t key = (local >> 12) & 0x1FF;
    if ((local & kKeyDown) && (key == kMarkF12 || key == kMarkGrave))
    {
        if (mFrame != mMarkFrame + 1) // held: one mark
        {
            printf("net: mark %d at frame %u, tick %u\n", ++mMarks, mFrame, mTick);
        }
        mMarkFrame = mFrame;
    }
    // Escape, and every key while the pause menu shows, are this client's: the menu, over the game going on (MenuKey).
    // Neither the game here nor the server gets them: the game's own pause and quit prompt are on its player, which the
    // snapshots and resimulations set again, and the server's quit ended everyone's game. While the menu shows the
    // player stands still: no controls go. While a chat line is typed, Escape is the line's (dropped: the game has no
    // use for it there)
    const uint32_t kKeyBits = 0x1FF000, kEscape = 0x01; // DIK_ESCAPE
    const bool menu = engine::MenuOpen();
    const bool chatting = !menu && engine::TakeLocalChat().chatting;
    if (!mReplay && (local & kKeyDown) && (key == kEscape || menu))
    {
        if ((mFrame != mMenuKeyFrame + 1 || key != mMenuKey) && !chatting) // held: once
        {
            engine::MenuKey(key, engine::TypedChar(local));
        }
        mMenuKeyFrame = mFrame;
        mMenuKey = key;
        local &= ~(kKeyDown | kKeyBits);
    }
    if (!mReplay && engine::MenuOpen())
    {
        local = 0;
    }
    ActOnMenu();
    if (!mReplay && (local & kKeyDown))
    {
        CompleteChat(local, key);
        RoundKeys(local, key);
    }
    if (InputsHeld(engine::CurrentRound().phase))
    {
        local &= kKeyEventBits; // as the server has it: held in the results and the countdown, the chat goes on
    }
    mProbe.Input(mFrame, local);
    return local;
}

void net::Client::ActOnMenu()
{
    switch (engine::TakeMenuChoice())
    {
        case engine::kMenuLeave:
            printf("net: frame %u, left the game\n", mFrame);
            LeaveToMenu("");
            break;
        case engine::kMenuReady:
            SendCommand("/ready");
            break;
        case engine::kMenuYes:
            SendCommand("/yes");
            break;
        case engine::kMenuNo:
            SendCommand("/no");
            break;
        default:
            break;
    }
}

// For the test log: the deaths this game carried out, and how often it had a player in or out of a car otherwise than
// the newest snapshot
void net::Client::CountTestStats()
{
    mWasWasted.resize(mRemote.size());
    mLocalDeaths.resize(mRemote.size());
    for (size_t i = 0; i < mRemote.size(); i++)
    {
        const bool w = engine::PlayerWasted((int)i);
        mLocalDeaths[i] += w && !mWasWasted[i];
        mWasWasted[i] = w;
    }
    for (const EntityState& e : mReceived[mLatestTick % kRing].entities)
    {
        int p = 0;
        uint32_t b = 0;
        if (engine::PlayerOf(e, &p, &b))
        {
            mStats.carFrames++;
            mStats.carMismatch += (engine::PlayerCarId(e) != 0) != engine::PlayerInCar(p);
        }
    }
}

// The tick that just ran: saved, and what it showed
void net::Client::RecordTick()
{
    mTick++;
    engine::SaveGame(*mSaves[mTick % kSaves], mTick);
    mStale[mTick % kSaves] = false;
    TickRecord& rec = mHist[mTick % kRing];
    engine::PlayerPosition(mSlot, &rec.predX, &rec.predY);
    rec.shownTick = 0; // until RecordShown
}

void net::Client::HandleEvents(const std::vector<TransportEvent>& events)
{
    for (const TransportEvent& e : events)
    {
        if (e.type == TransportEvent::kReceive && e.channel == kChannelUnreliable)
        {
            BitReader r(e.data.data(), e.data.size());
            const uint32_t type = r.Bits(8);
            if (type == kMsgSnapshot)
            {
                OnSnapshot(r);
            }
            else if (type == kMsgSnapshotPart)
            {
                OnSnapshotPart(r);
            }
        }
        else if (e.type == TransportEvent::kReceive)
        {
            BitReader r(e.data.data(), e.data.size());
            const uint32_t type = r.Bits(8);
            Welcome welcome;
            Reject reject;
            if (type == kMsgMapChange)
            {
                OnMapChange(r);
            }
            else if (type == kMsgWelcome && !mConnected && ReadWelcome(r, welcome))
            {
                Rejoined(welcome);
            }
            else if (type == kMsgNotice)
            {
                std::string text;
                if (ReadText(r, text))
                {
                    engine::ShowHudMessage(text.c_str());
                    printf("net: frame %u, notice: %s\n", mFrame, Printable(text).c_str());
                }
            }
            else if (type == kMsgReject && mConnected && ReadReject(r, reject))
            {
                printf("net: frame %u, the server put us out: %s\n", mFrame, Printable(reject.reason).c_str());
                const bool shown = reject.reason == "kicked by vote" || reject.reason == "the server closed";
                LeaveToMenu(shown ? reject.reason.c_str() : "the server put us out");
            }
            else if (type == kMsgReject && !mConnected && ReadReject(r, reject))
            {
                printf("net: frame %u, the server won't have us back: %s\n", mFrame, Printable(reject.reason).c_str());
                if (reject.reason != "the server is full")
                {
                    LeaveToMenu(Printable(reject.reason).c_str()); // another build, kicked: no use trying again
                    return;
                }
                mTransport.reset(); // and try again: it may be full only for now
            }
        }
        else if (e.type == TransportEvent::kConnect && !mConnected && mTransport)
        {
            Hello hello;
            hello.build = mConfig.build;
            hello.name = mConfig.name;
            hello.carInfo = engine::DataFileCrc("nyc.gci");
            hello.session = mSession; // the server gives us our slot back by them, with its score
            hello.token = mToken;
            BitWriter w;
            WriteHello(w, hello);
            mTransport->Send(0, kChannelReliable, w.Data().data(), w.ByteCount(), true);
            mTransport->Flush();
        }
        else if (e.type == TransportEvent::kDisconnect)
        {
            if (mConnected)
            {
                printf("net: frame %u, lost the server\n", mFrame);
                mReconnects = 0;
                std::fill(mRemote.begin(), mRemote.end(), 0u); // the others stand still meanwhile
                mKnown = ForwardedInputs();
            }
            mConnected = false;
            mTransport.reset();
        }
    }
}

// The server's next map: this game ends with the server's (after the same pause) and the next starts on it
void net::Client::OnMapChange(BitReader& r)
{
    MapChange m;
    if (ReadMapChange(r, m))
    {
        ChangeGame(m.map, m.gmp, m.sty, m.scr, m.print, m.game, m.players, m.slot, 90);
    }
}

bool net::Client::ChangeGame(const std::string& map, const std::string& gmp, const std::string& sty, const std::string& scr, const MapPrint& print,
                             int game, int players, int slot, int pauseTicks)
{
    std::string missing;
    if (players <= slot)
    {
        missing = "the server's next game has " + std::to_string(players) + " players, not enough for us (player " + std::to_string(slot) + ")";
    }
    if (!missing.empty() || !PlainNames(BadMapName(map, gmp, sty, scr), "the server", &missing) || !HaveMap(gmp, sty, scr, &missing) || !SameMap(map, gmp, sty, scr, print, &missing) ||
        !PlayableMap(map, gmp, &missing))
    {
        printf("net: frame %u, %s\n", mFrame, missing.c_str());
        SendLeave(missing);
        LeaveToMenu(missing.c_str());
        return false;
    }
    printf("net: frame %u, the next map: %s%s\n", mFrame, scr.c_str(), slot != mSlot ? (", as player " + std::to_string(slot)).c_str() : "");
    engine::ChangeMap(gmp, sty, scr, players, slot, pauseTicks);
    mNextSlot = slot;
    mScr = scr;
    mServerGame = game; // from now on: the old game's snapshots would put its things into the next map's
    return true;
}

void net::Client::StartOver()
{
    mGeneration = engine::GameGeneration();
    mSlot = mNextSlot;
    mSynced = false;
    mNewSnapshot = false;
    mTakenTick = 0;
    for (Snapshot& s : mReceived)
    {
        s = Snapshot();
    }
    for (Parts& p : mParts)
    {
        p = Parts();
    }
    mRemote.assign(engine::PlayerCount(), 0); // the new game's players
    mRemoteSeen.assign(engine::PlayerCount(), 0);
    mKnown = ForwardedInputs();
    mKnownTick = 0;
    mBot.reset(); // and its map of the old one
    mNavMap.reset();
    printf("net: frame %u, a new game\n", mFrame);
}

// Not running ahead of the server (yet, or any more), or far off: jump to where we should be
bool net::Client::Saved(uint32_t tick) const
{
    const engine::SavedWorld* saved = tick <= mTick && tick + kSaves > mTick ? mSaves[tick % kSaves].get() : NULL;
    return saved && saved->valid && saved->tick == tick && engine::Restorable(*saved);
}

// Our commands came late: the ticks they lack, as live ticks with the input held now (sent as commands for them), each
// saved, as the frame loop runs a few steps at once after a stall
void net::Client::FastForward(int ticks)
{
    const uint32_t local = mHist[mTick % kRing].tick == mTick ? mHist[mTick % kRing].buttons : 0;
    std::vector<uint32_t> buttons(mRemote.size());
    for (int i = 0; i < ticks; i++)
    {
        for (size_t p = 0; p < buttons.size(); p++)
        {
            buttons[p] = (int)p == mSlot ? local : RemoteInput((int)p, mTick + 1); // as a live tick would have them
        }
        SetNextInputs(local);
        engine::ResimTick(buttons.data(), (int)buttons.size());
        RecordTick();
    }
    mSlackKnown = false; // until a snapshot answers commands for the new ticks
    mJumpTick = mTick;
    mStats.fastForwards++;
    printf("net: frame %u, our commands came late: %d ticks run at once\n", mFrame, ticks);
}

void net::Client::Resync(const Snapshot& snap)
{
    const int off = mSynced && mSlackKnown ? mSlack - 2 : 0;
    const uint32_t before = mTick;
    const bool back = mSynced && mLatestTick <= mTick && off > 0;
    mTick = !mSynced || mLatestTick > mTick ? mLatestTick + Lead() : mTick - off;
    if (mSynced)
    {
        mStats.jumps++;
        printf("net: frame %u, a jump to tick %u (our commands %d ticks off two early)\n", mFrame, mTick, off);
    }
    mSynced = true;
    mSlackKnown = false; // until a snapshot answers commands for the new ticks
    // Back: the server keeps the newest command it had, ours from before the jump, and its slack would have us jump
    // back again: known once our commands go past those
    mJumpTick = back ? before + 1 : mTick;
    mTakenAt = mTick;
    for (size_t i = 0; i < mSaves.size(); i++)
    {
        mSaves[i]->valid = false;
    }
    engine::Apply(snap);
    engine::ApplyScores(snap);
}

// When time is short, on the HUD (who won is the server's notice)
void net::Client::AnnounceRound()
{
    const engine::Round& now = engine::CurrentRound();
    std::string line;
    if (now.number == mRound.number && now.secondsLeft < mRound.secondsLeft && (now.secondsLeft == 60 || now.secondsLeft == 10))
    {
        line = std::to_string(now.secondsLeft) + " seconds left";
    }
    if (!line.empty())
    {
        engine::ShowHudMessage(line.c_str());
        printf("net: frame %u, %s\n", mFrame, line.c_str());
    }
    mRound = now;
}

void net::Client::TakeSnapshot()
{
    const Snapshot& snap = mReceived[mLatestTick % kRing];
    const int game = engine::SnapshotGame(snap);
    if (mServerGame < 0)
    {
        mServerGame = game;
    }
    if (game != mServerGame || engine::MapChangePending())
    {
        mNewSnapshot = false; // the old map's (the server's pause, or this game ending): not for this one
        return;
    }
    // How far our commands are from two ticks early. ENet's round trip doesn't see a link conditioner, and the server's
    // word is better anyway: it says which tick's command it had last. Late by a few ticks: the snapshot reconciles on
    // its save and the ticks the commands lack are run at once, so the saves go on; else a jump (Resync), which loses
    // every save and with them the prediction until snapshots of the new ticks come. Far ahead is paced back slowly
    const int off = mSynced && mSlackKnown ? mSlack - 2 : 0;
    mTakenAt = mTick; // before the resimulation: its ticks have this snapshot's news
    const bool behind = mSynced && mLatestTick <= mTick && off < -3;
    if (!mProbe.Alone() && behind && -off <= kMaxFastForward && Saved(mLatestTick))
    {
        Reconcile(snap);
        FastForward(-off);
    }
    else if (!mProbe.Alone() && (!mSynced || mLatestTick > mTick || behind || off > kMaxAhead))
    {
        Resync(snap);
    }
    else
    {
        Reconcile(snap);
    }
    mTakenTick = mLatestTick;
    mNewSnapshot = false;
    if (mFrame % 300 == 0 || mFrame < 3)
    {
        PrintStats(snap);
    }
}

void net::Client::PrintStats(const Snapshot& snap)
{
    mProbe.PrintStats(mFrame);
    int cars = 0, peds = 0;
    float drift = 0;
    engine::ReplicaStats(snap, &cars, &peds, &drift);
    const Stats& st = mStats;
    printf("net: frame %u, tick %u (server %u, our commands %d ticks early), %zu entities (%d cars, %d peds), %u rollbacks, "
           "%.1f ticks resimulated each, prediction error up to %.2f blocks, cars and peds shown %.2f blocks off (up to %.2f)\n",
           mFrame, mTick, mLatestTick, mSlack, snap.entities.size(), cars, peds, st.rollbacks, st.rollbacks ? (float)st.resimTicks / st.rollbacks : 0.0f,
           st.maxError, st.shownErrorCount ? (float)(st.shownErrorSum / st.shownErrorCount) : 0.0f, st.shownErrorMax);
    printf("net: frame %u, players shown off the server's: the others %.3f blocks on average (up to %.2f), ourselves %.3f (up to %.2f)\n",
           mFrame, st.others.Mean(), st.others.max, st.self.Mean(), st.self.max);
    if (st.rollbacks)
    {
        printf("net: frame %u, a rollback takes %.2f ms on average (worst %.2f): restore and apply %.2f, %.1f ticks resimulated %.2f, "
               "their saves %.2f\n",
               mFrame, (st.restoreUs + st.resimUs + st.saveUs) / st.rollbacks / 1000, st.reconcileWorstUs / 1000, st.restoreUs / st.rollbacks / 1000,
               (float)st.resimTicks / st.rollbacks, st.resimUs / st.rollbacks / 1000, st.saveUs / st.rollbacks / 1000);
    }
    if (st.jumps || st.fastForwards || st.plainApplies || st.staleRestores)
    {
        printf("net: frame %u, %u jumps, %u catch-ups at once, %u snapshots taken without their save, %u on a save from before a correction\n",
               mFrame, st.jumps, st.fastForwards, st.plainApplies, st.staleRestores);
    }
    std::string local;
    for (size_t i = 0; i < mLocalDeaths.size(); i++)
    {
        local += " " + std::to_string(mLocalDeaths[i]);
    }
    printf("net: frame %u, frags/deaths(health)%s; deaths shown here:%s\n", mFrame, Scoreboard(snap).c_str(), local.c_str());
    if (!mReplay) // a replay has no network
    {
        printf("net: frame %u, ping %u ms (ENet's %u)\n", mFrame, mPingMs, mRtt);
    }
    int poolCars = 0, poolPeds = 0, poolChars = 0, sent = 0, driven = 0, same = 0, traffic = 0, trafficHere = 0;
    engine::PrintReplicaLog();
    engine::PoolUse(&poolCars, &poolPeds, &poolChars);
    engine::DriverStats(snap, &sent, &driven, &same);
    engine::TrafficDriverStats(snap, &traffic, &trafficHere);
    printf("net: frame %u, drivers: %d peds in the snapshot, %d of their cars driven here, %d by the same ped; %d traffic cars, %d "
           "with the traffic driver here\n",
           mFrame, sent, driven, same, traffic, trafficHere);
    printf("net: frame %u, this game has %d cars, %d peds, %d chars; in or out of a car differently from the newest snapshot: "
           "%.1f%% of player frames\n",
           mFrame, poolCars, poolPeds, poolChars, st.carFrames ? 100.0 * st.carMismatch / st.carFrames : 0.0);
    if (mBot)
    {
        printf("net: frame %u, bot %s\n", mFrame, mBot->Describe().c_str());
    }
    st.players.Print(mFrame, "players");
    st.cars.Print(mFrame, "cars");
    st.carsView[0].Print(mFrame, "cars in view");
    st.carsView[1].Print(mFrame, "cars beyond the view");
    for (int v = 0; v < 2; v++)
    {
        printf("net: frame %u, cars and peds %s shown %.3f blocks off (up to %.2f, %u compared)\n", mFrame, v ? "beyond the view" : "in view",
               st.shownViewCount[v] ? (float)(st.shownViewSum[v] / st.shownViewCount[v]) : 0.0f, st.shownViewMax[v], st.shownViewCount[v]);
    }
    mStats = Stats();
}

// Keep our commands about two ticks early: a little faster when they are late, slower when far ahead
int net::Client::PacedTickMs() const
{
    if (!mSynced || !mSlackKnown)
    {
        return mTickMs;
    }
    // Late: a quarter faster; far ahead: a fifth slower, before it comes to a jump (kMaxAhead)
    return mSlack < 1 ? mTickMs * 3 / 4 : mSlack - 2 > 12 ? mTickMs + mTickMs / 5 : mSlack > 4 ? mTickMs + mTickMs / 10 : mTickMs;
}

// The next tick's inputs: ours (predicted and sent), the others' last known
void net::Client::SetNextInputs(uint32_t local)
{
    const uint32_t next = mTick + 1;
    mHist[next % kRing].tick = next;
    mHist[next % kRing].buttons = local;
    mHist[next % kRing].typed = engine::TypedChar(local);
    for (size_t i = 0; i < mRemote.size(); i++)
    {
        engine::SetPlayerInput((int)i, (int)i == mSlot ? local : RemoteInput((int)i, next));
    }
}

// The commands for the last ticks, consecutive, up to the next one
void net::Client::SendCommands()
{
    const uint32_t next = mTick + 1;
    InputCmd cmds[kMaxCmdsPerPacket];
    int n = 0;
    for (uint32_t t = next >= (uint32_t)kMaxCmdsPerPacket ? next - kMaxCmdsPerPacket + 1 : 1; t <= next; t++)
    {
        if (mHist[t % kRing].tick == t)
        {
            cmds[n].tick = t;
            cmds[n].buttons = mHist[t % kRing].buttons;
            cmds[n].typed = mHist[t % kRing].typed;
            n++;
        }
        else
        {
            n = 0; // only consecutive ticks
        }
    }
    if (mTransport)
    {
        BitWriter w;
        // The ack is the newest snapshot this game took, not the newest that came: loading the next map, the client
        // ignores the new game's snapshots, and an ack of them would count it in and start the countdown unseen
        WriteCommands(w, mTakenTick, cmds, n);
        mTransport->Send(0, kChannelUnreliable, w.Data().data(), w.ByteCount(), false);
        mTransport->Flush();
    }
}

void net::Client::Reconnect()
{
    const long long now = NowMs();
    mLostMs = mReconnects ? mLostMs : now;
    if (now - mLostMs > kGiveUpMs)
    {
        printf("net: frame %u, no answer from the server after %d attempts\n", mFrame, mReconnects);
        LeaveToMenu("lost the server");
        return;
    }
    if (mTransport || now < mReconnectMs)
    {
        return; // connecting (the hello follows its kConnect: HandleEvents), or not yet
    }
    mReconnectMs = now + kReconnectMs;
    mReconnects++;
    printf("net: frame %u, connecting to %s:%u again (attempt %d)\n", mFrame, mConfig.host.c_str(), mConfig.port, mReconnects);
    std::string error;
    mTransport.reset(CreateClientTransport(mConfig.host.c_str(), mConfig.port, &error));
    if (mTransport && mConfig.link.Any())
    {
        mTransport.reset(CreateConditioner(mTransport.release(), mConfig.link));
    }
}

// Back in: the same game goes on from the server's next snapshot, if it is the same server's run (a restarted one has
// another game, whatever its map) and it has us as the same player on the same map
void net::Client::Rejoined(const Welcome& welcome)
{
    if (welcome.session != mSession)
    {
        printf("net: frame %u, the server was restarted: its game isn't this one\n", mFrame);
        LeaveToMenu("the server was restarted");
        return;
    }
    // The server's game went on to another (a round's fresh game, another map) while we were away: that one here too,
    // in the slot it has us in (a map change renumbers them)
    const bool otherGame = welcome.scr != mScr || (mServerGame >= 0 && welcome.game != mServerGame);
    if (welcome.version != kProtocolVersion || (!otherGame && welcome.slot != mNextSlot))
    {
        printf("net: frame %u, the server has us back as player %d, not %d: start the game again to play on\n", mFrame, welcome.slot, mNextSlot);
        LeaveToMenu("the server has us back as another player");
        return;
    }
    if (otherGame && !ChangeGame(welcome.map, welcome.gmp, welcome.sty, welcome.scr, welcome.print, welcome.game, welcome.players, welcome.slot, 1))
    {
        return;
    }
    printf("net: frame %u, back in as player %d\n", mFrame, welcome.slot);
    mToken = welcome.token; // a new one if the server had forgotten us
    mConfig.name = welcome.name;
    mConnected = true;
    mSynced = false;
    mNewSnapshot = false;
    mLatestTick = 0;
    mServerGame = welcome.game;
    mLastSnapshotMs = NowMs();
    for (Snapshot& s : mReceived)
    {
        s = Snapshot();
    }
    for (Parts& p : mParts)
    {
        p = Parts();
    }
}

// While a round is played: its clock and goal ("9:42   FIRST TO 10"), a line of its own; empty otherwise
std::string net::Client::RoundCorner() const
{
    const engine::Round& r = engine::CurrentRound();
    if (r.phase != kPhasePlaying || !mConnected || mLeft)
    {
        return "";
    }
    std::string line;
    if (r.secondsLeft > 0)
    {
        char clock[16];
        snprintf(clock, sizeof(clock), "%d:%02d", r.secondsLeft / 60, r.secondsLeft % 60);
        line = clock;
    }
    if (mFragLimit > 0 && mGameType != 3)
    {
        line += (line.empty() ? "" : "   ") + std::string("FIRST TO ") + (mGameType == 2 ? "$" : "") + std::to_string(mFragLimit);
    }
    return line.empty() ? line : line + "\n";
}

void net::Client::ShowStatus()
{
    const long long now = NowMs();
    if (now - mPingShownMs >= 1000) // a number that holds still long enough to read
    {
        mPingShown = mPingMs;
        mPingShownMs = now;
        if (Env().stats)
        {
            // The server sends a snapshot every tick: the ones missing since the last second are lost (or came too late)
            const uint32_t sent = mLink.fromTick && mLatestTick > mLink.fromTick ? mLatestTick - mLink.fromTick : 0;
            const uint32_t lost = sent > mLink.snapshots ? sent - mLink.snapshots : 0;
            char text[64];
            snprintf(text, sizeof(text), "loss %u%%  snap %u/s  ahead %d  resim %.1f", sent ? (lost * 100 + sent / 2) / sent : 0,
                     mLink.snapshots, (int)(mTick - mLatestTick), mLink.rollbacks ? (float)mLink.resimTicks / mLink.rollbacks : 0.0f);
            mLinkShown = std::string(text) + "\n";
            mLink = LinkStats();
            mLink.fromTick = mLatestTick;
        }
    }
    std::string centre;
    if (mLeft)
    {
        centre = "";
    }
    else if (!mConnected)
    {
        centre = "Lost the server, reconnecting";
    }
    else if (mSynced && now - mLastSnapshotMs > kStallMs)
    {
        centre = "Connection interrupted";
    }
    engine::SetNetStatus(RoundCorner() + (mConnected && mPingShown ? mLinkShown + "ping " + std::to_string(mPingShown) : ""), centre);
}

void net::Client::SendLeave(const std::string& why)
{
    if (mTransport)
    {
        BitWriter w;
        WriteText(w, kMsgLeave, why);
        mTransport->Send(0, kChannelReliable, w.Data().data(), w.ByteCount(), true);
        mTransport->Flush();
        // A disconnect after it, not at once: the server drops what came with an abrupt one (enet_peer_reset_queues)
        mTransport->Disconnect(0);
        TransportEvent e;
        for (const long long until = NowMs() + 500; NowMs() < until && !(mTransport->Poll(e) && e.type == TransportEvent::kDisconnect);)
        {
            mTransport->Flush();
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    }
}

void net::Client::Leave()
{
    mTransport.reset(); // its destructor disconnects (enet_peer_disconnect_now)
}

void net::Client::LeaveToMenu(const char* why)
{
    mLeft = true; // no reconnecting
    mConnected = false;
    mLeftWhy = why;
    Leave();
    engine::LeaveForMenu(); // and the frontend's network page (port/net/net_main.cpp, LeaveNetworkGame)
}

void net::Client::Frame()
{
    Step();
    engine::GuardPeds(); // again: the snapshot this frame applied comes before the next tick
    engine::AnchorAttachments(); // the frame is drawn next
    mProbe.FrameEnd();
}

void net::Client::Step()
{
    mFrame++;
    engine::KeepMapChange();
    if (engine::GameGeneration() != mGeneration)
    {
        StartOver();
    }
    mProbe.Frame(mFrame, mRemote);
    std::vector<TransportEvent> events;
    uint32_t recordedInput = 0;
    static int sOn = Env().replayOn;
    if (!TakeEvents(events, &recordedInput) && sOn-- <= 0)
    {
        printf("net: the replay ends at frame %u\n", mFrame);
        fflush(stdout);
        _Exit(0); // not exit: its destructors ran under the audio and GL threads still going (freed memory read on Linux)
    }
    const uint32_t local = ChooseInput(recordedInput);
    if (mRecorder)
    {
        mRecorder->Frame(mFrame, mRtt, local, events);
    }
    engine::ClientModeFrame();
    if (mSaves.empty())
    {
        for (int i = 0; i < kSaves; i++)
        {
            mSaves.emplace_back(new engine::SavedWorld());
        }
        mStale.assign(kSaves, false);
        mRemote.assign(engine::PlayerCount(), 0);
        mRemoteSeen.assign(engine::PlayerCount(), 0);
    }
    CountTestStats();
    mProbe.AfterTick();
    if (mSynced)
    {
        RecordTick();
    }
    HandleEvents(events);
    if (!mReplay)
    {
        ShowStatus();
        ShowRound();
        WatchChat();
        KeepChatLines();
        mProbe.Say(mFrame, mConnected, [this](const std::string& command) { SendCommand(command); });
        BotRound();
    }
    if (!mConnected)
    {
        if (!mReplay && !mLeft)
        {
            Reconnect();
        }
        SetNextInputs(local); // the game goes on here meanwhile: ours, the others' standing still
        return;
    }
    if (mNewSnapshot)
    {
        TakeSnapshot();
        AnnounceRound();
    }
    if (mSynced)
    {
        RecordShown();
    }
    mProbe.Jerk(mFrame, mSlot, mSynced, mReceived[mLatestTick % kRing], mReceived[(mLatestTick - 1) % kRing]);
    engine::SetTickMs(mReplay ? 1 : PacedTickMs()); // every frame: a restore brings back the game's tick length too (a replay: no waiting)
    SetNextInputs(local);
    if (mSynced)
    {
        SendCommands();
    }
    if (!mReplay)
    {
        SendPing();
    }
}

// --- The round flow, a client's part (port/docs/netcode.md, "Playing") ---

void net::Client::SendCommand(const std::string& text)
{
    if (mReplay || !mTransport || !mConnected)
    {
        return;
    }
    BitWriter w;
    WriteText(w, kMsgCommand, text);
    mTransport->Send(0, kChannelReliable, w.Data().data(), w.ByteCount(), true);
    printf("net: frame %u, command %s\n", mFrame, text.c_str());
}

// F1 in the warmup (ready), else while a vote is open (yes); Y or N, as the keyboard's layout types them, while a vote
// is open; in the results, the number keys, or Up, Down and Enter, choose the next map. A gamepad's Back is F1, its
// d-pad and A in the results the arrows and Enter (SetRoundPad). Not while typing a chat line, whose keys they are
// then. The keys go to neither the game here nor the server
void net::Client::RoundKeys(uint32_t& local, uint32_t key)
{
    const uint32_t kKeyDown = 0x200000, kKeyBits = 0x1FF000, kF1 = 0x3B; // DIK_F1
    const uint32_t kUp = 0xC8, kDown = 0xD0, kEnter = 0x1C, kPadEnter = 0x9C; // DIK_UP, DIK_DOWN, DIK_RETURN, DIK_NUMPADENTER
    const engine::Round& round = engine::CurrentRound();
    const int typed = tolower(engine::TypedChar(local) & 0xFF);
    const bool voteOpen = round.voteKind != kVoteNone && !(mSlot < kRoundSlots && ((round.voteYes | round.voteNo) >> mSlot & 1));
    const bool unready = round.phase == kPhaseWarmup && round.minPlayers && !(mSlot < kRoundSlots && (round.ready >> mSlot & 1));
    const bool ready = key == kF1 && (unready || (round.phase == kPhaseWarmup && round.minPlayers && !voteOpen));
    const bool yes = round.voteKind != kVoteNone && (typed == 'y' || (key == kF1 && !ready));
    const bool no = round.voteKind != kVoteNone && typed == 'n';
    const bool results = round.phase == kPhaseResults && !mMaps.empty();
    const bool move = results && (key == kUp || key == kDown);
    // In the results, 1-9 chooses the next map (DIK_1 is 2 .. DIK_9 is 10), Enter the one under the cursor
    uint32_t map = key >= 0x02 && key <= 0x0A && results && key - 1 <= mMaps.size() ? key - 1 : 0;
    map = results && (key == kEnter || key == kPadEnter) ? mMapCursor + 1 : map;
    if ((!ready && !yes && !no && !map && !move) || engine::TakeLocalChat().chatting)
    {
        return;
    }
    if (mFrame != mRoundKeyFrame + 1) // held: once
    {
        if (move)
        {
            mMapCursor = (mMapCursor + (key == kDown ? 1 : (uint32_t)mMaps.size() - 1)) % (uint32_t)mMaps.size();
        }
        else
        {
            SendCommand(ready ? "/ready" : map ? "/map " + std::to_string(map) : yes ? "/yes" : "/no");
        }
    }
    mRoundKeyFrame = mFrame;
    local &= ~(kKeyDown | kKeyBits);
}

// Tab in a chat line starting with '/': its last word completed (CompleteCommand), the options over the frame when
// several fit. The key goes to neither the game here nor the server; the others see the line as the server typed it
void net::Client::CompleteChat(uint32_t& local, uint32_t key)
{
    const uint32_t kKeyDown = 0x200000, kKeyBits = 0x1FF000, kTab = 0x0F; // DIK_TAB
    engine::LocalChat chat = engine::TakeLocalChat();
    if (key != kTab || !chat.chatting)
    {
        return;
    }
    local &= ~(kKeyDown | kKeyBits);
    std::string line;
    for (int i = 0; i < chat.length && i < 79; i++)
    {
        line += (char)(chat.text[i] & 0xFF);
    }
    std::vector<std::string> names, options;
    for (int i = 0; i < engine::PlayerCount(); i++)
    {
        if (engine::SlotInGame(i))
        {
            names.push_back(engine::PlayerName(i));
        }
    }
    const std::string done = CompleteCommand(line, mMaps, names, options);
    chat.length = (int)(done.size() < 78 ? done.size() : 78);
    for (int i = 0; i < 79; i++)
    {
        chat.text[i] = i < chat.length ? (short)(unsigned char)done[i] : 0;
    }
    engine::PutLocalChat(chat);
    if (!options.empty())
    {
        std::string list;
        for (const std::string& o : options)
        {
            list += (list.empty() ? "" : ",  ") + o;
        }
        engine::ShowHudMessage(list.c_str());
    }
}

void net::Client::WatchChat()
{
    const engine::LocalChat chat = engine::TakeLocalChat();
    if (mWasChatting && !chat.chatting)
    {
        std::string line;
        for (int i = 0; i < chat.length && i < 79; i++)
        {
            line += (char)(chat.text[i] & 0x7F);
        }
        if (!line.empty() && line[0] == '/')
        {
            SendCommand(line);
        }
    }
    mWasChatting = chat.chatting != 0;
}

// A line ends in a received snapshot (a resimulated one would end it again); commands stay out
void net::Client::KeepChatLines()
{
    if (mLatestTick == mChatTick)
    {
        return;
    }
    mChatTick = mLatestTick;
    uint32_t chatting = 0;
    for (const EntityState& e : mReceived[mLatestTick % kRing].entities)
    {
        int p = 0;
        uint32_t b = 0;
        std::string line;
        if (!engine::PlayerOf(e, &p, &b) || p < 0 || p >= 32)
        {
            continue;
        }
        if (engine::PlayerChat(e, &line))
        {
            chatting |= 1u << p;
        }
        else if ((mChatting >> p & 1) && !line.empty() && line[0] != '/')
        {
            engine::AddChatLine(engine::PlayerName(p) + ": " + line);
        }
    }
    mChatting = chatting;
}

void net::Client::ShowRound()
{
    const engine::Round& r = engine::CurrentRound();
    const Snapshot& snap = mReceived[mLatestTick % kRing];
    std::vector<ScoreRow> rows;
    for (const EntityState& e : snap.entities)
    {
        int p = 0;
        uint32_t b = 0;
        if (engine::PlayerOf(e, &p, &b))
        {
            rows.push_back(ScoreRow{p, engine::PlayerFrags(e), engine::PlayerDeaths(e), engine::TagSeconds(p), engine::PlayerPoints(e)});
        }
    }
    // The results show the scores the round ended with: the game goes on meanwhile, and so do the frags
    if (r.phase == kPhaseResults && mResultsRows.empty())
    {
        mResultsRows = rows;
    }
    else if (r.phase != kPhaseResults)
    {
        mResultsRows.clear();
    }
    auto bits = [](unsigned v) {
        int n = 0;
        for (; v; v &= v - 1)
        {
            n++;
        }
        return n;
    };
    if (r.phase != kPhaseResults)
    {
        mMapCursor = 0;
    }
    engine::SetRoundPad(r.phase == kPhaseResults && !mMaps.empty());
    const bool pad = engine::PadMode();
    std::string banner;
    const bool hidden = !mConnected || mLeft || engine::MenuOpen(); // the pause menu alone over the frame
    if (r.phase == kPhaseCountdown && !hidden)
    {
        // Large, in the middle; 0: the server waits for everyone to load the map
        banner = r.phaseLeft ? "Round " + std::to_string(r.number) + " in " + std::to_string(r.phaseLeft) : std::string("Waiting for players");
    }
    engine::SetNetBanner(banner);
    std::vector<std::string> lines;
    if (hidden)
    {
        // nothing: the connection's own lines show
    }
    else if (r.phase == kPhaseWarmup)
    {
        int ready = 0;
        for (const ScoreRow& row : rows)
        {
            ready += row.slot < kRoundSlots && (r.ready >> row.slot & 1);
        }
        lines.push_back("Warmup");
        if ((int)rows.size() < r.minPlayers)
        {
            lines.push_back("Waiting for players: " + std::to_string(rows.size()) + " of " + std::to_string(r.minPlayers));
        }
        else
        {
            lines.push_back(std::to_string(ready) + " of " + std::to_string(rows.size()) + " ready" +
                            (r.phaseLeft ? ", the round starts in " + std::to_string(r.phaseLeft) + " s anyway" : ""));
        }
        if (mSlot < kRoundSlots && (r.ready >> mSlot & 1))
        {
            lines.push_back("You are ready");
        }
        lines.push_back(pad ? "Back ready   Start menu" : "F1 ready   F10 chat   Esc menu");
    }
    else if (r.phase == kPhaseResults)
    {
        lines.push_back("Round " + std::to_string(r.number - 1) + ": " + (r.winner ? engine::PlayerName(r.winner - 1) + " wins" : std::string("a draw")));
        std::vector<ScoreRow> board = mResultsRows;
        const bool tag = mGameType == 3; // the longest "it" wins
        const bool points = mGameType == 2; // the most money wins
        std::sort(board.begin(), board.end(), [tag, points](const ScoreRow& a, const ScoreRow& b) {
            return tag ? a.tag > b.tag : points ? a.points > b.points : a.frags > b.frags;
        });
        lines.push_back("");
        // tables: the overlay lines up rows with tabs
        lines.push_back(tag ? "Player\tIt (s)\tFrags\tDeaths" : points ? "Player\tPoints\tFrags\tDeaths" : "Player\tFrags\tDeaths");
        for (const ScoreRow& row : board)
        {
            const std::string first = tag ? std::to_string(row.tag) + "\t" : points ? std::to_string(row.points) + "\t" : "";
            lines.push_back(engine::PlayerName(row.slot) + "\t" + first + std::to_string(row.frags) + "\t" + std::to_string(row.deaths));
        }
        lines.push_back("");
        lines.push_back("The next round in " + std::to_string(r.phaseLeft) + " s");
        lines.push_back("");
        lines.push_back(pad ? "\tNext map (d-pad and A)\tVotes\t" : "\tNext map (its number, or arrows and Enter)\tVotes\t");
        for (size_t m = 0; m < mMaps.size(); m++)
        {
            int votes = 0;
            for (int i = 0; i < kRoundSlots; i++)
            {
                votes += r.mapChoice[i] == m + 1;
            }
            const bool mine = mSlot < kRoundSlots && r.mapChoice[mSlot] == m + 1;
            const std::string title = mMapTitles[m] + (mMapPlayers[m] ? " " + std::to_string(mMapPlayers[m]) + "p" : "");
            const std::string name = m == mMapCursor ? "(" + title + ")" : title; // the font has no '>'
            lines.push_back(std::to_string(m + 1) + "\t" + name + "\t" + (votes ? std::to_string(votes) : std::string("-")) + "\t" + (mine ? "you" : ""));
        }
    }
    const std::string what = r.voteKind == kVoteNone    ? std::string()
                             : r.voteKind == kVoteStart ? "start the round"
                             : r.voteKind == kVoteMap   ? "change to " + (r.voteArg < mMapTitles.size() ? mMapTitles[r.voteArg] : std::string("a map"))
                                                        : "kick " + engine::PlayerName(r.voteArg);
    const bool mine = mSlot < kRoundSlots && ((r.voteYes | r.voteNo) >> mSlot & 1);
    if (engine::MenuOpen())
    {
        // The pause menu's PLAYERS, by score (the ping is only ours: the others' isn't sent), its READY and the open vote
        std::vector<engine::MenuPlayer> players;
        for (const ScoreRow& row : rows)
        {
            const int score = mGameType == 3 ? row.tag : mGameType == 2 ? row.points : row.frags;
            const bool you = row.slot == mSlot;
            players.push_back({engine::PlayerName(row.slot), score, row.deaths, you && mConnected && mPingShown ? (int)mPingShown : -1, you});
        }
        std::stable_sort(players.begin(), players.end(), [](const engine::MenuPlayer& a, const engine::MenuPlayer& b) { return a.score > b.score; });
        const bool ready = mSlot < kRoundSlots && (r.ready >> mSlot & 1);
        engine::OfferMenu(players, mGameType == 3 ? "IT" : mGameType == 2 ? "POINTS" : "FRAGS",
                          r.phase == kPhaseWarmup && r.minPlayers ? (ready ? 2 : 1) : 0,
                          r.voteKind != kVoteNone && !mine ? "Vote to " + what : std::string());
    }
    if (r.voteKind != kVoteNone && !hidden)
    {
        lines.push_back("");
        lines.push_back("Vote: " + what + "   " + std::to_string(bits(r.voteYes)) + " yes, " + std::to_string(bits(r.voteNo)) + " no   " +
                        std::to_string(r.voteLeft) + " s" + (mine ? "" : pad ? "   Back: yes" : "   Y / N"));
    }
    engine::SetNetPanel(lines);
}

// A bot's map in the results: one of the others, picked by its slot and the frame (a vote among bots isn't always the
// same map); map 2 for a test (GTA2_NET_BOTMAP2)
uint32_t net::Client::BotMapChoice() const
{
    if (Env().botVoteMap2 || mMaps.size() < 2)
    {
        return mMaps.size() > 1 ? 2 : 1;
    }
    auto lower = [](std::string t) {
        std::transform(t.begin(), t.end(), t.begin(), [](unsigned char c) { return (char)tolower(c); });
        return t;
    };
    const std::string now = lower(mScr.substr(0, mScr.rfind('.')));
    std::vector<uint32_t> others;
    for (size_t m = 0; m < mMaps.size(); m++)
    {
        if (lower(mMaps[m]) != now)
        {
            others.push_back((uint32_t)m + 1);
        }
    }
    return others.empty() ? 1 : others[(mSlot * 7 + mFrame) % others.size()];
}

void net::Client::BotRound()
{
    // Not synced: the round is the last game's still, its flags by the slots before a map change renumbered them
    if (!mBot || !mConnected || !mSynced || mFrame < mBotSentFrame + 90)
    {
        return;
    }
    const engine::Round& r = engine::CurrentRound();
    const bool ready = mSlot < kRoundSlots && (r.ready >> mSlot & 1);
    if (r.phase == kPhaseWarmup && r.minPlayers && !ready)
    {
        SendCommand("/ready");
    }
    else if (r.phase == kPhaseResults && !mMaps.empty() && mSlot < kRoundSlots && !r.mapChoice[mSlot])
    {
        SendCommand("/map " + std::to_string(BotMapChoice()));
    }
    else if (r.voteKind != kVoteNone && mSlot < kRoundSlots && !((r.voteYes | r.voteNo) >> mSlot & 1))
    {
        SendCommand(r.voteKind == kVoteKick && r.voteArg == mSlot ? "/no" : "/yes"); // anything but its own kick
    }
    else
    {
        return;
    }
    mBotSentFrame = mFrame;
}

