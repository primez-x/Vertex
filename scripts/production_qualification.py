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
    # This is the packaged clean-offline end-to-end workflow. Keep the
    # environmental and deterministic-mode assertions explicit so a generic
    # desktop smoke run cannot masquerade as this production boundary.
    "OPS-QA-004": tuple(dict.fromkeys(CORE + ("packaged_install", "network_denied", "assistance_disabled"))),
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
    observation_owners = {}

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

    def artifact(value, label, *, run_id=None, role=None):
        value = obj(value, label)
        if role is not None:
            if value.get("run_id") != run_id:
                errors.append(label + ": run_id must match the containing run")
            if value.get("role") != role:
                errors.append(label + ": role must be " + role)
            text(value.get("content_description"), label + ".content_description")
            media_type = value.get("media_type")
            if not isinstance(media_type, str) or not re.fullmatch(r"[\w.+-]+/[\w.+-]+", media_type):
                errors.append(label + ": media_type must be a MIME type")
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
            if selected.stat().st_size == 0:
                raise ValueError("evidence file is empty")
            with selected.open("rb") as stream:
                actual = hashlib.file_digest(stream, "sha256").hexdigest()
            if actual != digest.lower():
                raise ValueError("file hash differs from declared sha256")
        except (OSError, ValueError, RuntimeError) as error:
            errors.append(f"{label}: {error}")
            return None
        result = {"path": path.as_posix(), "sha256": actual}
        if role is not None:
            result.update({key: value.get(key) for key in ("run_id", "role", "media_type", "content_description")})
        return result

    def evidence_path_key(reference):
        if reference is None:
            return None
        return (root / pathlib.PurePosixPath(reference["path"])).resolve().as_posix().casefold()

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
        def run_artifact(value, context, role):
            return artifact(value, context, run_id=identity, role=role if kind == "real" else None)

        artifacts = {key: run_artifact(run.get(key), label + "." + key, key) for key in ("application", "source_project", "reopened_project")}
        role_path_keys = {evidence_path_key(value) for value in artifacts.values() if value is not None}
        if kind == "real":
            if len(role_path_keys) != len([value for value in artifacts.values() if value is not None]):
                errors.append(label + ": application, source_project, and reopened_project must be distinct files for real evidence")
            application = artifacts["application"]
            if application is not None and any(project is not None and project["sha256"] == application["sha256"]
                                               for project in (artifacts["source_project"], artifacts["reopened_project"])):
                errors.append(label + ": project content cannot duplicate the application binary")
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
                             "status": status, "evidence": run_artifact(observation.get("evidence"), context + ".evidence", "observation:" + name)}
            if kind == "real" and checked[name]["evidence"] is not None and evidence_path_key(checked[name]["evidence"]) in role_path_keys:
                errors.append(context + ": observation evidence must be separate from application, source_project, and reopened_project artifacts")
            if kind == "real" and checked[name]["evidence"] is not None:
                digest = checked[name]["evidence"]["sha256"]
                observation_owners.setdefault(digest, set()).add(label)
                if digest in {item["sha256"] for item in artifacts.values() if item is not None}:
                    errors.append(context + ": observation evidence content cannot duplicate a role artifact")
        execution = None
        if kind == "real":
            execution = run_artifact(run.get("execution"), label + ".execution", "execution")
            if execution is not None:
                context = label + ".execution"
                if execution["media_type"] != "application/json":
                    errors.append(context + ": media_type must be application/json")
                try:
                    record = obj(load_manifest(root / execution["path"]), context)
                except (OSError, ValueError, RuntimeError) as error:
                    errors.append(f"{context}: invalid execution JSON: {error}")
                    record = {}
                for key, expected in (("schema_version", "1.0"), ("run_id", identity),
                                      ("requirement", requirement), ("recorded_at", timestamp), ("dpi_percent", scale)):
                    if record.get(key) != expected or (key == "dpi_percent" and type(record.get(key)) is not type(expected)):
                        errors.append(context + ": " + key + " differs from run binding")
                process = obj(record.get("process"), context + ".process")
                if type(process.get("pid")) is not int or process["pid"] <= 0:
                    errors.append(context + ": process.pid must be a positive integer")
                if type(process.get("exit_code")) is not int:
                    errors.append(context + ": process.exit_code must be an integer")
                elif process["exit_code"] != 0:
                    blockers.append(context + ": recorded process did not exit successfully")
                expected_artifacts = {key: item["sha256"] if item is not None else None for key, item in artifacts.items()}
                if record.get("artifacts") != expected_artifacts:
                    errors.append(context + ": artifacts must bind all role hashes")
                if process.get("application_sha256") != expected_artifacts["application"] or expected_artifacts["application"] is None:
                    errors.append(context + ": process application hash differs from application artifact")
                expected_observations = {key: {"evidence_sha256": item["evidence"]["sha256"] if item["evidence"] else None,
                                               **{field: item[field] for field in ("status", "expected", "observed")}}
                                         for key, item in checked.items()}
                if record.get("observations") != expected_observations:
                    errors.append(context + ": observations must bind every check, result and evidence hash")
        runs.append({"id": label, "requirement": requirement, "dpi_percent": scale, "evidence_kind": kind,
                     "metadata": {key: run.get(key) for key in ("operator", "recorded_at", "environment", "device", "provenance")},
                     "artifacts": artifacts, "observations": checked, "execution": execution})
    for digest, owners in sorted(observation_owners.items()):
        if len(owners) > 1:
            errors.append("observation evidence content reused across runs: " + digest + " (" + ", ".join(sorted(owners)) + ")")
    required = [(key, None) for key in PRODUCTION] + [("OPS-QA-005", scale) for scale in (100, 150, 200)]
    for item in required:
        if item not in coverage:
            errors.append(f"missing required run: {item[0]} dpi={item[1]}")
    return {"schema_version": "1.0", "contract_valid": not errors,
            "real_evidence_complete": not errors and not blockers,
            "audit_status": "incomplete", "qualification_passed": False, "production_accepted": False,
            "reason": "Caller-declared execution bindings and hashes only; independent runtime review remains required.",
            "errors": sorted(errors), "blockers": sorted(blockers), "runs": sorted(runs, key=lambda run: run["id"])}


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("manifest", type=pathlib.Path)
    parser.add_argument("--root", required=True, type=pathlib.Path)
    args = parser.parse_args(argv)
    try:
        report = validate_and_build(load_manifest(args.manifest), root=args.root)
    except (OSError, ValueError, RuntimeError) as error:
        print(json.dumps({"audit_status": "incomplete", "qualification_passed": False, "production_accepted": False,
                          "contract_valid": False, "real_evidence_complete": False, "errors": [str(error)]}, sort_keys=True))
        return 2
    print(json.dumps(report, indent=2, sort_keys=True, allow_nan=False))
    return 0 if report["real_evidence_complete"] else 1


if __name__ == "__main__":
    sys.exit(main())
