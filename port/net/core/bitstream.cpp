// port/net core: see bitstream.hpp.
#include "bitstream.hpp"
#include <string.h>

void net::BitWriter::Put(uint64_t value, int count)
{
    const size_t at = mBits >> 3;
    value <<= mBits & 7;
    mBits += (size_t)count;
    const size_t size = (mBits + 7) >> 3;
    if (size > mData.size())
    {
        if (size > mData.capacity())
        {
            mData.reserve(size < 128 ? 256 : size * 2);
        }
        mData.resize(size); // zeros: the bits above mBits stay clear
    }
    for (size_t k = at; k < size; k++, value >>= 8)
    {
        mData[k] |= (uint8_t)value;
    }
}

void net::BitWriter::VarU(uint32_t value)
{
    uint64_t bytes = 0;
    int count = 0;
    do
    {
        const uint32_t part = value & 0x7F;
        value >>= 7;
        bytes |= (uint64_t)(part | (value ? 0x80 : 0)) << count;
        count += 8;
    } while (value);
    Put(bytes, count); // up to 5 bytes
}

void net::BitWriter::Bytes(const void* data, size_t size)
{
    mBits = (mBits + 7) & ~(size_t)7;
    const size_t at = mBits / 8;
    mData.resize(at + size);
    memcpy(&mData[at], data, size);
    mBits += size * 8;
}

uint32_t net::BitReader::Bits(int count)
{
    if (mOverflow || mBits + count > mSize * 8)
    {
        mOverflow = true;
        return 0;
    }
    if (count <= 0)
    {
        return 0;
    }
    const size_t at = mBits >> 3;
    const size_t end = (mBits + (size_t)count + 7) >> 3; // within mSize: checked above
    uint64_t bytes = 0;
    for (size_t k = at; k < end; k++)
    {
        bytes |= (uint64_t)mData[k] << (8 * (k - at));
    }
    const uint64_t value = bytes >> (mBits & 7);
    mBits += (size_t)count;
    return count < 32 ? (uint32_t)value & ((1u << count) - 1) : (uint32_t)value;
}

uint32_t net::BitReader::VarU()
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
    mOverflow = true; // more than 5 steps: not ours
    return 0;
}

int32_t net::BitReader::VarS()
{
    const uint32_t z = VarU();
    return (int32_t)((z >> 1) ^ (0u - (z & 1)));
}

bool net::BitReader::Bytes(void* out, size_t size)
{
    mBits = (mBits + 7) & ~(size_t)7;
    if (mOverflow || mBits / 8 + size > mSize)
    {
        mOverflow = true;
        memset(out, 0, size);
        return false;
    }
    memcpy(out, mData + mBits / 8, size);
    mBits += size * 8;
    return true;
}
