// Embedded PNGs (port/resources) decoded to RGBA for the renderer's textures.
#pragma once

#include "types.hpp"
#include <stddef.h>
#include <vector>

namespace PortPng
{
// The picture's pixels, a u32 each with the bytes r, g, b, a, row after row; false if it does not decode.
bool Decode(const unsigned char* pPng, size_t size, std::vector<u32>* pPixels, s32* pWidth, s32* pHeight);
} // namespace PortPng
