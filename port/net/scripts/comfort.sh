#!/bin/bash
# port/net (port/docs/netcode.md, "Checks"): the comfort measurement. A local server with arena bots and bot clients on
# a conditioned link, GTA2_NET_COMFORT on every end, for MINUTES; then one table (comfort.py).
# usage: BIN=build_linux/reg2 DATA=<game data> port/net/scripts/comfort.sh [out dir]
#   MINUTES (3), MAP (mp1-6p), ARENA (the server's bots), CLIENTS (the bot clients' brains), LINK (the clients' link
#   conditioner), ARM (GTA2_NET_TESTARM on the server: an smg and rockets), NICE (19). HOME and XDG_DATA_HOME are fresh
#   folders under the out dir: the run never writes the user's game data.
set -u
HERE=$(dirname "$(realpath "$0")")
BIN=$(realpath "${BIN:-build_linux/reg2}")
DATA=$(realpath "${DATA:?the game data folder}")
OUT=$(realpath -m "${1:-comfort}")
MINUTES=${MINUTES:-3}
MAP=${MAP:-mp1-6p}
ARENA=${ARENA:-hunter,soldier,rammer,simple}
CLIENTS=${CLIENTS:-hunter soldier}
LINK=${LINK:---lag 100 --jitter 10}
ARM=${ARM-1:99,2:30}
NICE=${NICE:-19}
mkdir -p "$OUT/home" "$OUT/xdg"
rm -f "$OUT"/server.log "$OUT"/client_*.log
export HOME="$OUT/home" XDG_DATA_HOME="$OUT/xdg" GTA2_NET_RECORD=off GTA2_HEADLESS=1 GTA2_GAMEPAD=none SDL_AUDIODRIVER=dummy
export GTA2_NET_COMFORT=1
port=$((35000 + RANDOM % 1000))
pids=()
[ -n "$ARM" ] && export GTA2_NET_TESTARM="$ARM"
nice -n "$NICE" ionice -c3 "$BIN" --data "$DATA" --server --port $port --map "$MAP" --players 6 --arena "$ARENA" \
  > "$OUT/server.log" 2>&1 &
pids+=($!)
unset GTA2_NET_TESTARM
sleep 3
for b in $CLIENTS; do
  # shellcheck disable=SC2086
  nice -n "$NICE" ionice -c3 "$BIN" --data "$DATA" --connect 127.0.0.1:$port --name "c_$b" --bot "$b" $LINK \
    > "$OUT/client_$b.log" 2>&1 &
  pids+=($!)
done
sleep $((MINUTES * 60))
for ((i = ${#pids[@]} - 1; i >= 0; i--)); do
  kill "${pids[$i]}" 2>/dev/null
done
wait 2>/dev/null
python3 "$HERE/comfort.py" "$OUT"
