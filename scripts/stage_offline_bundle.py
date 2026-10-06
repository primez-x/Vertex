"""Stage a deterministic Windows offline bundle from reviewed local evidence.

This command composes the existing distribution inventory, portable package
allowlist, and source-kit manifest into one self-contained directory.  It
does not build, sign, or certify an installer.  The generated PowerShell
installer copies only the runtime file set after verifying the bundle, and the
generated verifier can re-check the bundle or an installed runtime without a
developer checkout or network access.
"""

from __future__ import annotations

import argparse
import hashlib
import importlib.util
import json
import os
import pathlib
import re
import shutil
import sys
import tempfile
from collections.abc import Mapping
from typing import Any


SCHEMA_VERSION = 1
MANIFEST_VERSION = 1
DEFAULT_DESTINATION = "vertex-offline"
DEFAULT_BUNDLE_MANIFEST = "offline-bundle-manifest.json"
DEFAULT_RUNTIME_MANIFEST = "runtime-manifest.json"
DEFAULT_INSTALLER_NAME = "install-offline-bundle.ps1"
DEFAULT_VERIFIER_NAME = "verify-offline-bundle.ps1"
FIXED_MTIME = 946684800  # 2000-01-01T00:00:00Z; package bytes are time-free.
SHA256_RE = re.compile(r"^[0-9a-fA-F]{64}$")
WINDOWS_ABSOLUTE_RE = re.compile(r"^[A-Za-z]:[\\/]")
MANIFEST_KINDS = {"offline-bundle", "runtime"}


class BundleError(ValueError):
    """Raised when bundle inputs are incomplete, stale, or unsafe."""


def _error(message: str) -> None:
    raise BundleError(message)


def _require(condition: bool, message: str) -> None:
    if not condition:
        _error(message)


def _require_string(value: Any, field: str) -> str:
    _require(isinstance(value, str) and bool(value.strip()),
             f"{field} must be a nonempty string")
    _require("\x00" not in value, f"{field} contains a forbidden NUL")
    return value.strip()


def canonical_relative(value: Any, field: str) -> str:
    """Return a canonical repository/package-relative POSIX path."""

    text = _require_string(value, field)
    _require(not text.startswith(("/", "\\")) and not WINDOWS_ABSOLUTE_RE.match(text),
             f"{field} must be relative")
    normalized = text.replace("\\", "/")
    _require(":" not in normalized, f"{field} contains a forbidden colon")
    path = pathlib.PurePosixPath(normalized)
    _require(path != pathlib.PurePosixPath(".") and bool(path.parts)
             and ".." not in path.parts, f"{field} contains an unsafe path")
    return path.as_posix()


def _validate_sha256(value: Any, field: str) -> str:
    _require(isinstance(value, str) and SHA256_RE.fullmatch(value) is not None,
             f"{field} must be a 64-digit SHA-256 value")
    return value.lower()


def _is_link(path: pathlib.Path) -> bool:
    try:
        if path.is_symlink():
            return True
        is_junction = getattr(path, "is_junction", None)
        return bool(is_junction is not None and is_junction())
    except OSError as exc:
        _error(f"could not inspect path {path}: {exc}")
    return False


def _reject_link_chain(path: pathlib.Path, field: str) -> None:
    for ancestor in (path, *path.parents):
        if _is_link(ancestor):
            _error(f"{field} cannot contain a symlink or junction: {path}")


def _resolve_directory(value: pathlib.Path | str, field: str, *, create: bool = False) -> pathlib.Path:
    candidate = pathlib.Path(value)
    _reject_link_chain(candidate, field)
    if create:
        if candidate.exists() and not candidate.is_dir():
            _error(f"{field} is not a directory: {candidate}")
        try:
            candidate.mkdir(parents=True, exist_ok=True)
            resolved = candidate.resolve(strict=True)
        except (OSError, RuntimeError) as exc:
            _error(f"could not create {field} {candidate}: {exc}")
    else:
        try:
            resolved = candidate.resolve(strict=True)
        except (OSError, RuntimeError) as exc:
            _error(f"{field} is missing: {candidate} ({exc})")
    if not resolved.is_dir():
        _error(f"{field} is not a directory: {candidate}")
    _reject_link_chain(resolved, field)
    return resolved


def _resolve_input_file(root: pathlib.Path, relative: str, field: str) -> pathlib.Path:
    relative = canonical_relative(relative, f"{field}.path")
    candidate = root.joinpath(*pathlib.PurePosixPath(relative).parts)
    _reject_link_chain(candidate, field)
    if not candidate.exists():
        _error(f"{field} missing input: {relative}")
    if not candidate.is_file():
        _error(f"{field} is not a file: {relative}")
    try:
        resolved = candidate.resolve(strict=True)
        resolved.relative_to(root)
    except (OSError, RuntimeError, ValueError) as exc:
        _error(f"{field} must resolve inside the explicit source root: {relative} ({exc})")
    _reject_link_chain(resolved, field)
    return resolved


def _hash_file(path: pathlib.Path) -> tuple[str, int]:
    digest = hashlib.sha256()
    size = 0
    try:
        with path.open("rb") as stream:
            while chunk := stream.read(1024 * 1024):
                digest.update(chunk)
                size += len(chunk)
    except OSError as exc:
        _error(f"could not hash {path}: {exc}")
    return digest.hexdigest(), size


def _read_json(path: pathlib.Path, field: str) -> dict[str, Any]:
    def unique_pairs(pairs: list[tuple[str, Any]]) -> dict[str, Any]:
        result: dict[str, Any] = {}
        for key, value in pairs:
            if key in result:
                _error(f"{field} contains duplicate JSON key: {key}")
            result[key] = value
        return result

    try:
        value = json.loads(path.read_text(encoding="utf-8"), object_pairs_hook=unique_pairs)
    except BundleError:
        raise
    except (OSError, UnicodeError, json.JSONDecodeError) as exc:
        _error(f"malformed {field} JSON at {path}: {exc}")
    _require(isinstance(value, dict), f"{field} JSON must contain an object")
    return value


def _write_json(path: pathlib.Path, value: Mapping[str, Any]) -> None:
    try:
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(json.dumps(value, ensure_ascii=False, indent=2, sort_keys=True) + "\n",
                        encoding="utf-8", newline="\n")
        os.utime(path, (FIXED_MTIME, FIXED_MTIME))
    except OSError as exc:
        _error(f"could not write JSON {path}: {exc}")


def _copy_deterministic(source: pathlib.Path, destination: pathlib.Path, relative: str) -> None:
    _reject_link_chain(destination.parent, f"bundle output {relative}")
    if destination.exists() and _is_link(destination):
        _error(f"bundle output contains a symlink or junction: {relative}")
    try:
        destination.parent.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(source, destination)
        os.utime(destination, (FIXED_MTIME, FIXED_MTIME))
    except OSError as exc:
        _error(f"could not copy {source} to {relative}: {exc}")


def _load_sibling(name: str, filename: str) -> Any:
    path = pathlib.Path(__file__).resolve().parent / filename
    spec = importlib.util.spec_from_file_location(name, path)
    if spec is None or spec.loader is None:
        _error(f"could not load packaging helper {path}")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


