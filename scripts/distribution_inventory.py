"""Build a path-portable, evidence-backed distribution component inventory.

The versioned component manifest describes ownership and provenance.  This
script joins it to a Release PE import report and the local SPDX/provenance
files, checking every supplied hash before writing an inventory.  It is an
inventory aid only: a successful run does not qualify an installer, an
offline source kit, or redistribution terms.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import pathlib
import re
import subprocess
import sys
from datetime import datetime, timezone
from typing import Any


ROOT = pathlib.Path(__file__).resolve().parents[1]
SCHEMA_VERSION = 1
INVENTORY_VERSION = 1
SHA256_RE = re.compile(r"^[0-9a-fA-F]{64}$")
SHA1_RE = re.compile(r"^[0-9a-fA-F]{40}$")
COMMIT_RE = re.compile(r"/tree/([0-9a-fA-F]{40})(?:[/?#]|$)")
PLANEGCS_HASH_RE = re.compile(r"^([0-9a-fA-F]{64})  (upstream/[^\r\n]+)$", re.MULTILINE)
WINDOWS_ABSOLUTE_RE = re.compile(r"^[A-Za-z]:[\\/]")

SOURCE_KINDS = {
    "workspace",
    "redistributable",
    "vcpkg-spdx",
    "qt-spdx",
    "planegcs-provenance",
    "bootstrap-dependency",
}
COMPONENT_KINDS = {
    "application",
    "runtime",
    "plugin",
    "static-source",
    "asset",
    "build-only",
}
RUNTIME_IMPORT_KINDS = {
    "local-component",
    "installed-system-runtime",
    "windows-api-contract",
    "unresolved",
}


class InventoryError(ValueError):
    """Raised when inventory input cannot be trusted or is incomplete."""


def _error(message: str) -> None:
    raise InventoryError(message)


def _require(condition: bool, message: str) -> None:
    if not condition:
        _error(message)


def _require_string(value: Any, field: str) -> str:
    _require(isinstance(value, str) and bool(value.strip()), f"{field} must be a nonempty string")
    return value


def _sha256(path: pathlib.Path) -> str:
    try:
        with path.open("rb") as source:
            return hashlib.file_digest(source, "sha256").hexdigest()
    except OSError as exc:
        _error(f"Could not hash {path}: {exc}")
    raise AssertionError("unreachable")


def _sha1(path: pathlib.Path) -> str:
    try:
        with path.open("rb") as source:
            return hashlib.file_digest(source, "sha1").hexdigest()
    except OSError as exc:
        _error(f"Could not hash {path}: {exc}")
    raise AssertionError("unreachable")


def _validate_sha256(value: Any, field: str) -> str:
    _require(isinstance(value, str) and SHA256_RE.fullmatch(value) is not None,
             f"{field} must be a 64-digit SHA-256 value")
    return value.lower()


def _canonical_relative(value: Any, field: str) -> str:
    text = _require_string(value, field).strip()
    if text.startswith(("/", "\\")) or WINDOWS_ABSOLUTE_RE.match(text):
        _error(f"{field} must be repository-relative")
    text = text.replace("\\", "/")
    if ":" in text:
        _error(f"{field} contains a forbidden colon")
    path = pathlib.PurePosixPath(text)
    if not path.parts or path == pathlib.PurePosixPath(".") or ".." in path.parts:
        _error(f"{field} contains an unsafe path")
    return path.as_posix()


def _canonical_name(value: Any, field: str) -> str:
    text = _require_string(value, field).strip()
    if text in {".", ".."} or "/" in text or "\\" in text or ":" in text:
        _error(f"{field} must be a filename")
    return text


def _relative_existing_path(root: pathlib.Path, value: Any, field: str,
                            *, file_only: bool | None = None) -> tuple[str, pathlib.Path]:
    if isinstance(value, pathlib.Path):
        text = str(value)
    else:
        text = _require_string(value, field).strip()
    is_absolute = bool(WINDOWS_ABSOLUTE_RE.match(text) or text.startswith(("/", "\\")))
    if is_absolute:
        candidate = pathlib.Path(text)
    else:
        relative = _canonical_relative(text, field)
        candidate = root.joinpath(*relative.split("/"))
    try:
        resolved = candidate.resolve()
        root_resolved = root.resolve()
        relative_path = resolved.relative_to(root_resolved)
    except (OSError, ValueError) as exc:
        _error(f"{field} must resolve inside the repository: {text} ({exc})")
    relative = relative_path.as_posix()
    if file_only is True and not resolved.is_file():
        _error(f"{field} does not identify a file: {text}")
    if file_only is False and not resolved.is_dir():
        _error(f"{field} does not identify a directory: {text}")
    if file_only is None and not resolved.exists():
        _error(f"{field} does not exist: {text}")
    return relative, resolved


def _tree_sha256(path: pathlib.Path) -> str:
    digest = hashlib.sha256()
    try:
        children = sorted(path.rglob("*"), key=lambda item: item.relative_to(path).as_posix())
    except OSError as exc:
        _error(f"Could not enumerate source tree {path}: {exc}")
    for child in children:
        if child.is_symlink():
            _error(f"Symlink is not valid source evidence: {child}")
        if not child.is_file():
            continue
        relative = child.relative_to(path).as_posix()
        digest.update(relative.encode("utf-8"))
        digest.update(b"\0")
        try:
            with child.open("rb") as source:
                while chunk := source.read(1024 * 1024):
                    digest.update(chunk)
        except OSError as exc:
            _error(f"Could not read source evidence {child}: {exc}")
        digest.update(b"\0")
    return digest.hexdigest()


def _read_json(path: pathlib.Path, field: str) -> dict[str, Any]:
    try:
        value = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, UnicodeError, json.JSONDecodeError) as exc:
        _error(f"Malformed {field} JSON at {path}: {exc}")
    _require(isinstance(value, dict), f"{field} JSON must contain an object")
    return value


def _read_text(path: pathlib.Path, field: str) -> str:
    try:
        return path.read_text(encoding="utf-8")
    except (OSError, UnicodeError) as exc:
        _error(f"Could not read {field} at {path}: {exc}")
    raise AssertionError("unreachable")


def _validate_path_list(value: Any, field: str) -> list[str]:
    _require(isinstance(value, list) and bool(value), f"{field} must be a nonempty list")
    result = [_canonical_relative(item, f"{field}[{index}]") for index, item in enumerate(value)]
    _require(len(set(result)) == len(result), f"{field} contains duplicate paths")
    return result


def _validate_source_files(value: Any, field: str) -> list[dict[str, str]]:
    if value is None:
        return []
    _require(isinstance(value, list), f"{field} must be a list")
    result: list[dict[str, str]] = []
    seen: set[str] = set()
    for index, item in enumerate(value):
        _require(isinstance(item, dict), f"{field}[{index}] must be an object")
        path = _canonical_relative(item.get("path"), f"{field}[{index}].path")
        _require(path not in seen, f"{field} contains duplicate path {path}")
        seen.add(path)
        result.append({
            "path": path,
            "sha256": _validate_sha256(item.get("sha256"), f"{field}[{index}].sha256"),
        })
    return result


def _validate_spdx_hash_overrides(value: Any, field: str) -> dict[str, dict[str, str]]:
    """Validate explicit provenance for a known-bad SPDX file checksum.

    Some vendor SBOMs have shipped a stale file digest while the pinned
    archive and installed binary agree.  An override is accepted only when it
    names the affected relative file, pins the archive bytes, identifies the
    archive member and the local extraction tool, and records why the
    exception is necessary.  The archive and member are re-hashed during
    inventory generation; this is not a free-form trust escape.
    """

    if value is None:
        return {}
    _require(isinstance(value, dict) and bool(value), f"{field} must be a nonempty object")
    result: dict[str, dict[str, str]] = {}
    for path_value, item in value.items():
        path = _canonical_relative(path_value, f"{field} file path")
        _require(path not in result, f"{field} contains duplicate file path {path}")
        _require(isinstance(item, dict), f"{field}[{path}] must be an object")
        _require(set(item) == {
            "algorithm", "value", "archive_path", "archive_sha256",
            "archive_member", "tool_path", "reason"
        }, f"{field}[{path}] has unsupported fields")
        algorithm = _require_string(item.get("algorithm"), f"{field}[{path}].algorithm").upper()
        _require(algorithm in {"SHA1", "SHA256"},
                 f"{field}[{path}].algorithm must be SHA1 or SHA256")
        digest = _require_string(item.get("value"), f"{field}[{path}].value")
        pattern = SHA1_RE if algorithm == "SHA1" else SHA256_RE
        _require(pattern.fullmatch(digest) is not None,
                 f"{field}[{path}].value is not a valid {algorithm} value")
        archive_path = _canonical_relative(item.get("archive_path"),
                                           f"{field}[{path}].archive_path")
        tool_path = _canonical_relative(item.get("tool_path"),
                                        f"{field}[{path}].tool_path")
        archive_member = _canonical_relative(item.get("archive_member"),
                                             f"{field}[{path}].archive_member")
        archive_sha256 = _validate_sha256(item.get("archive_sha256"),
                                          f"{field}[{path}].archive_sha256")
        reason = _require_string(item.get("reason"), f"{field}[{path}].reason")
        result[path] = {
            "algorithm": algorithm,
            "value": digest.lower(),
            "archive_path": archive_path,
            "archive_sha256": archive_sha256,
            "archive_member": archive_member,
            "tool_path": tool_path,
            "reason": reason,
        }
    return result


def _validate_artifacts(value: Any, field: str) -> list[dict[str, str]]:
    if value is None:
        return []
    _require(isinstance(value, list), f"{field} must be a list")
    result: list[dict[str, str]] = []
    seen: set[str] = set()
    for index, item in enumerate(value):
        _require(isinstance(item, dict), f"{field}[{index}] must be an object")
        path = _canonical_relative(item.get("path"), f"{field}[{index}].path")
        _require(path not in seen, f"{field} contains duplicate path {path}")
        seen.add(path)
        result.append({
            "path": path,
            "sha256": _validate_sha256(item.get("sha256"), f"{field}[{index}].sha256"),
        })
    return result


def validate_manifest(manifest: Any) -> None:
    """Validate the structural part of a distribution component manifest."""

    _require(isinstance(manifest, dict), "distribution manifest must contain an object")
    _require(manifest.get("schema_version") == SCHEMA_VERSION,
             f"unsupported distribution manifest schema_version: {manifest.get('schema_version')!r}")
    _require(manifest.get("audit_status") == "incomplete",
             "distribution manifest audit_status must remain 'incomplete'")

    runtime_evidence = manifest.get("runtime_evidence")
    _require(isinstance(runtime_evidence, dict), "runtime_evidence must be an object")
    _canonical_relative(runtime_evidence.get("path"), "runtime_evidence.path")
    _require(runtime_evidence.get("required", True) is True,
             "runtime_evidence.required must be true; inventory cannot skip evidence")

    package_managers = manifest.get("package_managers", [])
    _require(isinstance(package_managers, list), "package_managers must be a list")
    for index, manager in enumerate(package_managers):
        field = f"package_managers[{index}]"
        _require(isinstance(manager, dict), f"{field} must be an object")
        _require_string(manager.get("name"), f"{field}.name")
        _canonical_relative(manager.get("manifest_path"), f"{field}.manifest_path")
        _require_string(manager.get("repository"), f"{field}.repository")
        _require_string(manager.get("commit"), f"{field}.commit")

    components = manifest.get("components")
    _require(isinstance(components, list) and bool(components), "components must be a nonempty list")
    component_ids: set[str] = set()
    runtime_names: dict[str, str] = {}
    destinations: dict[str, str] = {}
    for index, component in enumerate(components):
        field = f"components[{index}]"
        _require(isinstance(component, dict), f"{field} must be an object")
        identifier = _require_string(component.get("id"), f"{field}.id")
        _require(identifier not in component_ids, f"duplicate component id: {identifier}")
        component_ids.add(identifier)
        _require(component.get("kind") in COMPONENT_KINDS,
                 f"{field}.kind is unknown: {component.get('kind')!r}")

        package = component.get("package")
        _require(isinstance(package, dict), f"{field}.package must be an object")
        _require_string(package.get("name"), f"{field}.package.name")
        _require_string(package.get("version"), f"{field}.package.version")
        _require_string(package.get("license"), f"{field}.package.license")

        source = component.get("source")
        _require(isinstance(source, dict), f"{field}.source must be an object")
        source_kind = source.get("kind")
        _require(source_kind in SOURCE_KINDS, f"{field}.source.kind is unknown: {source_kind!r}")
        if source_kind in {"vcpkg-spdx", "qt-spdx"}:
            _canonical_relative(source.get("spdx_path"), f"{field}.source.spdx_path")
            if "prefix_path" in source:
                _canonical_relative(source.get("prefix_path"), f"{field}.source.prefix_path")
            if "package_id" in source:
                _require_string(source.get("package_id"), f"{field}.source.package_id")
            if source_kind == "qt-spdx":
                _require_string(source.get("module_version"), f"{field}.source.module_version")
            _validate_spdx_hash_overrides(source.get("hash_overrides"),
                                          f"{field}.source.hash_overrides")
        elif source_kind == "planegcs-provenance":
            _canonical_relative(source.get("provenance_path"), f"{field}.source.provenance_path")
        elif source_kind == "bootstrap-dependency":
            _canonical_relative(source.get("dependencies_path"), f"{field}.source.dependencies_path")
            _require(bool(source.get("dependency_name") or source.get("asset_name")),
                     f"{field}.source needs dependency_name or asset_name")
            _require(not (source.get("dependency_name") and source.get("asset_name")),
                     f"{field}.source cannot set both dependency_name and asset_name")
        if "paths" in source:
            _validate_path_list(source.get("paths"), f"{field}.source.paths")

        notice_paths = component.get("notice_paths")
        _require(isinstance(notice_paths, list) and bool(notice_paths),
                 f"{field}.notice_paths must be a nonempty list")
        notice_values = [_canonical_relative(item, f"{field}.notice_paths[{notice_index}]")
                         for notice_index, item in enumerate(notice_paths)]
        _require(len(set(notice_values)) == len(notice_values), f"{field}.notice_paths contains duplicates")

        source_files = _validate_source_files(component.get("source_files"), f"{field}.source_files")
        _ = source_files
        _validate_artifacts(component.get("artifacts"), f"{field}.artifacts")

        names = component.get("runtime_names", [])
        _require(isinstance(names, list), f"{field}.runtime_names must be a list")
        destinations_map = component.get("destinations", {})
        _require(isinstance(destinations_map, dict), f"{field}.destinations must be an object")
        local_names: set[str] = set()
        for name_index, name in enumerate(names):
            canonical_name = _canonical_name(name, f"{field}.runtime_names[{name_index}]")
            key = canonical_name.casefold()
            _require(key not in local_names, f"{field} contains duplicate runtime name {canonical_name}")
            local_names.add(key)
            _require(key not in runtime_names,
                     f"runtime ownership is declared more than once for {canonical_name}")
            runtime_names[key] = identifier
            _require(canonical_name in destinations_map or
                     any(isinstance(map_key, str) and map_key.casefold() == key
                         for map_key in destinations_map),
                     f"{field}.destinations is missing {canonical_name}")
        for destination_name, destination in destinations_map.items():
            _require(isinstance(destination_name, str), f"{field}.destinations keys must be strings")
            destination_key = destination_name.casefold()
            _require(destination_key in local_names,
                     f"{field}.destinations names an unknown runtime file {destination_name}")
            destination_path = _canonical_relative(destination,
                                                   f"{field}.destinations[{destination_name!r}]")
            destination_key = destination_path.casefold()
            _require(destination_key not in destinations,
                     f"duplicate destination path: {destination_path}")
            destinations[destination_key] = identifier


def load_manifest(path: pathlib.Path | str) -> dict[str, Any]:
    """Read and validate a versioned component manifest."""

    manifest_path = pathlib.Path(path)
    manifest = _read_json(manifest_path, "distribution manifest")
    validate_manifest(manifest)
    return manifest


def _describe_path(root: pathlib.Path, relative: str, expected: str | None = None) -> dict[str, Any]:
    canonical, resolved = _relative_existing_path(root, relative, "source path")
    if resolved.is_file():
        actual = _sha256(resolved)
        kind = "file"
    elif resolved.is_dir():
        actual = _tree_sha256(resolved)
        kind = "directory"
    else:
        _error(f"source path is neither a file nor directory: {relative}")
    if expected is not None:
        expected = _validate_sha256(expected, f"source_files[{canonical}].sha256")
        if actual != expected:
            _error(f"stale source hash for {canonical}: expected {expected}, received {actual}")
    return {"path": canonical, "kind": kind, "sha256": actual}


def _describe_paths(root: pathlib.Path, source: dict[str, Any], component: dict[str, Any]) -> list[dict[str, Any]]:
    paths = list(source.get("paths", []))
    expected = {item["path"]: item["sha256"] for item in _validate_source_files(
        component.get("source_files"), f"component {component['id']}.source_files")}
    result: list[dict[str, Any]] = []
    for relative in paths:
        result.append(_describe_path(root, relative, expected.get(relative)))
    for relative, sha256 in expected.items():
        # A declared file hash must be checked even when a broader source
        # directory is also listed.  The directory tree hash is useful
        # context, but it is not the declared file-level evidence.
        if not any(item["path"] == relative for item in result):
            result.append(_describe_path(root, relative, sha256))
    return sorted(result, key=lambda item: item["path"])


def _describe_artifacts(root: pathlib.Path, component: dict[str, Any]) -> list[dict[str, str]]:
    result: list[dict[str, str]] = []
    for item in _validate_artifacts(component.get("artifacts"), f"component {component['id']}.artifacts"):
        relative, resolved = _relative_existing_path(root, item["path"], "artifact path", file_only=True)
        actual = _sha256(resolved)
        if actual != item["sha256"]:
            _error(f"stale artifact hash for {relative}: expected {item['sha256']}, received {actual}")
        result.append({"path": relative, "sha256": actual})
    return result


def _describe_notices(root: pathlib.Path, component: dict[str, Any]) -> list[dict[str, str]]:
    result: list[dict[str, str]] = []
    for index, value in enumerate(component["notice_paths"]):
        relative, resolved = _relative_existing_path(root, value, f"notice_paths[{index}]", file_only=True)
        result.append({"path": relative, "sha256": _sha256(resolved)})
    return result


def _spdx_package(doc: dict[str, Any], package_id: str, field: str) -> dict[str, Any]:
    _require(doc.get("spdxVersion") in {"SPDX-2.2", "SPDX-2.3"},
             f"{field} has unsupported SPDX version")
    packages = doc.get("packages")
    _require(isinstance(packages, list), f"{field}.packages must be a list")
    matches = [item for item in packages if isinstance(item, dict) and item.get("SPDXID") == package_id]
    _require(len(matches) == 1, f"{field} does not contain exactly one package {package_id}")
    package = matches[0]
    for key in ("name", "versionInfo", "downloadLocation", "licenseConcluded"):
        _require(key in package, f"{field} package {package_id} is missing {key}")
    return package


def _spdx_files(doc: dict[str, Any], field: str) -> list[dict[str, Any]]:
    files = doc.get("files")
    _require(isinstance(files, list), f"{field}.files must be a list")
    result = []
    ids: set[str] = set()
    for index, item in enumerate(files):
        _require(isinstance(item, dict), f"{field}.files[{index}] must be an object")
        identifier = _require_string(item.get("SPDXID"), f"{field}.files[{index}].SPDXID")
        _require(identifier not in ids, f"{field} repeats file SPDXID {identifier}")
        ids.add(identifier)
        filename = _require_string(item.get("fileName"), f"{field}.files[{index}].fileName")
        normalized = filename.replace("\\", "/")
        if (normalized.startswith("/") or WINDOWS_ABSOLUTE_RE.match(normalized)
                or ":" in normalized
                or ".." in pathlib.PurePosixPath(normalized).parts):
            _error(f"{field}.files[{index}].fileName is unsafe")
        result.append(item)
    return result


def _spdx_context(root: pathlib.Path, component: dict[str, Any]) -> tuple[dict[str, Any], dict[str, Any], list[dict[str, Any]]]:
    source = component["source"]
    relative, path = _relative_existing_path(root, source["spdx_path"],
                                             f"component {component['id']}.source.spdx_path", file_only=True)
    doc = _read_json(path, f"component {component['id']} SPDX")
    package_id = source.get("package_id", "SPDXRef-port" if source["kind"] == "vcpkg-spdx"
                            else f"SPDXRef-Package-{component['package']['name']}")
    package = _spdx_package(doc, package_id, f"component {component['id']} SPDX")
    expected_package = component["package"]
    _require(package["name"] == expected_package["name"],
             f"component {component['id']} SPDX package ownership mismatch: {package['name']!r}")
    source_kind = source["kind"]
    selected_package = package
    module_revision = package.get("versionInfo")
    if source_kind == "vcpkg-spdx":
        _require(package["versionInfo"] == expected_package["version"],
                 f"component {component['id']} vcpkg version mismatch: {package['versionInfo']!r}")
    else:
        module_version = source["module_version"]
        module_matches = [item for item in doc["packages"]
                          if isinstance(item, dict) and item.get("versionInfo") == module_version
                          and item.get("licenseConcluded") not in (None, "NOASSERTION")]
        _require(module_matches, f"component {component['id']} Qt SPDX lacks module version {module_version}")
        selected_package = next((item for item in module_matches
                                 if item.get("name") == expected_package["name"]), module_matches[0])
        _require(expected_package["version"] == module_version,
                 f"component {component['id']} Qt manifest version does not match module version")
        module_revision = package.get("versionInfo")
    license_expression = selected_package.get("licenseConcluded")
    _require(isinstance(license_expression, str) and license_expression not in {"", "NOASSERTION"},
             f"component {component['id']} SPDX has no concluded license")
    _require(license_expression == expected_package["license"],
             f"component {component['id']} SPDX license mismatch: {license_expression!r}")
    download_location = package.get("downloadLocation")
    _require(isinstance(download_location, str) and download_location not in {"", "NOASSERTION", "NONE"},
             f"component {component['id']} SPDX has no source location")
    document_namespace = _require_string(
        doc.get("documentNamespace"), f"component {component['id']} SPDX documentNamespace")
    upstream_sources = []
    for candidate in doc["packages"]:
        if not isinstance(candidate, dict) or candidate.get("SPDXID") == package_id:
            continue
        location = candidate.get("downloadLocation")
        if not isinstance(location, str) or location in {"", "NOASSERTION", "NONE"}:
            continue
        if "vcpkg" in location.lower():
            continue
        candidate_name = _require_string(
            candidate.get("name"), f"component {component['id']} SPDX upstream package name")
        candidate_version = candidate.get("versionInfo")
        if candidate_version is not None:
            _require_string(candidate_version,
                            f"component {component['id']} SPDX upstream package version")
        upstream_sources.append({
            "name": candidate_name,
            "version": candidate_version,
            "url": location,
            "license": candidate.get("licenseConcluded"),
        })
    prefix_relative = source.get("prefix_path")
    prefix_path = None
    if prefix_relative is not None:
        prefix_relative, prefix_path = _relative_existing_path(root, prefix_relative,
                                                               f"component {component['id']}.source.prefix_path",
                                                               file_only=False)
    hash_overrides = _validate_spdx_hash_overrides(
        source.get("hash_overrides"), f"component {component['id']}.source.hash_overrides")
    spdx_files = _spdx_files(doc, f"component {component['id']} SPDX")
    spdx_file_paths = {
        item["fileName"].replace("\\", "/").lstrip("./") for item in spdx_files
    }
    for override_path in hash_overrides:
        _require(override_path in spdx_file_paths,
                 f"component {component['id']} SPDX override names an unknown file {override_path}")
    context = {
        "kind": "vcpkg" if source_kind == "vcpkg-spdx" else "qt",
        "spdx_path": relative,
        "spdx_sha256": _sha256(path),
        "package_id": package_id,
        "document_namespace": document_namespace,
        "package_name": package["name"],
        "package_version": expected_package["version"],
        "evidence_package_version": module_revision,
        "url": download_location,
        "homepage": package.get("homepage"),
        "upstream_sources": upstream_sources,
        "prefix_path": prefix_relative,
        "hash_overrides": hash_overrides,
        "_prefix_path": prefix_path,
        "_files": spdx_files,
        "_relationships": doc.get("relationships", []),
        "_hash_overrides": hash_overrides,
    }
    public_context = {key: value for key, value in context.items() if not key.startswith("_")}
    return public_context, context, doc


def _archive_member_digest(root: pathlib.Path, override: dict[str, str],
                           component_id: str) -> str:
    """Hash one pinned archive member through the repository's local 7-Zip."""

    archive_relative, archive_path = _relative_existing_path(
        root, override["archive_path"],
        f"component {component_id} SPDX override archive_path", file_only=True)
    tool_relative, tool_path = _relative_existing_path(
        root, override["tool_path"],
        f"component {component_id} SPDX override tool_path", file_only=True)
    archive_sha256 = _sha256(archive_path)
    _require(archive_sha256 == override["archive_sha256"],
             f"stale SPDX override archive hash for {archive_relative}: "
             f"expected {override['archive_sha256']}, received {archive_sha256}")
    member = override["archive_member"]
    try:
        completed = subprocess.run(
            [str(tool_path), "x", "-so", str(archive_path), member],
            capture_output=True, check=False, timeout=120)
    except (OSError, subprocess.SubprocessError) as exc:
        _error(f"could not extract SPDX override archive member {member} with {tool_relative}: {exc}")
    _require(completed.returncode == 0,
             f"could not extract SPDX override archive member {member} from {archive_relative}: "
             f"{completed.stderr.decode('utf-8', errors='replace').strip()}")
    # A corrupted or unexpectedly broad member must not turn inventory into a
    # memory sink.  Qt plugins are much smaller than this bound.
    _require(len(completed.stdout) <= 512 * 1024 * 1024,
             f"SPDX override archive member is unexpectedly large: {member}")
    algorithm = override["algorithm"].lower()
    return hashlib.new(algorithm, completed.stdout).hexdigest()


