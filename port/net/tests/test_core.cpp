// port/net core tests (ctest): no game, no network; a failed CHECK prints and exits 1.
#include "check.hpp"
#include "../core/bitstream.hpp"
#include "../core/crc32.hpp"
#include "../core/map_players.hpp"
#include "../core/protocol.hpp"
#include "../core/snapshot.hpp"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <vector>


namespace
{
unsigned gSeed = 1;
uint32_t Rand()
{
    gSeed = gSeed * 1103515245u + 12345u;
    return (gSeed >> 8) ^ (gSeed << 13);
}

void TestBits()
{
    net::BitWriter w;
    std::vector<uint32_t> values, widths;
    for (int i = 0; i < 2000; i++)
    {
        const uint32_t n = 1 + Rand() % 32;
        const uint32_t v = n == 32 ? Rand() : Rand() & ((1u << n) - 1);
        w.Bits(v, (int)n);
        values.push_back(v);
        widths.push_back(n);
    }
    w.VarU(0);
    w.VarU(127);
    w.VarU(128);
    w.VarU(0xFFFFFFFFu);
    w.VarS(-1);
    w.VarS(-2147483647 - 1);
    w.VarS(12345);
    w.Bool(true);
    const char text[] = "gta2";
    w.Bytes(text, 4);
    net::BitReader r(w.Data().data(), w.ByteCount());
    for (size_t i = 0; i < values.size(); i++)
    {
        CHECK(r.Bits((int)widths[i]) == values[i]);
    }
    CHECK(r.VarU() == 0);
    CHECK(r.VarU() == 127);
    CHECK(r.VarU() == 128);
    CHECK(r.VarU() == 0xFFFFFFFFu);
    CHECK(r.VarS() == -1);
    CHECK(r.VarS() == -2147483647 - 1);
    CHECK(r.VarS() == 12345);
    CHECK(r.Bool());
    char back[4];
    CHECK(r.Bytes(back, 4) && !memcmp(back, text, 4));
    CHECK(!r.Overflow());
    r.Bits(16);
    CHECK(r.Overflow()); // past the end: zeros, and remembered
    CHECK(r.Bits(8) == 0);

    const uint8_t endless[8] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
    net::BitReader bad(endless, sizeof(endless));
    bad.VarU();
    CHECK(bad.Overflow()); // a VarU longer than 5 steps is rejected
}

net::Schema TestSchema()
{
    net::Schema s;
    net::EntityType car = {"car", {{"x", 22, false}, {"y", 22, false}, {"z", 17, false}, {"rot", 11, false}, {"vx", 16, true}, {"model", 7, false}}};
    net::EntityType ped = {"ped", {{"x", 22, false}, {"y", 22, false}, {"health", 8, false}, {"raw", 32, true}}};
    net::EntityType wide = {"wide", {}}; // more fields than a 32-bit mask
    for (int i = 0; i < 110; i++) // more than a 32-bit or 64-bit mask holds
    {
        wide.fields.push_back(net::FieldDesc{"w", (uint8_t)(i % 2 ? 9 : 32), i % 3 == 0});
    }
    s.types.push_back(car);
    s.types.push_back(ped);
    s.types.push_back(wide);
    return s;
}

net::EntityState Entity(uint32_t id, uint8_t type, const net::Schema& s)
{
    net::EntityState e;
    memset(&e, 0, sizeof(e));
    e.id = id;
    e.type = type;
    for (size_t i = 0; i < s.types[type].fields.size(); i++)
    {
        const net::FieldDesc& d = s.types[type].fields[i];
        uint32_t v = Rand();
        if (d.bits < 32)
        {
            v &= (1u << d.bits) - 1;
            if (d.isSigned && (v >> (d.bits - 1)) & 1)
            {
                v |= ~0u << d.bits;
            }
        }
        e.f[i] = (int32_t)v;
    }
    return e;
}

bool Same(const net::Snapshot& a, const net::Snapshot& b)
{
    if (a.entities.size() != b.entities.size())
    {
        return false;
    }
    for (size_t i = 0; i < a.entities.size(); i++)
    {
        if (a.entities[i].id != b.entities[i].id || a.entities[i].type != b.entities[i].type ||
            memcmp(a.entities[i].f, b.entities[i].f, sizeof(a.entities[i].f)))
        {
            return false;
        }
    }
    return true;
}

void RoundTrip(const net::Schema& s, const net::Snapshot& from, const net::Snapshot& to, size_t* pBytes = NULL)
{
    net::BitWriter w;
    net::WriteDelta(w, s, from, to);
    net::BitReader r(w.Data().data(), w.ByteCount());
    net::Snapshot got;
    CHECK(net::ReadDelta(r, s, from, got));
    CHECK(Same(got, to));
    if (pBytes)
    {
        *pBytes = w.ByteCount();
    }
}

void TestDelta()
{
    const net::Schema s = TestSchema();
    net::Snapshot empty, a, b;
    for (uint32_t id = 1; id < 200; id += 1 + Rand() % 3)
    {
        a.entities.push_back(Entity(id, (uint8_t)(Rand() % 3), s));
    }
    RoundTrip(s, empty, a); // everything new
    RoundTrip(s, a, a); // nothing changed
    size_t bytes = 0;
    RoundTrip(s, a, a, &bytes);
    CHECK(bytes <= 2); // an unchanged world costs the entry count only

    // Change a few fields, remove some, add some, and give one id to another type
    b = a;
    b.entities.erase(b.entities.begin() + 3);
    b.entities.erase(b.entities.begin() + 10);
    b.entities[0].f[0] += 5;
    b.entities[5].f[2] ^= 1;
    for (size_t k = 0; k < b.entities.size(); k++)
    {
        if (b.entities[k].type == 2)
        {
            b.entities[k].f[45] ^= 1; // fields in the mask's second and fourth word
            b.entities[k].f[100] ^= 1;
            break;
        }
    }
    b.entities[7] = Entity(b.entities[7].id, (uint8_t)((b.entities[7].type + 1) % 3), s);
    b.entities.push_back(Entity(500, 0, s));
    b.entities.push_back(Entity(501, 1, s));
    b.entities.push_back(Entity(502, 2, s));
    net::SortById(b);
    RoundTrip(s, a, b);
    RoundTrip(s, b, a);
    RoundTrip(s, b, empty); // everything removed

    // Random worlds against each other
    for (int round = 0; round < 200; round++)
    {
        net::Snapshot x, y;
        for (uint32_t id = 0; id < 300; id++)
        {
            if (Rand() % 3 == 0)
            {
                x.entities.push_back(Entity(id, (uint8_t)(Rand() % 3), s));
            }
            if (Rand() % 3 == 0)
            {
                y.entities.push_back(Entity(id, (uint8_t)(Rand() % 3), s));
            }
        }
        RoundTrip(s, x, y);
    }

    // The check's hashes (GTA2_NET_SNAPCHECK): read against the sender's baseline, nothing is wrong; against another
    // one, the entities carried from it are named. Without them, nothing is checked
    {
        net::BitWriter cw;
        net::WriteDelta(cw, s, a, b, true);
        net::BitReader cr(cw.Data().data(), cw.ByteCount());
        net::Snapshot got;
        net::DeltaCheck check;
        CHECK(net::ReadDelta(cr, s, a, got, &check) && Same(got, b) && check.checked && check.bad.empty());
        net::Snapshot other = a;
        other.entities[1].f[0] += 1; // carried unchanged into b
        net::BitReader cr2(cw.Data().data(), cw.ByteCount());
        CHECK(net::ReadDelta(cr2, s, other, got, &check) && check.bad.size() == 1 && got.entities[check.bad[0]].id == a.entities[1].id &&
              check.how[check.bad[0]] == net::DeltaCheck::kCarried);
        net::BitWriter pw;
        net::WriteDelta(pw, s, a, b);
        net::BitReader pr(pw.Data().data(), pw.ByteCount());
        net::DeltaCheck none;
        CHECK(net::ReadDelta(pr, s, a, got, &none) && !none.checked);
    }

    // A truncated delta is rejected, never read past its end
    net::BitWriter w;
    net::WriteDelta(w, s, empty, a);
    for (size_t cut = 0; cut + 1 < w.ByteCount(); cut += 7)
    {
        net::BitReader r(w.Data().data(), cut);
        net::Snapshot got;
        CHECK(!net::ReadDelta(r, s, empty, got));
    }
    // Garbage is rejected or read safely
    for (int round = 0; round < 2000; round++)
    {
        uint8_t junk[64];
        for (size_t i = 0; i < sizeof(junk); i++)
        {
            junk[i] = (uint8_t)Rand();
        }
        net::BitReader r(junk, 1 + Rand() % sizeof(junk));
        net::Snapshot got;
        net::ReadDelta(r, s, a, got);
    }
}

void TestMessages()
{
    net::Welcome w;
    w.slot = 2;
    w.players = 6;
    w.gameType = 3;
    w.fragLimit = 25;
    w.tickMs = 33;
    w.gmp = "wil-multi.gmp";
    w.sty = "wil.sty";
    w.scr = "downtown-6P.scr";
    w.session = 0xDEADBEEF;
    w.name = "PLAYER 2";
    w.map = "downtown-6p";
    w.print.gmp = 0x80000001;
    w.print.sty = 7;
    w.print.scr = 9;
    w.serverTick = 123456;
    w.maps = {"mp1-6p", "mp2-6p", "downtown-6p"};
    w.token = 0xC0FFEE11;
    w.game = 513;
    net::BitWriter bw;
    net::WriteWelcome(bw, w);
    net::BitReader br(bw.Data().data(), bw.ByteCount());
    CHECK(br.Bits(8) == net::kMsgWelcome);
    net::Welcome got;
    CHECK(net::ReadWelcome(br, got));
    CHECK(got.slot == 2 && got.players == 6 && got.gameType == 3 && got.fragLimit == 25 && got.gmp == w.gmp &&
          got.scr == w.scr && got.serverTick == 123456 && got.maps == w.maps && got.session == w.session && got.name == w.name &&
          got.map == w.map && got.print.gmp == w.print.gmp && got.print.sty == 7 && got.print.scr == 9 && got.token == w.token &&
          got.game == 513);

    // The most player slots, the last one this client's; one more is refused
    for (int players : {net::kMaxPlayers, net::kMaxPlayers + 1})
    {
        w.players = (uint8_t)players;
        w.slot = (uint8_t)(net::kMaxPlayers - 1);
        net::BitWriter mw;
        net::WriteWelcome(mw, w);
        net::BitReader mr(mw.Data().data(), mw.ByteCount());
        CHECK(mr.Bits(8) == net::kMsgWelcome);
        CHECK(net::ReadWelcome(mr, got) == (players <= net::kMaxPlayers));
    }

    net::Hello hello;
    hello.build = "0.1.0-dev 1a2b3c4d5e";
    hello.name = "me";
    hello.carInfo = 0xFFFFFFFF;
    hello.session = 0x12345678;
    hello.token = 0x9ABCDEF0;
    net::BitWriter hw2;
    net::WriteHello(hw2, hello);
    net::BitReader hr2(hw2.Data().data(), hw2.ByteCount());
    net::Hello hgot;
    CHECK(hr2.Bits(8) == net::kMsgHello && net::ReadHello(hr2, hgot) && hgot.version == net::kProtocolVersion && hgot.build == hello.build &&
          hgot.name == "me" && hgot.carInfo == 0xFFFFFFFF && hgot.session == hello.session && hgot.token == hello.token);

    // A command and a notice: a line of text each, cut at 120 characters
    for (net::MsgType type : {net::kMsgCommand, net::kMsgNotice})
    {
        net::BitWriter tw;
        net::WriteText(tw, type, "/map 2");
        net::BitReader tr(tw.Data().data(), tw.ByteCount());
        std::string text;
        CHECK(tr.Bits(8) == type && net::ReadText(tr, text) && text == "/map 2");
    }
    net::BitWriter lw;
    net::WriteText(lw, net::kMsgNotice, std::string(200, 'x'));
    net::BitReader lr(lw.Data().data(), lw.ByteCount());
    std::string longText;
    CHECK(lr.Bits(8) == net::kMsgNotice && net::ReadText(lr, longText) && longText.size() == 120);

    net::MapChange mc;
    mc.gmp = "MP1-comp.gmp";
    mc.sty = "wil.sty";
    mc.scr = "MP1-6P.scr";
    mc.map = "mp1-6p";
    mc.print.sty = 12345;
    mc.game = 513;
    mc.players = (uint8_t)net::kMaxPlayers;
    mc.slot = (uint8_t)(net::kMaxPlayers - 1);
    net::BitWriter mw;
    net::WriteMapChange(mw, mc);
    net::BitReader mr(mw.Data().data(), mw.ByteCount());
    CHECK(mr.Bits(8) == net::kMsgMapChange);
    net::MapChange mgot;
    CHECK(net::ReadMapChange(mr, mgot) && mgot.gmp == mc.gmp && mgot.sty == mc.sty && mgot.scr == mc.scr && mgot.game == 513 && mgot.map == mc.map &&
          mgot.print.sty == 12345 && mgot.print.gmp == 0 && mgot.players == net::kMaxPlayers &&
          mgot.slot == net::kMaxPlayers - 1);

    // The last two have a key press: what it typed follows each (0xE9: an AZERTY's é)
    net::InputCmd cmds[5] = {{100, 1, 0}, {101, 1, 0}, {102, 0x10, 0}, {103, 0x3FFFFF, 0xE9}, {104, 0x3FFFFF, 'q'}};
    net::BitWriter cw;
    net::WriteCommands(cw, 98, cmds, 5);
    CHECK(cw.ByteCount() <= 20); // 8 + 8 + 4 + 8 + 3 x 23 + 2 + 2 x 16 bits
    net::BitReader cr(cw.Data().data(), cw.ByteCount());
    CHECK(cr.Bits(8) == net::kMsgCommands);
    uint32_t ack = 0;
    net::InputCmd back[net::kMaxCmdsPerPacket];
    CHECK(net::ReadCommands(cr, &ack, back, net::kMaxCmdsPerPacket) == 5);
    CHECK(ack == 98);
    for (int i = 0; i < 5; i++)
    {
        CHECK(back[i].tick == cmds[i].tick && back[i].buttons == cmds[i].buttons && back[i].typed == cmds[i].typed);
    }

    net::SnapshotHeader h = {5000, 4990, 4998}, hb;
    h.forwarded.count[1] = 3;
    h.forwarded.controls[1][0] = 0x101;
    h.forwarded.controls[1][1] = 0x101;
    h.forwarded.controls[1][2] = 0xFFF;
    h.forwarded.count[5] = 1;
    h.forwarded.controls[5][0] = 0x020;
    net::BitWriter hw;
    net::WriteSnapshotHeader(hw, h);
    net::BitReader hr(hw.Data().data(), hw.ByteCount());
    CHECK(hr.Bits(8) == net::kMsgSnapshot);
    CHECK(net::ReadSnapshotHeader(hr, hb) && hb.tick == 5000 && hb.baseTick == 4990 && hb.lastCmdTick == 4998);
    CHECK(hb.forwarded.count[0] == 0 && hb.forwarded.count[1] == 3 && hb.forwarded.count[5] == 1);
    CHECK(hb.forwarded.controls[1][0] == 0x101 && hb.forwarded.controls[1][1] == 0x101 && hb.forwarded.controls[1][2] == 0xFFF);
    CHECK(hb.forwarded.controls[5][0] == 0x020);

    // Every slot with inputs, the last one too
    net::SnapshotHeader all = {7000, 7000, 6999}, allb;
    for (int i = 0; i < net::kMaxForwardSlots; i++)
    {
        all.forwarded.count[i] = 1;
        all.forwarded.controls[i][0] = (uint16_t)(0x100 + i);
    }
    net::BitWriter aw;
    net::WriteSnapshotHeader(aw, all);
    net::BitReader ar(aw.Data().data(), aw.ByteCount());
    CHECK(ar.Bits(8) == net::kMsgSnapshot && net::ReadSnapshotHeader(ar, allb));
    for (int i = 0; i < net::kMaxForwardSlots; i++)
    {
        CHECK(allb.forwarded.count[i] == 1 && allb.forwarded.controls[i][0] == 0x100 + i);
    }
}

// zlib's CRC-32, in one go or in pieces
void TestCrc()
{
    CHECK(net::Crc32("123456789", 9) == 0xCBF43926u);
    CHECK(net::Crc32("56789", 5, net::Crc32("1234", 4)) == 0xCBF43926u);
    CHECK(net::Crc32("", 0) == 0);
}

// Players' names: printable, 15 characters at most, never the same as another's but for case
void TestNames()
{
    const std::vector<std::string> none;
    CHECK(net::PlayerName("bob", none) == "bob");
    CHECK(net::PlayerName("", none) == "PLAYER");
    CHECK(net::PlayerName("  \t\x01 ", none) == "PLAYER");
    CHECK(net::PlayerName("  a\x7f b  ", none) == "a b");
    CHECK(net::PlayerName("ABCDEFGHIJKLMNOPQRST", none) == "ABCDEFGHIJKLMNO");
    CHECK(net::PlayerName("PLAYER", {"player"}) == "PLAYER 2");
    CHECK(net::PlayerName("", {"PLAYER", "PLAYER 2"}) == "PLAYER 3");
    CHECK(net::PlayerName("ABCDEFGHIJKLMNO", {"abcdefghijklmno"}) == "ABCDEFGHIJKLM 2");
    CHECK(net::PlayerName("bob", {"alice"}) == "bob");
}

// File names from the network: a bare name in data/ with its extension, nothing that could be a path
void TestFileNames()
{
    for (const char* ok : {"wil.sty", "MP1-comp.gmp", "Industrial-6P.SCR", "downtown-6P.scr", "a_b-c.sty", "abcdefghijklmnopqrst.gmp"})
    {
        const char* dot = strrchr(ok, '.');
        CHECK(net::BareFileName(ok, dot));
    }
    CHECK(net::BareFileName("downtown-6p", ""));
    const std::string hostile[] = {
        "", ".sty", "abcdefghijklmnopqrstu.sty", std::string(300, 'a') + ".sty", "../wil.sty", "..\\wil.sty", "a/../wil.sty",
        "audio" + std::string(120, '/') + "../wil.sty", "x\\wil.sty", "/etc/passwd", "C:\\gta2\\data\\wil.sty", "\\\\host\\share\\wil.sty",
        "wil.sty.sty", "wil..sty", "wil sty.sty", "w\x01il.sty", "wil\x1b[2J.sty", std::string("wi\0l.sty", 8), "wil.sty\n", "wil.gmp",
        "wil.st", "wil", "w\xe9l.sty", "~/wil.sty", "wil:x.sty"};
    for (const std::string& name : hostile)
    {
        CHECK(!net::BareFileName(name, ".sty"));
    }
    for (const char* name : {"", "..", ".", "../x", "a/b", "a\\b", "/x", "a.mmp", "abcdefghijklmnopqrstu", "a\x7f"})
    {
        CHECK(!net::BareFileName(name, ""));
    }
    CHECK(net::BadMapName("mp1-6p", "MP1-comp.gmp", "wil.sty", "MP1-6P.scr").empty());
    CHECK(net::BadMapName("../mp1-6p", "MP1-comp.gmp", "wil.sty", "MP1-6P.scr") == "map");
    CHECK(net::BadMapName("mp1-6p", "../MP1-comp.gmp", "wil.sty", "MP1-6P.scr") == "gmp");
    CHECK(net::BadMapName("mp1-6p", "MP1-comp.gmp", std::string(70, 'A') + "/../wil.sty", "MP1-6P.scr") == "sty");
    CHECK(net::BadMapName("mp1-6p", "MP1-comp.gmp", "wil.sty", "x//////../ste.scr") == "scr");
    CHECK(net::BadMapName("mp1-6p", "MP1-comp.gmp", "wil.sty", "MP1-6P.sty") == "scr");
    // In a welcome as it comes off the wire, the vote's maps too
    net::Welcome w;
    w.players = 2;
    w.map = "mp1-6p";
    w.gmp = "MP1-comp.gmp";
    w.sty = "wil.sty";
    w.scr = "MP1-6P.scr";
    w.maps = {"mp1-6p", "res-6p"};
    net::BitWriter bw;
    net::WriteWelcome(bw, w);
    net::BitReader br(bw.Data().data(), bw.ByteCount());
    net::Welcome got;
    CHECK(br.Bits(8) == net::kMsgWelcome && net::ReadWelcome(br, got) && net::BadMapName(got).empty());
    w.maps.push_back("../../../../home/x/.ssh/id_ed25519");
    CHECK(net::BadMapName(w) == "vote map");
}

// Network text to print: printable ASCII kept, anything else (escape sequences, controls, high bytes) a '?'
void TestPrintable()
{
    CHECK(net::Printable("hello, world ~!") == "hello, world ~!");
    CHECK(net::Printable("") == "");
    CHECK(net::Printable("\x1b]52;c;ZXZpbA==\x07") == "?]52;c;ZXZpbA==?");
    CHECK(net::Printable("\x1b[2Jtitle\r\n\t\x7f") == "?[2Jtitle????");
    CHECK(net::Printable(std::string("a\0b", 3)) == "a?b");
    CHECK(net::Printable("caf\xc3\xa9 \x9b") == "caf?? ?");
}
} // namespace