_PORTABLE = _load_sibling("stage_portable_package_for_offline_bundle", "stage_portable_package.py")
_SOURCE_KIT = _load_sibling("source_kit_manifest_for_offline_bundle", "source_kit_manifest.py")
_SBOM = _load_sibling("distribution_sbom_for_offline_bundle", "distribution_sbom.py")


def _resolve_rooted_file(value: pathlib.Path | str, root: pathlib.Path, field: str) -> tuple[pathlib.Path, str]:
    candidate = pathlib.Path(value)
    if not candidate.is_absolute():
        relative = canonical_relative(str(candidate), field)
        candidate = root.joinpath(*pathlib.PurePosixPath(relative).parts)
    _reject_link_chain(candidate, field)
    try:
        resolved = candidate.resolve(strict=True)
        relative = resolved.relative_to(root).as_posix()
    except (OSError, RuntimeError, ValueError) as exc:
        _error(f"{field} must resolve to a file inside the explicit source root: {value} ({exc})")
    if not resolved.is_file():
        _error(f"{field} is not a file: {value}")
    return resolved, canonical_relative(relative, field)


def _validate_source_kit(root: pathlib.Path, path: pathlib.Path | str) -> tuple[pathlib.Path, str, dict[str, Any]]:
    manifest_path, relative = _resolve_rooted_file(path, root, "source-kit manifest path")
    manifest = _read_json(manifest_path, "source-kit manifest")
    try:
        _SOURCE_KIT.validate_manifest(manifest)
    except Exception as exc:
        if isinstance(exc, BundleError):
            raise
        _error(f"source-kit manifest is invalid: {exc}")
    for index, entry in enumerate(manifest["files"]):
        if not isinstance(entry, Mapping):
            _error(f"source-kit files[{index}] must be an object")
        item_path = canonical_relative(entry.get("path"), f"source-kit files[{index}].path")
        source = _resolve_input_file(root, item_path, f"source-kit files[{index}]")
        expected_hash = _validate_sha256(entry.get("sha256"), f"source-kit files[{index}].sha256")
        expected_size = entry.get("size")
        _require(isinstance(expected_size, int) and not isinstance(expected_size, bool)
                 and expected_size >= 0, f"source-kit files[{index}].size must be nonnegative")
        actual_hash, actual_size = _hash_file(source)
        _require(actual_hash == expected_hash,
                 f"stale source-kit hash for {item_path}: expected {expected_hash}, received {actual_hash}")
        _require(actual_size == expected_size,
                 f"stale source-kit size for {item_path}: expected {expected_size}, received {actual_size}")
    return manifest_path, relative, manifest


DEPENDENCY_PREFIX = "source-kit/third_party/dependency-inputs"


