# The network game

Client/server multiplayer for the port (`../README.md`), in the style of Quake 3 and its successors:

- an **authoritative server** runs the game and takes only the players' inputs;
- each **client predicts** its game ahead of the server, and on every snapshot rolls the whole world back, applies the
  server's state and runs the ticks since again (**whole-world rollback**);
- **snapshots** are deltas against what each client has acknowledged, per entity, cut by priority to a packet budget;
- **ENet** carries them: unreliable snapshots and commands, reliable messages for everything else.

It replaces the original's DirectPlay lockstep game. The game's own code runs unchanged: the netcode sits where
DirectPlay sat (`NetPlay`), and the few changes to `Source/` are port-only hooks ("The game's code").

What works: 1-10 players on the game's multiplayer maps, frags, points and tag games, police, traffic, trains and ambient peds,
joining and leaving mid-game, rounds with warmup, results and votes, map rotations, chat with commands, reconnecting,
a frontend page to join, bots (scripted and trained), recordings that replay without a server. Protocol 43.

The player limit is `NET_MAX_PLAYERS` (`core/player_limit.hpp`, 10), the port build's `MAX_PLAYERS`; the original and
the MSVC build have 6. Every map has the server's player slots ("Maps and players"). Each look a script gives a player has
its HUD colour and arrow from one table (`port/src/player_looks.hpp`): the original's, and three more looks (the police,
Elvis, the fireman) for 10 apart. What stays as the game has it: the police chase 4 criminals at once, the ambient
traffic and peds are 3 players' worth for everyone, and the respawns go round the map's restart zones (Residential has 4).

## Running

Build the port (`../README.md`), then from the game folder (or with `GTA2_DIR` pointing to one):

```bash
./reg2 --server --map downtown-6p --players 4         # a dedicated server, headless (--window to watch it)
./reg2 --connect 127.0.0.1 --name me                   # play with the keyboard or a pad
./reg2 --connect 127.0.0.1 --name bot1 --bot hunter    # a bot client (GTA2_HEADLESS=1: no window)
```

On macOS the program is `reg2.app/Contents/MacOS/reg2`. Without `--server` or `--connect` the game starts at its frontend,
where **Network game** joins a server (below). The command line (`port/net/cmdline.cpp` lists every option):

| | Options |
|---|---|
| Server | `--port` (27920), `--map <data/*.mmp>` (downtown-6p), `--maps a,b,c` (a rotation), `--players N` (1-10, 10: every map's slots), `--gametype 1-3` (frags, points, tag), `--fraglimit` (frags, 10; a points game's is money, 10000 as the original lobby's), `--timelimit` (minutes), `--tickms`, `--police [stars]` (off by default, as the lobby; up to the game's 1 star, or 1-6), `--traffic N` (ambient cars per player, 16), `--packets N` (a snapshot's parts at most, 4), `--interest blocks` (24) |
| Round flow | `--minplayers N` (2), `--warmup s` (60), `--results s` (15), `--votes start,map,kick` (or `none`), `--rounds on\|off` |
| Arena | `--arena brain,...` (bots on the server itself), `--ticks N` (the result printed and the server ended at tick N), `--seed N` |
| Client | `--connect host[:port]`, `--name`, `--bot [brain]` (simple, carhop, pulse, rammer, soldier, hunter, `nn:<file>`), `--replay <file>` (a client's recording, or a server's: that server again, "Recordings") |
| Either end | `--lag ms`, `--jitter ms`, `--loss %`, `--lossburst N`, `--spike ms:on:every`: a link conditioner on this end's sends (losses in bursts of N on average; ms more delay for `on` s every `every` s); `--mtu N`: the link's MTU (IP packets, 1280 by default, which fits the VPNs people play over: Tailscale 1280, WireGuard 1420, Hamachi 1404; 1428 the most) |
| Any | `--data <folder>` (the game folder, or its `data/`: else `GTA2_DIR`...), `--help` |

### Maps and players

The server's `--players` (10 by default) is how many play on every map. A map's script makes a ped for each player it
was made for (`PLAYER_PED`, `ScriptThread::Handle_PLAYER_PED`): that count, at most its `.mmp`'s `PlayerCount` when it
has one (`core/map_players.cpp`), is only shown, in the results' map list ("Downtown 6p") and in the server's warning
at start about the maps it plays that were made for fewer players than its slots. The game's maps are made for 6.

The players past the script's get a ped at boot (`Game::BootGame`, `engine/starts.cpp`), the k-th at the k-th restart
zone in the map's order (the restart zones' turn stays the script's), in the first of the 10 looks below the map's
players don't have. A ped starting at a restart zone (those, a join, a respawn: `Ped::SpawnCharInZone`) takes the
block its slot picks: of the zone's blocks in turn from its centre, at the level of its first block with ground, slot
N takes the N-th, counting on into the next zones once a zone has fewer, and round again past the last
(`core/spawn_spots.cpp`). Slot 0 starts at the centre, the game's own spot. The zone is the game's turn's (the round
robin) unless another living player is within 12 blocks of it: then the next one in turn with nobody that near, else the
one whose nearest player is farthest (`SafeZone`). A client predicting a respawn sees the others where it predicts them,
so near that 12 blocks it can pick another zone than the server, which the next snapshot corrects (a jump).

A map without a restart zone isn't playable (the game ends at its start with "No Restart Zone"): the server leaves
it out of its rotation and votes with a warning, refuses to start if no map is left, checks a map again before it
changes to it (the next playable one instead), and a client sent one leaves with the reason (`Playable`,
`core/map_players.cpp`).

Choosing maps that suit the server's players is the admin's job. The shipped maps have 4 (Residential) to 10 (Tiny Town)
restart zones; their scripts handle players alike, so extras play as the others. A community map whose script
handles players one by one (missions, scores kept per player on `p1`..`pN`) leaves the extras out of those.

A join takes the first free slot; at a map change the taken slots are numbered again from 0 in their order
(`CompactSlots`: 0, 1, 8 become 0, 1, 2), then those of players who dropped in the last 40 s (their reconnect); the
other names are forgotten. Slot N is the map's N-th `PLAYER_PED` while there is one. Each client hears its new slot in
its `kMsgMapChange` and takes it with the new game (its local player); the server moves each slot's token, name,
commands, ready flag, votes and map choice with it, and holds every input through the pause.

A player's look is the remap its `PLAYER_PED` gives the ped, and the HUD tells players apart by it (the digits' colour,
the arrow's). For a map of its own, give each player a different one of these 10:

| Remap | Look | HUD digits | Arrow |
|---|---|---|---|
| 0 | police | steel blue | rust |
| 5 | red-head redneck | the original's | the original's |
| 7 | scientist | the original's | the original's |
| 8 | Zaibatsu | the original's | the original's |
| 9 | Krishna | the original's | the original's |
| 10 | Russian | the original's | the original's |
| 11 | Loonie | the original's | the original's |
| 12 | Elvis | cyan | cyan |
| 13 | Yakuza | the original's | the original's |
| 14 | fireman | pink | purple |

Other remaps give players the HUD can't tell apart: 6 (the blond redneck) has the red-head's colours, as in the
original, and 1-4 and those past 14 have none of their own (the digits' and the arrow's default).

`port/net/testdata/tiny-10p` is a 10-player map on the game's Tiny Town (`MP1-comp.gmp`, `bil.sty`): a player at each
of its 10 restart zones in 10 looks, a few weapon generators. Its script is ours, `tiny-10p.mis`, compiled by
`port/net/scripts/mis2scr.py` (only what it needs: `PLAYER_PED`, `GENERATOR`, `SWITCH_GENERATOR`, `LEVELSTART`,
`LEVELEND`). To play it, copy `tiny-10p.mmp` and `tiny-10p.scr` into `data/`: `--map tiny-10p`.

## Hosting a game

Hosting is running a server; the host plays as a client of it like everyone else. Both ends need the same reG2 build
(version and commit: the 32 and 64-bit builds of one commit play together, whatever the system) and the same game data:
a join is turned away otherwise, with what differs on the network page (and printed with `--connect`).

1. **The host** starts a server: `./reg2 --server --map downtown-6p` (`--players`, `--maps` and the rest above). It has no
   window and prints its log; on Windows, where nothing shows it, add `--window` (closing the window ends it) or end it
   in the Task Manager. The menu can't start one yet. Then it joins it like the others, at `127.0.0.1`. From the same
   folder too: a server writes its own log (`reG2-server.log`) and never writes gta2_registry.ini, the client's settings.
2. **Its network**: the server listens on **UDP 27920** (`--port` for another). Over the internet the host's router
   forwards that port to the host's machine (port forwarding, in the router's settings: UDP, 27920, to the machine's
   LAN address), and its firewall lets it in: Windows asks the first time the server starts (allow it on private and,
   for players from the internet, public networks), Linux firewalls need a rule (`ufw allow 27920/udp`). The address to
   give the others is the router's public address (what a "what is my IP" page shows); no NAT traversal or UPnP yet.
3. **The others** join with the network page (start menu, **Network game**: the address, a name, **Join game**) or
   `./reg2 --connect <address>`, with `:<port>` after the address for another port.

On a LAN nobody forwards anything: the others join at the host's LAN address (`ip addr`, `ipconfig`: 192.168.x.y), the
host's firewall still letting UDP 27920 in. A player whose name is taken, empty or longer than 15 characters plays under
one the server gives ("PLAYER 2"). A server that ends (its window closed, Ctrl-C, a stop signal) tells its players, who
go back to the network page with "the server closed"; one started again starts a new game, which they join again.

### A dedicated server on a Linux VPS

A server is a plain command-line program: no window, no dialog, no sound and no setup. Its log goes to stderr (and to
a file with `GTA2_LOG_FILE=<path>`; a fatal error also writes `reG2-server.log` next to the program), an error ends it
with a non-zero exit code, and SIGINT or SIGTERM end it cleanly with 0, after telling its players. `reg2 --help` lists
the options.

- **The program**: the portable `reG2-linux-x86_64.tar.gz` (extract it, run `./reg2 --server ...`; it needs glibc 2.29
  or later and none of the desktop libraries its README lists) or the AppImage (`./reG2-x86_64.AppImage --server ...`,
  with `--appimage-extract-and-run` before `--server` where FUSE is missing). The client and the server must be the same
  build.
- **The data**: a server reads only `data/fstyle.sty`, `data/nyc.gci`, a language's `.gxt` (`data/e.gxt`) and, for each
  map it plays, `data/<map>.mmp` and the `.gmp`, `.sty` and `.scr` files it names (without a rotation the vote offers
  the six multiplayer maps whose files are there). No audio, music, movie, frontend images, keyboard files or attract
  mode replays. The top-level files of an installed game's `data/` are all it needs, about 35 MB:
  `rsync -a --exclude='*/' ~/.local/share/reG2/data/ vps:/srv/reg2/data/`.
- **Where**: `--data /srv/reg2` (the folder holding `data/`, or `data/` itself) or `GTA2_DIR=/srv/reg2`; else the
  current folder, the program's, then `~/.local/share/reG2/`. A missing file is named with the folder it was looked
  for in. The server writes `player/` and `test/` there.
- **The firewall**: UDP 27920 (or `--port`) in: `ufw allow 27920/udp`, or the provider's own firewall.

A systemd unit, `/etc/systemd/system/reg2.service` (`systemctl enable --now reg2`, `journalctl -u reg2` for its log):

```ini
[Unit]
Description=reG2 dedicated server
After=network-online.target
Wants=network-online.target

[Service]
User=reg2
ExecStart=/opt/reg2/reg2 --server --data /srv/reg2 --maps downtown-6p,mp1-6p,industrial-6p --players 6
Restart=on-failure
RestartSec=5

[Install]
WantedBy=multi-user.target
```

## Playing

**Joining from the frontend.** The start menu has **Network game** (`port/net/menu.cpp`, frontend page 13): an address
(`host[:port]`, empty until one is typed, then the last one), a name and **Join game**, kept in gta2_registry.ini
(`Network`). The join runs in the background with "Connecting..." and any
error on the page; welcomed, the frontend boots the network game as it boots a level. `--name` and the link
conditioner's options apply to a join from the page too.

