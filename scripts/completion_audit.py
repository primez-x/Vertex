"""Build a deterministic checklist of implementation and release evidence.

This report is deliberately fail-closed.  It records what the repository and
captured artifacts prove, separates development-host smoke from production
qualification, and never turns a passing internal test into a release claim.
"""

from __future__ import annotations

import argparse
import collections
import hashlib
import importlib.util
import json
import pathlib
import re
import sys
from collections.abc import Mapping


ROOT = pathlib.Path(__file__).resolve().parents[1]
SCRIPT_DIR = pathlib.Path(__file__).resolve().parent


def _load_requirement_audit():
    try:
        from requirement_audit import release_gaps, source_fingerprint, validate_contract

        return release_gaps, source_fingerprint, validate_contract
    except ImportError:
        path = SCRIPT_DIR / "requirement_audit.py"
        spec = importlib.util.spec_from_file_location("requirement_audit_for_completion", path)
        if spec is None or spec.loader is None:
            raise ImportError(f"could not load {path}")
        module = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(module)
        return module.release_gaps, module.source_fingerprint, module.validate_contract


release_gaps, source_fingerprint, validate_contract = _load_requirement_audit()


REQUIRED_DOCUMENTS = (
    "README.md",
    "docs/production-plan.md",
    "docs/project-format.md",
    "docs/production-qualification.md",
    "docs/dependencies/offline-static-audit.md",
    "docs/dependencies/source-provenance.md",
    "docs/requirements/apex-parity.json",
    "docs/requirements/production-gates.json",
)
REQUIRED_CONFIGURATION = (
    "CMakeLists.txt",
    "CMakePresets.json",
    "vcpkg.json",
    ".gitattributes",
)
COMPATIBILITY_REQUIREMENTS = {
    "APX-COMPAT-001",
    "APX-COMPAT-002",
    "APX-NATIVE-AX5-001",
    "APX-NATIVE-AX7-001",
    "APX-NATIVE-LEGACY-001",
    "APX-INT-001",
    "APX-INT-002",
    "APX-INPUT-002",
}


def _load_json(path: pathlib.Path):
    def unique(pairs):
        result = {}
        for key, value in pairs:
            if key in result:
                raise ValueError(f"duplicate JSON key: {key}")
            result[key] = value
        return result

    def invalid_constant(value):
        raise ValueError(f"nonfinite JSON number: {value}")

    return json.loads(path.read_text(encoding="utf-8"),
                      object_pairs_hook=unique, parse_constant=invalid_constant)


def _relative(path: pathlib.Path, root: pathlib.Path) -> str:
    try:
        return path.resolve().relative_to(root).as_posix()
    except (OSError, ValueError):
        return str(path)


def _check(identifier: str, status: str, summary: str, evidence=(), details=()):
    return {
        "id": identifier,
        "status": status,
        "summary": summary,
        "evidence": sorted(str(item) for item in evidence),
        "details": sorted(str(item) for item in details),
    }


def _path_check(root: pathlib.Path, identifier: str, label: str, paths):
    missing = [path for path in paths
               if not (root / path).is_file() or (root / path).stat().st_size == 0]
    evidence = [path for path in paths if (root / path).is_file()]
    if missing:
        return _check(identifier, "missing", f"{label} is incomplete", evidence,
                      [f"missing or empty: {path}" for path in missing])
    return _check(identifier, "pass", f"{label} is present and non-empty", evidence)


def runtime_status(report):
    """Classify an installed-runtime report without claiming certification."""

    if not isinstance(report, Mapping):
        return _check("runtime_smoke", "missing", "No installed-runtime report was supplied")
    errors = report.get("errors")
    if report.get("passed") is not True:
        return _check("runtime_smoke", "blocked", "Installed-runtime smoke did not pass",
                      details=errors if isinstance(errors, list) else ("passed flag is false",))
    if isinstance(errors, list) and errors:
        return _check("runtime_smoke", "blocked", "Installed-runtime smoke reported errors",
                      details=errors)
    environment = report.get("environment")
    environment = environment if isinstance(environment, Mapping) else {}
    missing = []
    for field in ("production_qualified", "clean_machine", "network_denied"):
        if report.get(field) is not True:
            missing.append(field)
    if environment.get("registry_isolated") is not True:
        missing.append("registry_isolated")
    if environment.get("other_environment_inherited") is not False:
        missing.append("other_environment_inherited=false")
    if missing:
        return _check("runtime_smoke", "partial",
                      "Installed-runtime smoke passed, but production boundaries are unproven",
                      details=["unproven boundary: " + item for item in missing])
    return _check("runtime_smoke", "pass", "Runtime report records a clean isolated offline run")


