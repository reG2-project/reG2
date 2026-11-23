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
a frontend page to join, bots (scripted and trained), recordings that replay without a server. Protocol 30.

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
| Client | `--connect host[:port]`, `--name`, `--bot [brain]` (simple, carhop, rammer, soldier, hunter, `nn:<file>`), `--replay <file>` |
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
(`core/spawn_spots.cpp`). Slot 0 starts at the centre, the game's own spot. The choice reads only the map, not where the
other players are, so a client predicting its own respawn picks the server's block whatever it sees of the others.

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
last inputs held for 5 ticks after the last snapshot, then without fire, then from 10 ticks on none. When ENet drops
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
   Then the next round's fresh game, on the most chosen map (a tie: one of the tied, at random), else the rotation's next,
   else the same map.

**Votes**: `/start` (end the warmup), `/map N` (change now; a round ends without a winner), `/kick NAME` (out, and the
name turned away for 10 minutes). One at a time, open 20 s, decided by a majority of the people in the game (not
voting counts as no; the caller votes yes); Y or N while it shows (the keys that type them, so QWERTZ works), F1 for
yes, or the pause menu's VOTE YES and VOTE NO. On a gamepad the warmup's and the vote's hints and notices point to the
Start menu (`PadNotice`). `--votes` sets which are allowed.

With `--rounds off` (the default with `--arena`) the round starts at once and the next follows its end directly, in
the same game (everyone respawned with no score: `engine::NewRound`).
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
  its ticks to keep them there (a quarter faster when late, a tenth or a fifth slower when early); commands a few
  ticks late are caught up by running the missing ticks at once, saved, and only more than 8 late or 20 early make it
  jump (its saves lost until snapshots of the new ticks come).
