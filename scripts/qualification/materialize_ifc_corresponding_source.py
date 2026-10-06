"""Materialize exact preferred IFC/SDK/Boost sources without executing payloads.

The caller freezes all inputs for this operation. Fresh Windows directory rename
publishes only a verified complete tree. On failure, the private stage is retained
for diagnosis; originals and an existing output are never removed or overwritten.
Exact source bytes do not qualify transitive source, builds, relocation or licenses.
"""
from __future__ import annotations

import argparse
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import stat
import tempfile


def _helper(name, path):
    spec = importlib.util.spec_from_file_location(name, path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


safe = _helper("_materialize_receipts", Path(__file__).with_name("compose_controlled_cad_runtime.py"))
inspector = _helper("_materialize_inspector", Path(__file__).parents[1] / "inspect_selected_cad_runtime.py")
OUTPUT_RELATIVE = ".deps/source-closure/ifc-controlled"
INDEX = "materialization-index.json"
SOURCE_PATHS = "corresponding-source-paths.json"
# Keep the private sibling short enough for the kit's 152-character archive
# members under the supported workspace and the existing 240-character limit.
STAGE_PREFIX = ".ifc-src-"
SDK_MANIFEST = "ifc-sdk-source-manifest.json"
SDK_MANIFEST_SHA256 = "7fc047eb929d3b034769698465628fc1927d262c69828a98ab103bcbb179786a"
BOOST_LIBRARY = "build/stage/lib/Release/boost_thread-vc143-mt-x64-1_86.lib"
BOOST_SDK_LIBRARY = "lib/boost_thread-vc143-mt-x64-1_86.lib"
BOOST_LIBRARY_SHA256 = "b71debf1ec706b943b79ca3326899a97e6798c62bfcd683bfc2887467df255c7"
BOOST_REQUIRED_SOURCES = {"CMakeLists.txt", "src/win32/thread.cpp", "src/win32/tss_dll.cpp",
    "src/win32/tss_pe.cpp", "src/win32/thread_primitives.cpp", "src/future.cpp"}
MAX_TREE_FILES = 4096
FLAGS = ("source_closure_qualified", "corresponding_source_qualified", "licensing_clearance",
         "build_qualified", "relocation_rebuild_qualified")


def require(condition, message):
    if not condition:
        raise ValueError(message)


def table(rows):
    require(isinstance(rows, list) and 0 < len(rows) <= MAX_TREE_FILES,
            "Source tree exceeds file bound or is empty")
    return safe.file_table(rows)


def records(mapping):
    return [{"path": name, **safe.checked_record(row)} for name, row in sorted(mapping.items())]


def tree_hash(root, expected):
    """Inventory-compatible path/NUL/content/NUL hash with stable bounded reads."""
    safe.verify_tree(root, expected)
    digest = hashlib.sha256()
    for name, receipt in sorted(expected.items()):
        path = root / name
        safe.no_links(path)
        before = path.stat()
        stamp = lambda info: (info.st_dev, info.st_ino, info.st_size, info.st_mtime_ns)
        require(stat.S_ISREG(before.st_mode) and before.st_nlink == 1,
                "Tree hash requires ordinary unlinked files")
        digest.update(name.encode("utf-8") + b"\0")
        content, size = hashlib.sha256(), 0
        with path.open("rb") as stream:
            require(stamp(os.fstat(stream.fileno())) == stamp(before), "Tree source changed before read")
            while chunk := stream.read(1 << 20):
                size += len(chunk)
                require(size <= receipt["bytes"], "Tree source grew during read")
                digest.update(chunk)
                content.update(chunk)
            require(stamp(os.fstat(stream.fileno())) == stamp(before), "Tree source changed during read")
        safe.no_links(path)
        require(stamp(path.stat()) == stamp(before) and
                {"bytes": size, "sha256": content.hexdigest()} == safe.checked_record(receipt),
                "Tree source changed after read")
        digest.update(b"\0")
    safe.verify_tree(root, expected)
    return digest.hexdigest()


def _disjoint(paths):
    require(all(left != right and not left.is_relative_to(right) and not right.is_relative_to(left)
                for index, left in enumerate(paths) for right in paths[index + 1:]),
            "Input/output roots must be separate without overlap")


def materialize(workspace, runtime_root, derived_source_root, sdk_source_root, boost_rebuild_root, output_root):
    workspace = safe.local_path(workspace)
    runtime, derived, kit, boost, output = (
        safe.local_path(path, workspace) for path in
        (runtime_root, derived_source_root, sdk_source_root, boost_rebuild_root, output_root))
    require(workspace.is_dir() and output == workspace / OUTPUT_RELATIVE and output.parent.is_dir()
            and not output.exists(), "Expected fresh exact workspace source-closure output and existing parent")
    _disjoint([runtime, derived, kit, boost, output])
    require(os.name == "nt", "Atomic no-replace directory publication requires Windows")
    identity = inspector.inspect(runtime, workspace)
    require(identity.get("kind") == "controlled" and identity.get("manifest_name") == safe.MANIFEST and
            identity.get("source_revision") == safe.SOURCE_REVISION, "Expected selected controlled runtime")
    runtime_manifest, runtime_receipt = safe.read_json(runtime / safe.MANIFEST)
    require(runtime_receipt["sha256"] == identity["manifest_sha256"], "Selected runtime manifest drifted")
    runtime_table = safe.file_table(runtime_manifest["files"], extra_keys={"origin"})
    safe.verify_tree(runtime, runtime_table, auxiliary={safe.MANIFEST: runtime_receipt})
    portable = {}
    for role, name in (("derivation", "receipts/source-derivation.json"),
                       ("preparation", "receipts/source-preparation.json.receipt.json"),
                       ("build", "receipts/build.json")):
        require(name in runtime_table, "Missing selected portable source receipt")
        portable[role], receipt = safe.read_json(runtime / name)
        require(receipt == safe.checked_record(runtime_table[name]), "Selected portable receipt changed")
    source = {"path": ".deps/ifc-src", "repository": safe.SOURCE_REPOSITORY, "revision": safe.SOURCE_REVISION}
    submodules = [{"path": p, "repository": u, "revision": r} for p, u, r in safe.SUBMODULES]
    derivation, preparation, build = (portable[key] for key in ("derivation", "preparation", "build"))
    require(all(value.get("source") == source and value.get("submodules") == submodules
                for value in (derivation, preparation)), "Prepared/derived source identity differs")
    derived_table = table(derivation.get("derived_source_files"))
    original_table = table(preparation.get("source_files"))
    modified = derivation.get("modified_file")
    require(isinstance(modified, dict) and set(modified) == {"path", "original_bytes", "original_sha256",
            "derived_bytes", "derived_sha256"} and modified["path"] == "src/ifcwrap/utils/typemaps_out.i",
            "Unsupported source modification receipt")
    require(derived_table.keys() == original_table.keys() and
            {name for name in derived_table if safe.checked_record(derived_table[name]) !=
             safe.checked_record(original_table[name])} == {modified["path"]},
            "Preferred derived source differs beyond the bound modification")
    for prefix, mapping in (("original", original_table), ("derived", derived_table)):
        require(safe.checked_record(mapping[modified["path"]]) == safe.checked_record(
            {"bytes": modified[prefix + "_bytes"], "sha256": modified[prefix + "_sha256"]}),
            "Modification byte binding differs")
    safe.verify_tree(derived, derived_table)
    kit_manifest, kit_receipt = safe.read_json(kit / SDK_MANIFEST)
    require(kit_receipt["sha256"] == SDK_MANIFEST_SHA256 and
            type(kit_manifest.get("schema_version")) is int and kit_manifest["schema_version"] == 1,
            "Exact SDK source-kit manifest differs")
    kit_table = table(kit_manifest.get("files"))
    require(SDK_MANIFEST not in kit_table and len(kit_table) < MAX_TREE_FILES,
            "SDK source tree exceeds file bound or includes its own manifest")
    kit_table = table(records({**kit_table, SDK_MANIFEST: kit_receipt}))
    safe.verify_tree(kit, kit_table)
    rebuild, rebuild_receipt = safe.read_json(boost / "evidence/rebuild.json")
    require(type(rebuild.get("schema_version")) is int and rebuild["schema_version"] == 1 and
            rebuild.get("status") == "built" and rebuild.get("expected_library") == BOOST_LIBRARY,
            "Expected completed controlled Boost.Thread rebuild receipt")
    safe.require_false(rebuild, ("build_qualified", "source_closure_qualified", "offline_sdk_rebuild_qualified",
                                "product_runtime_replaced", "redistribution_qualified"))
    require(safe.local_path(rebuild["source_kit"]) == kit and
            safe.local_path(rebuild["output_root"]) == boost, "Rebuild local source/input roots differ")
    sdk = safe.local_path(rebuild["support_sdk"])
    _disjoint([runtime, derived, kit, boost, output, sdk])
    library = rebuild.get("library")
    require(isinstance(library, dict) and set(library) == {"path", "bytes", "sha256"} and
            library["path"] == BOOST_LIBRARY and library["sha256"] == BOOST_LIBRARY_SHA256 and
            safe.checked_record(library)["bytes"] > 0, "Rebuilt Boost.Thread library pin differs")
    selected = [row for row in build.get("inputs", []) if isinstance(row, dict) and
                row.get("root") == "support_sdk" and row.get("path") == BOOST_SDK_LIBRARY and
                row.get("role") == "boost-static-library"]
    require(len(selected) == 1 and set(selected[0]) == {"root", "path", "role", "bytes", "sha256"} and
            safe.checked_record(selected[0]) == safe.checked_record(library),
            "Rebuilt library differs from selected IFC build input")
    safe.verify_file(boost / BOOST_LIBRARY, library)
    safe.verify_file(sdk / BOOST_SDK_LIBRARY, library)
    results = rebuild.get("command_results")
    require(isinstance(results, list) and len(results) == 4 and all(isinstance(row, dict) and
            type(row.get("exit_code")) is int and row["exit_code"] == 0 and row.get("error") is None
            for row in results), "Boost rebuild command evidence failed or incomplete")
    boost_table = table(rebuild.get("source_files"))
    require({"libs/thread/" + name for name in BOOST_REQUIRED_SOURCES} | {"CMakeLists.txt"}
            <= boost_table.keys(), "Boost preferred source omits required build controls/Windows sources")
    safe.verify_tree(boost / "source", boost_table)
    evidence_table = table(rebuild.get("evidence_inputs"))
    require(evidence_table == table(rebuild.get("source_kit_inputs")) and
            SDK_MANIFEST in evidence_table and all(name in kit_table and
                safe.checked_record(row) == safe.checked_record(kit_table[name])
                for name, row in evidence_table.items()), "Rebuild source-kit evidence differs from exact kit")
    safe.verify_tree(boost / "evidence/inputs", evidence_table)
    sdk_table = safe.file_table(rebuild.get("sdk_files"))
    cmake_controls = {name: row for name, row in sdk_table.items() if name.startswith("share/boost/cmake-build/")}
    require("share/boost/cmake-build/BoostRoot.cmake" in cmake_controls,
            "Rebuild SDK lacks required Boost CMake controls")
    cmake_table = table([
        {"path": name[len("share/boost/cmake-build/"):], **safe.checked_record(row)}
        for name, row in sorted(cmake_controls.items())])
    safe.verify_tree(sdk / "share/boost/cmake-build", cmake_table)
    helper_path = workspace / "scripts/rebuild_ifc_boost_thread.py"
    helper = rebuild.get("tools", {}).get("script")
    require(isinstance(helper, dict) and set(helper) == {"path", "bytes", "sha256"} and
            safe.local_path(helper["path"]) == helper_path, "Rebuild helper recipe identity differs")
    safe.verify_file(helper_path, helper)
    # Use only explicitly named roles and bounded byte records in portable JSON.
    # Original private receipts (commands, tools, local paths) are never copied.
    plan = {"derived-ifc": {name: (derived / name, row) for name, row in derived_table.items()},
            "sdk-source-kit": {name: (kit / name, row) for name, row in kit_table.items()},
            "boost-thread": {name: (boost / "source" / name, row) for name, row in boost_table.items()},
            "build-controls": {"rebuild_ifc_boost_thread.py": (helper_path, helper)}}
    for name, row in runtime_table.items():
        if name.startswith(("receipts/recipes/", "receipts/patches/")):
            plan["build-controls"]["ifc/" + name[len("receipts/"):]] = (runtime / name, row)
    require(any(name.startswith("ifc/recipes/") for name in plan["build-controls"]),
            "Selected runtime has no bound IFC build recipes")
    for name, row in cmake_controls.items():
        plan["build-controls"]["boost-cmake/" + name[len("share/boost/cmake-build/"):]] = (sdk / name, row)
    tables = {name: table(records({p: row for p, (_, row) in entries.items()}))
              for name, entries in plan.items()}
    all_bytes = sum(row["bytes"] for tree in tables.values() for row in tree.values())
    require(all_bytes <= safe.MAX_TOTAL_BYTES, "Materialization exceeds aggregate byte bound")
    roots = [workspace, runtime, derived, kit, boost, sdk, output.parent,
             boost / "source", boost / "evidence/inputs", sdk / "share/boost/cmake-build"]
    identities = [(path, safe.directory_identity(path)) for path in roots]
    original_receipts = {"boost-rebuild": rebuild_receipt, "selected-runtime": runtime_receipt,
        "sdk-source-kit-manifest": kit_receipt, "boost-rebuild-helper": safe.checked_record(helper)}
    stage = Path(tempfile.mkdtemp(prefix=STAGE_PREFIX, dir=output.parent))
    stage_identity = safe.directory_identity(stage)
    # Retain this private stage on every failure. No cleanup follows receipt-owned
    # paths or recursively deletes a tree that an attacker may have substituted.
    for tree, entries in sorted(plan.items()):
        for name, (source_path, receipt) in sorted(entries.items()):
            require(len(str(stage / tree / name)) <= 240, "Source output exceeds Windows path bound")
            safe.copy_verified(source_path, stage / tree / name, receipt)
    trees = {name: {"files": records(tree), "file_count": len(tree),
                   "bytes": sum(row["bytes"] for row in tree.values()),
                   "sha256": tree_hash(stage / name, tree)} for name, tree in sorted(tables.items())}
    index = {"schema_version": 1, "kind": "ifc_preferred_source_materialization",
        "qualification": "incomplete", **{name: False for name in FLAGS},
        "source": source, "submodules": submodules, "modified_file": modified,
        "selected_runtime_manifest": safe.checked_record(runtime_receipt),
        "original_receipts": {name: {"role": name, **safe.checked_record(row)}
                              for name, row in sorted(original_receipts.items())},
        "original_private_receipts_retained": True, "trees": trees,
        "limitations": ["Transitive preferred-source and license closure remains unqualified.",
            "Original generated Boost CMake source preserves its build-root references; relocate or regenerate using the bound helper recipe.",
            "Exact materialization does not establish a successful offline rebuild.",
            "Caller-owned input freeze is required; final checks are not a persistent snapshot guarantee."]}
    index_data = safe.json_bytes(index)
    require(len(index_data) <= safe.MAX_JSON_BYTES, "Materialization index exceeds JSON bound")
    index_receipt = {"bytes": len(index_data), "sha256": hashlib.sha256(index_data).hexdigest()}
    paths = [{"path": (output / name).relative_to(workspace).as_posix(), "kind": "directory",
              "sha256": row["sha256"]} for name, row in sorted(trees.items())]
    paths.append({"path": (output / INDEX).relative_to(workspace).as_posix(), "kind": "file",
                  "sha256": index_receipt["sha256"]})
    paths.sort(key=lambda row: row["path"])
    paths_data = safe.json_bytes(paths)
    expected = {tree + "/" + name: row for tree, entries in tables.items() for name, row in entries.items()}
    for name, data in ((INDEX, index_data), (SOURCE_PATHS, paths_data)):
        with (stage / name).open("xb") as stream:
            stream.write(data)
        expected[name] = {"bytes": len(data), "sha256": hashlib.sha256(data).hexdigest()}
    safe.verify_tree(stage, expected)
    # Recheck source bytes, exact membership, original private receipts, selected
    # identity and root identities after the whole copy/serialization window.
    safe.verify_tree(derived, derived_table)
    safe.verify_tree(kit, kit_table)
    safe.verify_tree(boost / "source", boost_table)
    safe.verify_tree(boost / "evidence/inputs", evidence_table)
    safe.verify_tree(sdk / "share/boost/cmake-build", cmake_table)
    safe.verify_file(boost / "evidence/rebuild.json", rebuild_receipt)
    safe.verify_file(boost / BOOST_LIBRARY, library)
    safe.verify_file(sdk / BOOST_SDK_LIBRARY, library)
    for entries in plan.values():
        for source_path, receipt in entries.values():
            safe.verify_file(source_path, receipt)
    require(inspector.inspect(runtime, workspace) == identity, "Selected runtime identity drifted")
    safe.verify_tree(runtime, runtime_table, auxiliary={safe.MANIFEST: runtime_receipt})
    for path, receipt in identities:
        safe.verify_directory_identity(path, receipt)
    safe.no_links(output)
    safe.verify_directory_identity(stage, stage_identity)
    require(not output.exists() and stage.parent == output.parent, "Publication target changed; target preserved")
    safe.verify_tree(stage, expected)
    safe.verify_directory_identity(stage, stage_identity)
    safe.verify_directory_identity(output.parent, dict(identities)[output.parent])
    safe.no_links(output)
    os.rename(stage, output)  # Windows refuses replacement of any existing directory.
    return index


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ("workspace", "runtime-root", "derived-source-root", "sdk-source-root", "boost-rebuild-root", "output-root"):
        parser.add_argument("--" + name, required=True, type=Path)
    args = parser.parse_args()
    try:
        result = materialize(args.workspace, args.runtime_root, args.derived_source_root,
            args.sdk_source_root, args.boost_rebuild_root, args.output_root)
    except (ValueError, OSError, KeyError, TypeError, IndexError, UnicodeError, RecursionError) as error:
        parser.exit(1, "IFC source materialization refused; any private partial stage is retained: " + str(error) + "\n")
    print(json.dumps({"kind": result["kind"], "files": sum(tree["file_count"] for tree in result["trees"].values()),
                      **{name: False for name in FLAGS}}))


if __name__ == "__main__":
    main()
