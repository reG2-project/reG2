#!/bin/bash
# The netcode's checks in one go (port/docs/netcode.md, "Checks"), on the macOS build (port/README.md):
#   arena   bot matches on the server alone, several maps, brains and seeds: none may crash
#   live    a server and two bot clients over a bad link (100 ms, jitter, loss): no crash, no CHECK line, and the
#           server's recording replays to the same world
#   churn   two bot clients staying while two others join and leave, four times (stopped: SIGTERM, Ctrl-C): no
#           crash, no CHECK line, nobody turned away as the server being full
#   rounds  the round flow: two bot clients ready up, play a round, vote for the next map and follow; a third joins late
#   maps    a map rotation, a round of 15 s each (the clock 4 times as fast): two bot clients follow every map change,
#           no crash, no CHECK line
#   soak    a server and three bot clients, the clock 10 times as fast: 10 minutes of play in 60 s, no crash
#   throws  blasts beside a server's bots and one set on fire, a bot client watching: it shows each player thrown as the
#           server has it at that tick, with its outcome (a flying death knocked down), and burning doesn't bleed
#   replay  recorded client sessions replayed against their saved logs: the same, or nondeterministic (it differs
#           from itself too), else a regression
# usage: port/net/scripts/net_check.sh [--quick] [--record] [--save] [arena|live|churn|maps|rounds|soak|throws|replay ...]
#   --record  new fixtures (recorded sessions) and their logs; --save: the current build's replay logs as the baseline
# Fixtures and logs: $BUILD/net_check/. Exit 1 if anything failed.
for v in BUILD GAME BIN; do # given as relative paths: from here (the games run in $GAME)
  eval "p=\${$v:-}"
  case $p in "" | /*) ;; *) eval "$v=\$PWD/\$p" ;; esac
done
cd "$(dirname "$0")/../../.."
ROOT=$PWD
BUILD=${BUILD:-$ROOT/build_mac} # BUILD=<cmake build folder>: another build
BIN=${BIN:-$BUILD/reg2.app/Contents/MacOS/reg2}
[ -x "$BIN" ] || BIN=$BUILD/reg2
GAME=${GAME:-$BUILD/game} # GAME=<a game folder>: one with data/
export GTA2_DIR=$GAME
OUT=$BUILD/net_check
QUICK= RECORD= SAVE= STEPS=()
for a in "$@"; do
  case $a in
    --quick) QUICK=1 ;;
    --record) RECORD=1 ;;
    --save) SAVE=1 ;;
    *) STEPS+=("$a") ;;
  esac
done
[ ${#STEPS[@]} -eq 0 ] && STEPS=(arena live churn maps rounds soak throws replay)
[ -x "$BIN" ] && [ -d "$GAME/data" ] || { echo "no build or game: the port built in $BUILD (cmake -S port -B build_mac -G Ninja), and a game folder with data/ in $GAME"; exit 1; }
mkdir -p "$OUT"
trap 'kill $(jobs -p) 2>/dev/null' EXIT # our games end with us (most jobs exec their game; an arena match's subshell waits for it)
FAILED=0
pass() { echo "PASS $*"; }
fail() { echo "FAIL $*"; FAILED=1; }
BASE=$((33000 + RANDOM % 1000)) # the servers' ports: BASE + 1.. (arena), BASE + 200.. (the rest)

arena() {
  local maps=(mp1-6p mp5-6p downtown-6p) seeds=8 crashed=0 n=0
  [ -n "$QUICK" ] && seeds=3
  rm -f "$OUT"/arena_*.log
  for m in "${maps[@]}"; do
    for s in $(seq 1 $seeds); do
      n=$((n + 1))
      (cd "$GAME" && GTA2_HEADLESS=1 GTA2_SPEED=1000 "$BIN" --server --port $((BASE + n)) --map $m --players 6 --seed $s \
        --arena rammer,hunter,soldier,simple,carhop,rammer --ticks 9000 > "$OUT/arena_${m}_$s.log" 2>&1
       echo $? > "$OUT/arena_${m}_$s.exit") &
    done
  done
  wait
  for f in "$OUT"/arena_*.exit; do
    if [ "$(cat "$f")" != 0 ]; then
      crashed=$((crashed + 1))
      echo "  ${f%.exit}.log: $(grep -m1 -A3 crashed "${f%.exit}.log" | tail -1 | sed 's/^ *//' | cut -c1-120)"
    fi
  done
  [ $crashed = 0 ] && pass "arena: $n matches of 5 minutes" || fail "arena: $crashed of $n matches crashed"
}