def _spdx_file_for_binary(root: pathlib.Path, context: dict[str, Any], module_relative: str,
                          component_id: str) -> dict[str, Any]:
    target = pathlib.PurePosixPath(module_relative)
    prefix_path = context.get("_prefix_path")
    target_name = None
    if prefix_path is not None:
        try:
            module_path = root.joinpath(*module_relative.split("/")).resolve()
            relative = module_path.relative_to(prefix_path.resolve()).as_posix()
        except ValueError:
            _error(f"{component_id} binary is outside its SPDX prefix: {module_relative}")
        target_name = "./" + relative.replace("\\", "/")
    files = context["_files"]
    exact = [item for item in files if item.get("fileName", "").replace("\\", "/") == target_name]
    candidates = exact or [item for item in files
                           if pathlib.PurePosixPath(item.get("fileName", "").lstrip("./")).name.casefold()
                           == target.name.casefold()]
    _require(len(candidates) == 1,
             f"unknown or ambiguous SPDX ownership for {component_id} binary {module_relative}")
    item = candidates[0]
    relationships = context.get("_relationships")
    _require(isinstance(relationships, list),
             f"{component_id} SPDX relationships are missing; file ownership is unknown")
    contained: dict[str, set[str]] = {}
    for relationship_index, relationship in enumerate(relationships):
        _require(isinstance(relationship, dict),
                 f"{component_id} SPDX relationship[{relationship_index}] must be an object")
        parent = relationship.get("spdxElementId")
        child = relationship.get("relatedSpdxElement")
        _require_string(parent, f"{component_id} SPDX relationship[{relationship_index}].spdxElementId")
        _require_string(child, f"{component_id} SPDX relationship[{relationship_index}].relatedSpdxElement")
        relationship_type = _require_string(
            relationship.get("relationshipType"),
            f"{component_id} SPDX relationship[{relationship_index}].relationshipType")
        # vcpkg emits a generated binary package between the port package and
        # its binary files; Qt SBOMs generally use CONTAINS throughout.
        if relationship_type in {"CONTAINS", "GENERATES"}:
            contained.setdefault(parent, set()).add(child)
    reachable = {context["package_id"]}
    pending = [context["package_id"]]
    while pending:
        parent = pending.pop()
        for child in contained.get(parent, set()):
            if child not in reachable:
                reachable.add(child)
                pending.append(child)
    _require(item["SPDXID"] in reachable,
             f"unknown SPDX ownership for {component_id} binary {module_relative}")
    checksums = item.get("checksums", [])
    _require(isinstance(checksums, list), f"{component_id} SPDX file checksums must be a list")
    checksum_map: dict[str, Any] = {}
    for checksum_index, entry in enumerate(checksums):
        _require(isinstance(entry, dict),
                 f"{component_id} SPDX file checksum[{checksum_index}] must be an object")
        algorithm = _require_string(entry.get("algorithm"),
                                    f"{component_id} SPDX file checksum[{checksum_index}].algorithm").upper()
        _require(algorithm not in checksum_map,
                 f"{component_id} SPDX file repeats {algorithm} checksum")
        checksum_map[algorithm] = entry.get("checksumValue")
    _require("SHA256" in checksum_map or "SHA1" in checksum_map,
             f"{component_id} SPDX file has no SHA-256 or SHA-1 checksum")
    actual_path = root.joinpath(*module_relative.split("/"))
    override_key = relative.lstrip("./") if prefix_path is not None else module_relative
    override = context.get("_hash_overrides", {}).get(override_key)
    if override is not None:
        override_algorithm = override["algorithm"]
        _require(override_algorithm in checksum_map,
                 f"{component_id} SPDX override algorithm {override_algorithm} is not present in the SBOM")
        spdx_value = checksum_map[override_algorithm]
        pattern = SHA1_RE if override_algorithm == "SHA1" else SHA256_RE
        _require(isinstance(spdx_value, str) and pattern.fullmatch(spdx_value) is not None,
                 f"{component_id} SPDX file {override_algorithm} checksum is malformed")
        _require(spdx_value.lower() != override["value"],
                 f"{component_id} SPDX override must document a changed checksum")
        actual = _sha1(actual_path) if override_algorithm == "SHA1" else _sha256(actual_path)
        _require(actual == override["value"],
                 f"stale SPDX override binary hash for {module_relative}: "
                 f"expected {override['value']}, received {actual}")
        archive_actual = _archive_member_digest(root, override, component_id)
        _require(archive_actual == override["value"],
                 f"SPDX override archive member hash does not match {module_relative}: "
                 f"expected {override['value']}, received {archive_actual}")
        # Verify any non-overridden checksum still supplied by the SBOM.
        for algorithm, expected_value in checksum_map.items():
            if algorithm == override_algorithm:
                continue
            if algorithm == "SHA256":
                expected = _validate_sha256(expected_value, f"{component_id} SPDX file checksum")
                actual_value = _sha256(actual_path)
            elif algorithm == "SHA1":
                _require(isinstance(expected_value, str) and SHA1_RE.fullmatch(expected_value) is not None,
                         f"{component_id} SPDX file SHA-1 checksum is malformed")
                expected = expected_value.lower()
                actual_value = _sha1(actual_path)
            else:
                _error(f"unsupported SPDX checksum algorithm {algorithm} for {module_relative}")
            _require(actual_value == expected,
                     f"stale SPDX binary hash for {module_relative}: expected {expected}, received {actual_value}")
    elif "SHA256" in checksum_map:
        expected = _validate_sha256(checksum_map["SHA256"], f"{component_id} SPDX file checksum")
        actual = _sha256(actual_path)
        if actual != expected:
            _error(f"stale SPDX binary hash for {module_relative}: expected {expected}, received {actual}")
    elif "SHA1" in checksum_map:
        expected_sha1 = checksum_map["SHA1"]
        _require(isinstance(expected_sha1, str) and SHA1_RE.fullmatch(expected_sha1),
                 f"{component_id} SPDX file SHA-1 checksum is malformed")
        actual_sha1 = _sha1(actual_path)
        if actual_sha1 != expected_sha1.lower():
            _error(f"stale SPDX binary hash for {module_relative}: expected {expected_sha1}, received {actual_sha1}")
    return {
        "spdx_id": item["SPDXID"],
        "file_name": item["fileName"],
        "license_concluded": item.get("licenseConcluded"),
        "checksums": checksum_map,
        **({"verification_override": {
            "algorithm": override["algorithm"],
            "value": override["value"],
            "archive_path": override["archive_path"],
            "archive_member": override["archive_member"],
            "archive_sha256": override["archive_sha256"],
            "reason": override["reason"],
        }} if override is not None else {}),
    }


