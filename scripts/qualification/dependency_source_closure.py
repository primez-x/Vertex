"""Offline, candidate-bound dependency source inventory; never legal clearance.

No download, extraction, dependency execution, Git command or native build runs.
Exact cached archives and recipes are distinguished from missing source and
binary wheels. URLs and SPDX records alone cannot satisfy corresponding source.
"""

from __future__ import annotations

import argparse
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import re
import stat
import sys
import tempfile
from urllib.parse import urlparse

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import stage_ifc_sdk_sources as safe

MAX_FILE = 1024 * 1024 * 1024
MAX_FILES = 4096
MAX_CACHE_BYTES = 16 * 1024 * 1024 * 1024
# SDK source trees are distinct from the small archive-cache table. The measured
# Boost input has 8,189 files / 90,657,648 bytes; allow finite growth up to twice
# its file count, with a separate budget for directories (including empty ones).
MAX_SOURCE_TREE_FILES = 16_384
MAX_SOURCE_TREE_ENTRIES = 32_768
MAX_SOURCE_TREE_BYTES = 16 * 1024 * 1024 * 1024
# The complete bundle includes the 8,189 Boost headers, controlled IFC source,
# runtime, Qt notices and project kit. This budget is separate from archive caches.
MAX_BUNDLE_FILES = 32_768
MAX_DEPENDENCY_RECORDS = 20_000
DEPENDENCY_KIT_PREFIX = "source-kit/third_party/dependency-inputs"
DEFAULT_CACHE_DIRS = (".deps/vcpkg/downloads", ".deps/downloads", ".deps/downloads/cad-runtime")
QT_BASE = "https://download.qt.io/archive/qt/6.8/6.8.3/submodules/"
# Official Qt MirrorBrain metadata, verified 2026-10-06. PDF source is in WebEngine.
QT_SOURCES = {
    "qtbase": ("qtbase", "56001b905601bb9023d399f3ba780d7fa940f3e4861e496a7c490331f49e0b80"),
    "qtpdf": ("qtwebengine", "df4e19ba2b3a540551b6f998d62597377ffa688c1cff564589b7da2e2bf87337"),
    "qtsvg": ("qtsvg", "35eb516460f00f264eb504baa253432384351cf23fb9980a5857190e8deef438"),
}


def require(condition, message):
    if not condition:
        raise ValueError(message)


def checked_path(root: Path, name: str) -> Path:
    name = safe.relative_path(name)
    target = root / name
    safe.no_links(target)
    return target


def file_record(root: Path, name: str, expected: str | None = None, algorithm="sha256") -> dict:
    path = checked_path(root, name)
    before = path.lstat()
    require(stat.S_ISREG(before.st_mode) and before.st_size <= MAX_FILE, "nonregular/oversized file")
    value = hashlib.new(algorithm)
    with path.open("rb") as stream:
        require(safe.stamp(os.fstat(stream.fileno())) == safe.stamp(before), "input changed before hashing")
        while chunk := stream.read(1024 * 1024):
            value.update(chunk)
        require(safe.stamp(os.fstat(stream.fileno())) == safe.stamp(before), "input changed during hashing")
    safe.no_links(path)
    require(safe.stamp(path.lstat()) == safe.stamp(before), "input changed after hashing")
    actual = value.hexdigest()
    if expected is not None:
        require(isinstance(expected, str) and re.fullmatch(r"[0-9a-f]{" + str(value.digest_size * 2) + "}", expected)
                and expected == actual, f"hash binding differs: {name}")
    return {"path": name, algorithm: actual, "bytes": before.st_size}


def json_input(root: Path, name: str, expected: str | None = None):
    record = file_record(root, name, expected)
    require(hashlib.sha256(json_bytes := safe.read_bounded(checked_path(root, name), 32 * 1024 * 1024)).hexdigest()
            == record["sha256"], "JSON changed after receipt")
    # Parse the exact bytes verified above, rather than retaining an earlier read.
    return safe.parse_json(json_bytes), record


def source_tree_snapshot(root: Path, name: str):
    """Bound every entry, including excluded caches and empty directories."""
    base = checked_path(root, name)
    require(stat.S_ISDIR(base.lstat().st_mode), "source tree is not a directory")
    files = []
    stamps = {"": safe.stamp(base.lstat())}
    names = {}
    entries = total = 0
    def traversal_error(error):
        raise error
    for current, directories, filenames in os.walk(base, followlinks=False, onerror=traversal_error):
        entries += len(directories) + len(filenames)
        require(entries <= MAX_SOURCE_TREE_ENTRIES, "source tree entry count exceeds bound")
        for child in directories + filenames:
            path = Path(current) / child
            safe.no_links(path)
            relative = safe.relative_path(path.relative_to(base).as_posix())
            kind = "directory" if child in directories else "file"
            for index in range(1, len(parts := relative.split("/")) + 1):
                spelling = "/".join(parts[:index])
                entry = (spelling, "directory" if index < len(parts) else kind)
                require(names.get(spelling.casefold(), entry) == entry, "source tree case-colliding path")
                names[spelling.casefold()] = entry
            before = path.lstat()
            stamps[relative] = safe.stamp(before)
            if kind == "directory":
                require(stat.S_ISDIR(before.st_mode), "invalid source tree directory")
            else:
                require(stat.S_ISREG(before.st_mode) and before.st_size <= MAX_FILE, "invalid source tree file")
                files.append(path)
                require(len(files) <= MAX_SOURCE_TREE_FILES, "source tree file count exceeds bound")
                total += before.st_size
                require(total <= MAX_SOURCE_TREE_BYTES, "source tree bytes exceed bound")
    return base, sorted(files, key=lambda path: path.relative_to(base).as_posix()), stamps