def _validate_dependency_payload(payload_root: pathlib.Path, source_kit: Mapping[str, Any],
                                 resolve_project: Any, *, helper: Any = None) -> tuple[Any, dict, dict]:
    """Replay every receipt against frozen inputs and selected project sources.

    Replay the composer's metadata schema through a receipt-only adapter. This
    shares its schema checks while resolving copies exclusively from the frozen
    payload, and project references exclusively from the selected source kit.
    """
    if helper is None:
        helper = _load_sibling("dependency_payload_for_offline_bundle",
                               "qualification/compose_dependency_source_kit.py")
    safe, closure = helper.safe, helper.closure
    _require(payload_root.is_dir(), "dependency source kit must be a directory")
    document, receipt = closure.json_input(payload_root, helper.MANIFEST)
    helper.fields(document, {"schema_version", "payload_kind", "source_report", "candidate_binding",
                             "components", "files", "licensing_clearance", "corresponding_source_qualified",
                             "offline_rebuild_qualified", "boundary"},
                  {"schema_version", "payload_kind", "source_report", "candidate_binding", "components",
                   "files", "licensing_clearance", "corresponding_source_qualified", "offline_rebuild_qualified", "boundary"})
    _require(type(document["schema_version"]) is int and document["schema_version"] == 1
             and document["payload_kind"] == "candidate_dependency_source_payload",
             "unsupported dependency source kit schema")
    for key in ("licensing_clearance", "corresponding_source_qualified", "offline_rebuild_qualified"):
        _require(document[key] is False, "dependency source kit cannot confer qualification")
    helper.text(document["boundary"])
    rows = helper.table(document["files"], limit=helper.MAX_PAYLOAD_FILES)
    _require([row["path"] for row in rows] == sorted(row["path"] for row in rows),
             "dependency payload files must be sorted")
    safe.check_names([helper.MANIFEST, *[row["path"] for row in rows]])
    by_path = {}
    total = 0
    for row in rows:
        helper.fields(row, {"source_path", "sha256", "bytes", "sha512", "path", "component_ids", "roles"},
                      {"source_path", "sha256", "bytes", "path", "component_ids", "roles"})
        source = safe.relative_path(row["source_path"])
        _require(row["path"] == "inputs/" + source, "dependency payload source/path differs")
        actual = closure.file_record(payload_root, row["path"], row["sha256"])
        _require(type(row["bytes"]) is int and row["bytes"] == actual["bytes"],
                 "dependency payload bytes differ")
        if "sha512" in row:
            closure.file_record(payload_root, row["path"], row["sha512"], algorithm="sha512")
        total += actual["bytes"]
        _require(total <= helper.MAX_TOTAL, "dependency payload bytes exceed bound")
        for field in ("component_ids", "roles"):
            values = helper.strings(row[field])
            _require(values == sorted(set(values)), "dependency payload owners/roles must be unique and sorted")
        _require(bool(row["roles"]), "dependency payload roles cannot be empty")
        by_path[row["path"]] = row

    # Enumerate every entry, including caches and empty directories. Unlike a
    # source tree, a composed payload has no implicit file exclusions.
    allowed_dirs = {"/".join(path.split("/")[:index]) for path in by_path
                    for index in range(1, len(path.split("/")))}
    actual_files = set()
    entries = 0
    names = {}
    def walk_error(error):
        raise error
    for current, directories, filenames in os.walk(payload_root, followlinks=False, onerror=walk_error):
        entries += len(directories) + len(filenames)
        _require(entries <= helper.MAX_RECORDS, "dependency payload entry count exceeds bound")
        for child in directories + filenames:
            path = pathlib.Path(current) / child
            safe.no_links(path)
            entry = safe.relative_path(path.relative_to(payload_root).as_posix())
            kind = "directory" if child in directories else "file"
            for index in range(1, len(parts := entry.split("/")) + 1):
                spelling = "/".join(parts[:index])
                identity = (spelling, "directory" if index < len(parts) else kind)
                _require(names.get(spelling.casefold(), identity) == identity,
                         "dependency payload case-colliding path")
                names[spelling.casefold()] = identity
            if kind == "file":
                actual_files.add(entry)
    _require(actual_files == {helper.MANIFEST, *by_path}, "dependency payload contains missing or unlisted files")

    project_rows = {safe.relative_path(row["path"]): row for row in source_kit["files"]}
    used = {}
    trees = set()
    record_count = 0

    def restore(value):
        if isinstance(value, list):
            return [restore(item) for item in value]
        if not isinstance(value, dict):
            return value
        result = {key: restore(item) for key, item in value.items()
                  if key not in {"source_path", "payload_path", "project_source_kit_path"}}
        if "source_path" in value:
            result["path"] = value["source_path"]
        return result

    class ReceiptValidator(helper.Composer):
        def record(self, row, owner, role, *, copy=True, project=False, extra=()):
            nonlocal record_count
            helper.fields(row, {"path", "sha256", "sha512", "bytes", "kind", *extra}, ("path", "sha256"))
            record_count += 1
            _require(record_count <= helper.MAX_RECORDS, "dependency receipt count exceeds bound")
            identity = safe.relative_path(row["path"])
            _validate_sha256(row["sha256"], "dependency receipt hash")
            result = {"source_path": identity, "sha256": row["sha256"]}
            if row.get("kind") == "directory":
                _require(role in {"source", "sdk_source"} and not ({"bytes", "sha512"} & set(row)),
                         "invalid dependency directory receipt")
                result["kind"] = "directory"
                if project:
                    children = sorted(path for path in project_rows if path.startswith(identity + "/"))
                    digest = hashlib.sha256(b"Vertex-source-tree-v2\0")
                    digest.update(len(children).to_bytes(8, "big"))
                    for child in children:
                        name = child[len(identity) + 1:].encode("utf-8")
                        actual_hash, actual_bytes = _hash_file(resolve_project(child))
                        bound = project_rows[child]
                        _require(actual_hash == bound["sha256"] and actual_bytes == bound["size"],
                                 "dependency project tree file differs from selected source kit")
                        digest.update(len(name).to_bytes(8, "big"))
                        digest.update(name)
                        digest.update(actual_bytes.to_bytes(8, "big"))
                        digest.update(bytes.fromhex(actual_hash))
                    _require(digest.hexdigest() == row["sha256"],
                             "dependency project tree differs from selected source kit")
                    result["project_source_kit_path"] = identity
                else:
                    location = "inputs/" + identity
                    trees.add(location)
                    _require(closure.source_tree_hash(payload_root, location) == row["sha256"],
                             "dependency payload tree hash differs")
                    result["payload_path"] = location
                    for child, bound in by_path.items():
                        if child.startswith(location + "/"):
                            self.record({"path": bound["source_path"], "sha256": bound["sha256"],
                                         "bytes": bound["bytes"]}, owner, role)
                return result
            _require(row.get("kind", "file") == "file" and type(row.get("bytes")) is int
                     and 0 <= row["bytes"] <= closure.MAX_FILE, "invalid dependency file receipt")
            result["bytes"] = row["bytes"]
            if "sha512" in row:
                _require(isinstance(row["sha512"], str) and re.fullmatch(r"[0-9a-f]{128}", row["sha512"]),
                         "invalid dependency SHA512 receipt")
                result["sha512"] = row["sha512"]
            if project:
                bound = project_rows.get(identity)
                _require(bound is not None and bound["sha256"] == row["sha256"] and bound["size"] == row["bytes"],
                         "dependency project file differs from selected source kit")
                project_file = resolve_project(identity)
                actual_hash, actual_bytes = _hash_file(project_file)
                _require(actual_hash == row["sha256"] and actual_bytes == row["bytes"],
                         "dependency project file differs from selected source kit")
                if "sha512" in row:
                    closure.file_record(project_file.parent, project_file.name, row["sha512"], algorithm="sha512")
                result["project_source_kit_path"] = identity
            elif copy:
                location = "inputs/" + identity
                bound = by_path.get(location)
                _require(bound is not None and all(bound.get(key) == result.get(key)
                                                   for key in ("sha256", "bytes")),
                         "dependency receipt differs from declared payload")
                if "sha512" in row:
                    closure.file_record(payload_root, location, row["sha512"], algorithm="sha512")
                result["payload_path"] = location
                owners, roles = used.setdefault(location, (set(), set()))
                if owner is not None:
                    owners.add(owner)
                roles.add(role)
            return result

        def binding(self, binding, owners):
            binding = dict(binding)
            direct = binding.pop("source_kit", None)
            result = super().binding(binding, owners)
            # Portable workspace receipts already use their original identities.
            self.project_prefix = None
            if direct is not None:
                result["source_kit"] = self.record(direct, None, "source_kit")
            return result

    components = document["components"]
    _require(isinstance(components, list) and 0 < len(components) <= 512, "invalid dependency component table")
    owners = [helper.fields(row, row.keys(), ("id",))["id"] for row in components if isinstance(row, dict)]
    _require(len(owners) == len(components) and all(isinstance(owner, str) and
             re.fullmatch(r"[a-z0-9][a-z0-9-]*", owner) for owner in owners)
             and owners == sorted(set(owners)), "invalid dependency component IDs/order")
    validator = ReceiptValidator(payload_root, payload_root.parent / ".unused-validator-output")
    _require(validator.record(restore(document["source_report"]), None, "source_report", copy=False)
             == document["source_report"], "invalid dependency source report receipt")
    binding = validator.binding(restore(document["candidate_binding"]), owners)
    _require(binding == document["candidate_binding"], "invalid dependency candidate binding schema")
    for item in components:
        _require(validator.component(restore(item)) == item, "invalid dependency component schema")
    for path, row in by_path.items():
        admitted = used.get(path)
        _require(admitted is not None and row["component_ids"] == sorted(admitted[0])
                 and row["roles"] == sorted(admitted[1]), "dependency payload has unbound files/owners/roles")
    allowed_dirs.update("/".join(path.split("/")[:index]) for path in trees
                        for index in range(1, len(path.split("/")) + 1))
    for spelling, kind in names.values():
        if kind == "directory":
            _require(spelling in allowed_dirs or spelling in trees,
                     "dependency payload contains an unlisted directory")
    return helper, document, receipt


