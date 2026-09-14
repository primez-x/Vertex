"""Write current, hash-bound evidence for the independent analytical fixtures."""

from __future__ import annotations

import argparse
import hashlib
import importlib.util
import json
import pathlib
import sys
from typing import Any


def _load_module(name: str, path: pathlib.Path):
    spec = importlib.util.spec_from_file_location(name, path)
    if spec is None or spec.loader is None:
        raise RuntimeError(f"could not load {path}")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def _sha256(path: pathlib.Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as source:
        for chunk in iter(lambda: source.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def _path_record(root: pathlib.Path, relative: str) -> dict[str, Any]:
    path = (root / relative).resolve()
    if not path.is_relative_to(root) or not path.is_file():
        raise RuntimeError(f"required evidence path is missing or outside the source tree: {relative}")
    return {
        "path": relative.replace("\\", "/"),
        "sha256": _sha256(path),
        "size": path.stat().st_size,
    }


def build_evidence(root: pathlib.Path) -> dict[str, Any]:
    root = root.resolve()
    audit = _load_module("vertex_completion_audit", root / "scripts/completion_audit.py")
    requirement_audit = _load_module("vertex_requirement_audit", root / "scripts/requirement_audit.py")
    check = audit.qa_fixture_coverage_check(root)
    if check["status"] != "pass":
        raise RuntimeError("OPS-QA-001 fixture coverage is not passing in both configurations")

    fixtures = []
    for rule in audit.QA_FIXTURE_RULES:
        fixtures.append({
            "id": rule["id"],
            "tests": list(rule["tests"]),
            "source_files": [_path_record(root, source) for source in rule["sources"]],
            "source_anchors": list(rule["anchors"]),
        })

    ctest_logs = []
    for configuration in ("windows-debug", "windows-release"):
        ctest_logs.append({
            "configuration": configuration,
            "inventory": _path_record(root, f"build/{configuration}/CTestTestfile.cmake"),
            "log": _path_record(root, f"build/{configuration}/Testing/Temporary/LastTest.log"),
        })

    source_hash = requirement_audit.source_fingerprint(root)
    return {
        "schema_version": 1,
        "requirement_id": "OPS-QA-001",
        "result": "pass",
        "source_tree_sha256": source_hash,
        "acceptance": (
            "The verification suite covers lines, arcs, holes, overlaps, winding, tangency, "
            "units, area tolerance max(1e-6 m2, 1e-8 times reference area), and constraint branches."
        ),
        "fixture_families": fixtures,
        "ctest": ctest_logs,
        "audit_check": check,
        "qualification_boundary": (
            "This evidence covers independent analytical fixtures only. It does not certify "
            "Apex fidelity, clean-machine behavior, physical devices or printing, integrations, "
            "or the unified production release gate."
        ),
    }


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", type=pathlib.Path,
                        default=pathlib.Path(__file__).resolve().parents[1])
    parser.add_argument("--output", type=pathlib.Path,
                        default=None)
    args = parser.parse_args()
    root = args.root.resolve()
    output = (args.output or root / "artifacts/acceptance/ops-qa-001.json").resolve()
    if not output.is_relative_to(root):
        raise SystemExit("output must remain inside the repository root")
    output.parent.mkdir(parents=True, exist_ok=True)
    evidence = build_evidence(root)
    output.write_text(json.dumps(evidence, indent=2, sort_keys=True) + "\n", encoding="utf-8")
    print(f"Wrote {output} (OPS-QA-001; source {evidence['source_tree_sha256']})")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