def source_tree_files(root: Path, name: str) -> list[str]:
    base, files, stamps = source_tree_snapshot(root, name)
    require(source_tree_snapshot(root, name)[2] == stamps, "source tree changed during enumeration")
    return [path.relative_to(root).as_posix() for path in files
            if "__pycache__" not in path.relative_to(base).parts and path.suffix.lower() not in {".pyc", ".pyo"}]


def source_tree_hash(root: Path, name: str) -> str:
    """Match unambiguous v2 inventory receipts with bounded, stable reads."""
    base, files, stamps = source_tree_snapshot(root, name)
    value = hashlib.sha256(b"Vertex-source-tree-v2\0")
    selected_count = sum("__pycache__" not in path.relative_to(base).parts
                         and path.suffix.lower() not in {".pyc", ".pyo"} for path in files)
    value.update(selected_count.to_bytes(8, "big"))
    for path in files:
        relative = path.relative_to(base)
        child_name = relative.as_posix()
        before = path.lstat()
        require(safe.stamp(before) == stamps[child_name], "source changed before hashing")
        require(stat.S_ISREG(before.st_mode) and before.st_size <= MAX_FILE, "invalid source tree file")
        # Match future workspace inventory receipts. Traversal, no-links and
        # resource limits still cover caches before their bytes are excluded.
        if "__pycache__" in relative.parts or path.suffix.lower() in {".pyc", ".pyo"}:
            continue
        path_bytes = child_name.encode("utf-8")
        content_hash = hashlib.sha256()
        count = 0
        with path.open("rb") as stream:
            require(safe.stamp(os.fstat(stream.fileno())) == safe.stamp(before), "source changed before read")
            while chunk := stream.read(1024 * 1024):
                count += len(chunk)
                require(count <= before.st_size and count <= MAX_FILE, "source read bytes exceed bound")
                content_hash.update(chunk)
            require(safe.stamp(os.fstat(stream.fileno())) == safe.stamp(before), "source changed during read")
        require(count == before.st_size, "source read bytes differ")
        safe.no_links(path)
        require(safe.stamp(path.lstat()) == safe.stamp(before), "source changed after read")
        value.update(len(path_bytes).to_bytes(8, "big"))
        value.update(path_bytes)
        value.update(count.to_bytes(8, "big"))
        value.update(content_hash.digest())
    require(source_tree_snapshot(root, name)[2] == stamps, "source tree changed during hashing")
    return value.hexdigest()


def source_cache(root: Path, directories) -> list[dict]:
    paths = set()
    for directory in directories:
        base = checked_path(root, directory)
        if not base.exists():
            continue
        require(base.is_dir(), "cache input is not a directory")
        for path in base.iterdir():
            if path.is_dir():
                safe.no_links(path)
                continue
            safe.no_links(path)
            if path.name.endswith((".zip", ".tar.gz", ".tar.xz", ".tar.bz2", ".tgz", ".tar", ".whl")):
                paths.add(path.relative_to(root).as_posix())
    safe.check_names(paths)
    require(len(paths) <= MAX_FILES, "too many cache files")
    require(sum((root / path).stat().st_size for path in paths) <= MAX_CACHE_BYTES, "cache bytes exceed bound")
    result = []
    for path in sorted(paths):
        record = file_record(root, path)
        record["sha512"] = file_record(root, path, algorithm="sha512")["sha512"]
        result.append(record)
    return result


def cached_source(cache, url, checksum, algorithm="sha512", **extra):
    require(isinstance(url, str) and url.startswith(("https://", "http://", "git+https://", "git://")),
            "invalid source URL")
    require(re.fullmatch(r"[0-9a-f]{" + ("128" if algorithm == "sha512" else "64") + "}", checksum) is not None,
            "invalid source checksum")
    matches = [item for item in cache if item[algorithm] == checksum and not item["path"].endswith(".whl")]
    return {"url": url, "declared_checksum": {"algorithm": algorithm, "value": checksum},
            "status": "exact_local_source_present" if matches else "missing_source",
            "local_files": matches, **extra}


def notice_record(root, record):
    result = file_record(root, record["path"], record["sha256"])
    data = safe.read_bounded(checked_path(root, record["path"]), 32 * 1024 * 1024)
    require(hashlib.sha256(data).hexdigest() == record["sha256"], "notice changed after hashing")
    text = data.decode("utf-8", "replace")
    if record["path"].endswith((".spdx", ".spdx.json")):
        role = "license_metadata_only"
    elif len(data) < 1024 and "http" in text and re.search(r"please visit|latest|see (?:https?|the)", text, re.I) and not re.search(r"copyright|permission is|redistribution and use|warranty|preamble", text, re.I):
        role = "pointer_only"
    elif re.search(r"copyright|permission is|redistribution and use|warranty|preamble|GNU (?:LESSER |GENERAL )", text, re.I):
        role = "notice_text_present_review_required"
    else:
        role = "unclassified_text_review_required"
    return {**result, **({"inventory_path": record["inventory_path"]} if "inventory_path" in record else {}),
            "content_role": role}


