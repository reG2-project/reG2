#!/usr/bin/env python3
# port/net: comfort.sh's table, from the GTA2_NET_COMFORT lines of server.log and each client_*.log in <out dir>.
# usage: comfort.py <out dir> [more out dirs: one column each]
import glob
import os
import re
import statistics
import sys

CAR_BASE = 0x01000000
TICK_MS = 33


def parse(path):
    life, shots, ends, last = [], {}, {}, {}
    rollback, ping, slot = [], [], None
    for line in open(path, errors="replace"):
        if line.startswith("comfort: "):
            f = line.split()
            kind = f[2]
            if kind == "life":
                life.append((int(f[3]), int(f[4]), int(f[5]), int(f[6]), int(f[7]), float(f[8])))
            elif kind == "shot+":
                shots[int(f[4])] = dict(tick=int(f[3]), model=int(f[5]), owner=int(f[6]), x=float(f[7]), y=float(f[8]))
            elif kind == "shot-":
                ends[int(f[4])] = int(f[3])
            elif kind in ("moves", "resp", "corr"):
                last[(kind, f[4])] = [int(v) for v in f[5:]]
            elif kind == "synced":
                last[("synced",)] = [int(f[4])]
            continue
        m = re.search(r"a (?:rollback|reconciliation) takes ([\d.]+) ms on average \(worst ([\d.]+)\).* ([\d.]+) ticks resimulated", line)
        if m:
            rollback.append(tuple(float(v) for v in m.groups()))
        m = re.search(r"frame \d+, ping (\d+) ms", line)
        if m:
            ping.append(int(m.group(1)))
        m = re.search(r"joined .* as player (\d+) of", line)
        if m:
            slot = int(m.group(1))
    for n, s in shots.items():
        s["end"] = ends.get(n)
    return dict(life=life, shots=shots, last=last, rollback=rollback, ping=ping, slot=slot)


def by_id(life):
    out = {}
    for tick, ms, i, state, new, dist in life:
        out.setdefault(i, []).append((tick, ms, state, new))
    return out


def dist_str(v):
    if not v:
        return "-"
    v = sorted(v)
    return "n %d, med %g, mean %.1f, p90 %g" % (len(v), statistics.median(v), statistics.mean(v), v[int(len(v) * 0.9)])


# Down (knocked, held) to the next state: its ticks, by what came next
def knockdowns(ids, players):
    dead, up = [], []
    for i, ev in ids.items():
        if (i < CAR_BASE) != players:
            continue
        for a, b in zip(ev, ev[1:]):
            if a[2] == 1 and not a[3]:
                (dead if b[2] == 2 else up).append(b[0] - a[0])
    return dead, up


def add(a, b):
    return [x + y for x, y in zip(a, b)] if a else list(b)


