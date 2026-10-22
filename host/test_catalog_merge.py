# SPDX-License-Identifier: GPL-3.0-or-later
"""Tests for the catalog half of blazingmount-host.py (--merge, atomic writes, the lock).
Run:  python3 -I host/test_catalog_merge.py      (stdlib only; builds nothing, touches no drive or console)"""
import importlib.util, json, os, sys, tempfile, threading, unittest

HERE = os.path.dirname(os.path.abspath(__file__))
spec = importlib.util.spec_from_file_location("bmhost", os.path.join(HERE, "blazingmount-host.py"))
bm = importlib.util.module_from_spec(spec)
spec.loader.exec_module(bm)


def game(tid, name=None, **kw):
    g = {"tid": tid, "name": name or ("Game %s (%s)" % (tid, tid)), "fw": 6.5,
         "bundle": "bundles/%s.starter.tar" % tid, "cfg": "cfg/%s.cfg" % tid}
    g.update(kw)
    return g


class MergeTests(unittest.TestCase):
    def test_new_game_is_appended_and_old_rows_stay(self):
        out = bm.merge_games([game("PPSA00001"), game("PPSA00002")], [game("PPSA00003")])
        self.assertEqual([g["tid"] for g in out], ["PPSA00001", "PPSA00002", "PPSA00003"])

    def test_same_title_id_replaces_in_place(self):
        out = bm.merge_games([game("PPSA00001", starter_bytes=1), game("PPSA00002")], [game("PPSA00001", starter_bytes=2)])
        self.assertEqual([g["tid"] for g in out], ["PPSA00001", "PPSA00002"])
        self.assertEqual(out[0]["starter_bytes"], 2)

    def test_inputs_are_not_mutated(self):
        existing = [game("PPSA00001", starter_bytes=1)]
        bm.merge_games(existing, [game("PPSA00001", starter_bytes=2)])
        self.assertEqual(existing[0]["starter_bytes"], 1)


class CatalogFileTests(unittest.TestCase):
    @staticmethod
    def put(out, text):
        with open(os.path.join(out, "catalog.json"), "w") as fh:
            fh.write(text)

    def test_roundtrip_writes_json_and_the_tab_separated_txt(self):
        with tempfile.TemporaryDirectory() as out:
            bm.write_catalog(out, "192.168.1.130", [game("PPSA00001", "Street Fighter 6 (PPSA00001)")])
            self.assertEqual([g["tid"] for g in bm.read_catalog(out)], ["PPSA00001"])
            with open(os.path.join(out, "catalog.txt")) as fh:
                lines = fh.read().splitlines()
            self.assertTrue(lines[0].startswith("# TID"))
            self.assertEqual(lines[1].split("\t"), ["PPSA00001", "Street Fighter 6 (PPSA00001)", "bundles/PPSA00001.starter.tar", "cfg/PPSA00001.cfg"])
            with open(os.path.join(out, "catalog.json")) as fh:
                self.assertEqual(json.load(fh)["host_ip"], "192.168.1.130")

    def test_no_temp_files_are_left_behind(self):
        with tempfile.TemporaryDirectory() as out:
            bm.write_catalog(out, "10.0.0.1", [game("PPSA00001")])
            self.assertEqual(sorted(os.listdir(out)), ["catalog.json", "catalog.txt"])

    def test_missing_or_broken_catalog_reads_as_empty(self):
        with tempfile.TemporaryDirectory() as out:
            self.assertEqual(bm.read_catalog(out), [])
            self.put(out, "{ not json")
            self.assertEqual(bm.read_catalog(out), [])
            self.put(out, '{"games": [{"name": "no tid"}, 7, {"tid": "PPSA00009"}]}')
            self.assertEqual([g["tid"] for g in bm.read_catalog(out)], ["PPSA00009"])

    def test_the_hand_written_header_only_catalog_reads_as_empty(self):
        # the live mac1 catalog.json on 22 Oct 2026: { "version": 1, "host_ip": "...", "games": [] }
        with tempfile.TemporaryDirectory() as out:
            self.put(out, '{ "version": 1, "host_ip": "192.168.1.130", "games": [] }')
            self.assertEqual(bm.read_catalog(out), [])


class LockTests(unittest.TestCase):
    def test_concurrent_merges_do_not_lose_rows(self):
        # 8 writers each merge their own game into one catalog at once: all 8 must survive.
        with tempfile.TemporaryDirectory() as out:
            def add(i):
                tid = "PPSA%05d" % i
                with bm.catalog_lock(out):
                    bm.write_catalog(out, "10.0.0.1", bm.merge_games(bm.read_catalog(out), [game(tid)]))
            threads = [threading.Thread(target=add, args=(i,)) for i in range(8)]
            for t in threads:
                t.start()
            for t in threads:
                t.join()
            self.assertEqual(sorted(g["tid"] for g in bm.read_catalog(out)), ["PPSA%05d" % i for i in range(8)])


if __name__ == "__main__":
    unittest.main()