def vcpkg_sources(root, source, cache):
    spdx, receipt = json_input(root, source["spdx_path"], source["spdx_sha256"])
    resources = [item for item in spdx.get("packages", []) if item.get("SPDXID", "").startswith("SPDXRef-resource-")]
    resolved = []
    for resource in resources:
        resolved.append(cached_source(cache, resource["downloadLocation"], safe.checksum(resource, "SHA512"),
                                      provenance=receipt, name=resource.get("name")))
    recipe_files = [item for item in spdx.get("files", []) if item.get("SPDXID", "").startswith("SPDXRef-port-file-")]
    recipe = {"status": "missing_or_stale_recipe", "files": [], "recipe_options": [],
              "reason": "exact historical recipe required; current ports are not substitutes"}
    package = source.get("package_name", "")
    require(re.fullmatch(r"[a-z0-9][a-z0-9-]*", package) is not None, "invalid native package name")
    try:
        records = []
        options = []
        names = []
        for item in recipe_files:
            name = item["fileName"]
            require(name.startswith("./"), "invalid SPDX recipe path")
            relative = safe.relative_path(name[2:])
            names.append(relative)
            path = f".deps/vcpkg/ports/{package}/{relative}"
            records.append(file_record(root, path, safe.checksum(item, "SHA256")))
            if relative == "portfile.cmake":
                text = safe.read_bounded(checked_path(root, path), safe.MAX_RECIPE).decode("utf-8")
                options = sorted(set(re.findall(r"-D[A-Za-z0-9_]+=[^\r\n)]+", text)))
        safe.check_names(names)
        if records:
            recipe = {"status": "exact_local_recipe_present", "files": records, "recipe_options": options,
                      "options_role": "recipe expressions; actual expanded configure options still require build evidence"}
    except (OSError, ValueError, UnicodeError):
        pass
    status_path = str(Path(source.get("prefix_path", ".deps/native/x64-windows")).parent / "vcpkg/status").replace("\\", "/")
    status = None
    if checked_path(root, status_path).is_file():
        status = file_record(root, status_path)
    return resolved, recipe, {"spdx": receipt, "installed_status": status,
                              "declared_recipe_origin": source.get("url")}


def metadata_index(root, path):
    if path is None:
        return {}, None
    document, receipt = json_input(root, path)
    require(document.get("schema_version") == 1 and isinstance(document.get("pypi"), list), "invalid upstream metadata receipt")
    result = {}
    for item in document["pypi"]:
        require(item.get("status") == "observed" and isinstance(item.get("metadata_sha256"), str)
                and re.fullmatch(r"[0-9a-f]{64}", item["metadata_sha256"]), "invalid official metadata digest/status")
        key = (item["project"].lower().replace("_", "-"), item["version"])
        require(key not in result, "duplicate upstream project/version")
        require(item["metadata_url"] == f"https://pypi.org/pypi/{item['project']}/{item['version']}/json", "upstream metadata origin differs")
        for archive in item.get("source_distributions", []):
            safe.relative_path(archive["filename"])
            require(urlparse(archive["url"]).scheme == "https" and urlparse(archive["url"]).hostname == "files.pythonhosted.org",
                    "source archive origin is not the official package index")
            require(not archive["filename"].endswith(".whl"), "wheel cannot be a source distribution")
        result[key] = item
    return result, receipt


def dependency_bundle_sources(root, bundle_root, manifest, by_path, document, inventory_receipt, source_kit_receipt, mapping):
    """Bind SDK inputs to delivered source bytes, never a writable SDK fallback."""
    reference = manifest.get("dependency_source_kit")
    if reference is None:
        return
    relative = DEPENDENCY_KIT_PREFIX + "/dependency-source-kit-manifest.json"
    require(isinstance(reference, dict) and reference.get("path") == relative,
            "invalid frozen dependency source kit location")
    member = by_path.get(relative)
    require(member is not None and member.get("kind") == "dependency-source" and member.get("install") is False
            and member["sha256"] == reference.get("sha256") and member.get("size") == reference.get("size"),
            "dependency source kit reference differs from frozen payload")
    kit, _ = json_input(root, bundle_root + "/" + relative, reference["sha256"])
    # Reuse staging's complete composer replay, resolving project references
    # exclusively from this delivered source kit rather than the live workspace.
    helper_path = Path(__file__).resolve().parents[1] / "stage_offline_bundle.py"
    spec = importlib.util.spec_from_file_location("bundle_frozen_dependency_validator", helper_path)
    require(spec is not None and spec.loader is not None, "cannot load frozen dependency validator")
    validator = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(validator)
    project_kit, _ = json_input(root, bundle_root + "/" + manifest["source_kit"]["path"],
                                source_kit_receipt["sha256"])
    validator._SOURCE_KIT.validate_manifest(project_kit)
    validator._validate_dependency_payload(
        checked_path(root, bundle_root + "/" + DEPENDENCY_KIT_PREFIX), project_kit,
        lambda identity: checked_path(root, bundle_root + "/source-kit/" + safe.relative_path(identity)))
    binding = kit.get("candidate_binding")
    require(isinstance(binding, dict) and binding.get("inventory", {}).get("sha256") == inventory_receipt["sha256"],
            "dependency source kit inventory differs from frozen candidate")
    direct = binding.get("source_kit")
    prior = binding.get("offline_bundle")
    require(direct is not None or prior is not None, "dependency source kit has no project kit binding")
    for selected in ([direct] if direct is not None else []) + ([prior.get("source_kit")] if isinstance(prior, dict) else []):
        require(isinstance(selected, dict) and selected.get("sha256") == source_kit_receipt["sha256"],
                "dependency source kit project kit differs from frozen candidate")
    require(prior is None or isinstance(prior, dict), "invalid dependency source kit prior binding")
    rows, components = kit.get("files"), kit.get("components")
    component_ids = {item["id"] for item in document["components"]}
    require(len(components) == len(component_ids) and {row["id"] for row in components} == component_ids,
            "dependency source kit component ownership differs from candidate")
    files = {}
    for row in rows:
        original = safe.relative_path(row["source_path"])
        location = safe.relative_path(row["path"])
        require(location == "inputs/" + original, "dependency source kit source/path differs")
        frozen = by_path.get(DEPENDENCY_KIT_PREFIX + "/" + location)
        require(frozen is not None and frozen.get("kind") == "dependency-source" and frozen.get("install") is False
                and frozen["sha256"] == row["sha256"] and type(row.get("bytes")) is int
                and frozen.get("size") == row["bytes"], "dependency source file differs from frozen payload")
        files[location] = row
    expected = {relative, *[DEPENDENCY_KIT_PREFIX + "/" + path for path in files]}
    require({path for path in by_path if path.startswith(DEPENDENCY_KIT_PREFIX + "/")} == expected,
            "frozen dependency source kit has missing or unlisted files")
    component_index = {row["id"]: row for row in components}
    for component in document["components"]:
        owner = component["id"]
        declared = component_index[owner].get("assets")
        require(isinstance(declared, list) and len(declared) <= MAX_DEPENDENCY_RECORDS,
                "invalid dependency source input table")
        for receipt in component.get("source_inputs", []):
            original = safe.relative_path(receipt["path"])
            matches = [row for row in declared if row.get("source_path") == original]
            require(len(matches) == 1, "missing or ambiguous delivered dependency source input")
            row = matches[0]
            kind = receipt.get("kind", "file")
            location = "inputs/" + original
            require(row.get("payload_path") == location and row.get("sha256") == receipt["sha256"]
                    and row.get("kind", "file") == kind, "delivered source input identity differs from inventory")
            if kind == "directory":
                require(source_tree_hash(root, bundle_root + "/" + DEPENDENCY_KIT_PREFIX + "/" + location)
                        == receipt["sha256"], "delivered source tree hash differs from inventory")
                children = source_tree_files(root, bundle_root + "/" + DEPENDENCY_KIT_PREFIX + "/" + location)
                for child in children:
                    child_relative = child[len(bundle_root + "/" + DEPENDENCY_KIT_PREFIX + "/"):]
                    entry = files.get(child_relative)
                    require(entry is not None and owner in entry["component_ids"] and "sdk_source" in entry["roles"],
                            "delivered source tree has unbound child ownership")
            else:
                require(kind == "file", "unknown delivered source input kind")
                entry = files.get(location)
                require(entry is not None and owner in entry["component_ids"] and "assets" in entry["roles"]
                        and entry["sha256"] == row["sha256"] and entry["bytes"] == row.get("bytes"),
                        "delivered source input file has unbound ownership")
            if not any((owner, original, role) in mapping for role in ("asset", "source")):
                mapping[(owner, original, "source")] = {
                    "path": bundle_root + "/" + DEPENDENCY_KIT_PREFIX + "/" + location,
                    "sha256": receipt["sha256"], "kind": kind}