// A message cut into parts and put together again, in any order, is the message
void TestParts()
{
    for (size_t size = 1; size < 6 * net::MaxPayload(); size += 997)
    {
        std::vector<uint8_t> message(size);
        for (size_t i = 0; i < size; i++)
        {
            message[i] = (uint8_t)Rand();
        }
        std::vector<std::vector<uint8_t>> parts;
        net::WriteSnapshotParts(message, 1234, parts);
        CHECK(parts.size() == (size + net::MaxPayload() - net::kPartHeader - 1) / (net::MaxPayload() - net::kPartHeader));
        std::vector<std::vector<uint8_t>> pieces(parts.size());
        for (size_t k = parts.size(); k-- > 0;) // backwards
        {
            CHECK(parts[k].size() <= net::MaxPayload());
            net::BitReader r(parts[k].data(), parts[k].size());
            CHECK(r.Bits(8) == net::kMsgSnapshotPart);
            uint32_t tick = 0;
            int index = 0, count = 0;
            CHECK(net::ReadSnapshotPart(r, &tick, &index, &count, pieces[k]));
            CHECK(tick == 1234 && index == (int)k && count == (int)parts.size());
        }
        std::vector<uint8_t> whole;
        for (size_t k = 0; k < pieces.size(); k++)
        {
            whole.insert(whole.end(), pieces[k].begin(), pieces[k].end());
        }
        CHECK(whole == message);
    }
}

