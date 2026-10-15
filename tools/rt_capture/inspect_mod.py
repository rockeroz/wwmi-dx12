#!/usr/bin/env python3
"""Inspect WWMI geometry using actual buffer lengths, without editing mod files."""
import argparse
from array import array
import collections
import hashlib
import json
import math
from pathlib import Path
import re
import sys


def sections(text):
    result = {}
    current = None
    for raw in text.splitlines():
        line = raw.split(";", 1)[0].strip()
        if not line:
            continue
        if line.startswith("[") and line.endswith("]"):
            current = line[1:-1].lower()
            result.setdefault(current, [])
        elif current:
            result[current].append(line)
    return result


def attributes(lines):
    return {key.strip().lower(): value.strip() for line in lines if "=" in line for key, value in [line.split("=", 1)]}


def inspect(ini):
    text = ini.read_text(encoding="utf-8-sig")
    parsed = sections(text)
    errors, warnings = [], []
    geometry = {}
    for name in ("Position", "Blend", "Index"):
        attrs = attributes(parsed.get("resource" + name.lower() + "buffer", []))
        filename = attrs.get("filename")
        if not filename:
            raise ValueError(f"Missing Resource{name}Buffer filename")
        rel = Path(filename.replace("\\", "/"))
        path = (ini.parent / rel).resolve()
        if not path.is_relative_to(ini.parent.resolve()):
            raise ValueError("Resource path escapes the mod directory")
        data = path.read_bytes()
        geometry[name.lower()] = {"filename": filename, "bytes": len(data), "sha256": hashlib.sha256(data).hexdigest(),
                                  "format": attrs.get("format"), "declared_stride": int(attrs.get("stride", 0)), "data": data}
    pos = geometry["position"]
    blend = geometry["blend"]
    ib = geometry["index"]
    if pos["declared_stride"] != 12 or "R32G32B32_FLOAT" not in (pos["format"] or "").upper():
        raise ValueError("Only tightly packed FLOAT3 positions are supported")
    if "R32_UINT" not in (ib["format"] or "").upper():
        raise ValueError("Only uint32 index elements are supported")
    if pos["bytes"] % 12 or ib["bytes"] % 4:
        raise ValueError("Misaligned position/index buffer")
    count = pos["bytes"] // 12
    stride = blend["declared_stride"]
    if stride not in (8, 16) or blend["bytes"] != count * stride:
        raise ValueError("Blend buffer must contain 4 or 8 packed byte influences for every position")
    declared = re.search(r"global\s+\$mesh_vertex_count\s*=\s*(\d+)", text, re.I)
    declared_count = int(declared[1]) if declared else None
    if declared_count != count:
        warnings.append(f"Declared vertex count {declared_count} differs from buffer-derived count {count}; do not size RT outputs from the declaration")
    indices = array("I")
    indices.frombytes(ib["data"])
    positions = array("f")
    positions.frombytes(pos["data"])
    if sys.byteorder != "little":
        indices.byteswap()
        positions.byteswap()
    if indices.itemsize != 4 or positions.itemsize != 4:
        raise ValueError("This platform does not provide 32-bit array I/f elements")
    bad_indices = sum(i >= count for i in indices)
    if bad_indices:
        errors.append(f"{bad_indices} indices are outside the position buffer")
    nonfinite = sum(not math.isfinite(v) for v in positions)
    if nonfinite:
        errors.append(f"{nonfinite} non-finite position values")
    sums = collections.Counter()
    bones = set()
    influences = stride // 2
    for v in range(count):
        row = blend["data"][v * stride:(v + 1) * stride]
        sums[sum(row[influences:])] += 1
        bones.update(b for b, w in zip(row[:influences], row[influences:]) if w)
    bad_weights = count - sums[255]
    if bad_weights:
        errors.append(f"{bad_weights} vertices have weights not summing to 255")
    components = []
    for name, lines in parsed.items():
        match = re.fullmatch(r"textureoverridecomponent(\d+)", name)
        if not match:
            continue
        attrs = attributes(lines)
        component = {"component": int(match[1]), "ib_hash": attrs.get("hash"),
                     "game_first_index": int(attrs.get("match_first_index", 0)),
                     "game_index_count": int(attrs.get("match_index_count", 0)), "draws": []}
        conditions = []
        for line in lines:
            lowered = line.lower()
            if lowered.startswith("if "):
                conditions.append(line[3:].strip())
            elif lowered.startswith("elif ") or lowered.startswith("else if "):
                if conditions:
                    conditions[-1] = line
            elif lowered == "else":
                if conditions:
                    conditions[-1] = "else of (" + conditions[-1] + ")"
            elif lowered == "endif":
                if conditions:
                    conditions.pop()
            else:
                draw = re.fullmatch(r"drawindexed\s*=\s*(\d+)\s*,\s*(\d+)\s*,\s*(-?\d+)", line, re.I)
                if not draw:
                    continue
                n, first, base = map(int, draw.groups())
                valid = first <= len(indices) and n <= len(indices) - first
                if not valid:
                    errors.append(f"Component {match[1]} draw {first}+{n} exceeds the mod index buffer")
                elif any(index + base < 0 or index + base >= count for index in indices[first:first + n]):
                    errors.append(f"Component {match[1]} draw references vertices outside the mod position buffer")
                component["draws"].append({"first_index": first, "index_count": n, "base_vertex": base,
                                            "conditions": list(conditions), "index_window_valid": valid})
        components.append(component)
    for value in geometry.values():
        del value["data"]
    return {"ini": ini.name, "declared_vertices": declared_count, "vertices": count, "index_elements": len(indices),
            "max_index": max(indices, default=None), "influences_per_vertex": influences,
            "active_bone_count": len(bones), "max_active_bone": max(bones, default=None),
            "weight_sums": dict(sorted(sums.items())), "nonfinite_position_values": nonfinite,
            "bounds": {"min": [min(positions[i::3], default=0) for i in range(3)],
                       "max": [max(positions[i::3], default=0) for i in range(3)]} if not nonfinite else None,
            "buffers": geometry, "components": components, "errors": errors, "warnings": warnings,
            "native_rt_ready": False}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("mod_directory", type=Path)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    results = []
    try:
        for ini in sorted(args.mod_directory.rglob("*.ini")):
            if "[ResourcePositionBuffer]" not in ini.read_text(encoding="utf-8-sig"):
                continue
            result = inspect(ini)
            result["ini"] = str(ini.relative_to(args.mod_directory))
            results.append(result)
        if not results:
            raise ValueError("No geometry-bearing mod INIs found")
        args.output.write_text(json.dumps({"schema": "wwmi-rt-mod-profile-v1", "mods": results}, indent=2, ensure_ascii=False) + "\n", encoding="utf-8")
    except (OSError, ValueError) as error:
        parser.exit(1, f"Could not inspect mod: {error}\n")
    for result in results:
        print(f"{result['ini']}: {result['vertices']} vertices, {len(result['errors'])} errors, {len(result['warnings'])} warnings")


if __name__ == "__main__":
    main()
