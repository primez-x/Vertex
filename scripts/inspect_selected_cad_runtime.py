"""Inspect exact CAD runtime bytes without importing or executing its payload."""
from __future__ import annotations

import argparse
import importlib.util
import json
from pathlib import Path
import sys

_spec = importlib.util.spec_from_file_location(
    "_selected_cad_receipts", Path(__file__).parent / "qualification/compose_controlled_cad_runtime.py")
receipts = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(receipts)
EXTENSION = "Lib/site-packages/ifcopenshell/" + receipts.EXTENSION_NAME
WRAPPER = "Lib/site-packages/ifcopenshell/ifcopenshell_wrapper.py"


def require(condition, message):
    if not condition:
        raise ValueError(message)


def controlled(root, value, table, lock, lock_receipt):
    require(set(value) == {"schema_version", "kind", "platform", "python_version", "python_abi",
        "ezdxf_version", "library_versions", "ifcopenshell", "qualification", "receipts", "files",
        *receipts.RESULT_FLAGS}, "Unsupported controlled manifest shape")
    require(type(value["schema_version"]) is int and value["schema_version"] == 1,
            "Unsupported controlled manifest version")
    receipts.require_false(value, receipts.RESULT_FLAGS)
    require(value["kind"] == "controlled_cad_runtime" and value["platform"] == "win_amd64" and
        value["python_abi"] == "cp313" and value["ezdxf_version"] == "1.4.3" and
        value["library_versions"] == {k: v for k, v in lock["library_versions"].items() if k != "ifcopenshell"},
        "Controlled platform or library identity differs")
    source = {"path": ".deps/ifc-src", "repository": receipts.SOURCE_REPOSITORY,
              "revision": receipts.SOURCE_REVISION}
    modules = [{"path": p, "repository": u, "revision": r} for p, u, r in receipts.SUBMODULES]
    identity = value["ifcopenshell"]
    require(isinstance(identity, dict) and set(identity) == {"source", "submodules", "schemas",
        "upstream_version_informative_only", "wheel_identity_claimed", "extension",
        "generated_wrapper_bound_in_original_build_evidence"}, "Unsupported IFC identity shape")
    require(identity["source"] == source and identity["submodules"] == modules and
        identity["schemas"] == receipts.SCHEMAS and identity["wheel_identity_claimed"] is False and
        identity["upstream_version_informative_only"] == "0.0.0" and
        identity["generated_wrapper_bound_in_original_build_evidence"] is True,
        "Controlled source, wheel or generated-wrapper identity differs")
    require(identity["extension"] == {"path": EXTENSION, **receipts.checked_record(table[EXTENSION])} and
        table[EXTENSION]["sha256"] == receipts.EXTENSION_SHA256 and
        table[WRAPPER]["sha256"] == receipts.WRAPPER_SHA256, "Controlled IFC byte pins differ")
    for name in table:
        for part in receipts.relative_path(name).parts:
            normalized = part.casefold().replace("-", "_")
            require(not (normalized.startswith("ifcopenshell") and
                normalized.endswith((".dist_info", ".egg_info"))), "Stale IFC wheel identity")
    expected = {"baseline", "candidate", "build", "derivation", "generated_wrapper",
        *(n for n in receipts.PROVENANCE_NAMES if n not in {"build-evidence.json", "source-derivation.json"})}
    references = value["receipts"]
    require(isinstance(references, dict) and set(references) == expected, "Missing provenance receipts")
    evidence, paths = {}, set()
    for key, ref in references.items():
        require(isinstance(ref, dict) and set(ref) == {"path", "bytes", "sha256", "original_receipt"},
                "Unsupported provenance reference")
        name = receipts.relative_path(ref["path"]).as_posix()
        require(name.startswith("receipts/") and name not in paths and name in table and
            receipts.checked_record(ref) == receipts.checked_record(table[name]), "Unbound provenance file")
        paths.add(name)
        item, _ = receipts.read_json(root / name)
        require(type(item.get("schema_version")) is int and item["schema_version"] == 1 and
            item.get("kind") == "portable_controlled_cad_receipt" and
            item.get("original_retained_privately") is True and item.get("qualification_conferred") is False and
            item.get("original_receipt") == receipts.checked_record(ref["original_receipt"]),
            "Provenance original receipt differs")
        evidence[key] = item
    require({n for n, r in table.items() if r["origin"] == "portable-derived-receipt"} == paths,
            "Unreferenced provenance file")
    baseline = evidence["baseline"]
    require(baseline.get("lock_receipt") == lock_receipt and baseline.get("python_version") == "3.13.15" and
        baseline.get("library_versions") == lock["library_versions"], "Baseline provenance differs")
    candidate = evidence["candidate"]
    require(candidate.get("source") == source and candidate.get("submodules") == modules and
        candidate.get("schemas") == receipts.SCHEMAS, "Candidate provenance differs")
    require(evidence["derivation"].get("source") == source and
        evidence["derivation"].get("submodules") == modules, "Derivation provenance differs")
    for name in expected - {"baseline", "candidate", "build", "derivation", "generated_wrapper"}:
        require(evidence[name].get("evidence_name") == name and evidence[name].get("source") == source and
            evidence[name].get("content_role") == "original_provenance_hash_only" and
            evidence[name].get("source_closure_qualified") is False, "Source provenance identity differs")
    original = receipts.file_table(candidate.get("original_files"))
    require(type(candidate.get("file_count")) is int and candidate["file_count"] == len(original),
            "Candidate provenance inventory differs")
    build, wrapper = evidence["build"], evidence["generated_wrapper"]
    require(build.get("native_output") == identity["extension"] and build.get("schemas") == receipts.SCHEMAS and
        build.get("configuration") == "Release" and
        build.get("generated_wrapper_bound_in_original_build_evidence") is True and
        wrapper.get("path") == WRAPPER and wrapper.get("bound_in_original_build_evidence") is True and
        wrapper.get("bound_by_candidate_staging_receipt") is True and
        wrapper["original_receipt"] == receipts.checked_record(table[WRAPPER]), "Build output provenance differs")
    original_wrapper = {"root": "build", "path": "build/ifcwrap/ifcopenshell_wrapper.py",
                        **receipts.checked_record(table[WRAPPER])}
    require(build.get("original_build_generated_wrapper_receipt") == original_wrapper and
        wrapper.get("original_build_generated_wrapper_receipt") == original_wrapper and
        wrapper.get("original_build_evidence_receipt") == build["original_receipt"],
        "Original wrapper evidence differs")
    bindings = build.get("bound_provenance_inputs")
    require(isinstance(bindings, list) and len(bindings) == len(receipts.PROVENANCE_NAMES),
            "Missing provenance input bindings")
    bound = {}
    for ref in bindings:
        require(isinstance(ref, dict) and set(ref) == {"name", "bytes", "sha256"} and
            ref["name"] in receipts.PROVENANCE_NAMES and ref["name"] not in bound, "Invalid provenance binding")
        bound[ref["name"]] = receipts.checked_record(ref)
    for name in receipts.PROVENANCE_NAMES:
        key = {"build-evidence.json": "build", "source-derivation.json": "derivation"}.get(name, name)
        require(bound[name] == evidence[key]["original_receipt"] ==
            receipts.checked_record(original["provenance/" + name]), "Provenance binding differs")
    require(receipts.checked_record(original["ifcopenshell/" + receipts.EXTENSION_NAME]) ==
        receipts.checked_record(table[EXTENSION]) and
        receipts.checked_record(original["ifcopenshell/ifcopenshell_wrapper.py"]) ==
        receipts.checked_record(table[WRAPPER]), "Candidate IFC output differs")
    return receipts.SOURCE_REVISION


