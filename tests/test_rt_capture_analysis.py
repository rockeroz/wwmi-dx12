import copy
import importlib.util
from pathlib import Path
import unittest

SPEC = importlib.util.spec_from_file_location("analyze_capture", Path(__file__).parents[1] / "tools/rt_capture/analyze_capture.py")
module = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(module)


def fixture():
    return {
        "schema": "wwmi-rt-capture-v1",
        "inventory": [{"id": 1, "kind": "resource", "va": "0x1000", "bytes": 4096},
                      {"id": 2, "kind": "pipeline", "shaders": [{"stage_name": "cs", "fnv1a64": "0x1234"}]}],
        "events": [
            {"seq": 1, "event": "bind_pipeline", "list": 1, "generation": 1, "pipeline_id": 2},
            {"seq": 2, "event": "root_descriptors", "list": 1, "generation": 1, "layout_id": 3,
             "param": 4, "type": 5, "compute": True, "values": [{"va": "0x1100"}]},
            {"seq": 3, "event": "dispatch", "list": 1, "generation": 1},
            {"seq": 4, "event": "as_build", "list": 1, "generation": 1, "type": 1, "dest_va": "0x4000",
             "inputs": [{"geometry": 0, "type": 0, "vertices": {"resource_id": 1, "offset": "0x100"},
                         "vertex_count": 20, "vertex_stride": 12, "vertex_format": 6, "index_count": 60}]}],
    }


class AnalysisTest(unittest.TestCase):
    def test_legacy_typed_table_units_are_converted_without_touching_new_captures(self):
        sample = {"type": 3, "format": 41, "offset": "0x100", "bytes": 8}
        self.assertEqual(module.normalize_table_sample(sample, {"probe_version": "0.1.2"})["offset"], 1024)
        self.assertEqual(module.normalize_table_sample(sample, {"descriptor_range_units": "bytes"}), sample)
        self.assertIsNone(module.normalize_table_sample({**sample, "format": 999}, {"probe_version": "0.1.2"}))
        self.assertIsNone(module.normalize_table_sample({**sample, "offset": hex(2**64 - 1)}, {"probe_version": "0.1.2"}))

    def test_candidates_are_not_promoted_to_proof(self):
        report = module.analyze(fixture())
        self.assertFalse(report["full_rt_mesh_support"])
        self.assertEqual(len(report["candidate_links"]), 1)
        link = report["candidate_links"][0]["candidates"][0]
        self.assertEqual(link["shader"], "0x1234")
        self.assertTrue(link["same_recording"])
        self.assertIn("overlap", link["evidence"])

    def test_reset_and_address_reuse_do_not_inherit_bindings(self):
        capture = fixture()
        capture["events"].insert(2, {"seq": 2, "event": "reset", "list": 1, "generation": 2})
        capture["events"][3]["generation"] = 2
        capture["events"][4]["generation"] = 2
        self.assertEqual(module.analyze(capture)["candidate_links"], [])

    def test_missing_or_destroyed_resources_stay_unresolved(self):
        capture = fixture()
        capture["events"].insert(3, {"seq": 3, "event": "destroy_resource", "resource_id": 1})
        self.assertEqual(module.analyze(capture)["blas_geometries_resolved"], 0)

    def test_tlas_and_unknown_formats_do_not_create_geometry_links(self):
        for modification in ("tlas", "format"):
            capture = copy.deepcopy(fixture())
            if modification == "tlas":
                capture["events"][-1]["type"] = 0
            else:
                capture["events"][-1]["inputs"][0]["vertex_format"] = 0
            self.assertEqual(module.analyze(capture)["candidate_links"], [])

    def test_root_gpu_address_is_not_treated_as_a_resource_pointer(self):
        capture = fixture()
        capture["events"][-1]["inputs"][0]["vertices"] = {"va": "0x1100", "resource_id": 0}
        self.assertEqual(len(module.analyze(capture)["candidate_links"]), 1)

    def test_sampled_table_uav_is_included_but_srv_is_not(self):
        capture = fixture()
        capture["events"].pop(1)
        dispatch = capture["events"][1]
        dispatch["buffer_table_samples"] = [{"param": 7, "type": 3, "resource_id": 1, "offset": "0x100"}]
        self.assertEqual(module.analyze(capture)["candidate_links"][0]["candidates"][0]["source"], "sampled_table_uav")
        dispatch["buffer_table_samples"][0]["type"] = 2
        self.assertEqual(module.analyze(capture)["candidate_links"], [])

    def test_recycled_gpu_address_does_not_connect_different_resources(self):
        capture = fixture()
        capture["events"][3:3] = [
            {"seq": 3, "event": "destroy_resource", "resource_id": 1},
            {"seq": 3, "event": "metadata", "value": {"id": 3, "kind": "resource", "va": "0x1000", "bytes": 4096}}]
        capture["events"][-1]["inputs"][0]["vertices"] = {"resource_id": 3, "offset": "0x100"}
        self.assertEqual(module.analyze(capture)["candidate_links"], [])

    def test_graphics_table_bind_does_not_erase_compute_root_uav(self):
        capture = fixture()
        capture["events"].insert(2, {"seq": 2, "event": "tables", "list": 1, "generation": 1,
                                      "compute": False, "layout_id": 5, "first": 4, "tables": ["0x5000"]})
        self.assertEqual(len(module.analyze(capture)["candidate_links"]), 1)

    def test_unknown_compute_pipeline_does_not_inherit_known_shader(self):
        capture = fixture()
        capture["events"].insert(2, {"seq": 2, "event": "bind_pipeline", "list": 1, "generation": 1,
                                      "stages": 0x800, "pipeline_id": 0, "pipeline_handle": "0x9999"})
        candidate = module.analyze(capture)["candidate_links"][0]["candidates"][0]
        self.assertIsNone(candidate["shader"])
        self.assertEqual(candidate["pipeline_handle"], "0x9999")


if __name__ == "__main__":
    unittest.main()