live() {
  local secs=45 p=$((BASE + 200))
  [ -n "$QUICK" ] && secs=20
  rm -f "$OUT"/live_*.log
  rm -f "$OUT/live_server.rec"
  (cd "$GAME" && GTA2_HEADLESS=1 GTA2_NET_CHECKGRID=1 GTA2_NET_RECORD="$OUT/live_server.rec" exec "$BIN" --server --port $p --map mp1-6p --players 6 --police \
    --arena hunter,soldier,rammer,simple > "$OUT/live_server.log" 2>&1) &
  sleep 3
  for b in rammer soldier; do
    (cd "$GAME" && GTA2_HEADLESS=1 GTA2_NET_CHECKGRID=1 GTA2_NET_JERKLOG=1 GTA2_NET_COMPARE=1 GTA2_NET_CORRLOG=1 exec "$BIN" --connect 127.0.0.1:$p --name c_$b --bot $b \
      --lag 100 --jitter 10 --loss 2 > "$OUT/live_$b.log" 2>&1) &
  done
  sleep $secs
  kill $(jobs -p) 2>/dev/null
  wait 2>/dev/null
  local crashes checks joined
  crashes=$(grep -l "crashed" "$OUT"/live_*.log | wc -l | tr -d ' ')
  checks=$(cat "$OUT"/live_*.log | grep -c "net: CHECK")
  joined=$(grep -c "joined as player" "$OUT/live_server.log")
  echo "  $(grep "players' corrections" "$OUT/live_rammer.log" | tail -1 | sed 's/^net: //')"
  echo "  $(grep "prediction against the server" "$OUT/live_rammer.log" | tail -1 | sed 's/^net: //' | cut -d';' -f1-4)"
  # The server's recording replays to the same world at every tick
  (cd "$GAME" && GTA2_HEADLESS=1 GTA2_NET_CHECKGRID=1 exec "$BIN" --replay "$OUT/live_server.rec" > "$OUT/live_server_replay.log" 2>&1)
  local replayed
  replayed=$(grep "^net: replayed" "$OUT/live_server_replay.log" | sed 's/^net: //')
  echo "  server replay: ${replayed:-none}"
  if [ "$crashes" != 0 ] || [ "$checks" != 0 ] || [ "$joined" != 2 ]; then
    fail "live: $joined of 2 clients joined, $crashes crashed, $checks CHECK lines ($BUILD/net_check/live_*.log)"
  elif ! grep -q "matches the recording's" "$OUT/live_server_replay.log"; then
    fail "live: the server's replay differs ($BUILD/net_check/live_server_replay.log)"
  else
    pass "live: 2 bot clients for ${secs} s at 100 ms, 2% loss, police on; the server's replay matches"
  fi
}

