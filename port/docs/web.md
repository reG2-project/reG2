# The web build

reG2 compiled to WebAssembly with Emscripten, in a page that imports the player's own copy of GTA2 and keeps it
in the browser. Single player only: the network game is not built into the page's menus (the port has none) and
DirectPlay stays the compat layer's stub.

| Path | What it is |
|---|---|
| `port/web/deps.sh` | SDL 3, zlib, xz, unshield and libarchive for WebAssembly, from `packaging/versions.sh` |
| `port/web/build.sh` | Configures and builds with `emcmake`, then assembles the site |
| `port/web/web.cmake` | The web targets, included by `port/CMakeLists.txt` when `EMSCRIPTEN` is set |
| `port/web/src/web_platform.cpp`, `web_library.js` | The yield to the browser, frame pacing |
| `port/web/src/web_input.cpp` | What the page asks the game: its state, the pause, the touch controls' gamepad |
| `port/web/src/import_main.cpp` | `reg2_import.wasm`: `setup/install.cpp`'s check and copy, for the page |
| `port/web/asyncify_only.json` | The only functions Asyncify instruments |
| `port/web/site/` | The page: `index.html`, `style.css`, `app.js`, `storage.js`, `touch.js`, `i18n.js`, `import-worker.js`, `sw.js`, `manifest.webmanifest`, fonts and vendored libraries |
| `port/web/home/`, `port/web/ds.css` | The project's landing page at the site's root (its own README) |
| `.github/workflows/web.yml` | Builds the site; on `main`, publishes it on GitHub Pages |

## Build and run locally

```bash
git clone https://github.com/emscripten-core/emsdk.git && emsdk/emsdk install 7.0.1 && emsdk/emsdk activate 7.0.1
. emsdk/emsdk_env.sh
sh port/web/deps.sh /tmp/web/dl /tmp/web/prefix /tmp/web/b      # once
sh port/web/build.sh /tmp/web/prefix /tmp/web/build /tmp/web/site
python3 -m http.server -d /tmp/web/site 8000                   # then http://localhost:8000/
```