def _validate_dependency_source_kit(root: pathlib.Path, relative: pathlib.Path | str,
                                    inventory_relative: str, source_kit_file: pathlib.Path,
                                    source_kit: Mapping[str, Any]) -> tuple[Any, pathlib.Path, dict, dict]:
    """Validate the shared frozen receipt graph against workspace selections."""
    helper = _load_sibling("dependency_payload_for_offline_bundle",
                           "qualification/compose_dependency_source_kit.py")
    name = helper.safe.relative_path(relative.as_posix() if isinstance(relative, pathlib.PurePath) else relative)
    payload_root = helper.closure.checked_path(root, name)
    helper, document, receipt = _validate_dependency_payload(
        payload_root, source_kit,
        lambda identity: _resolve_input_file(root, identity, "dependency project reference"), helper=helper)
    safe, closure = helper.safe, helper.closure
    binding = document["candidate_binding"]
    owners = [row["id"] for row in document["components"]]
    selected_inventory = closure.file_record(root, inventory_relative)
    _require(binding["inventory"] == {"source_path": inventory_relative,
             "sha256": selected_inventory["sha256"], "bytes": selected_inventory["bytes"],
             "payload_path": "inputs/" + inventory_relative},
             "dependency inventory binding differs from selected inventory")
    frozen = binding["offline_bundle"]
    direct = binding.get("source_kit")
    _require(frozen is not None or direct is not None,
             "dependency source kit requires a source kit candidate binding")
    if direct is not None:
        selected_relative = source_kit_file.relative_to(root).as_posix()
        selected_hash, selected_bytes = _hash_file(source_kit_file)
        _require(direct == {"source_path": selected_relative, "sha256": selected_hash,
                           "bytes": selected_bytes, "payload_path": "inputs/" + selected_relative},
                 "dependency source kit binding differs from selected source kit")
    if frozen is not None:
        selected_hash, selected_bytes = _hash_file(source_kit_file)
        _require(frozen["source_kit"]["sha256"] == selected_hash
                 and frozen["source_kit"]["bytes"] == selected_bytes,
                 "dependency source kit binding differs from selected source kit")
        _require(frozen["source_inventory"]["sha256"] == selected_inventory["sha256"]
                 and frozen["source_inventory"]["bytes"] == selected_inventory["bytes"],
                 "dependency frozen inventory binding differs from selected inventory")
    inventory, _ = closure.json_input(root, inventory_relative, selected_inventory["sha256"])
    _require(set(owners) == {row["id"] for row in inventory["components"]},
             "dependency components differ from selected inventory")
    for key in ("component_manifest", "runtime"):
        evidence = inventory.get("evidence", {}).get(key)
        if evidence is not None:
            _require(binding[key]["source_path"] == evidence["path"]
                     and binding[key]["sha256"] == evidence["sha256"],
                     "dependency candidate evidence differs from selected inventory")
    expected_binaries = []
    if frozen is not None:
        _require(frozen["source_location"].endswith("/source-kit"), "invalid frozen project source location")
    for row in inventory["binaries"]:
        original = safe.relative_path(row["path"])
        expected_binaries.append((row["component_id"], original, row["sha256"], _hash_file(
            _resolve_input_file(root, original, "dependency selected binary"))[1]))
    actual_binaries = []
    for row in binding["binaries"]:
        original = row["source_path"]
        actual_binaries.append((row["component_id"], original, row["sha256"], row["bytes"]))
    _require(sorted(actual_binaries) == sorted(expected_binaries),
             "dependency binary binding differs from selected inventory")
    return helper, payload_root, document, receipt


def _dependency_tree_paths(value: Any) -> set[str]:
    """Declared directory receipts, including empty source trees."""
    if isinstance(value, list):
        return set().union(*(_dependency_tree_paths(item) for item in value))
    if not isinstance(value, dict):
        return set()
    paths = set().union(*(_dependency_tree_paths(item) for item in value.values()))
    if value.get("kind") == "directory" and "payload_path" in value:
        paths.add(value["payload_path"])
    return paths


def _inventory_license_rows(inventory: Mapping[str, Any]) -> list[dict[str, Any]]:
    rows: list[dict[str, Any]] = []
    components = inventory.get("components")
    _require(isinstance(components, list), "distribution inventory components must be a list")
    for index, component in enumerate(components):
        _require(isinstance(component, Mapping), f"distribution inventory components[{index}] must be an object")
        identifier = _require_string(component.get("id"), f"distribution inventory components[{index}].id")
        package = component.get("package")
        _require(isinstance(package, Mapping), f"distribution inventory components[{index}].package must be an object")
        notices = component.get("notices", [])
        _require(isinstance(notices, list), f"distribution inventory components[{index}].notices must be a list")
        notice_rows: list[dict[str, str]] = []
        for notice_index, notice in enumerate(notices):
            _require(isinstance(notice, Mapping), f"distribution inventory components[{index}].notices[{notice_index}] must be an object")
            notice_path = canonical_relative(notice.get("path"), f"distribution inventory components[{index}].notices[{notice_index}].path")
            notice_hash = _validate_sha256(notice.get("sha256"), f"distribution inventory components[{index}].notices[{notice_index}].sha256")
            notice_rows.append({"path": notice_path, "sha256": notice_hash})
        notice_rows.sort(key=lambda row: (row["path"].casefold(), row["path"]))
        rows.append({
            "component_id": identifier,
            "name": _require_string(package.get("name"), f"distribution inventory components[{index}].package.name"),
            "version": _require_string(package.get("version"), f"distribution inventory components[{index}].package.version"),
            "license": _require_string(package.get("license"), f"distribution inventory components[{index}].package.license"),
            "distribution_status": _require_string(component.get("distribution_status", "included"),
                                                     f"distribution inventory components[{index}].distribution_status"),
            "notices": notice_rows,
        })
    rows.sort(key=lambda row: (row["component_id"].casefold(), row["component_id"]))
    return rows


