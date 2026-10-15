import copy
import importlib.util
from pathlib import Path
import unittest

SPEC = importlib.util.spec_from_file_location("match_rt_geometry", Path(__file__).parents[1] / "tools/rt_capture/match_rt_geometry.py")
module = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(module)


def fixture():
    profile = {"schema": "wwmi-rt-mod-profile-v1", "mods": [{"ini": "normal/mod.ini", "vertices": 20,
        "components": [{"component": 0, "game_first_index": 0, "game_index_count": 6},
                       {"component": 1, "game_first_index": 6, "game_index_count": 3}]}]}
    geometries = [{"geometry": i, "type": 0, "vertex_stride": 12, "vertex_format": 6,
                   "vertex_count": 10, "vertices": {"va": "0x1000"}, "indices": {"va": hex(0x2000 + i * 24)},
                   "index_format": 42, "index_count": count} for i, count in enumerate((6, 3))]
    events = [{"seq": 1, "event": "vertex_buffers", "first": 0, "buffers": [{"address": {"va": "0x1000"}, "stride": 12}]},
              {"seq": 2, "event": "index_buffer", "address": {"va": "0x2000"}, "index_bytes": 4},
              {"seq": 3, "event": "draw_indexed", "count": 6, "first_index": 0, "base_vertex": 0},
              {"seq": 4, "event": "as_build", "type": 1, "inputs": geometries, "dest_va": "0x4000"}]
    for e in events:
        e.update(list=1, generation=1)
    return {"schema": "wwmi-rt-capture-v1", "inventory": [], "events": events}, profile


class GeometryMatchingTest(unittest.TestCase):
    def test_ordered_index_windows_and_shared_raster_buffers_are_only_candidates(self):
        c, p = fixture()
        candidate = module.match(c, p)["candidates"][0]
        self.assertEqual(candidate["component_raster_draw_counts"], {0: 1})
        self.assertEqual(candidate["raster_draw_sequences"], [3])
        self.assertFalse(candidate["ready_for_rt_replacement"])
        self.assertFalse(candidate["verified_character_ownership"])

    def test_equal_counts_with_wrong_relative_indices_or_vertex_layout_are_rejected(self):
        for field, value in (("indices", {"va": "0x201c"}), ("vertices", {"va": "0x3000"}),
                             ("vertex_format", 16), ("transform", {"va": "0x3000"})):
            c, p = fixture()
            c["events"][-1]["inputs"][1][field] = value
            self.assertEqual(module.match(c, p)["candidates"], [])

    def test_mod_variants_are_reported_without_selecting_one(self):
        c, p = fixture()
        other = copy.deepcopy(p["mods"][0]); other["ini"] = "variant/mod.ini"
        p["mods"].append(other)
        self.assertEqual(len(module.match(c, p)["candidates"]), 2)

    def test_reset_invalidates_raster_binding_state(self):
        c, p = fixture()
        c["events"].insert(2, {"seq": 2, "event": "reset", "list": 1, "generation": 2})
        c["events"][3]["generation"] = 2
        candidate = module.match(c, p)["candidates"][0]
        self.assertEqual(candidate["raster_draw_sequences"], [])


if __name__ == "__main__":
    unittest.main()
