"""Verify a compiled controlled IFC selection against exact owned stage bytes.

The caller exclusively owns and freezes inputs for its entire preparation or
inventory operation. Retained Windows handles and final checks detect changes
within this call; they do not promise a snapshot against arbitrary concurrent
writers after return. No CAD/native payload is imported, executed or qualified.
Portable receipts are provenance evidence, not corresponding-source delivery.
"""
from __future__ import annotations

import importlib.util
import os
from pathlib import Path


def _helper(name, filename):
    spec = importlib.util.spec_from_file_location(name, Path(__file__).parent / filename)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


stage = _helper("_distribution_stage", "stage_cad_runtime.py")
inspector = _helper("_distribution_inspector", "inspect_selected_cad_runtime.py")
receipts = stage._receipts
SOURCE_KIND = "controlled-runtime"
ADAPTER_SOURCE = "src/desktop/cad_library_adapter.py"


def _child(value, workspace):
    """Use the shared Windows namespace contract and require a strict child."""
    path = receipts.local_path(value, workspace)
    raw = Path(value)
    if not raw.is_absolute():
        raw = workspace / raw
    if os.path.normcase(str(raw)) != os.path.normcase(str(path)):
        raise ValueError("Repository path uses an alias or changed during resolution")
    if path == workspace or not path.is_relative_to(workspace):
        raise ValueError("Expected an ordinary local repository child")
    receipts.relative_path(path.relative_to(workspace).as_posix())
    return path


