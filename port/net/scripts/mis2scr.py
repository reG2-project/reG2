#!/usr/bin/env python3
"""A small mission script compiler: a .mis of PLAYER_PED, GENERATOR, LEVELSTART, SWITCH_GENERATOR and LEVELEND into
the compiled .scr the game loads (Source/ScriptManager.cpp, ScriptManager::Load). Enough for a multiplayer map's
script (port/net/testdata/tiny-10p.mis); anything else is an error.

  mis2scr.py <in.mis> <out.scr>

The .scr: u16 mBasePointers[6000] (each command's offset in the data, 0 none), u8 mScriptData[65536], the u16 size
of the string table and the table (none here: 5118 zero bytes, the size the game reads). Each command starts with
SCR_CMD_HEADER (Source/ScriptThread.hpp): its own index, its type, the next index (-1 ends) and a return value.
"""
import re
import struct
import sys

POINTERS = 6000
DATA = 65536
STRINGS = 5118
FIRST_OFFSET = 8  # offset 0 means "no command" (ScriptManager::GetBasePointer)

# ScriptManager.hpp's SCRCMD_ types
PLAYER_PED = 5
GENERATOR_2D = 29  # SCRCMD_GENERATOR_DECSET1: no z, the ground's
GENERATOR_3D = 30  # SCRCMD_GENERATOR_DECSET2
LEVELSTART = 59
LEVELEND = 60
SWITCH_GENERATOR = 268  # SCRCMD_SWITCH_GENERATOR1
GROUND_Z = 255.0  # a z of 255: the ground at (x, y), as the game finds it
COLLECT_BASE = 200  # COLLECT_NN: object NN + 200 (Source/enums.hpp: pistol_weapon_200, ...)

NUM = r"(-?\d+(?:\.\d+)?)"
NAME = r"([A-Za-z_][A-Za-z0-9_]*)"
POINT = r"\(\s*" + NUM + r"\s*,\s*" + NUM + r"\s*(?:,\s*" + NUM + r"\s*)?\)"
RE_PLAYER_PED = re.compile(r"PLAYER_PED\s+" + NAME + r"\s*=\s*" + POINT + r"\s+(\d+)\s+(\d+)$")
RE_GENERATOR = re.compile(r"GENERATOR\s+" + NAME + r"\s*=\s*" + POINT + r"\s+(\d+)\s+(COLLECT_\d+|\d+)\s+(\d+)\s+(\d+)(?:\s+(\d+))?$")
RE_SWITCH = re.compile(r"SWITCH_GENERATOR\s*\(\s*" + NAME + r"\s*,\s*(ON|\d+)\s*\)$")


class MisError(Exception):
    pass


def fix16(v):
    """Fix16: 14 fraction bits"""
    return int(round(float(v) * 16384))


def parse(text):
    """The commands in order: (kind, name, fields)"""
    cmds, names = [], {}
    state = "declarations"
    for n, raw in enumerate(text.splitlines(), 1):
        line = raw.split("//", 1)[0].strip()
        if not line:
            continue

        def fail(why):
            raise MisError("line %d: %s: %s" % (n, why, raw.strip()))

        if line == "LEVELSTART":
            if state != "declarations":
                fail("a second LEVELSTART")
            state = "level"
            cmds.append(("LEVELSTART", None, ()))
        elif line == "LEVELEND":
            if state != "level":
                fail("LEVELEND without LEVELSTART")
            state = "end"
            cmds.append(("LEVELEND", None, ()))
        elif state == "end":
            fail("after LEVELEND")
        elif state == "declarations" and RE_PLAYER_PED.match(line):
            m = RE_PLAYER_PED.match(line)
            if m.group(4) is None:
                fail("a PLAYER_PED has a z (255.0: the ground)")
            cmds.append(("PLAYER_PED", m.group(1), (m.group(2), m.group(3), m.group(4), int(m.group(5)), int(m.group(6)))))
        elif state == "declarations" and RE_GENERATOR.match(line):
            m = RE_GENERATOR.match(line)
            obj = m.group(6)
            obj = COLLECT_BASE + int(obj[8:]) if obj.startswith("COLLECT_") else int(obj)
            cmds.append(("GENERATOR", m.group(1), (m.group(2), m.group(3), m.group(4), int(m.group(5)), obj,
                                                   int(m.group(7)), int(m.group(8)), int(m.group(9) or 0))))
        elif state == "level" and RE_SWITCH.match(line):
            m = RE_SWITCH.match(line)
            cmds.append(("SWITCH_GENERATOR", m.group(1), (m.group(2),)))
        else:
            fail("not supported here")
        name = cmds[-1][1]
        if name and cmds[-1][0] != "SWITCH_GENERATOR":
            if name in names:
                fail("%s declared twice" % name)
            names[name] = (cmds[-1][0], len(cmds))  # command indexes start at 1
    if state != "end":
        raise MisError("no LEVELSTART ... LEVELEND")
    return cmds, names


def compile_mis(text):
    cmds, names = parse(text)
    if len(cmds) >= POINTERS:
        raise MisError("too many commands")
    pointers = [0] * POINTERS
    data = bytearray(DATA)
    at = FIRST_OFFSET
    for i, (kind, name, f) in enumerate(cmds, 1):
        nxt = -1 if kind == "LEVELEND" else i + 1
        if kind == "PLAYER_PED":
            x, y, z, remap, rot = f
            for v, what in ((remap, "look"), (rot, "rotation")):
                if not 0 <= v < 65536:
                    raise MisError("%s: %s out of range" % (name, what))
            # SCR_PLAYER_PED: the ped's slot (filled at run time), x y z, the rotation in degrees, the look
            body = struct.pack("<iiiiHH", 0, fix16(x), fix16(y), fix16(z), rot, remap)
            cmd, ret = PLAYER_PED, 1
        elif kind == "GENERATOR":
            x, y, z, rot, obj, lo, hi, ammo = f
            # SCR_GENERATOR: the generator's slot, x y z, the rotation in degrees, the object, min and max delay, ammo
            body = struct.pack("<iiiiHHHHBBH", 0, fix16(x), fix16(y), fix16(GROUND_Z if z is None else z), rot, obj, lo, hi,
                               ammo, 0, 0)
            cmd, ret = (GENERATOR_2D if z is None else GENERATOR_3D), 0
        elif kind == "SWITCH_GENERATOR":
            target = names.get(name)
            if not target or target[0] != "GENERATOR":
                raise MisError("SWITCH_GENERATOR of %s: no such GENERATOR" % name)
            count = 0xFFFF if f[0] == "ON" else int(f[0])  # ON: no end to its spawns
            body = struct.pack("<HH", target[1], count)
            cmd, ret = SWITCH_GENERATOR, 0
        else:
            body = b""
            cmd, ret = (LEVELSTART if kind == "LEVELSTART" else LEVELEND), 0
        record = struct.pack("<hHhh", i, cmd, nxt, ret) + body
        if at + len(record) > DATA:
            raise MisError("the script data is full")
        pointers[i] = at
        data[at:at + len(record)] = record
        at += len(record)
    return struct.pack("<%dH" % POINTERS, *pointers) + bytes(data) + struct.pack("<H", 0) + bytes(STRINGS)


def main(argv):
    if len(argv) != 3:
        sys.stderr.write(__doc__)
        return 2
    try:
        with open(argv[1], encoding="ascii") as f:
            scr = compile_mis(f.read())
    except (MisError, UnicodeDecodeError) as e:
        sys.stderr.write("%s: %s\n" % (argv[1], e))
        return 1
    with open(argv[2], "wb") as f:
        f.write(scr)
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