def test_log_status(path: pathlib.Path):
    """Classify a CTest log using only its recorded terminal markers."""

    if not path.is_file():
        return _check("test_matrix", "missing", "CTest log is missing", details=(str(path),))
    try:
        text = path.read_text(encoding="utf-8", errors="replace")
    except OSError as error:
        return _check("test_matrix", "blocked", "CTest log could not be read", details=(str(error),))
    if "Test Failed." in text or re.search(r"\*\*\*Failed", text):
        return _check("test_matrix", "blocked", "CTest log contains a failed test",
                      evidence=(str(path),))
    if "End testing:" not in text or "Test Passed." not in text:
        return _check("test_matrix", "partial", "CTest log is incomplete", evidence=(str(path),))
    passed = text.count("Test Passed.")
    return _check("test_matrix", "pass", f"CTest log records {passed} passing test entries",
                  evidence=(str(path),))


def _latest_runtime_report(root: pathlib.Path):
    candidates = sorted((root / "artifacts/installed-runtime/current").glob(
        "run-*/report.json"))
    return candidates[-1] if candidates else None


def _offline_static_check(root: pathlib.Path):
    """Classify the direct-import offline audit without certifying runtime use."""

    path = root / "artifacts/runtime/release-imports.json"
    script = root / "scripts/offline_static_audit.py"
    if not path.is_file():
        return _check("offline_static", "missing", "No Release PE import report is available")
    if not script.is_file():
        return _check("offline_static", "missing", "Offline static audit script is missing",
                      evidence=(_relative(path, root),))
    try:
        spec = importlib.util.spec_from_file_location("offline_static_audit_for_completion", script)
        if spec is None or spec.loader is None:
            raise ImportError(f"could not load {script}")
        module = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(module)
        result = module.audit_report(module.load_import_report(path))
    except (OSError, ValueError, RuntimeError, ImportError, json.JSONDecodeError) as error:
        return _check("offline_static", "blocked", "Offline static import report is invalid",
                      evidence=(_relative(path, root), _relative(script, root)), details=(str(error),))
    status = "pass" if result.get("static_network_audit_passed") is True else "blocked"
    return _check("offline_static", status,
                  "Application entry points have no direct network imports" if status == "pass" else
                  "Application entry points have a direct or unresolved network import",
                  evidence=(_relative(path, root), _relative(script, root)),
                  details=result.get("errors", ()))


def _source_provenance_check(root: pathlib.Path):
    """Verify that tracked project paths have one declared ownership class."""

    manifest = root / "third_party/source-provenance.json"
    script = root / "scripts/source_provenance_audit.py"
    evidence = (_relative(manifest, root), _relative(script, root))
    if not manifest.is_file():
        return _check("source_provenance", "missing",
                      "Source ownership manifest is missing", evidence=evidence)
    if not script.is_file():
        return _check("source_provenance", "missing",
                      "Source ownership audit script is missing", evidence=evidence)
    try:
        spec = importlib.util.spec_from_file_location("source_provenance_for_completion", script)
        if spec is None or spec.loader is None:
            raise ImportError(f"could not load {script}")
        module = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(module)
        result = module.audit_repository(root, manifest)
    except (OSError, ValueError, RuntimeError, ImportError, UnicodeError) as error:
        return _check("source_provenance", "blocked",
                      "Source ownership audit could not be evaluated",
                      evidence=evidence, details=(str(error),))
    status = "pass" if result.get("audit_status") == "pass" else "blocked"
    return _check("source_provenance", status,
                  "Tracked paths have exactly one declared ownership class" if status == "pass" else
                  "Source ownership boundary is incomplete or overlapping",
                  evidence=evidence,
                  details=result.get("errors", ()))


def _latest_test_logs(root: pathlib.Path):
    return [root / "build/windows-release/Testing/Temporary/LastTest.log",
            root / "build/windows-debug/Testing/Temporary/LastTest.log"]


