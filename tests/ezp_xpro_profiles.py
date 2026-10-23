#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Offline tests of selector retention and malformed vendor input handling."""

from pathlib import Path
import runpy
import unittest

generator = runpy.run_path(str(Path(__file__).resolve().parents[1] / "util/ezp_xpro_profiles.py"))
parse_profiles = generator["parse_profiles"]
generate_header = generator["generate_header"]

ROW = "@TEST:256:4KB:1MB:N:324:C84014:N:3:000"


def database(*rows, eeprom=()):
    source = "$25 SPI FLASH\r\n#VENDOR:\r\n" + "\r\n".join(rows) + "\r\n$24 EEPROM\r\n"
    if eeprom:
        source += "$95 EEPROM\r\n#ST:\r\n" + "\r\n".join(eeprom) + "\r\n"
    return source


class ProfileTests(unittest.TestCase):
    def test_conflicts_and_alternates_are_retained(self):
        rows = parse_profiles(database(ROW, ROW, ROW.replace("4KB", "F"),
                                       ROW.replace("C84014:N", "C84014:C84015")))
        self.assertEqual(len(rows), 4)
        self.assertIn((0xc84014, 1048576, 256, 4096, 0x324, 0, False, 0, 0, "TEST"), rows)
        self.assertIn((0xc84014, 1048576, 256, 0, 0x324, 0, False, 0, 0, "TEST"), rows)
        self.assertIn((0xc84014, 1048576, 256, 4096, 0x324, 0, False, 0xc84015, 0, "TEST"), rows)
        self.assertIn((0xc84015, 1048576, 256, 4096, 0x324, 0, True, 0xc84014, 0, "TEST"), rows)
        named_rows = parse_profiles(database(ROW, ROW.replace("TEST", "OTHER")))
        self.assertEqual({row[-1] for row in named_rows}, {"TEST", "OTHER"})

    def test_sst_and_esmt_identity_links_preserve_distinct_algorithms(self):
        rows = parse_profiles(database(
            "@SST25VF016B:N:4KB:2MB:N:324:BF4100:BF2541:3:090",
            "@F25L008A:2:4KB:1MB:N:324:8C2114:8C2014:3:030",
            "@F25L08PA:256:4KB:1MB:N:324:8C2014:N:3:050",
        ))
        self.assertEqual(len(rows), 5)
        self.assertIn((0xbf4100, 2097152, 0, 4096, 0x324, 0x90, False,
                       0xbf2541, 0, "SST25VF016B"), rows)
        self.assertIn((0xbf2541, 2097152, 0, 4096, 0x324, 0x90, True,
                       0xbf4100, 0, "SST25VF016B"), rows)
        self.assertIn((0x8c2114, 1048576, 2, 4096, 0x324, 0x30, False,
                       0x8c2014, 0, "F25L008A"), rows)
        self.assertIn((0x8c2014, 1048576, 2, 4096, 0x324, 0x30, True,
                       0x8c2114, 0, "F25L008A"), rows)
        self.assertIn((0x8c2014, 1048576, 256, 4096, 0x324, 0x50, False,
                       0, 0, "F25L08PA"), rows)

    def test_m95_eeprom_profiles_preserve_family_and_missing_identity(self):
        rows = parse_profiles(database(ROW, eeprom=(
            "@M95320:32:N:4KB:N:216:N:N:5:200",
            "@M95M02:256:N:256KB:N:324:N:N:5:200",
            "@M95010:8:N:128B:N:107:N:N:5:200",
        )))
        self.assertEqual(len(rows), 4)
        self.assertIn((0, 4096, 32, 0, 0x216, 0, False, 0, 2, "M95320"), rows)
        self.assertIn((0, 262144, 256, 0, 0x324, 0, False, 0, 2, "M95M02"), rows)
        self.assertIn((0, 128, 8, 0, 0x107, 0, False, 0, 2, "M95010"), rows)

    def test_other_sections_are_ignored(self):
        source = database(ROW) + "$25 EEPROM\r\n@INVALID\r\n$93 EEPROM\r\n@INVALID\r\n"
        source += "$95 EEPROM\r\n@M95320:32:N:4KB:N:216:N:N:5:200\r\n"
        source += "$OTHER\r\n@INVALID\r\n"
        self.assertEqual(parse_profiles(source), parse_profiles(database(
            ROW, eeprom=("@M95320:32:N:4KB:N:216:N:N:5:200",))))

    def test_names_are_escaped_in_c_and_control_characters_rejected(self):
        name = 'TEST"\\NAME??/'
        header = generate_header(database(ROW.replace("TEST", name)).encode("ascii"))
        self.assertIn(r'"TEST\"\\NAME\?\?/"', header)
        for char in ("\x00", "\t", "\x1b", "\x7f", "\u00e9"):
            with self.subTest(char=repr(char)), self.assertRaises(ValueError):
                parse_profiles(database(ROW.replace("TEST", "TEST" + char)))

    def test_eeprom_malformed_fields_are_rejected(self):
        row = "@M95320:32:N:4KB:N:216:N:N:5:200"
        for before, after in ((":32:", ":N:"), (":32:", ":0:"),
                              (":32:N:", ":32:F:"), (":5:", ":3:"),
                              (":N:N:5:", ":200012:N:5:"),
                              (":N:N:5:", ":N:200012:5:"),
                              ("200", "000"), ("200", "201"), ("200", "220"),
                              ("4KB", "4294967296B")):
            with self.subTest(field=after), self.assertRaises(ValueError):
                parse_profiles(database(ROW, eeprom=(row.replace(before, after),)))

    def test_special_and_large_profiles_are_retained(self):
        row = ROW.replace(":256:", ":N:").replace("4KB", "F").replace("1MB", "64MB")
        row = row.replace("324", "4B7").replace("000", "090")
        self.assertEqual(parse_profiles(database(row)),
                         [(0xc84014, 67108864, 0, 0, 0x4b7, 0x90, False, 0, 0, "TEST")])

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
