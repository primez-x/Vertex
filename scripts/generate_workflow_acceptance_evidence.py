"""Write hash-bound acceptance evidence for implemented architecture workflows."""

from __future__ import annotations

import argparse
import hashlib
import importlib.util
import json
import pathlib
from typing import Any


SCRIPT_DIR = pathlib.Path(__file__).resolve().parent


WORKFLOW_RULES: dict[str, dict[str, Any]] = {
    "ARCH-MOD-009": {
        "acceptance": (
            "A room can relate to, follow, or remain independent from a measurement "
            "boundary without silently conflating classifications or geometry."
        ),
        "sources": (
            "include/sketch/room_relationships.hpp",
            "src/core/room_relationships.cpp",
            "include/sketch/room_relationship_geometry.hpp",
            "src/core/room_relationship_geometry.cpp",
            "include/sketch/room_relationship_geometry_commit.hpp",
            "src/core/room_relationship_geometry_commit.cpp",
            "tests/room_relationships_tests.cpp",
            "tests/room_relationship_geometry_tests.cpp",
            "tests/room_relationship_geometry_commit_tests.cpp",
            "docs/room-relationships.md",
            "src/desktop/main_window.cpp",
            "tests/desktop_smoke.cpp",
            "docs/desktop-workflow.md",
        ),
        "anchors": (
            "RoomRelationshipSnapshot",
            "room_boundary",
            "appraisal_measurement_boundary",
            "retarget",
            "propagation",
            "independent",
            "undo",
        ),
        "tests": (
            "room_relationships",
            "room_relationship_geometry",
            "room_relationship_geometry_commit",
            "desktop_workflow",
        ),
        "qualification_boundary": (
            "This evidence covers deterministic relationship semantics, controlled retargeting, "
            "geometry propagation, and the Windows workflow on the development host. It does "
            "not certify imported Apex files, physical devices, printing, or the unified release gate."
        ),
    },
    "ARCH-MOD-011": {
        "acceptance": (
            "Alternative fixtures switch, compare, save, and print without contaminating one "
            "another or changing the shared existing state."
        ),
        "sources": (
            "include/sketch/model_phases.hpp",
            "src/core/model_phases.cpp",
            "src/desktop/main_window.cpp",
            "tests/model_phases_tests.cpp",
            "tests/desktop_smoke.cpp",
            "docs/phases-alternatives.md",
        ),
        "anchors": (
            "ModelPhases",
            "with_active",
            "compare",
            "remodelingComparisonList",
            "exportDraftPdf",
            "save and reopen",
        ),
        "tests": (
            "model_phases",
            "desktop_workflow",
        ),
        "qualification_boundary": (
            "This evidence covers the shared model phase value, comparison controls, coordinated "
            "plan/elevation/section output, undo/redo, and save/reopen on the development host. "
            "It does not certify imported Apex files, physical printers, or the unified release gate."
        ),
    },
    "CORE-DOC-009": {
        "acceptance": (
            "The CLI validates a good fixture, rejects malformed or incomplete data with "
            "actionable diagnostics, extracts assets, and produces a migration report."
        ),
        "sources": (
            "src/cli/main.cpp",
            "tests/test_cli.py",
            "docs/project-format.md",
        ),
        "anchors": (
            "inspect",
            "validate",
            "extract",
            "migrate",
            "source_preserved",
            "malformed",
        ),
        "tests": ("project_cli",),
        "qualification_boundary": (
            "This evidence covers the local inspect, validate, extract, and migrate CLI fixture "
            "including malformed input and source preservation. It does not certify Apex/native "
            "migration fidelity or a production release."
        ),
    },
    "APX-KEY-004": {
        "acceptance": (
            "Each point-jump, automatic-closure, and bay-window operation is observable in "
            "command history, produces the expected topology, and is undoable."
        ),
        "sources": (
            "include/sketch/geometry_operations.hpp",
            "src/core/geometry_operations.cpp",
            "tests/geometry_operations_tests.cpp",
            "docs/geometry-operations.md",
            "include/sketch/desktop/main_window.hpp",
            "src/desktop/main_window.cpp",
            "tests/desktop_smoke.cpp",
            "docs/desktop-workflow.md",
        ),
        "anchors": (
            "jump_to_boundary_vertex",
            "automatically_close_boundary",
            "complete_bay_window",
            "Auto close boundary",
            "Complete bay window",
            "undo",
            "redo",
        ),
        "tests": ("geometry_operations", "desktop_workflow"),
        "qualification_boundary": (
            "This evidence covers the deterministic geometry operations and Windows workflow "
            "history/undo behavior. Physical Apex key mapping, native Apex fixtures, and the "
            "unified production gate remain open."
        ),
    },
    "REC-004": {
        "acceptance": (
            "Concurrent-open and external-change fixtures prevent silent overwrite and make the "
            "selected read-only or independent-copy path explicit."
        ),
        "sources": (
            "include/sketch/project_store.hpp",
            "src/core/project_store.cpp",
            "include/sketch/project_ownership.hpp",
            "src/core/project_ownership.cpp",
            "tests/project_ownership_tests.cpp",
            "tests/project_ownership_process_tests.cpp",
            "tests/desktop_smoke.cpp",
            "docs/project-format.md",
        ),
        "anchors": (
            "ProjectOwnershipSession",
            "external_change",
            "read-only",
            "case-insensitive",
            "second_open_is_read_only",
            "saveProject",
        ),
        "tests": ("project_ownership", "project_ownership_process", "desktop_workflow"),
        "qualification_boundary": (
            "This evidence covers local and cross-process ownership, Windows path identity, "
            "external-change save blocking, and the desktop read-only second-open behavior. "
            "Clean-machine lifetime and hostile-writer qualification remain open."
        ),
    },
}


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


