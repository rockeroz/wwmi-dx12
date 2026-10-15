#!/usr/bin/env python3
"""Inspect copied UPLOAD windows; affine-looking data is not a verified bone palette."""
import argparse
import json
import math
from pathlib import Path
import struct


def fnv1a64(data):
    value = 0xcbf29ce484222325
    for byte in data:
        value = ((value ^ byte) * 0x100000001b3) & (2**64 - 1)
    return f"0x{value:016x}"


def affine_runs(data):
    def plausible(offset):
        values = struct.unpack_from("<12f", data, offset)
        if not all(math.isfinite(v) for v in values) or any(abs(values[i]) > 1e7 for i in (3, 7, 11)):
            return False
        rows = [values[i:i + 3] for i in (0, 4, 8)]
        lengths = [sum(v * v for v in row) for row in rows]
        if any(not 0.0625 <= n <= 16 for n in lengths):
            return False
        return all(abs(sum(a * b for a, b in zip(rows[i], rows[j]))) <= 0.2 * math.sqrt(lengths[i] * lengths[j])
                   for i, j in ((0, 1), (0, 2), (1, 2)))

    valid = {offset for offset in range(0, len(data) - 47, 16) if plausible(offset)}
    runs = []
    for offset in sorted(valid):
        if offset - 48 in valid:
            continue
        count = 1
        while offset + count * 48 in valid:
            count += 1
        if count >= 4:
            runs.append({"offset": offset, "matrix_count": count, "matrix_stride": 48, "heuristic_only": True})
    return runs[:16]


def inspect(path):
    capture = json.loads(path.read_text(encoding="utf-8"))
    if capture.get("schema") != "wwmi-rt-capture-v1":
        raise ValueError("Unsupported capture schema")
    windows = capture.get("upload_windows", [])
    if len(windows) > 128:
        raise ValueError("Too many windows")
    root = path.parent.resolve()
    results = []
    total = 0
    for window in windows:
        result = {"id": window["id"], "root": window["root"], "compute": window.get("compute"),
                  "pipeline_handle": window.get("pipeline_handle"), "errors": [], "affine_candidates": [],
                  "verified_bone_palette": False}
        file = (root / window["file"]).resolve()
        if not file.is_relative_to(root) or file.stat().st_size > 16384:
            raise ValueError("Invalid window path or size")
        data = file.read_bytes()
        total += len(data)
        if total > 2 * 1024 * 1024:
            raise ValueError("Window byte budget exceeded")
        if len(data) != window["bytes"] or fnv1a64(data) != window["fnv1a64"]:
            result["errors"].append("Window length/hash mismatch")
        elif not window.get("copy_fence_completed") or window.get("source_heap") != "UPLOAD":
            result["errors"].append("Missing completed-copy/UPLOAD provenance")
        else:
            result["affine_candidates"] = affine_runs(data)
        results.append(result)
    return {"schema": "wwmi-rt-upload-inspection-v1", "windows": results,
            "readback_status": capture.get("upload_readback", {}), "full_rt_mesh_support": False,
            "limitations": ["Affine-looking constants may be camera/object data rather than bones.",
                            "Windows have unknown CBV extent and may include adjacent allocations.",
                            "Submission-time UPLOAD copies do not verify a draw pose or the native GPU skin output.",
                            "No native palette adapter or RT replacement is generated."]}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("capture", type=Path)
    parser.add_argument("--output", type=Path)
    args = parser.parse_args()
    try:
        report = inspect(args.capture)
        out = args.output or args.capture.with_name("upload-analysis.json")
        out.write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    except (OSError, ValueError, KeyError, TypeError) as error:
        parser.exit(1, f"Could not inspect UPLOAD windows: {error}\n")
    print(f"Windows: {len(report['windows'])}; verified bone palettes: 0")


if __name__ == "__main__":
    main()
