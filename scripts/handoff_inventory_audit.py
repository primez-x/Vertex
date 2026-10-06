"""Audit OPS-PACK-002 handoff inventory and local manifest integrity only."""

from __future__ import annotations

import argparse
import importlib.util
import json
import pathlib
import sys
from typing import Any


# This command is designed to run from inside the immutable bundle it audits.
# Prevent helper imports from creating unlisted __pycache__ files before strict
# inventory verification begins.
sys.dont_write_bytecode = True


def _load_bundle_helper():
    spec = importlib.util.spec_from_file_location(
        "stage_offline_bundle_for_handoff", pathlib.Path(__file__).with_name("stage_offline_bundle.py"))
    if spec is None or spec.loader is None:
        raise RuntimeError("cannot load offline bundle verifier")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


_BUNDLE = _load_bundle_helper()
_SOURCE = _BUNDLE._SOURCE_KIT
CONTRACT = pathlib.Path(__file__).resolve().parents[1] / "packaging/handoff-inventory-contract.json"
ARTIFACT_IDS = {
    "offline_help", "keyboard_workflow_reference", "source_build_kit", "dependency_sources",
    "dependency_notices", "sbom", "project_format", "adapter_documentation", "fixtures",
    "verification_reports",
}
BOUNDARY = (
    "Local integrity and declared artifact presence only. This does not prove a clean-machine "
    "rebuild, legal clearance, production certification, dependency source completeness, "
    "report validity, offline operation, or authenticity of the supplied manifests."
)


def _require(condition: bool, message: str) -> None:
    if not condition:
        raise ValueError(message)


def _contract() -> dict[str, Any]:
    contract = _BUNDLE._read_json(CONTRACT, "handoff inventory contract")
    _require(type(contract.get("schema_version")) is int and contract["schema_version"] == 1,
             "unsupported handoff contract schema_version")
    _require(contract.get("requirement_id") == "OPS-PACK-002", "wrong handoff requirement")
    _require(contract.get("source_kit_categories") == list(_SOURCE.CATEGORIES),
             "contract must require all source-kit categories")
    artifacts = contract.get("artifacts")
    _require(isinstance(artifacts, list) and len(artifacts) == len(ARTIFACT_IDS),
             "contract must enumerate every required artifact class")
    ids = []
    for rule in artifacts:
        _require(isinstance(rule, dict), "contract artifact must be an object")
        _require(set(rule) <= {"id", "path", "prefix", "kind", "category"}
                 and len(rule) > 1, "contract artifact needs supported selectors")
        _require(all(isinstance(value, str) and value.strip() for value in rule.values()),
                 "contract selectors must be nonempty strings")
        ids.append(rule["id"])
        if "category" in rule:
            _require(rule["category"] in _SOURCE.CATEGORIES, "unknown contract category")
        for key in ("path", "prefix"):
            if key in rule:
                text = rule[key].rstrip("/") if key == "prefix" else rule[key]
                _require(_BUNDLE.canonical_relative(text, key) == text,
                         "contract paths must be canonical")
        if "prefix" in rule:
            _require(rule["prefix"].endswith("/"), "contract prefix must end at directory boundary")
    _require(set(ids) == ARTIFACT_IDS, "contract artifact classes are missing or duplicated")
    return contract


def _matches(record: dict[str, Any], rule: dict[str, Any]) -> bool:
    return all(
        key == "id" or (record["path"].startswith(value) if key == "prefix"
                        else record.get(key) == value)
        for key, value in rule.items()
    )


def _dependency_members(root: pathlib.Path, manifest: dict[str, Any],
                        records: dict[str, Any]) -> set[str]:
    """Admit only the separately bound, frozen dependency attachment."""
    reference = manifest.get("dependency_source_kit")
    if reference is None:
        _require(not any(row.get("kind") == "dependency-source" for row in records.values()),
                 "dependency source files require a dependency source kit reference")
        return set()
    closure = _BUNDLE._load_sibling(
        "dependency_closure_for_handoff", "qualification/dependency_source_closure.py")
    inventory, inventory_receipt = closure.json_input(
        root, manifest["source_inventory"]["path"], manifest["source_inventory"]["sha256"])
    source_receipt = closure.file_record(root, manifest["source_kit"]["path"], manifest["source_kit"]["sha256"])
    kit = _BUNDLE._read_json(
        _BUNDLE._resolve_input_file(root, reference["path"], "dependency source kit"), "dependency source kit")
    _require(isinstance(kit, dict), "dependency source kit must be an object")
    binding = kit.get("candidate_binding")
    _require(isinstance(binding, dict) and isinstance(binding.get("inventory"), dict),
             "dependency source kit requires an inventory binding object")
    # Frozen closure reuses the stager's receipt replay with a resolver for
    # delivered project files; no original mutable workspace paths are opened.
    closure.dependency_bundle_sources(root.parent, root.name, manifest, records, inventory,
                                      inventory_receipt, source_receipt, {})
    members = {reference["path"], *(_BUNDLE.DEPENDENCY_PREFIX + "/" + row["path"] for row in kit["files"])}
    _require({path for path, row in records.items() if row.get("kind") == "dependency-source"} == members,
             "bundle and dependency source kit file sets differ")
    for path in members:
        row = records[path]
        _require(row.get("role") == "source-kit" and row.get("category") == "dependency-source"
                 and row.get("install") is False, f"invalid dependency source bundle role: {path}")

    def selected(receipt, reference, actual, label, *, original=False):
        _require(isinstance(receipt, dict) and receipt.get("sha256") == actual["sha256"]
                 and type(receipt.get("bytes")) is int and receipt["bytes"] == actual["bytes"],
                 f"dependency {label} binding differs from frozen candidate")
        if original:
            identity = reference.get("original_path")
            _require(isinstance(identity, str) and receipt.get("source_path") == identity
                     and receipt.get("payload_path") == "inputs/" + identity,
                     f"dependency {label} original identity differs from frozen candidate")

    selected(binding["inventory"], manifest["source_inventory"], inventory_receipt, "inventory", original=True)
    if binding.get("source_kit") is not None:
        selected(binding["source_kit"], manifest["source_kit"], source_receipt, "project kit", original=True)
    prior = binding.get("offline_bundle")
    if prior is not None:
        selected(prior.get("source_kit"), manifest["source_kit"], source_receipt, "project kit")
        selected(prior.get("source_inventory"), manifest["source_inventory"], inventory_receipt, "inventory")
    for key in ("component_manifest", "runtime"):
        evidence = inventory.get("evidence", {}).get(key)
        if evidence is not None:
            receipt = binding.get(key)
            _require(isinstance(receipt, dict) and receipt.get("source_path") == evidence["path"]
                     and receipt.get("sha256") == evidence["sha256"],
                     "dependency candidate evidence differs from frozen inventory")
    expected_binaries = []
    for row in inventory["binaries"]:
        member = records.get(row["destination"])
        _require(member is not None and member.get("kind") == "binary"
                 and member.get("component_id") == row["component_id"] and member["sha256"] == row["sha256"],
                 "dependency binary differs from frozen inventory")
        expected_binaries.append((row["component_id"], row["path"], row["sha256"], member["size"]))
    binaries = binding.get("binaries")
    _require(isinstance(binaries, list) and all(isinstance(row, dict) for row in binaries),
             "invalid dependency binary binding table")
    _require(sorted((row.get("component_id"), row.get("source_path"), row.get("sha256"), row.get("bytes"))
                    for row in binaries) == sorted(expected_binaries),
             "dependency binary binding differs from frozen inventory")
    return members


