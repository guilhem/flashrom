#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Offline tests of selector retention and malformed vendor input handling."""

from pathlib import Path
import runpy
import unittest

generator = runpy.run_path(str(Path(__file__).resolve().parents[1] / "util/ezp_xpro_profiles.py"))
parse_profiles = generator["parse_profiles"]

ROW = "@TEST:256:4KB:1MB:N:324:C84014:N:3:000"


def database(*rows):
    return "$25 SPI FLASH\r\n#VENDOR:\r\n" + "\r\n".join(rows) + "\r\n$24 EEPROM\r\n"


class ProfileTests(unittest.TestCase):
    def test_conflicts_and_alternates_are_retained(self):
        rows = parse_profiles(database(ROW, ROW, ROW.replace("4KB", "F"),
                                       ROW.replace("C84014:N", "C84014:C84015")))
        self.assertEqual(len(rows), 3)
        self.assertIn((0xc84014, 1048576, 256, 4096, 0x324, 0, False), rows)
        self.assertIn((0xc84014, 1048576, 256, 0, 0x324, 0, False), rows)
        self.assertIn((0xc84015, 1048576, 256, 4096, 0x324, 0, True), rows)

    def test_special_and_large_profiles_are_retained(self):
        row = ROW.replace(":256:", ":N:").replace("4KB", "F").replace("1MB", "64MB")
        row = row.replace("324", "4B7").replace("000", "090")
        self.assertEqual(parse_profiles(database(row)),
                         [(0xc84014, 67108864, 0, 0, 0x4b7, 0x90, False)])

    def test_malformed_fields_are_rejected(self):
        for before, after in ((":N:324", ":Y:324"), (":3:", ":5:"),
                              ("C84014", "C8401Z"), ("4KB", "4MB"),
                              ("256", "0"), ("1MB", "1XB"), ("000", "100"),
                              ("1MB", "4096MB")):
            with self.subTest(field=after), self.assertRaises(ValueError):
                parse_profiles(database(ROW.replace(before, after)))
        for contents in ("", database(), database(ROW).replace("$24 EEPROM", "")):
            with self.subTest(contents=contents), self.assertRaises(ValueError):
                parse_profiles(contents)


if __name__ == "__main__":
    unittest.main()