def _implementation_check(ledger):
    rows = ledger.get("requirements", []) if isinstance(ledger, Mapping) else []
    counts = collections.Counter(row.get("implementation_status") for row in rows
                                 if isinstance(row, Mapping))
    if counts.get("verified", 0) == len(rows) and rows:
        status = "pass"
        summary = "Every requirement is marked verified"
    else:
        status = "partial"
        summary = "Implementation is present in development checkpoints; verification remains open"
    details = [f"{key or 'unknown'}={value}" for key, value in sorted(counts.items(), key=lambda item: str(item[0]))]
    return _check("implementation_matrix", status, summary, details=details), counts


def _compatibility_check(ledger):
    rows = {row.get("id"): row for row in ledger.get("requirements", [])
            if isinstance(row, Mapping)}
    unresolved = []
    for identifier in sorted(COMPATIBILITY_REQUIREMENTS):
        row = rows.get(identifier)
        if row is None:
            unresolved.append(identifier + ": missing ledger row")
        elif row.get("implementation_status") != "verified" or row.get("evidence_status") != "documented":
            unresolved.append(identifier + ": implementation or evidence is unresolved")
    if unresolved:
        return _check("compatibility", "blocked",
                      "Apex editions, native files, callers, and device adapters are not certified",
                      details=unresolved)
    return _check("compatibility", "pass", "Compatibility requirements have verified evidence")


def _qualification_check(root):
    path = root / "docs/production-qualification.pending.json"
    if not path.is_file():
        return _check("qualification_manifest", "missing", "Production qualification manifest is missing")
    try:
        manifest = _load_json(path)
    except (OSError, ValueError) as error:
        return _check("qualification_manifest", "blocked", "Production qualification manifest is invalid",
                      evidence=(_relative(path, root),), details=(str(error),))
    runs = manifest.get("runs") if isinstance(manifest, Mapping) else None
    if not isinstance(runs, list) or not runs:
        return _check("qualification_manifest", "blocked",
                      "No real production or accessibility runs are declared",
                      evidence=(_relative(path, root),))
    return _check("qualification_manifest", "partial",
                  "Qualification runs exist but still require independent review",
                  evidence=(_relative(path, root),))


def _configuration_check(root):
    paths = [root / item for item in REQUIRED_CONFIGURATION]
    missing = [item for item, path in zip(REQUIRED_CONFIGURATION, paths)
               if not path.is_file() or path.stat().st_size == 0]
    cmake = root / "CMakeLists.txt"
    markers = ()
    if cmake.is_file():
        text = cmake.read_text(encoding="utf-8", errors="replace")
        markers = (
            "production_acceptance_gate",
            "production_qualification_contract",
            'set_tests_properties(production_acceptance_gate PROPERTIES LABELS "production" DISABLED TRUE)',
        )
        missing.extend("CMakeLists.txt marker: " + marker for marker in markers if marker not in text)
    if missing:
        return _check("configuration", "blocked", "Build and gate configuration is incomplete",
                      evidence=[item for item in REQUIRED_CONFIGURATION if (root / item).is_file()],
                      details=missing)
    return _check("configuration", "pass", "Build metadata and explicit acceptance gate are present",
                  evidence=REQUIRED_CONFIGURATION)


def _packaging_check(root):
    package = "artifacts/packages/property-studio-offline-current"
    paths = [package + "/offline-bundle-manifest.json",
             package + "/runtime-manifest.json",
             package + "/metadata/distribution-sbom.spdx.json",
             package + "/metadata/source-kit-manifest.json"]
    result = _path_check(root, "packaging", "Offline package integrity inputs", paths)
    if result["status"] == "pass":
        result["status"] = "partial"
        result["summary"] = "Offline package integrity inputs are present; clean-machine qualification remains open"
    return result


