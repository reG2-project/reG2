# GTA2 port

Native builds of the decompiled game with clang, 64-bit and 32-bit. Plan: `docs/port_plan.md`.

| Build | Platform layer | Toolchain file |
|---|---|---|
| Linux (x86_64) | SDL3, OpenGL ES 3.0 (or OpenGL 3.3 core), built-in renderer, sound and input ([docs/64bit.md](docs/64bit.md)) | `toolchain-linux64-clang.cmake` |
| Linux (i686) | the same, with the host's 32-bit libraries | `toolchain-linux32-clang.cmake` |
| Windows (MinGW, x86_64) | the same as Linux | `toolchain-mingw64-clang.cmake` |
| Windows (MinGW, i686) | the same as Linux; `-DGTA2_PORT_DIRECTX=ON` for DMA's DirectX, Miles and Bink DLLs | `toolchain-mingw32-clang.cmake` |
| macOS (universal: arm64 and x86_64) | the same as Linux, OpenGL 3.3 core; makes `reG2.app` (untested) | none (Xcode's clang) |

## Windows

Needs SDL3 for MinGW (`SDL3-devel-<version>-mingw` from the SDL releases), and libarchive and
unshield (see [Setup libraries](#setup-libraries)). `SDL3.dll` is copied next to `reg2.exe`; the exe
needs no DirectX, Miles or Bink DLL.

```bash
cmake -S port -B build_port -G Ninja -DCMAKE_TOOLCHAIN_FILE=$PWD/port/toolchain-mingw32-clang.cmake \
      -DSDL3_DIR=<SDL3-devel>/i686-w64-mingw32/lib/cmake/SDL3 -DGTA2_SETUP_DEPS_DIR=<prefix> \
      -DCMAKE_BUILD_TYPE=RelWithDebInfo
ninja -C build_port
```

For x64, `toolchain-mingw64-clang.cmake` and the SDK's `x86_64-w64-mingw32` folder.

## macOS

Not tested yet. `packaging/deps.sh macos` builds the libraries (SDL3, OpenAL Soft, libarchive, unshield)
for arm64 and x86_64 in one go, `packaging/build.sh macos` the universal app, `packaging/macos_app.sh` the .dmg
(the `macos` job of `.github/workflows/port.yml`, on an Intel runner). `lipo` checks that every library and the
executable hold both architectures. The OpenGL declarations come from `renderer/khronos`, the
Khronos headers, which macOS lacks.

## Linux

Needs clang, lld, ninja, cmake, 32-bit SDL3 (`lib32-sdl3` on Arch), libarchive and unshield (see
[Setup libraries](#setup-libraries); `-DGTA2_SETUP_DEPS_DIR=<prefix>` where they are not on the system).

```bash
cmake -S port -B build_linux -G Ninja -DCMAKE_TOOLCHAIN_FILE=$PWD/port/toolchain-linux32-clang.cmake -DCMAKE_BUILD_TYPE=RelWithDebInfo
ninja -C build_linux
```

On its first start the port asks for the game's files (see [Game data](#game-data-and-first-start)).
A developer can also run it from a copy of the game folder, or point `GTA2_DIR` to one:

```bash
GTA2_DIR=~/games/gta2 ./build_linux/reg2
```

Settings live in `gta2_registry.ini` (the registry keys of the Windows game). When its `language` is unset or
has no `data/<code>.gxt`, the start picks the first of the system's preferred languages that the game has, else
English, else any language there. Alt+Enter switches
between window and full screen, and is no Enter for the game or the menus. Other keys pressed with Left Alt
(LOOK AROUND by default) act as usual: the original dropped Space, Tab and Enter while it was held. Every gamepad drives the player (only those whose name contains
`GTA2_GAMEPAD` if set): a button counts on any of them, the sticks are those of the pad used last.
Bind them in the game's control options (stick and d-pad are the two axes).
In a vehicle, with FORWARD and BACKWARD on the pad's up and down (the default), RT accelerates, LT brakes
or reverses, both as far as they are pressed, the left stick only steers, more gently when pushed part
way, and the d-pad works as on foot; LOOK AROUND (the camera pan, LT
by default) is then on L3, unless a control uses it.
The menus, the pause menu too, move with the arrows or WASD by their position (ZQSD on AZERTY), whatever the
controls, except while a player name is typed.
They also take the mouse: pointing selects, a click enters or changes a value, the right
button goes back, the wheel changes a value; the cursor shows while the mouse is in use over a menu.
Each control has a key, a second key (ALTERNATE: a key, a mouse button or a wheel notch, by default
WASD by their position (ZQSD on AZERTY) to move, F to enter or exit, E for the horn, the left button to attack
and the wheel to change weapons; Delete clears it) and a gamepad input. F1 to F12 cannot be bound: they keep their
own uses (F7 the brief, F9 the zone name, F10 the network chat).
The original's SPECIAL 1 is HORN OR TAUNT (C by default), SPECIAL 2 is LOOK AROUND, and SPECIAL 3,
which the game never reads, is not listed but keeps its bindings. Saved bindings keep their keys.
CONTROLS on the controls page chooses how the player walks on foot: CLASSIC, the original's tank
controls, or TWIN STICK, which moves and aims apart, both relative to the screen.
The left stick, the d-pad or the movement keys walk; the right stick aims (its reticle
further out the more it is pushed, up to the screen's edge, and held a moment once released), or the mouse does (confined to the window,
a reticle in place of the cursor). RT attacks (held from driving or getting in, once pressed again), the other controls stay as bound. Without an aim the player faces the
way he walks; with one he turns to it and walks any way at the same speed (backward with the run
animation, as the original's back key; his shots, flames and throws take on only the part of his walk toward the aim,
as the original's forward walk does, so walking sideways or backing off leaves them where a standing throw goes). Walking off the aim, his steps shorten the
more the move is to the side, down to none at 90 degrees, where his legs stay under him, his back rounded, and he
bobs a little along the move; further back his walk plays backward, his legs turned along the move, the one ahead
longer than the one behind. STRAFE ANIMATION (on by default, experimental) draws these legs; off, he runs with the
original's animation whichever way he walks. In a jump the move steers him toward its way, as fast as the
original's left and right turn him in the air. The machine guns fire straight along the aim: the original bends
their shots toward a ped up to 16 degrees off the line of fire. The electro weapons keep their lock on a target.
Peds standing to shoot at him with a pistol or a machine gun lead him by a part of his move while their bullets
fly, drawn between 0.4 and 0.9 every half second: walking sideways still dodges most shots, no longer all.
AIM CAMERA (on by default) leads the view a little ahead of the player on foot, as Hotline Miami does: 0.3 of
the half screen height along the aim (the mouse's direction from him or the right stick's), however far the
cursor or the reticle is, and along the last aim once the stick is released; held, left Shift, LT (instead of
LOOK AROUND, while that keeps its default LT) or R3 (unless a control is bound to it) looks toward the cursor or
the reticle, as far as keeps the player on screen.
In vehicles the controls stay classic, but the fire truck's, the tank's and the gun jeep's roof gun turns
toward the mouse or the right stick's reticle at the original's turret speed; ATTACK fires it as before, RB
too (instead of NEXT WEAPON, while that keeps its default RB), and HORN OR TAUNT with left or right still turns it
while held. The stick and mouse are not recorded in replays.
VIBRATION (on by default) shakes the pad used last when the player is hit on foot, at an explosion within a few
blocks (more the nearer it is) and when the car he drives crashes (more the harder the hit), never in the menus or the pause.
The intro movie (`data/Movie/intro.bik`, after `preintro.bik` in the editions that have one) plays as in
the original, and any key or a pad button that works in the menus skips it. libbink decodes it at the file's
frame rate and shows it in true colour, letterboxed, whatever the renderer settings; its sound goes through
the game's mixer. The start turns `do_play_movie` on once, as soon as the game has the intro; set to 0
afterwards, it stays. A movie that cannot be decoded is skipped.
Renderer options are in `gta2gl.ini` (`renderer/gta2gl.ini.example`);
with the debug setting `do_debug_keys`, Scroll Lock toggles them.

## Game data and first start

The port does not include GTA2's files. It runs in the first of these folders that holds the game's
data (`data/fstyle.sty`), and writes its settings and saves there:

1. `GTA2_DIR`, if set;
2. the current folder, then the executable's folder (a game folder the port was copied into);
3. the per-user folder, which the setup fills. Packages (the Linux ones, a macOS bundle, an
   installed Windows build) always end up here: next to the executable only an error's log is written.

| Platform | Per-user folder (`SDL_GetPrefPath`) |
|---|---|
| Linux | `$XDG_DATA_HOME/reG2/`, by default `~/.local/share/reG2/` (Flatpak: `~/.var/app/<app id>/data/reG2/`) |
| Windows | `%APPDATA%\reG2\` |
| macOS | `~/Library/Application Support/reG2/` |

It holds `data/` (the game's data, the music in `data/GTAudio/`), `gta2_registry.ini` (settings),
`gta2gl.ini` (renderer options), `player/` (saves and high scores) and `test/` (replays, logs).

After an error message or, on Windows, a crash, `reG2.log` (a network server's: `reG2-server.log`; the start-up steps, the OpenGL driver,
errors, message boxes and the crash report) is written next to the executable when that folder is
writable, else in the per-user folder, and the message gives its path. A run without an error
writes no log.

Without the data there, a message box names what the setup reads (`kSourceKinds` in `setup/install.hpp`)
and offers three choices:

- **Use an existing install:** a GTA2 folder (the one holding `data`, or `data` itself). Its
  `data` folder is copied, with `gta2_registry.ini`, `gta2gl.ini` and `player/` if the port ran there.
- **Install from a file:** the game's installer or a disc image, read by the port (nothing is run).
- **I don't have the game:** a short text (`kNoGameText` in `setup/setup.cpp`) and a link to the
  Internet Archive item [gta2_rockstar-classics](https://archive.org/details/gta2_rockstar-classics), which
  holds the 2004 Rockstar Classics freeware installer alone (`GTA2INSTALLER.exe`, the same file as the
  `GTA2.exe` Rockstar gave away).

Before anything is copied, a message lists what the chosen copy holds, each marked `[x]` or
`[ ]`: the game files (the essential ones are listed in `setup/install.cpp`), the music, the intro
movie and the five languages (`data/<code>.gxt`; the 2004 freeware has only English and Spanish; at
least one is needed). It comes from the source's list of files: of InstallShield cabinets only
`data1.hdr` is extracted (unshield lists their files from it). **Install** then copies the files into
a temporary folder, checks them, moves them into place and starts the game. Without the music (the 22
tracks of `GTAudio` on the original CD) or the intro movie the message warns and still offers
Install; the music can be added later from a folder (the CD, its `GTAudio`), a disc image or the
installer. Without an essential file it offers only **Choose another...** and **Quit**. `reg2 --setup`
lists the installed data the same way, to replace it or add the music: a source with only the music
or the movie (a `GTAudio` folder) is added to a complete install.

| Source | Read with |
|---|---|
| A game folder, or its `data` or `GTAudio` folder | |
| InstallShield setup `.exe` (the 2004 Rockstar Classics `GTA2.exe`): its files follow the PE image | `setup/install.cpp`, then unshield |
| InstallShield cabinets (`data1.hdr`, `data*.cab`: the original CD), in a folder or a disc image, with the CD's `GTAudio` and `Movie/intro.bik` beside them | unshield |
| Disc images (`.iso`, Joliet names), `.zip`, `.7z` and the other formats of libarchive | libarchive |

Other installers (NSIS, Inno Setup) are not read: install the game with them, then use its folder.
The original 1999 CD is read as a folder (the disc, or a copy of its files): libarchive refuses its
`.iso` (the primary volume descriptors carry Joliet escape sequences), and raw `.mdf`/`.bin` images
(2352-byte sectors) are not read.

Message boxes are SDL's: task dialogs on Windows, `NSAlert` on macOS. On Linux, zenity if installed
(GTK dialogs), else SDL's own X11 box; when neither is available (Wayland without XWayland) the setup
logs the question and quits, and `GTA2_SETUP_PATH` can name the source. The folder and file pickers are SDL's (the
xdg-desktop-portal on Linux, else zenity). The progress is a small SDL
window; closing it cancels the copy.

## Welcome screen, options and saves

The first start shows CHOOSE YOUR STYLE before the start menu. CLASSIC is the game as it was: its
look, its controls and church saves only. ENHANCED, selected at first, turns on the new lights and
shadows, the day and night cycle, twin stick controls, AUTOSAVE, RETRY JOBS and FRENZY RESPAWN. Pick one with the
keys, the mouse or a pad (Left and Right, or LB and RB, as the hint under it shows); every setting can
be changed later. `GTA2_WELCOME=1` shows the screen again.

OPTIONS has five pages: GAME (language, text speed, AUTOSAVE, RETRY JOBS, FRENZY RESPAWN, INFINITE LIVES, blood, lighting and the
day and night cycle), GRAPHICS, DISPLAY (full screen, frame rate, zoom, HUD), SOUND and CONTROLS.
On GRAPHICS, AMBIENT OCCLUSION (`gl_ambient_occlusion`, on in ENHANCED) softly shades inner corners and the foot of
walls, worked out from the map's blocks, in every LIGHTING mode.
FIRE EFFECTS (`gl_fire_effects`, on in ENHANCED) adds heat haze over the flames and a plume of smoke, drawn from the
game's own smoke frames and lit by the fires, with a few embers, over burning cars, wrecks and fires.
TEXTURE FILTERING (`gl_texture_filter`) is BILINEAR as the original, ANISOTROPIC (mipmaps, on in ENHANCED),
PIXELATED: the game's textures drawn as sharp squares of texels, the menus left smooth, or SHARP BILINEAR: the same squares with their edges blended over one screen pixel, so they keep an even size as the camera moves.
Their settings are grouped under small titles, and the less common ones say under the page what they
do, or what their value does (PRESET, LIGHTING, FRAME LIMIT, CONTROLS, AIM CAMERA, STRAFE ANIMATION).
In a game, Escape opens the same menu as a pause menu, with RESUME, RADIO (in a car with a radio: its station by
name, Left and Right or Enter to change it, as F1 and F2 did), STATISTICS (the original pause screen's lines),
MISSION TRANSCRIPT (the last mission messages, newest first), LOAD GAME and QUIT GAME. The hints under the pages show the keys, or once a pad is used its
buttons: PlayStation's symbols on a PlayStation pad, otherwise Xbox's pictures with the letters
printed on the pad (a Nintendo pad's bottom button is B), from SDL's pad type and button labels.
The bottom right corner shows the player's position in the map's blocks (X, Y and Z, his car's in one).

Saves are kept per city, in `player/`:

- A church save goes to `plyslotN.svg`, as in the original, and to a copy for its city.
- AUTOSAVE (`port_autosave`) saves the city after each job or kill frenzy, once the player is safe, and
  as he leaves it through the exit arrow (a few steps back from it). SAVING shows at the top of the
  screen, SAVE FAILED in red if the file could not be written; the game goes on. With it on, the pause
  menu's QUIT GAME offers SAVE AND QUIT, QUIT and CANCEL. SAVE AND QUIT writes the autosave, then quits
  (if the save fails, SAVE FAILED and the game goes on); it is greyed during a job or a frenzy, when
  wasted, busted or burning (NOT NOW), or with no firm ground to save on (NOT HERE). QUIT and closing
  the window save nothing.
- RETRY JOBS (`port_retry`) keeps a checkpoint taken as a job or a kill frenzy starts. After a failure,
  the pause menu offers RETRY JOB or RETRY FRENZY (RESTART while it runs), with the job's name, the
  money then and how long ago. Passing it, a church save or a new game deletes the checkpoint.
- FRENZY RESPAWN (`port_kf_respawn`): a failed kill frenzy is no longer marked started in the save, and a new skull
  and its trigger are back at once; standing on the skull starts it again. A vehicle frenzy's old vehicle locks the
  player out once he leaves it and despawns as traffic does, and a new one is parked with its trigger the first time no
  car is within 3 blocks of its spot and no player sees it. A passed one stays gone. Off in network games and bonus
  stages.
- INFINITE LIVES (`port_infinite_lives`, off in both styles): dying costs no life, so never GAME OVER, but still ends at
  the hospital; it sets the original's `do_infinite_lives` debug flag, at once in a game, and changes nothing in network
  games, which count no lives.
- LOAD GAME (the original's RESUME SAVED STATUS, on the Play, quit, dead and area complete pages)
  lists the saves, even a single one: a tab per city (Left and
  Right, Q and E, or LB and RB), each save with its money, jobs passed and date, newest first.
  The pause menu's LOAD GAME (not in a bonus stage or a network game) first warns that the progress since the
  last save will be lost; the game it leaves is not scored, as for RETRY.
- With AUTOSAVE on, a new game in a city that has an autosave warns first: YOUR DOWNTOWN AUTOSAVE
  WILL BE REPLACED, CONTINUE or CANCEL.

## Frame rate

The game logic always runs in the original's steps of 33 ms (11 ms in fast forward), 30.3 per
second, on a monotonic clock; the original's `max_frame_rate` (off: the game as fast as the
machine) is not read. The DISPLAY settings page chooses how often frames are drawn:

- FRAME LIMIT (`port_max_fps`): 30 ORIGINAL draws once after each step, as the original; 60, 120,
  144, 240 or UNLIMITED draw in between, with every sprite and the camera interpolated between the
  last two steps (`src/interpolation.cpp`). That shows the game one step (33 ms) late.
- VSYNC (`port_vsync`): adaptive where the driver has it. With it, above 30 ORIGINAL, the screen's
  refresh paces the frames: the port presents on every refresh, a new frame every whole number of
  refreshes nearest the frame limit (60 on a 144 Hz screen: every 2, 72 per second) and the last one
  again in between, so each frame stays on the screen equally long. Without it, or when the screen
  does not give its refresh rate, a timer paces them.
- DISPLAY MODE (`port_fullscreen_width`, `_height`, `_centihz`): full screen at the desktop's
  mode (borderless) or one of the screen's own modes (resolution and refresh rate).

After a stall the game catches up at most 4 steps before the next frame, then drops the rest.

## Network game

Client/server multiplayer on the game's multiplayer maps (6 players) and maps for up to 10, in place of the original's
DirectPlay game ([docs/netcode.md](docs/netcode.md)). NETWORK GAME in the start menu joins a server; from a game folder (macOS:
`reg2.app/Contents/MacOS/reg2`):

```bash
./reg2 --server --map downtown-6p --players 4         # a dedicated server, headless
./reg2 --connect 127.0.0.1 --name me                   # play
./reg2 --connect 127.0.0.1 --name bot1 --bot hunter    # a bot
```

To play over the internet the host runs the server, forwards UDP 27920 to its machine in its router, lets it through its
firewall and gives the others its public address ([docs/netcode.md](docs/netcode.md#hosting-a-game), "Hosting a game"; on a
LAN, its LAN address). Everyone needs the same reG2 build and game data.

`ctest --test-dir <build folder> -L net` runs the netcode's unit tests (`-DGTA2_GAME=OFF` configures only those, without
SDL3), and `BUILD=<build folder> GAME=<game folder> net/scripts/net_check.sh` servers and bot clients on this machine
(about 6 minutes).

## 3D sound (OpenAL Soft)

The game's 3D sound option (`do_3d_sound` under `Sound` in `gta2_registry.ini`) uses
[OpenAL Soft](https://github.com/kcat/openal-soft) (LGPL), loaded at run time from next to the exe
(`libopenal.so.1`, `OpenAL32.dll`): positional sound with DirectSound3D's distance model and the
EAX environment the game picks, as EFX reverb. OpenAL renders into the SDL mix through its loopback
device. Without the library the game keeps its 2D sound. `GTA2_HRTF=1` renders binaurally for
headphones; `GTA2_NO_OPENAL=1` ignores the library.

On Linux without `GTA2_OPENAL_DIR`, the build uses the system's OpenAL Soft headers when it finds
them (`AL/efx-presets.h`, for example from the distribution's `openal` package) and the game loads
the system's `libopenal.so.1`, which must match the build's architecture.

Build it once per target (no audio backend needed), then pass its install prefix to the port:

```bash
OPTS="-DALSOFT_UTILS=OFF -DALSOFT_EXAMPLES=OFF -DALSOFT_NO_CONFIG_UTIL=ON -DALSOFT_ENABLE_MODULES=OFF \
      -DALSOFT_INSTALL_CONFIG=OFF -DALSOFT_INSTALL_HRTF_DATA=OFF -DALSOFT_INSTALL_AMBDEC_PRESETS=OFF \
      -DALSOFT_BACKEND_PIPEWIRE=OFF -DALSOFT_BACKEND_PULSEAUDIO=OFF -DALSOFT_BACKEND_ALSA=OFF \
      -DALSOFT_BACKEND_OSS=OFF -DALSOFT_BACKEND_JACK=OFF -DALSOFT_BACKEND_SNDIO=OFF -DALSOFT_BACKEND_SDL3=OFF \
      -DALSOFT_BACKEND_WASAPI=OFF -DALSOFT_BACKEND_DSOUND=OFF -DALSOFT_BACKEND_WINMM=OFF -DALSOFT_BACKEND_WAVE=OFF \
      -DHAVE_WFUNCTION_EFFECTS=OFF -DCMAKE_BUILD_TYPE=Release"
# Linux
cmake -S openal-soft-1.25.2 -B al_linux -G Ninja -DCMAKE_TOOLCHAIN_FILE=$PWD/port/toolchain-linux32-clang.cmake \
      -DCMAKE_C_FLAGS=-m32 -DCMAKE_CXX_FLAGS=-m32 -DCMAKE_INSTALL_PREFIX=$PWD/openal_linux32 $OPTS
ninja -C al_linux install
# Windows: also -DALSOFT_EAX=OFF and a self-contained DLL
cmake -S openal-soft-1.25.2 -B al_mingw -G Ninja -DCMAKE_TOOLCHAIN_FILE=$PWD/port/toolchain-mingw32-clang.cmake \
      -DALSOFT_EAX=OFF "-DCMAKE_SHARED_LINKER_FLAGS=-fuse-ld=lld -static -static-libgcc -static-libstdc++ -lpthread" \
      -DCMAKE_INSTALL_PREFIX=$PWD/openal_mingw32 $OPTS
ninja -C al_mingw install
```

Then add `-DGTA2_OPENAL_DIR=$PWD/openal_linux32` (or `openal_mingw32`) to the port's cmake line.
`-DHAVE_WFUNCTION_EFFECTS=OFF` works around recent clang rejecting OpenAL Soft 1.25.2's
`-Werror=function-effects`.

## License

The port's own code in `port/` is under the MIT license (`port/LICENSE`), except the third-party code
below, under its own license. The rest of the repository comes from gta2_re, which has no license.

## Third-party code

| Code | Version | License | Use |
|---|---|---|---|
| [SDL 3](https://libsdl.org) | 3.4 | zlib | platform layer (linked) |
| [libbink](https://github.com/raynorpat/libbink) | 73d884f4 | LGPL 2.1 | the movies' Bink 1 decoder (`3rdParty/libbink`, built in, static) |
| [OpenAL Soft](https://github.com/kcat/openal-soft) | 1.25.2 | LGPL | 3D sound (loaded at run time, optional) |
| [libarchive](https://github.com/libarchive/libarchive/releases/tag/v3.8.11) | 3.8.11 | BSD | setup: disc images and archives (linked) |
| [unshield](https://github.com/twogood/unshield/releases/tag/1.6.2) | 1.6.2 | MIT | setup: InstallShield cabinets (linked) |
| [ENet](http://enet.bespin.org) | 1.3.18 | MIT | the network game (vendored in `net/enet`) |
| [zlib](https://github.com/madler/zlib/releases/tag/v1.3.2), [xz](https://github.com/tukaani-project/xz/releases/tag/v5.8.4) | 1.3.2, 5.8.4 | zlib, 0BSD | their compression, where the system has none (MinGW) |

libbink is linked statically, so the LGPL asks that its users can rebuild the game with a changed copy:
the port's source, which holds it, is public, and the packages carry its licence.

The key and button pictures (`resources/prompts_*.png`, made by `resources/prompts_atlas.py`) are from
[Gamepad UI / Controller Prompts Pack](https://greatdocbrown.itch.io/gamepad-ui) by greatdocbrown (CC0).

### Setup libraries

The setup links libarchive and unshield. Distributions and Homebrew have libarchive; unshield is
packaged by most distributions and Homebrew.
CMake finds them on the system, or in `GTA2_SETUP_DEPS_DIR`, an install prefix with both built
static (and, for MinGW, zlib as `libzs.a` and liblzma). Arch has no 32-bit libarchive or unshield;
build them from their releases with the port's toolchain files, for example:

```bash
P=$PWD/setup_deps    # then -DGTA2_SETUP_DEPS_DIR=$P
TC="-G Ninja -DCMAKE_TOOLCHAIN_FILE=$PWD/port/toolchain-linux32-clang.cmake -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_INSTALL_PREFIX=$P -DCMAKE_C_FLAGS=-m32"
cmake -S unshield-1.6.2 -B b_unshield $TC -DBUILD_STATIC=ON -DCMAKE_DISABLE_FIND_PACKAGE_OpenSSL=ON
ninja -C b_unshield install
# The static library leaves out unshield's MD5 objects.
llvm-ar rs $P/lib/libunshield.a b_unshield/lib/md5/CMakeFiles/md5.dir/md5c.c.o
cmake -S libarchive-3.8.11 -B b_archive $TC -DBUILD_SHARED_LIBS=OFF -DPOSIX_REGEX_LIB=NONE -DENABLE_TEST=OFF \
      -DENABLE_TAR=OFF -DENABLE_CPIO=OFF -DENABLE_CAT=OFF -DENABLE_UNZIP=OFF -DENABLE_OPENSSL=OFF \
      -DENABLE_CNG=OFF -DENABLE_LIBXML2=OFF -DENABLE_EXPAT=OFF -DENABLE_ZSTD=OFF -DENABLE_LZ4=OFF \
      -DENABLE_BZip2=OFF -DENABLE_ACL=OFF -DENABLE_XATTR=OFF -DENABLE_ICONV=OFF -DENABLE_LIBB2=OFF
ninja -C b_archive install
```

For MinGW, first build zlib (`-DZLIB_BUILD_SHARED=OFF`) and xz (`-DBUILD_SHARED_LIBS=OFF -DXZ_ASM_I386=OFF`)
into the same prefix, then pass `-DZLIB_LIBRARY=$P/lib/libzs.a -DZLIB_INCLUDE_DIR=$P/include
-DLIBLZMA_LIBRARY=$P/lib/liblzma.a -DLIBLZMA_INCLUDE_DIR=$P/include -DCMAKE_C_FLAGS=-DLZMA_API_STATIC`
to unshield and libarchive.

## Packages

`.github/workflows/port.yml` builds and tests the packages on pushes to `main` and `port/**` branches, on
`v*` tags (a draft release of the 64-bit packages) and on demand. On Linux each is `packaging/ci.sh` in a
Debian 13 container, which installs the tools, downloads the sources pinned in `packaging/versions.sh`, builds
the libraries (`packaging/deps.sh`), the port (`packaging/build.sh`), then the packages. The mingw64 build
turns on clang's `-Wshorten-64-to-32` and fails on a 64-bit value truncated into a `long`, 32 bits on
Windows and 64 on Linux (lengths, `size_t` and `ptrdiff_t`, aside). The same runs locally, from the
repository's root:

```bash
docker run --rm -v $PWD:/src:ro -v $PWD/../work:/work -w /src debian:trixie sh port/packaging/ci.sh linux64 /work
docker run --rm -v $PWD:/src:ro -v $PWD/../work:/work -w /src debian:trixie sh port/packaging/ci.sh mingw64 /work
docker run --rm -v $PWD:/src:ro -v $PWD/../work:/work -w /src i386/debian:trixie linux32 sh port/packaging/ci.sh linux32 /work
docker run --rm -v $PWD:/src:ro -v $PWD/../work:/work -w /src debian:trixie sh port/packaging/ci.sh mingw32 /work
# A Linux package in a clean container; a GTA2 folder as a second argument also installs and plays the game.
docker run --rm -v $PWD/../work/out:/out:ro -v $PWD/port/packaging:/p:ro ubuntu:24.04 sh /p/smoke_test.sh /out/reG2-linux-x86_64.tar.gz
# The Flatpak bundle, and its test on a clean Debian 13 (bwrap needs these options in a container).
BWRAP="--security-opt seccomp=unconfined --security-opt apparmor=unconfined --security-opt systempaths=unconfined"
docker run --rm $BWRAP -v $PWD:/src:ro -v $PWD/../fpwork:/work -w /src \
    ghcr.io/flathub-infra/flatpak-github-actions:freedesktop-26.08 sh port/packaging/flatpak.sh /work
docker run --rm $BWRAP -v $PWD/../fpwork/out:/out:ro -v $PWD/port/packaging:/p:ro debian:trixie \
    sh /p/flatpak_smoke_test.sh /out/reG2-x86_64.flatpak
```

The releases are 64-bit:

- Windows: `reG2-windows-x64.zip`, with `reg2.exe`, `SDL3.dll`, `OpenAL32.dll`, this README and
  the libraries' licences. Built with llvm-mingw on UCRT, part of Windows 10 and later. The exe with
  its debug information is `work/symbols/reg2-x64.exe` (the `symbols-*` artifact): the frames of a
  crash report in `reG2.log` are module+offset, and the exe's are found with
  `llvm-symbolizer --obj=reg2-x64.exe -f -C 0x<0x140000000 + offset>`.
- Linux, portable: `reG2-linux-x86_64.tar.gz`. Extract it anywhere and start `./reg2`. It brings
  SDL 3 and OpenAL Soft, and takes glibc (2.29 or later), OpenGL, X11, Wayland and sound from the system,
  so it uses the system's GPU driver. `packaging/linux/README-x86_64.md` is its README; `./reg2` says which
  libraries are missing.
- Linux, AppImage: `reG2-x86_64.AppImage`, the same build and libraries in an AppImage
  (`packaging/appimage.sh`, the type2 runtime), with its desktop file, icon and AppStream metainfo.
- Linux, Flatpak: `reG2-x86_64.flatpak`, a bundle of `packaging/flatpak/<app id>.yml`
  on the `org.freedesktop.Platform` 26.08 runtime, whose SDL 3, OpenAL Soft and libarchive it uses
  (`flatpak install --user reG2-x86_64.flatpak` fetches the runtime from Flathub). It needs no
  access to the files: the setup's choosers go through the desktop portal, and the game's data goes to
  `~/.var/app/<app id>/data/reG2/`.
- macOS: `reG2-macos.dmg`, with a universal `reG2.app` (arm64 and x86_64; SDL 3 and OpenAL Soft in
  `Contents/Frameworks`) for macOS 11 and later, built on GitHub's Intel macOS runner (`packaging/macos_app.sh`).
  The app is signed ad hoc, not notarized: the first start needs a right click, Open (or
  `xattr -dr com.apple.quarantine reG2.app`). Not tested on a Mac yet.

The file names carry no version, so `releases/latest/download/<name>` always gives the newest. The version
is CMake's `GTA2_VERSION` (by default `0.1.0-dev`; `build.sh` and `flatpak.sh` take `$GTA2_VERSION`, a tag
such as `v0.2.0` in CI): the start menu's corner, the Windows file version, the macOS bundle's, and the
AppStream release, dated the day it is packaged.

To release:

1. Actions, Release, Run workflow, on `main`, with the version (`0.2.0`). It tags the commit `v0.2.0` and
   starts the Port workflow on the tag, which builds that version.
2. Once the Port run is green, check the draft release it made in Releases: the packages and the notes
   generated from the pull requests since the last release.
3. Edit the notes if needed, then Publish release.

The start menu looks for a newer release at most once a day (`curl`, else `wget`, on
`api.github.com/repos/reG2-project/reG2/releases/latest`), and shows it beside the version: a click opens the
release's page. OPTIONS, GAME, CHECK FOR UPDATES turns it off; `-DGTA2_UPDATE_CHECK=OFF` leaves it out, as in the
Flatpak. `GTA2_UPDATE_URL` replaces the API's URL, for tests.

The executable is `reg2` (`reg2.exe`), so that it never replaces a GTA2 folder's `gta2.exe`. The app id of
the desktop file, metainfo, Flatpak and macOS bundle is set once, in `packaging/app.sh`; changing it also
renames `packaging/flatpak/<id>.yml` and its `id:` line.

The i686 packages (`reG2-windows-i686.zip`, `reG2-linux-i686.tar.gz`) still build and
are tested in CI, but are not released. The i686 Linux build takes everything but SDL 3 and OpenAL Soft from
the system's 32-bit libraries; `packaging/linux/README.md` lists them per distribution. With NVIDIA's
proprietary driver it needs its 32-bit libraries too, else Mesa draws on the CPU.

The Linux builds compile with Debian 13's clang (OpenAL Soft with its GCC) against a system root of Debian 11
(`packaging/sysroot.sh`, glibc 2.31; `packaging/linux32-sysroot.cmake`, `linux64-sysroot.cmake`), so they
start on distributions as old as that. The C++ runtime is linked statically; OpenAL Soft's needs a few glibc
2.32+ functions, which `packaging/glibc_compat.c` provides. `packaging/smoke_test.sh` installs a tarball or
the AppImage in a clean Debian, Ubuntu or Arch Linux container with the libraries it needs.

## Layout

- `compat/`: the Win32 subset the game calls, on SDL3 and POSIX (window, registry, files,
  DirectInput, Miles and its 3D provider on OpenAL Soft). Outside Windows, `compat/include` declares
  the Win32 and DirectX subset the game uses (Windows' 32-bit layouts and values); on Windows,
  MinGW's headers declare it and `win32_redirect.hpp` sends the game's calls to the same code.
- `renderer/`: the OpenGL ES renderer behind the game's `gbh_*` and `Vid_*` tables.
- `include/`: copies of headers whose originals don't build outside MSVC.
- `setup/`: the game folder and the first-start setup (copy, message boxes).
- `net/`: the network game ([docs/netcode.md](docs/netcode.md)); `src/net_hooks.hpp`, `src/rand/` and
  `src/port_globals.hpp` are what it needs from the game.

## Unattended runs

| Variable | Effect |
|---|---|
| `GTA2_INPUT=<frame>:<key>[:<frames held>],...` | key presses at given frames (SDL key names: `Return`, `Up`, `Left Ctrl`, `Alt+Return`, `Shift+1`, `Comma`), a virtual gamepad (`pad.a`, `pad.dpup`, `pad.lefty-`, `pad.rightx=0.5` for part of an axis; `pad2.` a second one, the first then idle; it logs the rumble it gets), or the mouse (`mouse.<x>x<y>` moves it to that window position, `mouse.left`, `mouse.right`, `mouse.wheelup`, `mouse.wheeldown`) |
| `GTA2_EXIT_AT=<frame>` | quit at that frame |
| `GTA2_WELCOME=1` | the first start's CHOOSE YOUR STYLE screen shown again |
| `GTA2_CURSOR_BMP=<file>` | the mouse cursor's image written to that BMP |
| `GTA2_AO_DEBUG=1` | AMBIENT OCCLUSION alone, in grey, on the surfaces it applies to |
| `GTA2_HAZE_DEBUG=1` | FIRE EFFECTS' heat haze: its heat shown as a colour ramp |
| `GTA2GL_CAPTURE=<dir>,<frame>,<a>-<b>,...` | write those frames as BMP (the first frame drawn at each); `GTA2GL_CAPTURE_FLIPS=1` counts presented frames instead |
| `GTA2_LOOP_STATS=1` | logic steps and frames per second on stderr, every 5 seconds |
| `GTA2_LOOP_TRACE=<file>` | the player's and the camera's position after each step (`S`) and as drawn (`D`), in a network game the other players' too (`p<slot>`); then the player's place its markers are drawn at against its sprite (`pedcam`), each exploding score's size (`score`) and how far objects on cars are drawn off their car (`attached`, blocks), and each burning car's flames (`flames <car sprite> <flame sprite>@<x>,<y>`) |
| `GTA2_TEST_SCORE=<step>,...` | an exploding score over the player at those steps (for the trace) |
| `GTA2_PRESENT_LOG=<file>` | every presented frame with the time it reached the screen (Wayland's presentation-time protocol; elsewhere when the swap returned), and every 10 seconds and at exit a summary: frame to frame times, how many refreshes each frame was shown, the shown time against the game time it depicts, on Wayland how many frames the compositor showed on vblank and scanned out directly, and the GPU time of each frame and how long before its presentation the GPU finished it |
| `GTA2_NO_SOUND=1` | no audio device |
| `GTA2_HEADLESS=1` | no window content, sound or gamepads (a network server is headless without `--window`) |
| `GTA2_SPEED=<k>` | the game's clock k times faster |
| `GTA2_NO_OPENAL=1`, `GTA2_HRTF=1` | no 3D sound; binaural 3D sound |
| `GTA2_LOG_FILE=<file>` | the log written to that file as the game runs, instead of `reG2.log` |
| `GTA2_NO_MESSAGEBOX=1` | message boxes only logged, never shown |
| `GTA2_TEST_CRASH=1`, `GTA2_TEST_CRASH=abort` | a null pointer write or `abort()` once the window exists, to try the crash report |
| `GTA2_AUDIO_DRIVER`, `GTA2_AUDIO_FILE` | SDL's audio driver and disk output file (wine hides `SDL_*` variables from Windows programs) |
| `GTA2_DEBUG_OUTPUT=1` | the game's debug strings on stderr and in the log |
| `GTA2_SETUP_PATH=<folder or file>` | the setup installs from it without asking; a source missing an essential file, or an error, still shows its message, unless `GTA2_NO_MESSAGEBOX=1` |
| `GTA2_SETUP_CAPTURE=<dir>` | the setup's progress window written there as BMP |

Frames count presented frames, except that while the game runs they count its logic steps, so a
script does the same at any frame rate. A run nobody sees, with the sound written to a file:

```bash
SDL_AUDIO_DRIVER=disk SDL_AUDIO_DISK_OUTPUT_FILE=/tmp/audio.raw \
GTA2_INPUT=100:Return,160:Return GTA2_EXIT_AT=900 GTA2GL_CAPTURE=cap,600 \
gamescope --backend headless -W 640 -H 480 -- ./build_linux/reg2
```