Any static server works; the page needs no special headers (no `SharedArrayBuffer`). `localhost` counts as a
secure context, so the service worker and storage work there as on the published site. `?stats` in the page's
address shows, at the bottom left of the game: frames per second, the game code's and the GPU's time per frame
(`EXT_disjoint_timer_query_webgl2` where the browser has it), late frames, the display's rate, the canvas size and
the WebGL renderer (`?gpu=1` adds the GPU's time, from a timer query each frame). `?debug` also logs a console line a
second: frame intervals, time in the game, Asyncify, sound and storage, WebGL calls, draws, framebuffer binds and
upload bytes, long tasks and page events. Session toggles, to compare frame rates without touching the settings:
`?fps=N` (frame limit, 0 every refresh), `?scale=F` (canvas resolution), `?msaa=N`, `?bloom=0`, `?nofx=1` (the
ENHANCED rendering passes off). `?soundlog` logs, once a second, the sounds started, those restarted before the
mixer reached them, and the time from a start to the mix that plays it (`GTA2_SOUND_LOG` natively); `=2` adds a line per start and per first mix.

### Frame rate in a browser

Each WebGL call goes through the browser's GPU process. The renderer batches sprites and tiles into about 135 draws
a frame; a batch keeps the target, program and vertex array the previous one bound (`GLRenderer::Flush`), about 8
calls a draw. Before that, about 15 calls a draw kept Chrome at 25 to 40 frames a second on a 120 Hz screen; after,
it draws 120. Two things in the page made long tasks and must stay out of a frame: reading the WebGL renderer's name
(it waits for the GPU to finish), and, less clearly, a GPU timer query each frame (now only with `?gpu=1`). On the web
the first start sets FRAME LIMIT 60. Playwright's Chromium on the same KDE Wayland session sometimes drew every second
refresh in unlimited mode (59 a second, its GPU process about 2 ms busy a frame) and other times 120; the user's
Chrome draws 120.

## Publishing (GitHub Pages)

`web.yml` runs on pushes to `main` and `port/**` and on demand. It builds the site and lays it out as:

| Site path | Source |
|---|---|
| `/` | `port/web/home/` |
| `/ds.css` | `port/web/ds.css` |
| `/play/` | `port/web/site/` with the WebAssembly build |

Only `main` deploys. One-time setup in the repository: Settings, Pages, Build and deployment, Source: **GitHub
Actions**. The `github-pages` environment appears with the first deployment. Nothing else is needed: no secret,
no custom headers. A custom domain is optional (Settings, Pages, Custom domain). The site is about 6 MB
(1.4 MB of it the compressed game wasm), well within GitHub Pages' limits.

## How it works

### The main loop

The game keeps its own loop (`GameWinMain` in `Source/winmain.cpp`) and calls `port_WebYield` once per pass
(under `__EMSCRIPTEN__`). That yields to the browser until the next `requestAnimationFrame` after a frame was
drawn, when the loop waits for the next logic step, or at the latest every 50 ms. The waits that would sleep on
the desktop (`Sleep`, `port_DelayNs`, `BinkWait`, `AIL_delay`) only mark the loop idle; the swap only marks a
frame drawn. The frame loop treats the browser's frames as vsync, with the display's period measured from
`requestAnimationFrame` (the median of the last 31 intervals).

Options considered:

- `emscripten_set_main_loop`: would need `GameWinMain`'s nested loop (frontend, game, results) rewritten as a
  state machine, in a matched function; no.
- Full Asyncify: works everywhere but instruments every function that may be on the stack at a yield. Measured:
  3.70 MB wasm (1.62 MB gzipped) against 2.96 MB (1.42 MB) for the narrow version, and every instrumented
  function runs slower.
- JSPI: smaller and faster, but Safari does not ship it.
- **Asyncify limited to `main`, `WinMain`, `GameWinMain` and `port_WebYield`** (`asyncify_only.json`, with
  `ASYNCIFY_IGNORE_INDIRECT`): the cost is near zero, since only those functions are instrumented. The catch: no
  other code may yield, so SDL's own Asyncify yields are turned off (`SDL_HINT_EMSCRIPTEN_ASYNCIFY`) and a level
  load blocks the page for its second or two, as it blocked the window on the desktop. Emscripten's `fsync`
is replaced by a no-op for the same reason (it is asynchronous under Asyncify; the memory file system has nothing
to wait for). Binaryen inlines
  `WinMain` and `port_WebYield` before Asyncify runs, which prints harmless "non-existing function" warnings.

No pthreads, so no `SharedArrayBuffer` and no cross-origin isolation (which GitHub Pages cannot provide without a
service worker trick).

### Storage

IndexedDB, through `idb-keyval`: one entry per file of the game folder (`data/...`, `player/...`,
`gta2_registry.ini`, `gta2gl.ini`) as a Blob, and a record saying whether the imported data is complete. At Play,
the files are copied into Emscripten's in-memory file system under `/reG2`, except the music: its tracks are empty
files until the game opens one (`compat/src/miles.cpp` calls `port_WebLoadFile`); the page then reads it from
IndexedDB while the stream plays silence, and keeps the last two tracks in memory. That keeps 90 to 145 MB
of game files in memory instead of 380 MB (a track is 6 to 32 MB). The game's writes outside `data/` are stored again 250 ms after the file is closed (a
wrapper on `FS.close`, `FS.unlink` and `FS.rename`), and on `pagehide` and when the tab is hidden. After an import
the page asks for persistent storage (`navigator.storage.persist`).

OPFS was not used: synchronous access needs a worker (so the game in a worker, with OffscreenCanvas), and
Emscripten's OPFS backend needs pthreads. The wasm heap stays at 64 MB in a game.

### Import

`import-worker.js` runs `reg2_import.wasm`, the native setup's `install.cpp` with libarchive and unshield (no
setup UI, no thread): the chosen folder or file is mounted read-only with WORKERFS (no copy), the result is
written to the worker's memory file system and sent back file by file to be stored. It reads what the desktop
setup reads: a game folder, the 2004 freeware installer, the CD (as a folder) or an `.iso`, `.zip`, `.7z`. To add
the music to an existing import, the installed files are given to the setup as empty placeholders, so it sees
the game as installed.

### Input

