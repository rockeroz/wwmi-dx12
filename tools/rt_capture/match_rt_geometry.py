#!/usr/bin/env python3
"""Find candidate mod/BLAS associations using ordered index windows and raster buffers.

Only CPU metadata is compared. Matches do not verify character ownership, GPU
execution, bone palettes or materials, and must never enable RT replacement.
"""
import argparse
import collections
import json
from pathlib import Path


def number(value):
    return int(value, 0) if isinstance(value, str) else int(value)


def match(capture, profile):
    if capture.get("schema") != "wwmi-rt-capture-v1" or profile.get("schema") != "wwmi-rt-mod-profile-v1":
        raise ValueError("Unsupported capture or mod profile")
    resources = {m["id"]: m for m in capture.get("inventory", []) if m.get("kind") == "resource"}
    states, draws, builds = {}, [], []

    def resolve(ref):
        if not isinstance(ref, dict):
            return None
        if "va" in ref:
            va = number(ref["va"])
        else:
            resource = resources.get(ref.get("resource_id"), {})
            offset = number(ref.get("offset", 0))
            if "va" not in resource or not 0 <= offset < resource.get("bytes", 0):
                return None
            va = number(resource["va"]) + offset
        return va if 0 < va < 2**64 else None

    for e in capture.get("events", []):
        kind = e.get("event")
        if kind == "metadata":
            if e["value"].get("kind") == "resource":
                resources[e["value"]["id"]] = e["value"]
            continue
        if kind == "destroy_resource":
            resources.pop(e.get("resource_id"), None)
            continue
        key = (e.get("list", 0), e.get("generation", 0))
        if kind in ("reset", "destroy_list"):
            states = {k: v for k, v in states.items() if k[0] != key[0]}
        if not key[0] or kind == "destroy_list":
            continue
        state = states.setdefault(key, {})
        if kind == "vertex_buffers" and e.get("first") == 0:
            buffers = e.get("buffers", [])
            state["vb"] = resolve(buffers[0].get("address")) if buffers else None
            state["stride"] = buffers[0].get("stride") if buffers else None
        elif kind == "index_buffer":
            state["ib"] = resolve(e.get("address"))
            state["index_bytes"] = e.get("index_bytes")
        elif kind == "draw_indexed" and e.get("base_vertex", 0) == 0 and state.get("vb") and state.get("ib"):
            draws.append({"seq": e["seq"], "frame": e.get("frame"), **state,
                          "first": e["first_index"], "count": e["count"]})
        elif kind == "as_build" and e.get("type") == 1 and not e.get("inputs_truncated"):
            geometries = []
            for g in e.get("inputs", []):
                if g.get("type") != 0:
                    break
                geometries.append({**g, "vb": resolve(g.get("vertices")), "ib": resolve(g.get("indices")),
                                   "transform_va": resolve(g.get("transform"))})
            else:
                if geometries:
                    builds.append({"event": e, "geometries": geometries})

    candidates = []
    for build in builds:
        geometries = build["geometries"]
        first = geometries[0]
        width = {42: 4, 57: 2}.get(first.get("index_format"))
        if not width or not first.get("vb") or first.get("vertex_stride") != 12 or first.get("vertex_format") != 6:
            continue
        if any(not g.get("ib") or g.get("transform_va") or g.get("vb") != first["vb"] or
               g.get("vertex_count") != first.get("vertex_count") or g.get("vertex_stride") != 12 or
               g.get("vertex_format") != 6 or g.get("index_format") != first.get("index_format") or
               g.get("geometry") != i for i, g in enumerate(geometries)):
            continue
        for mod in profile.get("mods", []):
            if mod.get("errors"):
                continue
            components = sorted(mod.get("components", []), key=lambda c: c["component"])
            if len(components) != len(geometries) or not components:
                continue
            if any(c["component"] != i or c["game_index_count"] <= 0 or
                   c["game_first_index"] < 0 or c["game_index_count"] != g.get("index_count")
                   for i, (c, g) in enumerate(zip(components, geometries))):
                continue
            bases = [g["ib"] - c["game_first_index"] * width for c, g in zip(components, geometries)]
            if bases[0] <= 0 or any(base != bases[0] for base in bases):
                continue
            correlations = collections.Counter()
            correlated_sequences = []
            for draw in draws:
                if (draw["vb"], draw["ib"], draw.get("stride"), draw.get("index_bytes")) != (first["vb"], bases[0], 12, width):
                    continue
                for component in components:
                    if (draw["first"], draw["count"]) == (component["game_first_index"], component["game_index_count"]):
                        correlations[component["component"]] += 1
                        correlated_sequences.append(draw["seq"])
            event = build["event"]
            candidates.append({"mod_ini": mod["ini"], "build_seq": event["seq"], "frame": event.get("frame"),
                               "list": event["list"], "generation": event["generation"], "dest_va": event.get("dest_va"),
                               "vertex_va": hex(first["vb"]), "index_base_va": hex(bases[0]),
                               "native_vertex_count": first.get("vertex_count"), "geometry_count": len(geometries),
                               "mod_vertex_count": mod.get("vertices"),
                               "component_raster_draw_counts": dict(sorted(correlations.items())),
                               "raster_draw_sequences": correlated_sequences,
                               "evidence": "ordered counts and relative index addresses; shared raster VB/IB when present",
                               "verified_character_ownership": False, "ready_for_rt_replacement": False})
    return {"schema": "wwmi-rt-geometry-candidates-v1", "candidates": candidates,
            "general_trace_truncated": capture.get("general_trace_truncated", False),
            "limitations": ["CPU metadata matches are candidates, not proof of GPU execution or character ownership.",
                            "Several mod variants can share the same original topology; no variant is selected.",
                            "Numeric GPU addresses may be reused; cross-recording correlations need lifetime/fence verification.",
                            "No bone palette, native skinning shader, TLAS instance or material/SBT contents are inferred."]}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("capture", type=Path)
    parser.add_argument("mod_profile", type=Path)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    try:
        result = match(json.loads(args.capture.read_text(encoding="utf-8")),
                       json.loads(args.mod_profile.read_text(encoding="utf-8")))
        args.output.write_text(json.dumps(result, indent=2, ensure_ascii=False) + "\n", encoding="utf-8")
    except (OSError, ValueError, KeyError, TypeError) as error:
        parser.exit(1, f"Could not correlate geometry: {error}\n")
    print(f"Candidate mod/build associations: {len(result['candidates'])}; ready for RT replacement: false")


if __name__ == "__main__":
    main()
