"""Rebuild and run workflow fixtures, binding their logs to unchanged source.

Existing logs cannot be attested retrospectively. Run this helper after source
edits are finished, then generate workflow acceptance evidence.
"""

from __future__ import annotations

import argparse
import hashlib
import importlib.util
import json
import pathlib
import re
import subprocess


SCRIPT_DIR = pathlib.Path(__file__).resolve().parent
PROVENANCE_FILE = "workflow-test-provenance.json"
PROVENANCE_LOG_FILE = "workflow-test-provenance.log"
CONFIGURATIONS = {"windows-debug": "Debug", "windows-release": "Release"}


def _load(name):
    spec = importlib.util.spec_from_file_location(name, SCRIPT_DIR / f"{name}.py")
    if spec is None or spec.loader is None:
        raise RuntimeError(f"could not load provenance dependency: {name}")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def _run(command, root):
    subprocess.run(command, cwd=root, check=True,
                   creationflags=getattr(subprocess, "CREATE_NO_WINDOW", 0))


def source_fingerprint(root):
    return _load("requirement_audit").source_fingerprint(root)


def sha256(path):
    with path.open("rb") as stream:
        return hashlib.file_digest(stream, "sha256").hexdigest()


def _build(root, configuration):
    if configuration not in CONFIGURATIONS:
        raise RuntimeError(f"unsupported provenance configuration: {configuration}")
    build = (root / "build" / configuration).resolve()
    if not build.is_relative_to(root):
        raise RuntimeError("provenance build directory escapes the source tree")
    return build


def _artifacts(root, configuration):
    """Bind configuration, generated test definitions, and compiled payloads."""
    build = _build(root, configuration)
    cache = build / "CMakeCache.txt"
    inventory = build / "CTestTestfile.cmake"
    if not cache.is_file() or not inventory.is_file():
        raise RuntimeError(f"{configuration}: provenance requires a configured CMake build")
    cache_text = cache.read_text(encoding="utf-8")
    source = re.search(r"(?m)^CMAKE_HOME_DIRECTORY:INTERNAL=(.+)$", cache_text)
    kind = re.search(r"(?m)^CMAKE_BUILD_TYPE:STRING=(.+)$", cache_text)
    if (source is None or pathlib.Path(source.group(1).strip()).resolve() != root or
            kind is None or kind.group(1).strip() != CONFIGURATIONS[configuration]):
        raise RuntimeError(f"{configuration}: provenance CMake source/configuration mismatch")
    paths = {cache, inventory}
    for path in build.rglob("*"):
        if path.is_file() and (path.name == "CTestTestfile.cmake" or
                               path.suffix.lower() in {".exe", ".dll", ".lib", ".a", ".so", ".dylib"}):
            paths.add(path)
    records = {}
    for path in sorted(paths):
        if not path.resolve().is_relative_to(build):
            raise RuntimeError(f"{configuration}: provenance artifact escapes build directory: {path}")
        records[path.relative_to(root).as_posix()] = sha256(path)
    return records


def verify(root, configuration, expected_source=None):
    root = root.resolve()
    build = _build(root, configuration)
    path = build / PROVENANCE_FILE
    try:
        record = json.loads(path.read_text(encoding="utf-8"))
        if not isinstance(record, dict):
            raise ValueError("expected a JSON object")
        if (record.get("schema_version") != 1 or
                record.get("configuration") != configuration or
                record.get("build_method") != "cmake-clean-first" or
                record.get("source_tree_sha256") != (expected_source or source_fingerprint(root))):
            raise ValueError("source or build identity does not match")
        if record.get("artifacts") != _artifacts(root, configuration):
            raise ValueError("build artifacts do not match")
        log = build / PROVENANCE_LOG_FILE
        if record.get("log_sha256") != sha256(log):
            raise ValueError("CTest log does not match")
    except (OSError, ValueError, TypeError) as error:
        raise RuntimeError(f"{configuration}: invalid workflow test provenance: {error}; "
                           "rerun scripts/workflow_test_provenance.py") from error
    return record


def run(root, configuration, test_names, *, cmake="cmake", ctest="ctest"):
    root = root.resolve()
    build = _build(root, configuration)
    receipt = build / PROVENANCE_FILE
    receipt.unlink(missing_ok=True)
    provenance_log = build / PROVENANCE_LOG_FILE
    provenance_log.unlink(missing_ok=True)
    if not test_names:
        raise RuntimeError("provenance requires at least one workflow test")
    # Require an existing, correctly configured build before any clean operation.
    _artifacts(root, configuration)
    source = source_fingerprint(root)
    _run([str(cmake), "-S", str(root), "-B", str(build)], root)
    _run([str(cmake), "--build", str(build), "--clean-first"], root)
    if source_fingerprint(root) != source:
        raise RuntimeError("provenance source changed during configure/build")
    artifacts = _artifacts(root, configuration)
    log = build / "Testing/Temporary/LastTest.log"
    log.unlink(missing_ok=True)
    _run([str(ctest), "--test-dir", str(build), "--output-on-failure",
          "--no-tests=error"], root)
    if source_fingerprint(root) != source or _artifacts(root, configuration) != artifacts:
        raise RuntimeError("provenance source/build changed during testing")
    # Check each requested fixture actually ran; CTest can succeed with disabled tests.
    generator = _load("generate_workflow_acceptance_evidence")
    audit = _load("completion_audit")
    generator._workflow_test_records(root, configuration, tuple(test_names), audit)
    provenance_log.write_bytes(log.read_bytes())
    record = {"schema_version": 1, "configuration": configuration,
              "build_method": "cmake-clean-first", "source_tree_sha256": source,
              "artifacts": artifacts, "log_sha256": sha256(provenance_log)}
    receipt.write_text(json.dumps(record, indent=2, sort_keys=True) + "\n", encoding="utf-8")
    return record


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", type=pathlib.Path, default=SCRIPT_DIR.parent)
    parser.add_argument("--configuration", choices=CONFIGURATIONS, action="append")
    parser.add_argument("--cmake", default="cmake")
    parser.add_argument("--ctest", default="ctest")
    args = parser.parse_args()
    generator = _load("generate_workflow_acceptance_evidence")
    names = sorted({name for rule in generator.WORKFLOW_RULES.values() for name in rule["tests"]})
    for configuration in args.configuration or CONFIGURATIONS:
        run(args.root, configuration, names, cmake=args.cmake, ctest=args.ctest)
        print(f"Recorded source/build/test provenance for {configuration}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