def inspect(runtime_root, workspace=None):
    workspace = receipts.local_path(workspace or Path(__file__).resolve().parents[1])
    root = receipts.local_path(runtime_root, workspace)
    names = [name for name in ("runtime-manifest.json", receipts.MANIFEST) if (root / name).exists()]
    require(len(names) == 1, "Expected exactly one selected runtime manifest")
    name = names[0]
    value, manifest_receipt = receipts.read_json(root / name)
    lock, lock_receipt = receipts.read_json(workspace / receipts.LOCK_RELATIVE)
    receipts.validate_lock(lock)
    require(value.get("python_version") == "3.13.15" and value.get("qualification") == "incomplete",
            "Unsupported runtime version or qualification")
    is_controlled = name == receipts.MANIFEST
    table = receipts.file_table(value.get("files"), extra_keys={"origin"} if is_controlled else None)
    receipts.verify_tree(root, table, auxiliary={name: manifest_receipt})
    require(EXTENSION in table and WRAPPER in table, "Missing exact IFC extension or wrapper")
    result = {"kind": "controlled" if is_controlled else "baseline", "manifest_name": name,
        "manifest_sha256": manifest_receipt["sha256"], "ifc_extension_sha256": table[EXTENSION]["sha256"],
        "ifc_wrapper_sha256": table[WRAPPER]["sha256"], "python_version": "3.13.15",
        "ezdxf_version": "1.4.3", "qualification": "incomplete"}
    if is_controlled:
        result["source_revision"] = controlled(root, value, table, lock, lock_receipt)
    else:
        require(set(value) == {"schema_version", "lock_sha256", "python_version", "library_versions",
            "qualification", "production_worker_integrated", "files"} and
            type(value["schema_version"]) is int and value["schema_version"] == 1 and
            value["lock_sha256"] == lock_receipt["sha256"] and
            value["library_versions"] == lock["library_versions"] and value["production_worker_integrated"] is False,
            "Baseline manifest differs from supported lock state")
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--runtime-root", required=True)
    parser.add_argument("--workspace", default=str(Path(__file__).resolve().parents[1]))
    args = parser.parse_args()
    try:
        result = inspect(args.runtime_root, args.workspace)
    except (ValueError, OSError, KeyError, TypeError):
        print("Selected CAD runtime identity refused", file=sys.stderr)
        return 1
    print(json.dumps(result, sort_keys=True, separators=(",", ":")))
    return 0


if __name__ == "__main__":
    sys.exit(main())