def frozen_bundle_binding(root, bundle_root, inventory_receipt, document):
    """Bind explicit portable source identities to verified frozen payloads."""
    bundle_root = safe.relative_path(bundle_root)
    manifest, manifest_receipt = json_input(root, bundle_root + "/offline-bundle-manifest.json")
    require(manifest.get("schema_version") == 1 and isinstance(manifest.get("files"), list)
            and len(manifest["files"]) <= MAX_BUNDLE_FILES, "invalid offline bundle manifest")
    require(manifest["source_inventory"]["sha256"] == inventory_receipt["sha256"], "bundle inventory differs")
    safe.check_names([entry["path"] for entry in manifest["files"]])
    by_path = {entry["path"]: entry for entry in manifest["files"]}
    payload = [file_record(root, bundle_root + "/" + entry["path"], entry["sha256"])
               for entry in sorted(manifest["files"], key=lambda entry: entry["path"])]
    for entry, record in zip(sorted(manifest["files"], key=lambda entry: entry["path"]), payload):
        if "size" in entry:
            require(entry["size"] == record["bytes"], "bundle payload size differs")
    bound_receipts = {}
    for key in ("source_inventory", "runtime_manifest", "source_kit"):
        reference = manifest[key]
        require(reference["path"] in by_path and by_path[reference["path"]]["sha256"] == reference["sha256"],
                "bundle reference differs from payload identity")
        bound_receipts[key] = file_record(root, bundle_root + "/" + reference["path"], reference["sha256"])
    portable_rows = [entry for entry in manifest["files"] if entry.get("kind") == "portable-package-manifest"]
    require(len(portable_rows) == 1, "missing or ambiguous portable package mapping manifest")
    portable_entry = portable_rows[0]
    portable, portable_receipt = json_input(root, bundle_root + "/" + portable_entry["path"], portable_entry["sha256"])
    require(portable.get("schema_version") == 1 and isinstance(portable.get("files"), list)
            and len(portable["files"]) <= 10000, "invalid portable mapping manifest")
    require(portable["source_inventory"]["sha256"] == inventory_receipt["sha256"], "portable inventory differs")
    original_inventory = manifest["source_inventory"].get("original_path")
    if original_inventory is not None:
        require(portable["source_inventory"]["path"] == original_inventory, "portable original inventory path differs")
    safe.check_names([entry["path"] for entry in portable["files"]])
    components = document["components"]
    component_ids = {entry["id"] for entry in components}
    binary_receipts = {(entry["component_id"], entry["path"]): entry for entry in document["binaries"]}
    notice_receipts = {}
    asset_receipts = {}
    for component in [*components, *document.get("static_inputs", [])]:
        identifier = component.get("id", component.get("component_id"))
        require(identifier in component_ids, "unknown static source component")
        for fields, target in ((("notices",), notice_receipts), (("source_inputs", "sources", "assets"), asset_receipts)):
            for field in fields:
                for receipt in component.get(field, []):
                    key = (identifier, safe.relative_path(receipt["path"]))
                    require(key not in target or target[key] == receipt["sha256"], "conflicting original source receipts")
                    target[key] = receipt["sha256"]
    mapping = {}
    for entry in portable["files"]:
        destination = entry["path"]
        frozen = by_path.get(destination)
        require(frozen is not None and all(frozen.get(key) == entry.get(key) for key in ("kind", "component_id", "sha256")),
                "portable destination/component/kind/hash differs from frozen payload")
        if entry.get("kind") == "sbom":
            continue
        require(entry.get("kind") in ("binary", "notice", "source", "asset") and
                entry.get("component_id") in component_ids and entry.get("inventory_entry") == entry["component_id"],
                "portable mapping has unknown kind or component identity")
        original = safe.relative_path(entry["source"])
        source_key = (entry["component_id"], original)
        if entry["kind"] == "binary":
            receipt = binary_receipts.get(source_key)
            require(receipt is not None and receipt["destination"] == destination and receipt["sha256"] == entry["sha256"],
                    "portable binary differs from original source/destination/hash receipt")
        else:
            admitted = notice_receipts if entry["kind"] == "notice" else asset_receipts
            require(admitted.get(source_key) == entry["sha256"], "portable source differs from original component receipt")
        key = (entry["component_id"], original, entry["kind"])
        require(key not in mapping, "ambiguous portable source mapping")
        mapping[key] = {**entry, "path": bundle_root + "/" + destination}
    runtime, _ = json_input(root, bound_receipts["runtime_manifest"]["path"], bound_receipts["runtime_manifest"]["sha256"])
    require(runtime.get("schema_version") == 1 and isinstance(runtime.get("files"), list)
            and len(runtime["files"]) <= 10000, "invalid frozen runtime manifest")
    safe.check_names([entry["path"] for entry in runtime["files"]])
    identity = lambda entry: (entry["path"], entry["sha256"], entry.get("kind"), entry.get("component_id"))
    require(sorted(map(identity, runtime["files"])) ==
            sorted(identity(entry) for entry in manifest["files"] if entry.get("install") is True),
            "runtime manifest differs from frozen install payload")
    dependency_bundle_sources(root, bundle_root, manifest, by_path, document, inventory_receipt,
                              bound_receipts["source_kit"], mapping)
    bundle = {"manifest": manifest_receipt, **bound_receipts, "portable_manifest": portable_receipt,
              "payload": payload, "source_location": bundle_root + "/source-kit"}
    return bundle, mapping


