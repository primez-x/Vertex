#!/usr/bin/env python3
"""Check a bounded OPS-PERF evidence contract, without certifying production."""

import argparse
import datetime
import hashlib
import json
import math
import os
from pathlib import Path, PureWindowsPath
import re
import stat

try:
    from .performance_report import validate_report, THRESHOLDS_MS
except ImportError:
    from performance_report import validate_report, THRESHOLDS_MS

MAX_JSON_BYTES = 4 * 1024 * 1024
MAX_ARTIFACT_BYTES = 2 * 1024 * 1024 * 1024
MIN_SAMPLES = {"navigation": 100, "input": 100, "edit": 100, "open": 20, "save": 20}
PROFILES = ("drawing", "architecture", "sheets")
BOUNDARY = (
    "Contract completeness advances evidence readiness only. Caller claims, hardware, operator, "
    "agreement, report authenticity and measurement accuracy are not independently established. "
    "This does not certify production performance or the unified release gate."
)


class InputError(ValueError):
    """Malformed, unsafe, oversized or unreadable input."""


def _unique(pairs):
    result = {}
    for key, value in pairs:
        if key in result:
            raise InputError("duplicate JSON key: " + key)
        result[key] = value
    return result


def _invalid(value):
    raise InputError("nonfinite JSON number: " + value)


def _float(value):
    number = float(value)
    return number if math.isfinite(number) else _invalid(value)


def _json(data):
    try:
        return json.loads(data.decode("utf-8"), object_pairs_hook=_unique,
                          parse_constant=_invalid, parse_float=_float)
    except (ValueError, UnicodeError, RecursionError) as error:
        raise InputError(str(error)) from error


def _check_path(path):
    # Check every ancestor before resolving; resolution would erase link evidence.
    for part in reversed((path, *path.parents)):
        info = part.lstat()
        if stat.S_ISLNK(info.st_mode) or getattr(info, "st_file_attributes", 0) & 0x400:
            raise InputError("symlink or reparse point is forbidden")
    if not stat.S_ISREG(path.lstat().st_mode):
        raise InputError("regular file required")


def _read(path, limit, keep):
    _check_path(path)
    digest, chunks, total = hashlib.sha256(), [], 0
    before = path.stat()
    if not 0 < before.st_size <= limit:
        raise InputError("file size outside allowed bounds")
    with path.open("rb") as stream:
        opened = os.fstat(stream.fileno())
        if (opened.st_dev, opened.st_ino) != (before.st_dev, before.st_ino):
            raise InputError("file changed before read")
        while True:
            chunk = stream.read(min(1024 * 1024, limit + 1 - total))
            if not chunk:
                break
            total += len(chunk)
            if total > limit:
                raise InputError("file size outside allowed bounds")
            digest.update(chunk)
            if keep:
                chunks.append(chunk)
        after = os.fstat(stream.fileno())
    _check_path(path)
    current = path.stat()
    # Windows Python may expose creation time via stat and change time via
    # fstat as st_ctime. Compare ctime only between observations of the handle.
    identity = lambda s: (s.st_dev, s.st_ino, s.st_size, s.st_mtime_ns)
    if (identity(before) != identity(opened) or identity(opened) != identity(after)
            or identity(after) != identity(current) or opened.st_ctime_ns != after.st_ctime_ns
            or total != after.st_size):
        raise InputError("file changed during read")
    file_identity = (opened.st_dev, opened.st_ino)
    if not opened.st_ino:
        file_identity = ("path", os.path.normcase(os.path.realpath(path)))
    return {"bytes": total, "sha256": digest.hexdigest()}, b"".join(chunks), file_identity


def _relative(root, raw):
    if not isinstance(raw, str) or not raw or "\\" in raw or ":" in raw or "\0" in raw:
        raise InputError("reference path must use safe relative POSIX components")
    parts = raw.split("/")
    if PureWindowsPath(raw).drive or any(p in ("", ".", "..") or p.endswith((".", " ")) or
                                       PureWindowsPath(p).is_reserved() for p in parts):
        raise InputError("unsafe reference path")
    return root.joinpath(*parts)


def _text(value):
    return isinstance(value, str) and bool(value.strip()) and len(value) <= 4096


def _hash(value):
    return isinstance(value, str) and re.fullmatch(r"[0-9a-f]{64}", value) is not None


def _count(value):
    return type(value) is int and value >= 0


def _number(value):
    return type(value) in (int, float) and value >= 0 and math.isfinite(value)


def _timestamp(value):
    if not isinstance(value, str) or not re.fullmatch(r"\d{4}-\d\d-\d\dT\d\d:\d\d:\d\d(?:\.\d{1,6})?(?:Z|[+-]\d\d:\d\d)", value):
        return False
    try:
        return datetime.datetime.fromisoformat(value.replace("Z", "+00:00")).utcoffset() is not None
    except ValueError:
        return False