def _inventory_dependency_rows(inventory: Mapping[str, Any]) -> dict[str, Any]:
    binaries = inventory.get("binaries")
    _require(isinstance(binaries, list), "distribution inventory binaries must be a list")
    runtime: list[dict[str, Any]] = []
    for index, item in enumerate(binaries):
        _require(isinstance(item, Mapping), f"distribution inventory binaries[{index}] must be an object")
        runtime.append({
            "name": _require_string(item.get("name"), f"distribution inventory binaries[{index}].name"),
            "path": canonical_relative(item.get("path"), f"distribution inventory binaries[{index}].path"),
            "destination": canonical_relative(item.get("destination"), f"distribution inventory binaries[{index}].destination"),
            "sha256": _validate_sha256(item.get("sha256"), f"distribution inventory binaries[{index}].sha256"),
            "component_id": _require_string(item.get("component_id"), f"distribution inventory binaries[{index}].component_id"),
        })
    runtime.sort(key=lambda row: (row["destination"].casefold(), row["destination"]))
    static_rows: list[dict[str, Any]] = []
    static = inventory.get("static_inputs", [])
    _require(isinstance(static, list), "distribution inventory static_inputs must be a list")
    for index, item in enumerate(static):
        _require(isinstance(item, Mapping), f"distribution inventory static_inputs[{index}] must be an object")
        component_id = _require_string(item.get("component_id"), f"distribution inventory static_inputs[{index}].component_id")
        source_inputs = item.get("source_inputs", [])
        _require(isinstance(source_inputs, list), f"distribution inventory static_inputs[{index}].source_inputs must be a list")
        static_inputs: list[dict[str, str]] = []
        for source_index, source in enumerate(source_inputs):
            _require(isinstance(source, Mapping), f"distribution inventory static_inputs[{index}].source_inputs[{source_index}] must be an object")
            static_inputs.append({
                "path": canonical_relative(source.get("path"), f"distribution inventory static_inputs[{index}].source_inputs[{source_index}].path"),
                "sha256": _validate_sha256(source.get("sha256"), f"distribution inventory static_inputs[{index}].source_inputs[{source_index}].sha256"),
            })
        static_inputs.sort(key=lambda row: (row["path"].casefold(), row["path"]))
        package = item.get("package", {})
        _require(isinstance(package, Mapping),
                 f"distribution inventory static_inputs[{index}].package must be an object")
        package_summary = {
            "name": _require_string(package.get("name"),
                                     f"distribution inventory static_inputs[{index}].package.name"),
            "version": _require_string(package.get("version"),
                                        f"distribution inventory static_inputs[{index}].package.version"),
            "license": _require_string(package.get("license"),
                                        f"distribution inventory static_inputs[{index}].package.license"),
        }
        static_rows.append({
            "component_id": component_id,
            "kind": _require_string(item.get("kind"), f"distribution inventory static_inputs[{index}].kind"),
            "distribution_status": _require_string(item.get("distribution_status", "included"), f"distribution inventory static_inputs[{index}].distribution_status"),
            "package": package_summary,
            "source_inputs": static_inputs,
        })
    static_rows.sort(key=lambda row: (row["component_id"].casefold(), row["component_id"]))
    imports = inventory.get("runtime_imports", [])
    _require(isinstance(imports, list), "distribution inventory runtime_imports must be a list")
    normalized_imports: list[dict[str, Any]] = []
    for index, item in enumerate(imports):
        _require(isinstance(item, Mapping), f"distribution inventory runtime_imports[{index}] must be an object")
        normalized: dict[str, Any] = {}
        for key in ("from", "to", "name", "component_id"):
            if key in item:
                normalized[key] = _require_string(
                    item[key], f"distribution inventory runtime_imports[{index}].{key}")
        for key in ("resolved_path", "destination"):
            if key in item:
                normalized[key] = canonical_relative(
                    item[key], f"distribution inventory runtime_imports[{index}].{key}")
        if "sha256" in item:
            normalized["sha256"] = _validate_sha256(
                item["sha256"], f"distribution inventory runtime_imports[{index}].sha256")
        normalized["kind"] = _require_string(
            item.get("kind"), f"distribution inventory runtime_imports[{index}].kind")
        normalized_imports.append(normalized)
    normalized_imports.sort(key=lambda row: json.dumps(row, sort_keys=True, separators=(",", ":")))
    system = inventory.get("system_runtime_imports", [])
    contracts = inventory.get("windows_api_contracts", [])
    _require(isinstance(system, list), "distribution inventory system_runtime_imports must be a list")
    _require(not any(isinstance(name, str) and re.match(
        r"^(?:msvcp|vcruntime|concrt|vccorlib)\d.*\.dll$", name, re.IGNORECASE) for name in system),
        "offline bundle must carry its Visual C++ runtime; installed system CRT is not a Windows OS boundary")
    _require(isinstance(contracts, list), "distribution inventory windows_api_contracts must be a list")
    return {
        "runtime": runtime,
        "static": static_rows,
        "imports": normalized_imports,
        "system_runtime_imports": sorted(
            {_require_string(name, "distribution inventory system runtime import") for name in system},
            key=str.casefold,
        ),
        "windows_api_contracts": sorted(
            {_require_string(name, "distribution inventory Windows API contract") for name in contracts},
            key=str.casefold,
        ),
    }


def _file_record(path: pathlib.Path, relative: str, *, kind: str, role: str,
                 component_id: str | None = None, category: str | None = None,
                 install: bool = False) -> dict[str, Any]:
    digest, size = _hash_file(path)
    record: dict[str, Any] = {
        "path": canonical_relative(relative, "bundle file path"),
        "sha256": digest,
        "size": size,
        "kind": kind,
        "role": role,
        "install": bool(install),
    }
    if component_id is not None:
        record["component_id"] = component_id
    if category is not None:
        record["category"] = category
    return record


def _sorted_file_records(records: list[dict[str, Any]]) -> list[dict[str, Any]]:
    return sorted(records, key=lambda row: (row["path"].casefold(), row["path"]))


def _validate_file_records(records: list[dict[str, Any]]) -> None:
    seen: set[str] = set()
    previous: tuple[str, str] | None = None
    for index, record in enumerate(records):
        field = f"bundle files[{index}]"
        _require(isinstance(record, Mapping), f"{field} must be an object")
        path = canonical_relative(record.get("path"), f"{field}.path")
        key = path.casefold()
        _require(key not in seen, f"bundle contains duplicate file path: {path}")
        seen.add(key)
        sort_key = (key, path)
        _require(previous is None or sort_key >= previous,
                 "bundle files are not deterministically ordered")
        previous = sort_key
        _validate_sha256(record.get("sha256"), f"{field}.sha256")
        size = record.get("size")
        _require(isinstance(size, int) and not isinstance(size, bool) and size >= 0,
                 f"{field}.size must be a nonnegative integer")
        _require(isinstance(record.get("install"), bool), f"{field}.install must be boolean")


def _validate_manifest_shape(manifest: Mapping[str, Any]) -> str:
    _require(manifest.get("schema_version") == SCHEMA_VERSION,
             f"unsupported bundle manifest schema_version: {manifest.get('schema_version')!r}")
    _require(manifest.get("manifest_version") == MANIFEST_VERSION,
             f"unsupported bundle manifest manifest_version: {manifest.get('manifest_version')!r}")
    kind = _require_string(manifest.get("manifest_kind"), "bundle manifest_kind")
    _require(kind in MANIFEST_KINDS, f"unsupported bundle manifest kind: {kind!r}")
    _require(manifest.get("audit_status") == "incomplete",
             "bundle audit_status must remain 'incomplete'")
    if "installer_qualified" in manifest:
        _require(manifest["installer_qualified"] is False,
                 "bundle cannot claim installer qualification")
    if "offline_qualified" in manifest:
        _require(manifest["offline_qualified"] is False,
                 "bundle cannot claim offline qualification")
    qualification = manifest.get("qualification")
    if qualification is not None:
        _require(isinstance(qualification, Mapping), "bundle qualification must be an object")
        _require(qualification.get("installer_qualified") is False,
                 "bundle qualification cannot claim installer qualification")
        _require(qualification.get("offline_qualified") is False,
                 "bundle qualification cannot claim offline qualification")
    files = manifest.get("files")
    _require(isinstance(files, list) and bool(files), "bundle files must be a nonempty list")
    _validate_file_records(files)
    return kind


def _file_identity(record: Mapping[str, Any]) -> tuple[str, str, int, str, str | None]:
    return (
        canonical_relative(record.get("path"), "runtime manifest file path").casefold(),
        _validate_sha256(record.get("sha256"), "runtime manifest file hash"),
        record.get("size"),
        _require_string(record.get("kind"), "runtime manifest file kind"),
        record.get("component_id"),
    )


def _validate_runtime_manifest_consistency(root: pathlib.Path,
                                           manifest: Mapping[str, Any]) -> None:
    """Ensure the bundle's install list matches its separately hashed runtime manifest."""

    if manifest.get("manifest_kind") != "offline-bundle":
        return
    runtime_meta = manifest.get("runtime_manifest")
    _require(isinstance(runtime_meta, Mapping),
             "offline bundle runtime_manifest metadata must be an object")
    runtime_relative = canonical_relative(runtime_meta.get("path"),
                                          "offline bundle runtime manifest path")
    runtime_path = _resolve_input_file(root, runtime_relative,
                                       "offline bundle runtime manifest")
    expected_hash = _validate_sha256(runtime_meta.get("sha256"),
                                     "offline bundle runtime manifest hash")
    actual_hash, _ = _hash_file(runtime_path)
    _require(actual_hash == expected_hash,
             "offline bundle runtime manifest hash does not match metadata")
    runtime = _read_json(runtime_path, "runtime manifest")
    _require(_validate_manifest_shape(runtime) == "runtime",
             "offline bundle runtime manifest must have manifest_kind 'runtime'")
    expected = [record for record in manifest["files"] if record.get("install") is True]
    actual = runtime["files"]
    _require(
        [_file_identity(record) for record in actual]
        == [_file_identity(record) for record in expected],
        "runtime manifest install list does not match bundle files",
    )


