// port/net encoder tests (ctest): the bit stream and the snapshot coder give the bytes, bit counts and values of a plain
// bit-by-bit reference (below), on random streams, snapshots, deltas and entries. A failed CHECK prints and exits 1.
#include "check.hpp"
#include "../core/bitstream.hpp"
#include "../core/snapshot.hpp"
#include <algorithm>
#include <random>
#include <stdio.h>
#include <string.h>
#include <vector>

namespace ref
{
class BitWriter
{
  public:
    void Bits(uint32_t value, int count)
    {
        for (int i = 0; i < count; i++)
        {
            if ((mBits & 7) == 0)
            {
                mData.push_back(0);
            }
            if ((value >> i) & 1)
            {
                mData[mBits >> 3] |= (uint8_t)(1u << (mBits & 7));
            }
            mBits++;
        }
    }
    void Bool(bool value)
    {
        Bits(value ? 1 : 0, 1);
    }
    void VarU(uint32_t value)
    {
        do
        {
            const uint32_t part = value & 0x7F;
            value >>= 7;
            Bits(part | (value ? 0x80 : 0), 8);
        } while (value);
    }
    void VarS(int32_t value)
    {
        VarU(((uint32_t)value << 1) ^ (uint32_t)(value >> 31));
    }
    void Bytes(const void* data, size_t size)
    {
        mBits = (mBits + 7) & ~(size_t)7;
        const size_t at = mBits / 8;
        mData.resize(at + size);
        memcpy(&mData[at], data, size);
        mBits += size * 8;
    }
    std::vector<uint8_t> mData;
    size_t mBits = 0;
};

class BitReader
{
  public:
    BitReader(const uint8_t* data, size_t size) : mData(data), mSize(size)
    {
    }
    uint32_t Bits(int count)
    {
        if (mOverflow || mBits + count > mSize * 8)
        {
            mOverflow = true;
            return 0;
        }
        uint32_t value = 0;
        for (int i = 0; i < count; i++)
        {
            value |= (uint32_t)((mData[mBits >> 3] >> (mBits & 7)) & 1) << i;
            mBits++;
        }
        return value;
    }
    uint32_t VarU()
    {
        uint32_t value = 0;
        for (int shift = 0; shift < 35; shift += 7)
        {
            const uint32_t part = Bits(8);
            value |= (part & 0x7F) << shift;
            if (!(part & 0x80))
            {
                return value;
            }
        }
        mOverflow = true;
        return 0;
    }
    int32_t VarS()
    {
        const uint32_t z = VarU();
        return (int32_t)((z >> 1) ^ (0u - (z & 1)));
    }
    size_t BitsLeft() const
    {
        return mOverflow ? 0 : mSize * 8 - mBits;
    }
    const uint8_t* mData;
    size_t mSize;
    size_t mBits = 0;
    bool mOverflow = false;
};

// The snapshot coder (core/snapshot.cpp) as first written: every field and mask bit visited, an entry priced by
// encoding it
struct Mask
{
    uint32_t w[net::kMaxFields / 32];
    bool Any() const
    {
        for (uint32_t x : w)
        {
            if (x)
            {
                return true;
            }
        }
        return false;
    }
    bool Has(size_t i) const
    {
        return (w[i / 32] >> (i % 32)) & 1;
    }
    void Set(size_t i)
    {
        w[i / 32] |= 1u << (i % 32);
    }
};

int32_t Wire(const net::FieldDesc& d, int32_t v)
{
    if (d.bits >= 32)
    {
        return v;
    }
    uint32_t u = (uint32_t)v & ((1u << d.bits) - 1);
    if (d.isSigned && (u >> (d.bits - 1)) & 1)
    {
        u |= ~0u << d.bits;
    }
    return (int32_t)u;
}

int IndexBits(size_t fields)
{
    int b = 1;
    while ((1u << b) < fields)
    {
        b++;
    }
    return b;
}

int VarSBits(int32_t v)
{
    uint32_t z = ((uint32_t)v << 1) ^ (uint32_t)(v >> 31);
    int bits = 8;
    while (z >= 128)
    {
        z >>= 7;
        bits += 8;
    }
    return bits;
}

void WriteMask(BitWriter& w, const Mask& mask, int n)
{
    for (int i = 0; i < n; i += 32)
    {
        w.Bits(mask.w[i / 32], n - i < 32 ? n - i : 32);
    }
}

void WriteFields(BitWriter& w, const net::EntityType& t, const net::EntityState& s, const Mask& mask, const net::EntityState* base)
{
    const size_t n = t.fields.size();
    size_t changed = 0;
    for (size_t i = 0; i < n; i++)
    {
        changed += mask.Has(i);
    }
    if (base)
    {
        const int ib = IndexBits(n);
        const bool list = changed < 128 && 7 + changed * (size_t)ib < n;
        w.Bits(list ? 1 : 0, 1);
        if (list)
        {
            w.Bits((uint32_t)changed, 7);
            for (size_t i = 0; i < n; i++)
            {
                if (mask.Has(i))
                {
                    w.Bits((uint32_t)i, ib);
                }
            }
        }
        else
        {
            WriteMask(w, mask, (int)n);
        }
    }
    else
    {
        WriteMask(w, mask, (int)n);
    }
    for (size_t i = 0; i < n; i++)
    {
        if (!mask.Has(i))
        {
            continue;
        }
        const net::FieldDesc& d = t.fields[i];
        if (base)
        {
            const int32_t diff = (int32_t)((uint32_t)Wire(d, s.f[i]) - (uint32_t)Wire(d, base->f[i]));
            const bool small = VarSBits(diff) < d.bits;
            w.Bits(small ? 1 : 0, 1);
            if (small)
            {
                w.VarS(diff);
                continue;
            }
        }
        w.Bits((uint32_t)s.f[i], d.bits);
    }
}

Mask ChangedMask(const net::EntityType& t, const net::EntityState& a, const net::EntityState& b)
{
    Mask mask = {};
    for (size_t i = 0; i < t.fields.size(); i++)
    {
        if (Wire(t.fields[i], a.f[i]) != Wire(t.fields[i], b.f[i]))
        {
            mask.Set(i);
        }
    }
    return mask;
}

void WriteDelta(BitWriter& w, const net::Schema& schema, const net::Snapshot& from, const net::Snapshot& to, bool check)
{
    static const net::EntityState kZero = {};
    struct Entry
    {
        uint32_t id;
        int op;
        const net::EntityState* now;
        Mask mask;
        const net::EntityState* was;
    };
    std::vector<Entry> entries;
    size_t i = 0, j = 0;
    while (i < from.entities.size() || j < to.entities.size())
    {
        const net::EntityState* a = i < from.entities.size() ? &from.entities[i] : NULL;
        const net::EntityState* b = j < to.entities.size() ? &to.entities[j] : NULL;
        if (b && (!a || b->id < a->id))
        {
            entries.push_back({b->id, 1, b, ChangedMask(schema.types[b->type], kZero, *b), NULL});
            j++;
        }
        else if (a && (!b || a->id < b->id))
        {
            entries.push_back({a->id, 2, NULL, Mask(), NULL});
            i++;
        }
        else
        {
            if (a->type != b->type)
            {
                entries.push_back({b->id, 1, b, ChangedMask(schema.types[b->type], kZero, *b), NULL});
            }
            else
            {
                const Mask mask = ChangedMask(schema.types[b->type], *a, *b);
                if (mask.Any())
                {
                    entries.push_back({b->id, 0, b, mask, a});
                }
            }
            i++;
            j++;
        }
    }
    w.VarU((uint32_t)entries.size());
    uint32_t prev = 0;
    for (const Entry& e : entries)
    {
        w.VarU(e.id - prev);
        prev = e.id;
        w.Bits((uint32_t)e.op, 2);
        if (e.op == 1)
        {
            w.VarU(e.now->type);
        }
        if (e.op != 2)
        {
            WriteFields(w, schema.types[e.now->type], *e.now, e.mask, e.op == 0 ? e.was : NULL);
        }
    }
    if (check)
    {
        w.Bits(1, 1);
        for (const net::EntityState& e : to.entities)
        {
            w.Bits(net::EntityHash(schema, e), 16);
        }
    }
}

size_t EntryBits(const net::Schema& schema, const net::EntityState* from, const net::EntityState& to)
{
    static const net::EntityState kZero = {};
    const net::EntityType& t = schema.types[to.type];
    BitWriter w;
    if (from && from->type == to.type)
    {
        const Mask mask = ChangedMask(t, *from, to);
        if (!mask.Any())
        {
            w.VarU(0);
            return w.mBits;
        }
        w.VarU(1);
        w.VarU(to.id);
        w.Bits(0, 2);
        WriteFields(w, t, to, mask, from);
        return w.mBits;
    }
    w.VarU(1);
    w.VarU(to.id);
    w.Bits(1, 2);
    w.VarU(to.type);
    WriteFields(w, t, to, ChangedMask(t, kZero, to), NULL);
    return w.mBits;
}

Mask ReadMask(BitReader& r, int n)
{
    Mask mask = {};
    for (int i = 0; i < n; i += 32)
    {
        mask.w[i / 32] = r.Bits(n - i < 32 ? n - i : 32);
    }
    return mask;
}

bool ReadFields(BitReader& r, const net::EntityType& t, net::EntityState& s, bool update)
{
    const size_t n = t.fields.size();
    Mask mask = {};
    if (update && r.Bits(1))
    {
        const uint32_t changed = r.Bits(7);
        const int ib = IndexBits(n);
        for (uint32_t k = 0; k < changed && !r.mOverflow; k++)
        {
            const uint32_t i = r.Bits(ib);
            if (i >= n)
            {
                return false;
            }
            mask.Set(i);
        }
    }
    else
    {
        mask = ReadMask(r, (int)n);
    }
    for (size_t i = 0; i < n; i++)
    {
        if (!mask.Has(i))
        {
            continue;
        }
        const net::FieldDesc& d = t.fields[i];
        if (update && r.Bits(1))
        {
            s.f[i] = Wire(d, (int32_t)((uint32_t)Wire(d, s.f[i]) + (uint32_t)r.VarS()));
            continue;
        }
        uint32_t v = r.Bits(d.bits);
        if (d.isSigned && d.bits < 32 && (v >> (d.bits - 1)) & 1)
        {
            v |= ~0u << d.bits;
        }
        s.f[i] = (int32_t)v;
    }
    return !r.mOverflow;
}

bool ReadDelta(BitReader& r, const net::Schema& schema, const net::Snapshot& from, net::Snapshot& to, net::DeltaCheck* check)
{
    to.entities.clear();
    std::vector<uint8_t> how;
    const uint32_t count = r.VarU();
    if (count > (uint32_t)(from.entities.size() + r.BitsLeft()))
    {
        return false;
    }
    size_t i = 0;
    uint32_t id = 0;
    for (uint32_t k = 0; k < count; k++)
    {
        const uint32_t gap = r.VarU();
        id += gap;
        const int op = (int)r.Bits(2);
        if (r.mOverflow || op > 2 || (k && !gap))
        {
            return false;
        }
        while (i < from.entities.size() && from.entities[i].id < id)
        {
            to.entities.push_back(from.entities[i++]);
            how.push_back(net::DeltaCheck::kCarried);
        }
        const net::EntityState* base = i < from.entities.size() && from.entities[i].id == id ? &from.entities[i] : NULL;
        if (base)
        {
            i++;
        }
        if (op == 2)
        {
            if (!base)
            {
                return false;
            }
            continue;
        }
        net::EntityState s;
        if (op == 1)
        {
            memset(&s, 0, sizeof(s));
            s.id = id;
            const uint32_t type = r.VarU();
            if (type >= schema.types.size())
            {
                return false;
            }
            s.type = (uint8_t)type;
        }
        else
        {
            if (!base)
            {
                return false;
            }
            s = *base;
        }
        if (!ReadFields(r, schema.types[s.type], s, op == 0))
        {
            return false;
        }
        to.entities.push_back(s);
        how.push_back(op == 1 ? net::DeltaCheck::kCreated : net::DeltaCheck::kUpdated);
    }
    while (i < from.entities.size())
    {
        to.entities.push_back(from.entities[i++]);
        how.push_back(net::DeltaCheck::kCarried);
    }
    if (r.mOverflow)
    {
        return false;
    }
    if (check && r.BitsLeft() >= 1 + 16 * to.entities.size() && r.Bits(1))
    {
        check->checked = true;
        check->bad.clear();
        for (size_t k = 0; k < to.entities.size(); k++)
        {
            if ((uint16_t)r.Bits(16) != net::EntityHash(schema, to.entities[k]))
            {
                check->bad.push_back(k);
            }
        }
        check->how.swap(how);
    }
    return !r.mOverflow;
}
} // namespace ref

