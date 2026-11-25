// The Bink API the game's movie code calls (Source/Bink.cpp), on the libbink decoder: each frame at the
// file's rate, shown letterboxed by the renderer, the sound mixed by miles.cpp. A file that cannot be
// played opens as an empty movie, which ends at once.
#include "types.hpp"
#include "3rdParty/bink.h"
#include "port_platform.hpp"
#include "../../3rdParty/libbink/bink.h"
#include <limits.h>
#include <string.h>
#include <vector>

namespace
{
// The surface type of our BinkBufferOpen: RGBA rows.
const u32 kSurfaceRgba = 0x100;

struct Movie
{
    BINK mHeader; // what the game reads: first, the handle points to it
    bink mDecoder;
    Uint64 mFrameNs;
    Uint64 mStartNs; // when the first frame was shown, 0 before
    bool mSound;
};

struct Buffer
{
    BINKBUFFER mHeader; // first, as for Movie
    std::vector<u8> mPixels;
    s32 mWidth;
    bool mFilled; // a frame was copied in since the last blit
};

Movie* M(BINK* hbink)
{
    return reinterpret_cast<Movie*>(hbink);
}

Buffer* B(BINKBUFFER* hbinkbuffer)
{
    return reinterpret_cast<Buffer*>(hbinkbuffer);
}

// The movie the sound comes from: one plays while the next opens.
Movie* sSoundMovie;
Buffer* sCopyBuffer;

bool OpenDecoder(Movie* m, const char* name)
{
    char path[PATH_MAX];
    m->mDecoder = bink_open_file(port_resolve_path(name, path), BINK_MODE_DISK);
    unsigned long w = 0, h = 0, flags = 0, frame = 0, frames = 0;
    double usf = 0;
    if (!m->mDecoder || bink_info_video(m->mDecoder, &w, &h, &flags) != 0 ||
        bink_info_all(m->mDecoder, &frame, &frames, &usf) != 0 || !w || !h || !frames || !(usf > 0) ||
        bink_enable_video(m->mDecoder, 1) != 0)
    {
        return false;
    }
    unsigned char tracks = 0, channels[7] = {}, bits[7] = {};
    unsigned long rates[7] = {};
    if (bink_info_audio(m->mDecoder, &tracks, channels, bits, rates) == 0 && (tracks & 1) && bits[0] == 16 &&
        (channels[0] == 1 || channels[0] == 2) && bink_enable_audio(m->mDecoder, 0, 1) == 0)
    {
        m->mSound = port_MovieSoundOpen((int)rates[0], channels[0]);
        if (m->mSound)
        {
            sSoundMovie = m;
        }
    }
    const char first = bink_first(m->mDecoder);
    if (first != BINK_MORE && first != BINK_LAST)
    {
        return false;
    }
    m->mHeader.width = (s32)w;
    m->mHeader.height = (s32)h;
    m->mHeader.total_frames = (s32)frames;
    m->mFrameNs = (Uint64)(usf * 1000.0 + 0.5);
    SDL_Log("Movie %s: %lux%lu, %lu frames, %.3f per second%s", name, w, h, frames, 1e6 / usf, m->mSound ? ", sound" : "");
    return true;
}

void CloseDecoder(Movie* m)
{
    if (sSoundMovie == m)
    {
        port_MovieSoundClose();
        sSoundMovie = NULL;
    }
    m->mSound = false;
    if (m->mDecoder)
    {
        bink_close(m->mDecoder);
        m->mDecoder = NULL;
    }
}
} // namespace

BINKDEC BINK* __stdcall BinkOpen(const char* name, s32)
{
    Movie* m = new Movie();
    m->mHeader.total_frames = 1;
    m->mHeader.current_frame = 1;
    if (!OpenDecoder(m, name))
    {
        SDL_Log("Movie %s: cannot be played, skipped", name);
        CloseDecoder(m);
    }
    return &m->mHeader;
}

BINKDEC void __stdcall BinkClose(BINK* hbink)
{
    CloseDecoder(M(hbink));
    delete M(hbink);
}

BINKDEC void __stdcall BinkGetSummary(BINK*, void* summary)
{
    memset(summary, 0, 0x78);
}

// The current frame's sound, as it is shown.
BINKDEC s32 __stdcall BinkDoFrame(BINK* hbink)
{
    Movie* m = M(hbink);
    if (m->mSound && sSoundMovie == m)
    {
        port_MovieSoundQueue(bink_get_audio(m->mDecoder, 0), (int)bink_get_audio_size(m->mDecoder, 0));
    }
    return 0;
}

