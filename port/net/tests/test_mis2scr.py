#!/usr/bin/env python3
"""port/net tests (ctest): scripts/mis2scr.py compiles testdata/tiny-10p.mis into testdata/tiny-10p.scr as committed,
and the result reads as ScriptManager::Load and the level thread read it (Source/ScriptManager.cpp, ScriptThread.cpp)."""
import os
import struct
import sys
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, "..", "scripts"))
import mis2scr  # noqa: E402

TESTDATA = os.path.join(HERE, "..", "testdata")


def load(scr):
    """As ScriptManager::Load: the pointers, the data, the string table's size"""
    pointers = struct.unpack_from("<6000H", scr, 0)
    data = scr[12000:12000 + 65536]
    (table,) = struct.unpack_from("<H", scr, 12000 + 65536)
    return pointers, data, table


def command(pointers, data, i):
    """GetBasePointer(i): (this, type, next, return value, offset); None if there is none"""
    if not pointers[i]:
        return None
    return struct.unpack_from("<hHhh", data, pointers[i]) + (pointers[i],)


class TinyTown(unittest.TestCase):
    def setUp(self):
        with open(os.path.join(TESTDATA, "tiny-10p.mis"), encoding="ascii") as f:
            self.scr = mis2scr.compile_mis(f.read())

    def test_committed(self):
        with open(os.path.join(TESTDATA, "tiny-10p.scr"), "rb") as f:
            self.assertEqual(f.read(), self.scr, "tiny-10p.scr is stale: run scripts/mis2scr.py")

    def test_loads(self):
        self.assertEqual(len(self.scr), 12000 + 65536 + 2 + 5118)  # what Load reads, the strings' 5118 at most
        pointers, data, table = load(self.scr)
        self.assertEqual(table, 0)
        cmds = [command(pointers, data, i) for i in range(1, 6000)]
        cmds = [c for c in cmds if c]
        for n, c in enumerate(cmds, 1):
            self.assertEqual(c[0], n)  # each command's own index
        # ExecOpCode's declarations: 10 players in 10 looks
        peds = [c for c in cmds if c[1] == mis2scr.PLAYER_PED]
        self.assertEqual(len(peds), 10)
        looks = [struct.unpack_from("<H", data, c[4] + 0x1A)[0] for c in peds]
        self.assertEqual(len(set(looks)), 10)
        for c in peds:
            x, y, z = struct.unpack_from("<iii", data, c[4] + 0xC)
            self.assertEqual(z, 255 * 16384)  # the ground (Handle_PLAYER_PED)
            self.assertTrue(0 < x < 256 * 16384 and 0 < y < 256 * 16384)
        # StartLevelThread: the first LEVELSTART, then the thread follows mCmdNext to LEVELEND
        start = next(c for c in cmds if c[1] == mis2scr.LEVELSTART)
        switches, c = 0, start
        while c[2] != -1:
            c = command(pointers, data, c[2])
            self.assertIsNotNone(c)
            self.assertIn(c[1], (mis2scr.SWITCH_GENERATOR, mis2scr.LEVELEND))
            if c[1] == mis2scr.SWITCH_GENERATOR:
                (gen,) = struct.unpack_from("<H", data, c[4] + 8)
                self.assertIn(command(pointers, data, gen)[1], (mis2scr.GENERATOR_2D, mis2scr.GENERATOR_3D))
                switches += 1
        self.assertEqual(c[1], mis2scr.LEVELEND)
        self.assertEqual(switches, 5)


class Errors(unittest.TestCase):
    def bad(self, text):
        with self.assertRaises(mis2scr.MisError):
            mis2scr.compile_mis(text)

    def test_bad(self):
        self.bad("PLAYER_PED p = (1.5, 1.5, 255.0) 5 0\n")  # no LEVELSTART
        self.bad("LEVELSTART\nPLAYER_PED p = (1.5, 1.5, 255.0) 5 0\nLEVELEND\n")  # a declaration in the level
        self.bad("LEVELSTART\nSWITCH_GENERATOR (g, ON)\nLEVELEND\n")  # no such generator
        self.bad("PLAYER_PED p = (1.5, 1.5) 5 0\nLEVELSTART\nLEVELEND\n")  # no z
        self.bad("CAR_DATA c = (1.5, 1.5) 0 0\nLEVELSTART\nLEVELEND\n")  # not supported


if __name__ == "__main__":
    unittest.main()
