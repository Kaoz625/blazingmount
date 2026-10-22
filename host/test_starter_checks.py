# SPDX-License-Identifier: GPL-3.0-or-later
"""Tests for the refusal rules of blazingmount-starter.py (refusal_for), which both a real build and
--check-only use. Run:  python3 -I host/test_starter_checks.py   (stdlib only; reads no drive, touches no console)"""
import importlib.util, os, unittest

HERE = os.path.dirname(os.path.abspath(__file__))
spec = importlib.util.spec_from_file_location("bmstarter", os.path.join(HERE, "blazingmount-starter.py"))
bm = importlib.util.module_from_spec(spec)
spec.loader.exec_module(bm)

OK = {"tid": "PPSA02632", "name": "Street Fighter 6", "loads_libSceAmpr": True, "fw": 6.5,
      "starter_bytes": 410 * 1000000, "remote": [("a", 1)] * 8, "verdict": ["can stream: link loads with libSceAmpr"]}


def r(**kw):
    d = dict(OK)
    d.update(kw)
    return d


class RefusalTests(unittest.TestCase):
    def test_a_normal_game_is_accepted(self):
        self.assertIsNone(bm.refusal_for(OK, 13.60))
        self.assertIsNone(bm.refusal_for(OK, 13.60, 2048))

    def test_no_title_id(self):
        self.assertIn("no title id", bm.refusal_for(r(tid=None), 13.60))

    def test_a_game_whose_eboot_never_loads_the_link_cannot_stream(self):
        why = bm.refusal_for(r(loads_libSceAmpr=False, verdict=["CANNOT STREAM: the eboot does not load libSceAmpr"]), 13.60)
        self.assertTrue(why.startswith("CANNOT STREAM"))

    def test_a_game_needing_newer_firmware_is_refused_not_backported(self):
        why = bm.refusal_for(r(fw=13.2), 10.20)
        self.assertIn("needs FW 13.2 but the console is 10.2", why)
        self.assertIn("does NOT backport", why)
        self.assertIsNone(bm.refusal_for(r(fw=13.2), 13.60))
        self.assertIsNone(bm.refusal_for(r(fw=None), 10.20), "unknown game firmware is not a refusal")

    def test_the_starter_cap_is_what_keeps_a_stream_from_being_a_full_install(self):
        big = r(starter_bytes=69_720_000_000)             # Ghost of Tsushima's starter on 21 Oct: 69.7 GB
        why = bm.refusal_for(big, 13.60, 2048)
        self.assertIn("69.7 GB", why)
        self.assertIn("stored on the console", why)
        self.assertIsNone(bm.refusal_for(big, 13.60), "no cap given: the behaviour a build has always had")
        self.assertIsNone(bm.refusal_for(r(starter_bytes=2_048_000_000), 13.60, 2048), "exactly the cap is allowed")
        self.assertIsNotNone(bm.refusal_for(r(starter_bytes=2_048_000_001), 13.60, 2048))

    def test_refusals_are_checked_in_order_title_id_then_link_then_firmware_then_size(self):
        both = r(loads_libSceAmpr=False, verdict=["no link"], fw=99, starter_bytes=10**12)
        self.assertEqual(bm.refusal_for(both, 13.60, 2048), "no link")


if __name__ == "__main__":
    unittest.main()
