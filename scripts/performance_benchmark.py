#!/usr/bin/env python3
"""Summarize supplied timing measurements without certifying runtime performance."""
import argparse
import copy
import json
import math
from pathlib import Path
import sys

THRESHOLDS_MS = {"navigation": 16.7, "input": 50, "edit": 250, "open": 5000, "save": 5000}
WORKLOAD_COUNTS = ("entities", "objects", "triangles", "sheets", "project_bytes")
MAX_SAMPLES_PER_METRIC = 1_000_000
MAX_INPUT_BYTES = 64 * 1024 * 1024


def _label(value, name):
    if not isinstance(value, str) or not value.strip() or len(value) > 4096:
        raise ValueError(f"{name} must be a nonempty label of at most 4096 characters")
    return value


def build_report(data):
    """Validate one workload and compute nearest-rank p95 in milliseconds."""
    if not isinstance(data, dict):
        raise ValueError("input must be a JSON object")
    if set(data) != {"workload", "reference_hardware", "samples_ms"}:
        raise ValueError("input requires exactly workload, reference_hardware, and samples_ms")
    hardware = _label(data["reference_hardware"], "reference_hardware")
    workload = data["workload"]
    if not isinstance(workload, dict) or set(workload) != {"id", *WORKLOAD_COUNTS}:
        raise ValueError("workload requires id, entities, objects, triangles, sheets, project_bytes")
    _label(workload["id"], "workload.id")
    for key in WORKLOAD_COUNTS:
        if type(workload[key]) is not int or workload[key] < 0:
            raise ValueError(f"workload.{key} must be a nonnegative integer")
    samples = data["samples_ms"]
    if not isinstance(samples, dict) or set(samples) != set(THRESHOLDS_MS):
        raise ValueError("samples_ms requires exactly navigation, input, edit, open, save")
    metrics = {}
    for name, threshold in THRESHOLDS_MS.items():
        values = samples[name]
        if not isinstance(values, list) or not 1 <= len(values) <= MAX_SAMPLES_PER_METRIC:
            raise ValueError(f"samples_ms.{name} must contain 1..{MAX_SAMPLES_PER_METRIC} samples")
        for value in values:
            try:
                valid = type(value) in (int, float) and math.isfinite(value) and value >= 0
            except OverflowError:
                valid = False
            if not valid:
                raise ValueError(f"samples_ms.{name} requires finite nonnegative numeric timings")
        ordered = sorted(values)
        # Integer arithmetic avoids percentile-index rounding at large sample counts.
        p95 = ordered[(95 * len(ordered) + 99) // 100 - 1]
        metrics[name] = {"sample_count": len(values), "p95_ms": p95,
                         "threshold_ms": threshold, "within_threshold": p95 <= threshold}
    return {
        "schema_version": 1,
        "audit_status": "incomplete",
        "evidence_scope": "offline summary of caller-supplied measurements; not hardware qualification",
        "percentile_method": "nearest-rank: sorted[ceil(0.95 * count) - 1]",
        "workload": copy.deepcopy(workload),
        "reference_hardware": hardware,
        "metrics": metrics,
        "threshold_status": "within_targets" if all(m["within_threshold"] for m in metrics.values()) else "failed",
        "unresolved_gates": {
            "OPS-PERF-001": "Agreed hardware and representative 50,000-entity, 10,000-object/1,000,000-triangle, and 20-sheet/250 MB workloads require independent recorded runtime evidence.",
            "OPS-PERF-002": "Instrumentation and cancellable background regeneration preserving a valid revision are not verified by timing samples.",
            "OPS-PERF-003": "Representative project selection, snapshot/revision identity, asset integrity, and manifest validation require independent evidence.",
        },
    }


def _unique_object(pairs):
    result = {}
    for key, value in pairs:
        if key in result:
            raise ValueError(f"duplicate JSON key: {key}")
        result[key] = value
    return result


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--input", required=True, type=Path, help="explicit workload and measured sample JSON")
    parser.add_argument("--output", type=Path, help="report JSON; defaults to standard output")
    args = parser.parse_args(argv)
    try:
        if args.output and args.input.resolve() == args.output.resolve():
            raise ValueError("output must not overwrite input")
        with args.input.open("rb") as stream:
            raw = stream.read(MAX_INPUT_BYTES + 1)
        if len(raw) > MAX_INPUT_BYTES:
            raise ValueError(f"input exceeds {MAX_INPUT_BYTES} bytes")
        data = json.loads(raw.decode("utf-8-sig"), object_pairs_hook=_unique_object)
        report = build_report(data)
        rendered = json.dumps(report, indent=2, sort_keys=True, allow_nan=False) + "\n"
        if args.output:
            args.output.write_text(rendered, encoding="utf-8")
        else:
            sys.stdout.write(rendered)
    except (OSError, ValueError, RecursionError) as error:
        print(f"performance benchmark: {error}", file=sys.stderr)
        return 2
    return 1 if report["threshold_status"] == "failed" else 0


if __name__ == "__main__":
    raise SystemExit(main())
