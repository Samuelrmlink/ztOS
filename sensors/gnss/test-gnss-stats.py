#!/usr/bin/env python3
"""Host tests for gnss-stats.py: constellation counts, TTFF, redaction.
Every position in here is made up."""
import importlib.machinery
import importlib.util
from pathlib import Path
import unittest

TOOL = Path(__file__).resolve().parent / "gnss-stats.py"
loader = importlib.machinery.SourceFileLoader("gnss_stats", str(TOOL))
gs = importlib.util.module_from_spec(importlib.util.spec_from_loader(loader.name, loader))
loader.exec_module(gs)


def s(body):
    x = 0
    for ch in body.encode():
        x ^= ch
    return "$%s*%02X" % (body, x)


GGA_NOFIX = s("GPGGA,120000.00,,,,,0,00,99.0,,M,,M,,")
GGA_FIX = s("GPGGA,120001.00,1030.0000,S,02015.0000,E,1,09,0.8,10.0,M,,M,,")
RMC_FIX = s("GPRMC,120001.00,A,1030.0000,S,02015.0000,E,0.0,0.0,291026,,,A")


def epoch(t0):
    """One fix epoch as the S9's engine sends it (talker GSA and GN GSA)."""
    return [
        (t0, GGA_FIX),
        (t0, s("GPGSV,1,1,03,05,40,083,35,12,17,308,28,25,10,100,00")),
        (t0, s("GLGSV,1,1,02,70,50,120,30,80,20,200,22")),
        (t0, s("GAGSV,1,1,01,104,30,050,25")),
        (t0, s("BDGSV,1,1,02,201,60,010,33,215,15,300,00")),
        (t0, s("GPGSA,A,3,05,12,,,,,,,,,,,1.5,0.8,1.2")),
        # GLONASS used only through a GN GSA, numbered 65-96
        (t0, s("GNGSA,A,3,70,80,,,,,,,,,,,1.5,0.8,1.2")),
        (t0, s("GAGSA,A,3,104,,,,,,,,,,,,1.5,0.8,1.2")),
        # NMEA 4.10 with a system ID: 4 = BeiDou
        (t0, s("GNGSA,A,3,201,,,,,,,,,,,,1.5,0.8,1.2,4")),
        (t0, s("PZTFX,6.0,,")),
    ]


class StatsTests(unittest.TestCase):
    def run_lines(self, lines):
        st = gs.Stats()
        for t, l in lines:
            st.feed(l, t)
        return st.summary()

    def test_constellations_view_tracked_used(self):
        lines = [(0.0, GGA_NOFIX), (1.0, GGA_NOFIX)] + epoch(7.5) + epoch(8.5) + [(9.5, GGA_FIX)]
        r = self.run_lines(lines)
        sy = r["systems"]
        self.assertEqual(sy["GPS"]["in_view_median"], 3)
        self.assertEqual(sy["GPS"]["tracked_median"], 2)
        self.assertEqual(sy["GPS"]["used_median"], 2)
        self.assertEqual(sy["GLONASS"]["used_max"], 2)
        self.assertEqual(sy["Galileo"]["used_max"], 1)
        self.assertEqual(sy["BeiDou"]["used_max"], 1)
        self.assertEqual(sy["BeiDou"]["tracked_median"], 1)
        self.assertEqual(sy["GLONASS"]["snr_median"], 26)
        self.assertEqual(r["ttff_s"], 7.5)
        self.assertEqual(r["first_accuracy_s"], 7.5)
        self.assertEqual(r["accuracy_m_median"], 6.0)
        self.assertEqual(r["fix_quality"], {"0": 2, "1": 3})
        self.assertEqual(r["sats_used_gga_max"], 9)
        self.assertEqual(r["gsa_by_talker"], {"GP": 2, "GN": 4, "GA": 2})

    def test_prn_ranges(self):
        self.assertEqual(gs.system_of_prn(7), "GPS")
        self.assertEqual(gs.system_of_prn(40), "SBAS")
        self.assertEqual(gs.system_of_prn(85), "GLONASS")
        self.assertEqual(gs.system_of_prn(133), "Galileo")
        self.assertEqual(gs.system_of_prn(195), "QZSS")
        self.assertEqual(gs.system_of_prn(237), "BeiDou")

    def test_bad_checksum_counted_not_used(self):
        r = self.run_lines([(0.0, GGA_FIX[:-2] + "00")])
        self.assertEqual(r["bad_checksums"], 1)
        self.assertIsNone(r["ttff_s"])

    def test_fix_gaps(self):
        r = self.run_lines([(0.0, GGA_FIX), (1.0, GGA_FIX), (6.0, GGA_FIX)])
        self.assertEqual(r["fix_gaps_s"], [5.0])

    def test_summary_has_no_position(self):
        r = self.run_lines(epoch(1.0) + [(2.0, RMC_FIX)])
        text = repr(r)
        self.assertNotIn("1030", text)
        self.assertNotIn("02015", text)


class RedactTests(unittest.TestCase):
    def test_positions_blanked_checksums_valid(self):
        for line in (GGA_FIX, RMC_FIX,
                     s("GNGLL,1030.0000,S,02015.0000,E,120001.00,A,A"),
                     s("GNGNS,120001.00,1030.0000,S,02015.0000,E,AAN,09,0.8,10.0,,,")):
            out = gs.redact(line)
            self.assertTrue(gs.checksum_ok(out), out)
            self.assertNotIn("1030", out)
            self.assertNotIn("2015", out)

    def test_gga_keeps_quality_and_sats(self):
        f = gs.nmea_fields(gs.redact(GGA_FIX))
        self.assertEqual((f[6], f[7], f[8]), ("1", "09", "0.8"))
        self.assertEqual(f[9], "")          # altitude gone too

    def test_last_fix_sentence_dropped_accuracy_kept(self):
        self.assertIsNone(gs.redact(s("PZTLF,-10.500000,20.250000,6,1793000000")))
        self.assertEqual(gs.redact(s("PZTFX,6.0,,")), s("PZTFX,6.0,,"))

    def test_satellite_sentences_unchanged(self):
        line = s("GPGSV,1,1,01,05,40,083,35")
        self.assertEqual(gs.redact(line), line)


if __name__ == "__main__":
    unittest.main()