def measure(out):
    srv = parse(os.path.join(out, "server.log"))
    sdeaths = {i: [(t, ms) for t, ms, st, new in ev if st == 2 and not new] for i, ev in by_id(srv["life"]).items()}
    a = {k: [] for k in ("dp", "dped", "dpms", "dpedms", "own", "remote", "burst_own", "burst_remote", "kd_p", "kd_ped", "up_p", "up_ped")}
    c = dict(minutes=0.0, missed=0, false=0, undone=0, ghost_own=0, ghost_remote=0, short=0, own=0, remote=0, resp=[], respcar=[],
             corr={}, mv={}, rb=[], ping=[])
    for path in sorted(glob.glob(os.path.join(out, "client_*.log"))):
        cl = parse(path)
        if cl["slot"] is None or ("synced",) not in cl["last"]:
            continue
        me = cl["slot"] + 1
        c["minutes"] += cl["last"][("synced",)][0] / (60000.0 / TICK_MS)
        ids = by_id(cl["life"])
        # 1. The server's tick of a death to the client's first frame drawing it dead (in the client's ticks)
        for i, deaths in sdeaths.items():
            if i == me:
                continue
            drawn = [(t, ms) for t, ms, st, new in ids.get(i, []) if st == 2 and not new]
            for t, ms in deaths:
                hit = [h for h in drawn if t - 60 <= h[0] <= t + 150]
                if not hit:
                    c["missed"] += i < CAR_BASE and t > 300
                    continue
                ct, cms = min(hit, key=lambda h: abs(h[0] - t))
                a["dp" if i < CAR_BASE else "dped"].append(ct - t)
                a["dpms" if i < CAR_BASE else "dpedms"].append(cms - ms)
        for i, ev in ids.items():
            for t, ms, st, new in ev:
                if st == 2 and not new and i != me and not any(t - 150 <= s <= t + 60 for s, _ in sdeaths.get(i, [])):
                    c["false"] += 1
        for i, ev in ids.items():
            for x, y in zip(ev, ev[1:]):
                c["undone"] += i != me and x[2] == 2 and y[2] != 2 and y[0] - x[0] < 80
        for players, k, ku in ((True, "kd_p", "up_p"), (False, "kd_ped", "up_ped")):
            d, u = knockdowns(ids, players)
            a[k] += d
            a[ku] += u
        # 4. Each shot the client drew, matched with the server's by owner, model, tick and place
        ss = srv["shots"]
        index = {}
        for n, s in ss.items():
            index.setdefault((s["owner"], s["model"]), []).append(n)
        used = set()
        for n, s in sorted(cl["shots"].items(), key=lambda kv: kv[1]["tick"]):
            if not s["owner"] or s["owner"] >= CAR_BASE:
                continue
            own = s["owner"] == me
            best = None
            for m in index.get((s["owner"], s["model"]), []):
                t = ss[m]
                dt = s["tick"] - t["tick"]
                if m in used or not -3 <= dt <= 60:
                    continue
                d = ((s["x"] - t["x"]) ** 2 + (s["y"] - t["y"]) ** 2) ** 0.5
                if d <= 3 + 1.5 * abs(dt) and (best is None or abs(dt) < best[0]):
                    best = (abs(dt), m, dt)
            c["own" if own else "remote"] += 1
            if best is None:
                c["ghost_own" if own else "ghost_remote"] += 1
                continue
            used.add(best[1])
            a["own" if own else "remote"].append(best[2])
            t = ss[best[1]]
            if s["end"] is not None and t["end"] is not None and s["end"] < t["end"] - 3:
                c["short"] += 1
        # A burst's first shot (none by its owner for 20 ticks before) to the client's first drawn of that owner
        drawn = {}
        for s in cl["shots"].values():
            drawn.setdefault(s["owner"], []).append(s["tick"])
        last = {}
        for s in sorted(ss.values(), key=lambda s: s["tick"]):
            o = s["owner"]
            if o and o < CAR_BASE and s["tick"] - last.get(o, -99) > 20:
                seen = [t for t in drawn.get(o, []) if s["tick"] - 10 <= t <= s["tick"] + 60]
                if seen:
                    a["burst_own" if o == me else "burst_remote"].append(min(seen) - s["tick"])
            last[o] = s["tick"]
        # 2, 3, 5: the clients' running counts (their last lines)
        for key, v in cl["last"].items():
            if key[0] == "moves":
                c["mv"][key[1]] = add(c["mv"].get(key[1]), v)
            elif key[0] == "corr":
                c["corr"][key[1]] = add(c["corr"].get(key[1]), v)
            elif key[0] == "resp":
                c["resp" if key[1] == "foot" else "respcar"].append(v)
        c["rb"] += cl["rollback"]
        c["ping"] += cl["ping"]
    smv = {k[1]: v for k, v in srv["last"].items() if k[0] == "moves"}
    return a, c, smv


