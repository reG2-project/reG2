#!/usr/bin/env python3
"""Compares two GTA2_NET_RANDLOG logs (a server's and a client's, port/docs/netcode.md "Checks"): per function that drew,
the rand() draws per tick on each side.

usage: port/net/scripts/rand_diff.py [--stream] [--tick <tick>] <program> <server log> <client log>
  --stream  only the game's stream's draws (the world entity carries it), not the owners' (port/src/rand/owner.hpp)
  --tick    the game's stream's draws of one tick in order, the server's against the client's last simulation of it before
            it compared the snapshot of that tick (GTA2_NET_COMPARE's "isn't the server's at tick" line)

Each log line is: cycle, o (an owner drew) or s (the stream), the caller of rand(), and the two above it, as offsets in
the program; a client with GTA2_NET_COMPARE also writes "M <tick>" before it compares the snapshot of that tick. A function is named
by the first frame that isn't an rng wrapper (rng::get_int and the like). Names come from atos (macOS) or
addr2line (Linux).
"""
import collections
import difflib
import subprocess
import sys


def symbolize(program, offsets):
    offsets = sorted(offsets)
    if sys.platform == "darwin":
        out = subprocess.run(["atos", "-o", program, "-l", "0x100000000"] + ["0x%x" % (0x100000000 + o) for o in offsets],
                             capture_output=True, text=True).stdout.splitlines()
        names = [l.split(" (in ")[0] for l in out]
    else:
        out = subprocess.run(["addr2line", "-f", "-C", "-e", program] + ["0x%x" % o for o in offsets],
                             capture_output=True, text=True).stdout.splitlines()
        names = out[0::2]
    return {o: n.split("(")[0] for o, n in zip(offsets, names)}


def read(path, stream):
    rows = []
    for line in open(path):
        if line.startswith("M "):
            continue
        cycle, kind, *frames = line.split()
        if kind == "s" or not stream:
            rows.append((int(cycle), [int(f, 16) for f in frames]))
    return rows


def tick_draws(path, tick):
    """The game's stream's draws of <tick>: a tick draws with the cycle before it, then advances the cycle with a draw of the
    new one (rng::AdvanceCycle). On a client, its last simulation of the tick before its "M <tick>" line"""
    found, run, prev = [], [], None
    for line in open(path):
        parts = line.split()
        if parts[0] == "M":
            if int(parts[1]) == tick:
                break
            run, prev = [], None
            continue
        cycle = int(parts[0])
        row = (parts[1], [int(f, 16) for f in parts[2:]])
        if prev is not None and cycle == prev + 1:
            if cycle == tick:
                found = [r for r in run + [row] if r[0] == "s"]
            run = []  # this row advanced the cycle: it ends that tick
        elif cycle != prev:
            run = [row]  # after a restore
        else:
            run.append(row)
        prev = cycle
    return [frames for _, frames in found]


def one_tick(program, server, client, tick):
    s, c = tick_draws(server, tick), tick_draws(client, tick)
    names = symbolize(program, {f for frames in s + c for f in frames})
    line = lambda frames: " < ".join(names[f] for f in frames)
    print("tick %d: the game's stream drew %d times on the server, %d on the client" % (tick, len(s), len(c)))
    for l in difflib.unified_diff([line(f) for f in s], [line(f) for f in c], "server", "client", lineterm=""):
        print(l)


def main():
    args = sys.argv[1:]
    stream = "--stream" in args
    args = [a for a in args if a != "--stream"]
    if "--tick" in args:
        i = args.index("--tick")
        tick = int(args[i + 1])
        del args[i:i + 2]
        one_tick(args[0], args[1], args[2], tick)
        return
    program, server, client = args[0], read(args[1], stream), read(args[2], stream)
    names = symbolize(program, {f for _, frames in server + client for f in frames})
    ticks = sorted({c for c, _ in client} & {c for c, _ in server})

    def per_function(rows):
        counts = collections.Counter()
        for cycle, frames in rows:
            if cycle in ticks:
                named = [names[f] for f in frames if not names[f].startswith("rng::")]
                counts[named[0] if named else names[frames[0]]] += 1
        return counts

    s, c = per_function(server), per_function(client)
    n = len(ticks)
    print("ticks %d..%d: draws per tick, server %.1f, client %.1f" % (ticks[0], ticks[-1], sum(s.values()) / n, sum(c.values()) / n))
    print("%-70s %8s %8s" % ("per tick, by the function that drew", "server", "client"))
    for name in sorted(set(s) | set(c), key=lambda k: -(s[k] + c[k])):
        mark = "" if s[name] == c[name] else "  <-"
        print("%-70s %8.1f %8.1f%s" % (name[:70], s[name] / n, c[name] / n, mark))


if __name__ == "__main__":
    main()