namespace
{
std::mt19937 gRandom(7);

uint32_t Rand(uint32_t n)
{
    return (uint32_t)(gRandom() % n);
}

// Random widths and signs; field counts at the mask words' edges and the most a type has
net::Schema RandomSchema()
{
    static const int kCounts[] = {1, 2, 31, 32, 33, 64, 65, 127, net::kMaxFields};
    net::Schema s;
    for (int t = 0; t < 12; t++)
    {
        net::EntityType et;
        et.name = "t";
        const int n = t < 9 ? kCounts[t] : 1 + (int)Rand(net::kMaxFields);
        for (int i = 0; i < n; i++)
        {
            et.fields.push_back({"f", (uint8_t)(1 + Rand(32)), Rand(2) != 0});
        }
        s.types.push_back(et);
    }
    return s;
}

int32_t RandomValue()
{
    switch (Rand(4))
    {
        case 0:
            return (int32_t)gRandom(); // any width: cut on the wire
        case 1:
            return 0;
        default:
            return (int32_t)Rand(400) - 200;
    }
}

// A snapshot; with like, mostly its entities a little changed (deltas with few fields), some new, some gone
net::Snapshot RandomSnapshot(const net::Schema& schema, const net::Snapshot* like)
{
    net::Snapshot s;
    const int count = (int)Rand(80);
    uint32_t id = 0;
    for (int k = 0; k < count; k++)
    {
        net::EntityState e = {};
        if (like && k < (int)like->entities.size() && Rand(4))
        {
            e = like->entities[k];
            const int kind = (int)Rand(5);
            const size_t n = schema.types[e.type].fields.size();
            if (kind == 0)
            {
                e.type = (uint8_t)Rand((uint32_t)schema.types.size()); // another kind under the id
            }
            for (int c = kind == 1 ? 0 : (int)Rand(kind == 2 ? 3 : 40); c > 0; c--)
            {
                const size_t i = Rand((uint32_t)n);
                e.f[i] = Rand(3) ? (int32_t)((uint32_t)e.f[i] + Rand(2000) - 1000) : RandomValue();
            }
            if (kind == 3)
            {
                e.f[Rand((uint32_t)n)] ^= 1 << Rand(32); // a bit above a narrow field's width: no change on the wire
            }
        }
        else
        {
            id += 1 + Rand(6);
            e.id = id;
            e.type = (uint8_t)Rand((uint32_t)schema.types.size());
            for (int j = 0; j < net::kMaxFields; j++)
            {
                e.f[j] = RandomValue();
            }
        }
        s.entities.push_back(e);
    }
    net::SortById(s);
    s.entities.erase(std::unique(s.entities.begin(), s.entities.end(),
                                 [](const net::EntityState& a, const net::EntityState& b) { return a.id == b.id; }),
                     s.entities.end());
    return s;
}

void TestWriter()
{
    for (int round = 0; round < 500; round++)
    {
        net::BitWriter w;
        ref::BitWriter r;
        for (int op = (int)Rand(300); op > 0; op--)
        {
            const uint32_t v = gRandom();
            switch (Rand(6))
            {
                case 0:
                {
                    const int count = (int)Rand(33);
                    w.Bits(v, count); // bits above the count ignored
                    r.Bits(count < 32 ? v & ((1u << count) - 1) : v, count);
                    break;
                }
                case 1:
                    w.Bool(v & 1);
                    r.Bool(v & 1);
                    break;
                case 2:
                {
                    const uint32_t u = v >> Rand(32);
                    w.VarU(u);
                    r.VarU(u);
                    break;
                }
                case 3:
                {
                    const int32_t value = (int32_t)v >> Rand(32);
                    w.VarS(value);
                    r.VarS(value);
                    break;
                }
                case 4:
                {
                    uint8_t bytes[9];
                    const size_t size = Rand(10);
                    for (size_t k = 0; k < size; k++)
                    {
                        bytes[k] = (uint8_t)gRandom();
                    }
                    w.Bytes(bytes, size);
                    r.Bytes(bytes, size);
                    break;
                }
                default:
                    if (Rand(20) == 0)
                    {
                        w.Clear();
                        r = ref::BitWriter();
                    }
                    break;
            }
            CHECK(w.BitCount() == r.mBits);
        }
        CHECK(w.Data() == r.mData);
        CHECK(w.ByteCount() == r.mData.size());
    }
}

void TestReader()
{
    for (int round = 0; round < 3000; round++)
    {
        std::vector<uint8_t> data(Rand(40));
        for (uint8_t& b : data)
        {
            b = (uint8_t)gRandom();
        }
        net::BitReader a(data.data(), data.size());
        ref::BitReader b(data.data(), data.size());
        for (int op = (int)Rand(80); op > 0; op--)
        {
            switch (Rand(4))
            {
                case 0:
                case 1:
                {
                    const int count = (int)Rand(33);
                    CHECK(a.Bits(count) == b.Bits(count));
                    break;
                }
                case 2:
                    CHECK(a.VarU() == b.VarU());
                    break;
                default:
                    CHECK(a.Bool() == (b.Bits(1) != 0));
                    break;
            }
            CHECK(a.Overflow() == b.mOverflow);
            CHECK(a.BitsLeft() == (b.mOverflow ? 0 : b.mSize * 8 - b.mBits));
        }
    }
}

bool SameEntities(const net::Snapshot& a, const net::Snapshot& b)
{
    if (a.entities.size() != b.entities.size())
    {
        return false;
    }
    for (size_t k = 0; k < a.entities.size(); k++)
    {
        const net::EntityState& x = a.entities[k];
        const net::EntityState& y = b.entities[k];
        if (x.id != y.id || x.type != y.type || memcmp(x.f, y.f, sizeof(x.f)) != 0)
        {
            return false;
        }
    }
    return true;
}

// The reader against the reference on <data>: the same answer, entities and check
void SameRead(const net::Schema& schema, const net::Snapshot& from, const std::vector<uint8_t>& data, bool check)
{
    net::BitReader in(data.data(), data.size());
    ref::BitReader refIn(data.data(), data.size());
    net::Snapshot got, want;
    got.entities.push_back({}); // both clear it
    net::DeltaCheck dc, refDc;
    const bool ok = net::ReadDelta(in, schema, from, got, check ? &dc : NULL);
    CHECK(ok == ref::ReadDelta(refIn, schema, from, want, check ? &refDc : NULL));
    CHECK(SameEntities(got, want));
    CHECK(in.Overflow() == refIn.mOverflow && in.BitsLeft() == refIn.BitsLeft());
    CHECK(dc.checked == refDc.checked && dc.bad == refDc.bad && dc.how == refDc.how);
}

void TestDeltas()
{
    for (int round = 0; round < 40; round++)
    {
        const net::Schema schema = RandomSchema();
        for (int it = 0; it < 100; it++)
        {
            const net::Snapshot a = RandomSnapshot(schema, NULL);
            const net::Snapshot b = RandomSnapshot(schema, &a);
            const net::Snapshot empty;
            for (int k = 0; k < 2; k++)
            {
                const net::Snapshot& from = k ? empty : a;
                const bool check = Rand(2) != 0;
                net::BitWriter w;
                ref::BitWriter r;
                net::WriteDelta(w, schema, from, b, check);
                ref::WriteDelta(r, schema, from, b, check);
                CHECK(w.BitCount() == r.mBits);
                CHECK(w.Data() == r.mData);
                net::BitReader in(w.Data().data(), w.ByteCount());
                net::Snapshot back;
                net::DeltaCheck dc;
                CHECK(net::ReadDelta(in, schema, from, back, check ? &dc : NULL));
                CHECK(back.entities.size() == b.entities.size());
                CHECK(!check || (dc.checked && dc.bad.empty()));
                SameRead(schema, from, w.Data(), check);
                // A malformed one: bits flipped, cut short, or noise
                std::vector<uint8_t> bad = w.Data();
                for (int f = 1 + (int)Rand(4); f > 0 && !bad.empty(); f--)
                {
                    bad[Rand((uint32_t)bad.size())] ^= (uint8_t)(1u << Rand(8));
                }
                SameRead(schema, from, bad, check);
                bad.resize(Rand((uint32_t)bad.size() + 1));
                SameRead(schema, from, bad, check);
                for (uint8_t& x : bad)
                {
                    x = (uint8_t)gRandom();
                }
                SameRead(schema, from, bad, check);
            }
            // Every entry priced against each baseline it could have: none, the same id's, any other entity
            for (size_t k = 0; k < b.entities.size(); k++)
            {
                const net::EntityState& to = b.entities[k];
                const net::EntityState* bases[] = {NULL, k < a.entities.size() ? &a.entities[k] : NULL,
                                                    a.entities.empty() ? NULL : &a.entities[Rand((uint32_t)a.entities.size())], &to};
                for (const net::EntityState* from : bases)
                {
                    CHECK(net::EntryBits(schema, from, to) == ref::EntryBits(schema, from, to));
                }
            }
        }
    }
}
} // namespace

int main()
{
    TestWriter();
    TestReader();
    printf("bitstream as the reference\n");
    fflush(stdout);
    TestDeltas();
    printf("snapshot deltas, their reading and entry sizes as the reference\n");
    return 0;
}