def bound_component_record(root, component_id, receipt, mapping, kinds, *, required=False, source_input=False):
    original = safe.relative_path(receipt["path"])
    kind = receipt.get("kind", "file")
    require(kind in ("file", "directory"), "unknown component receipt kind")
    require(kind != "directory" or source_input, "directory receipt is only admitted as source input")
    require(isinstance(receipt["sha256"], str) and re.fullmatch(r"[0-9a-f]{64}", receipt["sha256"]),
            "invalid component receipt hash")
    if kind == "directory":
        require(not ({"bytes", "sha512"} & set(receipt)), "invalid directory receipt")
    def record(location):
        if kind == "directory":
            require(source_tree_hash(root, location) == receipt["sha256"], "source tree hash differs")
            return {"path": location, "sha256": receipt["sha256"], "kind": "directory"}
        actual = file_record(root, location, receipt["sha256"])
        require("bytes" not in receipt or (type(receipt["bytes"]) is int and receipt["bytes"] == actual["bytes"]),
                "component receipt bytes differ")
        return actual
    matches = [mapping[(component_id, original, kind)] for kind in kinds if (component_id, original, kind) in mapping]
    require(len(matches) <= 1, "ambiguous frozen component source identity")
    if matches:
        entry = matches[0]
        require(entry["sha256"] == receipt["sha256"], "frozen source hash differs from original inventory receipt")
        require(entry.get("kind") == "directory" if kind == "directory" else entry.get("kind") != "directory",
                "frozen source kind differs from original inventory receipt")
        return {**record(entry["path"]), "inventory_path": original}
    require(not required, "missing exact frozen component source mapping")
    return record(original)


