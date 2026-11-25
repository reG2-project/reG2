#include "gl_context.hpp"
#include "gl_funcs.hpp"
#include "present_log.hpp"
#include <SDL3/SDL.h>

#if defined(__EMSCRIPTEN__)
// The browser shows the canvas once the game's loop yields (port/web/src/web_platform.cpp).
extern "C" void port_WebMarkPresented();
#endif

namespace
{
class SDLGLContext : public IGLContext
{
  public:
    SDLGLContext(SDL_Window* pWindow, SDL_GLContext ctx)
        : mWindow(pWindow), mContext(ctx), mFlush(reinterpret_cast<void (*)()>(SDL_GL_GetProcAddress("glFlush")))
    {
    }

    ~SDLGLContext()
    {
        SDL_GL_MakeCurrent(mWindow, NULL);
        SDL_GL_DestroyContext(mContext);
    }

    void* GetProcAddress(const char* name) override
    {
        return reinterpret_cast<void*>(SDL_GL_GetProcAddress(name));
    }
    void GetDrawableSize(int* pW, int* pH) override
    {
        SDL_GetWindowSizeInPixels(mWindow, pW, pH);
    }
    void SwapBuffers() override
    {
        PresentLog::BeforeSwap();
        // SDL's Wayland swap waits for the frame callback before the commands go out: send them first, so the GPU draws during the wait.
#if defined(__EMSCRIPTEN__)
        port_WebMarkPresented();
#else
        mFlush();
        SDL_GL_SwapWindow(mWindow);
#endif
        PresentLog::AfterSwap();
    }
    void MakeCurrent() override
    {
        SDL_GL_MakeCurrent(mWindow, mContext);
    }

  private:
    SDL_Window* mWindow;
    SDL_GLContext mContext;
    void (*mFlush)();
};
} // namespace

// OpenGL ES 3.0 first; else OpenGL 3.3 core, for Windows drivers and wine without ES contexts.
IGLContext* CreateGLContext(SDL_Window* pWindow, const char** ppError)
{
    SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
    SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 0);
    SDL_GL_SetAttribute(SDL_GL_STENCIL_SIZE, 0);
#if defined(__EMSCRIPTEN__)
    // An opaque canvas: the browser need not blend it with the page behind.
    SDL_GL_SetAttribute(SDL_GL_ALPHA_SIZE, 0);
#endif
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_ES);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 0);
    SDL_GLContext ctx = SDL_GL_CreateContext(pWindow);
    if (!ctx)
    {
        SDL_Log("OpenGL ES 3.0: %s; trying OpenGL 3.3 core", SDL_GetError());
        SDL_GL_UnloadLibrary();
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 3);
        if (SDL_GL_LoadLibrary(NULL))
        {
            ctx = SDL_GL_CreateContext(pWindow);
        }
        if (!ctx)
        {
            SDL_Log("OpenGL 3.3 core: %s", SDL_GetError());
        }
    }
    if (!ctx)
    {
        *ppError = SDL_GetError();
        return NULL;
    }
    PFNGLGETSTRINGPROC getString = reinterpret_cast<PFNGLGETSTRINGPROC>(SDL_GL_GetProcAddress("glGetString"));
    auto glString = [getString](GLenum name) {
        const char* s = getString ? reinterpret_cast<const char*>(getString(name)) : NULL;
        return s ? s : "?";
    };
    SDL_Log("OpenGL: %s, %s, %s, GLSL %s", glString(GL_VERSION), glString(GL_VENDOR), glString(GL_RENDERER),
            glString(GL_SHADING_LANGUAGE_VERSION));
#if !defined(__EMSCRIPTEN__)
    // In a browser the page's frames pace the game (web/src/web_platform.cpp); SDL's swap interval there sets a
    // main loop timing the port does not use.
    SDL_GL_SetSwapInterval(1);
#endif
    PresentLog::Attach(pWindow);
    return new SDLGLContext(pWindow, ctx);
}
