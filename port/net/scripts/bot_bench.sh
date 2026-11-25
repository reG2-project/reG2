#!/bin/bash
# port/net (port/docs/netcode.md, "Bots"): the bots' stuck meter over headless arena matches, two at a time, niced.
# usage: BIN=build_linux/reg2 DATA=<game data> port/net/scripts/bot_bench.sh [out dir]
#   MAPS, SEEDS, TICKS, BRAINS override the defaults below, ARGS adds server options (--kit smg). Prints per map and
#   brain the stuck seconds per bot and 5 minutes (of which in a car), the longest stretch, failed searches and
#   give-ups; then frags, deaths, idle seconds, retreats and turnbacks, quick car switches; then the search cells per tick.
set -u
BIN=$(realpath "${BIN:-build_linux/reg2}")
DATA=${DATA:?the game data folder}
OUT=$(realpath -m "${1:-bot_bench}")
MAPS=${MAPS:-downtown-6p mp1-6p Industrial-6P}
SEEDS=${SEEDS:-1 2 3}
TICKS=${TICKS:-9000}
BRAINS=${BRAINS:-hunter,soldier,simple,hunter,soldier,simple}
mkdir -p "$OUT"
port=$((34000 + RANDOM % 1000))
for m in $MAPS; do
  for s in $SEEDS; do
    while [ "$(jobs -rp | wc -l)" -ge 2 ]; do wait -n; done
    port=$((port + 1))
    GTA2_HEADLESS=1 GTA2_SPEED=1000 GTA2_NET_RECORD=off GTA2_GAMEPAD=none SDL_AUDIODRIVER=dummy \
      nice -n 19 ionice -c3 "$BIN" --data "$DATA" --server --port $port --map "$m" --players 6 --seed "$s" \
      --arena "$BRAINS" --ticks "$TICKS" ${ARGS:-} > "$OUT/${m}_$s.log" 2>&1 &
  done
done
wait
for m in $MAPS; do
  cat "$OUT/${m}"_*.log | awk -v map="$m" -v ticks="$TICKS" '
    / stuck .* longest / { k = $4; n[k]++; stuck[k] += $6; if ($9 > lng[k]) lng[k] = $9; srch[k] += $12; fail[k] += $14; give[k] += $16; car[k] += $22 }
    / frags .* deaths / { frg[$4] += $6; dth[$4] += $8 }
    / idle .* retreats / { idl[$4] += $6; if ($9 > lidl[$4]) lidl[$4] = $9; ret[$4] += $12; tb[$4] += $15 }
    / car switches under 5 s / { sw[$4] += $10 }
    /search cells per tick/ { runs++; mean += $8; if ($10 > max) max = $10 }
    END {
      scale = 9000 / ticks
      for (k in n) printf "%-14s %-8s bots %2d  stuck %6.1f s/bot/5min (car %5.1f)  longest %5.1f s  failed %4.1f%% of %d searches  giveups %.1f/bot\n",
        map, k, n[k], stuck[k] / n[k] * scale, car[k] / n[k] * scale, lng[k], srch[k] ? 100 * fail[k] / srch[k] : 0, srch[k], give[k] / n[k]
      for (k in n) printf "%-14s %-8s frags %5.2f deaths %5.2f per bot  idle %5.1f s/bot/5min  longest %5.1f s  retreats %5.1f s/bot/5min  turnbacks %4.1f/bot  quick switches %4.1f/bot\n",
        map, k, frg[k] / n[k], dth[k] / n[k], idl[k] / n[k] * scale, lidl[k], ret[k] / n[k] * scale, tb[k] / n[k] * scale, sw[k] / n[k] * scale
      if (runs) printf "%-14s search cells per tick: mean %.0f, max %d\n", map, mean / runs, max
    }'
done