def _workflow_test_records(root: pathlib.Path, configuration: str, test_names: tuple[str, ...], audit):
    build = root / "build" / configuration
    inventory_path = build / "CTestTestfile.cmake"
    log_path = build / "Testing" / "Temporary" / "LastTest.log"
    inventory, inventory_errors = audit._ctest_inventory(build)
    if inventory is None:
        raise RuntimeError(f"{configuration}: invalid CTest inventory: {inventory_errors}")
    if not log_path.is_file():
        raise RuntimeError(f"{configuration}: CTest log is missing")
    try:
        log_text = log_path.read_text(encoding="utf-8", errors="replace")
    except OSError as error:
        raise RuntimeError(f"{configuration}: CTest log could not be read: {error}") from error
    if "End testing:" not in log_text:
        raise RuntimeError(f"{configuration}: CTest log is incomplete")
    blocks = audit._test_log_blocks(log_text)
    by_name: dict[str, list[dict[str, Any]]] = {}
    for block in blocks:
        by_name.setdefault(block["name"], []).append(block)

    records = []
    for test_name in test_names:
        properties = inventory["tests"].get(test_name)
        if properties is None:
            raise RuntimeError(f"{configuration}: workflow test is absent from CTest inventory: {test_name}")
        if properties["disabled"] or properties["skip_return_code"]:
            raise RuntimeError(f"{configuration}: workflow test is disabled or skippable: {test_name}")
        entries = by_name.get(test_name, [])
        if len(entries) != 1 or entries[0]["status"] != "passed":
            status = entries[0]["status"] if len(entries) == 1 else "missing_or_duplicate"
            raise RuntimeError(f"{configuration}: workflow test did not pass exactly once: {test_name} ({status})")
        records.append({"name": test_name, "status": "passed"})

    return {
        "configuration": configuration,
        "inventory": _path_record(root, f"build/{configuration}/CTestTestfile.cmake"),
        "log": _path_record(root, f"build/{configuration}/Testing/Temporary/LastTest.log"),
        "tests": records,
    }


def build_evidence(root: pathlib.Path) -> dict[str, dict[str, Any]]:
    root = root.resolve()
    audit = _load_module("vertex_completion_audit_for_workflow", SCRIPT_DIR / "completion_audit.py")
    requirement_audit = _load_module("vertex_requirement_audit_for_workflow", SCRIPT_DIR / "requirement_audit.py")
    result: dict[str, dict[str, Any]] = {}

    for requirement_id, rule in WORKFLOW_RULES.items():
        source_records = []
        source_text = []
        for relative in rule["sources"]:
            record = _path_record(root, relative)
            source_records.append(record)
            source_text.append((root / relative).read_text(encoding="utf-8", errors="replace"))
        combined_source = "\n".join(source_text).casefold()
        missing_anchors = [anchor for anchor in rule["anchors"] if anchor.casefold() not in combined_source]
        if missing_anchors:
            raise RuntimeError(f"{requirement_id}: source anchors are missing: {missing_anchors}")

        ctest = [
            _workflow_test_records(root, configuration, rule["tests"], audit)
            for configuration in ("windows-debug", "windows-release")
        ]
        result[requirement_id] = {
            "schema_version": 1,
            "requirement_id": requirement_id,
            "result": "pass",
            "source_tree_sha256": requirement_audit.source_fingerprint(root),
            "acceptance": rule["acceptance"],
            "source_files": source_records,
            "source_anchors": list(rule["anchors"]),
            "tests": list(rule["tests"]),
            "ctest": ctest,
            "qualification_boundary": rule["qualification_boundary"],
        }
    return result


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", type=pathlib.Path,
                        default=pathlib.Path(__file__).resolve().parents[1])
    parser.add_argument("--output-directory", type=pathlib.Path, default=None)
    args = parser.parse_args()
    root = args.root.resolve()
    output_directory = (args.output_directory or root / "artifacts/acceptance/workflows").resolve()
    if not output_directory.is_relative_to(root):
        raise SystemExit("output directory must remain inside the repository root")
    output_directory.mkdir(parents=True, exist_ok=True)
    evidence = build_evidence(root)
    for requirement_id, record in evidence.items():
        output = output_directory / f"{requirement_id.lower()}.json"
        output.write_text(json.dumps(record, indent=2, sort_keys=True) + "\n",
                          encoding="utf-8", newline="\n")
        print(f"Wrote {output} ({requirement_id}; source {record['source_tree_sha256']})")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
