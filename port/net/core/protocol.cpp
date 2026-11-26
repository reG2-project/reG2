// port/net core: see protocol.hpp.
#include "protocol.hpp"
#include <algorithm>
#include <ctype.h>
#include <string.h>

namespace
{
const size_t kMaxString = 255;

void WriteString(net::BitWriter& w, const std::string& s)
{
    const size_t n = s.size() < kMaxString ? s.size() : kMaxString;
    w.VarU((uint32_t)n);
    w.Bytes(s.data(), n);
}

bool ReadString(net::BitReader& r, std::string& s)
{
    const uint32_t n = r.VarU();
    if (n > kMaxString)
    {
        return false;
    }
    s.resize(n);
    return r.Bytes(&s[0], n);
}
} // namespace

static bool SameName(const std::string& a, const std::string& b)
{
    if (a.size() != b.size())
    {
        return false;
    }
    for (size_t i = 0; i < a.size(); i++)
    {
        if (toupper((unsigned char)a[i]) != toupper((unsigned char)b[i]))
        {
            return false;
        }
    }
    return true;
}

std::string net::PlayerName(const std::string& asked, const std::vector<std::string>& taken)
{
    std::string base;
    for (char c : asked)
    {
        if (c >= ' ' && c <= '~' && base.size() < kMaxName && !(c == ' ' && base.empty()))
        {
            base += c;
        }
    }
    while (!base.empty() && base.back() == ' ')
    {
        base.pop_back();
    }
    base = base.empty() ? "PLAYER" : base;
    for (int n = 1;; n++)
    {
        const std::string suffix = n > 1 ? " " + std::to_string(n) : "";
        const std::string name = base.substr(0, kMaxName - suffix.size()) + suffix;
        bool used = false;
        for (const std::string& t : taken)
        {
            used = used || SameName(t, name);
        }
        if (!used)
        {
            return name;
        }
    }
}

std::string net::Printable(const std::string& s)
{
    std::string out = s;
    for (char& c : out)
    {
        c = c >= ' ' && c <= '~' ? c : '?';
    }
    return out;
}

bool net::BareFileName(const std::string& name, const char* ext)
{
    const size_t extLen = strlen(ext);
    if (name.size() <= extLen || name.size() > kMaxFileName + extLen)
    {
        return false;
    }
    const size_t base = name.size() - extLen;
    for (size_t i = 0; i < base; i++)
    {
        const char c = name[i];
        if (!isalnum((unsigned char)c) && c != '_' && c != '-')
        {
            return false;
        }
    }
    for (size_t i = 0; i < extLen; i++)
    {
        if (tolower((unsigned char)name[base + i]) != tolower((unsigned char)ext[i]))
        {
            return false;
        }
    }
    return true;
}

std::string net::BadMapName(const std::string& map, const std::string& gmp, const std::string& sty, const std::string& scr)
{
    return !BareFileName(map, "")      ? "map"
           : !BareFileName(gmp, ".gmp") ? "gmp"
           : !BareFileName(sty, ".sty") ? "sty"
           : !BareFileName(scr, ".scr") ? "scr"
                                        : "";
}

std::string net::BadMapName(const Welcome& m)
{
    const std::string bad = BadMapName(m.map, m.gmp, m.sty, m.scr);
    if (!bad.empty())
    {
        return bad;
    }
    for (const std::string& name : m.maps)
    {
        if (!BareFileName(name, ""))
        {
            return "vote map";
        }
    }
    return "";
}

void net::WriteHello(BitWriter& w, const Hello& m)
{
    w.Bits(kMsgHello, 8);
    w.VarU(m.version);
    WriteString(w, m.build);
    WriteString(w, m.name);
    w.Bits(m.carInfo, 32);
    w.Bits(m.session, 32);
    w.Bits(m.token, 32);
}

bool net::ReadHello(BitReader& r, Hello& m)
{
    m.version = r.VarU();
    const bool ok = ReadString(r, m.build) && ReadString(r, m.name);
    m.carInfo = r.Bits(32);
    if (r.BitsLeft() >= 64) // an older build's hello ends here: still read, so the refusal can name its build
    {
        m.session = r.Bits(32);
        m.token = r.Bits(32);
    }
    return ok && !r.Overflow();
}

static void WritePrint(net::BitWriter& w, const net::MapPrint& p)
{
    w.Bits(p.gmp, 32);
    w.Bits(p.sty, 32);
    w.Bits(p.scr, 32);
}

static void ReadPrint(net::BitReader& r, net::MapPrint& p)
{
    p.gmp = r.Bits(32);
    p.sty = r.Bits(32);
    p.scr = r.Bits(32);
}