def _bootstrap_context(root: pathlib.Path, component: dict[str, Any]) -> dict[str, Any]:
    source = component["source"]
    manifest_relative, manifest_path = _relative_existing_path(
        root, source["dependencies_path"], f"component {component['id']}.source.dependencies_path", file_only=True)
    dependencies = _read_json(manifest_path, f"component {component['id']} dependency manifest")
    if source.get("dependency_name"):
        entries = dependencies.get("bootstrap_dependencies")
        _require(isinstance(entries, list),
                 f"component {component['id']} dependency manifest bootstrap_dependencies must be a list")
        matches = [item for item in entries
                   if isinstance(item, dict) and item.get("name") == source["dependency_name"]]
        _require(len(matches) == 1, f"component {component['id']} dependency manifest has no unique dependency")
        item = matches[0]
        source_url = _require_string(item.get("url"), f"component {component['id']} dependency url")
        provenance = _require_string(item.get("provenance", item.get("url")),
                                     f"component {component['id']} dependency provenance")
    else:
        entries = dependencies.get("bundled_assets")
        _require(isinstance(entries, list),
                 f"component {component['id']} dependency manifest bundled_assets must be a list")
        matches = [item for item in entries
                   if isinstance(item, dict) and item.get("name") == source["asset_name"]]
        _require(len(matches) == 1, f"component {component['id']} dependency manifest has no unique asset")
        item = matches[0]
        source_url = _require_string(item.get("source"), f"component {component['id']} asset source")
        provenance = source_url
    _require(item.get("version") == component["package"]["version"],
             f"component {component['id']} dependency version mismatch")
    _require(item.get("license") == component["package"]["license"],
             f"component {component['id']} dependency license mismatch")
    _require_string(item.get("name"), f"component {component['id']} dependency name")
    if item.get("filename") is not None:
        _canonical_name(item.get("filename"), f"component {component['id']} dependency filename")
    if item.get("commit") is not None:
        _require_string(item.get("commit"), f"component {component['id']} dependency commit")
    if item.get("destination") is not None:
        _canonical_relative(item.get("destination"), f"component {component['id']} dependency destination")
    if item.get("kind") is not None:
        _require_string(item.get("kind"), f"component {component['id']} dependency kind")
    item_hash = _validate_sha256(item.get("sha256"), f"component {component['id']} dependency sha256")
    artifacts = _validate_artifacts(component.get("artifacts"), f"component {component['id']}.artifacts")
    if item.get("filename"):
        _require(any(pathlib.PurePosixPath(artifact["path"]).name == item["filename"]
                     and artifact["sha256"] == item_hash for artifact in artifacts),
                 f"component {component['id']} artifact does not match dependency filename/hash")
    return {
        "kind": "bootstrap",
        "dependencies_path": manifest_relative,
        "dependencies_sha256": _sha256(manifest_path),
        "name": item["name"],
        "version": item["version"],
        "url": source_url,
        "provenance": provenance,
        "commit": item.get("commit"),
        "declared_sha256": item_hash,
        "artifact_filename": item.get("filename"),
        "destination": item.get("destination"),
        "kind_detail": item.get("kind"),
    }