def verify_selected_stage(workspace, runtime_root, staged_root, selection_path):
    """Return portable IFC payload receipts only after exact identity checks.

The result binds the complete selected SDK and stage but selects only IFC
package bytes, notices, portable provenance receipts and their manifest for
shipping. The generated ownership marker and adapter have separate owners.
All returned payload hashes come from retained stage objects.
"""
    workspace = receipts.local_path(workspace)
    if not workspace.is_dir():
        raise ValueError("Workspace must be an ordinary local repository directory")
    runtime_root, staged_root, selection_path, adapter_path = (
        _child(p, workspace) for p in
        (runtime_root, staged_root, selection_path, ADAPTER_SOURCE))
    if (runtime_root == staged_root or runtime_root.is_relative_to(staged_root) or
            staged_root.is_relative_to(runtime_root)):
        raise ValueError("Selected SDK and owned stage must be disjoint")
    if any(p.is_relative_to(root) for p in (selection_path, adapter_path)
           for root in (runtime_root, staged_root)):
        raise ValueError("Compiled selection and current adapter must be separate from SDK/stage")
    ancestors, source_tree, owned, selected, adapter = [], None, None, None, None
    try:
        # Pin ancestry before following descendants. No mutation/cleanup occurs.
        paths = set(runtime_root.parents) | set(staged_root.parents) | set(selection_path.parents) | set(adapter_path.parents)
        for path in sorted(paths, key=lambda p: len(p.parts)):
            ancestors.append(stage.Directory(path))
        selected = stage.Object(selection_path)
        selection, selection_receipt = stage.read_json(selection_path, selected)
        actual = inspector.inspect(runtime_root, workspace)
        if selection != actual or actual.get("kind") != "controlled" or actual.get("manifest_name") != receipts.MANIFEST:
            raise ValueError("Compiled selection differs from the exact controlled SDK identity")

        source_tree = stage.Tree(runtime_root)
        manifest_path = runtime_root / receipts.MANIFEST
        source, manifest_receipt = receipts.read_json(manifest_path)
        if manifest_receipt["sha256"] != actual["manifest_sha256"]:
            raise ValueError("SDK manifest differs from compiled selection")
        if (source.get("kind") != "controlled_cad_runtime" or source.get("qualification") != "incomplete" or
                source.get("ifcopenshell", {}).get("source", {}).get("revision") != actual.get("source_revision")):
            raise ValueError("Controlled source revision differs from selected identity")
        receipts.require_false(source, receipts.RESULT_FLAGS)
        source_table = receipts.file_table(source.get("files"), extra_keys={"origin"})
        for name, entry in source_table.items():
            if not isinstance(entry["origin"], str) or not 0 < len(entry["origin"]) <= 128:
                raise ValueError("Controlled file origin must be a bounded inert role")
            for part in receipts.relative_path(name).parts:
                normalized = part.casefold().replace("-", "_")
                if normalized.startswith("ifcopenshell") and normalized.endswith((".dist_info", ".egg_info")):
                    raise ValueError("Stale IFC wheel identity is forbidden")
        if (inspector.EXTENSION not in source_table or inspector.WRAPPER not in source_table or
                source_table[inspector.EXTENSION]["sha256"] != actual["ifc_extension_sha256"] or
                source_table[inspector.WRAPPER]["sha256"] != actual["ifc_wrapper_sha256"]):
            raise ValueError("IFC native or generated wrapper differs from compiled selection")
        # Strip origins only for the generic owned-stage byte table.
        source_bytes = {n: receipts.checked_record(r) for n, r in source_table.items()}
        exact_source = {**source_bytes, receipts.MANIFEST: manifest_receipt}
        # A source manifest must be auxiliary, never its own listed member.
        if receipts.MANIFEST in source_bytes:
            raise ValueError("Controlled manifest lists itself")
        source_tree.verify(exact_source)
        retained_source, retained_receipt = stage.read_json(manifest_path, source_tree.files[receipts.MANIFEST])
        if (retained_source, retained_receipt) != (source, manifest_receipt):
            raise ValueError("SDK manifest changed while its exact tree was retained")

        adapter = stage.Object(adapter_path)
        adapter_receipt = adapter.receipt()
        owned, marker, marker_receipt = stage.owned_tree(staged_root)
        retained_marker, retained_marker_receipt = stage.read_json(
            staged_root / stage.MANIFEST, owned.files[stage.MANIFEST])
        if (retained_marker, retained_marker_receipt) != (marker, marker_receipt):
            raise ValueError("Owned stage manifest changed while retaining its tree")
        table = stage.validate_owned(marker)
        expected = {**exact_source, stage.ADAPTER: adapter_receipt}
        if (marker["source"] != {"manifest": receipts.MANIFEST, **manifest_receipt} or
                marker["adapter"] != {"path": stage.ADAPTER, **adapter_receipt} or
                {n: receipts.checked_record(r) for n, r in table.items()} != expected):
            raise ValueError("Owned stage differs from selected SDK and current adapter")

        payload = []
        for name, entry in sorted(source_table.items()):
            if name.startswith(("Lib/site-packages/ifcopenshell/", "notices/ifcopenshell/", "receipts/")):
                payload.append({"path": name, **owned.files[name].receipt(), "origin": entry["origin"]})
        payload.append({"path": receipts.MANIFEST,
                        **owned.files[receipts.MANIFEST].receipt(limit=receipts.MAX_JSON_BYTES),
                        "origin": "controlled-runtime-manifest"})
        payload.sort(key=lambda r: r["path"])

        # The inspector revalidates semantic provenance and the exact SDK again.
        # Then close all remaining byte/membership and retained-identity windows.
        if inspector.inspect(runtime_root, workspace) != actual:
            raise ValueError("Selected SDK identity changed during verification")
        source_tree.verify(exact_source)
        owned.verify({**table, stage.MANIFEST: marker_receipt})
        final_selection, final_selection_receipt = stage.read_json(selection_path, selected)
        if ((final_selection, final_selection_receipt) != (selection, selection_receipt) or
                not selected.matches_path() or adapter.receipt() != adapter_receipt or
                not adapter.matches_path() or any(not item.matches_path() for item in ancestors)):
            raise ValueError("Selected inputs changed during final verification")
        return {"identity": actual, "source_kind": SOURCE_KIND,
            "sdk_root": runtime_root.relative_to(workspace).as_posix(),
            "staged_root": staged_root.relative_to(workspace).as_posix(),
            "selection_record": {"path": selection_path.relative_to(workspace).as_posix(), **selection_receipt},
            "manifest_record": {"path": manifest_path.relative_to(workspace).as_posix(), **manifest_receipt},
            "payload": payload, "qualification": "incomplete", "provenance_only": True,
            "source_closure_qualified": False, "corresponding_source_qualified": False}
    finally:
        for item in (owned, source_tree, adapter, selected):
            if item is not None:
                item.close()
        for item in reversed(ancestors):
            item.close()