def table(outs):
    cols = [measure(o) for o in outs]
    rows = []

    def row(name, f):
        rows.append([name] + [f(*col) for col in cols])

    def per_min(n, c):
        return "%.1f/min" % (n / c["minutes"]) if c["minutes"] else "-"

    def mv(kind):
        def f(a, c, s):
            v = c["mv"].get(kind, [0, 0, 0, 0])
            ref = " (server %.2f%%)" % (100.0 * s[kind][1] / s[kind][0]) if kind in s and s[kind][0] else ""
            return "%s >0.25, %s >1, %s >2; %.2f%% of %d frames%s" % (per_min(v[1], c), per_min(v[2], c), per_min(v[3], c),
                                                                    100.0 * v[1] / v[0] if v[0] else 0, v[0], ref)
        return f

    def resp(v):
        if not v:
            return "-"
        n, to = sum(x[0] for x in v), sum(x[1] for x in v)
        h = [sum(x[2 + i] for x in v) for i in range(7)]
        p = lambda k: 100.0 * k / n if n else 0
        return "n %d: +1 frame %.0f%%, +2 %.0f%%, +3..5 %.0f%%, later %.0f%%; %d none" % (n, p(h[0] + h[1]), p(h[2]), p(sum(h[3:6])), p(h[6]), to)

    def corr(kind):
        def f(a, c, s):
            v = c["corr"].get(kind, [0, 0, 0])
            return "%s >0.25, %s >1 (%d in all)" % (per_min(v[1], c), per_min(v[2], c), v[0])
        return f

    def rb(a, c, s):
        if not c["rb"]:
            return "-"
        return "mean %.2f ms, worst %.2f ms, %.1f ticks resimulated" % (statistics.mean(r[0] for r in c["rb"]), max(r[1] for r in c["rb"]),
                                                                        statistics.mean(r[2] for r in c["rb"]))

    row("client minutes (synced)", lambda a, c, s: "%.1f" % c["minutes"])
    row("ping (ms, median)", lambda a, c, s: "%g" % statistics.median(c["ping"]) if c["ping"] else "-")
    row("1 death delay, players (ticks)", lambda a, c, s: dist_str(a["dp"]))
    row("1   same, wall ms", lambda a, c, s: dist_str(a["dpms"]))
    row("1 death delay, peds (ticks)", lambda a, c, s: dist_str(a["dped"]))
    row("1   same, wall ms", lambda a, c, s: dist_str(a["dpedms"]))
    row("1 player deaths never drawn", lambda a, c, s: str(c["missed"]))
    row("1 deaths drawn, not the server's", lambda a, c, s: per_min(c["false"], c))
    row("1 deaths drawn, then undone", lambda a, c, s: per_min(c["undone"], c))
    row("1 knock-down to death, players", lambda a, c, s: dist_str(a["kd_p"]))
    row("1 knock-down to death, peds", lambda a, c, s: dist_str(a["kd_ped"]))
    row("1 knock-downs up again pl/peds", lambda a, c, s: "%d / %d" % (len(a["up_p"]), len(a["up_ped"])))
    for kind in ("own_player", "own_car", "players", "cars", "peds"):
        row("2 drawn jumps, " + kind, mv(kind))
    row("2 corrections, others", corr("others"))
    row("3 corrections, own player+car", corr("self"))
    row("3 input to motion, on foot", lambda a, c, s: resp(c["resp"]))
    row("3 input to motion, own car", lambda a, c, s: resp(c["respcar"]))
    row("4 own shots, delay (ticks)", lambda a, c, s: dist_str(a["own"]) + ("; at 0: %.0f%%" % (100.0 * a["own"].count(0) / len(a["own"])) if a["own"] else ""))
    row("4 own bursts, first drawn (ticks)", lambda a, c, s: dist_str(a["burst_own"]))
    row("4 remote bursts, first drawn", lambda a, c, s: dist_str(a["burst_remote"]))
    row("4 ghosts own / remote", lambda a, c, s: "%s / %s (of %d / %d drawn)" % (per_min(c["ghost_own"], c), per_min(c["ghost_remote"], c), c["own"], c["remote"]))
    row("4 shots ended early", lambda a, c, s: per_min(c["short"], c))
    row("5 reconcile cost", rb)
    head = ["metric"] + [os.path.basename(o.rstrip("/")) for o in outs]
    widths = [max(len(r[i]) for r in rows + [head]) for i in range(len(head))]
    for r in [head] + rows:
        print("  ".join(v.ljust(w) for v, w in zip(r, widths)).rstrip())


if __name__ == "__main__":
    table(sys.argv[1:])