def _planegcs_context(root: pathlib.Path, component: dict[str, Any]) -> dict[str, Any]:
    source = component["source"]
    provenance_relative, provenance_path = _relative_existing_path(
        root, source["provenance_path"], f"component {component['id']}.source.provenance_path", file_only=True)
    text = _read_text(provenance_path, f"component {component['id']} PlaneGCS provenance")
    commit_matches = COMMIT_RE.findall(text)
    _require(len(commit_matches) == 1, f"component {component['id']} PlaneGCS provenance must contain one upstream commit")
    records = PLANEGCS_HASH_RE.findall(text)
    _require(records and len({name for _, name in records}) == len(records),
             f"component {component['id']} PlaneGCS provenance has no unique upstream file hashes")
    upstream_files: list[dict[str, str]] = []
    for expected, relative in records:
        canonical = _canonical_relative(relative, f"component {component['id']} provenance source path")
        provenance_source_path = pathlib.PurePosixPath(provenance_relative).parent / canonical
        resolved_relative, resolved = _relative_existing_path(root, provenance_source_path.as_posix(),
                                                              f"component {component['id']} provenance source path",
                                                              file_only=True)
        actual = _sha256(resolved)
        expected = _validate_sha256(expected, f"component {component['id']} provenance hash")
        if actual != expected:
            _error(f"stale PlaneGCS provenance hash for {resolved_relative}: expected {expected}, received {actual}")
        upstream_files.append({"path": resolved_relative, "sha256": actual})
    source_paths = _describe_paths(root, source, component)
    return {
        "kind": "planegcs",
        "provenance_path": provenance_relative,
        "provenance_sha256": _sha256(provenance_path),
        "upstream_project": "FreeCAD",
        "upstream_commit": commit_matches[0].lower(),
        "upstream_files": sorted(upstream_files, key=lambda item: item["path"]),
        "source_paths": source_paths,
    }