void net::WriteWelcome(BitWriter& w, const Welcome& m)
{
    w.Bits(kMsgWelcome, 8);
    w.VarU(m.version);
    w.Bits(m.slot, 8);
    w.Bits(m.players, 8);
    w.Bits(m.gameType, 8);
    w.VarU(m.fragLimit);
    w.VarU(m.timeLimitMin);
    w.VarU(m.tickMs);
    w.Bits(m.police, 8);
    w.Bits(m.session, 32);
    WriteString(w, m.name);
    WriteString(w, m.map);
    WriteString(w, m.gmp);
    WriteString(w, m.sty);
    WriteString(w, m.scr);
    WritePrint(w, m.print);
    w.VarU(m.serverTick);
    w.Bits((uint32_t)(m.maps.size() < 15 ? m.maps.size() : 15), 4);
    for (size_t i = 0; i < m.maps.size() && i < 15; i++)
    {
        WriteString(w, m.maps[i]);
    }
    w.Bits(m.token, 32);
    w.Bits(m.game, 16);
}

bool net::ReadWelcome(BitReader& r, Welcome& m)
{
    m.version = r.VarU();
    m.slot = (uint8_t)r.Bits(8);
    m.players = (uint8_t)r.Bits(8);
    m.gameType = (uint8_t)r.Bits(8);
    m.fragLimit = r.VarU();
    m.timeLimitMin = (uint16_t)r.VarU();
    m.tickMs = (uint16_t)r.VarU();
    m.police = (uint8_t)r.Bits(8);
    m.session = r.Bits(32);
    bool ok = ReadString(r, m.name) && ReadString(r, m.map) && ReadString(r, m.gmp) && ReadString(r, m.sty) && ReadString(r, m.scr);
    ReadPrint(r, m.print);
    m.serverTick = r.VarU();
    m.maps.resize(r.Bits(4));
    for (size_t i = 0; ok && i < m.maps.size(); i++)
    {
        ok = ReadString(r, m.maps[i]);
    }
    m.token = r.Bits(32);
    m.game = (uint16_t)r.Bits(16);
    return ok && !r.Overflow() && m.slot < m.players && m.players <= kMaxPlayers;
}

void net::WriteReject(BitWriter& w, const Reject& m)
{
    w.Bits(kMsgReject, 8);
    WriteString(w, m.reason);
}

bool net::ReadReject(BitReader& r, Reject& m)
{
    return ReadString(r, m.reason) && !r.Overflow();
}

void net::WriteText(BitWriter& w, MsgType type, const std::string& text)
{
    w.Bits(type, 8);
    WriteString(w, text.size() > 120 ? text.substr(0, 120) : text);
}

bool net::ReadText(BitReader& r, std::string& text)
{
    return ReadString(r, text) && text.size() <= 120 && !r.Overflow();
}

void net::WritePings(BitWriter& w, const Pings& m)
{
    w.Bits(kMsgPings, 8);
    for (int i = 0; i < kMaxPlayers; i++)
    {
        const uint16_t ms = m.ms[i];
        w.Bits(ms != kPingNone, 1);
        if (ms != kPingNone)
        {
            w.Bits(ms == kPingBot, 1);
            if (ms != kPingBot)
            {
                w.Bits(std::min(ms, kPingMax), 12);
            }
        }
    }
}

bool net::ReadPings(BitReader& r, Pings& m)
{
    for (int i = 0; i < kMaxPlayers; i++)
    {
        m.ms[i] = !r.Bits(1) ? kPingNone : r.Bits(1) ? kPingBot : (uint16_t)r.Bits(12);
    }
    return !r.Overflow();
}

void net::WriteMapChange(BitWriter& w, const MapChange& m)
{
    w.Bits(kMsgMapChange, 8);
    WriteString(w, m.map);
    WriteString(w, m.gmp);
    WriteString(w, m.sty);
    WriteString(w, m.scr);
    WritePrint(w, m.print);
    w.Bits(m.game, 16);
    w.Bits(m.players, BitsFor(kMaxPlayers));
    w.Bits(m.slot, BitsFor(kMaxPlayers));
}

bool net::ReadMapChange(BitReader& r, MapChange& m)
{
    const bool ok = ReadString(r, m.map) && ReadString(r, m.gmp) && ReadString(r, m.sty) && ReadString(r, m.scr);
    ReadPrint(r, m.print);
    m.game = (uint16_t)r.Bits(16);
    m.players = (uint8_t)r.Bits(BitsFor(kMaxPlayers));
    m.slot = (uint8_t)r.Bits(BitsFor(kMaxPlayers));
    return ok && !r.Overflow() && m.players >= 1 && m.players <= kMaxPlayers;
}

void net::WriteCommands(BitWriter& w, uint32_t ackTick, const InputCmd* cmds, int count)
{
    w.Bits(kMsgCommands, 8);
    w.VarU(ackTick);
    w.Bits((uint32_t)count, 4);
    if (!count)
    {
        return;
    }
    w.VarU(cmds[count - 1].tick); // the newest; the others are the ticks before it
    for (int i = 0; i < count; i++)
    {
        const bool same = i && cmds[i].buttons == cmds[i - 1].buttons;
        w.Bool(same);
        if (!same)
        {
            w.Bits(cmds[i].buttons, 22);
        }
        if (cmds[i].buttons & kKeyDownBit)
        {
            w.Bits(cmds[i].typed, 16);
        }
    }
}

