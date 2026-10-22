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


class KeepModulesLocalTests(unittest.TestCase):
    """EA SPORTS FC 27, 22 Oct 2026: the builder streamed two executable modules from the game root as 0-byte placeholders and the
    launch died in rtld ('self_load_shared_object: size 0'). Executable modules stay real files in the starter."""

    def game(self):
        return {"tid": "PPSA34015", "name": "EA SPORTS FC 27", "loads_libSceAmpr": True, "fw": 6.5, "verdict": ["can stream"],
                "starter_bytes": 425_000_000, "remote_bytes": 0,
                "remote": [("data/pak0.big", 40_000_000_000), ("Engine.Render.Core2.PlatformPs5.prx", 3_925_528),
                           ("Engine.MovieVLC.LibVLC.prx", 3_339_443), ("PS5/Party.prx", 8_421_881), ("PS5/Party.prx.esbak", 8_421_881),
                           ("movie/intro.sprx", 1_500_000), ("tools/helper.ELF", 2_000_000), ("data/pak1.big", 9_000_000_000)]}

    def test_modules_leave_the_streamed_set_and_join_the_starter(self):
        r = self.game()
        kept = bm.keep_modules_local(r)
        self.assertEqual(sorted(kept), sorted(["Engine.Render.Core2.PlatformPs5.prx", "Engine.MovieVLC.LibVLC.prx", "PS5/Party.prx",
                                               "movie/intro.sprx", "tools/helper.ELF"]), "any case of the extension, any folder")
        self.assertEqual([f[0] for f in r["remote"]], ["data/pak0.big", "PS5/Party.prx.esbak", "data/pak1.big"],
                         ".esbak is a backup of a module, not a module: still streamed")
        self.assertEqual(r["remote_bytes"], 40_000_000_000 + 8_421_881 + 9_000_000_000)
        self.assertEqual(r["starter_bytes"], 425_000_000 + 3_925_528 + 3_339_443 + 8_421_881 + 1_500_000 + 2_000_000)
        self.assertEqual(r["kept_local"], kept)

    def test_nothing_to_keep_changes_nothing(self):
        r = {"remote": [("data/pak0.big", 1), ("data/pak1.big", 2)], "remote_bytes": 3, "starter_bytes": 10}
        self.assertEqual(bm.keep_modules_local(r), [])
        self.assertEqual(r, {"remote": [("data/pak0.big", 1), ("data/pak1.big", 2)], "remote_bytes": 3, "starter_bytes": 10})

    def test_the_size_cap_counts_the_modules_that_stay_on_the_console(self):
        r = {"tid": "PPSA1", "loads_libSceAmpr": True, "fw": 6.5, "verdict": [], "starter_bytes": 1_900_000_000,
             "remote": [("data/pak0.big", 9_000_000_000), ("Big.prx", 300_000_000)]}
        self.assertIsNone(bm.refusal_for(r, 13.60, 2048), "before: under the cap")
        bm.keep_modules_local(r)
        self.assertIn("starter would be 2.2 GB", bm.refusal_for(r, 13.60, 2048), "after: the module is on the console, so the cap sees it")

    def test_a_game_with_no_remote_key_is_left_alone(self):
        r = {"starter_bytes": 5}
        self.assertEqual(bm.keep_modules_local(r), [])


if __name__ == "__main__":
    unittest.main()
