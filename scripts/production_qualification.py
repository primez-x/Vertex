"""Check explicit production/accessibility evidence; never certify runtime behavior."""

import argparse
import datetime
import hashlib
import json
import math
import pathlib
import re
import sys

CORE = ("create", "measure", "editable_3d", "edit", "calculate", "revise", "save_reopen", "recovery", "print", "export")
ASSERTIONS = ("semantic", "calculation", "output", "recovery", "fidelity")
PRODUCTION = {
    "OPS-QA-002": tuple(dict.fromkeys(CORE + ("architectural_authoring", "plans", "elevations", "sections", "schedules", "alternatives", "revisions", "sheets") + ASSERTIONS)),
    "OPS-QA-003": tuple(dict.fromkeys(CORE + ("multiple_levels", "assemblies", "structural_objects", "schedules", "quantities", "sheets", "coordinated_views") + ASSERTIONS)),
}
ACCESSIBILITY = ("real_pen", "real_touch", "keyboard_navigation", "focus", "accessible_properties", "light_theme", "dark_theme", "high_contrast", "dpi_layout")


def load_manifest(path):
    def unique(pairs):
        result = {}
        for key, value in pairs:
            if key in result:
                raise ValueError(f"duplicate JSON key: {key}")
            result[key] = value
        return result

    def invalid_constant(value):
        raise ValueError(f"nonfinite JSON number: {value}")

    def finite_float(value):
        parsed = float(value)
        if not math.isfinite(parsed):
            return invalid_constant(value)
        return parsed

    return json.loads(pathlib.Path(path).read_text(encoding="utf-8"), object_pairs_hook=unique,
                      parse_constant=invalid_constant, parse_float=finite_float)


def validate_and_build(manifest, *, root):
    root = pathlib.Path(root).resolve()
    errors, blockers, runs = [], [], []
    coverage, identities = set(), set()

    def obj(value, label):
        if not isinstance(value, dict):
            errors.append(label + ": object required")
            return {}
        return value

    def text(value, label):
        if not isinstance(value, str) or not value.strip():
            errors.append(label + ": nonempty string required")
            return False
        return True

    def artifact(value, label):
        value = obj(value, label)
        raw, digest = value.get("path"), value.get("sha256")
        if not text(raw, label + ".path"):
            return None
        path = pathlib.PurePosixPath(raw.replace("\\", "/"))
        win = pathlib.PureWindowsPath(raw)
        if win.drive or win.root or path.is_absolute() or ".." in path.parts or ":" in raw or "\0" in raw:
            errors.append(label + ": unsafe path")
            return None
        if not isinstance(digest, str) or not re.fullmatch("[0-9a-fA-F]{64}", digest):
            errors.append(label + ": sha256 must contain 64 hex digits")
            return None
        try:
            selected = (root / path).resolve()
            if not selected.is_relative_to(root) or not selected.is_file():
                raise ValueError("missing file or path escapes evidence root")
            with selected.open("rb") as stream:
                actual = hashlib.file_digest(stream, "sha256").hexdigest()
            if actual != digest.lower():
                raise ValueError("file hash differs from declared sha256")
        except (OSError, ValueError, RuntimeError) as error:
            errors.append(f"{label}: {error}")
            return None
        return {"path": path.as_posix(), "sha256": actual}

    manifest = obj(manifest, "manifest")
    if manifest.get("schema_version") != "1.0":
        errors.append("schema_version: expected 1.0")
    supplied = manifest.get("runs")
    if not isinstance(supplied, list):
        errors.append("runs: explicit list required")
        supplied = []
    for index, value in enumerate(supplied):
        run = obj(value, f"runs[{index}]")
        identity = run.get("id")
        label = identity if isinstance(identity, str) and identity.strip() else f"runs[{index}]"
        if text(identity, label + ".id"):
            if identity in identities:
                errors.append(label + ": duplicate run id")
            identities.add(identity)
        requirement = run.get("requirement")
        if not isinstance(requirement, str) or requirement not in (*PRODUCTION, "OPS-QA-005"):
            errors.append(label + ": unsupported requirement")
            continue
        scale = run.get("dpi_percent")
        if requirement == "OPS-QA-005":
            if type(scale) is not int or scale not in (100, 150, 200):
                errors.append(label + ": dpi_percent must be 100, 150, or 200")
            else:
                coverage.add((requirement, scale))
            checks = ACCESSIBILITY + CORE
            text(run.get("device"), label + ".device")
        else:
            coverage.add((requirement, None))
            checks = PRODUCTION[requirement]
        for key in ("operator", "environment", "provenance"):
            text(run.get(key), label + "." + key)
        timestamp = run.get("recorded_at")
        try:
            if not isinstance(timestamp, str) or datetime.datetime.fromisoformat(timestamp.replace("Z", "+00:00")).utcoffset() is None:
                raise ValueError()
        except ValueError:
            errors.append(label + ": recorded_at requires ISO 8601 timestamp with timezone")
        kind = run.get("evidence_kind")
        if kind not in ("real", "synthetic"):
            errors.append(label + ": evidence_kind must be real or synthetic")
        if kind != "real":
            blockers.append(label + ": real runtime evidence not declared")
        artifacts = {key: artifact(run.get(key), label + "." + key) for key in ("application", "source_project", "reopened_project")}
        observations = obj(run.get("observations"), label + ".observations")
        for name in sorted(set(observations) - set(checks)):
            errors.append(label + ".observations: unsupported check " + name)
        checked = {}
        for name in checks:
            context = label + ".observations." + name
            observation = obj(observations.get(name), context)
            for key in ("expected", "observed"):
                text(observation.get(key), context + "." + key)
            status = observation.get("status")
            if status not in ("pass", "fail", "blocked", "not_run"):
                errors.append(context + ": explicit status required")
            if status != "pass":
                blockers.append(context + ": observation has not passed")
            checked[name] = {"expected": observation.get("expected"), "observed": observation.get("observed"),
                             "status": status, "evidence": artifact(observation.get("evidence"), context + ".evidence")}
        runs.append({"id": label, "requirement": requirement, "dpi_percent": scale, "evidence_kind": kind,
                     "metadata": {key: run.get(key) for key in ("operator", "recorded_at", "environment", "device", "provenance")},
                     "artifacts": artifacts, "observations": checked})
    required = [(key, None) for key in PRODUCTION] + [("OPS-QA-005", scale) for scale in (100, 150, 200)]
    for item in required:
        if item not in coverage:
            errors.append(f"missing required run: {item[0]} dpi={item[1]}")
    return {"schema_version": "1.0", "contract_valid": not errors,
            "real_evidence_complete": not errors and not blockers,
            "audit_status": "incomplete", "qualification_passed": False,
            "reason": "Caller-declared evidence and hashes only; independent runtime review remains required.",
            "errors": sorted(errors), "blockers": sorted(blockers), "runs": sorted(runs, key=lambda run: run["id"])}


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("manifest", type=pathlib.Path)
    parser.add_argument("--root", required=True, type=pathlib.Path)
    args = parser.parse_args(argv)
    try:
        report = validate_and_build(load_manifest(args.manifest), root=args.root)
    except (OSError, ValueError, RuntimeError) as error:
        print(json.dumps({"audit_status": "incomplete", "qualification_passed": False,
                          "contract_valid": False, "real_evidence_complete": False, "errors": [str(error)]}, sort_keys=True))
        return 2
    print(json.dumps(report, indent=2, sort_keys=True, allow_nan=False))
    return 0 if report["real_evidence_complete"] else 1


if __name__ == "__main__":
    sys.exit(main())
