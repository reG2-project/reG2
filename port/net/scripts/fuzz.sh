#!/bin/bash
# The snapshot fuzz test (port/docs/netcode.md, "Security"): a server and a bot client per seed, the client turning a part
# of each snapshot's fields into random values of their width (GTA2_NET_FUZZ), as a hostile server could send them.
# Best with the client built with AddressSanitizer (CLIENT=<its reg2>, below). Nothing may crash.
# usage: port/net/scripts/fuzz.sh [-s seconds (60)] [-r per mille of fields (20)] [-p map (downtown-6p)] seed...
#   BUILD=<cmake build folder> (build_linux by default), CLIENT=<the client's reg2> (that build's), GAME=<a game folder>
# An AddressSanitizer build: cmake -S port -B build_asan -G Ninja <the usual toolchain> -DCMAKE_BUILD_TYPE=RelWithDebInfo
#   "-DCMAKE_CXX_FLAGS=-fsanitize=address -fno-omit-frame-pointer -mllvm -asan-globals=0" "-DCMAKE_C_FLAGS=-fsanitize=address"
#   "-DCMAKE_EXE_LINKER_FLAGS=-fuse-ld=lld -fsanitize=address" (the game's globals overlap by design: not checked)
# Logs: $BUILD/net_fuzz/<seed>_{server,client}.log. Exit 1 on a crash.
for v in BUILD GAME CLIENT; do
  eval "p=\${$v:-}"
  case $p in "" | /*) ;; *) eval "$v=\$PWD/\$p" ;; esac
done
cd "$(dirname "$0")/../../.."
BUILD=${BUILD:-$PWD/build_linux}
BIN=$BUILD/reg2
CLIENT=${CLIENT:-$BIN}
GAME=${GAME:-$BUILD/game}
SECONDS_EACH=60 RATE=20 MAP=downtown-6p
while getopts "s:r:p:" o; do
  case $o in
    s) SECONDS_EACH=$OPTARG ;;
    r) RATE=$OPTARG ;;
    p) MAP=$OPTARG ;;
    *) exit 1 ;;
  esac
done
shift $((OPTIND - 1))
[ $# -gt 0 ] || set -- 1 2 3
[ -x "$BIN" ] && [ -x "$CLIENT" ] && [ -d "$GAME/data" ] || { echo "no build or game"; exit 1; }
OUT=$BUILD/net_fuzz
mkdir -p "$OUT"
export GTA2_DIR=$GAME GTA2_HEADLESS=1 SDL_AUDIO_DRIVER=dummy SDL_VIDEODRIVER=dummy GTA2_GAMEPAD=none
export ASAN_OPTIONS=detect_leaks=0:alloc_dealloc_mismatch=0
failed=0
cd "$GAME"
for seed in "$@"; do
  port=$((34000 + seed % 1000))
  GTA2_SPEED=2 timeout $((SECONDS_EACH + 15)) "$BIN" --server --port $port --map "$MAP" --players 2 --police 3 > "$OUT/${seed}_server.log" 2>&1 &
  server=$!
  sleep 3
  GTA2_SPEED=2 GTA2_NET_RECORD=off GTA2_NET_FUZZ=$seed:$RATE timeout "$SECONDS_EACH" "$CLIENT" --connect 127.0.0.1:$port --name fuzz --bot hunter \
    > "$OUT/${seed}_client.log" 2>&1
  code=$?
  kill $server 2> /dev/null
  wait $server 2> /dev/null
  if [ $code -ne 124 ] || grep -aq "crashed (signal\|ERROR: AddressSanitizer" "$OUT/${seed}_client.log"; then
    echo "seed $seed: FAIL (exit $code), $OUT/${seed}_client.log"
    failed=1
  else
    echo "seed $seed: ok, $(grep -ac 'CHECK snapshot field' "$OUT/${seed}_client.log") fields clamped (printed)"
  fi
done
exit $failed
