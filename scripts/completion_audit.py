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

# Independent analytical fixtures required by OPS-QA-001.  These are kept as
# named CTest families rather than inferred from a total test count so a
# focused run cannot accidentally look like full coverage.  The source
# anchors are deliberately broad, stable behavior names; this check proves
# fixture presence and execution, while semantic and visual qualification
# remains a separate production gate.
QA_FIXTURE_RULES = (
    {
        "id": "geometry",
        "tests": ("geometry", "geometry_operations"),
        "sources": ("tests/geometry_tests.cpp",),
        "anchors": ("signed_area", "overlap", "tangent"),
    },
    {
        "id": "curves",
        "tests": ("curve_construction",),
        "sources": ("tests/curve_construction_tests.cpp",),
        "anchors": ("arc_from_chord_arc_length", "arc_from_start_tangent", "tangent"),
    },
    {
        "id": "calculations_and_units",
        "tests": ("calculations", "quantity"),
        "sources": ("tests/calculation_tests.cpp", "tests/quantity_tests.cpp"),
        "anchors": ("calculate_areas", "parse_quantity", "round", "overlap", "winding"),
    },
    {
        "id": "topology",
        "tests": ("architecture", "room_relationship_geometry", "slab_semantics"),
        "sources": ("tests/architecture_tests.cpp",),
        "anchors": ("holes", "overlap", "volume"),
    },
    {
        "id": "constraints",
        "tests": ("constraints", "constraint_authoring", "constraint_integrity"),
        "sources": ("tests/constraints_tests.cpp",),
        "anchors": ("degrees_of_freedom", "redundan", "conflict", "winding"),
    },
    {
        "id": "persistence",
        "tests": ("project_storage", "project_exchange", "project_archive"),
        "sources": ("tests/project_store_tests.cpp",),
        "anchors": ("ProjectStore", "reopen", "recovery"),
    },
)


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


def _ctest_inventory(build_dir: pathlib.Path):
    """Read the generated CTest inventory and execution properties."""

    path = build_dir / "CTestTestfile.cmake"
    if not path.is_file():
        return None, (f"CTest inventory is missing: {path}",)
    try:
        text = path.read_text(encoding="utf-8", errors="replace")
    except OSError as error:
        return None, (f"CTest inventory could not be read: {error}",)
    source_match = re.search(r"(?m)^# Source directory:\s*(.+?)\s*$", text)
    build_match = re.search(r"(?m)^# Build directory:\s*(.+?)\s*$", text)
    tests = {}
    for match in re.finditer(r"(?m)^add_test\(\[=\[(.+?)\]=\]", text):
        tests[match.group(1)] = {"disabled": False, "skip_return_code": False}
    for match in re.finditer(
        r"set_tests_properties\(\[=\[(.+?)\]=\]\s+PROPERTIES\s+(.*?)\)\s*$",
        text, re.MULTILINE | re.DOTALL,
    ):
        name, properties = match.group(1), match.group(2)
        if name not in tests:
            continue
        tests[name]["disabled"] = bool(re.search(r"\bDISABLED\s+\"?TRUE\"?", properties,
                                                   re.IGNORECASE))
        tests[name]["skip_return_code"] = bool(re.search(r"\bSKIP_RETURN_CODE\b", properties,
                                                          re.IGNORECASE))
    if not tests:
        return None, (f"CTest inventory contains no tests: {path}",)
    return {
        "path": path,
        "source_directory": pathlib.Path(source_match.group(1).strip()) if source_match else None,
        "build_directory": pathlib.Path(build_match.group(1).strip()) if build_match else None,
        "tests": tests,
    }, ()


def _test_log_blocks(text: str):
    starts = list(re.finditer(r"(?m)^\d+/\d+\s+Testing:\s*(.+?)\s*$", text))
    records = []
    for index, start in enumerate(starts):
        stop = starts[index + 1].start() if index + 1 < len(starts) else len(text)
        block = text[start.start():stop]
        name = start.group(1).strip()
        if re.search(r"(?m)^Test Failed\.|\*\*\*Failed", block):
            status = "failed"
        elif re.search(r"(?m)^Test Skipped\.", block):
            status = "skipped"
        elif re.search(r"(?m)^Test Passed\.", block):
            status = "passed"
        else:
            status = "incomplete"
        command = re.search(r"(?m)^Command:\s*(.*?)\s*$", block)
        directory = re.search(r"(?m)^Directory:\s*(.*?)\s*$", block)
        records.append({
            "name": name,
            "status": status,
            "command": command.group(1).strip() if command else "",
            "directory": directory.group(1).strip() if directory else "",
            "text": block,
        })
    return records


