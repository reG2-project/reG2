// port/net core: CRC-32 (zlib's), for the data files both ends must have the same (the join's check, protocol.hpp)
#pragma once
#include <stddef.h>
#include <stdint.h>

namespace net
{
// <crc>: the CRC of what came before, to go on with (0 to start)
uint32_t Crc32(const void* data, size_t size, uint32_t crc = 0);
} // namespace net