churn() {
  local p=$((BASE + 203)) r
  rm -f "$OUT"/churn_*.log
  (cd "$GAME" && GTA2_HEADLESS=1 exec "$BIN" --server --port $p --map mp1-6p --players 6 --arena hunter,soldier \
    > "$OUT/churn_server.log" 2>&1) &
  sleep 2
  for b in rammer soldier; do
    (cd "$GAME" && GTA2_HEADLESS=1 GTA2_NET_CHECKGRID=1 exec "$BIN" --connect 127.0.0.1:$p --name stay_$b --bot $b \
      > "$OUT/churn_stay_$b.log" 2>&1) &
  done
  for r in 1 2 3 4; do
    (cd "$GAME" && GTA2_HEADLESS=1 exec "$BIN" --connect 127.0.0.1:$p --name in_a$r --bot carhop > "$OUT/churn_a$r.log" 2>&1) &
    local a=$!
    (cd "$GAME" && GTA2_HEADLESS=1 exec "$BIN" --connect 127.0.0.1:$p --name in_b$r --bot simple > "$OUT/churn_b$r.log" 2>&1) &
    local b=$!
    sleep 10
    kill -TERM $a
    kill -INT $b
    sleep 1
  done
  kill $(jobs -p) 2>/dev/null
  wait 2>/dev/null
  local crashes checks full joined
  crashes=$(grep -l "crashed" "$OUT"/churn_*.log | wc -l | tr -d ' ')
  checks=$(cat "$OUT"/churn_*.log | grep -c "net: CHECK")
  full=$(grep -l "is full" "$OUT"/churn_*.log | wc -l | tr -d ' ')
  joined=$(grep -c "joined as" "$OUT/churn_server.log")
  if [ "$crashes" != 0 ] || [ "$checks" != 0 ] || [ "$full" != 0 ] || [ "$joined" != 10 ]; then
    fail "churn: $joined of 10 joins, $full turned away, $crashes crashed, $checks CHECK lines ($BUILD/net_check/churn_*.log)"
  else
    pass "churn: 8 joins and leaves beside 2 clients that stayed"
  fi
}

maps() {
  local secs=120 p=$((BASE + 204))
  [ -n "$QUICK" ] && secs=60
  rm -f "$OUT"/maps_*.log
  (cd "$GAME" && GTA2_HEADLESS=1 GTA2_SPEED=4 exec "$BIN" --server --port $p --maps mp1-6p,mp2-6p,downtown-6p --players 6 --timelimit 1 \
    --arena simple,hunter,soldier > "$OUT/maps_server.log" 2>&1) &
  sleep 2
  for b in rammer soldier; do
    (cd "$GAME" && GTA2_HEADLESS=1 GTA2_SPEED=4 GTA2_NET_CHECKGRID=1 exec "$BIN" --connect 127.0.0.1:$p --name m_$b --bot $b \
      > "$OUT/maps_$b.log" 2>&1) &
  done
  sleep $secs
  kill $(jobs -p) 2>/dev/null
  wait 2>/dev/null
  local changes crashes checks followed=1 b
  changes=$(grep -c "net: the next map:" "$OUT/maps_server.log")
  crashes=$(grep -l "crashed" "$OUT"/maps_*.log | wc -l | tr -d ' ')
  checks=$(cat "$OUT"/maps_*.log | grep -c "net: CHECK")
  for b in rammer soldier; do
    # every change but perhaps the last, still under way at the end
    [ $(grep -c "a new game" "$OUT/maps_$b.log") -ge $((changes - 1)) ] || followed=0
  done
  if [ "$changes" -lt 3 ] || [ "$crashes" != 0 ] || [ "$checks" != 0 ] || [ $followed = 0 ]; then
    fail "maps: $changes map changes, clients followed: $followed, $crashes crashed, $checks CHECK lines ($BUILD/net_check/maps_*.log)"
  else
    pass "maps: $changes map changes, both clients followed"
  fi
}