def _workspace_context(root: pathlib.Path, component: dict[str, Any]) -> dict[str, Any]:
    source = component["source"]
    return {"kind": "workspace", "source_paths": _describe_paths(root, source, component)}


def _package_manager_evidence(root: pathlib.Path, manifest: dict[str, Any]) -> list[dict[str, str]]:
    result: list[dict[str, str]] = []
    for index, manager in enumerate(manifest.get("package_managers", [])):
        relative, path = _relative_existing_path(root, manager["manifest_path"],
                                                 f"package_managers[{index}].manifest_path", file_only=True)
        manager_manifest = _read_json(path, f"package manager {manager['name']} manifest")
        if manager["name"] == "vcpkg":
            actual = manager_manifest.get("native_package_manager")
            _require(isinstance(actual, dict), "vcpkg manifest has no native_package_manager record")
            _require(actual.get("repository") == manager["repository"],
                     "vcpkg repository provenance does not match the versioned inventory")
            _require(actual.get("commit") == manager["commit"],
                     "vcpkg commit provenance does not match the versioned inventory")
        result.append({
            "name": manager["name"],
            "manifest_path": relative,
            "manifest_sha256": _sha256(path),
            "repository": manager["repository"],
            "commit": manager["commit"],
        })
    return result


def _prepare_component(root: pathlib.Path, component: dict[str, Any]) -> dict[str, Any]:
    source_kind = component["source"]["kind"]
    if source_kind in {"vcpkg-spdx", "qt-spdx"}:
        public_context, private_context, _ = _spdx_context(root, component)
        source_payload = public_context
        private_source = private_context
    elif source_kind == "bootstrap-dependency":
        source_payload = _bootstrap_context(root, component)
        private_source = None
    elif source_kind == "planegcs-provenance":
        source_payload = _planegcs_context(root, component)
        private_source = None
    elif source_kind == "redistributable":
        pins = component["source"].get("sha256")
        _require(isinstance(pins, dict) and bool(pins), "redistributable requires reviewed binary hashes")
        normalized = {name.casefold(): _validate_sha256(value, "redistributable SHA256")
                      for name, value in pins.items()}
        _require(len(normalized) == len(pins) and set(normalized) ==
                 {name.casefold() for name in component.get("runtime_names", [])},
                 "redistributable hashes must exactly cover runtime names")
        source_payload = {"kind": "redistributable", "sha256": normalized,
                          "source_paths": _describe_paths(root, component["source"], component),
                          "licensing_clearance": False}
        private_source = None
    else:
        source_payload = _workspace_context(root, component)
        private_source = None

    notices = _describe_notices(root, component)
    artifacts = _describe_artifacts(root, component)
    component_payload = {
        "id": component["id"],
        "kind": component["kind"],
        "distribution_status": component.get("distribution_status", "included"),
        "package": {
            "name": component["package"]["name"],
            "version": component["package"]["version"],
            "license": component["package"]["license"],
            "source": source_payload,
        },
        "runtime_names": sorted(component.get("runtime_names", []), key=str.casefold),
        "destinations": {
            key: value for key, value in sorted(component.get("destinations", {}).items(), key=lambda item: item[0].casefold())
        },
        "notices": notices,
        "artifacts": artifacts,
    }
    return {
        "manifest": component,
        "payload": component_payload,
        "private_source": private_source,
    }


