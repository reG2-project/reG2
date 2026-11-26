#include "png.hpp"
#if defined(GTA2_PORT_SDL)
#include <SDL3/SDL.h>
#include <string.h>
#endif

namespace PortPng
{
bool Decode(const unsigned char* pPng, size_t size, std::vector<u32>* pPixels, s32* pWidth, s32* pHeight)
{
#if defined(GTA2_PORT_SDL)
    SDL_Surface* pLoaded = SDL_LoadPNG_IO(SDL_IOFromConstMem(pPng, size), true);
    SDL_Surface* pSurface = pLoaded ? SDL_ConvertSurface(pLoaded, SDL_PIXELFORMAT_RGBA32) : NULL;
    SDL_DestroySurface(pLoaded);
    if (!pSurface)
    {
        return false;
    }
    *pWidth = pSurface->w;
    *pHeight = pSurface->h;
    pPixels->resize(static_cast<size_t>(pSurface->w) * pSurface->h);
    for (s32 y = 0; y < pSurface->h; y++)
    {
        memcpy(&(*pPixels)[static_cast<size_t>(y) * pSurface->w], static_cast<const u8*>(pSurface->pixels) + y * pSurface->pitch,
               static_cast<size_t>(pSurface->w) * 4);
    }
    SDL_DestroySurface(pSurface);
    return true;
#else
    (void)pPng;
    (void)size;
    (void)pPixels;
    (void)pWidth;
    (void)pHeight;
    return false;
#endif
}
} // namespace PortPng
