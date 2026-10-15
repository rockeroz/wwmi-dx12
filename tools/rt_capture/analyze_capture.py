#!/usr/bin/env python3
"""Offline metadata analysis. Address overlap is a candidate, never proof of skinning."""
import argparse
import bisect
import collections
import json
from pathlib import Path


def number(value):
    return int(value, 0) if isinstance(value, str) else int(value)


def analyze(capture):
    if capture.get("schema") != "wwmi-rt-capture-v1":
        raise ValueError("Unsupported capture schema")
    metadata = {m["id"]: m for m in capture.get("inventory", [])}
    # Destroyed IDs remain archived; their numeric IDs are never reused by the probe.
    live_resources = {m["id"] for m in metadata.values() if m.get("kind") == "resource"}
    state = {}
    writes = []
    builds = []
    counts = collections.Counter()

    def resolve(ref):
        if not isinstance(ref, dict):
            return None
        if "va" in ref:
            return number(ref["va"]) or None
        rid = ref.get("resource_id", 0)
        res = metadata.get(rid, {}) if rid in live_resources else {}
        if "va" not in res:
            return None
        base, offset = number(res["va"]), number(ref.get("offset", 0))
        if base <= 0 or offset < 0 or offset >= number(res.get("bytes", 0)) or base + offset > 2**64 - 1:
            return None
        return base + offset

    def resource_at(va):
        matches = []
        for rid in live_resources:
            res = metadata.get(rid, {})
            if "va" in res and number(res["va"]) <= va < number(res["va"]) + number(res.get("bytes", 0)):
                matches.append(rid)
        return matches[0] if len(matches) == 1 else None

    for event in capture.get("events", []):
        kind = event.get("event")
        counts[kind] += 1
        if kind == "metadata":
            m = event["value"]
            metadata[m["id"]] = m
            if m.get("kind") == "resource":
                live_resources.add(m["id"])
            continue
        if kind == "destroy_resource":
            live_resources.discard(event.get("resource_id"))
            continue
        key = (event.get("list", 0), event.get("generation", 0))
        if not key[0]:
            continue
        if kind == "reset":
            # Generations already isolate recordings; discard previous list state to bound replay memory.
            state = {k: v for k, v in state.items() if k[0] != key[0]}
        if kind == "destroy_list":
            state = {k: v for k, v in state.items() if k[0] != key[0]}
            continue
        current = state.setdefault(key, {"cs": None, "roots": {}, "layout": None})
        if kind == "bind_pipeline":
            pipe = metadata.get(event.get("pipeline_id"), {})
            for shader in pipe.get("shaders", []):
                if shader.get("stage_name") == "cs":
                    current["cs"] = shader.get("fnv1a64")
        elif kind == "root_descriptors" and event.get("compute"):
            if current["layout"] != event.get("layout_id"):
                current["roots"].clear()
                current["layout"] = event.get("layout_id")
            current["roots"][event["param"]] = event
        elif kind == "tables" and event.get("compute"):
            # A table at a root parameter invalidates an earlier root descriptor there.
            if current["layout"] != event.get("layout_id"):
                current["roots"].clear()
                current["layout"] = event.get("layout_id")
            for param in range(event["first"], event["first"] + len(event["tables"])):
                current["roots"].pop(param, None)
        elif kind == "dispatch":
            candidates = []
            for param, binding in current["roots"].items():
                if binding.get("type") != 5:  # native DX12 root UAV: buffer_unordered_access_view
                    continue
                for ref in binding.get("values", []):
                    va = resolve(ref)
                    if va:
                        candidates.append((param, va, "root_uav"))
            for sample in event.get("buffer_table_samples", []):
                if sample.get("type") not in (3, 5):  # unordered_access_view / buffer_unordered_access_view
                    continue
                va = resolve(sample)
                if va:
                    candidates.append((sample["param"], va, "sampled_table_uav"))
            for param, va, source in candidates:
                writes.append({"va": va, "shader": current["cs"], "param": param, "source": source,
                               "seq": event["seq"], "list": key[0], "generation": key[1], "resource_id": resource_at(va)})
        elif kind == "as_build" and event.get("type") == 1:
            for geometry in event.get("inputs", []):
                if geometry.get("type") != 0:  # triangles
                    continue
                va = resolve(geometry.get("vertices"))
                count = geometry.get("vertex_count", 0)
                stride = geometry.get("vertex_stride", 0)
                # DXGI/ReShade R32G32B32_FLOAT == 6. Other formats remain unresolved.
                extent = (count - 1) * stride + 12 if count and stride >= 12 and geometry.get("vertex_format") == 6 else 0
                if not va or not extent or va + extent > 2**64 - 1:
                    continue
                builds.append({"va": va, "extent": extent, "vertices": count, "indices": geometry.get("index_count", 0),
                               "geometry": geometry["geometry"], "dest_va": event["dest_va"], "seq": event["seq"],
                               "list": key[0], "generation": key[1], "resource_id": resource_at(va)})

    writes.sort(key=lambda item: item["va"])
    addresses = [item["va"] for item in writes]
    links = []
    seen = set()
    for build in builds:
        lo = bisect.bisect_left(addresses, build["va"])
        hi = bisect.bisect_left(addresses, build["va"] + build["extent"])
        candidates = []
        unique = set()
        for write in writes[lo:hi]:
            if write["seq"] >= build["seq"]:
                continue
            if write["resource_id"] and build["resource_id"] and write["resource_id"] != build["resource_id"]:
                continue
            identity = (write["shader"], write["param"], write["source"])
            if identity in unique:
                continue
            unique.add(identity)
            same_list = write["list"] == build["list"] and write["generation"] == build["generation"]
            candidates.append({**write, "same_recording": same_list,
                               "evidence": "address overlap; CPU callback order only"})
            if len(candidates) == 8:
                break
        identity = (build["dest_va"], build["geometry"], build["va"])
        if candidates and identity not in seen:
            seen.add(identity)
            links.append({**build, "candidates": candidates})
    return {"schema": "wwmi-rt-analysis-v1", "full_rt_mesh_support": False,
            "event_counts": dict(counts), "blas_geometries_resolved": len(builds),
            "candidate_links": links, "dropped_events": capture.get("dropped_events", 0),
            "dropped_metadata": capture.get("dropped_metadata", 0),
            "dropped_descriptors": capture.get("dropped_descriptors", 0),
            "limitations": ["Candidate links do not prove writes, character ownership or bone palette layout.",
                            "Cross-list GPU order requires submission/fence analysis.",
                            "SBT local records, instance descriptors and skinned vertex contents need GPU readback.",
                            "Table samples can include unrelated descriptors and recycled heap addresses."]}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("capture", type=Path)
    parser.add_argument("--output", type=Path)
    args = parser.parse_args()
    try:
        report = analyze(json.loads(args.capture.read_text(encoding="utf-8")))
    except (OSError, ValueError, KeyError, TypeError) as error:
        parser.exit(1, f"Could not analyze capture: {error}\n")
    output = args.output or args.capture.with_name("analysis.json")
    try:
        output.write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    except OSError as error:
        parser.exit(1, f"Could not save analysis: {error}\n")
    print(f"BLAS geometries: {report['blas_geometries_resolved']}; candidate links: {len(report['candidate_links'])}")
    print(output)


if __name__ == "__main__":
    main()