- **Snapshots**, per client, every tick (`Server::SendSnapshot`): every player and the car each sits in, wherever they
  are, and everything within the client's interest area (24 blocks around its player; a camera sees 6 to 8). Each
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
  past the server's). `GTA2_NET_NOFORWARD=1` predicts by the repeated input alone (to compare on a replay).
  Everything else moves on by the game's own simulation in those ticks. Of the resimulated ticks only the first 4 are
  saved again (the next snapshots restore those; a save of the 3 MB world was three quarters of a rollback's time): the
  later ones keep their save from before, a world of that tick still, which a snapshot after 4 lost ones would restore
  (counted in the log).
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

  Player peds take no damage on a client, and everything counts as seen there, so nothing is removed off screen. A
  player's health that drops in a snapshot shows the server's hit: the game's blood burst at its ped and the grunt of a
  punch (`ShowServerHit`, `engine/players.cpp`), which the client's own hits on an invulnerable ped don't give.
- **The client's world** (`engine/replica.cpp`, `RemoveUnsent`, `EvictOwn`): a snapshot is all the client knows of the
  server's world. It carries every player, the car each sits in and every train wherever they are, and everything
  within the reach (the interest area, 24 blocks around the client's player as the server has it), each entity the
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
  out of its own, at once, ending the way the game's own entering and leaving end. Another player's enter key reaches
  a client after its tick, so when the server's player is on its way into a car and the client's isn't, the client
  starts it as the key does.
- **Guards** (`engine/guards.cpp`, on the server too): what a network game breaks in the game's own logic is made
  safe: peds given on-foot orders while in a car, the HUD arrows to players who joined after the start, objects
  attached to cars the client moved (a tank's turret), a player driving a train that reversed (`Train::ReassignTrainHead`
  makes the other end the lead and its driver, the ped stays in its old carriage: it goes with the driver's seat).

### Entities

The **net schema** (`engine/schema.cpp`) declares each entity type's fields, by name and width; `Capture` reads them
from the game and `Apply` writes them through the game's own functions wherever it keeps derived state (moving a
sprite relinks it in the collision grid; seating a ped sets both sides of the link). References to other entities go
as net ids. Structures go member by member by name (`engine/members.hpp`), never by position: the 64-bit layout isn't
the original's.

| Entity | Carries |
|---|---|
| World (one per snapshot, `engine/world_entity.cpp`) | the game's tick counter (timers are stamped with it), the random seed and stream, the restart zones' turn, the round's state (`core/round_state.hpp`), the game's generation (a map change), the traffic lights, the tag game's fugitive and times, the map's light level (its script sets it: `MapRenderer`'s ambient, its target and step), the police's state that a client's cops, cop cars and HUD read (the highest wanted level, the strongest crew, a crew on screen, the last cop whose shot hit, and each pursuit's criminal, level, state, timer and crews) |
| Player (`engine/players.cpp`) | place, input, health, death count and last killer, weapons and the ammo of all 28 (a car weapon the car here lacks is added to it), frags and points, the HUD's money and multiplier, power-up timers, its car, the chat line |
| Car (`engine/cars.cpp`, `car_ai.cpp`) | place, its whole `CarPhysics` (but its two pointers), damage, its wreck's fire, lights, horn, colour, its roof turret's angle, the trailer it tows (towed trailers sent too), its proofs and alarm (`Car::mFlags`), its driver, and its AI (`CarAI`: route, speed, the cars it chases) |
| Ped (`engine/ped_state.cpp`) | place, and the plain values its AI runs on (objectives, targets, speeds, timers, occupation, flags, health, death cause), its references (targets, group) and its car and seat |
| Pickup (`engine/pickups.cpp`) | model and place; roadblock barriers too (they are off the object pool's list), and the car weapons' mines and oil |
| Train (`engine/trains.cpp`, every train in every snapshot) | its lead's place, speed and driver (none, the traffic driver, a ped, a player), its carriages' car ids (the client's train of the same index takes them), which end leads (`Train::ReassignTrainHead`), its state, stop phase and timers, the station it heads for and that station's state, each carriage's damage and explosion timer |

Health, deaths, weapons and scores are the server's. A death the server had is carried out on the client with the
game's own `Ped::Kill` (dead at once, also when a blast has the ped knocked down), credited to the killer, and the deaths carried out are part of the rollback set, so a
resimulation never repeats one. Its respawn takes the server's restart zone: the snapshot's zone turn is already past the
server's respawn, so it goes back one first (`ZoneTurnBack`), and the slot picks the same block. The HUD's kill notices come from these deaths only, with the server's killer (none for
the police's), not from the deaths the client's prediction starts. A client goes by the game's death count, which only counts up (the round's, in the
results, starts again with each round), and takes a player's count as it finds it the first time it sees the player. Scores (with the money and multiplier) are set before the resimulation, so its ticks and saves carry the server's
(the HUD's rolling digits roll towards them), and again after it with power-up timers and the tag game's "it";
a new round sets them all back to a game's start.

A ped's AI state is applied only while it is on foot on both sides (a driver in the same car gets its objectives but
not its states), its states only when plain (walking, running, standing) on both sides, and all or nothing: if a
reference its objective acts on (its target ped, internal target, car thief, the car it heads for, enters or flees in)
isn't in the client's game, the ped keeps its own state for that snapshot; the others (who attacked, punched, was hit
or killed it, the car it stole) stay the client's when missing. Groups are rebuilt on
the client from each ped's leader, keeping only members that are present (the game walks a group's list up to its
count).

Not synced: objects other than pickups (fires, explosions, projectiles); the client's own simulation makes them, and
rolls them back with the world, but nothing corrects them.

### Random draws

The game has one random stream (`rand()`, behind `gRng`). Its original netcode was lockstep, every machine
simulating the whole map. A client here simulates its own area only, so every draw the server makes for anything else
would move every later draw of the client's. In a network game the draws have owners (`port/src/rand/owner.hpp`): each
ped's update (with its char's), each car's, and each service only the server runs draws a hash of the game's seed, the
owner (its net id, or the service), the tick and how many it drew in this update. An entity then decides the same way
wherever it is simulated. Draws without an owner (a player's controls, objects, explosions) stay the one stream, which
the world entity carries. Sound and particles draw from streams of their own (`port/src/rand/sound.hpp`, `particle.hpp`),
so muting sound in a resimulation, or the particles there are, don't move the game's draws. Outside a network game
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
(`gPortSoundRequestsHook`, in `sound_obj::ServiceSoundEffects`, around `InterrogateAudioEntities`), keyed by the entity's
owner (its pointer and id), which of its sounds it is (`sound_0x68::mSampleIndex`; the collision queue's impacts by
place), the place and the tick. A resimulated tick runs the same entities without the mixer (`SoundLogResimTick`: the
queues and channels as they were, no radio, vocal stream or pager) and notes what they would have asked for; their
progress moves on with the world as on a live frame (entity ages, `SoundPosEntry`, the frame counter, the events they
take). Once a snapshot settles a tick (its tick and the next, which runs with the server's state and inputs), that
tick's requests no live frame had (same owner and sound, within 2 ticks and a block) are added to the next live frame's
queue, each once, and noted as heard. What a snapshot's Apply leaves (a death the server carried out) is asked for by a
pass before the first resimulated tick, as the snapshot's next tick's. Loops are asked for every frame by the live
frames from the corrected world, and the channels playing them are kept over a restore.

| Sound | Where (`sound_obj.cpp`) | On a client |
|---|---|---|
| Shots (pistol, SMG, shotgun, rocket, car guns, mines) | `ProcessType7_Weapon`, `Weapon::mShotFired` | one-shot: the log |
| Flamethrower, fire truck gun, shocker | `ProcessType7_Weapon` | loop: live, channels kept |
| Impacts (landings, crashes, bullets) | `ProcessType6_CollisionSoundQueue`, `CollisionSoundQueue` | one-shot: the log, by place |
| Explosions: the bang at age 2, the crackle after | `ProcessOtherObjects` (object 113), the entity's age | one-shot: the log; the age moves on in a resimulation |
| Car fires, fire | `ProcessOtherObjects` | one-shot asked for every frame: the log |
| Footsteps | `ProcessPed`, the animation frame and the frame counter | one-shot: the log (the variant is drawn: keyed by the sound, not the sample) |
| Ped voices (hits, screams, a death's cry), a burning ped's scream | `HandlePedVoiceEvent` (`Ped::mVoiceEvent`), `ProcessPed` (`mScreamTimer`) | one-shot: the log; an Apply's by the pass before the first resimulated tick |
| Car doors, engine start, air brakes, AI horn beeps, car shop | `HandleCarDoorSounds`, `HandleCarEngineStartSound`, `HandleHeavyVehicleStopSound` (`SoundPosEntry::mLastSpeed`), `HandleAICarHornBeep` (`Car::mSoundEvent`), `PlayCarShopEventSound` | one-shot: the log; the last speed moves on in a resimulation |
| Phones, blaster, tank crush | `ProcessOtherObjects`, `HandleTankCrushSound` | one-shot: the log |
| Engines, cornering, skids, sirens, alarms, horn, damage, car bomb, trains, tank, cranes, crushers, ambient emitters | `HandleCarEngineSound`, `HandleAICarEngineSound`, ..., `ProcessType8_Crane`, `ProcessType9_Crusher`, `ProcessEffectOnlyObject` | loop: live, channels kept |
| Announcer lines (vocals) | `ProcessType10_Vocals`, `mVocals.mPendingVoice` | stream: the log, by line |
| Car radio, police radio, music | `ProcessType3_CopRadioAndMusic` | stream: live, kept over a restore, not run in a resimulation |
| HUD pager, frontend | `ProcessType11_HudPager`, `ProcessType2_Frontend` | live only |

`GTA2_NET_SFXLOG` measures it: a server with it listens at a client's player, both print every sample they start.

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
so `--replay <file>` plays the session again without a server, tick for tick, as fast as the game runs: a minute in
about 2 s, the same every time. **F12, or the key under Esc, marks a moment** in play: the client and its replay print
`net: mark <n> at frame <f>`, so a bug seen in play is "the last session, mark 2". A recording replays only on a build
of the same protocol (its welcome's version): another one's is refused with both numbers.

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
A bot client still takes the keyboard's key events (Escape and the pause menu, the chat), so `GTA2_INPUT` can press
them in a test.

| Brain | |
|---|---|
| `simple` | walks its route to the nearest player and fights when close |
| `carhop` | gets into the nearest car, drives, gets out, again |
| `rammer` | drives at players |
| `hunter` | the rammer when a car is near and nobody is, the fighter otherwise |
| `soldier` | a weapon, a car, then to the nearest player: runs it over when coming at it head on, or gets out and shoots |
| `human` | for crash hunts, what people do and the others don't, in turns: cars, buses and the tank (in, about, out at speed), trains (on at a station, a ride, off), every weapon it has, pickups, into water, its own blasts (`session/bot_human.cpp`) |
| `nn:<file>` | a trained network (`session/bot_nn.cpp`): 62 inputs (`BotFeatures`), a choice in six groups of outputs |

In the round flow bots ready up, vote for one of the other maps in the results (map 2 with `GTA2_NET_BOTMAP2=1`, for a
test) and yes on any vote but their own kick.

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
                    players.cpp, cars.cpp, car_ai.cpp, ped_state.cpp, pickups.cpp, trains.cpp   the entities (members.hpp)
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
| `Game.cpp` | `gPortAfterPlayersHook` | after the players' tick, before anything spawns: the server's camera lead |
| `Game.cpp` | `gPortExtraPlayerPeds` | at boot, after the level start: a ped for each player past the script's `PLAYER_PED`s |
| `Ped.cpp` | `gPortSpawnBlock` | a ped starting at a restart zone: a block of it no other player stands on |
| `Game.cpp`, `sound_obj.cpp` | `gPortWorldStill` | the results: nothing runs but the players' tick and the HUD; the world's sounds stop |
| `Game.cpp` | `gPortDrawOverlay` | drawn last: the status, the round's panel, chat completion |
| `Car.cpp`, `Char_Pool.cpp` | `PortTrafficLimit()`, `gPortSpawnPlayers` | the spawn limits times the players |
| `port/src/options_menu.cpp` | `gPortFrontendSetup`, `gPortFrontendInput` | the network page, beside reG2's options |
| `winmain.cpp` | `gPortNextNetworkGame`, `gPortLeaveNetworkGame`, `gPortNetFrameLoop`, `gPortNoDraw` | a map change goes to the next game; a client that left goes to the network page; a client runs reG2's frame loop; a headless server draws nothing |
| `map_0x370.cpp` | a file static in port builds | the restart zones' turn, a function's static in the original: registered from the start, so rolled back and sent |
| `sprite.cpp` | `gPortSpriteMovedHook` | a sprite moved: the grid check (`GTA2_NET_CHECKGRID`) |
| `port/src/rumble.cpp` | `gPortNoRumble` | no vibration on a server (its local player is a remote one) or in a client's resimulation |
| `sound_obj.cpp` | `gPortSoundRequestsHook` | a client's sound log: what a frame asks for, and what resimulated ticks owe added to it |
| `port/compat/src` | `gPortIdleHook`, `gPortNoAudio`, `gPortCrashHook` | polling while a frame waits (`window.cpp`), no sound on a server (`miles.cpp`), the crash report (`main.cpp`) |
| `Ped.cpp` | `gPortKeepWantedPoints` | a client keeps the server's wanted points with its police off |
| `ScriptThread.cpp` | `gPortScriptCommandRuns` | a client runs the level start's look and sound commands, and steps over the rest |
| `Car.cpp` | `gPortCarMayExplode` | a client's car the server sends blows up when a snapshot says the server's did, not on its own prediction |
| `Car.cpp` | `Car::TryHonkHorn` | a train's lead without a driver (a player got out of a carriage that stood still) doesn't read it |
| `Hud.cpp` | `port_TypedChar` (`port/compat/src/dinput.cpp`), `gPortChatChar` | the chat types the keyboard layout's characters; a server, those its clients typed |
| `Hud.cpp` | `gPortServerKillNotices` | a client's kill notices are the server's deaths only |
| `Hud.cpp` | `gPortDrawNetStandings` | a client's rank and score at the top left, in place of the list of the other players and the game's frags or tag time counter |
| `Hud.cpp` | `PortHudTextPalette` (`port/src/player_looks.hpp`) | the names over the players and the tag times in the port's looks' colours (the fonts have 9 remaps) |
| `Player.cpp`, `Hud.cpp` | `PortLookOf` (`port/src/player_looks.hpp`) | each look's HUD colour and arrow from one table, three more looks than the original's |
| `TagGame.cpp` | `gPortTagTimeCounts` | the tag game counts the "it" seconds without the game's time limit, which the original counts by |
| `PlayerScoreTracker.cpp` | `gPortServerKillAwards`, `gPortScoreBonusHook` | a client's kills award no points or bonuses: the scores are the server's, and a bonus's message and voice are its player's client's, from the server |
| `Pool.hpp`, `Car.cpp`, `Char_Pool.cpp`, `Game.cpp` | `PortRand::Scope` (`port/src/rand/owner.hpp`) | who a random draw belongs to |
| `sound_obj.*`, `ParticleManager.cpp`, `Particle_4C.cpp` | `port/src/rand/sound.hpp`, `particle.hpp` | their own random streams; `SoundPosEntry` from the pool; `sound_obj`'s state public for the rollback |

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
| The network check | `port/net/scripts/net_check.sh [--quick] [step...]` (`BUILD=<cmake build folder>`, `build_mac` by default; `GAME=<game folder>`, `$BUILD/game` by default), about 6 minutes | PASS or FAIL per step, below |
| The fuzz test | `port/net/scripts/fuzz.sh [-s seconds] [-r per mille] [-p map] [seed...]` (`CLIENT=<an AddressSanitizer build's reg2>`), a minute a seed | no crash with random snapshot fields ("Security") |
| The stress run | `port/net/scripts/stress.sh [-m minutes] [-n servers] [-s speed] [-p map] [-c seconds] [-g]`, 10 minutes by default | crashes and `CHECK` lines per process, below |

`net_check.sh`'s steps, on the macOS build; logs and fixtures in `build_mac/net_check/`:

- **arena**: 24 bot matches of 5 minutes on the server alone (three maps, every brain, random seeds): none may crash.
- **live**: a server and two bot clients at 100 ms, jitter and 2% loss, police on, with the grid check: no crash and
  no `net: CHECK` line. Prints the corrections and the comparison check's worst fields.
- **churn**: two bot clients stay while two others join and leave four times (stopped by SIGTERM and Ctrl-C): no
  crash, no `CHECK` line, nobody turned away as the server being full.
- **maps**: a three-map rotation, 15 s rounds at 4 times the clock: both clients follow every change.
- **rounds**: two bot clients ready up, play a round, vote for map 2 and follow the server there (a check of where
  the players stand: on the new map); a third joins during the results and plays round 2.
- **soak**: a server and three bot clients at 10 times the clock: 10 minutes of play in 60 s, no crash.
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
peds are predicted, so they differ.

**The divergence check** (`GTA2_NET_DIVERGE=<server tick>[:<ticks>]`, 300 by default): the client reconciles up to that
tick, then runs on its own; each later snapshot is compared with its save of that tick, and at the end it prints per
field after how many ticks each entity first differed. Below the fields, `GTA2_NET_DUMPPEDS` dumps every ped's bytes
on both ends (`port/net/scripts/ped_diff.py` names the members that differ) and `GTA2_NET_RANDLOG` each `rand()` draw's
caller (`port/net/scripts/rand_diff.py` counts them per tick and function).

### Environment variables

The session's (`session/env.cpp`) and the engine's (`engine/dev/checks.cpp`, the dumps in `engine/dev/compare.cpp`), all
off by default:

| Variable | Where | Effect |
|---|---|---|
| `GTA2_NET_RECORD=<file>\|off` | client | the recording's file (default `recordings/session-<n>.rec`) |
| `GTA2_NET_REPLAYON=<frames>` | replay | the game runs on that many frames after the recording ends, nothing arriving |
| `GTA2_NET_STATS=1` | client | the connection beside the ping, over the last second: the snapshots lost (the server sends one a tick), received per second, the ticks the game runs ahead of the newest, and the ticks resimulated per rollback |
| `GTA2_NET_NOFORWARD=1` | client | other players predicted by their repeated input only, not the forwarded controls |
| `GTA2_NET_HOLDALL=1` | client | other players' last input held through a stall, not let go |
| `GTA2_NET_CUT=<every ms>:<ms>` | either end | with the link conditioner on (`--lag 1`): every unreliable send lost that long that often |
| `GTA2_NET_RESIMSAVES=<n>` | client | the resimulated ticks saved again after a rollback: the first n (4; 0: all) |
| `GTA2_NET_BLEND=<0..1>`, `_BLEND_FAR`, `_BLEND_SELF` | client | how far a correction is blended in each tick: a small one (0.3), one near 2 blocks (0.5), the local player's own (0.5); 1: at once |
| `GTA2_NET_COMPARE=1` | client | the comparison check |
| `GTA2_NET_DIVERGE=<tick>[:<ticks>]` | client | the divergence check |
| `GTA2_NET_CORRLOG=1` | client | corrections over half a block or 45 degrees |
| `GTA2_NET_JERKLOG=1\|2` | both | how jerky what is shown moves (2: and each jump), and how far its way is from its facing and how fast it turns (on a client also where the newest snapshots have the two agree) |
| `GTA2_NET_MARKER=1` | client | a square where the newest snapshot has the local player |
| `GTA2_NET_SFXLOG=1` (client), `=<slot>` (server) | both | every sample the mixer starts, by tick, what made it and where; a server with it opens its audio device and listens at that slot's player (to count what a client misses or plays twice) |
| `GTA2_NET_INPUTLOG=1`, `_ANIMLOG=1`, `_SELFLOG=1`, `_SOUNDLOG=1` | client | the input, the other players' animation, the local player around a death and the server's hits on players, the sound's channels and the car radio |
| `GTA2_NET_CARDIFF=1` | client | where the prediction of the local player's car differs from the snapshot |
| `GTA2_NET_GANGLOG=1` | both | player 0's group every 15 ticks, by the server's ids |
| `GTA2_NET_STACKLOG=1` | both | two cars in one place (under 0.4 blocks apart), once per pair: a car the client kept that the server dropped |
| `GTA2_NET_PICKUPLOG=1` | client | pickups made and removed |
| `GTA2_NET_FIRELOG=1` | both | every fire at every 30th tick of the game's cycle: a car's (its id, wrecked or not, its fire's explosion types), the ones on nothing by place, the peds on fire |
| `GTA2_NET_TRAINLOG=1` | both | every train's place and state at every 150th tick of the game's cycle (the same tick on both ends) |
| `GTA2_NET_CHECKGRID=1` | both | the collision grid checked after every move, a sprite moved off the map reported with the code that moved it, a rollback block whose pointee moved since the set was taken, on a client a ped driving a car that has another driver or leaving it through another car, and on both the weapon pool's lists looping or sharing a weapon and a ped holding a weapon it would free wrongly (a freed one, a player slot's, a car's, another ped's), a tank, fire truck or gun jeep without its roof turret, and a collision grid row that isn't a sorted list of pool cells and links, after every tick, restore and apply (`net: CHECK` lines) |
| `GTA2_NET_SNAPCHECK=1` | server | every entity's hash in the snapshots, checked by the clients, and the snapshot sanity check run on the server's own world (`net: CHECK` lines) |
| `GTA2_NET_DUMPPEDS=<file>[:<from>:<to>]`, `GTA2_NET_RANDLOG=<file>:<from>:<to>` | both | the divergence check's dumps |
| `GTA2_NET_NAVDUMP=<file.pgm>` | client bot | the bots' map as a picture |
| `GTA2_NET_TESTKILL=<tick>:<victim>:<killer>,...` | server | a player killed by the game's own death |
| `GTA2_NET_TESTPOWERUP=<tick>:<player>:<power-up>,...` | server | a power-up (12: an instant gang) |
| `GTA2_NET_TESTFIRE=<tick>:<player>,...` | server | the player set on fire |
| `GTA2_NET_TESTPATIENTS=<tick>:<player>:<n>[:<blocks>],...` | server | the n ambient peds nearest the player (at least that many blocks away) set on fire: the ambulance comes for their corpses |
| `GTA2_NET_TESTSTREAK=<tick>:<player>,...` | server | a kill streak's bonus, as the 20th kill in a row gives it |
| `GTA2_NET_TESTCAR=<tick>:<player>:<model>,...` | server | a car beside the player (11: a bus; 63, 64: a truck cab with a container trailer); model + 1000 * n: damaged by n * 1000 (n = 16: on fire, 25: burning); a model the game can't wreck (24 to 26, 85: the HOTDOG_D vans; the wrecks) isn't spawned |
| `GTA2_NET_TESTTRAIN=<tick>:<player>:<train>,...` | server | the player at the wheel of a train; train + 10: that train into reverse; train + 20: the player on the platform of the station it stops at next |
| `GTA2_NET_TESTARM=<weapon>:<ammo>[,...]`, `GTA2_NET_TESTWANTED=<points>` | server | every player kept armed (car weapons from 15: the car it drives), or at least that wanted (600: a star) |
| `GTA2_NET_SCOREBOARD=1` | client | the scoreboard shown all the time, as if SCORES were held (captures) |
| `GTA2_NET_BOTMAP2=1` | client bot | it votes map 2 in the results (net_check's rounds step) |
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
| A link at 300 ms (Linux, x86_64 desktop) | 12 ticks resimulated per snapshot: a rollback takes 3 ms a tick on average, 6 ms at worst, three quarters of it the 12 saves of the world (LAN: 0.6 ms); in the client's log every 300 frames |

## Open

- **Typing on the network page with a gamepad**: the address and name take the keyboard only.
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