def qualify(path):
    """Return (deterministic report, exit code): 0 complete, 1 gaps, 2 input failure."""
    result = {"schema": "vertex-performance-qualification-result", "schema_version": 1,
              "contract_passed": False, "audit_status": "incomplete", "production_qualified": False,
              "qualification_boundary": BOUNDARY, "evidence": [], "workloads": [], "errors": []}
    errors = result["errors"]

    def require(condition, label):
        if not condition:
            errors.append(label)
        return condition

    def obj(value, label):
        if require(isinstance(value, dict), label + ": object required"):
            return value
        return {}

    try:
        path = Path(os.path.abspath(path))
        metadata, data, _ = _read(path, MAX_JSON_BYTES, True)
        result["evidence"].append(dict(metadata, role="qualification_manifest", path=path.name))
        manifest = obj(_json(data), "manifest")
        require(manifest.get("schema") == "vertex-performance-qualification", "schema")
        require(type(manifest.get("schema_version")) is int and manifest["schema_version"] == 1, "schema_version")
        for field in ("run_id", "operator", "source_revision"):
            require(_text(manifest.get(field)), field)
        require(_timestamp(manifest.get("timestamp")), "timestamp: timezone-qualified ISO-8601 required")
        result["run_id"] = manifest.get("run_id")
        hardware = obj(manifest.get("hardware"), "hardware")
        for field in ("id", "cpu", "gpu", "memory", "os_build"):
            require(_text(hardware.get(field)), "hardware." + field)
        require(hardware.get("agreed") is True, "hardware.agreed")

        def reference(value, role, json_report=False):
            ref = obj(value, role)
            if not require(_text(ref.get("path")) and _hash(ref.get("sha256")), role + ": path and lowercase SHA-256 required"):
                return {}, {}, None
            selected = _relative(path.parent, ref["path"])
            meta, raw, identity = _read(
                selected, MAX_JSON_BYTES if json_report else MAX_ARTIFACT_BYTES, json_report)
            meta.update(role=role, path=ref["path"])
            result["evidence"].append(meta)
            require(meta["sha256"] == ref["sha256"], role + ": SHA-256 mismatch")
            return meta, obj(_json(raw), role) if json_report else {}, identity

        reference(manifest.get("build_artifact"), "build_artifact")
        reference(manifest.get("offline_package_manifest"), "offline_package_manifest", True)
        workloads = manifest.get("workloads")
        if not require(isinstance(workloads, list) and len(workloads) == 3, "exactly three workloads required"):
            workloads = []
        found = []
        document_ids = set()
        project_file_identities = set()
        for index, value in enumerate(workloads):
            label = "workloads[" + str(index) + "]"
            entry = obj(value, label)
            profile = entry.get("profile")
            found.append(profile)
            require(profile in PROFILES, label + ".profile")
            _, desktop, _ = reference(entry.get("desktop_report"), label + ".desktop_report", True)
            _, storage, _ = reference(entry.get("storage_report"), label + ".storage_report", True)
            project, _, project_identity = reference(entry.get("project"), label + ".project")
            roundtrip, _, roundtrip_identity = reference(entry.get("roundtrip"), label + ".roundtrip")
            for identity, role in ((project_identity, "project"),
                                   (roundtrip_identity, "roundtrip")):
                if identity is not None:
                    require(identity not in project_file_identities,
                            label + ": reused " + role + " file identity")
                    project_file_identities.add(identity)
            try:
                validate_report(desktop)
            except (ValueError, OverflowError) as error:
                errors.append(label + ".desktop_report: " + str(error))
                desktop = {}
            require(desktop.get("reference_hardware") == hardware.get("id") and _text(hardware.get("id")), label + ": desktop hardware mismatch")
            metrics = desktop.get("metrics", {})
            sample_counts = {}
            for name, minimum in MIN_SAMPLES.items():
                metric = metrics.get(name, {})
                count = metric.get("sample_count")
                sample_counts[name] = count
                require(_count(count) and count >= minimum, label + ": insufficient " + name + " samples")
                require(metric.get("within_threshold") is True, label + ": " + name + " exceeds target or missing")
                require(metric.get("dropped_sample_count") == 0, label + ": dropped " + name + " samples")
            require(type(storage.get("schema_version")) is int and storage["schema_version"] == 1, label + ": storage schema_version")
            require(storage.get("audit_status") == "incomplete", label + ": storage audit_status")
            require(storage.get("generator") == "vertex-representative-workload-v1", label + ": storage generator")
            provenance = obj(storage.get("provenance"), label + ".provenance")
            for field, expected in (("reference_hardware", hardware.get("id")), ("source_revision", manifest.get("source_revision"))):
                require(_text(expected) and provenance.get(field) == expected, label + ": storage " + field + " mismatch")
            require(provenance.get("labels_verified") is False, label + ": provenance labels must remain unverified")
            facts = obj(storage.get("workload"), label + ".storage.workload")
            desktop_facts = desktop.get("workload", {})
            require(facts.get("id") == "vertex-representative-" + str(profile) + "-v1", label + ": workload identity mismatch")
            for field in ("entities", "objects", "triangles", "sheets", "project_bytes"):
                require(_count(facts.get(field)) and facts[field] == desktop_facts.get(field), label + ": workload " + field + " mismatch")
            for field in ("drawing_entities", "placed_asset_bytes", "asset_bytes"):
                require(_count(facts.get(field)), label + ": invalid " + field)
            if _count(facts.get("drawing_entities")) and _count(facts.get("entities")):
                require(facts["drawing_entities"] <= facts["entities"], label + ": drawing count exceeds entities")
            if _count(facts.get("placed_asset_bytes")) and facts["placed_asset_bytes"] > 0:
                require(_count(facts.get("asset_bytes")) and facts["asset_bytes"] > 0,
                        label + ": placed assets require nonempty asset content")
            if _count(facts.get("placed_asset_bytes")) and _count(facts.get("asset_bytes")):
                require(facts["placed_asset_bytes"] <= facts["asset_bytes"],
                        label + ": placed asset bytes exceed inventoried asset content")
            minimums = {"drawing": {"entities": 50000, "drawing_entities": 50000},
                        "architecture": {"objects": 10000, "triangles": 1000000},
                        "sheets": {"sheets": 20, "placed_asset_bytes": 250000000}}
            for field, minimum in minimums.get(profile if isinstance(profile, str) else "", {}).items():
                require(_count(facts.get(field)) and facts[field] >= minimum, label + ": minimum " + field)
            require(facts.get("id") == desktop_facts.get("id"), label + ": desktop workload identity mismatch")
            require(facts.get("project_bytes") == project.get("bytes"), label + ": project_bytes mismatch")
            integrity = obj(storage.get("integrity"), label + ".storage.integrity")
            document_id = entry.get("document_id")
            require(_text(document_id) and document_id == integrity.get("document_id"), label + ": document identity mismatch")
            if _text(document_id):
                require(document_id not in document_ids, label + ": duplicate document identity")
                document_ids.add(document_id)
            require(_count(entry.get("revision")) and _count(integrity.get("revision")) and entry["revision"] == integrity["revision"], label + ": revision identity mismatch")
            for field in ("authoring_source_sha256", "project_sha256", "roundtrip_sha256"):
                require(_hash(integrity.get(field)), label + ": invalid " + field)
            require(integrity.get("project_sha256") == project.get("sha256"), label + ": project hash mismatch")
            require(integrity.get("roundtrip_sha256") == roundtrip.get("sha256"), label + ": roundtrip hash mismatch")
            require(integrity.get("snapshot_and_assets_equal_after_both_reopens") is True, label + ": snapshot/asset integrity")
            require(_hash(facts.get("semantic_sha256")), label + ": semantic hash")
            assets = facts.get("assets")
            if require(isinstance(assets, list), label + ": assets list required"):
                ids, total = set(), 0
                for asset in assets:
                    asset = obj(asset, label + ".asset")
                    identity = asset.get("id")
                    if require(_text(identity), label + ": asset id"):
                        require(identity not in ids, label + ": duplicate asset id")
                        ids.add(identity)
                    require(_hash(asset.get("sha256")), label + ": asset hash")
                    if require(_count(asset.get("bytes")), label + ": asset bytes"):
                        total += asset["bytes"]
                require(_count(facts.get("asset_bytes")) and total == facts["asset_bytes"], label + ": asset byte total")
            claims = obj(entry.get("integrity"), label + ".integrity")
            for field in ("semantic", "snapshot", "assets", "manifest_validation"):
                require(claims.get(field) is True, label + ": integrity." + field)
            samples = obj(storage.get("samples_ms"), label + ".samples_ms")
            for name in ("open", "save"):
                values = samples.get(name)
                if require(isinstance(values, list) and 2 <= len(values) <= 4096 and all(_number(v) for v in values), label + ": storage " + name + " samples"):
                    p95 = sorted(values)[(95 * len(values) + 99) // 100 - 1]
                    require(p95 <= THRESHOLDS_MS[name], label + ": storage " + name + " exceeds target")
            result["workloads"].append({"profile": profile, "sample_counts": sample_counts,
                                        "counts": {field: facts.get(field) for field in ("entities", "objects", "triangles", "sheets", "placed_asset_bytes")}})
        require(all(found.count(profile) == 1 for profile in PROFILES), "each prescribed profile must occur exactly once")
        cancel = obj(manifest.get("cancellation"), "cancellation")
        for field in ("request", "observation"):
            require(_text(cancel.get(field)), "cancellation." + field)
        require(_number(cancel.get("latency_ms")), "cancellation.latency_ms")
        before = cancel.get("document_revision_before")
        after = cancel.get("document_revision_after")
        require(_count(before), "cancellation.document_revision_before")
        require(_count(after) and after == before, "cancellation.document_revision_after")
        for field in ("stale_result_discarded", "valid_revision_preserved"):
            require(cancel.get(field) is True, "cancellation." + field)
    except (InputError, OSError, UnicodeError, RecursionError, OverflowError) as error:
        errors.append("input error: " + str(error))
        return result, 2
    result["contract_passed"] = not errors
    return result, 1 if errors else 0


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("manifest", type=Path, help="explicit qualification manifest path")
    args = parser.parse_args(argv)
    report, code = qualify(args.manifest)
    print(json.dumps(report, sort_keys=True, indent=2, allow_nan=False))
    return code


if __name__ == "__main__":
    raise SystemExit(main())