def _validate_bundle_references(manifest: Mapping[str, Any]) -> None:
    """Ensure top-level evidence references agree with their file records."""

    files = manifest["files"]
    by_path = {canonical_relative(row["path"], "bundle file path").casefold(): row
               for row in files}
    for field in ("source_inventory", "source_kit", "runtime_manifest", "sbom", "dependency_source_kit"):
        reference = manifest.get(field)
        if reference is None:
            continue
        _require(isinstance(reference, Mapping), f"bundle {field} reference must be an object")
        relative = canonical_relative(reference.get("path"), f"bundle {field} path")
        expected = _validate_sha256(reference.get("sha256"), f"bundle {field} hash")
        record = by_path.get(relative.casefold())
        _require(record is not None, f"bundle {field} has no file record: {relative}")
        _require(record["sha256"].lower() == expected,
                 f"bundle {field} hash does not match its file record: {relative}")
        if field == "sbom":
            _require(reference.get("format") == "SPDX-2.3",
                     "bundle sbom format must be SPDX-2.3")
        if field == "dependency_source_kit":
            _require(type(reference.get("size")) is int and reference["size"] == record["size"]
                     and record.get("install") is False and record.get("kind") == "dependency-source"
                     and record.get("category") == "dependency-source",
                     "bundle dependency source kit reference differs from its source file record")


def _validate_sbom_reference(root: pathlib.Path, manifest: Mapping[str, Any]) -> None:
    reference = manifest.get("sbom")
    if reference is None:
        return
    _require(isinstance(reference, Mapping), "bundle sbom reference must be an object")
    relative = canonical_relative(reference.get("path"), "bundle sbom path")
    path = _resolve_input_file(root, relative, "bundle sbom")
    document = _read_json(path, "bundle SPDX SBOM")
    try:
        _SBOM.validate_sbom(document)
    except (ValueError, RuntimeError) as exc:
        _error(f"bundle SPDX SBOM is invalid: {exc}")
    expected = _validate_sha256(reference.get("sha256"), "bundle sbom hash")
    actual, _ = _hash_file(path)
    _require(actual == expected, "bundle sbom hash does not match its file")


def verify_bundle(bundle_root: pathlib.Path | str,
                  manifest_name: pathlib.Path | str = DEFAULT_BUNDLE_MANIFEST,
                  *, strict_files: bool | None = None) -> dict[str, Any]:
    """Verify one bundle/runtime manifest and all declared file bytes.

    Offline bundle manifests are strict by default: every regular file under
    the root must be listed, allowing only the manifest itself as metadata.
    Runtime manifests are open to user-created project/configuration files and
    verify only the declared runtime set.
    """

    root = _resolve_directory(bundle_root, "bundle root")
    manifest_relative = canonical_relative(manifest_name, "bundle manifest name")
    manifest_path = root.joinpath(*pathlib.PurePosixPath(manifest_relative).parts)
    _reject_link_chain(manifest_path, "bundle manifest path")
    if not manifest_path.is_file():
        _error(f"bundle manifest is missing: {manifest_relative}")
    manifest = _read_json(manifest_path, "bundle manifest")
    kind = _validate_manifest_shape(manifest)
    _validate_bundle_references(manifest)
    _validate_sbom_reference(root, manifest)
    _validate_runtime_manifest_consistency(root, manifest)
    if strict_files is None:
        strict_files = kind == "offline-bundle"
    declared: set[str] = set()
    for index, record in enumerate(manifest["files"]):
        relative = canonical_relative(record["path"], f"bundle files[{index}].path")
        declared.add(relative.casefold())
        selected = _resolve_input_file(root, relative, f"bundle files[{index}]")
        actual_hash, actual_size = _hash_file(selected)
        expected_hash = _validate_sha256(record["sha256"], f"bundle files[{index}].sha256")
        _require(actual_hash == expected_hash,
                 f"bundle hash mismatch for {relative}: expected {expected_hash}, received {actual_hash}")
        _require(actual_size == record["size"],
                 f"bundle size mismatch for {relative}: expected {record['size']}, received {actual_size}")
    if strict_files:
        allowed = declared | {manifest_relative.casefold()}
        for candidate in root.rglob("*"):
            _reject_link_chain(candidate, "bundle root")
            if candidate.is_file():
                relative = candidate.relative_to(root).as_posix().casefold()
                _require(relative in allowed,
                         f"bundle contains an unlisted file: {candidate.relative_to(root).as_posix()}")
    return {"file_count": len(manifest["files"]), "manifest_kind": kind}


def _load_portable_manifest(path: pathlib.Path) -> dict[str, Any]:
    manifest = _read_json(path, "portable package manifest")
    files = manifest.get("files")
    _require(isinstance(files, list) and bool(files), "portable package manifest files must be nonempty")
    for index, row in enumerate(files):
        _require(isinstance(row, Mapping), f"portable package files[{index}] must be an object")
        canonical_relative(row.get("path"), f"portable package files[{index}].path")
        _validate_sha256(row.get("sha256"), f"portable package files[{index}].sha256")
    return manifest