def controlled_runtime_sources(root, component_id, source, mapping, *, frozen=False):
    """Separate selected runtime/provenance from declared preferred source bytes."""
    require(source.get("licensing_clearance") is False and source.get("source_closure_qualified") is False
            and source.get("ifcopenshell_version_informative_only") is True,
            "controlled runtime cannot confer source or licensing qualification")
    revision = source.get("source_revision")
    require(isinstance(revision, str) and re.fullmatch(r"[0-9a-f]{40}", revision),
            "invalid controlled runtime source revision")

    def descriptor(row, *, kinds):
        require(isinstance(row, dict), "invalid controlled source receipt")
        safe.relative_path(row.get("path"))
        require(isinstance(row.get("sha256"), str) and re.fullmatch(r"[0-9a-f]{64}", row["sha256"])
                and row.get("kind", "file") in kinds, "invalid controlled source hash/kind")
        if "bytes" in row:
            require(type(row["bytes"]) is int and 0 <= row["bytes"] <= MAX_FILE, "invalid controlled receipt bytes")
        return row

    def exact_file(row, *, runtime=False):
        descriptor(row, kinds=("file",))
        if runtime:
            record = bound_component_record(root, component_id, row, mapping, ("asset", "source"), required=frozen)
        else:
            record = file_record(root, row["path"], row["sha256"])
        require("bytes" not in row or row["bytes"] == record["bytes"], "controlled receipt bytes differ")
        return record

    provenance = exact_file(source.get("runtime_manifest"))
    selection = exact_file(source.get("selection"))
    runtime_paths = source.get("source_paths")
    declared = source.get("corresponding_source_paths", [])
    require(isinstance(runtime_paths, list) and 0 < len(runtime_paths) <= MAX_FILES
            and isinstance(declared, list) and len(declared) <= MAX_FILES, "invalid controlled runtime source table")
    safe.check_names([descriptor(row, kinds=("file",))["path"] for row in runtime_paths])
    safe.check_names([descriptor(row, kinds=("file", "directory"))["path"] for row in declared])
    assets = [exact_file(row, runtime=True) for row in runtime_paths]
    records, missing = [], []
    for row in declared:
        # Dependency sources remain workspace-relative even for a frozen app kit.
        path = checked_path(root, row["path"])
        if not path.exists() or (row.get("kind", "file") == "directory" and not path.is_dir()):
            missing.append(row["path"])
            continue
        if row.get("kind", "file") == "directory":
            actual = {"path": row["path"], "kind": "directory", "sha256": source_tree_hash(root, row["path"])}
        else:
            actual = file_record(root, row["path"])
            require("bytes" not in row or row["bytes"] == actual["bytes"], "controlled source bytes differ")
        if actual["sha256"] == row["sha256"]:
            records.append(actual)
        else:
            missing.append(row["path"])
    sources = [{"url": None, "status": "exact_local_source_present" if records and not missing else "missing_source",
                "local_files": records, "missing_or_changed_paths": missing, "provenance": selection,
                "binding_role": "Declared source bytes for controlled runtime revision " + revision
                                + "; transitive closure and rebuild remain unqualified"}]
    return sources, assets, provenance, selection


