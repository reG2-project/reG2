// port/net core: see crc32.hpp.
#include "crc32.hpp"

uint32_t net::Crc32(const void* data, size_t size, uint32_t crc)
{
    static uint32_t table[256];
    if (!table[1])
    {
        for (uint32_t i = 0; i < 256; i++)
        {
            uint32_t c = i;
            for (int k = 0; k < 8; k++)
            {
                c = c & 1 ? 0xEDB88320u ^ (c >> 1) : c >> 1;
            }
            table[i] = c;
        }
    }
    const uint8_t* p = (const uint8_t*)data;
    crc = ~crc;
    for (size_t i = 0; i < size; i++)
    {
        crc = table[(crc ^ p[i]) & 0xFF] ^ (crc >> 8);
    }
    return ~crc;
}