BINKDEC void __stdcall BinkNextFrame(BINK* hbink)
{
    Movie* m = M(hbink);
    const char next = m->mDecoder ? bink_next(m->mDecoder) : BINK_ERROR;
    if (next != BINK_MORE && next != BINK_LAST)
    {
        // A broken frame: the movie ends on the one shown.
        m->mHeader.total_frames = m->mHeader.current_frame;
        return;
    }
    m->mHeader.current_frame++;
}

// 0 once the current frame is due. The game polls it in its message loop: a short sleep keeps the
// loop from spinning while the key that skips the movie is still read often.
BINKDEC s32 __stdcall BinkWait(BINK* hbink)
{
    Movie* m = M(hbink);
    const Uint64 now = SDL_GetTicksNS();
    if (!m->mDecoder || !m->mStartNs)
    {
        m->mStartNs = now;
        return 0;
    }
    const Uint64 due = m->mStartNs + (Uint64)(m->mHeader.current_frame - 1) * m->mFrameNs;
    if (now >= due)
    {
        return 0;
    }
#if defined(__EMSCRIPTEN__)
    port_WebMarkIdle();
#else
    SDL_DelayPrecise(SDL_min(due - now, 2 * SDL_NS_PER_MS));
#endif
    return 1;
}

// Only into our buffers: the game's other path, into the video surface, needs the 3dfx renderer.
BINKDEC s32 __stdcall BinkCopyToBuffer(BINK* hbink, void* dest, s32 destpitch, s32 destheight, s32, s32, s32 flags)
{
    Movie* m = M(hbink);
    const u8* pRgb = m->mDecoder ? bink_get_video(m->mDecoder) : NULL;
    if (!pRgb || (u32)(flags & 0xFFFF) != kSurfaceRgba || !sCopyBuffer || dest != sCopyBuffer->mPixels.data())
    {
        return 0;
    }
    const s32 w = SDL_min(m->mHeader.width, sCopyBuffer->mWidth);
    const s32 h = SDL_min(m->mHeader.height, destheight);
    for (s32 y = 0; y < h; y++)
    {
        const u8* in = pRgb + (size_t)y * m->mHeader.width * 3;
        u8* out = static_cast<u8*>(dest) + (size_t)y * destpitch;
        for (s32 x = 0; x < w; x++)
        {
            out[x * 4 + 0] = in[x * 3 + 0];
            out[x * 4 + 1] = in[x * 3 + 1];
            out[x * 4 + 2] = in[x * 3 + 2];
            out[x * 4 + 3] = 0xFF;
        }
    }
    sCopyBuffer->mFilled = true;
    return 0;
}

BINKDEC void __stdcall BinkSetSoundSystem(void*, s32)
{
}

BINKDEC void __stdcall BinkSetIOSize(s32)
{
}

BINKDEC void* __stdcall BinkOpenMiles(s32)
{
    return NULL;
}

BINKDEC BINKBUFFER* __stdcall BinkBufferOpen(HWND, s32 width, s32 height, s32)
{
    Buffer* b = new Buffer();
    b->mWidth = SDL_max(width, 1);
    b->mHeader.height = SDL_max(height, 1);
    b->mHeader.pitch = b->mWidth * 4;
    b->mHeader.surface_type = kSurfaceRgba;
    b->mPixels.assign((size_t)b->mHeader.pitch * b->mHeader.height, 0);
    b->mHeader.pixels = b->mPixels.data();
    return &b->mHeader;
}

BINKDEC void __stdcall BinkBufferClose(BINKBUFFER* hbinkbuffer)
{
    if (sCopyBuffer == B(hbinkbuffer))
    {
        sCopyBuffer = NULL;
    }
    delete B(hbinkbuffer);
}

BINKDEC void* __stdcall BinkBufferLock(BINKBUFFER* hbinkbuffer)
{
    sCopyBuffer = B(hbinkbuffer);
    return hbinkbuffer->pixels;
}

BINKDEC void __stdcall BinkBufferUnlock(BINKBUFFER*)
{
    sCopyBuffer = NULL;
}

BINKDEC void __stdcall BinkBufferBlit(BINKBUFFER* hbinkbuffer, void*, s32)
{
    Buffer* b = B(hbinkbuffer);
    if (b->mFilled)
    {
        b->mFilled = false;
        port_ShowMovieFrame(b->mPixels.data(), b->mWidth, b->mHeader.height);
    }
}

BINKDEC void __stdcall BinkBufferSetDDPrimary(void*)
{
}

BINKDEC void __stdcall BinkBufferCheckWinPos(BINKBUFFER*, s32*, s32*)
{
}

BINKDEC void __stdcall BinkBufferSetOffset(BINKBUFFER*, s32, s32)
{
}