**The pause menu.** Escape (a pad's Start) in a network game is the client's: it opens single player's pause menu
(`port/src/pause_menu.cpp`, not while a chat line is typed) over the game going on. Nothing is paused: the client keeps
receiving and simulating, and sends no controls while the menu shows, so its player stands still. Its keys reach neither
the game nor the server. It has RESUME, READY or NOT READY in a warmup (`/ready`), VOTE YES and VOTE NO under an open
vote's question (`/yes`, `/no`), PLAYERS (the scoreboard, below, with "Esc: back"), GRAPHICS, DISPLAY, SOUND and CONTROLS (TWIN STICK with its settings and ZOOM greyed: they are off in
a network game), and LEAVE GAME, whose question LEAVE (Enter, Y, a pad's A) or CANCEL (N, Escape) answers. A round's
fresh game closes it. Leaving leaves the server and goes back to the network page
as a single player again. So does a client that lost the server for good, was kicked, or hasn't the next map's files,
with why on the page; without the map's files a join fails the same way.

**The scoreboard** (`engine/scoreboard.cpp`) shows over the middle of the frame while SCORES is held (a control of its
own on the CONTROLS page, Tab by default: `kScoresKey` in `compat/src/pad.cpp`; the game never sees it) or a gamepad's
Back is held (a pad's ready and votes are the pause menu's), and as the pause menu's PLAYERS. HORN OR TAUNT (SPECIAL 1)
defaults to C; a profile that still has it on Tab keeps it (saved bindings are never changed), and in a network game's
round the SCORES key's presses don't reach the game (`port_ScoresKeyInGame`), so Tab shows the board without the horn.
On a translucent panel: the map's title, the game type and its limit, the round and its time left, the players in the
game and the server's address (until servers have a name); then a row per player by score: its name in its HUD colour (a square for the cyan, which no font has), then chat,
IT, dead or ready, its score (a tag game's time IT as 1:23), its money ("$1010": the money on its HUD, which the original's HUD listed for the other
players; a points game's score is already the money, shown so), frags in a points or tag game, deaths, and the ping
(each client reports its own in its pings, the server sends everyone's once a second: `kMsgPings`; BOT for an arena bot). The columns are laid out from the right by their widest entry, and the
names cut to what is left. The local row is marked. Under the rows, what the round panel it hides would ask (ready, a vote:
F1, Y / N, or the Start menu on a pad). Not over the pause menu, nor while typing a chat line. `GTA2_NET_SCOREBOARD=1`
shows it all the time.

**The HUD's standings.** Instead of the original's list of every other player's money and frags (or tag time) down the
left side, a client shows its rank and score at the top left ("2nd / 10  7 frags", "1st / 10  1:23 it"), in place of
the game's own counter at the top (the frags, or the tag time); in a points game the rank only, the score being the
game's money at the top right, and no frags counter either. The others' scores and money are the scoreboard's, whose ranks these are, by the same
score (`gPortDrawNetStandings`). A replay keeps the original's list and counter.

**Connection.** The ping shows at the bottom right: the network's round trip alone (`kMsgPing`, answered at once,
both polled between frames). With no snapshot for 0.7 s it shows **Connection interrupted** and plays on, the others'
last inputs held for 10 ticks (the fire for 5) after the last snapshot, then none. When ENet drops
the connection (after 5-10 s) it shows **Lost the server, reconnecting** and connects again every 3 s with its slot's
token (the welcome's): the server gives the slot back with its score and ammo, also while it still has the old
connection (dropped then, the player stays where it is), else at a restart zone. A game the server went on to
meanwhile (a round's fresh game, another map) starts here as a map change does, in the slot the welcome gives (a map
change may have renumbered it; a dropped player's slot is kept if the map has room after those in, else he joins again as
a new one). After 30 s it goes back to the
network page. A new join under the name of a player who left takes that slot, with an empty score.

**Chat** is the game's own: F10 starts a line, each key types into it, Enter ends it, and every machine shows
"name: line_" while it is typed. The key events reach the server in the commands, so the server's player types the
same line, and the player entity carries it to the other clients. A key types the character the system's keyboard
layout gives with shift (`port_TypedChar`): the game's own tables (`data/keyboard`) have one character per key and no
shift. The server's copy of a line, which has no keyboard, types the character each press typed on its client, sent
with the command (`gPortChatChar`), so the others read what was typed on any layout (the game's US table when none came).
The game shows a line only while it is typed; a client keeps the last 5 ended lines (but commands) over the frame for
10 s each, from the received snapshots (`KeepChatLines`). The warmup's panel and notice name the keys.

**Commands** are chat lines starting with `/`, sent to the server when typed to their end: `/ready`, `/start`,
`/map N` (or a name), `/kick NAME`, `/yes`, `/no`. Tab completes the command, then a map from the welcome's list or a
player's name, as far as all that fit agree; when several fit they show over the frame (`CompleteCommand`).

**Rounds** (`session/round_flow.cpp`): a match is rounds in phases, run by the server and shown on every client:

1. **Warmup**: the map is played as usual. It ends when `--minplayers` are in and all are ready (F1, the pause menu's READY, or `/ready`), or
   `--warmup` seconds after the minimum was reached (0: only when all are ready).
2. **Countdown**, 5 s, large in the middle of the frame, in **a fresh game**: every round is a game of its own, started
   on the server and every client the way a map change starts one (on the same map, or the voted one), so scores,
   money, weapons, wanted levels, wrecks and the camera are as at a game's start. Its map script sets it up while the
   players are held (their inputs are key events only, the chat). The countdown starts once every client has the
   new game (it acknowledged one of its snapshots: the newest its game took, not the newest that arrived, so a client
   still loading the map isn't counted), "Waiting for players" until then, 30 s at most. The players
   start where the map script puts them. Too few left: back to the warmup.
3. **The round**, to the frag limit (points in a points game; tag: time only) or the time limit. The leader wins
   (in a tag game, as the game's own results: the player who was "it" the longest); a tie at the time limit is a draw.
   A client shows the clock and the goal in the bottom corner ("9:42   FIRST TO 10").
4. **Results** (`--results` s): the winner, a table of frags and deaths (and points first, sorted by them, in a points
   game), and the maps with their votes (the number keys, Up, Down and Enter, a gamepad's
   d-pad and A, or `/map N`). The maps show by their `.mmp` `Description` ("Face Off" for mp5-6p, `MapTitle`), as the
   original lobby listed them; `/map` and Tab take the file names.
   The world stands still meanwhile: a tick runs only the players' own (the chat goes on) and the HUD, and the
   inputs are key events only, on the server and every client (`gPortWorldStill`); its sounds stop as in a pause.
   Then the next round's fresh game, on the most chosen map (a tie: one of the tied, drawn from the server's secret, as
   its replay draws it), else the rotation's next, else the same map. The board and the still world stay through the
   map change's pause, till that game's countdown.

**Votes**: `/start` (end the warmup), `/map N` (change now; a round ends without a winner), `/kick NAME` (out, and the
name turned away for 10 minutes). One at a time, open 20 s, decided by a majority of the people in the game (not
voting counts as no; the caller votes yes). Bots don't vote, nor choose the next map: an arena's, and a client's
(`--bot`, its hello says so), are left out of the count and refused ("Bots don't vote"), and a client's can be kicked; Y or N while it shows (the keys that type them, so QWERTZ works), F1 for
yes, or the pause menu's VOTE YES and VOTE NO. On a gamepad the warmup's and the vote's hints and notices point to the
Start menu (`PadNotice`). `--votes` sets which are allowed.

With `--rounds off` (the default with `--arena`) the round starts at once, with no warmup. With bots only, the next
follows its end directly, in the same game (everyone respawned with no score: `engine::NewRound`) or on the rotation's
next map; with a person in, a round's end is the round flow's: the results, then the next round's fresh game and its
countdown.
The game's own end of a network game (its frag and time limits) can't be come back from on a dedicated server, so the
game gets neither and the server ends rounds itself. A map change ends the game on the server and every client and
starts the next at once: `winmain.cpp` asks the netcode (`gPortNextNetworkGame`) instead of going to the frontend.
Until then the game's exit timer is set again every frame (`KeepMapChange`): a client's restore brings back a game
that wasn't ending.

## How it works

### Why the engine shapes the design

- **No per-entity update.** `Game::UpdateGame` runs every subsystem once per tick, so neither end can simulate
  one player on its own. The client predicts by rolling back and resimulating the whole world; the server applies
  each player's input at a given tick, so a client runs ahead of the server (the Overwatch and Rocket League model,
  not Quake 3's) and the server keeps a small input buffer per player.
- **State is fixed-point.** Positions are `Fix16` (14 fractional bits), angles `Ang16` (0-1439): a position and
  rotation go over the wire losslessly, so predicted and server state agree to the bit when nothing else interferes.
- **State is pools allocated once**, about 3.2 MB in all, every byte of it reachable from a global: the port registers
  every global and static (`port/src/port_globals.hpp`), and that registry gives the rollback set.
- **A tick is the game's unit.** Speeds, timers and animations count ticks, so the tick stays the game's (33 ms
  by default, `--tickms`).

### The game loop

Every network step runs the tick, then `NetPlay::SendToAll`, where the original sent its inputs. The port's `NetPlay`
(`engine/netplay.cpp`) makes that the netcode's frame hook, once per tick: after tick T, before anything is drawn.
There the server reads its clients and sets every player's input for T+1; the client sends its command, applies
snapshots and rolls back. Between steps both answer pings at once (`gPortIdleHook`): the client polls the network about
every millisecond, the server waits on it until about 2 ms before its next step (`engine::NetworkWaitMs`) and polls the
last stretch, so neither the tick nor the frame adds to the ping.

A client runs reG2's frame loop (`port/src/frame_loop.cpp`, `gPortNetFrameLoop`), as single player does: ticks at the
game's rate, frames at the FRAME LIMIT, each drawn between the last two ticks (`port/src/interpolation.cpp`), SHOW FPS.
The step is the tick and the hook together, so a draw only ever sees the state after a hook: the interpolation goes
from what the frames before showed (the state before the tick, after the last hook's blend) to the state after this
one's rollback and blend, and a correction is spread over the frames of a tick. Objects and particles go from the
corrected game's place before the tick instead (recorded before the shown tick is resimulated, `engine::ResimStepStart`):
after a rollback the same sprite slot and id may hold another object in the prediction. A server and a replay run the
original's frame path in `winmain.cpp` (a step, then a frame): a headless server skips `Game::Draw` (`gPortNoDraw`: its
steps don't depend on it, an arena's world is the same tick by tick without it), a replay runs as fast as it can.

### The server

- **Commands** carry the server tick they are for and repeat the last few, so a lost packet loses no input. The server
  buffers them per tick and repeats a player's last input when one is late (its controls only: a repeated key would
  type twice), for 6 ticks at most: a client silent longer plays nothing, its player stands until it is back. It
  reports the newest command tick it has, and the client keeps its commands about two ticks early. The client paces
  its ticks to keep them there (a quarter faster when late, a tenth or a fifth slower when early: only the wait between
  them, the game's timers still count the server's tick length); commands a few
  ticks late are caught up by running the missing ticks at once, saved, and only more than 8 late or 20 early make it
  jump (its saves lost until snapshots of the new ticks come).
- **Snapshots**, per client, every tick (`Server::SendSnapshot`): every player and the car each sits in, gets into or is pulled out of, wherever they
  are, and everything within the client's interest area (24 blocks around its player's foci; a camera sees 6 to 8). Each
  entity is a delta against the state that client acknowledged for it (per-entity baselines, so a snapshot can carry
  part of the world and nothing waits for a lost packet). What doesn't fit the budget goes with the state the client
  already has and its age, a few bits. New state goes by priority: the age plus one over one plus a sixth of the
  distance; an entity new to the client counts as 8 ticks old. Beyond 12 blocks (off screen with a margin), an entity
  whose new state costs more than its age gets it only every third tick. A busy area's snapshot goes in up to
  `--packets` parts of the link's largest unfragmented packet (`--mtu` less 56 bytes: 1224 at the default 1280).
- **Spawning** (`engine/spawn.cpp`): the game's limits on ambient traffic (16 cars) and peds (50) are for the whole
  game, and the first players' cameras took them all. A server allows the limits times its players, up to 3 players'
  worth (its traffic has drivers of its own: more ran the ped pool out). Before anything spawns, each client's camera
  bounds grow ahead along its motion by that client's lead, so nothing appears inside what it already shows.
- **One view for everyone**: every player sees the same, 16:9 at the game's own zoom. CAMERA ZOOM doesn't apply in a
  network game (`PortCameraZoom`), a wider screen shows bars at its sides (`ReadScreenSettings` keeps the game's screen
  16:9 at most, the renderer its aspect) and a narrower one sees less width. The server gives every player's cameras
  that view, so peds and cars spawn and go out of everyone's sight, and a traffic car, which the game spawns 5 blocks
  beyond one camera's edge, isn't spawned where another player's view is (`TrafficSpawner::SpawnTrafficCar`).
- **What is on screen is the same on both ends** (`engine/spawn.cpp`, `NetworkViews`): the game decides by what the
  players see in many places (a ped's AI draws a number only on screen, a fleeing car's route ends off screen, a car
  blocking a chase goes once off screen), so a client gives every player's cameras the bounds the server's have: the
  network view whatever its window or the size the others' cameras have here (640x480), grown by each client's lead,
  which the snapshots carry (the world entity's; a client resimulates with the last it got). The cameras follow the same peds and cars on both ends, so they agree but for a misprediction; a player
  a snapshot moves more than 8 blocks (its first snapshot, a respawn) gets its camera there at once, as the server's.
- **Joining and leaving** (`engine/slots.cpp`): a slot without a client is out of the game the way the game handles a
  player who left (its ped walks on as a pedestrian). A joining client gets a new player ped at the next restart zone,
  on a block nobody stands on ("Maps and players"), and an empty score. Snapshots carry the slots in the game, and clients mirror them. The others read "BOB joined",
  "BOB is back", "BOB left" or "BOB was kicked" (the server's notices).
- **Idle when empty**: a server with nobody to play for (no client connected or connecting, no arena bot) runs no
  step: its idle hook waits on the network up to 100 ms at a time (`Server::Deserted`, `Transport::Wait`) and puts the
  next step off (`engine::HoldStep`). Its tick and the round flow stand still, so the round goes on where it was when a
  client connects (that event lets the steps run again); pings and connects are answered meanwhile.
- **No sound**: a server opens no audio device (`gPortNoAudio`). Nobody hears it, and its listener would follow slot
  0's camera, whose ped can be any ped in any car once that player has left.

### The client

- **Prediction** (`engine/rollback.cpp`): the client saves the game after every tick. On a snapshot of tick S it
  restores S, applies the server's state, and runs the ticks up to its own with its own inputs; other players take
  the controls the snapshot forwards for the ticks after it (the next tick's as the server runs it, then the commands
  it holds, up to 4), then the last of them repeated, a turn only once two in a row have it (a tap repeated turns far
  past the server's). The repeat is what rollback games do (GGPO's fighting games, Rocket League's cars). On
  recordings (`GTA2_NET_FWDLOG`) it had the movement wrong in 12% of the first guessed ticks and 23% of the fourth;
  letting go was wrong in 80% and drew more run-overs the server never had. Those come from what the victim does in the
  guessed ticks, which no repeat knows: a jump (a jumping ped doesn't collide with cars), a step aside or back; 5% of a
  client's run-overs of players at 50 ms of lag, 15% at 100. An arena bot is guessed longer (only its next tick is
  forwarded: 4 ticks at 50 ms against 2 or 3 for a client's player), but deciding its controls ahead doesn't help: its
  reaction to a car then comes as much later as it is known earlier. A repeated fire is a guess: the shooter's pose shows (`Ped::mBf21C.bAttacking`, the gun raised),
  but its weapon is not used (`SetFireGuessed`, `gPortPedMayFire` in `Ped::ManageWeapon`): another player's shots come
  only from the ticks the server forwarded, never drawn for a burst it didn't fire (`GTA2_NET_HOLDFIRE=1` fires the
  repeated fire too, to compare). As a guess doesn't count its weapon's reload down, the player entity carries the
  held weapon's timer (`Weapon::mReloadTimer`), so another player's held fire shoots on the server's ticks (a machine gun's
  lock-on can depend on that tick: who else fired in it). With 150 ms of lag, 13 of an arena hunter's 145 shots came
  out on other ticks than the server's, none after. `GTA2_NET_NOFORWARD=1` predicts
  by the repeated input alone (to compare on a replay).
  Everything else moves on by the game's own simulation in those ticks. Of the resimulated ticks only the first 4 are
  saved again (the next snapshots restore those; a save of the 3 MB world was three quarters of a rollback's time): the
  later ones keep their save from before, a world of that tick still, which a snapshot after 4 lost ones would restore
  (counted in the log). A snapshot whose save is lost (a jump) or was taken with another rollback set (one with a block in the
  middle, not only at the end: a pool allocated) is applied on the game as it is, and the ticks since run again the same way, so the game stays at the client's tick. Every snapshot is
  reconciled so, as routine: the client's log counts, every 300 frames, the snapshots reconciled, those that changed something (a player's or its car's state the client predicts, or the
  world's, differed from its own at that tick: the local player's prediction error among them) and those that changed
  nothing, the ticks resimulated each, and what a reconciliation takes.
- **The blend**: what is shown, place and facing alike, moves a part of the way to the corrected state each tick: 0.3
  of a small error, rising to 0.5 of one near 2 blocks, and 0.5 of the local player's own (on foot or its car); more
  than 2 blocks off (a respawn, a car entered) jumps. Tuned on recordings by the display error (what a tick showed
  against the server's tick, players included, in the client's log): against 0.2 for all, the other players are shown
  a third nearer the server's place, the local player half as far off, and they move no jerkier but at 60 ms of lag,
  where steps changing by over 0.15 blocks/tick^2 go from 0.05% to 0.11% of the frames.
- **What a client runs** (`engine/client_mode.cpp`): a client simulates what moves, not what decides. The game's own
  debug switches turn the rest off, set again every frame:

  | Service | Client | |
  |---|---|---|
  | players, cars, peds, objects (movement, physics, collisions, weapons) | runs | the prediction |
  | traffic and ambient ped spawning, despawning | off | only the server creates and removes entities |
  | the map script's level start | its look and sound only | its shading level, ambient light and sounds, and the cars it puts on car transporters (`gPortScriptCommandRuns`); the rest is stepped over |
  | scripts after the level start, police, ambulance, fire engines, buses | off | they create entities; the results arrive as state (the police's as the world entity's; a wreck burns only where the game sent a fire engine to it, `Car::SpawnFire`, so its fire is the server's) |
  | generators, the garage, emergency crews | run, idle | scripts switch generators on and park cars in the garage, the police and the ambulance send crews |
  | doors, cranes, crushers, trains, traffic lights | runs | predicted, corrected by snapshots (the lights' phase is in the world entity, each train has one of its own) |
  | particles, tile animations, lights, HUD | runs | cosmetic |
  | sound | runs | the sound log ("Sound on a client" below): a resimulated tick runs the sound's entities without the mixer, and what it asked for that no live frame played is played once its tick is settled |

  Player peds take no damage on a client (`gPortProtectsPlayers` in `Ped::TakeDamage`): their health is the server's.
  Everything else of a hit is the game's own, by the server's invulnerability (the player entity carries it): the
  blood, the throw a car or a blast gives, getting up or dying after a fall. Peds and cars count their ticks off screen as the
  server's do (the same views), but nothing is removed there for it: a server's ped by `gPortPedMayGo`, a car by
  `gPortServerDespawns` (`Car::ManageDespawning`).
- **Phases** (`engine/phases.cpp`, `engine/throws.cpp`): a ped thrown (a blast, a car or a door throws it on a moving
  object its char follows until the object stops, then lands it by the throw's outcome: back on its feet, knocked down,
  or dead), lying knocked down, sinking in water, electrocuted, or pulled out of a car at its door. The player and the
  ped entity carry the same record: the phase, whether its char is yet to start it, the value that ends it (a throw's
  timeout, the lying, sinking and electrocution timers, the door's frame, its timer and the door), the door's car, and
  a throw's object (its place, motion and outcome). A client predicts every ped's phases as the game makes them, and at each snapshot's
  tick the server's wins: the same phase gets its timer, one the server has is started the game's way (`Char::DrownPed`,
  a throw's object, the door as the jacker's char puts a driver there; a ped getting in or out at a car door here is
  called off first), one the server lacks is cancelled to walking
  (a throw: its object removed). Nothing waits: there is no grace for a throw just landed.
- **Deaths are the server's** (`ClientPedMayDie`, `gPortPedMayDie` in `Ped::Kill`): a server's ped on foot (a player, or one
  mapped to a snapshot's; the client's own die as the game has them) dies only by
  the server's death (`KillNow`, also the ambient peds'); its own death, from a predicted shot, car or fall, is held
  (health 1, knocked down on its feet) until a snapshot says what became of it. A player held so collides with nothing
  until the next snapshot (`ClientCharSolid`, `gPortCharSolid` in `Char::ShouldCollideWithSprite`), as the server's
  corpse doesn't: lying, it would stop the car that ran it over each tick (`CarPhysics::StepMovementAndCollisions` ends
  a car's move at a sprite it hits twice), and the local player's car fell up to 0.6 blocks behind the server's until
  the snapshot pulled it on. An ambient ped on its feet or lying is
  shown dead at once instead (`ShowDeath`: its state, cry, corpse's sprite layer and blood), the rest of `Ped::Kill`
  (its corpse's AI, a dropped weapon, the score, the killer's wanted level) left to the server's death, which `KillNow`
  then carries out without starting its dying again; one the server still has alive `kShownDeathTicks` after it was
  shown gets up as the server's, and until then it lies where it fell. A throw's fatal landing is the game's (an ambient
  ped's thud, `Char::UpdateImmobilized`; a player's is silent, its cry the server's death's), its death held as any; before each tick a throw's timeout and the sinking and
  electrocution timers are held short of the death (`GuardPhases`); the
  server's death ends them as the game does (a charred corpse, a sunk one, neither dropping its weapon; a player's
  electrocution voice and death type). Deaths in a car are its car's, whose explosion is the server's. A corpse here
  whose ped the server has alive (the paramedics revived it, `AmbulanceService`) is revived as they do it.
- **A server's ped goes by the snapshots only** (`ClientPedMayGo`, `gPortPedMayGo` in `Ped::Deallocate`): the client's
  own AI doesn't free a ped the snapshots carry (one boarding a train, a group's cleanup); a server's death and a ped
  in a car going with its car (`Car::KillContainedPeds`) do. A player's corpse (`Player::Wasted` clones the player's
  ped, `mPlayer` included) is the client's own until the respawn: held as the server's, every death left one for good,
  and a long game's ped pool filled up.
- **A player's corpse stays after the respawn** (`gPortCorpseKept` in `Player::Wasted`, `CorpseKept`): where the
  original frees it, both ends turn it into a dead pedestrian (no player, camera, group, weapons, wanted level or
  voice, an id of its own), which the game frees as any corpse, 40 ticks off every player's screen
  (`gPedOffscreenDespawnTime`). The server sends it as any ped and the client adopts its own corpse for it (only a
  corpse adopts a corpse). The two countdowns end a few ticks apart: a server's corpse waits while the client still has
  the player's there (made, it would die again), and a client's own waits 30 ticks for the server's before it goes as
  any of its own. A corpse in a car goes as in the original. Single player keeps the original's.
- **A player's pull out of its car is predicted** (it can be undone, unlike a death): a jacker the client predicts, its own
  player or another, pulls the seated player out at the door as the game does. While the server's jacker is still on
  its way in, the pull stands (its victim stays where the pull put it, not in the server's seat); once the server's
  jacker isn't getting into that car and isn't in it, the jack is called off (`CancelJack`) and the victim put back
  in its seat (`SettleCar`). A pull the server had first starts here from its door (`StartPulledOut`). Deaths stay
  the server's. An ambient driver's pull is predicted and corrected as its phase. A ped jacking a driver on foot
  (`Ped::PullDriverOutOfCarStateMachine`, the carjacking animation) gets the server's states with its char at the
  victim's car here, or keeps its own while that car isn't here.
- **A leave the server cut short ends here too** (`EndLeaving`): a player getting out of a car here while the server's is
  on foot and not leaving is put on foot at once, so that whatever the server's does next (a jack) starts here as well.
- **The client's world** (`engine/replica.cpp`, `RemoveUnsent`, `EvictOwn`): a snapshot is all the client knows of the
  server's world. It carries every player, the car each sits in, gets into or is pulled out of, and every train wherever they are, and everything
  within the reach (the interest area, 24 blocks around the client's player's foci as the server has them), each entity the
  client has acknowledged at least as its last state and its age when the budget is short: so nothing the client has
  inside the reach is ever left out, and what a snapshot lacks is gone on the server or out of the client's area,
  where nothing more will come for it. Hence:
  - a server's car or ped lives on the client exactly as long as it is in the newest snapshot. Left out, it goes, unless
    the client's game still holds on to it (a player in it or getting in, a ped at its door, a corpse in it): then it
    stays mapped and goes on a later snapshot, once let go. Back in the area, it comes as new, made again or adopted;
  - the client's own cars and peds that no server entity is mapped to (the map script's at the start, what its
    prediction made) go inside the reach, where the server has none. Beyond it the client keeps them as a guess, to
    adopt when the server's comes into reach (the map script's parked cars), and they are the first to go when a pool
    runs short: below 32 free cars, peds or chars, the farthest go (logged as evicted), so a server's entity never
    waits for room. One that does, is logged ("waited for room") and made on a later snapshot;
  - the players' own peds, their corpses (`Player::mAuxPed`), the cars they sit in or are getting into, and the trains'
    carriages are never removed here.

  So the client holds the server's area, the players' cars and the trains, and the map script's leftovers: the pools
  stay well within what the server's own game uses. Keeping the server's cars beyond the reach once filled a long
  game's car pool (290 of 306), and the server's cars near the player were never made: invisible, the player's car hit
  them on the server only and was pulled back each time.
- **Replicas** (`engine/replica.cpp`): the client adopts what the map script made on both sides, makes the rest, moves
  them every snapshot and removes them by the rules above. A train's carriages are never made or removed: the client's own trains (both ends
  make the same ones at the map's start) take the server's state and car ids from the train entities, before the cars;
  a carriage is placed by its train only, never moved to a player in it. What the client's prediction did that the game can't undo (a wreck, a corpse of
  its own) is removed, and the next snapshot makes the entity again as the server has it. A car carries its driver: none, the traffic driver (all
  traffic cars share CarManager's spare, which is never sent as a ped), or a ped. A car's seated passengers come as
  peds at their car and are seated as the game seats them. Each mapping keeps its car's or ped's own id (`mCarId`,
  `mId`), so a pool slot the client's game freed and handed out again isn't taken for the old entity.
- **Players in cars** (`engine/car_entry.cpp`): a client gets into and out of cars by its own predicted input. When
  both sides have settled and the server has the player elsewhere, the client puts the ped into the server's car, or
  out of its own, at once, ending the way the game's own entering and leaving end; a car the same snapshot makes
  (one new to the client) once the cars are applied, so the ped never stands on foot where its car appears. Another player's enter key reaches
  a client after its tick, so when the server's player is on its way into a car and the client's isn't, the client
  starts it as the key does.
- **Guards** (`engine/guards.cpp`, on the server too): what a network game breaks in the game's own logic is made
  safe: peds given on-foot orders while in a car, the HUD arrows to players who joined after the start, objects
  attached to cars the client moved (a tank's turret), a player driving a train that reversed (`Train::ReassignTrainHead`
  makes the other end the lead and its driver, the ped stays in its old carriage: it goes with the driver's seat), and a
  player fallen through the floor (on foot at z 1 with no block under it: out of a car at the top of Tiny Town's ramp at
  67,173 the game puts it one level down, into the slab, and it sinks into the void) put back on its column's ground.

### Cars in turn

The game moves its cars one at a time in the pool's order, each against the others' places as they are. A car chasing
another that comes before it in the pool hits it where it was a tick ago: at speed it stops a tick's move (a fifth of a
car at 0.25 blocks a tick) short of the car it hits as both are drawn after the tick, the sparks, sound and push at a
gap. A network game (both ends) moves a car first when another close behind it drives at it while it drives away
(`core/car_order.cpp`, `engine/car_turns.cpp`, the pool's order otherwise and for a ring of them), so the follower hits
it where it is drawn. Measured on rammer matches (3 seeds, mp1, 10 minutes): same-way hits that leave the hit car over
0.1 blocks from where it was hit went from 22% to 0.4%. A car hit after its own update reacts in its next one (the game
reacts in the same tick when the hitter comes first); one freed since is forgotten.

### Entities

The **net schema** (`engine/schema.cpp`) declares each entity type's fields, by name and width; `Capture` reads them
from the game and `Apply` writes them through the game's own functions wherever it keeps derived state (moving a
sprite relinks it in the collision grid; seating a ped sets both sides of the link). References to other entities go
as net ids. Structures go member by member by name (`engine/members.hpp`), never by position: the 64-bit layout isn't
the original's.

| Entity | Carries |
|---|---|
| World (one per snapshot, `engine/world_entity.cpp`) | the game's tick counter (timers are stamped with it), the random seed and stream, the restart zones' turn, the round's state (`core/round_state.hpp`), the game's generation (a map change), the traffic lights, the tag game's fugitive and times, the map's light level (its script sets it: `MapRenderer`'s ambient, its target and step), the police's state that a client's cops, cop cars and HUD read (the highest wanted level, the strongest crew, a crew on screen, the last cop whose shot hit, and each pursuit's criminal, level, state, timer and crews), and each player's lead (its view, "What is on screen is the same on both ends") |
| Player (`engine/players.cpp`) | place, input, health, death count and last killer, weapons and the ammo of all 28 (a car weapon the car here lacks is added to it), frags and points, the HUD's money and multiplier, power-up timers, its car, the chat line, its phase (`engine/phases.cpp`: thrown, lying, sinking, electrocuted, at a car door, and a throw's object) |
| Car (`engine/cars.cpp`, `car_ai.cpp`) | place, its whole `CarPhysics` (but its two pointers), damage, its wreck's fire, lights, horn, colour, its roof turret's angle, the trailer it tows (towed trailers sent too), its proofs and alarm (`Car::mFlags`), its driver, and its AI (`CarAI`: route, speed, the cars it chases) |
| Ped (`engine/ped_state.cpp`) | place, and the plain values its AI runs on (objectives, targets, speeds, timers, occupation, flags, health, death cause), its states, its references (targets, group), its car and seat, and its phase as a player's |
| Pickup (`engine/pickups.cpp`) | model and place; roadblock barriers too (they are off the object pool's list), and the car weapons' mines and oil |
| Shot (`engine/shots.cpp`, those in the client's area) | a rocket's, grenade's, molotov's or throwing blast's start, keyed by prediction: place, model, blast type, timer or fuse, owner, the tick, its motion; a burning debris' state now (its fire's timer) |
| Train (`engine/trains.cpp`, every train in every snapshot) | its lead's place, speed and driver (none, the traffic driver, a ped, a player), its carriages' car ids (the client's train of the same index takes them), which end leads (`Train::ReassignTrainHead`), its state, stop phase and timers, the station it heads for and that station's state, each carriage's damage and explosion timer |

Health, deaths, weapons and scores are the server's. A death the server had is carried out on the client with the
game's own `Ped::Kill` (dead at once, also when a blast has the ped knocked down; a thrown player when it lands, as on
the server), credited to the killer, and the deaths carried out are part of the rollback set, so a
resimulation never repeats one. Its respawn takes the server's restart zone: the snapshot's zone turn is already past the
server's respawn, so it goes back one first (`ZoneTurnBack`), and the slot picks the same block. The HUD's kill notices come from these deaths only, with the server's killer (none for
the police's), not from the deaths the client's prediction starts. A client goes by the game's death count, which only counts up (the round's, in the
results, starts again with each round), and takes a player's count as it finds it the first time it sees the player. Scores (with the money and multiplier) are set before the resimulation, so its ticks and saves carry the server's
(the HUD's rolling digits roll towards them), and again after it with power-up timers and the tag game's "it";
a new round sets them all back to a game's start.

**The interest follows the view** (`engine::PlayerFoci`): a player's foci are what its view camera follows and its own
ped, when that is another. The game respawns a wasted or busted player's ped at once, at its restart zone, and shows its
corpse or its arrested self (`Player::mAuxPed`, camera mode 2) until the death's countdown ends: the interest covers
both, so the world around the death goes on in the view and the respawn's surroundings are there when the view jumps
to them. The client's reach is the same foci of its own prediction.

**The kill cam** (`engine::KillCamTicks`, `DeathCamera`; `gPortKillCamTicks`, `gPortDeathViewPed` in `Player.cpp`): a
player wasted by another player sees its corpse zoomed in with WASTED for the original's 70 ticks, then its killer for
90 more (the killer's own view: its corpse if it died meanwhile), then respawns. The killer is its slot, resolved each
tick; a slot out of the game ends the kill cams on it (the corpse again). Each change of the ped followed (the start,
the killer's respawn, a killer gone) cuts to one more than 6 blocks off, as the camera's own follow would cross the map
slowly. Every wasted player's countdown has the 90 ticks, a killer or not, so the respawn's tick never depends on
which killer each end found; only what the view follows does. The killer is in the rollback set, and from the death on
the interest covers it too, so its surroundings are there at the cut. A death by anything else (the police, a fall, its
own rocket) stays on the corpse; a busted player respawns as in the original. Looking away, the view no longer keeps the
corpse's car: a wreck or a sunk car goes once no view has it (`Car::IsCarInConditionsToDespawn`), and the corpse would
lie in a freed car, which the car radio reads (`Player::GetPlayerCar`). So the car the player's `GetPlayerCar` returns
is never despawned, as the original's view never leaves it.

A ped's AI state is applied only while it is on foot on both sides (a driver in the same car gets its objectives but
not its states). Its phase goes first, whatever it does (on its way to a car too); its states and its char's movement
and animation then, whatever they are, once both have the same phase (a fall of one side only waits). The rest is all
or nothing: if a reference its objective acts on (its target ped, internal target, car thief, the car it heads for,
enters or flees in) isn't in the client's game, the ped keeps its own objectives for that snapshot; the others (who
attacked, punched, was hit or killed it, the car it stole) stay the client's when missing. Groups are rebuilt on
the client from each ped's leader, keeping only members that are present (the game walks a group's list up to its
count).

**Shots** (`engine/shots.cpp`): rockets, grenades, molotovs and the blasts that throw (types 18, 19, 20, 32, 33) are
simulated on both ends and matched by a prediction key both
compute: a hash of the seed, the creator (the ped, car or service whose update runs, or the object whose update makes
it: a rocket its blast) and how many keyed objects that creator made, in this tick for an entity (with the tick), in
its life for an object (without: a rocket that hits a tick later on one end makes the same blast)
(`PortRand::NextObjectKey`). The key lives in the object (`Object::mUnused28..2A`, its count in `2B`), so a restore
brings it back with it. Two live shots with one key: the server's second takes another, a client drops its second.
The server sends each shot in the client's area, new ones first, as it was at the end of its first tick (place, model,
type, timer or fuse, owner, that tick, its `Object_3C` motion): both ends simulate it on from there, so a shot costs
little after it starts. At the snapshot's tick, inside the restore, a client keeps the shots it has the key of (of the
same model and blast type), makes those it lacks within the reach as the game makes them (the weapon's way; a
molotov's fire and light), at most 32 a snapshot and with room left in the pools, brought on to that tick by their
own update, each tick of it numbered as the server's for the draws (no blast, sound or rumble on the way: what it hits
makes its blast on the server, which comes next), and removes, silently, those well inside the reach the server
doesn't have (never fired there, or already gone). A new shot left out for room lowers the snapshot's
reach below it. A blast made from a snapshot already hurt on the server: its damage there is the server's
(`gPortBlastHurts`), its throws come with the players' state. Blasts riding on a car (its explosion) or an object (a
molotov's fire) are their carrier's.
The burning debris a large blast throws (loose fires: `explosion_fragments_127` with a fire and a light, at the blast's
timers 89 to 59) are shots too, but sent as they are at each tick (every snapshot within the interest radius),
place, motion and fire's timer (0: out), not as
they started: their way is the blast's last particle's (`Explosion::mSpawnAngle`), which depends on what each end
shows. A client puts its own at the server's state and makes the ones it lacks at it, with no catch-up. In a network game
a blast throws them whether a player sees it or not (`gPortBlastDebris`; the cameras differ between the ends), and on a
client one it didn't predict (a car's, one made from a snapshot) throws none: the server's come. Not synced: bullets, flames and other
shots that don't throw (their hits on players are the server's), and the client's guesses beyond its reach.

**Other players' shots shown** (`engine/shot_visuals.cpp`, display only): another player's gun shots come from the
ticks the server forwarded, made in the resimulation after a snapshot, so they are a few ticks into their flight when
the client first draws them, and the point-blank ones are made and gone before any frame. A shot of another player's
on foot (its pistol, machine gun, shotgun or rocket; known by the ped whose update made it, `PortRand::gLastPed`, and
kept by its shooter, tick and number across resimulations) that a frame shows for the first time is drawn from the
shooter's drawn gun and slides to its place over 1.5 ticks, 3 frames at 60 fps (`PortInterp::SlideFrom`). The game's
muzzle flash (`ParticleManager::GunMuzzelFlash`, three ticks) is made in the same resimulation and is mostly over by
then: whenever the world has none at the shooter, its three sprites are drawn at the drawn gun from the frame the shot
is first known (`PortShotFlash`, with the sprites' layers), as single player shows them. A point-blank shot, made and
gone in one tick, shows that flash only, as in single player; its sound and hits are the game's own (the sound log
plays them, the blood and sparks are particles). What is simulated, and the server's hits, stay as they are; the local player's own shots are drawn as
before, and nothing of it draws random numbers. `GTA2_NET_SHOTVIS=0` turns it off (still measured: comfort's `shotdraw`).

### Random draws

The game has one random stream (`rand()`, behind `gRng`). Its original netcode was lockstep, every machine
simulating the whole map. A client here simulates its own area only, so every draw the server makes for anything else
would move every later draw of the client's. In a network game the draws have owners (`port/src/rand/owner.hpp`): each
ped's update (with its char's), each car's, and each service only the server runs draws a hash of the game's seed, the
owner (its net id, or the service), the tick and how many it drew in this update. An entity then decides the same way
wherever it is simulated. The same owners make the shots' prediction keys ("Entities"), and a shot (an object with a
key, a thrown ped's object included) owns its own draws. On a client, the peds and cars the server doesn't have own
theirs too. Draws without an owner (a player's controls, objects without a key) stay the one stream, which the world
entity carries. Cosmetics never draw from it: sound and particles have streams of their own
(`port/src/rand/sound.hpp`, `particle.hpp`), and so do a blast's particles and the electro gun's arc
(`Explosion.cpp`, `DrawElectricArc`), which the screen and the particle pool decide, so what each end shows, or
muting sound in a resimulation, doesn't move the game's draws. The shots check sees the stream at every snapshot. Outside a network game
none of this is on, and the port draws exactly as `10.5.exe`.

### The rollback set

`engine/world.cpp` lists the world as memory blocks: every registered global and what it points to, the `Player`
objects, the light grid, the map blocks the doors' faces are in, and the netcode's own (the deaths carried out, the random streams, the sound entries'
pool). `SoundPosEntry` comes from a pool in the set (`engine/sound_pool.cpp`) instead of the heap, so a restore can't
bring back freed sound entries. Left out: the frame loop's own globals around the tick (input buffers, counters,
pacing, the keyboard recorder), the audio driver and DirectInput devices, and `file.cpp`'s open file, which a
resimulation doesn't run or which are freed while the game goes on. The sound's channel bookkeeping and the radios (the car
radio's station and in-car state, the police radio's calls) stay as they are over a restore: they mirror the mixer and
the radio's stream, which aren't rolled back, and a resimulation doesn't play sound to bring them up to date. The set is made again for every game (a new map's pools are elsewhere), and leaves
out what the game's scratch pointers point to (they point into a freed map after a map change). A function's static
registers when the function first runs; one that comes after the set was taken goes at its end and the set is taken
again, so a world saved before is the start of the new one. The whole world is about 3.2 MB; saving or restoring it
takes under 0.2 ms, a busy tick about 40 µs.

### Sound on a client

A client makes its sounds from its own simulation, as the original's lockstep game did; the server sends none (it has
no sound at all) and its snapshots carry state, not sound events. Most games with prediction do the same for what a
player's own simulation shows, and send what it can't know (another player's shot, an explosion) as an event or as
state the client plays from: here that state is the snapshot plus the resimulation that follows it. So the sound has to
follow the rollback: a tick first simulated in a resimulation, which plays nothing, would never be heard, and one
predicted and resimulated would be heard twice.

The sound log (`engine/sound_log.cpp`): every live frame notes the one-shot samples its audio entities ask for
(`gPortSoundRequestsHook`, in `sound_obj::ServiceSoundEffects`, around `InterrogateAudioEntities`), keyed by the
entity's owner (a car or a ped by its id, as a resimulation can make it again elsewhere in its pool; an object, a blast,
by place, as a snapshot's blast is made again with another id; anything else by its pointer), which of its sounds it is
(`sound_0x68::mSampleIndex`; an object's sample; the collision queue's impacts by place, a car's on a ped apart; an AI driver's shouts and
beeps, which take the next of a shared count, as one), the place and the tick, the game's cycle (`Rng::get_cur_rng`),
which every map starts anew (so do the logs, `SoundLogNewGame`): a restore learns which snapshot tick it is. A resimulated tick runs the same entities
without the mixer (`SoundLogResimTick`: the queues and channels as they were, no radio, vocal stream or pager) and notes
what they would have asked for; their progress moves on with the world as on a live frame (entity ages, `SoundPosEntry`,
the frame counter, the events they take). Its pause is its own world's, as `sound_obj::Service` has it (the results, a map change's
pause), not the restored save's: the resimulation saves its ticks again, and a paused flag from the results would
silence every later resimulated tick. Once a snapshot settles a tick (its tick and the next, which runs with the
server's state and inputs), that tick's requests no live frame had (same owner and sound, within 2 ticks and a block)
are added to the next live frame's queue, each once, and noted as heard. A car's impact on a ped that another player
takes part in, as the ped or as the car's driver, is a guess at that player's inputs: a live tick doesn't queue its sound
(`gPortImpactHeard`, `CollisionSoundQueue::AddCollision`), its settled tick gives it, lead and round trip later, when the
server had it. The victim's knock-down shows at once as predicted; the thud is never heard for a hit the server lacked,
nor again each tick as the car rolls over a player whose death is held. A bigger blast's bang given where a smaller one
plays ends that one (the car the server blew up where this client's own rocket blast was; the server's game had only the
car's). What a snapshot's Apply leaves (a death the server carried out) is asked for by a pass before the first
resimulated tick, as the snapshot's next tick's. Loops are asked for every frame by the live frames from the corrected
world, and the channels playing them are kept over a restore.

| Sound | Where (`sound_obj.cpp`) | On a client |
|---|---|---|
| Shots (pistol, SMG, shotgun, rocket, car guns, mines) | `ProcessType7_Weapon`, `Weapon::mShotFired` | one-shot: the log |
| Flamethrower, fire truck gun, shocker | `ProcessType7_Weapon` | loop: live, channels kept |
| Impacts (landings, crashes, bullets) | `ProcessType6_CollisionSoundQueue`, `CollisionSoundQueue` | one-shot: the log, by place |
| Explosions: the bang at age 2, the crackle after | `ProcessOtherObjects` (object 113), the entity's age | one-shot: the log; the age moves on in a resimulation |
| Car fires, fire | `ProcessOtherObjects` | one-shot asked for every frame: the log |
| Footsteps | `ProcessPed`, the animation frame and the frame counter | one-shot: the log (the variant is drawn: keyed by the sound, not the sample) |
| Ped voices (hits, screams, a death's cry), a burning ped's scream | `HandlePedVoiceEvent` (`Ped::mVoiceEvent`), `ProcessPed` (`mScreamTimer`) | one-shot: the log; an Apply's by the pass before the first resimulated tick; a player's death cry by a sound of its own (sample index 16), so that no other line of the ped's near it (a landing) stands for it; another player's at volume 55 (the original's 35 for the local one), heard over the gunfire around a kill |
| Car doors, engine start, air brakes, AI horn beeps, car shop | `HandleCarDoorSounds`, `HandleCarEngineStartSound`, `HandleHeavyVehicleStopSound` (`SoundPosEntry::mLastSpeed`), `HandleAICarHornBeep` (`Car::mSoundEvent`), `PlayCarShopEventSound` | one-shot: the log; the last speed moves on in a resimulation |
| Phones, blaster, tank crush | `ProcessOtherObjects`, `HandleTankCrushSound` | one-shot: the log |
| Engines, cornering, skids, sirens, alarms, horn, damage, car bomb, trains, tank, cranes, crushers, ambient emitters | `HandleCarEngineSound`, `HandleAICarEngineSound`, ..., `ProcessType8_Crane`, `ProcessType9_Crusher`, `ProcessEffectOnlyObject` | loop: live, channels kept |
| Announcer lines (vocals) | `ProcessType10_Vocals`, `mVocals.mPendingVoice` | stream: the log, by line |
| Car radio, police radio, music | `ProcessType3_CopRadioAndMusic` | stream: live, kept over a restore, not run in a resimulation |
| HUD pager, frontend | `ProcessType11_HudPager`, `ProcessType2_Frontend` | live only |

`GTA2_NET_SFXLOG` measures it: a server with it listens at a client's player, both print every sample they start.

A client's log has its sound in one line every 300 frames, at the old game's end and at a new game's start
(`engine/sound_diag.cpp`, from `gPortSoundStats` in `sound_obj.cpp` and `port_MixerStats` in `compat/src/miles.cpp`): the
samples asked for by live frames and by resimulated ticks, by kind; those dropped from a full queue, beyond their range or
at volume 0; the samples started and the ones refused; the entities, the sound's switches and its bank; the listener
against the local player and whose camera it follows; and the mixer: its device, callbacks, starts, voices and loudest
output. Where the sound stops shows as the first count that drops to 0.

Measured with it: a server with three arena bots and a bot client (all armed, burning cars and trucks put beside the
client), the samples the server's game started at the client's player against the client's (matched by owner, tick
within 12 and place), LAN and three links (30 ms with 10 of jitter, 45 with 15 and 1% loss, 60 with 20 and 2%), 2
minutes each, and pistols on foot at 45 ms:

| Sound | Before (server / client) | After |
|---|---|---|
| Other players' shots, at 30-60 ms | 117 / 93, 36 missing | 109 / 111, 1 missing |
| Other players' shots, LAN | 149 / 276 (each twice) | 51 / 51 |
| Explosions' bangs, LAN / links | 9 / 0, 22 / 50 | 8 / 8, 29 / 26 |
| Explosions' crackle, links | 39 / 2 | 41 / 42 |
| Air brakes, links | 66 / 77, 61 missing (a stopped truck hissed every tick, others never) | 102 / 97 |
| The client's own footsteps, pistols on foot | 306 / 600 | 309 / 315 |
| Impacts, links | 1032 / 1093 | 838 / 862 |

Car fires were over (112 / 441, then 77 / 393): wrecks burning on the client only. A wreck burns only where the game
sends it a fire engine (`Car::SpawnFire`), which a client never does, and the client lit them all; the car entity now
carries the server's wreck fire (`kCarWreckBurns`). With it, LAN 50 / 672 before, 50 / 76 after; 45 ms 30 / 127, 29 / 34.

### Wire format

A little-endian bitstream (`core/bitstream.cpp`), never a copy of a struct: clients are 32- and 64-bit. Messages
(`core/protocol.hpp`):

| Message | Channel | |
|---|---|---|
| `kMsgHello`, `kMsgWelcome`, `kMsgReject` | reliable | the join: the protocol version, the build, the name, `nyc.gci`'s CRC-32 (and a reconnect's session and token); the server's session, the slot, its token and name, the game's settings, the map with its files' CRC-32s (gmp, sty and scr) and the game to take snapshots of, the maps a vote chooses from |
| `kMsgLeave` | reliable | why a client leaves (its copy of the map differs) |
| `kMsgCommands` | unreliable | the last few commands (tick and input bits, each delta-coded against the one before; a key press with the character it typed) and the newest snapshot the client has |
| `kMsgSnapshot`, `kMsgSnapshotPart` | unreliable | server tick, the baseline it is made against, the other players' forwarded controls, entities with field masks and changed fields (`core/snapshot.cpp`) |
| `kMsgMapChange` | reliable | the next map, after a round, with its files' CRC-32s, the players of its game and the client's slot in it |
| `kMsgPing`, `kMsgPong` | unreliable | the client's clock, sent back at once, and its ping |
| `kMsgPings` | unreliable | once a second, every player's ping for the scoreboard: the one its client reported (ENet's round trip until it has), or a bot |
| `kMsgCommand`, `kMsgNotice` | reliable | a player's command; a line to show (a vote, a player ready) |

ENet runs with `mtu` the link's MTU less IP and UDP's 28 bytes (`--mtu`, 1280 by default), two channels (never reliable
data on the snapshot channel: under loss it delayed the snapshots), its throttle off (it dropped snapshots when the
round trip rose) and a flush after every tick.

## Recordings

Every client session is recorded (`session/recording.cpp`): the welcome, then per frame the packets, the round trip and
the input, to `recordings/session-<n>.rec` (the newest 10, each stopped at 200 MB: one to a few hours;
`GTA2_NET_RECORD=<file>` elsewhere, `off` none). A client does nothing but react to what arrives and to its own input,
so `--replay <file>` plays the session again without a server, tick for tick, as fast as the game runs (no wait between
the ticks, each of the server's length for the game's timers): a minute in
about 2 s, the same every time. **F12, or the key under Esc, marks a moment** in play: the client and its replay print
`net: mark <n> at frame <f>`, so a bug seen in play is "the last session, mark 2". A recording replays only on a build
of the same protocol (its welcome's version): another one's is refused with both numbers.

**A server records its session too** (`session/recording.cpp`, `ServerRecorder`), to `recordings/server-<n>.rec` next to
its data (the newest 10, each stopped at 200 MB; `GTA2_NET_RECORD=<file>` elsewhere, `off` none; a server that only
plays its own bots, `--arena`, records only with `GTA2_NET_RECORD`). Its world is a function of what it takes in, so the
recording is that: its command line, its `GTA2_NET_*` switches and the secret its session number and tokens are drawn
from, then per frame the transport events it took (hellos, commands, chat and votes, connects and disconnects; the
pings answered between frames are left out) and a checksum of its world after the frame (every entity's fields, as the
snapshots have them). About 115 KB a minute with two players (12 bytes a tick, and some 50 KB a minute per player: a command packet a tick), 0.5 MB with ten, so 200 MB is six hours of a full server. `--replay <file>` sees a server's
recording and plays that server again without the network, headless and as fast as it runs (two minutes in 4 s), with its command line and
its switches (a switch set where it replays wins, so a log or a dump can be added after the fact); the stretches the
server stood still with nobody connected aren't in it. It checks the world against the recording's at every tick and
ends with `net: replayed <n> ticks: the world matches the recording's at every tick`, or the first tick that differs
(exit code 1). `GTA2_NET_DUMPWORLD=<tick>[,<tick>...]` prints the server's world at those ticks (each entity's type,
id and fields, by the snapshots' ids), live or in a replay, to set beside a client's replay by hand. The recording holds
the secret the reconnect tokens come from: keep it as private as the server.

## Security

What a network game trusts, and what it doesn't:

- **Not encrypted, not authenticated**: ENet sends everything in the clear, and anyone on the path (the same Wi-Fi, a
  LAN party, a VPN) can read the chat and answer as the server. Play with people and networks you trust.
- **A map pack is like an executable mod**: the game's loaders (GMP, STY, SCR) are the 1999 originals and trust every
  size, count and offset in their files, so a crafted map can run code on the machine that loads it. Install only map
  packs from people you trust. A server sends no files: it only names files the player has in `data/`.
- **The name rule**: a file name from the network (the welcome's and a map change's map, gmp, sty and scr, the vote's
  maps, a recording's) is taken only as a bare name, `[A-Za-z0-9_-]{1,20}` and its extension (`.gmp`, `.sty`, `.scr`;
  none for a map's `.mmp` name), any case; otherwise the join, map change or replay is refused with which name is
  wrong (`BareFileName`, `core/protocol.cpp`). A server checks its own `.mmp` files the same way at its start. The
  game's own copies of those names are bounded in port builds too (`sound_obj::GetFileName`, `LoadWavSdtData`,
  `ScriptManager::GetScrFileName`, `Start_NetworkGame`), and port builds have stack canaries.
- **Snapshots**: a client puts every field the game uses as an index, a table's entry, an angle into the sine table,
  a place it looks the map up at or a count of a fixed array into its valid range before anything uses it
  (`engine/sanitize.cpp`; a changed field is a `net: CHECK` line, as a real server never sends one: a server with
  `GTA2_NET_SNAPCHECK` runs the same check on its own world every tick, so a range too narrow for the game shows there), and refuses at
  the place it is applied what needs the game to check: a car model without a `car_info`, a remap beyond the style's,
  a trailer the game wouldn't hitch, an object without a definition, a station that isn't a train's, a ped state whose
  objective lacks its target, a carjacking animation on foot. In port builds the map's lookups stay on the map
  (`Map_0x370`'s `get_base`). `port/net/scripts/fuzz.sh` plays games whose client turns random fields of each
  snapshot into random values (`GTA2_NET_FUZZ`), best under AddressSanitizer (`-p res-6p` for trains: the only
  multiplayer map whose script makes some).
- **Text** a peer sent goes to the terminal and the log as printable ASCII, anything else a `?` (`Printable`).
- **The transport**: 64 KB packets at most (ENet's `maximumPacketSize`, 32 MB by default), and a connection that
  says no hello within 10 s is dropped.

## Bots

Bots are players whose input comes from a brain (`session/bot.hpp`), on the same path as a person's. A brain sees a
`BotView` built from what its client knows (its own body as predicted, the snapshot's players, cars and peds, and the
map as plain data: `engine::NavMap`, the walking height and the ways between cells) and returns the input bits a
person would press. It uses no game logic: no route finder, no ped AI. Navigation is A* (`session/bot_nav.cpp`).
The map's cells joined by ways are labelled in parts once per map: a goal in another part than the bot's (a pickup on
a roof, a player behind a wall) is not chosen, and a search between parts fails at once. A body's cell is the one
whose height fits its z, or the nearest within 2 that does (under a roof, the ground beside it). Routes head for the
farthest of the next ten cells walkable in a straight line with room either side, and leave water out. On foot the
brains walk with a `BotWalker`: stuck (under 0.3 blocks in 1.5 s), it backs off and turns; stuck again at the same
cell within 10 s, it tries the next way out (the other side with the way it was crossing left out for 20 s, a jump,
then the goal given up for 30 s). A goal its route gets no shorter to for 10 s (3 s with no route) is given up for
30 s, except a player it has a route to (a player chased keeps moving). Going round a car, a corner it stands at counts
as passed. A car it stands beside without getting in for 4 s is given up too. With nobody on its ways (Tiny Town's
second arena slot starts on a raised spot), a fighter walks at the nearest player anyway. A car that stays within 2
blocks of a spot for 5 s with nobody within 6 is wedged: the bot gets out and leaves it 30 s. A car still for a
second backs out for one, the wheel turned the other way, then this way; still again after two back-outs within 2
blocks, the bot gets out whoever is near (car stuck time: rammer 8.9 to 6.6 s per 5 minutes, hunter 7.0 to 5.8). After a fight on foot
(the hunter: someone within 8 blocks), the hunter and the soldier stay on foot 5 s before going for a car. These brains draw only their dodges' reaction times, from their own stream seeded by the slot (`BotBrain::Seed`), never
the game's, and their searches are bounded in cells, so an arena match of them repeats for a seed.

**Stuck meter**: each arena bot's line `net: arena <slot> <brain> stuck S s longest L s searches N failed F giveups G
recoveries R in a car C s` counts the time it spent alive pressing on without getting anywhere (under 0.3 blocks in
1.5 s on foot, 0.5 in 2 s in a car), and the match's `search cells per tick`. `port/net/scripts/bot_bench.sh` (`BIN`,
`DATA`, `MAPS`, `SEEDS`) plays headless matches on downtown, mp1 and Industrial, two at a time, and sums them per map
and brain. With 10 seeds and hunter, soldier and simple twice each, stuck seconds per bot and 5 minutes went from 118,
104 and 60 (the three brains summed, downtown, mp1, Industrial) to 60, 74 and 68, frags about doubled, and search
cells per tick fell from 2200..15000 to 15..230 on average. Leaving a wedged car took the hunter's longest stuck
stretch from 96 to 14 s; its time stuck in a car (about 30 s per 5 minutes) is still the largest share.
**Behaviour line**: `net: arena <slot> <brain> behaviour alive A s armed G s fists F s dealing D in a car near one on
foot C s`: the time alive with a gun, unarmed on foot within 1.5 blocks of someone (and the damage dealt so), and in a
car within 6 blocks of someone on foot. The fighters (`simple`, the hunter on foot, `soldier`) remember the weapon
pickups they have seen by place (one missing while they stand within 8 blocks is taken, and left 30 s) and fetch the
best for its distance (machine guns, shotgun and rockets first); they hold the weapon that suits the target (rockets at
a car from 4 blocks, nothing that blows up nearer), and hold fire while it is in range and faced (every gun on foot
fires again while attack is held, `Weapon::CanFireWhileHeld`). Standing at someone, the fighter on foot (`simple`, the
hunter) turns the last 20 degrees by one-tick taps (12 units, 3 degrees; a held turn overshoots): a shot goes where the
shooter faces, or a player's machine gun's to the ped it locks on to (`Ped::ComputeAimAngle`, below in "Open").
Against a player standing still within a block (2 seeds), a hunter's shots that hit it went from 44 of 2080 to 140 of
212; between bots the frags stayed as they were (simple against simple, 8 seeds: 50 and 48; six bots on downtown and
Industrial, 6 seeds each: 159 and 166). Fists hit within a quarter block and only knock a player under 30
health down, so an unarmed bot punches only an unarmed target above that it hurt in the last 4 s, else it fetches a
weapon. **Avoiding cars** (`CarDodge`, its knobs at the top): a bot on foot sees each car where it was a reaction time ago
(8 to 14 ticks, 270 to 470 ms, drawn per bot and car), carried on at its speed then, so a car going straight is seen
where it is and one that turns is seen late. A car so seen whose path passes within 1.4 blocks in the next 30 ticks is
a threat, and so is a player's car within 4.5 blocks with its nose at the bot, however slow (a rammer backing off for
a run-up). One in four threats first seen outside its facing (90 degrees either way) goes unnoticed. For a threat, that
tick it steps off the car's path (its own side, or within half a block of the path the side it turns to sooner, with
1.5 blocks walkable in a straight line), or jumps when the car is under 12 ticks off or there is no room, pressing
jump every other tick so that it jumps again once landed (a jump clears the car: without the jump the run-over deaths
doubled). It doesn't fight while dodging (a throw under way stays held, a grenade let go at 85 ticks), and the rammer
and the hunter on foot on their way to a car dodge too. A car first seen near, one from behind unnoticed, or one that
turns late still runs it over. Against the old rule (no delay, 1 block and 20 ticks, one jump under 6 ticks), on mp1
and downtown with a rammer and two hunters against fighters on foot, run-over deaths per bot and 5 minutes went from
1.01, 1.08, 0.88, 0.63 to 0.59, 0.96, 0.73, 0.47 (soldier, simple, hunter, rammer; 128 seeds) and with `--kit smg` from
0.93, 1.48, 1.06, 0.80 to 0.56, 1.15, 0.90, 0.43 (64 seeds); the frags made from a car fell by 40% (the rammer's
match 3.1 to 2.3), the soldier's frags rose 8%, the simple bot's stayed, the hunter's on foot fell 5%. With the delay
and the old rule's moves the deaths were 2 to 3 times as many: the earlier, wider threat, the nose rule and the jumps
make up for it. Cars aren't in the walking map: a car's box (its sprite's size, its heading) within 3 blocks on the way makes the
walker head for the corner of it that keeps the way shortest and is clear, and jump over it when none is (stuck with a
car at hand, a jump is its first way out). The pickup memory starts with the weapon pickups the bot's own game has (the
map script makes them on both ends, listed every 10 s), so a client bot, which sees pickups within its area only, knows
the map's. Unarmed, a bot never punches near a car nor someone in one: it takes the car of someone driving slowly
within 8 blocks (the game pulls the driver out), else looks for someone on foot or a weapon. Molotovs and grenades are
thrown as the game throws them, as far as attack is held (measured standing: a molotov lands 0.68 + 0.0567 blocks per
tick held, 4.1 at the most from 60 ticks, 33 ticks later; a grenade as far, going off (96 - held) * 0.75 ticks later, in
the hand at 96): standing, turned to where the target will be when it lands, from 2.2 blocks (3 with a grenade) to
4.6, backing off holding on when it comes nearer; rockets from 4. A bot looking for a car with none in sight walks
towards the nearest player. Arena bots see what a client bot would: players and trains everywhere, cars, peds and
pickups within 24 blocks. The meter's lines `cars punched P s fists near one in a car F s stuck by a car S s round cars
R s` and `throws N nearer than 1.8 C farther than 5 F` (the nearest player when it threw) measure these, and `car
switches under 5 s S fire presses P held H s` how often it gets in or out of a car under 5 s after the last time, and
on foot armed its attack presses and the time attack was held (`bot_bench.sh` passes `ARGS` to the server: `--kit smg`). `idle I s longest L s retreats R s turnbacks T` counts the meter's windows it stood still in without pressing on or
firing, the seconds it went 1.5 blocks or more away from the nearest player within 10 blocks, and how often it went
back at the same one within 3 s of that; `bot_bench.sh` sums them with the frags. Over 12 seeds
on downtown, Tiny Town, Industrial and Residential (96 bots per brain): stuck 31.5, 18.9, 19.7 to 21.8, 6.3, 13.3 s per
5 minutes (hunter, simple, soldier; stuck by a car on foot 11.5, 14.9, 7.1 to 1.8, 1.6, 0.9), frags 535 to 717, throws
362 to 904 with 9% to 7% of them at someone nearer than 1.8 blocks and 51% to 16% with nobody within 5. A well armed hunter gets out of its car for someone on foot within 6 blocks, or with rockets for a car within 8;
the soldier gets out for a car only with rockets. Once in a car or out of one, the hunter, the soldier and the rammer
keep to that for 10 s before choosing again (a wedged car aside), and the soldier takes a car only with nobody within
10 blocks (it gets out for someone within 7): with `--kit smg` the soldier got in and out 27 to 44 times per 5 minutes
under 5 s apart, and the rammer, which doesn't fight on foot, got out to shoot and straight back in. With the kit, over
12 seeds on downtown and Tiny Town, frags went from 11.9 to 17.9 a match. Over 20 seeds on the three maps: frags 594 to
754, fistfight time 61, 27, 12 to 18, 17, 14 s per 5 minutes (simple, hunter, soldier), the hunter armed 112 to 208 s and
in a car near someone on foot 35 to 2 s; against rammers the simple bot's dodge took their frags from 265 to 162.
A fighter fires only with a clear shot: no car between it and the target along the shot's line (the target's own car
aside; with rockets, a car by the target counts as clear, its blast reaches it), and no wall (walkable in a straight
line). With a car in the way it steps 1 to 3 blocks aside, round the car's nearer end, to a spot it can walk to and
shoot clear from, else walks on at the target, the walker going round the car. A car on fire from `Car::mDamage` 25000
(the game's big fire, `mDamageLevel` 4, where its own drivers get out; its small fire from 16000 burns 1 a tick and
lasts minutes) makes the rammer, the hunter and the soldier get out before anything else, the 10 s commitment
included, and walk 2 s away from it until 4 blocks off; cars on fire and wrecks are not chosen to get into. The meter's
line `shots blocked by a car C s by a wall W s of A s aimed deaths in a car D in a burning car B s` counts fire held on
foot at the nearest player within 30 degrees and 12 blocks of where it faces, and of that with a car or a wall in the
way. Over 32 seeds on mp1 and downtown, with and without `--kit smg`: aimed fire blocked by a car 14..31% to 0, deaths
in a car 529 to 98, frags 2869 to 3099.
The rammer is the car brain and the simple fighter together, as the hunter is: in a car whenever it has one (it gets
out only when the car burns or is wedged, with a gun even with someone near, to fight there); on foot it takes a car
within 12 blocks (30 unarmed) unless it has a gun and someone was within 8 blocks in the last 5 s, and fights on foot
otherwise. The car brain (the rammer's and the hunter's) keeps its target until another is much nearer (someone on
foot counts as nearer), follows its route when far or round a block, aims at where the target will be when near,
backs off a second after a hit to ram again, and uses the handbrake to turn back at a target near and well off its
nose. The meter's line `car frags F far X s in a car C s` counts frags made in a car and the seconds alive with
nobody within 12 blocks and in a car. Over 64 seeds on mp1 and downtown, with and without `--kit smg`, the rammer's
frags went from 463 to 1097 (in a car 512 to 487), the cars it took from 3709 to 1969 and its idle time from 9.5 to
2.2 s per match; the hunter's and the soldier's frags rose, the simple bot's rose with the kit and fell 8% without.
The meter's line `dodges D unseen U run over R` counts the dodges a bot on foot began, the cars coming at it it never
noticed, and its deaths on foot within 30 ticks of a car moving 0.1 blocks a tick or more within 1.5 blocks of it (the
game keeps no cause of death: a shot by a car passing counts too); `bot_bench.sh` gives them per bot and 5 minutes.
A bot client still takes the keyboard's key events (Escape and the pause menu, the chat), so `GTA2_INPUT` can press
them in a test.

| Brain | |
|---|---|
| `simple` | walks its route to the nearest player and fights when close |
| `carhop` | gets into the nearest car, drives, gets out, again |
| `pulse` | for comfort's input to motion: in the nearest car, forward with a turn each way, braked to a stop, again |
| `rammer` | rams players in a car it keeps; on foot with a gun and someone near, or no car at hand, the fighter |
| `hunter` | the rammer when a car is near, nobody is and it has a good gun or none to fetch, the fighter otherwise |
| `soldier` | a weapon, a car, then to the nearest player: runs it over when coming at it head on, or gets out and shoots |
| `human` | for crash hunts, what people do and the others don't, in turns: cars, buses and the tank (in, about, out at speed), trains (on at a station, a ride, off), every weapon it has, pickups, into water, its own blasts (`session/bot_human.cpp`) |
| `nn:<file>` | a trained network (`session/bot_nn.cpp`): 62 inputs (`BotFeatures`), a choice in six groups of outputs |

In the round flow bots ready up and don't vote. With `GTA2_NET_BOTMAP2=1` (a test) a bot client joins as a person: it
votes map 2 in the results and yes on any vote but its own kick.

**Training** (`port/net/scripts/rl_bots.py`, PPO with torch): an **arena** (`--server --arena ... --ticks N`, bots
on the server itself, `port/net/dev/arena.cpp`) plays the trainer's `remote:<port>` slots over a local socket, a
decision every 3 ticks with the reward since; `GTA2_SPEED=1000` headless runs a 150 s match in under half a second.
Each remote slot also runs a scripted teacher on the same view, and an imitation loss on what it would press, fading
over the first updates, gets the policy started (from scratch PPO learned nothing). `rl_bots.py --watch [file.nn]`
plays a match in a window. Out: `build_mac/rl/` (`latest.nn`, `policy.pt`, `pool/`, `log.csv`). Head to head on Tiny
Town, the trained policy makes more frags than the simple bot and fewer than the hunter.

## Code layout

`port/net/`. **Only `engine/`, `eval/` and the frontend page (`menu.cpp`) include the game's headers**; everything else
works on plain data, so the decomp can move on (renames, struct fixes) while the netcode follows it in one place.
Dependencies go one way: `session` uses `core`, `transport`, `engine/engine.hpp` (plain types only) and the probes in
`dev/`; `engine` uses the game and `core`'s types; `dev` and `eval` use whatever they check; `net_main.cpp` sets the
hooks (`port/src/net_hooks.hpp`).

```
port/net/
  net_main.cpp    the session's lifetime: a server, a client, a replay, or a join from the frontend
  cmdline.cpp     the command line            maps.cpp   the .mmp files, the rotation, what a vote chooses from
  menu.cpp        the frontend's network page (game UI, so it includes the game's headers)
  core/           no game, unit-tested: the bitstream, the protocol, entity states and their delta coding, the
                  round's state, a map's players, the extra players' looks and start blocks
  transport/      the network: an interface, its ENet implementation, a link conditioner for tests
  session/        server.cpp, client.cpp, round_flow.cpp (no game: tested on its own), the bots (bot*.cpp),
                  recording.cpp, env.cpp (the GTA2_NET_* switches)
  engine/         the bridge to the game (engine.hpp: what the session sees; internal.hpp: what its files share):
                    netplay.cpp       the game's NetPlay/NetUi for the port: the frame hook, the network game's start
                    snapshot.cpp      a snapshot captured and applied, the game's controls, shared helpers
                    schema.cpp        every entity type's fields       world_entity.cpp  the world entity
                    players.cpp, cars.cpp, car_ai.cpp, ped_state.cpp, pickups.cpp, trains.cpp, shots.cpp   the entities
                    shot_visuals.cpp  other players' shots as a client shows them
                    replica.cpp       a client's cars and peds          car_entry.cpp     into and out of cars
                    slots.cpp         joining and leaving               spawn.cpp         the server's spawn budget
                    client_mode.cpp   what a client doesn't run         guards.cpp        the game made safe
                    world.cpp, rollback.cpp, sound_pool.cpp             the rollback set, save and resimulate
                    rounds.cpp        new rounds, map changes           overlay.cpp       what a client draws over the frame
                    stats.cpp         the periodic log's statistics     navmap.cpp        the map for bots
                    sound_log.cpp     a client's sound over rollbacks  starts.cpp        the players' peds and start blocks
                    dev/              checks, logs and test cheats (dev.hpp), off unless switched on
  dev/            the session's checks and test hooks (client_probe.cpp, server_probe.cpp), the arena, the jerk meter
  eval/           measurements behind the design (sim_bench.cpp, neteval.cpp)
  tests/          the unit tests (ctest)
  scripts/        the network check, the stress run, the divergence tools, the bots' training
  enet/           ENet 1.3.18, vendored (MIT)
```

Outside `port/net/`: the hooks (`port/src/net_hooks.hpp`), the random draws' owners and streams (`port/src/rand/`), and
the registry of every global and static (`port/src/port_globals.hpp`) that the rollback set is made from; a file or
function static is reached through it by its 10.5.exe address (`PortStatic`).

The server and the client call their probes (`dev/server_probe.hpp`, `dev/client_probe.hpp`) at fixed places in
their frame; the engine's dev functions are `engine/dev/dev.hpp`. Everything in `dev/` is linked in and off unless
its switch is set.

### The game's code

The netcode's hooks, declared in `port/src/net_hooks.hpp`; those in `Source/` are under `GTA2_PORT`:

| Where | Hook | |
|---|---|---|
| `Game.cpp` | `gPortAfterPlayersHook` | after the players' tick, before anything spawns: every player's view, the same on both ends |
| `Game.cpp` | `gPortExtraPlayerPeds` | at boot, after the level start: a ped for each player past the script's `PLAYER_PED`s |
| `Ped.cpp` | `gPortSpawnBlock` | a ped starting at a restart zone: a block of it no other player stands on |
| `Game.cpp`, `sound_obj.cpp` | `gPortWorldStill` | the results: nothing runs but the players' tick and the HUD; the world's sounds stop |
| `Game.cpp` | `gPortDrawOverlay` | drawn last: the status, the round's panel, chat completion |
| `Car.cpp`, `Char_Pool.cpp` | `PortTrafficLimit()`, `gPortSpawnPlayers` | the spawn limits times the players |
| `port/src/options_menu.cpp` | `gPortFrontendSetup`, `gPortFrontendInput` | the network page, beside reG2's options |
| `winmain.cpp` | `gPortNextNetworkGame`, `gPortLeaveNetworkGame`, `gPortNetFrameLoop`, `gPortNoDraw` | a map change goes to the next game; a client that left goes to the network page; a client runs reG2's frame loop; a headless server draws nothing |
| `map_0x370.cpp` | a file static in port builds | the restart zones' turn, a function's static in the original: registered from the start, so rolled back and sent |
| `sprite.cpp` | `gPortSpriteMovedHook` | a sprite moved: the grid check (`GTA2_NET_CHECKGRID`) |
| `sprite.cpp` | `gPortSpriteFreedHook` | a sprite freed: the stale link check (`GTA2_NET_CHECKSTALE`) |
| `port/src/rumble.cpp` | `gPortNoRumble` | no vibration on a server (its local player is a remote one) or in a client's resimulation |
| `sound_obj.cpp` | `gPortSoundRequestsHook` | a client's sound log: what a frame asks for, and what resimulated ticks owe added to it |
| `port/compat/src` | `gPortIdleHook`, `gPortNoAudio`, `gPortCrashHook` | polling while a frame waits (`window.cpp`), no sound on a server (`miles.cpp`), the crash report (`main.cpp`) |
| `Ped.cpp` | `gPortKeepWantedPoints` | a client keeps the server's wanted points with its police off |
| `ScriptThread.cpp` | `gPortScriptCommandRuns` | a client runs the level start's look and sound commands, and steps over the rest |
| `Car.cpp` | `gPortUpdateCars` | the car pool's update, a car ahead of one following it first ("Cars in turn") |
| `Car.cpp` | `gPortCarMayExplode` | a client's car the server sends blows up when a snapshot says the server's did, not on its own prediction |
| `Car.cpp` | `gPortCarHornId` | a client's car the server sends honks and its driver shouts by the server's id (`Car::TryHonkHorn` picks the cars by its low bits) |
| `Object_5C.cpp` | `gPortObjectMade` | an object made: a network game keys the shots it predicts (`engine/shots.cpp`) |
| `Explosion.cpp` | `gPortBlastHurts` | a client's blast made from a snapshot hurts nothing: the server's did |
| `char.cpp` | `gPortCharSolid` | a client's player whose death it holds collides with nothing, as the server's corpse |
| `Explosion.cpp` | `gPortBlastDebris` | a large blast's burning debris, seen or not; on a client none from a blast it didn't predict |
| `Car.cpp` | `Car::TryHonkHorn` | a train's lead without a driver (a player got out of a carriage that stood still) doesn't read it |
| `Hud.cpp` | `port_TypedChar` (`port/compat/src/dinput.cpp`), `gPortChatChar` | the chat types the keyboard layout's characters; a server, those its clients typed |
| `Hud.cpp` | `gPortServerKillNotices` | a client's kill notices are the server's deaths only |
| `Hud.cpp` | `gPortDrawNetStandings` | a client's rank and score at the top left, in place of the list of the other players and the game's frags or tag time counter |
| `Hud.cpp` | `PortDrawPlayerNames` (`port/src/player_names.cpp`) | the names over the other players: centred over the ped (its car in one) where the frame draws it, by the drawn camera, at three quarters of the HUD's text size. The original's are placed by the game camera, which the frame doesn't draw between steps, and by the HUD's layout, whose edge anchors move them on wide screens |
| `Hud.cpp` | `PortHudTextPalette` (`port/src/player_looks.hpp`) | the names over the players and the tag times in the port's looks' colours (the fonts have 9 remaps) |
| `Player.cpp`, `Hud.cpp` | `PortLookOf` (`port/src/player_looks.hpp`) | each look's HUD colour and arrow from one table, three more looks than the original's |
| `Player.cpp` | `gPortKillCamTicks`, `gPortDeathViewPed` | the kill cam: a player wasted by another has a longer death countdown (`Player::Wasted`), whose end its view spends on the killer (`Player::Service`, camera mode 2) |
| `Player.cpp` | `gPortCorpseKept` | a wasted player's corpse stays at its respawn as a dead pedestrian (`Player::Wasted`), freed off screen as the others |
| `TagGame.cpp` | `gPortTagTimeCounts` | the tag game counts the "it" seconds without the game's time limit, which the original counts by |
| `sound_obj.cpp`, `Player.cpp`, `char.cpp` | `gPortPlayersDeathCry` | a player cries out at its death as the other peds do (`HandlePedVoiceEvent`, voice event 4), from its corpse, or from its car when it died in one (`ProcessCar`); a fatal landing or lethal fall has the cry instead of the thud, on a client too, where its `Ped::Kill` is held; the cry has a sample index of its own (16), apart from the voice's; the cry is first in the mixer's queue (priority 0) to its end (`AddReleasingSounds` leaves its priority); the ped respawned elsewhere (`Player::Wasted`) drops the event |
| `sound_obj.cpp` | `gPortFullQueueRanked` | a sample asked for when the mixer's request queue is full is ranked among all the others (`AddDetailsToRequestedOrderList`); the original's search stops at the slot it takes and loses the sample at that place |
| `Object_3C.hpp` | `gPortGroundedObjects` | a new object isn't airborne by its pool slot's previous one, which the server and a client don't share |
| `PlayerScoreTracker.cpp` | `gPortServerKillAwards`, `gPortScoreBonusHook` | a client's kills award no points or bonuses: the scores are the server's, and a bonus's message and voice are its player's client's, from the server |
| `Pool.hpp`, `Car.cpp`, `Char_Pool.cpp`, `Game.cpp`, `Ped.cpp` | `PortRand::Scope` (`port/src/rand/owner.hpp`) | who a random draw belongs to (a robbed driver's: its car), and who makes a shot |
| `sound_obj.*`, `ParticleManager.cpp`, `Particle_4C.cpp`, `Explosion.cpp`, `Weapon.cpp` (`DrawElectricArc`) | `port/src/rand/sound.hpp`, `particle.hpp` | their own random streams; `SoundPosEntry` from the pool; `sound_obj`'s state public for the rollback |

The game's own multiplayer assumed one machine per player in places; under `GTA2_PORT` a network game fixes them:
the view (`winmain.cpp`'s `ReadScreenSettings`, `PortCameraZoom`) and the traffic spawn (`Car.cpp`) above, the police
updating every pursuit and not the first criminal's alone (`Police.cpp`), and the nearest player instead of
`mLocalPlayer` (on a server, slot 0) for the roadblock tank's aim and a roadblock's removal (`Ped.cpp`,
`Police_38.cpp`, `Game::PortNearestPlayerPed`) and a group's gang respect (`Ped.cpp`). The tag game's tick, run after
the game's tick outside `Game::UpdateGame`, runs in a resimulated tick too (`engine/rollback.cpp`).

Other files' statics the netcode reads (winmain.cpp's inputs and its player, the restart zones' turn) it reaches through
the registry by their 10.5.exe address (`PortStatic`, accessors in `engine/internal.hpp`).

reG2's own extras that would change the game outside the input bits a server gets are off in a network game: twin stick,
mouse aim, the turret's aim, the pad's analog driving and the aim camera (`port/src/twin_stick.cpp`), each
player's day and night (`day_night.cpp`) and the pause menu's pause (the menu opens over the game going on, as the
client's: "Playing"). AUTOSAVE, RETRY JOBS and FRENZY RESPAWN
(`autosave.cpp`, `retry.cpp`) are for single player only, and INFINITE LIVES goes with the debug flags a network game
clears (`InitializeGame`). Vibration is the local player's, from the ticks a client predicts (`gPortNoRumble`). A
client runs reG2's frame loop and interpolation ("The game loop"). The GRAPHICS settings only draw: they read the game
and draw no random number from it (FIRE EFFECTS hash their own), so they stay on, the fire effects placed by the drawn
player's view (`view.cpp`) and timed by the steps the client ran, not by its resimulated ones.

## Checks

| Check | | |
|---|---|---|
| Unit tests | `ctest --test-dir build_mac -L net`; without the game or SDL3: `cmake -S port -B build_nettests -G Ninja -DGTA2_GAME=OFF` (with UBSan: `-DCMAKE_CXX_FLAGS=-fsanitize=undefined -DCMAKE_EXE_LINKER_FLAGS=-fsanitize=undefined`), then `ninja` and `ctest` there | the core's coding, the transport over loopback, the bots' navigation, the round flow |
| The network check | `port/net/scripts/net_check.sh [--quick] [step...]` (`BUILD=<cmake build folder>`, `build_mac` by default; `GAME=<game folder>`, `$BUILD/game` by default), about 7 minutes | PASS or FAIL per step, below |
| The fuzz test | `port/net/scripts/fuzz.sh [-s seconds] [-r per mille] [-p map] [seed...]` (`CLIENT=<an AddressSanitizer build's reg2>`), a minute a seed | no crash with random snapshot fields ("Security") |
| The stress run | `port/net/scripts/stress.sh [-m minutes] [-n servers] [-s speed] [-p map] [-c seconds] [-g]`, 10 minutes by default | crashes and `CHECK` lines per process, below |

`net_check.sh`'s steps, on the macOS build; logs and fixtures in `build_mac/net_check/`:

- **arena**: 24 bot matches of 5 minutes on the server alone (three maps, every brain, random seeds): none may crash.
- **live**: a server and two bot clients at 100 ms, jitter and 2% loss, police on, with the grid check: no crash and
  no `net: CHECK` line, and the server's recording replays to the same world at every tick. Of the ticks a server bot
  stands with a gun and the fire held (`GTA2_NET_POSELOG`), a client shows at least 80% in the firing pose. Prints the
  corrections and the comparison check's worst fields.
- **churn**: two bot clients stay while two others join and leave four times (stopped by SIGTERM and Ctrl-C): no
  crash, no `CHECK` line, nobody turned away as the server being full.
- **maps**: a three-map rotation, 15 s rounds at 4 times the clock: both clients follow every change, and play the
  resimulated ticks' sounds on a later map.
- **rounds**: two bot clients ready up, play a round, vote for map 2 and follow the server there (a check of where
  the players stand: on the new map); a third joins during the results and plays round 2.
- **soak**: a server and three bot clients at 10 times the clock: 10 minutes of play in 60 s, no crash.
- **throws**: blasts at a server bot's feet and a block away (`GTA2_NET_TESTBLAST`, none while it is invulnerable after a
  respawn: a test blast is the server's alone, and an invulnerable bot walks into it again and again), another bot set on fire, a bot
  client watching with the blend off: every tick it shows a player thrown, the server had it thrown there with the
  same outcome, once a snapshot it applied had the
  tick that throw started at, the server's or its own (before that, it shows its own prediction of that player). The server listens at the client's player
  (`GTA2_NET_SFXLOG=2`): neither end plays a player's fatal landing thud.
  The burning bot is also electrocuted and drowned (`GTA2_NET_TESTPHASE`): the client still shows it in that state at
  the tick the server's dies. It is also put in a car and pulled out of it: the client shows it at the door only on ticks the
  server has it there, never seated or walking; then the client's player jacks a player out of a car twice
  (server's `GTA2_NET_TESTPHASE` code 4, `HeldPull`): the server calls the first jack off at the door, and pulls on the
  second after 10 ticks there. The client shows both pulls at once, the first back in its seat within 12 ticks of the
  call-off, the second no later than the server. The client's own player is drowned and electrocuted at times
  where the server never has it (`GTA2_NET_TESTLOCALPHASE`, on every run of that tick, live and resimulated): the
  snapshots cancel it within 20 ticks.
- **peds**: the ambient ped nearest the client's player drowned, electrocuted, knocked down or blown up in turns
  (`GTA2_NET_TESTPHASE`), the client driving into peds, the blend off: at every snapshot tick, as its apply leaves them,
  the client's peds on foot have the server's states and phase there, or are shown dead ahead of it; none is a corpse
  (health 0) on a live tick before the server's death, one shown dead dies there or gets up within 40 ticks, and no
  corpse of the client's own is made again.
- **shots**: armed bots, a server's own rocket, grenade or molotov beside a bot every 60 ticks (`GTA2_NET_TESTSHOT`)
  and a human:fight bot client at 60 ms, the blend off, with a rocket of its own the server never fires
  (`GTA2_NET_TESTGHOSTSHOT`): the shots it predicted keep their keys (no more than one in five made or removed within
  the reach; a burning debris made is the server's, of a car's blast or one beyond the reach), it makes the server's own, and the ghost goes at the next snapshot and never shows again
  (one that hit a wall at once is gone by itself). The game's random stream is the server's at every snapshot but the
  first (`GTA2_NET_COMPARE`), with blasts on the server's screen and not the client's. Red with `GTA2_NET_SHOTSKEW=1`.
- **replay**: recorded sessions replayed against their saved logs: the same, or nondeterministic (a second replay
  differs too), else a regression. `--record` makes new ones; `--save` takes the current build's logs as the baseline
  after an intended change.

**The stress run** is for bugs that take a while to show: several servers side by side (3), each with four bot
clients, a fresh game every round on one map, the clock 4 times as fast, with every check on: the grid check and the
sprite check on the clients and the servers, the snapshot check (`GTA2_NET_SNAPCHECK`), and the clients recording.
It ends with each process's crash (its backtrace and the ped and car last updated) and its first `CHECK` line; the
logs and recordings stay in `build_mac/net_stress/<n>/`. `-g` runs everything under Guard Malloc
(`DYLD_INSERT_LIBRARIES=/usr/lib/libgmalloc.dylib`: each allocation on its own pages, freed ones unmapped), so a read
or write of freed memory crashes where it is made (slower: `-s 2`). AddressSanitizer doesn't start on current macOS.

**The snapshot check** (`GTA2_NET_SNAPCHECK=1` on the server): each snapshot ends with a 16-bit hash of every entity
the client should have after reading it, and the client checks each one (older clients ignore them). One it got wrong
is named at once (its first time), with how it came (carried from the baseline, updated or created), the baseline's
tick and its fields against the baseline's; the client tells the server, which prints what it sent then from the
snapshots it keeps. `port/net/scripts/snapdiff.py build_mac/net_stress/<n>` pairs the two: the fields that differ.
A field the server sent unchanged that the client has otherwise was written in the client's memory since it came.

**The crash report** (`port/compat/src/main.cpp`, installed for network games only, then the netcode's `DebugOnCrash`): the backtrace, then the frame and tick, the
rollback block being saved or restored if that is where it was (`gCopyingBlock`), the ped and the car whose update
started last (`PortRand::gLastPed`, `PortRand::gLastCar`: a crash in a ped's or a car's update names
it), and on a client its player, cameras and their cars. It allocates nothing, and if it hangs anyway (a lock the crash
left held) an alarm ends the process with its signal after 5 s.

**The comparison check** (`GTA2_NET_COMPARE=1` on a client): on each snapshot, with the game restored to the
snapshot's tick and before it is applied, every mapped entity is captured here and compared with the server's, field
by field; every 300 frames the fields that differ most and how often (the cops apart, and the paramedics on a line of their own). A measure, not a pass: remote players and AI
peds are predicted, so they differ. A line of its own counts the snapshots at which the game's random stream was the
server's: it should be all of them.

**The divergence check** (`GTA2_NET_DIVERGE=<server tick>[:<ticks>]`, 300 by default): the client reconciles up to that
tick, then runs on its own; each later snapshot is compared with its save of that tick, and at the end it prints per
field after how many ticks each entity first differed. Below the fields, `GTA2_NET_DUMPPEDS` dumps every ped's bytes
on both ends (`port/net/scripts/ped_diff.py` names the members that differ) and `GTA2_NET_RANDLOG` each `rand()` draw's
caller and whether an owner drew it (`port/net/scripts/rand_diff.py` counts them per tick and function, `--stream` the
game's stream's only). With `GTA2_NET_COMPARE` a client logs them too, prints each tick at which the game's stream isn't the
server's, and `rand_diff.py --tick <tick>` lists that tick's stream draws on both ends.

### Environment variables

The session's (`session/env.cpp`) and the engine's (`engine/dev/checks.cpp`, the dumps in `engine/dev/compare.cpp`), all
off by default:

| Variable | Where | Effect |
|---|---|---|
| `GTA2_NET_RECORD=<file>\|off` | both | the recording's file (default `recordings/session-<n>.rec`, a server's `recordings/server-<n>.rec`) |
| `GTA2_NET_DUMPWORLD=<tick>[,<tick>...]` | server | every entity of the world at those ticks, with its fields (live or in a replay) |
| `GTA2_NET_REPLAYON=<frames>` | replay | the game runs on that many frames after the recording ends, nothing arriving |
| `GTA2_NET_STATS=1` | client | the connection beside the ping, over the last second: the snapshots lost (the server sends one a tick), received per second, the ticks the game runs ahead of the newest, and the ticks resimulated per rollback |
| `GTA2_NET_NOFORWARD=1` | client | other players predicted by their repeated input only, not the forwarded controls |
| `GTA2_NET_HOLDFIRE=1` | client | other players' held fire predicted too, past the forwarded controls |
| `GTA2_NET_HOLDALL=1` | client | other players' last input held through a stall, not let go |
| `GTA2_NET_CUT=<every ms>:<ms>` | either end | with the link conditioner on (`--lag 1`): every unreliable send lost that long that often |
| `GTA2_NET_RESIMSAVES=<n>` | client | the resimulated ticks saved again after a rollback: the first n (4; 0: all) |
| `GTA2_NET_BLEND=<0..1>`, `_BLEND_FAR`, `_BLEND_SELF` | client | how far a correction is blended in each tick: a small one (0.3), one near 2 blocks (0.5), the local player's own (0.5); 1: at once |
| `GTA2_NET_COMPARE=1` | client | the comparison check |
| `GTA2_NET_DIVERGE=<tick>[:<ticks>]` | client | the divergence check |
| `GTA2_NET_CORRLOG=1` | client | corrections over half a block or 45 degrees |
| `GTA2_NET_JERKLOG=1\|2` | both | how jerky what is shown moves (2: and each jump), and how far its way is from its facing and how fast it turns (on a client also where the newest snapshots have the two agree) |
| `GTA2_NET_COMFORT=1` | both | what a player feels, for `port/net/scripts/comfort.sh`: deaths and knock-downs, shots and drawn moves by tick, on a client also its input's response and the corrections (`comfort:` lines) |
| `GTA2_NET_CARTURNS=0` | both | the game's own car order ("Cars in turn"), to compare: set on the server and its clients alike |
| `GTA2_NET_MARKER=1` | client | a square where the newest snapshot has the local player |
| `GTA2_NET_SFXLOG=1` (client), `=<slot>` (server) | both | every sample the mixer starts, by tick, what made it and where; a server with it opens its audio device and listens at that slot's player (to count what a client misses or plays twice) |
| `GTA2_NET_INPUTLOG=1`, `_ANIMLOG=1`, `_SELFLOG=1`, `_SOUNDLOG=1` | client | the input, the other players' animation, the local player around a death (and the cars around its view, its camera) and the server's hits on players, the sound's channels and the car radio |
| `GTA2_NET_CARDIFF=1` | client | where the prediction of the local player's car differs from the snapshot |
| `GTA2_NET_HITLOG=1` | both | every tick, each player in a car: its place (`DRIVE`); on a client also each impact of the local player's car on a ped as it happens (`CARHIT`, live ticks only) |
| `GTA2_NET_FWDLOG=1` | client | at each snapshot, every other player's controls the server forwarded for the ticks after it (`FWD`): what it pressed, for the ticks the client guessed |
| `GTA2_NET_FRAMELOG=1` | client | each frame: the time since the last one, its step's, its reconciliation's and the ticks resimulated (`FRAME`) |
| `GTA2_NET_GANGLOG=1` | both | player 0's group every 15 ticks, by the server's ids |
| `GTA2_NET_STACKLOG=1` | both | two cars in one place (under 0.4 blocks apart), once per pair: a car the client kept that the server dropped |
| `GTA2_NET_PICKUPLOG=1` | client | pickups made and removed |
| `GTA2_NET_THROWLOG=1` | both | every tick, the players thrown, knocked down, dead or on fire (their states, the throw's object and motion, the player the game blames: `by p<slot>`) and the explosions just made |
| `GTA2_NET_SHOTLOG=1` | both | every tick, the shots by key (model, blast type and timer, owner, place); on a client each shot made, removed or beyond the reach at a snapshot, and each other player's shot slid or flashed (`SHOTVIS`, by the frame GTA2GL_CAPTURE numbers) |
| `GTA2_NET_PEDLOG=<player>` | both | the peds within 12 blocks of that player, their states and phase: every tick, and on a client also as each snapshot's apply left them |
| `GTA2_NET_FIRELOG=1` | both | every fire at every 30th tick of the game's cycle: a car's (its id, wrecked or not, its fire's explosion types), the ones on nothing by place, the peds on fire |
| `GTA2_NET_POSELOG=1` | both | every tick, each player on foot: its fire held (`Ped::mBf21C.bAttacking`, the firing pose), its weapon, animation and whether it moves |
| `GTA2_NET_TRAINLOG=1` | both | every train's place and state at every 150th tick of the game's cycle (the same tick on both ends) |
| `GTA2_NET_CHECKGRID=1` | both | the collision grid checked after every move, a sprite moved off the map reported with the code that moved it, a rollback block whose pointee moved since the set was taken, on a client a ped driving a car that has another driver or leaving it through another car, and on both the weapon pool's lists looping or sharing a weapon and a ped holding a weapon it would free wrongly (a freed one, a player slot's, a car's, another ped's), a tank, fire truck or gun jeep without its roof turret, and a collision grid row that isn't a sorted list of pool cells and links, after every tick, restore and apply (`net: CHECK` lines) |
| `GTA2_NET_CHECKSTALE=1` | both | no sprite grid links a freed sprite (id 0), after every tick, restore and apply: each one found printed with where its sprite was freed (its kind, owner, model and backtrace), and a count every 20000 checks (`net: CHECK` lines) |
| `GTA2_NET_SNAPCHECK=1` | server | every entity's hash in the snapshots, checked by the clients, and the snapshot sanity check run on the server's own world (`net: CHECK` lines) |
| `GTA2_NET_DUMPPEDS=<file>[:<from>:<to>]`, `GTA2_NET_RANDLOG=<file>:<from>:<to>` | both | the divergence check's dumps |
| `GTA2_NET_NAVDUMP=<file.pgm>` | client bot | the bots' map as a picture |
| `GTA2_NET_TESTKILL=<tick>:<victim>:<killer>,...` | server | a player killed by the game's own death |
| `GTA2_NET_TESTPOWERUP=<tick>:<player>:<power-up>,...` | server | a power-up (12: an instant gang) |
| `GTA2_NET_TESTFIRE=<tick>:<player>,...` | server | the player set on fire |
| `GTA2_NET_TESTBLAST=<tick>:<player>:<tenths>,...` | server | a grenade's explosion that many tenths of a block east of the player (0: at its feet, a flying death) |
| `GTA2_NET_TESTSHOT=<tick>:<player>:<model>,...` | server | a rocket (128), molotov (138) or grenade (183) thrown east from beside the player, made by the server alone |
| `GTA2_NET_TESTGHOSTSHOT=<tick>` | client | a rocket beside the local player at that tick of the game's cycle, in every run of it: one the server never has |
| `GTA2_NET_SHOTVIS=0` | client | other players' late shots drawn where they are, their muzzle flashes only while the world still has them ("Other players' shots shown") |
| `GTA2_NET_SHOTSKEW=1` | client | its shots' keys none of the server's (the shots step's red run) |
| `GTA2_NET_TESTPHASE=<tick>:<player>[:<what>],...` | server | the player on foot drowned (0) or electrocuted (1), seated in a car beside it (2), pulled out of its car (3), its car moved into mp1's water at (54.5, 160.5) to sink (5); 10 to 13: the ambient ped nearest it drowned, electrocuted, knocked down, or blown up |
| `GTA2_NET_TESTLOCALPHASE=<tick>:<what>,...` | client | its own player drowned (0), electrocuted (1) or knocked down (2) before each run of that tick, live and resimulated (the server never has it) |
| `GTA2_NET_TESTPATIENTS=<tick>:<player>:<n>[:<blocks>],...` | server | the n ambient peds nearest the player (at least that many blocks away) set on fire: the ambulance comes for their corpses |
| `GTA2_NET_TESTSTREAK=<tick>:<player>,...` | server | a kill streak's bonus, as the 20th kill in a row gives it |
| `GTA2_NET_TESTCAR=<tick>:<player>:<model>,...` | server | a car beside the player (11: a bus; 63, 64: a truck cab with a container trailer); model + 1000 * n: damaged by n * 1000 (n = 16: on fire, 25: burning); a model the game can't wreck (24 to 26, 85: the HOTDOG_D vans; the wrecks) isn't spawned |
| `GTA2_NET_TESTTRAIN=<tick>:<player>:<train>,...` | server | the player at the wheel of a train; train + 10: that train into reverse; train + 20: the player on the platform of the station it stops at next |
| `GTA2_NET_TESTARM=<weapon>:<ammo>[,...]`, `GTA2_NET_TESTWANTED=<points>` | server | every player kept armed (car weapons from 15: the car it drives), or at least that wanted (600: a star) |
| `GTA2_NET_SCOREBOARD=1` | client | the scoreboard shown all the time, as if SCORES were held (captures) |
| `GTA2_NET_BOTIDLE=<tick>:<tick>` | client bot, server | the bot (a server's: its arena bots) presses nothing from the first tick to before the second (net_check's throws step: the server's test phases and jacks) |
| `GTA2_NET_BOTMAP2=1` | client bot | it joins as a person and votes map 2 in the results (net_check's rounds step) |
| `GTA2_NET_BOTCARFIRE=1` | both | bots fire their car weapons while driving (a second in three) |
| `GTA2_NET_BOTTURRET=1` | both | bots turn their roof turret while driving (a second in three) |
| `GTA2_NET_SAY=<frame>:<command>;...` | client | commands sent at those frames |
| `GTA2_NET_FUZZ=<seed>[:<per mille>]` | client | that many of each snapshot's fields (20) turned into random values of their width (the fuzz test) |
| `GTA2_SIMBENCH=...`, `GTA2_SIMBENCH_FILL=<cars>`, `GTA2_NETEVAL=...`, `GTA2_OFFLINE_MP=...` | game | the measurements below (`port/net/eval`, `engine/netplay.cpp`) |

`GTA2_SPEED=<k>` runs the game's clock k times faster, `GTA2_HEADLESS=1` without window content, sound or gamepads
(`port/compat/src/kernel.cpp`).

## Measurements behind the design

Native macOS, arm64. The world's size and save time are this build's (RelWithDebInfo); the rest were measured on
the gta2_re port's build the netcode came from, with the same game code. `GTA2_SIMBENCH=<flip>[:<ticks>]` (`port/net/eval/sim_bench.cpp`) runs that many ticks back
to back without drawing, prints the cost of each service, the world's size and its save and restore time, and checks
the rollback; `GTA2_SIMBENCH_FILL=<cars>` fills the map with traffic first, by the game's own spawner with its caps
lifted. `GTA2_NETEVAL` (`port/net/eval/neteval.cpp`) measures what changes per tick (`churn`), how fast two runs
drift apart (`diverge`), the random draws (`rand`), sound in a rollback (`soundrollback`), a projectile advanced on its
own (`single`) and rollback in scenes with explosions (`explode`).

| | |
|---|---|
| The world | 1,684 blocks, 3.2 MB; save 0.19 ms, restore 0.16 ms; the rollback reproduces it exactly |
| A busy tick (73 cars, 39 peds, 176 objects) | about 30 µs: cars 52%, peds 28%, objects 13%; a 10-tick resimulation is about 2% of a 60 fps frame |
| A full map (4 times the pools, built for the measurement; 983 cars, 661 peds, no cleanup) | 0.9 ms a tick, slightly worse than linear; the collision grid's link pool is the first to run out |
| What changes per tick | about 680 bytes for the whole attract demo's world, half of it in one interest area: about 500 bytes a snapshot |
| Drift of a client's own simulation | within 330 ms nearly everything stays within a pixel; peds drift first (46% by a pixel after 1 s) |
| ENet 1.3.18 | the largest unfragmented unreliable packet at `mtu` 1400 is 1372 bytes; a larger one goes as reliable fragments (seconds of delay under loss) |
| A link at 150 ms, ±10 ms, 5% loss | commands 2 ticks early, none late; about 9 ticks resimulated per snapshot; the local player's prediction error 0.00 blocks |
| A link at 300 ms (Linux, x86_64 desktop) | 12 ticks resimulated per snapshot: a reconciliation takes 3 ms a tick on average, 6 ms at worst, three quarters of it the 12 saves of the world (LAN: 0.6 ms); in the client's log every 300 frames |

## Open

- **The machine gun's lock on a shooter behind**: a player's machine gun aims at a ped within 16 degrees of the
  facing, else (`Ped::FindBestTargetPed`'s fallback) at the nearest ped on the threat list within 4 blocks and 1 level,
  whatever the facing. A ped is on that list when it fired a gun other than the silenced SMG on the tick before. With
  everyone on machine guns (`--kit smg`) two players holding fire keep their 3-tick rhythm, so the lock takes every
  shot or none: firing behind you is impossible while someone near shoots at you. The original's, solo and network
  alike (checked in solo against FBI agents with shotguns, 2026-11-25). To drop it, a `GTA2_PORT` block in
  `FindBestTargetPed` returning no fallback target for a player ped, in network games or behind a setting.
- **Typing on the network page with a gamepad**: the address and name take the keyboard only.
- **A blast's particles** are drawn only on screen and while the particle pool has room, and they set the debris'
  way: the two ends differ there until the snapshot (it brings the debris). The game's other on-screen tests spawn or
  despawn (traffic, peds, buses, roadblocks): a client runs none of them.
- **Other players at car doors**: most of the remaining large corrections of other players are with the server at a
  door. Next: a remote player the server has at a door put there at once instead of walked there by its predicted
  inputs, measured on recorded sessions (`GTA2_NET_COMPARE`, `GTA2_NET_JERKLOG`).
- **Cops** differ the most of any entity (health from hits a client predicts, animation and aim on foot): corrected by
  the next snapshot; worth a look if players see cops jump.
- **The police are the original's**: one city for every criminal. Four pursuits for six players, and the most wanted
  player's level decides the cop cars in everyone's traffic, the roadblocks and the crews (`--police N` above 1 shows
  it). The ambient peds' density cap is also one count for all the players' screens.
- **An instant gang doesn't board a bus** (on the server, so the game's own AI): the members walk to the doors and stay
  there; normal cars work. To check against a traffic bus, then the original under wine.
- **The game's results page** instead of the overlay's text; a minimum of players that counts people only.
- **Modes** beyond the game's frags, points and tag (team deathmatch, CTF) as a mode module, reusing the game's assets.
- **Hardening**: the game's loaders (GMP, STY, SCR) on a hostile map pack: port-only checks in their write paths
  first (a sprite delta's apply, `LoadMissionScript`, `LoadStringTbl`), then a validator; rate limits on a server;
  the update check runs `curl` by name, which Windows looks for in the game's folder first (its full path,
  `%SystemRoot%\System32\curl.exe`, would close it); a ped's objectives beyond the ones checked whose target the
  game reads unchecked; a remote player's state against its own objectives.
- **Tick rates above 30 Hz** would need every per-tick constant and timer scaled (a project of its own, port-only).
- **Whole-map simulation**: a server could keep everything everywhere, with a cleanup policy as a server setting
  (none, wrecks only, the game's own). Every cleanup and spawn decision goes through `Game::IsSpriteInPlayerBounds`
  and five places (car despawn, ped and corpse removal, the object caps, the traffic and ped caps); the pools would
  need allocation that can fail or eviction before every spawn.

### Setting up games (plan)

Both ways players know: **classic** (start a server, open a port, tell friends the address) and **central** (a master
server lists them). One list, several views: the master's list is a public JSON API that a web app, the game and
anyone's tools read.

1. **Hosting from the menu**: a "Host game" page that starts a server in the background and joins it; the LAN and
   public address to share; UPnP / NAT-PMP to open the port, and a reachability test; an optional password;
   `server.ini` for a server's settings.
2. **The data check** (done: the build, `nyc.gci` and the map's files at the join and at a map change, "Hosting a game").
3. **The master**: a heartbeat from `--public` servers over their game socket (so the master sees the address after
   NAT), verified before listing; the web app's list with join links (`gta2://host:port`).
4. **In the game**: an info query every server answers outside ENet (name, map, players, version: real pings), LAN
   games by broadcast, recent servers.
5. **Connectivity**: NAT punch-through through the master, a relay as the last resort.
6. Admin (kick, ban, map, say), quick play, a server package (a static Linux build, a Dockerfile).