def _load_runtime_evidence(root: pathlib.Path, manifest: dict[str, Any], override: pathlib.Path | str | None) -> tuple[str, pathlib.Path, dict[str, Any]]:
    configured = manifest["runtime_evidence"]["path"] if override is None else override
    relative, path = _relative_existing_path(root, configured, "runtime evidence path", file_only=True)
    evidence = _read_json(path, "runtime evidence")
    modules = evidence.get("modules")
    _require(isinstance(modules, list) and bool(modules), "runtime evidence modules are missing")
    entry_points = evidence.get("entry_points")
    _require(isinstance(entry_points, list) and bool(entry_points), "runtime evidence entry_points are missing")
    unresolved = evidence.get("unresolved", [])
    _require(isinstance(unresolved, list), "runtime evidence unresolved must be a list")
    _require(not unresolved, f"runtime evidence contains unresolved imports: {unresolved}")
    _require(isinstance(evidence.get("recorded_utc"), str) and bool(evidence["recorded_utc"]),
             "runtime evidence recorded_utc is missing")
    return relative, path, evidence


def _runtime_inventory(root: pathlib.Path, evidence: dict[str, Any], prepared: dict[str, dict[str, Any]]) -> tuple[list[dict[str, Any]], list[dict[str, Any]], list[str], list[str], list[str]]:
    runtime_owners: dict[str, dict[str, Any]] = {}
    for item in prepared.values():
        for name in item["manifest"].get("runtime_names", []):
            runtime_owners[name.casefold()] = item

    modules = evidence["modules"]
    module_records: list[dict[str, Any]] = []
    module_by_path: dict[str, dict[str, Any]] = {}
    for index, module in enumerate(modules):
        field = f"runtime evidence modules[{index}]"
        _require(isinstance(module, dict), f"{field} must be an object")
        module_relative, module_path = _relative_existing_path(root, module.get("path"), f"{field}.path", file_only=True)
        key = os.path.normcase(str(module_path.resolve()))
        _require(key not in module_by_path, f"runtime evidence repeats module path {module_relative}")
        module_hash = _validate_sha256(module.get("sha256"), f"{field}.sha256")
        actual_hash = _sha256(module_path)
        if actual_hash != module_hash:
            _error(f"stale runtime hash for {module_relative}: expected {module_hash}, received {actual_hash}")
        imports = module.get("imports")
        _require(isinstance(imports, list), f"{field}.imports must be a list")
        owner = runtime_owners.get(module_path.name.casefold())
        _require(owner is not None, f"unknown ownership for runtime module {module_relative}")
        if owner["manifest"]["source"]["kind"] == "redistributable":
            pinned = owner["payload"]["package"]["source"]["sha256"][module_path.name.casefold()]
            _require(actual_hash == pinned, f"redistributable binary differs from reviewed hash: {module_relative}")
        record = {
            "path": module_relative,
            "name": module_path.name,
            "sha256": module_hash,
            "imports": imports,
            "owner": owner,
            "module_path": module_path,
        }
        module_by_path[key] = record
        module_records.append(record)

    entry_point_paths: list[str] = []
    entry_point_keys: set[str] = set()
    for index, entry in enumerate(evidence["entry_points"]):
        entry_relative, entry_path = _relative_existing_path(root, entry, f"runtime evidence entry_points[{index}]", file_only=True)
        key = os.path.normcase(str(entry_path.resolve()))
        _require(key not in entry_point_keys, f"runtime evidence repeats entry point {entry_relative}")
        entry_point_keys.add(key)
        _require(key in module_by_path, f"runtime evidence entry point has no module record: {entry_relative}")
        entry_point_paths.append(entry_relative)

    binaries: list[dict[str, Any]] = []
    local_imports: list[dict[str, Any]] = []
    system_names: set[str] = set()
    contract_names: set[str] = set()
    for module in module_records:
        owner = module["owner"]
        manifest_component = owner["manifest"]
        component_row = owner["payload"]
        binary = {
            "name": module["name"],
            "path": module["path"],
            "destination": next(destination for name, destination in manifest_component["destinations"].items()
                                  if name.casefold() == module["name"].casefold()),
            "sha256": module["sha256"],
            "component_id": manifest_component["id"],
            "package": component_row["package"],
            "notices": component_row["notices"],
            "evidence": {"runtime_module": module["path"], "runtime_sha256": module["sha256"]},
        }
        if owner["private_source"] is not None:
            binary["evidence"]["spdx_path"] = component_row["package"]["source"]["spdx_path"]
            binary["evidence"]["spdx_file"] = _spdx_file_for_binary(
                root, owner["private_source"], module["path"], manifest_component["id"])
        binaries.append(binary)
        for import_index, imported in enumerate(module["imports"]):
            field = f"{module['path']} imports[{import_index}]"
            _require(isinstance(imported, dict), f"{field} must be an object")
            name = _canonical_name(imported.get("name"), f"{field}.name")
            kind = imported.get("kind")
            _require(kind in RUNTIME_IMPORT_KINDS, f"{field} has unknown import ownership kind: {kind!r}")
            if kind == "unresolved":
                _error(f"unresolved runtime import {name} from {module['name']}")
            if kind == "windows-api-contract":
                contract_names.add(name)
                continue
            if kind == "installed-system-runtime":
                system_names.add(name)
                continue
            resolved_relative, resolved_path = _relative_existing_path(
                root, imported.get("resolved"), f"{field}.resolved", file_only=True)
            candidates = imported.get("candidates")
            _require(isinstance(candidates, list) and bool(candidates), f"{field}.candidates is missing")
            candidate_keys: set[str] = set()
            for candidate_index, candidate in enumerate(candidates):
                candidate_relative, candidate_path = _relative_existing_path(
                    root, candidate, f"{field}.candidates[{candidate_index}]", file_only=True)
                candidate_keys.add(os.path.normcase(str(candidate_path.resolve())))
                _ = candidate_relative
            resolved_key = os.path.normcase(str(resolved_path.resolve()))
            _require(resolved_key in candidate_keys, f"{field}.resolved is not one of its candidates")
            target_module = module_by_path.get(resolved_key)
            _require(target_module is not None, f"missing runtime module evidence for local import {name}")
            target_owner = runtime_owners.get(name.casefold())
            _require(target_owner is not None, f"unknown ownership for local runtime import {name}")
            _require(target_module["name"].casefold() == name.casefold(),
                     f"local import name/path mismatch: {name} -> {resolved_relative}")
            local_imports.append({
                "from": module["name"],
                "name": name,
                "kind": kind,
                "resolved_path": resolved_relative,
                "destination": next(destination for owner_name, destination in target_owner["manifest"]["destinations"].items()
                                      if owner_name.casefold() == name.casefold()),
                "sha256": target_module["sha256"],
                "component_id": target_owner["manifest"]["id"],
            })
    binaries.sort(key=lambda item: (item["destination"].casefold(), item["name"].casefold()))
    local_imports.sort(key=lambda item: (item["from"].casefold(), item["name"].casefold()))
    return binaries, local_imports, sorted(system_names, key=str.casefold), sorted(contract_names, key=str.casefold), entry_point_paths