def audit(root: Path, inventory_path: str, *, build_receipts=(), metadata_path=None, cache_dirs=DEFAULT_CACHE_DIRS, bundle_root=None) -> dict:
    root = root.absolute()
    safe.no_links(root)
    document, inventory_receipt = json_input(root, inventory_path)
    require(document.get("schema_version") == 1, "unsupported distribution inventory schema")
    components = document.get("components")
    binaries = document.get("binaries")
    require(isinstance(components, list) and 0 < len(components) <= 512 and isinstance(binaries, list) and len(binaries) <= 4096,
            "invalid candidate component/binary table")
    identifiers = [item["id"] for item in components]
    require(all(isinstance(value, str) and re.fullmatch(r"[a-z0-9][a-z0-9-]*", value) for value in identifiers), "invalid component ID")
    require(len(set(identifiers)) == len(identifiers), "duplicate candidate component IDs")
    safe.check_names([item["path"] for item in binaries])
    bundle, mapping = frozen_bundle_binding(root, bundle_root, inventory_receipt, document) if bundle_root is not None else (None, {})
    source_prefix = bundle["source_location"] + "/" if bundle else ""
    component_index = {item["id"]: item for item in components}
    if bundle:
        safe.check_names([item["destination"] for item in binaries])
    runtime_files = []
    for item in binaries:
        require(item["component_id"] in identifiers, "unowned candidate binary")
        if bundle:
            destination = safe.relative_path(item["destination"])
            destinations = component_index[item["component_id"]].get("destinations", {})
            matches = [path for name, path in destinations.items() if name.casefold() == item["name"].casefold()]
            require(matches == [destination], "binary destination differs from component destinations")
            entry = mapping.get((item["component_id"], item["path"], "binary"))
            require(entry is not None and entry["path"] == bundle_root + "/" + destination,
                    "missing or differing frozen binary destination mapping")
        runtime_files.append({"component_id": item["component_id"],
                              **bound_component_record(root, item["component_id"], item, mapping, ("binary",), required=bool(bundle))})
    evidence = document["evidence"]
    receipts = {key: file_record(root, evidence[key]["path"], evidence[key]["sha256"])
                for key in ("component_manifest", "runtime")}
    build = [file_record(root, path) for path in sorted(build_receipts)]
    safe.check_names([item["path"] for item in build])
    metadata, metadata_receipt = metadata_index(root, metadata_path)
    cache = source_cache(root, cache_dirs)
    result = []
    component_records = []
    controlled_trees = []
    for component in sorted(components, key=lambda item: item["id"]):
        source = component["package"]["source"]
        require(isinstance(source, dict) and isinstance(source.get("kind"), str), "invalid component source")
        included = component.get("distribution_status", "included") != "excluded"
        notices = [notice_record(root, bound_component_record(root, component["id"], entry, mapping, ("notice",),
                                                             required=bool(bundle) and included))
                   for entry in component.get("notices", [])]
        assets = [bound_component_record(root, component["id"], entry, mapping, ("asset", "source"), required=bool(bundle) and included,
                                         source_input=True)
                  for entry in component.get("source_inputs", [])]
        artifacts = [bound_component_record(root, component["id"], entry, mapping, ("asset", "source", "notice"))
                     for entry in component.get("artifacts", [])]
        component_records.extend([*notices, *assets, *artifacts])
        safe.check_names([item["path"] for item in notices])
        item = {"id": component["id"], "package": {key: component["package"][key] for key in ("name", "version", "license")},
                "declared_source_kind": source["kind"], "sources": [], "notices": notices, "assets": assets, "artifacts": artifacts,
                "source_status": "missing_source", "recipe": None, "remaining": [],
                "licensing_clearance": False, "corresponding_source_qualified": False}
        kind = source["kind"]
        if kind == "vcpkg":
            item["sources"], item["recipe"], item["provenance"] = vcpkg_sources(root, source, cache)
            if component["package"]["license"] == "LicenseRef-vcpkg-null":
                item["remaining"].append("upstream_spdx_license_conclusion_unresolved")
        elif kind == "qt":
            _, sbom = json_input(root, source["spdx_path"], source["spdx_sha256"])
            item["provenance"] = {"sbom": sbom, "declared_revision_locator": source.get("url")}
            if component["id"] in QT_SOURCES and component["package"]["version"] == "6.8.3":
                module, checksum = QT_SOURCES[component["id"]]
                url = QT_BASE + f"{module}-everywhere-src-6.8.3.tar.xz"
                item["sources"] = [cached_source(cache, url, checksum, "sha256", provenance_url=url + ".mirrorlist",
                                                 binding_role="official release source; exact prebuilt revision/options still require review")]
            item["remaining"].append("exact_prebuilt_source_and_embedded_third_party_binding_pending")
            prefix = source.get("prefix_path")
            if prefix and checked_path(root, prefix + "/mkspecs/qconfig.pri").is_file():
                configuration = file_record(root, prefix + "/mkspecs/qconfig.pri")
                text = safe.read_bounded(checked_path(root, configuration["path"]), safe.MAX_RECIPE).decode("utf-8")
                require(hashlib.sha256(text.encode("utf-8")).hexdigest() == configuration["sha256"], "Qt configuration changed")
                selected = {}
                for line in text.splitlines():
                    match = re.fullmatch(
                        r"(QT_CONFIG|QT_VERSION|QT_ARCH|QT_BUILDABI|QT_MSVC_(?:MAJOR|MINOR|PATCH)_VERSION|"
                        r"QT_COMPILER_[A-Z0-9_]+|QT\.global\.(?:enabled|disabled)_features)"
                        r"\s*(\+?=)\s*([A-Za-z0-9_. +\-]*)", line)
                    if match:
                        key, operator, value = match.groups()
                        selected[key] = (selected.get(key, []) if operator == "+=" else []) + value.split()
                item["configuration"] = {**configuration, "observed_variables": selected,
                                         "role": "observed prebuilt qconfig; complete configure/build/relink controls pending"}
                component_records.append(configuration)
        elif kind == "bootstrap":
            _, receipt = json_input(root, source["dependencies_path"], source["dependencies_sha256"])
            if source.get("asset_path") is not None:
                # A trained model/font artifact is not editable or training source.
                # Resolve frozen payloads through the original inventory identity.
                item["upstream_asset"] = {
                    **bound_component_record(root, component["id"],
                                             {"path": source["asset_path"], "sha256": source["declared_sha256"]},
                                             mapping, ("asset", "source"), required=bool(bundle) and included),
                    "url": source["url"], "provenance": receipt,
                    "binding_role": "exact upstream distributed asset; generation evidence not assessed"}
                component_records.append(item["upstream_asset"])
                item["source_status"] = "distributed_asset_recorded"
            archive = next((record for record in component.get("artifacts", [])
                            if record["path"].endswith((".zip", ".tar.gz", ".tar.xz"))), None)
            if archive:
                item["sources"] = [cached_source(cache, source["url"], source["declared_sha256"], "sha256", provenance=receipt)]
            elif source.get("kind_detail") == "file":
                path = ".deps/downloads/" + source["artifact_filename"]
                item["sources"] = [{"url": source["url"], "status": "exact_local_source_present",
                                    "local_files": [bound_component_record(root, component["id"], {"path": path, "sha256": source["declared_sha256"]},
                                                                          mapping, ("asset", "source"))], "provenance": receipt}]
        elif kind in ("workspace", "planegcs"):
            records = []
            missing = []
            for entry in source.get("source_paths", []):
                safe.relative_path(entry["path"])
                require(entry["kind"] in ("file", "directory") and isinstance(entry["sha256"], str)
                        and re.fullmatch(r"[0-9a-f]{64}", entry["sha256"]), "invalid source path receipt")
                location = source_prefix + entry["path"] if kind == "workspace" else entry["path"]
                path = checked_path(root, location)
                try:
                    if entry["kind"] == "directory":
                        require(source_tree_hash(root, location) == entry["sha256"], "source tree hash differs")
                        records.append({"path": location, "sha256": entry["sha256"], "kind": "directory"})
                    else:
                        records.append(file_record(root, location, entry["sha256"]))
                except (OSError, ValueError):
                    missing.append(entry["path"])
            item["sources"] = [{"url": "https://github.com/FreeCAD/FreeCAD/tree/" + source["upstream_commit"] if kind == "planegcs" else None,
                                "status": "exact_local_source_present" if records and not missing else "missing_source",
                                "local_files": records, "missing_or_changed_paths": missing}]
            if kind == "planegcs":
                item["provenance"] = file_record(root, source["provenance_path"], source["provenance_sha256"])
        elif kind == "controlled-runtime":
            item["sources"], controlled_assets, item["provenance"], selection = controlled_runtime_sources(
                root, component["id"], source, mapping, frozen=bool(bundle) and included)
            item["assets"].extend(record for record in controlled_assets if record not in item["assets"])
            component_records.extend([*controlled_assets, item["provenance"], selection])
            for record in item["sources"][0]["local_files"]:
                (controlled_trees if record.get("kind") == "directory" else component_records).append(record)
            if not source.get("corresponding_source_paths"):
                item["remaining"].append("controlled_runtime_corresponding_source_paths_not_declared")
            item["remaining"].append("controlled_runtime_transitive_source_closure_not_qualified")
        elif kind == "locked-archive":
            item["binary_archive"] = file_record(root, source["archive_path"], source["archive_sha256"])
            item["provenance"] = file_record(root, source["lock_path"], source["lock_sha256"])
            key = (component["package"]["name"].lower().replace("_", "-"), component["package"]["version"])
            upstream = metadata.get(key)
            if upstream:
                item["upstream_metadata"] = upstream
                for archive in upstream.get("source_distributions", []):
                    item["sources"].append(cached_source(cache, archive["url"], archive["sha256"], "sha256", provenance=metadata_receipt))
            if component["id"] == "cad-cpython":
                lock, _ = json_input(root, source["lock_path"], source["lock_sha256"])
                for asset in lock["assets"]:
                    if asset["name"] == "CPython source" and asset["version"] == component["package"]["version"]:
                        item["sources"].append(cached_source(cache, asset["url"], asset["sha256"], "sha256", provenance=item["provenance"]))
            item["remaining"].append("wheel_or_interpreter_embedded_native_source_and_build_binding_pending")
        elif kind == "redistributable":
            item["source_status"] = "redistributable_rights_review"
            item["remaining"].append("exact_distributor_eligibility_and_redistributable_terms_pending")
        if item["sources"]:
            item["source_status"] = "exact_local_source_present" if all(value["status"] == "exact_local_source_present" for value in item["sources"]) else "missing_source"
        if item["source_status"] == "missing_source":
            item["remaining"].append("exact_corresponding_source_missing")
        if item["recipe"] and item["recipe"]["status"] != "exact_local_recipe_present":
            item["remaining"].append("exact_historical_recipe_or_modifications_missing")
        if not any(value["content_role"] == "notice_text_present_review_required" for value in notices):
            item["remaining"].append("complete_notice_text_missing")
        if "upstream_asset" in item:
            item["remaining"].append("final_asset_notice_and_redistribution_review_pending")
        else:
            item["remaining"].extend(["build_and_relinking_evidence_not_qualified", "license_branch_and_embedded_dependency_obligations_not_qualified"])
        result.append(item)
    binding = {"inventory": inventory_receipt, **receipts, "binaries": sorted(runtime_files, key=lambda item: item["path"]),
               "build_receipts": build, "upstream_metadata": metadata_receipt, "offline_bundle": bundle}
    report = {"schema_version": 1, "audit_kind": "candidate_dependency_source_closure",
              "candidate_binding": binding, "components": result,
              "licensing_clearance": False, "corresponding_source_qualified": False,
              "boundary": "Exact local receipts and source availability only; no download, legal clearance, complete transitive source claim, source handoff or rebuild qualification.",
              "summary": {"components": len(result), "candidate_binaries": len(runtime_files),
                          "exact_local_source_present": sum(item["source_status"] == "exact_local_source_present" for item in result),
                          "missing_source": sum(item["source_status"] == "missing_source" for item in result),
                          "distributed_asset_recorded": sum(item["source_status"] == "distributed_asset_recorded" for item in result),
                          "redistributable_rights_review": sum(item["source_status"] == "redistributable_rights_review" for item in result)}}
    # Detect drift in the candidate receipts after the bounded audit as well.
    for record in [inventory_receipt, *receipts.values(), *build, *runtime_files, *component_records]:
        if record.get("kind") == "directory":
            require(source_tree_hash(root, record["path"]) == record["sha256"], "source tree hash changed during audit")
        else:
            file_record(root, record["path"], record["sha256"])
    for record in controlled_trees:
        require(source_tree_hash(root, record["path"]) == record["sha256"], "controlled source tree hash changed during audit")
    if bundle:
        for record in [bundle[key] for key in ("manifest", "source_inventory", "runtime_manifest", "source_kit", "portable_manifest")] + bundle["payload"]:
            file_record(root, record["path"], record["sha256"])
    return report