def audit_bundle(bundle_root: pathlib.Path | str,
                 manifest_name: str = "offline-bundle-manifest.json") -> dict[str, Any]:
    """Return a deterministic success/failure report; never confer qualification."""
    report: dict[str, Any] = {
        "schema_version": 1, "requirement_id": "OPS-PACK-002", "passed": False,
        "audit_status": "incomplete", "scope": "local-integrity-only",
        "qualification": {"clean_machine_rebuild": False, "legal_clearance": False,
                          "production_certification": False},
        "artifacts": [], "errors": [], "boundary": BOUNDARY,
    }
    try:
        contract = _contract()
        root = _BUNDLE._resolve_directory(bundle_root, "bundle root")
        result = _BUNDLE.verify_bundle(root, manifest_name, strict_files=True)
        _require(result["manifest_kind"] == "offline-bundle", "an offline-bundle manifest is required")
        manifest = _BUNDLE._read_json(
            _BUNDLE._resolve_input_file(root, manifest_name, "bundle manifest"), "bundle manifest")
        for key in ("source_kit", "runtime_manifest", "sbom", "source_inventory"):
            _require(isinstance(manifest.get(key), dict), f"bundle requires {key} reference")
        records = {row["path"]: row for row in manifest["files"]}
        for path in records:
            _require(_BUNDLE.canonical_relative(path, "bundle path") == path,
                     f"noncanonical bundle path: {path}")
            _SOURCE.verify_actual_filename(root, path)
        dependency_members = _dependency_members(root, manifest, records)
        source_reference = manifest["source_kit"]["path"]
        source = _BUNDLE._read_json(
            _BUNDLE._resolve_input_file(root, source_reference, "source-kit manifest"), "source-kit manifest")
        _SOURCE.validate_manifest(source)
        for category in contract["source_kit_categories"]:
            _require(source["category_counts"][category] > 0,
                     f"missing required source-kit category: {category}")
        expected_source_paths = set()
        for entry in source["files"]:
            relative = entry["path"]
            _require(_SOURCE.canonical_relative(relative, "source-kit path") == relative,
                     f"noncanonical source-kit path: {relative}")
            path = "source-kit/" + relative
            expected_source_paths.add(path)
            record = records.get(path)
            _require(record is not None, f"source-kit file has no bundle record: {path}")
            _require(record.get("kind") == "source-kit" and record.get("role") == "source-kit"
                     and record.get("install") is False, f"invalid source-kit bundle role: {path}")
            _require(record.get("category") == entry["category"], f"source-kit category mismatch: {path}")
            _require(record["size"] == entry["size"] and record["sha256"].lower() == entry["sha256"].lower(),
                     f"source-kit integrity mismatch: {path}")
        actual_source_paths = {path for path, row in records.items()
                               if (path.startswith("source-kit/") or row.get("kind") == "source-kit")
                               and path not in dependency_members}
        _require(actual_source_paths == expected_source_paths, "bundle and source-kit file sets differ")
        for rule in sorted(contract["artifacts"], key=lambda row: row["id"]):
            paths = sorted(path for path, row in records.items() if _matches(row, rule) and row["size"] > 0)
            report["artifacts"].append({"id": rule["id"], "present": bool(paths), "paths": paths})
            if not paths:
                report["errors"].append(f"missing required handoff artifact: {rule['id']}")
        report["file_count"] = result["file_count"]
        report["source_kit_category_counts"] = source["category_counts"]
        report["passed"] = not report["errors"]
    except (ValueError, OSError, RuntimeError, KeyError, TypeError) as exc:
        report["errors"].append(str(exc))
    return report


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--bundle-root", required=True, type=pathlib.Path)
    parser.add_argument("--manifest", default="offline-bundle-manifest.json")
    args = parser.parse_args(argv)
    report = audit_bundle(args.bundle_root, args.manifest)
    print(json.dumps(report, indent=2, sort_keys=True, ensure_ascii=False))
    return 0 if report["passed"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
