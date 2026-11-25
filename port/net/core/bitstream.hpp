// port/net core (port/docs/netcode.md, "Wire format"): a little-endian bit stream. The reader never reads past its end:
// it returns zeros and remembers the overflow, so a malformed packet from a client is rejected, not trusted.
#pragma once
#include <stddef.h>
#include <stdint.h>
#include <vector>

namespace net
{
class BitWriter
{
  public:
    void Bits(uint32_t value, int count) // the low <count> bits (0..32)
    {
        if (count > 0)
        {
            Put(count < 32 ? value & ((1u << count) - 1) : value, count);
        }
    }
    void Bool(bool value)
    {
        Put(value ? 1 : 0, 1);
    }
    void VarU(uint32_t value); // 7 bits per step, small values small
    void VarS(int32_t value) // zigzag, then VarU
    {
        VarU(((uint32_t)value << 1) ^ (uint32_t)(value >> 31));
    }
    void Bytes(const void* data, size_t size); // byte-aligns first
    void Clear() // empty, its storage kept
    {
        mData.clear();
        mBits = 0;
    }

    const std::vector<uint8_t>& Data() const
    {
        return mData;
    }
    size_t BitCount() const
    {
        return mBits;
    }
    size_t ByteCount() const
    {
        return (mBits + 7) / 8;
    }

  private:
    void Put(uint64_t value, int count); // value: count bits (1..56), none above

    std::vector<uint8_t> mData;
    size_t mBits = 0;
};

// BitWriter's bit count without the bytes (EntryBits prices an entry before it is written)
class BitCounter
{
  public:
    void Bits(uint32_t, int count)
    {
        mBits += count > 0 ? (size_t)count : 0;
    }
    void VarU(uint32_t value)
    {
        do
        {
            value >>= 7;
            mBits += 8;
        } while (value);
    }
    void VarS(int32_t value)
    {
        VarU(((uint32_t)value << 1) ^ (uint32_t)(value >> 31));
    }
    size_t BitCount() const
    {
        return mBits;
    }

  private:
    size_t mBits = 0;
};

class BitReader
{
  public:
    BitReader(const void* data, size_t size) : mData((const uint8_t*)data), mSize(size)
    {
    }

    uint32_t Bits(int count);
    bool Bool()
    {
        return Bits(1) != 0;
    }
    uint32_t VarU();
    int32_t VarS();
    bool Bytes(void* out, size_t size);

    bool Overflow() const
    {
        return mOverflow;
    }
    size_t BitsLeft() const
    {
        return mOverflow ? 0 : mSize * 8 - mBits;
    }

  private:
    const uint8_t* mData;
    size_t mSize;
    size_t mBits = 0;
    bool mOverflow = false;
};
} // namespace net
