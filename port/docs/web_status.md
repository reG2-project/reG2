# Web port: status

Scratch notes for following the work on `port/web`; `web.md` is the documentation.

## Works (tested with Playwright in headless Chromium, Firefox for the import)
- The game and the importer build with Emscripten (`port/web/deps.sh`, `build.sh`, `web.cmake`).
- Import of the 2004 freeware installer in a worker (6 s in Chromium), stored in IndexedDB.
- Play: intro, menus, a game in Downtown at 60 fps on a desktop GPU; the game code takes 0.6 to 0.8 ms per frame.
- Saves: SAVE AND QUIT stored at once, reload, LOAD GAME continues.
- WebGL context loss message, offline start through the service worker, the /play/ layout of the Pages site.
- Phone viewport with touch: menus by tapping, touch stick and buttons drive the game.
- Native Linux build still builds and runs; MSVC6 build: 3430/3430.

- `port/web/tests/e2e.mjs`: all scenarios pass in Chromium and Firefox (touch: Chromium only).
- Memory in a game: 64 MB of wasm memory, plus about 380 MB of game files in JavaScript memory (290 MB music).

## Next
- Not tested on real phones, Safari or iPhone; real gamepads in a browser.
- Possible: music read on demand rather than all loaded, to save about 290 MB on phones.

## Decisions
- Main loop: Asyncify limited to main, WinMain, GameWinMain, port_WebYield (`port/web/asyncify_only.json`):
  2.96 MB wasm against 3.70 MB for full Asyncify.
- No pthreads: the importer runs in a Web Worker of its own (a separate small wasm with install.cpp).
- Storage: IndexedDB (idb-keyval), files copied to MEMFS at Play; music loaded after the start.
- Hidden on the web: FULL SCREEN (the page's), DISPLAY MODE, VSYNC, 3D SOUND; CHECK FOR UPDATES not built.
- The page shares ds.css with the landing page, copied into /play/ at build time.