def build_report(root: pathlib.Path | str = ROOT):
    root = pathlib.Path(root).resolve()
    checks = []
    ledger = None
    gates = None
    ledger_path = root / "docs/requirements/apex-parity.json"
    gates_path = root / "docs/requirements/production-gates.json"
    try:
        ledger = _load_json(ledger_path)
        gates = _load_json(gates_path)
        contract_errors = validate_contract(ledger, gates)
        checks.append(_check("requirements_contract", "pass" if not contract_errors else "blocked",
                             "Requirement and gate schema is valid" if not contract_errors else
                             "Requirement and gate schema is invalid",
                             evidence=(_relative(ledger_path, root), _relative(gates_path, root)),
                             details=contract_errors))
    except (OSError, ValueError, TypeError) as error:
        checks.append(_check("requirements_contract", "missing" if isinstance(error, OSError) else "blocked",
                             "Requirement and gate schema could not be loaded",
                             details=(str(error),)))

    implementation, counts = _implementation_check(ledger or {})
    checks.append(implementation)
    if ledger is not None and gates is not None:
        gaps = release_gaps(ledger, gates, {}, root)
        checks.append(_check("production_gate", "pass" if not gaps else "blocked",
                             "All requirements have current passing evidence" if not gaps else
                             f"Release gate is blocked by {len(gaps)} unresolved items",
                             evidence=(_relative(ledger_path, root), _relative(gates_path, root)),
                             details=gaps))
    else:
        checks.append(_check("production_gate", "missing", "Release gate could not be evaluated"))

    checks.append(_path_check(root, "documentation", "Required project documentation", REQUIRED_DOCUMENTS))
    checks.append(_configuration_check(root))
    checks.append(_offline_static_check(root))
    checks.append(_source_provenance_check(root))

    log_results = [test_log_status(path) for path in _latest_test_logs(root)]
    log_passes = sum(result["status"] == "pass" for result in log_results)
    log_blockers = [detail for result in log_results for detail in result["details"]
                    if result["status"] in {"blocked", "missing"}]
    log_evidence = [_relative(path, root) for path in _latest_test_logs(root) if path.is_file()]
    if log_passes == len(log_results) and log_results:
        checks.append(_check("test_matrix", "pass", "Debug and Release CTest logs record passing runs",
                             evidence=log_evidence))
    elif log_passes:
        checks.append(_check("test_matrix", "partial", "Only some CTest configurations record passing runs",
                             evidence=log_evidence, details=log_blockers))
    else:
        checks.append(_check("test_matrix", "missing", "No complete Debug and Release CTest evidence is available",
                             evidence=log_evidence, details=log_blockers))

    runtime_path = _latest_runtime_report(root)
    if runtime_path is None:
        checks.append(_check("runtime_smoke", "missing", "No installed-runtime report is available"))
    else:
        try:
            runtime = runtime_status(_load_json(runtime_path))
            runtime["evidence"] = [_relative(runtime_path, root)]
            checks.append(runtime)
        except (OSError, ValueError) as error:
            checks.append(_check("runtime_smoke", "blocked", "Installed-runtime report is invalid",
                                 evidence=(_relative(runtime_path, root),), details=(str(error),)))

    checks.append(_packaging_check(root))
    checks.append(_compatibility_check(ledger or {}))
    checks.append(_qualification_check(root))

    summary = dict(collections.Counter(check["status"] for check in checks))
    production_checks = {"production_gate", "runtime_smoke", "packaging", "offline_static", "source_provenance",
                         "compatibility", "qualification_manifest"}
    production_ready = all(next(check for check in checks if check["id"] == identifier)["status"] == "pass"
                           for identifier in production_checks)
    return {
        "schema_version": "1.0",
        "source_tree_sha256": source_fingerprint(root),
        "checks": sorted(checks, key=lambda check: check["id"]),
        "summary": {key: summary.get(key, 0) for key in ("pass", "partial", "blocked", "missing")},
        "implementation_counts": dict(sorted(counts.items(), key=lambda item: str(item[0]))),
        "production_ready": production_ready,
    }


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", type=pathlib.Path, default=ROOT)
    parser.add_argument("--output", type=pathlib.Path)
    parser.add_argument("--strict", action="store_true",
                        help="return 2 when production-ready evidence is not complete")
    args = parser.parse_args(argv)
    try:
        report = build_report(args.root)
    except (OSError, ValueError, RuntimeError) as error:
        print(json.dumps({"schema_version": "1.0", "production_ready": False,
                          "errors": [str(error)]}, sort_keys=True))
        return 2
    payload = json.dumps(report, indent=2, sort_keys=True)
    if args.output:
        args.output.write_text(payload + "\n", encoding="utf-8")
    else:
        print(payload)
    return 0 if report["production_ready"] or not args.strict else 2


if __name__ == "__main__":
    raise SystemExit(main())