# The round flow (session/round_flow.cpp), the clock 4 times as fast: two bot clients ready up, play a 1-minute round,
# vote for map 2 in the results and follow the server there, where the next round starts; a third joins in the results
rounds() {
  local p=$((BASE + 205))
  rm -f "$OUT"/rounds_*.log
  (cd "$GAME" && GTA2_HEADLESS=1 GTA2_SPEED=4 exec "$BIN" --server --port $p --map mp1-6p --players 6 --timelimit 1 --results 5 \
    > "$OUT/rounds_server.log" 2>&1) &
  sleep 2
  for b in rammer soldier; do
    (cd "$GAME" && GTA2_HEADLESS=1 GTA2_SPEED=4 GTA2_NET_CHECKGRID=1 GTA2_NET_BOTMAP2=1 exec "$BIN" --connect 127.0.0.1:$p --name r_$b --bot $b \
      > "$OUT/rounds_$b.log" 2>&1) &
  done
  sleep 18
  (cd "$GAME" && GTA2_HEADLESS=1 GTA2_SPEED=4 GTA2_NET_CHECKGRID=1 exec "$BIN" --connect 127.0.0.1:$p --name r_late --bot simple \
    > "$OUT/rounds_late.log" 2>&1) &
  sleep 17
  kill $(jobs -p) 2>/dev/null
  wait 2>/dev/null
  local S="$OUT/rounds_server.log" crashes checks ok=1 b
  crashes=$(grep -l "crashed" "$OUT"/rounds_*.log | wc -l | tr -d ' ')
  checks=$(cat "$OUT"/rounds_*.log | grep -c "net: CHECK")
  for line in "countdown (round 1)" "round (round 1)" "round 1 over" "results (round 2)" "votes for" "the next map, mp2-6p" \
    "countdown (round 2)" "round (round 2)"; do
    grep -q "$line" "$S" || { ok=0; echo "  rounds: no \"$line\" on the server"; }
  done
  for b in rammer soldier; do
    grep -q "a new game" "$OUT/rounds_$b.log" || { ok=0; echo "  rounds: r_$b didn't follow to the next map"; }
  done
  grep -q "notice: Round 2: go" "$OUT/rounds_late.log" || { ok=0; echo "  rounds: the late client wasn't in round 2"; }
  # The game on the new map, not the old one again: Tiny Town (mp1) is within x 50..130, mp2's restart zones east of 140
  sed -n '/the next map, mp2-6p/,$p' "$S" | grep "frags/deaths" | tail -1 | grep -Eq "@(1[4-9][0-9]|2[0-9][0-9])," ||
    { ok=0; echo "  rounds: the server's players aren't on mp2-6p after the map change"; }
  if [ "$crashes" != 0 ] || [ "$checks" != 0 ] || [ $ok = 0 ]; then
    fail "rounds: $crashes crashed, $checks CHECK lines ($BUILD/net_check/rounds_*.log)"
  else
    pass "rounds: warmup, ready, a round, results with a map vote, the voted map, a player joining in the results"
  fi
}

soak() {
  local secs=60 p=$((BASE + 202))
  [ -n "$QUICK" ] && secs=30
  rm -f "$OUT"/soak_*.log
  (cd "$GAME" && GTA2_HEADLESS=1 GTA2_SPEED=10 exec "$BIN" --server --port $p --map mp1-6p --players 6 --police \
    --arena hunter,soldier,rammer > "$OUT/soak_server.log" 2>&1) &
  sleep 2
  for b in rammer soldier hunter; do
    (cd "$GAME" && GTA2_HEADLESS=1 GTA2_SPEED=10 GTA2_NET_CHECKGRID=1 exec "$BIN" --connect 127.0.0.1:$p --name s_$b --bot $b \
      > "$OUT/soak_$b.log" 2>&1) &
  done
  sleep $secs
  kill $(jobs -p) 2>/dev/null
  wait 2>/dev/null
  local crashes checks
  crashes=$(grep -l "crashed" "$OUT"/soak_*.log | wc -l | tr -d ' ')
  checks=$(cat "$OUT"/soak_*.log | grep -c "net: CHECK")
  if [ "$crashes" != 0 ] || [ "$checks" != 0 ]; then
    fail "soak: $crashes crashed, $checks CHECK lines ($BUILD/net_check/soak_*.log)"
  else
    pass "soak: 3 bot clients, $((secs * 10 / 60)) minutes of play, $(grep -c 'joined as' "$OUT/soak_server.log") joined"
  fi
}