def stage_bundle(
    inventory_path: pathlib.Path | str,
    allowlist_path: pathlib.Path | str,
    source_kit_manifest_path: pathlib.Path | str,
    source_root: pathlib.Path | str,
    output_root: pathlib.Path | str,
    destination: pathlib.Path | str = DEFAULT_DESTINATION,
    *,
    installer_template: pathlib.Path | str | None = None,
    verifier_template: pathlib.Path | str | None = None,
    installer_name: pathlib.Path | str = DEFAULT_INSTALLER_NAME,
    verifier_name: pathlib.Path | str = DEFAULT_VERIFIER_NAME,
    dependency_source_kit: pathlib.Path | str | None = None,
) -> dict[str, Any]:
    """Stage and verify an offline installer bundle atomically enough for local use."""

    root = _resolve_directory(source_root, "source root")
    output = _resolve_directory(output_root, "output root", create=True)
    package_relative = canonical_relative(destination, "bundle destination")
    package_root = output.joinpath(*pathlib.PurePosixPath(package_relative).parts)
    _reject_link_chain(package_root, "bundle destination")
    _require(not package_root.exists(),
             f"bundle destination already exists: {package_relative}; choose a new empty destination")

    inventory_file, inventory_relative = _resolve_rooted_file(inventory_path, root, "distribution inventory path")
    source_kit_file, source_kit_relative, source_kit = _validate_source_kit(root, source_kit_manifest_path)
    inventory = _read_json(inventory_file, "distribution inventory")
    inventory_qualified = inventory.get("distribution_qualified", False)
    _require(inventory.get("audit_status") == "incomplete",
             "distribution inventory audit_status must remain 'incomplete'")
    _require(inventory_qualified is False, "distribution inventory cannot claim qualification")
    dependency_payload = None
    if dependency_source_kit is not None:
        try:
            dependency_payload = _validate_dependency_source_kit(
                root, dependency_source_kit, inventory_relative, source_kit_file, source_kit)
        except (ValueError, OSError, KeyError, TypeError) as exc:
            _error(f"invalid dependency source kit: {exc}")

    installer_path = pathlib.Path(installer_template) if installer_template is not None else pathlib.Path(__file__).resolve().with_name(DEFAULT_INSTALLER_NAME)
    verifier_path = pathlib.Path(verifier_template) if verifier_template is not None else pathlib.Path(__file__).resolve().with_name(DEFAULT_VERIFIER_NAME)
    _reject_link_chain(installer_path, "installer template path")
    _reject_link_chain(verifier_path, "verifier template path")
    try:
        installer_bytes = installer_path.read_bytes()
        verifier_bytes = verifier_path.read_bytes()
    except OSError as exc:
        _error(f"could not read installer/verifier template: {exc}")
    _require(installer_bytes and verifier_bytes, "installer and verifier templates must be nonempty")

    installer_relative = canonical_relative(installer_name, "installer file name")
    verifier_relative = canonical_relative(verifier_name, "verifier file name")
    _require(installer_relative == DEFAULT_INSTALLER_NAME,
             f"installer file name must be {DEFAULT_INSTALLER_NAME}")
    _require(verifier_relative == DEFAULT_VERIFIER_NAME,
             f"verifier file name must be {DEFAULT_VERIFIER_NAME}")
    _require(installer_relative.casefold() != verifier_relative.casefold(),
             "installer and verifier file names must be distinct")
    _require(installer_relative.casefold() != DEFAULT_BUNDLE_MANIFEST.casefold()
             and verifier_relative.casefold() != DEFAULT_BUNDLE_MANIFEST.casefold(),
             "installer/verifier cannot replace the bundle manifest")

    with tempfile.TemporaryDirectory(prefix=".offline-bundle-", dir=output) as temporary:
        staging = pathlib.Path(temporary)
        portable_parent = staging / ".portable"
        portable_manifest = _PORTABLE.stage_package(
            inventory_file,
            allowlist_path,
            root,
            portable_parent,
            "runtime",
        )
        portable_root = portable_parent / "runtime"
        portable_manifest_path = portable_root / "portable-package-manifest.json"
        _require(portable_manifest_path.is_file(), "portable stager did not produce its manifest")
        loaded_portable = _load_portable_manifest(portable_manifest_path)
        _require(loaded_portable == portable_manifest,
                 "portable package manifest changed before bundle composition")

        runtime_records: list[dict[str, Any]] = []
        for index, row in enumerate(portable_manifest["files"]):
            relative = canonical_relative(row.get("path"), f"portable package files[{index}].path")
            source = _resolve_input_file(portable_root, relative, f"portable package files[{index}]")
            destination_path = staging.joinpath(*pathlib.PurePosixPath(relative).parts)
            _copy_deterministic(source, destination_path, relative)
            actual_hash, size = _hash_file(destination_path)
            expected = _validate_sha256(row.get("sha256"), f"portable package files[{index}].sha256")
            _require(actual_hash == expected,
                     f"portable package hash changed while composing bundle: {relative}")
            is_sbom = row.get("kind") == "sbom"
            runtime_records.append({
                "path": relative,
                "sha256": actual_hash,
                "size": size,
                "kind": _require_string(row.get("kind"), f"portable package files[{index}].kind"),
                "role": "metadata" if is_sbom else ("license" if row.get("kind") == "notice" else "runtime"),
                "install": not is_sbom,
                "component_id": _require_string(row.get("component_id"), f"portable package files[{index}].component_id"),
            })
        runtime_records = _sorted_file_records(runtime_records)

        metadata_specs = [
            (inventory_file, "metadata/distribution-inventory.json", "distribution-inventory"),
            (source_kit_file, "metadata/source-kit-manifest.json", "source-kit-manifest"),
            (portable_manifest_path, "metadata/portable-package-manifest.json", "portable-package-manifest"),
        ]
        metadata_records: list[dict[str, Any]] = []
        for source, relative, kind in metadata_specs:
            destination_path = staging.joinpath(*pathlib.PurePosixPath(relative).parts)
            _copy_deterministic(source, destination_path, relative)
            metadata_records.append(_file_record(destination_path, relative, kind=kind,
                                                 role="metadata", install=False))
        # The portable stager is an implementation detail. Remove its private
        # staging tree before strict verification so no unlisted developer
        # paths can accidentally enter the published bundle.
        try:
            shutil.rmtree(portable_parent)
        except OSError as exc:
            _error(f"could not remove temporary portable staging tree: {exc}")

        source_records: list[dict[str, Any]] = []
        for index, entry in enumerate(source_kit["files"]):
            relative = canonical_relative(entry.get("path"), f"source-kit files[{index}].path")
            source = _resolve_input_file(root, relative, f"source-kit files[{index}]")
            destination_relative = f"source-kit/{relative}"
            destination_path = staging.joinpath(*pathlib.PurePosixPath(destination_relative).parts)
            _copy_deterministic(source, destination_path, destination_relative)
            expected_hash = _validate_sha256(entry.get("sha256"), f"source-kit files[{index}].sha256")
            actual_hash, size = _hash_file(destination_path)
            _require(actual_hash == expected_hash,
                     f"source-kit hash changed while composing bundle: {relative}")
            _require(size == entry.get("size"), f"source-kit size changed while composing bundle: {relative}")
            source_records.append(_file_record(
                destination_path,
                destination_relative,
                kind="source-kit",
                role="source-kit",
                category=_require_string(entry.get("category"), f"source-kit files[{index}].category"),
                install=False,
            ))
        source_records = _sorted_file_records(source_records)

        dependency_manifest_record = None
        if dependency_payload is not None:
            helper, payload_root, dependency_document, dependency_receipt = dependency_payload
            # Reserve this directory; project sources must never overwrite the
            # bound dependency receipt or any of its frozen input files.
            _require(not (staging / DEPENDENCY_PREFIX).exists(),
                     "project source kit overlaps dependency input destination")
            for directory in sorted(_dependency_tree_paths(dependency_document)):
                (staging / DEPENDENCY_PREFIX / directory).mkdir(parents=True, exist_ok=True)
            for row in [dependency_receipt, *dependency_document["files"]]:
                relative = DEPENDENCY_PREFIX + "/" + row["path"]
                target = staging / relative
                helper.copy_file(payload_root, {key: row[key] for key in ("path", "sha256", "bytes")}, target)
                os.utime(target, (FIXED_MTIME, FIXED_MTIME))
                record = _file_record(target, relative, kind="dependency-source",
                                      role="source-kit", category="dependency-source", install=False)
                _require(record["sha256"] == row["sha256"] and record["size"] == row["bytes"],
                         "dependency payload changed while composing bundle")
                source_records.append(record)
                if row["path"] == helper.MANIFEST:
                    dependency_manifest_record = record
            # Revalidate after copying to reject concurrent drift, additions,
            # or relabeling of receipts during staging.
            try:
                checked = _validate_dependency_source_kit(
                    root, dependency_source_kit, inventory_relative, source_kit_file, source_kit)
                _require(checked[2:] == dependency_payload[2:], "dependency manifest changed during staging")
            except (ValueError, OSError, KeyError, TypeError) as exc:
                _error(f"dependency source kit changed during staging: {exc}")
            source_records = _sorted_file_records(source_records)

        installed_runtime_records = [row for row in runtime_records if row["install"]]
        sbom_records = [row for row in runtime_records if row["kind"] == "sbom"]
        _require(len(sbom_records) == 1, "portable package must contain exactly one SPDX SBOM")
        sbom_record = sbom_records[0]
        runtime_manifest: dict[str, Any] = {
            "schema_version": SCHEMA_VERSION,
            "manifest_version": MANIFEST_VERSION,
            "manifest_kind": "runtime",
            "audit_status": "incomplete",
            "installer_qualified": False,
            "offline_qualified": False,
            "qualification": {"installer_qualified": False, "offline_qualified": False},
            "files": installed_runtime_records,
            "summary": {
                "file_count": len(installed_runtime_records),
                "binary_count": sum(row["kind"] == "binary" for row in installed_runtime_records),
                "license_file_count": sum(row["kind"] == "notice" for row in installed_runtime_records),
                "installer_qualified": False,
                "offline_qualified": False,
            },
            "boundary": (
                "This runtime manifest verifies declared installed bytes only. It does not prove "
                "clean-machine installation, offline operation, dynamic-load completeness, licensing, "
                "or redistributability."
            ),
        }
        _write_json(staging / DEFAULT_RUNTIME_MANIFEST, runtime_manifest)
        runtime_manifest_record = _file_record(staging / DEFAULT_RUNTIME_MANIFEST,
                                               DEFAULT_RUNTIME_MANIFEST,
                                               kind="runtime-manifest", role="metadata", install=False)

        _copy_deterministic(installer_path, staging / installer_relative, installer_relative)
        _copy_deterministic(verifier_path, staging / verifier_relative, verifier_relative)
        installer_record = _file_record(staging / installer_relative, installer_relative,
                                         kind="installer-script", role="installer", install=False)
        verifier_record = _file_record(staging / verifier_relative, verifier_relative,
                                       kind="verifier-script", role="verifier", install=False)

        bundle_files = _sorted_file_records(
            runtime_records + source_records + metadata_records
            + [runtime_manifest_record, installer_record, verifier_record]
        )
        _validate_file_records(bundle_files)
        dependencies = _inventory_dependency_rows(inventory)
        licenses = _inventory_license_rows(inventory)
        bundle: dict[str, Any] = {
            "schema_version": SCHEMA_VERSION,
            "manifest_version": MANIFEST_VERSION,
            "manifest_kind": "offline-bundle",
            "audit_status": "incomplete",
            "installer_qualified": False,
            "offline_qualified": False,
            "qualification": {"installer_qualified": False, "offline_qualified": False},
            "source_inventory": {
                "path": "metadata/distribution-inventory.json",
                "sha256": _hash_file(inventory_file)[0],
                "original_path": inventory_relative,
            },
            "source_kit": {
                "path": "metadata/source-kit-manifest.json",
                "sha256": _hash_file(source_kit_file)[0],
                "original_path": source_kit_relative,
                "audit_status": source_kit["audit_status"],
            },
            "runtime_manifest": {
                "path": DEFAULT_RUNTIME_MANIFEST,
                "sha256": runtime_manifest_record["sha256"],
            },
            "sbom": {
                "path": sbom_record["path"],
                "sha256": sbom_record["sha256"],
                "format": "SPDX-2.3",
            },
            "installer": {
                "script": installer_relative,
                "verifier": verifier_relative,
                "runtime_manifest": DEFAULT_RUNTIME_MANIFEST,
                "network_required": False,
            },
            "license_inventory": licenses,
            "dependency_closure": dependencies,
            "files": bundle_files,
            "summary": {
                "file_count": len(bundle_files),
                "runtime_file_count": len(installed_runtime_records),
                "source_kit_file_count": len(source_records),
                "metadata_file_count": len(metadata_records) + 3 + len(sbom_records),
                "license_file_count": sum(row["kind"] == "notice" for row in runtime_records),
                "sbom_count": len(sbom_records),
                "installer_qualified": False,
                "offline_qualified": False,
            },
            "boundary": (
                "This is a deterministic, hash-verified offline bundle staging record. It does not "
                "certify a signed installer, clean-machine installation, complete dynamic-load coverage, "
                "license clearance, corresponding-source completeness, or offline runtime behavior."
            ),
        }
        if dependency_manifest_record is not None:
            bundle["dependency_source_kit"] = {
                "path": dependency_manifest_record["path"],
                "sha256": dependency_manifest_record["sha256"],
                "size": dependency_manifest_record["size"],
                "original_path": payload_root.relative_to(root).as_posix() + "/" + helper.MANIFEST,
            }
        _validate_manifest_shape(bundle)
        _write_json(staging / DEFAULT_BUNDLE_MANIFEST, bundle)
        verify_bundle(staging, strict_files=True)

        try:
            package_root.parent.mkdir(parents=True, exist_ok=True)
            _reject_link_chain(package_root.parent, "bundle destination")
            staging.rename(package_root)
        except OSError as exc:
            _error(f"could not publish offline bundle {package_root}: {exc}")
    return bundle


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--inventory", required=True, type=pathlib.Path,
                        help="generated distribution inventory JSON")
    parser.add_argument("--allowlist", required=True, type=pathlib.Path,
                        help="portable runtime asset/notice allowlist JSON")
    parser.add_argument("--source-kit", "--source-kit-manifest", dest="source_kit",
                        required=True, type=pathlib.Path,
                        help="generated source-kit manifest JSON")
    parser.add_argument("--source-root", "--root", dest="source_root", required=True,
                        type=pathlib.Path, help="explicit root for all input paths")
    parser.add_argument("--dependency-source-kit", type=pathlib.Path,
                        help="optional workspace-relative composed dependency source payload directory")
    parser.add_argument("--output-root", required=True, type=pathlib.Path,
                        help="directory under which the bundle is published")
    parser.add_argument("--destination", default=DEFAULT_DESTINATION,
                        help=f"relative bundle directory (default: {DEFAULT_DESTINATION})")
    parser.add_argument("--installer-template", type=pathlib.Path,
                        help="offline installer PowerShell template")
    parser.add_argument("--verifier-template", type=pathlib.Path,
                        help="offline verifier PowerShell template")
    args = parser.parse_args(argv)
    try:
        result = stage_bundle(
            args.inventory,
            args.allowlist,
            args.source_kit,
            args.source_root,
            args.output_root,
            args.destination,
            installer_template=args.installer_template,
            verifier_template=args.verifier_template,
            dependency_source_kit=args.dependency_source_kit,
        )
    except (BundleError, ValueError, OSError, RuntimeError) as exc:
        print(f"offline bundle staging: {exc}", file=sys.stderr)
        return 1
    print(f"Wrote offline bundle under {args.output_root} "
          f"({result['summary']['file_count']} files; qualification incomplete)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