int net::ReadCommands(BitReader& r, uint32_t* pAckTick, InputCmd* cmds, int max)
{
    *pAckTick = r.VarU();
    const int count = (int)r.Bits(4);
    if (count > max || count > kMaxCmdsPerPacket)
    {
        return -1;
    }
    if (!count)
    {
        return r.Overflow() ? -1 : 0;
    }
    const uint32_t last = r.VarU();
    if (last < (uint32_t)count)
    {
        return -1;
    }
    for (int i = 0; i < count; i++)
    {
        cmds[i].tick = last - (uint32_t)(count - 1 - i);
        cmds[i].buttons = r.Bool() && i ? cmds[i - 1].buttons : r.Bits(22);
        cmds[i].typed = cmds[i].buttons & kKeyDownBit ? (uint16_t)r.Bits(16) : 0;
    }
    return r.Overflow() ? -1 : count;
}

void net::WriteSnapshotHeader(BitWriter& w, const SnapshotHeader& h)
{
    w.Bits(kMsgSnapshot, 8);
    w.VarU(h.tick);
    w.VarU(h.tick - h.baseTick); // 0..: how far back the baseline is (0: none)
    w.VarU(h.lastCmdTick);
    // Per slot with any: the slot, how many, and each one as the one before (1 bit) or its 12 bits
    int slots = 0;
    for (int i = 0; i < kMaxForwardSlots; i++)
    {
        slots += h.forwarded.count[i] > 0;
    }
    w.Bits((uint32_t)slots, BitsFor(kMaxForwardSlots));
    for (int i = 0; i < kMaxForwardSlots; i++)
    {
        const int n = h.forwarded.count[i] > kMaxForwarded ? kMaxForwarded : h.forwarded.count[i];
        if (!n)
        {
            continue;
        }
        w.Bits((uint32_t)i, BitsFor(kMaxForwardSlots - 1));
        w.Bits((uint32_t)(n - 1), 2);
        for (int k = 0; k < n; k++)
        {
            const uint32_t c = h.forwarded.controls[i][k] & 0xFFF;
            const bool same = k > 0 && c == (h.forwarded.controls[i][k - 1] & 0xFFFu);
            w.Bits(same ? 1 : 0, 1);
            if (!same)
            {
                w.Bits(c, 12);
            }
        }
    }
}

bool net::ReadSnapshotHeader(BitReader& r, SnapshotHeader& h)
{
    h.tick = r.VarU();
    const uint32_t back = r.VarU();
    h.lastCmdTick = r.VarU();
    h.forwarded = ForwardedInputs();
    const uint32_t slots = r.Bits(BitsFor(kMaxForwardSlots));
    for (uint32_t j = 0; j < slots && !r.Overflow(); j++)
    {
        const uint32_t i = r.Bits(BitsFor(kMaxForwardSlots - 1));
        if (i >= (uint32_t)kMaxForwardSlots)
        {
            return false;
        }
        const int n = (int)r.Bits(2) + 1;
        for (int k = 0; k < n; k++)
        {
            h.forwarded.controls[i][k] = (uint16_t)(r.Bits(1) && k > 0 ? h.forwarded.controls[i][k - 1] : r.Bits(12));
        }
        h.forwarded.count[i] = (uint8_t)n;
    }
    if (r.Overflow() || back > h.tick)
    {
        return false;
    }
    h.baseTick = h.tick - back;
    return true;
}

namespace
{
int gLinkMtu = 1280;
} // namespace

void net::SetLinkMtu(int mtu)
{
    gLinkMtu = mtu < 576 ? 576 : mtu > 1428 ? 1428 : mtu;
}

int net::LinkMtu()
{
    return gLinkMtu;
}

size_t net::MaxPayload()
{
    return (size_t)gLinkMtu - 56;
}

void net::WriteSnapshotParts(const std::vector<uint8_t>& message, uint32_t tick, std::vector<std::vector<uint8_t>>& parts)
{
    parts.clear();
    const size_t piece = MaxPayload() - kPartHeader;
    const int count = (int)((message.size() + piece - 1) / piece);
    for (int i = 0; i < count; i++)
    {
        BitWriter w;
        w.Bits(kMsgSnapshotPart, 8);
        w.VarU(tick);
        w.Bits((uint32_t)i, 3);
        w.Bits((uint32_t)(count - 1), 3);
        const size_t at = (size_t)i * piece, n = message.size() - at < piece ? message.size() - at : piece;
        w.Bytes(&message[at], n);
        parts.push_back(w.Data());
        parts.back().resize(w.ByteCount());
    }
}

bool net::ReadSnapshotPart(BitReader& r, uint32_t* pTick, int* pIndex, int* pCount, std::vector<uint8_t>& piece)
{
    *pTick = r.VarU();
    *pIndex = (int)r.Bits(3);
    *pCount = (int)r.Bits(3) + 1;
    if (r.Overflow() || *pIndex >= *pCount)
    {
        return false;
    }
    // The rest, byte-aligned
    const size_t left = r.BitsLeft() / 8;
    piece.resize(left);
    return left > 0 && r.Bytes(piece.data(), left);
}