throws() {
  local p=$((BASE + 207)) blasts="" t
  for t in $(seq 300 90 1400); do # at the feet (a flying death) and a block away (knocked down), in turns
    blasts="$blasts,$t:0:$(( (t / 90) % 2 ? 0 : 9 ))"
  done
  rm -f "$OUT"/throws_*.log
  (cd "$GAME" && GTA2_HEADLESS=1 GTA2_NET_THROWLOG=1 GTA2_NET_TESTBLAST=${blasts#,} GTA2_NET_TESTFIRE=1500:1 GTA2_NET_TESTPHASE=400:1:1,1000:1 exec "$BIN" --server --port $p \
    --map mp1-6p --players 6 --arena simple,simple > "$OUT/throws_server.log" 2>&1) &
  sleep 2
  # The blend off: what the client shows at a tick is its prediction of it, the server's state there to the bit
  (cd "$GAME" && GTA2_HEADLESS=1 GTA2_NET_THROWLOG=1 GTA2_NET_SELFLOG=1 GTA2_NET_BLEND=1 GTA2_NET_BLEND_FAR=1 GTA2_NET_BLEND_SELF=1 \
    exec "$BIN" --connect 127.0.0.1:$p --name c_watch --bot carhop --lag 60 --jitter 5 > "$OUT/throws_client.log" 2>&1) &
  sleep 60
  kill $(jobs -p) 2>/dev/null
  wait 2>/dev/null
  # Per tick and player: thrown with an outcome (ped 8/24..26) or not, on fire or not
  local result
  result=$(awk '
    /THROW server tick [0-9]+: p[01] / { k = $5 $6; if ($12 ~ /^8\/2[456]$/) s[k] = $12; if ($24 == 1) { burn[$6]++; if ($6 == "p1") burning[$5 + 0] = 1 } }
    /THROW client tick [0-9]+: p[01] / && $12 ~ /^8\/2[456]$/ { c[$5 $6] = $12 }
    /hit on the server at tick/ && $3 == 1 && burning[$10 + 0] { hits++ }
    /THROW server tick [0-9]+: p1 / { sp[$5 + 0] = $12 }
    /THROW client tick [0-9]+: p1 / { cp[$5 + 0] = $12 }
    END {
      for (k in s) { thrown++; if (s[k] == "8/26") fatal++ }
      # A client never lands a player fatally itself (GuardThrow): a flying death of the server shows knocked down there
      for (k in c) { shown++; if (s[k] == "8/26") shownFatal++; if ((s[k] == "8/26" ? "8/25" : s[k]) != c[k]) wrong++ }
      # Electrocuted or sinking to a death on the server: the client still shows that tick alive in it
      for (t in sp) if (sp[t] ~ /^9\// && sp[t - 1] ~ /^8\/2[07]$/) { phased++; if (cp[t] !~ /^8\/2[07]$/) early++ }
      printf "%d %d %d %d %d %d %d %d %d", thrown, fatal, shown, shownFatal, wrong, burn["p1"], hits, phased, early
    }' "$OUT/throws_server.log" "$OUT/throws_client.log")
  local thrown fatal shown shownFatal wrong burnt hits phased early
  read -r thrown fatal shown shownFatal wrong burnt hits phased early <<< "$result"
  echo "  server: $thrown player ticks thrown, $fatal of them to a death; client: $shown shown ($shownFatal to a death), $wrong not as the server's; $hits hits shown on a player burning $burnt ticks; $phased deaths electrocuted or sinking, $early of them the client's own"
  if [ "${fatal:-0}" = 0 ] || [ "${shownFatal:-0}" = 0 ] || [ "${wrong:-1}" != 0 ] || [ "${burnt:-0}" = 0 ] || [ $((hits * 10)) -ge "$burnt" ] || [ "${phased:-0}" = 0 ] || [ "${early:-1}" != 0 ]; then
    fail "throws: the client's throws or burning aren't the server's ($BUILD/net_check/throws_*.log)"
  else
    pass "throws: every throw the client showed was the server's at that tick, flying deaths included; burning didn't bleed; electrocution and sinking deaths were the server's"
  fi
}

record() {
  local p=$((BASE + 201))
  rm -f "$OUT"/fixture_*.rec
  (cd "$GAME" && GTA2_HEADLESS=1 exec "$BIN" --server --port $p --map mp1-6p --players 6 --police --arena hunter,soldier,rammer \
    > "$OUT/record_server.log" 2>&1) &
  sleep 3
  for b in rammer soldier carhop; do
    (cd "$GAME" && GTA2_HEADLESS=1 GTA2_NET_RECORD="$OUT/fixture_$b.rec" exec "$BIN" --connect 127.0.0.1:$p --name f_$b --bot $b \
      --lag 100 --jitter 10 > /dev/null 2>&1) &
  done
  sleep 60
  kill $(jobs -p) 2>/dev/null
  wait 2>/dev/null
  echo "  recorded $(ls "$OUT"/fixture_*.rec | wc -l | tr -d ' ') sessions of 60 s"
  SAVE=1
}

# A replay's log without what changes from run to run by itself: pointers, timings, and the first block of a rollback set
# taken again that moved (a new game's allocations may land where the old game's were)
replay_log() {
  (cd "$GAME" && GTA2_HEADLESS=1 GTA2_NET_CHECKGRID=1 GTA2_NET_CORRLOG=1 exec "$BIN" --replay "$1" 2>&1) | grep "^net:" |
    grep -v "a rollback takes" | sed -E 's/0x[0-9a-f]+/PTR/g; s/, from block -?[0-9]+ \([^)]*\)$//'
}

replay() {
  [ -n "$RECORD" ] && record
  local fixtures=("$OUT"/fixture_*.rec) bad=0 flaky=0
  [ -e "${fixtures[0]}" ] || { fail "replay: no fixtures (--record makes them)"; return; }
  for r in "${fixtures[@]}"; do
    local name=$(basename "$r" .rec)
    replay_log "$r" > "$OUT/$name.now"
    if [ -n "$SAVE" ] || [ ! -e "$OUT/$name.base" ]; then
      cp "$OUT/$name.now" "$OUT/$name.base"
      echo "  $name: baseline saved ($(wc -l < "$OUT/$name.base" | tr -d ' ') lines)"
      continue
    fi
    if grep -q "net: CHECK" "$OUT/$name.now"; then
      echo "  $name: CHECK lines: $(grep -m1 "net: CHECK" "$OUT/$name.now" | cut -c1-120)"
      bad=$((bad + 1))
    elif ! cmp -s "$OUT/$name.now" "$OUT/$name.base"; then
      replay_log "$r" > "$OUT/$name.again"
      if cmp -s "$OUT/$name.now" "$OUT/$name.again"; then
        echo "  $name: differs from the baseline in $(diff "$OUT/$name.base" "$OUT/$name.now" | grep -c '^[<>]') lines (diff $BUILD/net_check/$name.base $name.now)"
        bad=$((bad + 1))
      else
        echo "  $name: nondeterministic (two replays differ in $(diff "$OUT/$name.now" "$OUT/$name.again" | grep -c '^[<>]') lines)"
        flaky=$((flaky + 1))
      fi
    fi
  done
  local note=""
  [ $flaky != 0 ] && note=", $flaky nondeterministic"
  [ $bad = 0 ] && pass "replay: ${#fixtures[@]} sessions$note" || fail "replay: $bad of ${#fixtures[@]} sessions differ"
}

for s in "${STEPS[@]}"; do
  case $s in
    arena | live | churn | maps | rounds | soak | throws | replay) $s ;;
    *) echo "no step $s (arena, live, churn, maps, rounds, soak, throws, replay)"; FAILED=1 ;;
  esac
done
exit $FAILED
