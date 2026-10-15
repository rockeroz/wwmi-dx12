import importlib.util
from pathlib import Path
import struct
import tempfile
import unittest


def load(name):
    path = Path(__file__).parents[1] / "tools/rt_capture" / f"{name}.py"
    spec = importlib.util.spec_from_file_location(name, path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


inspector = load("inspect_mod")
skin = load("skin_reference")
IDENTITY = ((1, 0, 0, 0), (0, 1, 0, 0), (0, 0, 1, 0))
MOVE_ROTATE = ((0, -1, 0, 10), (1, 0, 0, 20), (0, 0, 1, 30))


class PreparationTest(unittest.TestCase):
    def test_four_and_eight_influences_transform_correctly(self):
        for n in (4, 8):
            packed = bytes([0, 1] + [255] * (n - 2) + [85, 170] + [0] * (n - 2))
            actual = skin.skin_position((3, 6, 9), packed, [IDENTITY, MOVE_ROTATE])
            # Transform one rotates (3,6,9) -> (-6,3,9), then translates -> (4,23,39).
            expected = (11 / 3, 52 / 3, 29)
            for a, e in zip(actual, expected):
                self.assertAlmostEqual(a, e)

    def test_invalid_weights_and_active_bones_are_rejected(self):
        for packed in (bytes([2, 0, 0, 0, 255, 0, 0, 0]), bytes([0] * 8)):
            with self.assertRaises(ValueError):
                skin.skin_position((1, 2, 3), packed, [IDENTITY])
        with self.assertRaises(ValueError):
            skin.skin_position((float("nan"), 0, 0), bytes([0, 0, 0, 0, 255, 0, 0, 0]), [IDENTITY])

    def make_mod(self, root):
        (root / "Position.buf").write_bytes(struct.pack("<9f", 0, 0, 0, 1, 0, 0, 0, 1, 0))
        (root / "Blend.buf").write_bytes(bytes([0, 255, 255, 255, 255, 0, 0, 0]) * 3)
        (root / "Index.buf").write_bytes(struct.pack("<3I", 0, 1, 2))
        ini = root / "mod.ini"
        ini.write_text("""[Constants]
global $mesh_vertex_count = 2
[ResourcePositionBuffer]
filename = Position.buf
stride = 12
format = DXGI_FORMAT_R32G32B32_FLOAT
[ResourceBlendBuffer]
filename = Blend.buf
stride = 8
format = DXGI_FORMAT_R8_UINT
[ResourceIndexBuffer]
filename = Index.buf
stride = 12
format = DXGI_FORMAT_R32_UINT
[TextureOverrideComponent0]
hash = 12345678
match_first_index = 100
match_index_count = 600
if $mod_enabled
 if $variant
  drawindexed = 3, 0, 0
 endif
endif
[TextureOverrideComponent1]
match_first_index = 999
match_index_count = 600
handling = skip
""")
        return ini

    def test_actual_buffer_sizes_and_live_draws_drive_validation(self):
        with tempfile.TemporaryDirectory() as folder:
            result = inspector.inspect(self.make_mod(Path(folder)))
        self.assertEqual(result["vertices"], 3)
        self.assertEqual(result["index_elements"], 3) # stride=12 is not the index element size
        self.assertEqual(result["errors"], []) # game match windows are not mod draw windows
        self.assertEqual(len(result["warnings"]), 1)
        self.assertEqual(result["components"][0]["draws"][0]["conditions"], ["$mod_enabled", "$variant"])
        self.assertEqual(result["components"][1]["draws"], [])

    def test_out_of_bounds_mod_draw_is_rejected(self):
        with tempfile.TemporaryDirectory() as folder:
            ini = self.make_mod(Path(folder))
            ini.write_text(ini.read_text().replace("drawindexed = 3, 0, 0", "drawindexed = 4, 0, 0"))
            result = inspector.inspect(ini)
        self.assertTrue(result["errors"])


if __name__ == "__main__":
    unittest.main()