def publish(output: Path, report: dict):
    safe.no_links(output)
    require(output.parent.is_dir(), "output parent must already exist")
    temporary = None
    try:
        with tempfile.NamedTemporaryFile(mode="w", encoding="utf-8", dir=output.parent,
                                         prefix=output.name + ".", suffix=".tmp", delete=False) as stream:
            temporary = Path(stream.name)
            json.dump(report, stream, sort_keys=True, indent=2)
            stream.write("\n")
            stream.flush()
            os.fsync(stream.fileno())
        os.replace(temporary, output)
    finally:
        if temporary:
            temporary.unlink(missing_ok=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--workspace", type=Path, required=True)
    parser.add_argument("--inventory", required=True, help="Workspace-relative candidate distribution inventory.")
    parser.add_argument("--bundle-root", help="Workspace-relative frozen offline package; validate payload and use its application source kit.")
    parser.add_argument("--build-receipt", action="append", default=[], help="Exact workspace-relative build/cache receipt; hashes only.")
    parser.add_argument("--upstream-metadata", help="Workspace-relative official PyPI metadata receipt; no network request runs.")
    parser.add_argument("--cache-dir", action="append", help="Workspace-relative source archive cache; never executed/extracted.")
    parser.add_argument("--output", type=Path, help="Optional explicit report path; its parent must exist.")
    args = parser.parse_args()
    try:
        report = audit(args.workspace, args.inventory, build_receipts=args.build_receipt,
                       metadata_path=args.upstream_metadata, cache_dirs=args.cache_dir or DEFAULT_CACHE_DIRS, bundle_root=args.bundle_root)
        if args.output:
            publish(args.output.absolute(), report)
        print(json.dumps({"summary": report["summary"], "corresponding_source_qualified": False,
                          "licensing_clearance": False}, sort_keys=True))
        return 0
    except (ValueError, OSError, KeyError, TypeError) as error:
        print(f"dependency source closure: {error}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