def build_inventory(root: pathlib.Path | str, manifest: dict[str, Any],
                    runtime_evidence: pathlib.Path | str | None = None) -> dict[str, Any]:
    """Build an inventory from a validated manifest and repository evidence."""

    root = pathlib.Path(root).resolve()
    validate_manifest(manifest)
    package_manager_evidence = _package_manager_evidence(root, manifest)
    runtime_relative, runtime_path, evidence = _load_runtime_evidence(root, manifest, runtime_evidence)
    prepared_list = [_prepare_component(root, component) for component in manifest["components"]]
    prepared = {item["manifest"]["id"]: item for item in prepared_list}
    binaries, local_imports, system_names, contract_names, entry_points = _runtime_inventory(
        root, evidence, prepared)
    runtime_names = {name.casefold() for item in prepared.values() for name in item["manifest"].get("runtime_names", [])}
    observed_names = {item["name"].casefold() for item in binaries}
    _require(runtime_names == observed_names,
             f"runtime ownership evidence is incomplete; declared={sorted(runtime_names)} observed={sorted(observed_names)}")

    component_rows = [item["payload"] for item in sorted(prepared.values(), key=lambda item: item["manifest"]["id"])]
    static_inputs: list[dict[str, Any]] = []
    for item in sorted(prepared.values(), key=lambda item: item["manifest"]["id"]):
        component = item["manifest"]
        if component.get("runtime_names"):
            continue
        payload = item["payload"]
        source = component["source"]
        source_paths = _describe_paths(root, source, component) if source.get("paths") else []
        if source_paths:
            payload = dict(payload)
            payload["source_inputs"] = source_paths
        static_inputs.append({
            "component_id": component["id"],
            "kind": component["kind"],
            "distribution_status": component.get("distribution_status", "included"),
            "package": payload["package"],
            "source_inputs": source_paths,
            "artifacts": payload["artifacts"],
            "notices": payload["notices"],
        })

    return {
        "schema_version": SCHEMA_VERSION,
        "inventory_version": INVENTORY_VERSION,
        "audit_status": "incomplete",
        "distribution_qualified": False,
        "generated_utc": datetime.now(timezone.utc).isoformat().replace("+00:00", "Z"),
        "evidence": {
            "package_managers": package_manager_evidence,
            "runtime": {
                "path": runtime_relative,
                "sha256": _sha256(runtime_path),
                "recorded_utc": evidence["recorded_utc"],
                "module_count": len(evidence["modules"]),
                "entry_points": sorted(entry_points, key=str.casefold),
            },
        },
        "components": component_rows,
        "binaries": binaries,
        "static_inputs": static_inputs,
        "runtime_imports": local_imports,
        "system_runtime_imports": system_names,
        "windows_api_contracts": contract_names,
        "summary": {
            "binary_count": len(binaries),
            "static_input_count": len(static_inputs),
            "runtime_import_count": len(local_imports),
            "system_runtime_import_count": len(system_names),
            "windows_api_contract_count": len(contract_names),
            "installer_qualified": False,
            "offline_qualified": False,
        },
        "boundary": (
            "Evidence-backed source/package ownership and hashes only. This inventory does not prove "
            "dynamic loading coverage, clean-machine installation, offline behavior, license clearance, "
            "corresponding-source completeness, or redistributability."
        ),
    }