- Keyboard and mouse as on the desktop, through SDL. In a game with TWIN STICK, the mouse aims with pointer lock
  (SDL's relative mode); Escape releases it, a click takes it again.
- Gamepads through the Gamepad API (SDL), with hot plug and rumble where the browser has it.
- Touch: `touch.js` draws the controls and drives a virtual SDL gamepad (`port_WebPad`), so the port's pad
  support does the rest; the action set is the 3DS port's. Held in landscape with two thumbs:
  - on foot, a floating stick on the left half walks; on the right ATTACK, JUMP, ENTER and the weapons;
    with TWIN STICK (state bit 16) the right half is a second floating stick that aims and fires while pushed past
    a third of its range (right stick and RT), with JUMP, ENTER and the weapons in a column at the edge;
  - in a vehicle the left stick steers; GAS and BRAKE (RT, LT), HAND BRAKE, FIRE, HORN (special 1) and EXIT;
  - PAUSE at the right edge in a game, under the HUD's money, hearts and weapon (the top fifth);
  - in the menus (the frontend, the port's menus, the pause menu), a d-pad at the bottom left, OK and BACK at the
    right edge: the pad's d-pad, A and B, which drive every page as a gamepad does, the player's name included (up
    and down pick a letter, right adds one, left deletes one). A direction held repeats after 400 ms, every 180 ms;
    each press and release lasts at least 80 ms, as the game reads the pad once a frame and the page's timers run
    late while the game works. The menus can still be tapped (SDL turns a tap into the mouse);
  - buttons 56 to 96 px, inside the safe areas, a short vibration on each press where the browser has it.
  The sticks are drawn by `touch.js` (about 60 lines): nipplejs placed its stick from its zone's position measured
  when it was made, while hidden, so the stick appeared away from the thumb.
  In a game a blocker under the controls takes the touches that miss them, so none reaches the canvas as a mouse
  click (with TWIN STICK the mouse's left button attacks). Every press ends with its finger, also when the finger
  slides off, the browser cancels the touch, the last finger lifts, or the page loses focus. While the controls are
  in use the page asks for no pointer lock, and a touch ends one the game took for the mouse (TWIN STICK's relative
  mode): under pointer lock every event has the same position, so the sticks could not move. The sticks show,
  faint, at their usual place while at rest. The controls show while touch is in use and hide at a key, a mouse
  move or a gamepad button.
- SDL reads a virtual trigger's whole range, -32768 to 32767, as 0 to 32767: `port_WebPad` sends a released
  trigger as -32768. Sent as 0 it read as half pressed, so RT held ATTACK down with TWIN STICK (the player punched
  without end) and LT and RT held specials 2 and 3 in CLASSIC (the player could not move); emulation showed it as
  soon as the game's state was checked (`port_WebPlayer`, test only).

### Escape and the pause menu

What the player gets, in Chrome and Firefox alike:
- In a game, the mouse aims (pointer lock). Escape: the browser frees the mouse and the pause menu opens, once.
- In the pause menu, Escape (or RESUME) goes back to the game; a click takes the pointer again.
- In full screen in Chrome, Escape goes straight to the game (Keyboard Lock); holding it leaves full screen, as a
  hint says once. Firefox leaves full screen on Escape, and the pause menu opens.
- P (unless a control uses it) and Pause act as Escape in the game, and no browser takes them; so does the
  gamepad's Start, and the page's pause button at the right edge.

The browser takes the first Escape to leave pointer lock (held in a game with TWIN STICK) or full screen, so the
game would never see it. In full screen the page asks for Keyboard Lock (Chromium) so Escape reaches the game, and
says once that holding Escape leaves full screen. Losing pointer lock or full screen during play opens the pause
menu, as the player's Escape would; an Escape the browser delivers as well within 400 ms is dropped, else the menu
would close again. The pause menu also opens from the page's pause button, the touch controls' and a gamepad's
Start. Leaving it, the next click on the game takes the pointer again (a refusal just after Escape is retried).

### Import speed

`import-worker.js` tunes two of Emscripten's file systems for files of hundreds of MB: a memory file grew by an eighth
past 1 MB, copying all of it each time (about 2.7 GB copied for the installer's 300 MB of cabinets), and now doubles;
reads of the chosen file go 4 MB ahead, one `FileReaderSync` call each. With `?debug` the page logs the phases.
The free installer on a desktop (ms): before, source reads 1300 to 1650, memory writes 3000, unpacking 2250, sending
200, total 8300 to 9000; after, 550 to 980, 420 to 750, 2200, 130 to 210, total 4300 to 5200. The unpacking (zlib in
unshield) is CPU bound; the copies were memory bound, which phones pay most for. On a Poco F1 (Snapdragon 845) the
free installer went from several minutes to about 40 s.

Over plain HTTP (a LAN address) the page works without what browsers keep for secure contexts (service worker,
storage persistence, wake lock, the tab lock) and says so above everything else.

### Sound

SDL plays through a ScriptProcessorNode, mixed on the page's thread between frames. The menus used to crackle: the
frontend polls the clock between its 30 Hz updates without waiting, which kept the thread busy for 32 ms of every
frame; the loop now yields after 2 ms of that (`port_WebYield`). In a game, short hitches (streaming, a level's
first effects) left gaps of 80 to 370 ms between audio callbacks with SDL's 2048-frame buffer; the web build asks
for 4096 frames (85 ms at 48 kHz, `SDL_HINT_AUDIO_DEVICE_SAMPLE_FRAMES`), after which none was measured over 30 s
of play (since brought back to 2048, below). The browser's sample rate is used as is (SDL opens the device at it and converts the game's mix). An
AudioWorklet would need the game's mixer in another thread, so pthreads.

With 85 ms buffers, a sample started between two callbacks used to begin at the next buffer's start: a machine gun
firing every 100 ms (3 steps) was heard at 85, 85, 85, 85, 171 ms, faster with a hole, and a restart in the same
window dropped the first shot. `compat/src/miles.cpp` now times each start and stop and places it at the same point
of the next buffer, for a constant delay of one buffer: the shots are heard 100 ms apart, as natively (measured with
`?soundlog=2` and `GTA2_SOUND_LOG=2`, the same weapon held 15 s).

That delay is one buffer, so the buffer went back down once the page's thread had no more stalls (the clock busy
wait, the `?stats` GPU query, half the GL calls). `?debug` counts the audio callbacks that came after their buffer
was due (`late`) and the least margin (`slack`); `?audioframes=N` sets SDL's hint for a session (SDL doubles it).
Measured over 150 s of play each, headless at 60 Hz: 4096 frames, 0 late, margin 85 ms; 2048, 0 late, 37 ms;
1024, 0 late, 11 ms. On the real display at 120 Hz, 2048 frames: 0 late, margin 40 ms. The default is 2048 frames
(43 ms at 48 kHz): 1024 leaves too little margin for a phone. Natively the delay is one device buffer: PipeWire
gives SDL 1024 frames at 44.1 kHz, 23 ms.

### Short key presses

The game reads the keyboard once per logic step in a game (DirectInput's buffered events, applied in order) and
its state once per frame in the menus. In the original, as in the port before, a tap shorter than that interval was
lost: the press and the release cancel out in one read, or both fall between two reads of the state. The port's
DirectInput (`compat/src/dinput.cpp`) now shows a pressed key in the state once, and holds a release back to the
next pass of buffered reads when its press was given in the same pass. This is the same on the desktop builds.
Touch buttons still hold 50 ms: SDL reads the virtual pad's state once a frame.

### The ImGui debug window

The log's "ImGui debug enabled" comes from the original's hook (`LoadBeginSceneCBPtr` in `Source/winmain.cpp`); in
every port build the hooked function is an empty stub (`port/src/debug_stubs.cpp`), so no debug UI exists to reach.

### Settings

Hidden on the web (`port/src/settings.cpp`, one `#if defined(__EMSCRIPTEN__)` per list): FULL SCREEN (the page
owns it), DISPLAY MODE, VSYNC, 3D SOUND (no OpenAL). CHECK FOR UPDATES is not built (`GTA2_UPDATE_CHECK` off).
The page keeps only what the game's OPTIONS can't have, in `localStorage`: the picture, and on a touch screen the
touch controls' mode, size and opacity. The picture setting sets the canvas' pixel ratio: the page overrides
`devicePixelRatio`, which SDL reads for a `SDL_WINDOW_HIGH_PIXEL_DENSITY` window. Automatic keeps the canvas under
2.4 million pixels on a computer and 1.3 million on a phone. Phones also get at most 2x anti-aliasing
(`GTA2_MSAA_CAP`). Saves and data (export, import, delete) sit in one closed disclosure. Once the game is imported,
the page shows Play and little else: the introduction is for the first visit.

The footer's legal line is written once, in the landing page (`<p class="legal">` in `home/index.html`), and
`build.sh` puts it in the game page's footer (`<!--@LEGAL@-->`); `.foot` is in `ds.css`. It stays in English.

Icons are inline SVG in the text's colour (`.icon`, `.icon.line` in `ds.css`): the Apple and Linux marks from Simple
Icons (CC0), a four-square Windows mark drawn for the site, and Lucide's line icons (ISC); `home/icons-licenses.txt`
lists them, and `build.sh` adds it to the game page's `licenses.txt`.

### Sharing the landing page's design

The page uses the site's design system, `port/web/ds.css` (tokens, fonts, buttons, panels; dark only), which
`build.sh` copies into the game's site next to its fonts: one source file, and the service worker, whose scope is
`/play/`, keeps it for offline starts. `site/style.css` only has what the game page adds (the import, the
settings, the game's overlays and touch controls).

## Edge cases

| Case | Handling |
|---|---|
| No WebGL 2, WebAssembly, IndexedDB or workers | The page lists what is missing and stops before the import |
| Private browsing or storage blocked | Detected by a write test: the page says storage is needed |
| Storage full (import or save) | Quota errors say so; an import that fails leaves the data marked incomplete |
| Import interrupted (tab closed, cancelled) | The install record stays incomplete; the next visit asks for the copy again |
| Wrong file, incomplete copy, no music, no movie | The importer's report: missing files block the import, missing music or movie only warn |
| Two tabs | Web Locks: the second tab offers to take over; the first one stops its game |
| Tab hidden, phone call, notification | The pause menu opens, sound is suspended, files are stored, the wake lock is released |
| Leaving during a game | The browser asks to confirm (only in a game, not in the menus) |
| Page resized, full screen entered or left | The game switches to the canvas' new size 200 ms after it settles, as the DISPLAY setting does |
| QUIT GAME, SAVE AND QUIT, game over | Back to the game's menus, as on the desktop |
| The start menu's QUIT | The game hands over to the page (`port_WebQuit`), which stores the files and shows itself again |
| Crash or abort | A message with Reload and Copy details (version, browser, memory, the last 60 log lines); only for a trap in the game's code, an abort or an error in `reg2.js` |
| A browser refusing a request (pointer lock just after Escape, full screen, wake lock, orientation, audio) | Logged, never fatal; pointer lock is asked again at the next click |
| WebGL context lost | Files are stored, a message offers a reload; the game's textures are not rebuilt in place |
| Portrait phone | A "turn your device" overlay; the game pauses behind it; `screen.orientation.lock` is tried in full screen |
| iPhone (no full screen API) | A hint to Add to Home Screen; the manifest asks for full screen and landscape |
| Autoplay policy | Sound starts after the Play click; SDL resumes the audio context on input |
| Pause and full screen buttons with the pointer locked (TWIN STICK's mouse aim) | The page reads the game's cursor (`port_WebCursorX/Y`), marks the button under it red and takes a click there for the button, not the game |
| Pinch zoom, double tap, pull to refresh, selection, long press | `touch-action: none`, `overscroll-behavior: none`, no selection or callout on the game |
| Browser shortcuts | F1 to F10, Backspace and Tab go to the game; in full screen, Escape goes to the game (Keyboard Lock) |
| Safe areas (notch, home bar) | `viewport-fit=cover` and `env(safe-area-inset-*)` for the page and the touch controls; the game's SAFE AREA setting for its HUD |
| High-DPI and very large screens | The picture setting's pixel budget |
| Very small screens | The page is one column; the game's view widens or narrows with the canvas |
| Reduced motion | No transitions on the page |
| Photosensitivity | The game's BLOOM setting (GRAPHICS) |
| Offline | The service worker serves the build; the game data is in IndexedDB |
| New build | The usual service worker shape (`site/sw.js`): the page and its small scripts from the network first (3 s at most, else the cache), the game in a `build-<hash>/` folder from the cache first, a new worker taking over at once (skipWaiting, clients.claim). A new tab gets the new build; a tab already playing keeps the game it loaded in memory; a tab opened before the new build reloads once when it finds its build folder gone; offline, the cached build starts |

## Tests

`port/web/tests/e2e.mjs` drives the site with Playwright (Chromium and Firefox), with the player's own GTA2 as
input (`GTA2=...`; nothing of it is in the repository): the import, a game started and walked in, the pause
menu's SAVE AND QUIT then a reload and LOAD GAME, the WebGL context loss (`WEBGL_lose_context`), an offline
reload, two tabs, the tab hidden in a game, and a phone-sized viewport with touch (the menus, the stick). Its
header says how to run it. Screenshots go outside the repository (they show the game).

Not tested: a real iPhone or Android phone, Safari, real gamepads in a browser, real phone GPUs' frame times.

## Possible improvements

- The import's unpacking in 2 to 4 workers, each extracting a subset of the files: at most about 1.5 to 2 times
  faster in total (the reads and the storing stay serial), for more memory. A phone takes about 40 s for the
  free installer as it is.

## Inspirations

- OpenCE (OpenCommunityEdition/OpenCE), Halo CE in the browser: the service worker that keeps one build per
  cache and switches when the page allows, the touch controls as a gamepad the game reads, the import done by the
  page in a worker, the warnings about WebGL feedback loops and LTO.
- Libraries rather than custom code where one fits: idb-keyval (storage), fflate (save
  export and import as a zip). The formats of the game's copy are read by the native setup's own code
  (libarchive, unshield), which also covers InstallShield, for which no JavaScript library exists.