def test_log_status(path: pathlib.Path, source_root: pathlib.Path | None = None):
    """Classify a CTest log against its generated inventory.

    Marker-only logs are insufficient: a focused run, a disabled production
    test, or a SKIP_RETURN_CODE fixture must remain visible as partial evidence.
    """

    if not path.is_file():
        return _check("test_matrix", "missing", "CTest log is missing", details=(str(path),))
    try:
        text = path.read_text(encoding="utf-8", errors="replace")
    except OSError as error:
        return _check("test_matrix", "blocked", "CTest log could not be read", details=(str(error),))
    build_dir = path
    for candidate in (path.parent, *path.parents):
        if (candidate / "CTestTestfile.cmake").is_file():
            build_dir = candidate
            break
    inventory, inventory_errors = _ctest_inventory(build_dir)
    if inventory is None:
        return _check("test_matrix", "partial", "CTest inventory is unavailable",
                      evidence=(str(path),), details=inventory_errors)
    if "Test Failed." in text or re.search(r"\*\*\*Failed", text):
        return _check("test_matrix", "blocked", "CTest log contains a failed test",
                      evidence=(str(path),))
    if "End testing:" not in text:
        return _check("test_matrix", "partial", "CTest log is incomplete", evidence=(str(path),))
    if source_root is not None and inventory["source_directory"] is not None:
        try:
            expected_source = source_root.resolve()
            recorded_source = inventory["source_directory"].resolve()
        except OSError:
            expected_source = source_root
            recorded_source = inventory["source_directory"]
        if expected_source != recorded_source:
            return _check("test_matrix", "blocked", "CTest log belongs to a different source tree",
                          evidence=(str(path),), details=(f"recorded source: {recorded_source}",
                                                          f"expected source: {expected_source}"))
    blocks = _test_log_blocks(text)
    if not blocks:
        return _check("test_matrix", "partial", "CTest log contains no structured test records",
                      evidence=(str(path),))
    expected = inventory["tests"]
    seen = collections.Counter(record["name"] for record in blocks)
    failures = [record["name"] for record in blocks if record["status"] == "failed"]
    incomplete = [record["name"] for record in blocks if record["status"] == "incomplete"]
    unexpected = sorted(name for name in seen if name not in expected)
    missing = sorted(name for name, props in expected.items()
                     if not props["disabled"] and seen.get(name, 0) == 0)
    duplicates = sorted(name for name, count in seen.items() if count > 1)
    disabled = sorted(name for name, props in expected.items()
                      if props["disabled"] and seen.get(name, 0))
    skipped = sorted(record["name"] for record in blocks
                     if record["status"] == "skipped" or
                     (expected.get(record["name"], {}).get("skip_return_code") and
                      re.search(r"\bskip(?:ped)?\b", record["text"], re.IGNORECASE)))
    bad_directories = []
    expected_build = inventory["build_directory"] or build_dir
    try:
        expected_build_text = str(expected_build.resolve())
    except OSError:
        expected_build_text = str(expected_build)
    for record in blocks:
        if record["directory"]:
            try:
                actual_directory = str(pathlib.Path(record["directory"]).resolve())
            except OSError:
                actual_directory = record["directory"]
            if actual_directory.casefold() != expected_build_text.casefold():
                bad_directories.append(f"{record['name']}: {record['directory']}")
    if failures:
        return _check("test_matrix", "blocked", "CTest log contains failed tests",
                      evidence=(str(path),), details=failures)
    if incomplete or unexpected or duplicates or bad_directories:
        details = ([f"incomplete: {name}" for name in incomplete] +
                   [f"unexpected: {name}" for name in unexpected] +
                   [f"duplicate: {name}" for name in duplicates] +
                   [f"wrong execution directory: {item}" for item in bad_directories])
        return _check("test_matrix", "blocked", "CTest log structure is invalid",
                      evidence=(str(path),), details=details)
    if missing or skipped or disabled:
        details = ([f"missing from run: {name}" for name in missing] +
                   [f"skipped: {name}" for name in skipped] +
                   [f"disabled: {name}" for name in disabled])
        return _check("test_matrix", "partial", "CTest run is incomplete or contains non-running tests",
                      evidence=(str(path),), details=details)
    passed = sum(record["status"] == "passed" for record in blocks)
    return _check("test_matrix", "pass", f"CTest log records {passed} passing test entries",
                  evidence=(str(path),))