// EntryBits is a one-entity WriteDelta: new, unchanged, a few fields changed, another type under the id
void TestEntryBits()
{
    const net::Schema s = TestSchema();
    for (int i = 0; i < 3000; i++)
    {
        const uint8_t type = (uint8_t)(Rand() % 3);
        const net::EntityState to = Entity(1 + Rand() % 100000, type, s);
        net::EntityState from = to;
        const int kind = (int)(Rand() % 4);
        if (kind == 1)
        {
            for (int k = (int)(Rand() % 4); k >= 0; k--)
            {
                from.f[Rand() % s.types[type].fields.size()] ^= (int32_t)(1 + Rand() % 7);
            }
        }
        else if (kind == 2)
        {
            from = Entity(to.id, (uint8_t)((type + 1) % 3), s);
        }
        net::Snapshot a, b;
        if (kind != 3)
        {
            a.entities.push_back(from);
        }
        b.entities.push_back(to);
        net::BitWriter w;
        net::WriteDelta(w, s, a, b);
        CHECK(net::EntryBits(s, kind == 3 ? NULL : &from, to) == w.BitCount());
    }
}

// The players a map takes: the test map's script (testdata/tiny-10p.scr, scripts/mis2scr.py) has 10 PLAYER_PEDs
void TestMapPlayers()
{
    FILE* f = fopen(NET_TESTDATA "/tiny-10p.scr", "rb");
    CHECK(f);
    std::vector<uint8_t> scr(100000);
    const size_t n = fread(scr.data(), 1, scr.size(), f);
    fclose(f);
    CHECK(net::ScriptPlayerPeds(scr.data(), n) == 10);
    CHECK(net::ScriptPlayerPeds(scr.data(), 1000) == -1);
    CHECK(net::MapPlayers(10, 0) == 10 && net::MapPlayers(6, 0) == 6 && net::MapPlayers(6, 4) == 4 && net::MapPlayers(6, 8) == 6);
    CHECK(net::MapPlayers(net::kMaxPlayers + 2, 0) == net::kMaxPlayers && net::MapPlayers(0, 6) == 0);
    printf("map players ok\n");
}

int main()
{
    TestBits();
    printf("bitstream ok\n");
    fflush(stdout);
    TestDelta();
    TestEntryBits();
    printf("snapshot deltas ok\n");
    TestParts();
    printf("snapshot parts ok\n");
    fflush(stdout);
    TestMessages();
    TestCrc();
    TestNames();
    TestFileNames();
    TestPrintable();
    TestMapPlayers();
    printf("messages ok\n");
    printf("net core tests passed\n");
    return 0;
}
