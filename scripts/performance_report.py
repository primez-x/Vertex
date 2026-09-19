#!/usr/bin/env python3
"""Validate application timing evidence without claiming qualification or launching UI."""

import argparse
import hashlib
import json
import math
from pathlib import Path
import sys


THRESHOLDS_MS = {"navigation": 16.7, "input": 50, "edit": 250, "open": 5000, "save": 5000}
MAX_REPORT_BYTES = 4 * 1024 * 1024
# Version-1 desktop reports retain at most this many samples per metric.
# Keep aligned with PerformanceTelemetry::max_samples_per_metric.
MAX_SAMPLES_PER_METRIC = 4096


def _require(condition, field):
    if not condition:
        raise ValueError(f"invalid performance report schema: {field}")


def _object(pairs):
    result = {}
    for key, value in pairs:
        if key in result:
            raise ValueError(f"duplicate performance report key: {key}")
        result[key] = value
    return result


def _finite_float(value):
    result = float(value)
    if not math.isfinite(result):
        raise ValueError("non-finite performance report number")
    return result


def _invalid_constant(value):
    raise ValueError(f"invalid performance report JSON constant: {value}")


def _count(value):
    return type(value) is int and value >= 0


def _number(value):
    return type(value) in (int, float) and value >= 0 and math.isfinite(value)


def validate_report(report):
    _require(isinstance(report, dict), "root must be an object")
    _require(type(report.get("schema_version")) is int and report["schema_version"] == 1,
             "schema_version")
    _require(report.get("audit_status") == "incomplete", "audit_status")
    hardware = report.get("reference_hardware")
    _require(isinstance(hardware, str) and bool(hardware.strip()), "reference_hardware")
    workload = report.get("workload")
    _require(isinstance(workload, dict), "workload")
    _require(isinstance(workload.get("id"), str) and bool(workload["id"].strip()), "workload.id")
    # Unmeasured tessellation and unknown persisted file size have no numeric value.
    for field in ("entities", "objects", "sheets", "project_bytes", "triangles"):
        _require(field in workload and (_count(workload[field]) or
                 (field in ("triangles", "project_bytes") and workload[field] is None)), f"workload.{field}")
    # Objects are entities; sheets are decoded children of a model entity and
    # can outnumber entities. These counts describe final state, not every sample.
    _require(workload["objects"] <= workload["entities"],
             "workload object count exceeds entities")
    metrics = report.get("metrics")
    _require(isinstance(metrics, dict) and metrics.keys() == THRESHOLDS_MS.keys(), "metrics")
    all_sampled = True
    all_within = True
    for name, threshold in THRESHOLDS_MS.items():
        metric = metrics[name]
        _require(isinstance(metric, dict), f"metrics.{name}")
        for field in ("sample_count", "dropped_sample_count"):
            _require(_count(metric.get(field)), f"{name}.{field}")
        _require(metric["sample_count"] <= MAX_SAMPLES_PER_METRIC,
                 f"{name}.sample_count exceeds recorder capacity")
        for field in ("has_samples", "within_threshold"):
            _require(type(metric.get(field)) is bool, f"{name}.{field}")
        _require(_number(metric.get("threshold_ms")) and metric["threshold_ms"] == threshold,
                 f"{name}.threshold_ms")
        sampled = metric["sample_count"] > 0
        _require(metric["has_samples"] == sampled, f"{name}.has_samples inconsistent with count")
        _require("p95_ms" in metric, f"{name}.p95_ms missing")
        _require(_number(metric["p95_ms"]) if sampled else metric["p95_ms"] is None,
                 f"{name}.p95_ms inconsistent with samples")
        within = sampled and metric["p95_ms"] <= threshold
        _require(metric["within_threshold"] == within, f"{name}.within_threshold inconsistent with p95")
        all_sampled = all_sampled and sampled
        all_within = all_within and within
    expected = "incomplete" if not all_sampled else ("within_targets" if all_within else "exceeds_targets")
    _require(report.get("threshold_status") == expected, "threshold_status inconsistent with metrics")


def performance_evidence(path: Path) -> dict:
    """Hash exactly the bounded bytes validated, retaining the incomplete audit boundary."""
    try:
        with path.open("rb") as stream:
            data = stream.read(MAX_REPORT_BYTES + 1)
        _require(0 < len(data) <= MAX_REPORT_BYTES, "report size")
        report = json.loads(data.decode("utf-8"), object_pairs_hook=_object,
                            parse_float=_finite_float, parse_constant=_invalid_constant)
        validate_report(report)
    except (OSError, UnicodeError, ValueError, RecursionError, OverflowError) as error:
        raise ValueError(f"invalid performance report: {path}: {error}") from error
    return {"path": str(path), "bytes": len(data), "sha256": hashlib.sha256(data).hexdigest(),
            "threshold_status": report["threshold_status"],
            "validation": "desktop report schema, metric consistency and incomplete audit boundary only; origin and measurements are not authenticated"}


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("report", type=Path)
    args = parser.parse_args(argv)
    try:
        evidence = performance_evidence(args.report)
    except ValueError as error:
        print(str(error), file=sys.stderr)
        return 1
    print(json.dumps(evidence))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
