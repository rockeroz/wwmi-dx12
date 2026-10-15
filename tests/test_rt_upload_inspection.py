import importlib.util
import json
from pathlib import Path
import struct
import tempfile
import unittest

SPEC = importlib.util.spec_from_file_location("inspect_upload_windows", Path(__file__).parents[1] / "tools/rt_capture/inspect_upload_windows.py")
module = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(module)


class UploadInspectionTest(unittest.TestCase):
    def test_affine_runs_are_candidates_and_zero_nan_buffers_are_rejected(self):
        matrix = struct.pack("<12f", 1, 0, 0, 3, 0, 1, 0, 4, 0, 0, 1, 5)
        result = module.affine_runs(matrix * 4)
        self.assertEqual(result[0]["matrix_count"], 4)
        self.assertTrue(result[0]["heuristic_only"])
        self.assertEqual(module.affine_runs(bytes(512)), [])
        self.assertEqual(module.affine_runs(struct.pack("<f", float("nan")) * 128), [])

    def test_modified_bytes_or_missing_fence_provenance_are_not_analyzed(self):
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder); binary = root / "1.bin"; binary.write_bytes(bytes(192))
            manifest = {"schema": "wwmi-rt-capture-v1", "upload_windows": [{"id": 1, "root": 13, "file": "1.bin", "bytes": 192,
                "fnv1a64": module.fnv1a64(bytes(192)), "source_heap": "UPLOAD", "copy_fence_completed": True}]}
            capture = root / "capture.json"; capture.write_text(json.dumps(manifest))
            self.assertFalse(module.inspect(capture)["windows"][0]["verified_bone_palette"])
            binary.write_bytes(b'x' * 192)
            self.assertTrue(module.inspect(capture)["windows"][0]["errors"])
            binary.write_bytes(bytes(192)); manifest["upload_windows"][0]["copy_fence_completed"] = False
            capture.write_text(json.dumps(manifest))
            self.assertTrue(module.inspect(capture)["windows"][0]["errors"])

    def test_windows_cannot_escape_the_capture_directory(self):
        with tempfile.TemporaryDirectory() as folder:
            capture = Path(folder) / "capture.json"
            capture.write_text(json.dumps({"schema": "wwmi-rt-capture-v1", "upload_windows": [{"id": 1, "root": 13, "file": "../escape.bin"}]}))
            with self.assertRaises(ValueError):
                module.inspect(capture)


if __name__ == "__main__":
    unittest.main()