def _qa_fixture_config(root: pathlib.Path, configuration: str):
    """Inspect one configuration for the named OPS-QA-001 fixture families."""

    build = root / "build" / configuration
    inventory_path = build / "CTestTestfile.cmake"
    log_path = build / "Testing/Temporary/LastTest.log"
    evidence = (_relative(inventory_path, root), _relative(log_path, root))
    inventory, errors = _ctest_inventory(build)
    if inventory is None:
        return "missing", evidence, [f"{configuration}: {detail}" for detail in errors]
    if not log_path.is_file():
        return "missing", evidence, [f"{configuration}: CTest log is missing"]
    try:
        log_text = log_path.read_text(encoding="utf-8", errors="replace")
    except OSError as error:
        return "blocked", evidence, [f"{configuration}: CTest log could not be read: {error}"]
    if "End testing:" not in log_text:
        return "partial", evidence, [f"{configuration}: CTest log is incomplete"]

    blocks = _test_log_blocks(log_text)
    by_name = collections.defaultdict(list)
    for block in blocks:
        by_name[block["name"]].append(block)
    details = []
    severity = "pass"

    def record(level, detail):
        nonlocal severity
        details.append(f"{configuration}: {detail}")
        if level == "blocked" or (level == "partial" and severity == "pass"):
            severity = level
        elif level == "blocked":
            severity = "blocked"

    for rule in QA_FIXTURE_RULES:
        source_text = []
        for source in rule["sources"]:
            path = root / source
            if not path.is_file() or path.stat().st_size == 0:
                record("partial", f"{rule['id']}: source is missing or empty: {source}")
                continue
            evidence = evidence + (_relative(path, root),)
            try:
                source_text.append(path.read_text(encoding="utf-8", errors="replace"))
            except OSError as error:
                record("blocked", f"{rule['id']}: source could not be read: {source}: {error}")
        combined_source = "\n".join(source_text).casefold()
        for anchor in rule["anchors"]:
            if anchor.casefold() not in combined_source:
                record("partial", f"{rule['id']}: source anchor is missing: {anchor}")
        for test_name in rule["tests"]:
            properties = inventory["tests"].get(test_name)
            if properties is None:
                record("partial", f"{rule['id']}: fixture is absent from CTest inventory: {test_name}")
                continue
            if properties["disabled"]:
                record("partial", f"{rule['id']}: fixture is disabled: {test_name}")
            if properties["skip_return_code"]:
                record("partial", f"{rule['id']}: fixture allows skip return code: {test_name}")
            records = by_name.get(test_name, [])
            if not records:
                record("partial", f"{rule['id']}: fixture was not executed: {test_name}")
                continue
            if len(records) != 1:
                record("blocked", f"{rule['id']}: fixture has {len(records)} log entries: {test_name}")
                continue
            status = records[0]["status"]
            if status == "failed":
                record("blocked", f"{rule['id']}: fixture failed: {test_name}")
            elif status == "incomplete":
                record("blocked", f"{rule['id']}: fixture result is incomplete: {test_name}")
            elif status == "skipped":
                record("partial", f"{rule['id']}: fixture was skipped: {test_name}")
    return severity, evidence, details


def qa_fixture_coverage_check(root: pathlib.Path | str = ROOT):
    """Prove the required independent analytical fixture families ran twice.

    This is intentionally narrower than production qualification: it checks
    named fixture sources and passing Debug/Release CTest records, but does
    not claim Apex fidelity, clean-machine behavior, or physical output.
    """

    root = pathlib.Path(root).resolve()
    statuses, evidence, details = [], [], []
    for configuration in ("windows-debug", "windows-release"):
        status, config_evidence, config_details = _qa_fixture_config(root, configuration)
        statuses.append(status)
        evidence.extend(config_evidence)
        details.extend(config_details)
    if "blocked" in statuses:
        status = "blocked"
        summary = "Independent analytical fixture coverage contains a failing or invalid run"
    elif "partial" in statuses:
        status = "partial"
        summary = "Independent analytical fixture coverage is incomplete"
    elif "missing" in statuses:
        status = "missing"
        summary = "Independent analytical fixture coverage is missing"
    else:
        status = "pass"
        summary = "Independent analytical fixtures passed in geometry, curves, calculations_and_units, topology, constraints, and persistence"
    return _check("qa_fixture_coverage", status, summary,
                  evidence=sorted(set(evidence)), details=details)


def _latest_runtime_report(root: pathlib.Path):
    installed_root = root / "artifacts/installed-runtime"
    # Keep the historical `current/run-*` layout, but also discover reports
    # written directly by a task-owned install/smoke invocation.  Selecting by
    # filesystem mtime prevents a newer report from being hidden merely because
    # its directory name uses a different staging convention.
    candidates = list((installed_root / "current").glob("run-*/report.json"))
    candidates.extend(installed_root.glob("run-*/report.json"))
    if not candidates:
        return None

    def sort_key(path: pathlib.Path):
        try:
            modified = path.stat().st_mtime_ns
        except OSError:
            modified = -1
        return modified, path.as_posix()

    return max(candidates, key=sort_key)


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

    log_results = [test_log_status(path, root) for path in _latest_test_logs(root)]
    log_passes = sum(result["status"] == "pass" for result in log_results)
    log_blockers = [detail for result in log_results for detail in result["details"]
                    if result["status"] != "pass"]
    log_evidence = [_relative(path, root) for path in _latest_test_logs(root) if path.is_file()]
    log_statuses = {result["status"] for result in log_results}
    if log_passes == len(log_results) and log_results:
        checks.append(_check("test_matrix", "pass", "Debug and Release CTest logs record passing runs",
                             evidence=log_evidence))
    elif "blocked" in log_statuses:
        checks.append(_check("test_matrix", "blocked", "CTest logs contain a blocked configuration",
                             evidence=log_evidence, details=log_blockers))
    elif "pass" in log_statuses or "partial" in log_statuses:
        checks.append(_check("test_matrix", "partial", "CTest evidence includes non-running or incomplete tests",
                             evidence=log_evidence, details=log_blockers))
    else:
        checks.append(_check("test_matrix", "missing", "No complete Debug and Release CTest evidence is available",
                             evidence=log_evidence, details=log_blockers))

    checks.append(qa_fixture_coverage_check(root))

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