def write_inventory(path: pathlib.Path | str, inventory: dict[str, Any]) -> None:
    """Write an already validated inventory atomically enough for local use."""

    output = pathlib.Path(path)
    output.parent.mkdir(parents=True, exist_ok=True)
    try:
        output.write_text(json.dumps(inventory, indent=2, sort_keys=True) + "\n", encoding="utf-8")
    except OSError as exc:
        _error(f"Could not write distribution inventory {output}: {exc}")


def generate_inventory(root: pathlib.Path | str = ROOT,
                       manifest_path: pathlib.Path | str | None = None,
                       runtime_evidence: pathlib.Path | str | None = None,
                       output_path: pathlib.Path | str | None = None) -> dict[str, Any]:
    """Load the repository manifest, build the inventory, and optionally write it."""

    root = pathlib.Path(root).resolve()
    if manifest_path is None:
        manifest_path = root / "third_party" / "distribution-components.json"
    manifest = load_manifest(manifest_path)
    result = build_inventory(root, manifest, runtime_evidence)
    manifest_relative, manifest_file = _relative_existing_path(
        root, manifest_path, "distribution manifest path", file_only=True)
    result["evidence"]["component_manifest"] = {
        "path": manifest_relative,
        "sha256": _sha256(manifest_file),
    }
    if output_path is not None:
        write_inventory(output_path, result)
    return result


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", type=pathlib.Path, default=ROOT,
                        help="repository root (default: script repository)")
    parser.add_argument("--manifest", type=pathlib.Path,
                        help="versioned component manifest (default: third_party/distribution-components.json)")
    parser.add_argument("--runtime-evidence", type=pathlib.Path,
                        help="override runtime import evidence path")
    parser.add_argument("--output", type=pathlib.Path,
                        default=pathlib.Path("artifacts/runtime/distribution-inventory.json"),
                        help="path for the generated machine-readable inventory")
    args = parser.parse_args(argv)
    root = args.root.resolve()
    manifest_path = args.manifest if args.manifest is not None else root / "third_party" / "distribution-components.json"
    output_path = args.output if args.output.is_absolute() else root / args.output
    try:
        result = generate_inventory(root, manifest_path, args.runtime_evidence, output_path)
    except InventoryError as exc:
        print(f"distribution inventory: {exc}", file=sys.stderr)
        return 1
    print(f"Wrote {output_path} ({result['summary']['binary_count']} binaries; "
          f"{result['summary']['static_input_count']} static inputs; audit incomplete)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
