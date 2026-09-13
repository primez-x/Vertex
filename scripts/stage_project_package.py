"""Create a portable, self-contained package for one local ``.bldproj``.

The project database is the authority.  This command copies it byte-for-byte,
reads every revision asset through SQLite's read-only query path, verifies the
stored SHA-256, and writes one content-addressed copy of each asset beside the
project.  Optional templates, profiles, and documentation are accepted only
through an explicit resource manifest rooted at the caller's source root.

The result is an integrity package for offline transfer.  It does not claim
production qualification, installer qualification, or Apex compatibility.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import pathlib
import re
import shutil
import sqlite3
import tempfile
from collections.abc import Mapping
from typing import Any, Callable


SCHEMA_VERSION = 1
MANIFEST_VERSION = 1
DEFAULT_DESTINATION = "project-package"
DEFAULT_MANIFEST_NAME = "project-package-manifest.json"
DEFAULT_RESOURCE_MANIFEST_NAME = "metadata/project-resources.json"
FIXED_MTIME = 946684800  # 2000-01-01T00:00:00Z
SHA256_RE = re.compile(r"^[0-9a-fA-F]{64}$")
WINDOWS_ABSOLUTE_RE = re.compile(r"^[A-Za-z]:[\\/]")
RESOURCE_KINDS = {"template", "profile", "documentation"}


class ProjectPackageError(ValueError):
    """Raised when a project package input is incomplete, stale, or unsafe."""


def _error(message: str) -> None:
    raise ProjectPackageError(message)


def _require(condition: bool, message: str) -> None:
    if not condition:
        _error(message)


def _require_string(value: Any, field: str) -> str:
    _require(isinstance(value, str) and bool(value.strip()),
             f"{field} must be a nonempty string")
    _require("\x00" not in value, f"{field} contains a forbidden NUL")
    return value.strip()


def canonical_relative(value: Any, field: str) -> str:
    """Return a safe POSIX relative path or raise ``ProjectPackageError``."""

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


def _resolve_input_file(root: pathlib.Path, value: pathlib.Path | str, field: str) -> tuple[pathlib.Path, str]:
    text = os.fspath(value)
    if pathlib.Path(text).is_absolute():
        candidate = pathlib.Path(text)
        _reject_link_chain(candidate, field)
        try:
            resolved = candidate.resolve(strict=True)
            relative = resolved.relative_to(root).as_posix()
        except (OSError, RuntimeError, ValueError) as exc:
            _error(f"{field} must resolve to a file inside the explicit source root: {value} ({exc})")
    else:
        relative = canonical_relative(text, f"{field}.path")
        candidate = root.joinpath(*pathlib.PurePosixPath(relative).parts)
        _reject_link_chain(candidate, field)
        try:
            resolved = candidate.resolve(strict=True)
            resolved.relative_to(root)
        except (OSError, RuntimeError, ValueError) as exc:
            _error(f"{field} must resolve inside the explicit source root: {relative} ({exc})")
    if not resolved.is_file():
        _error(f"{field} is not a file: {value}")
    _reject_link_chain(resolved, field)
    return resolved, canonical_relative(relative, f"{field}.path")


def _hash_bytes(value: bytes) -> str:
    return hashlib.sha256(value).hexdigest()


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
    except ProjectPackageError:
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


def _copy_file(source: pathlib.Path, destination: pathlib.Path, relative: str) -> tuple[str, int]:
    try:
        destination.parent.mkdir(parents=True, exist_ok=True)
        if destination.exists() and _is_link(destination):
            _error(f"package output contains a symlink or junction: {relative}")
        shutil.copyfile(source, destination)
        os.utime(destination, (FIXED_MTIME, FIXED_MTIME))
    except ProjectPackageError:
        raise
    except OSError as exc:
        _error(f"could not copy {source} to {relative}: {exc}")
    actual, size = _hash_file(destination)
    return actual, size


def _output_path(root: pathlib.Path, relative: str) -> pathlib.Path:
    candidate = root.joinpath(*pathlib.PurePosixPath(relative).parts)
    try:
        candidate.resolve(strict=False).relative_to(root)
    except (OSError, RuntimeError, ValueError) as exc:
        _error(f"output path escapes the output root: {relative} ({exc})")
    return candidate


def _reserve(destinations: dict[str, str], relative: str, field: str) -> None:
    key = relative.casefold()
    for previous_key, previous in destinations.items():
        if key == previous_key or key.startswith(previous_key + "/") or previous_key.startswith(key + "/"):
            _error(f"duplicate package destination: {relative} conflicts with {previous}")
    destinations[key] = relative


def _parse_saved_revision(value: str) -> int | None:
    if value == "null":
        return None
    _require(value.isdecimal(), "project metadata saved_revision must be a decimal revision or null")
    return int(value)


def _inspect_project(
    project: pathlib.Path,
    write_asset: Callable[[str, bytes], None],
) -> tuple[dict[str, Any], list[dict[str, Any]]]:
    """Read the saved project without a write-capable SQLite connection."""

    try:
        database = sqlite3.connect(f"file:{project.as_posix()}?mode=ro", uri=True)
        database.execute("PRAGMA query_only=ON")
        database.row_factory = sqlite3.Row
    except sqlite3.Error as exc:
        _error(f"could not open project database read-only: {exc}")

    try:
        try:
            integrity = database.execute("PRAGMA quick_check").fetchone()
            _require(integrity is not None and integrity[0] == "ok",
                     "project SQLite integrity check failed")
            table_names = {row[0] for row in database.execute(
                "SELECT name FROM sqlite_master WHERE type='table'")}
            required = {"metadata", "revisions", "revision_assets"}
            _require(required.issubset(table_names),
                     "project is missing required persistence tables")
            metadata_rows = database.execute("SELECT key,value FROM metadata").fetchall()
            metadata = {str(row[0]): str(row[1]) for row in metadata_rows}
            for key in ("format_version", "document_id", "head_revision", "saved_revision", "logical_digest"):
                _require(key in metadata, f"project metadata is missing {key}")
            _require(metadata["format_version"].isdecimal(),
                     "project metadata format_version must be decimal")
            format_version = int(metadata["format_version"])
            _require(1 <= format_version <= 6,
                     f"unsupported project format_version: {format_version}")
            document_id = _require_string(metadata["document_id"], "project metadata document_id")
            _require(metadata["head_revision"].isdecimal(),
                     "project metadata head_revision must be decimal")
            head_revision = int(metadata["head_revision"])
            saved_revision = _parse_saved_revision(metadata["saved_revision"])
            logical_digest = _validate_sha256(metadata["logical_digest"], "project metadata logical_digest")
            revision_count = int(database.execute("SELECT count(*) FROM revisions").fetchone()[0])
            _require(revision_count > 0, "project must contain at least one revision")
            _require(head_revision < revision_count,
                     "project head_revision is outside the revision history")

            assets: dict[str, dict[str, Any]] = {}
            for row in database.execute(
                "SELECT revision,asset_id,media_type,sha256,metadata_json,data "
                "FROM revision_assets ORDER BY revision,asset_id"):
                revision = row[0]
                asset_id = _require_string(row[1], "project asset_id")
                media_type = _require_string(row[2], "project asset media_type")
                digest = _validate_sha256(row[3], f"project asset {asset_id}.sha256")
                try:
                    asset_metadata = json.loads(row[4])
                except (TypeError, UnicodeDecodeError, json.JSONDecodeError) as exc:
                    _error(f"project asset {asset_id} metadata is malformed: {exc}")
                _require(isinstance(asset_metadata, (dict, list, str, int, float, bool)) or asset_metadata is None,
                         f"project asset {asset_id} metadata is not portable JSON")
                _require(row[5] is not None, f"project asset {asset_id} has no data blob")
                data = bytes(row[5])
                actual = _hash_bytes(data)
                _require(actual == digest,
                         f"project asset {asset_id} bytes do not match stored SHA-256")
                key = digest
                entry = assets.setdefault(key, {
                    "sha256": digest,
                    "size": len(data),
                    "path": f"assets/{digest}.bin",
                    "references": [],
                    "_data": data,
                })
                _require(entry["size"] == len(data),
                         f"project asset hash {digest} has inconsistent sizes")
                entry["references"].append({
                    "revision": int(revision),
                    "asset_id": asset_id,
                    "media_type": media_type,
                    "metadata": asset_metadata,
                })

            asset_rows: list[dict[str, Any]] = []
            for digest in sorted(assets):
                entry = assets[digest]
                write_asset(digest, entry.pop("_data"))
                entry["references"].sort(key=lambda ref: (ref["revision"], ref["asset_id"]))
                asset_rows.append(entry)
            return ({
                "document_id": document_id,
                "format_version": format_version,
                "head_revision": head_revision,
                "saved_revision": saved_revision,
                "logical_digest": logical_digest,
                "revision_count": revision_count,
            }, asset_rows)
        except sqlite3.Error as exc:
            _error(f"could not read project database: {exc}")
    finally:
        database.close()


def _resource_entries(
    source_root: pathlib.Path,
    manifest_path: pathlib.Path | str | None,
    destinations: dict[str, str],
) -> tuple[list[dict[str, Any]], dict[str, Any] | None]:
    if manifest_path is None:
        return [], None
    manifest_file, manifest_source = _resolve_input_file(source_root, manifest_path, "project resource manifest")
    document = _read_json(manifest_file, "project resource manifest")
    _require(document.get("schema_version") == SCHEMA_VERSION,
             f"unsupported project resource manifest schema_version: {document.get('schema_version')!r}")
    raw_entries = document.get("entries")
    _require(isinstance(raw_entries, list), "project resource manifest entries must be a list")
    result: list[dict[str, Any]] = []
    for index, raw in enumerate(raw_entries):
        field = f"project resource manifest entries[{index}]"
        _require(isinstance(raw, Mapping), f"{field} must be an object")
        kind = _require_string(raw.get("kind"), f"{field}.kind").lower()
        _require(kind in RESOURCE_KINDS, f"{field}.kind must be template, profile, or documentation")
        source_value = raw.get("path", raw.get("source"))
        _require(source_value is not None, f"{field}.path is required")
        source, source_relative = _resolve_input_file(
            source_root, source_value, field)
        destination = canonical_relative(raw.get("destination"), f"{field}.destination")
        _reserve(destinations, destination, f"{field}.destination")
        expected = raw.get("sha256")
        if expected is not None:
            expected = _validate_sha256(expected, f"{field}.sha256")
        actual, size = _hash_file(source)
        _require(expected is None or expected == actual,
                 f"stale resource hash for {source_relative}: expected {expected}, received {actual}")
        result.append({
            "kind": kind,
            "source": source_relative,
            "path": destination,
            "sha256": actual,
            "size": size,
            "name": raw.get("name", pathlib.PurePosixPath(destination).stem),
        })
    result.sort(key=lambda row: (row["path"].casefold(), row["path"]))
    manifest_hash, manifest_size = _hash_file(manifest_file)
    return result, {
        "source": manifest_source,
        "path": DEFAULT_RESOURCE_MANIFEST_NAME,
        "sha256": manifest_hash,
        "size": manifest_size,
    }


def stage_project_package(
    project_path: pathlib.Path | str,
    source_root: pathlib.Path | str,
    output_root: pathlib.Path | str,
    destination: pathlib.Path | str = DEFAULT_DESTINATION,
    resource_manifest: pathlib.Path | str | None = None,
    manifest_name: pathlib.Path | str = DEFAULT_MANIFEST_NAME,
) -> dict[str, Any]:
    """Stage a project package from explicit local paths."""

    source_root_path = _resolve_directory(source_root, "source root")
    output_root_path = _resolve_directory(output_root, "output root", create=True)
    project_file, project_source = _resolve_input_file(source_root_path, project_path, "project")
    destination_relative = canonical_relative(destination, "package destination")
    package_root = _output_path(output_root_path, destination_relative)
    manifest_relative = canonical_relative(manifest_name, "manifest name")
    _require(manifest_relative == DEFAULT_MANIFEST_NAME,
             f"project package manifest must use {DEFAULT_MANIFEST_NAME}")
    _require(not package_root.exists(), "package destination must be new")

    destinations: dict[str, str] = {}
    project_relative = f"project/{project_file.name}"
    _reserve(destinations, project_relative, "project destination")
    _reserve(destinations, manifest_relative, "manifest name")
    _reserve(destinations, DEFAULT_RESOURCE_MANIFEST_NAME, "resource manifest destination")

    staging = pathlib.Path(tempfile.mkdtemp(prefix=f".{package_root.name}.stage-",
                                             dir=str(output_root_path)))
    try:
        project_hash, project_size = _copy_file(
            project_file, _output_path(staging, project_relative), project_relative)
        assets: list[dict[str, Any]] = []

        def write_asset(digest: str, data: bytes) -> None:
            relative = f"assets/{digest}.bin"
            _reserve(destinations, relative, f"asset {digest}")
            destination_file = _output_path(staging, relative)
            destination_file.parent.mkdir(parents=True, exist_ok=True)
            destination_file.write_bytes(data)
            os.utime(destination_file, (FIXED_MTIME, FIXED_MTIME))
            actual, size = _hash_file(destination_file)
            _require(actual == digest and size == len(data),
                     f"staged asset {digest} failed hash verification")

        project_metadata, assets = _inspect_project(project_file, write_asset)
        resources, resource_manifest_record = _resource_entries(
            source_root_path, resource_manifest, destinations)
        for resource in resources:
            source = _output_path(source_root_path, resource["source"])
            actual, size = _copy_file(source, _output_path(staging, resource["path"]), resource["path"])
            _require(actual == resource["sha256"] and size == resource["size"],
                     f"staged resource hash mismatch for {resource['path']}")
        if resource_manifest_record is not None:
            source = _output_path(source_root_path, resource_manifest_record["source"])
            actual, size = _copy_file(source, _output_path(staging, DEFAULT_RESOURCE_MANIFEST_NAME),
                                      DEFAULT_RESOURCE_MANIFEST_NAME)
            _require(actual == resource_manifest_record["sha256"] and size == resource_manifest_record["size"],
                     "staged project resource manifest hash mismatch")

        files = [{"kind": "project", "path": project_relative,
                  "sha256": project_hash, "size": project_size}]
        files.extend({"kind": "asset", "path": asset["path"],
                      "sha256": asset["sha256"], "size": asset["size"]}
                     for asset in assets)
        files.extend({"kind": resource["kind"], "path": resource["path"],
                      "sha256": resource["sha256"], "size": resource["size"]}
                     for resource in resources)
        if resource_manifest_record is not None:
            files.append({"kind": "resource-manifest", "path": resource_manifest_record["path"],
                          "sha256": resource_manifest_record["sha256"],
                          "size": resource_manifest_record["size"]})
        files.sort(key=lambda row: (row["path"].casefold(), row["path"]))
        manifest = {
            "schema_version": SCHEMA_VERSION,
            "manifest_version": MANIFEST_VERSION,
            "manifest_kind": "project-package",
            "audit_status": "incomplete",
            "offline_qualified": False,
            "project": {
                "source": project_source,
                "path": project_relative,
                "sha256": project_hash,
                "size": project_size,
                **project_metadata,
            },
            "assets": assets,
            "resources": resources,
            "resource_manifest": resource_manifest_record,
            "files": files,
            "summary": {
                "file_count": len(files) + 1,
                "asset_count": len(assets),
                "template_count": sum(row["kind"] == "template" for row in resources),
                "profile_count": sum(row["kind"] == "profile" for row in resources),
                "documentation_count": sum(row["kind"] == "documentation" for row in resources),
            },
            "boundary": (
                "This is a portable project package with verified project and asset bytes. "
                "It does not qualify production behavior, Apex compatibility, an installer, "
                "or cross-machine output equivalence."
            ),
        }
        manifest_path = _output_path(staging, manifest_relative)
        # The manifest is the package index and is intentionally not included
        # in its own ``files`` list; otherwise its digest would be recursive.
        manifest["files"] = files
        _write_json(manifest_path, manifest)
        manifest = _read_json(manifest_path, "staged project package manifest")

        # Publish only after every payload and the manifest have been written.
        staging.replace(package_root)
        return manifest
    except Exception:
        shutil.rmtree(staging, ignore_errors=True)
        raise


def verify_package(package_root: pathlib.Path | str) -> dict[str, Any]:
    """Verify the package manifest and every listed payload byte."""

    root = _resolve_directory(package_root, "project package")
    manifest_path = root / DEFAULT_MANIFEST_NAME
    _require(manifest_path.is_file() and not _is_link(manifest_path),
             "project package manifest is missing")
    manifest = _read_json(manifest_path, "project package manifest")
    _require(manifest.get("schema_version") == SCHEMA_VERSION and
             manifest.get("manifest_version") == MANIFEST_VERSION and
             manifest.get("manifest_kind") == "project-package",
             "unsupported project package manifest")
    _require(manifest.get("audit_status") == "incomplete" and
             manifest.get("offline_qualified") is False,
             "project package cannot claim qualification")
    files = manifest.get("files")
    _require(isinstance(files, list), "project package files must be a list")
    expected: dict[str, dict[str, Any]] = {}
    allowed_file_kinds = {"project", "asset", *RESOURCE_KINDS, "resource-manifest"}
    for index, row in enumerate(files):
        _require(isinstance(row, Mapping), f"project package files[{index}] must be an object")
        path = canonical_relative(row.get("path"), f"project package files[{index}].path")
        _require(path.casefold() not in expected, f"project package repeats file {path}")
        kind = _require_string(row.get("kind"), f"project package files[{index}].kind").lower()
        _require(kind in allowed_file_kinds,
                 f"project package files[{index}].kind is unsupported: {kind}")
        digest = _validate_sha256(row.get("sha256"), f"project package files[{index}].sha256")
        size = row.get("size")
        _require(isinstance(size, int) and not isinstance(size, bool) and size >= 0,
                 f"project package files[{index}].size must be nonnegative")
        expected[path.casefold()] = {"path": path, "kind": kind,
                                     "sha256": digest, "size": size}
    for key, row in expected.items():
        path = root.joinpath(*pathlib.PurePosixPath(row["path"]).parts)
        _reject_link_chain(path, f"project package payload {row['path']}")
        _require(path.is_file() and not _is_link(path), f"project package payload is missing: {row['path']}")
        actual, size = _hash_file(path)
        _require(actual == row["sha256"] and size == row["size"],
                 f"project package payload hash mismatch: {row['path']}")
    listed = {pathlib.PurePosixPath(row["path"]).as_posix().casefold() for row in expected.values()}
    for path in root.rglob("*"):
        _require(not _is_link(path), f"project package contains a symlink or junction: {path.relative_to(root).as_posix()}")
        if path.is_dir():
            continue
        relative = path.relative_to(root).as_posix().casefold()
        _require(relative in listed or relative == DEFAULT_MANIFEST_NAME.casefold(),
                 f"project package contains an unlisted file: {relative}")
    project = manifest.get("project")
    _require(isinstance(project, Mapping), "project package project record is missing")
    project_source = canonical_relative(project.get("source"), "project package project.source")
    project_path = canonical_relative(project.get("path"), "project package project.path")
    _require(project_path.casefold() in expected, "project package project file is not listed")
    _require(expected[project_path.casefold()]["kind"] == "project",
             "project package project file has the wrong kind")
    _require(project.get("sha256") == expected[project_path.casefold()]["sha256"],
             "project package project hash does not match its file record")
    _require(project.get("size") == expected[project_path.casefold()]["size"],
             "project package project size does not match its file record")
    assets = manifest.get("assets")
    _require(isinstance(assets, list), "project package assets must be a list")
    asset_paths: set[str] = set()
    for index, asset in enumerate(assets):
        _require(isinstance(asset, Mapping), f"project package assets[{index}] must be an object")
        digest = _validate_sha256(asset.get("sha256"), f"project package assets[{index}].sha256")
        path = canonical_relative(asset.get("path"), f"project package assets[{index}].path")
        _require(path == f"assets/{digest}.bin", f"project package asset path is not content-addressed: {path}")
        _require(path.casefold() in expected, f"project package asset is not listed: {path}")
        _require(expected[path.casefold()]["kind"] == "asset",
                 f"project package asset has the wrong file kind: {path}")
        _require(asset.get("size") == expected[path.casefold()]["size"],
                 f"project package asset size does not match its file record: {path}")
        _require(path.casefold() not in asset_paths, f"project package repeats asset {path}")
        asset_paths.add(path.casefold())

    resources = manifest.get("resources")
    _require(isinstance(resources, list), "project package resources must be a list")
    resource_paths: set[str] = set()
    resource_counts = {kind: 0 for kind in RESOURCE_KINDS}
    for index, resource in enumerate(resources):
        _require(isinstance(resource, Mapping),
                 f"project package resources[{index}] must be an object")
        kind = _require_string(resource.get("kind"),
                               f"project package resources[{index}].kind").lower()
        _require(kind in RESOURCE_KINDS,
                 f"project package resources[{index}].kind is unsupported: {kind}")
        path = canonical_relative(resource.get("path"),
                                  f"project package resources[{index}].path")
        _require(path.casefold() not in resource_paths,
                 f"project package repeats resource {path}")
        resource_paths.add(path.casefold())
        _require(path.casefold() in expected,
                 f"project package resource is not listed: {path}")
        _require(expected[path.casefold()]["kind"] == kind,
                 f"project package resource has the wrong file kind: {path}")
        digest = _validate_sha256(resource.get("sha256"),
                                  f"project package resources[{index}].sha256")
        _require(digest == expected[path.casefold()]["sha256"],
                 f"project package resource hash does not match its file record: {path}")
        _require(resource.get("size") == expected[path.casefold()]["size"],
                 f"project package resource size does not match its file record: {path}")
        resource_counts[kind] += 1

    resource_manifest = manifest.get("resource_manifest")
    if resource_manifest is None:
        _require(DEFAULT_RESOURCE_MANIFEST_NAME.casefold() not in expected,
                 "project package has an unreferenced resource manifest file")
    else:
        _require(isinstance(resource_manifest, Mapping),
                 "project package resource_manifest must be an object")
        manifest_resource_path = canonical_relative(resource_manifest.get("path"),
                                                    "project package resource_manifest.path")
        _require(manifest_resource_path == DEFAULT_RESOURCE_MANIFEST_NAME,
                 "project package resource manifest path is not canonical")
        _require(manifest_resource_path.casefold() in expected,
                 "project package resource manifest is not listed")
        _require(expected[manifest_resource_path.casefold()]["kind"] == "resource-manifest",
                 "project package resource manifest has the wrong file kind")
        digest = _validate_sha256(resource_manifest.get("sha256"),
                                  "project package resource_manifest.sha256")
        _require(digest == expected[manifest_resource_path.casefold()]["sha256"],
                 "project package resource manifest hash does not match its file record")
        _require(resource_manifest.get("size") == expected[manifest_resource_path.casefold()]["size"],
                 "project package resource manifest size does not match its file record")

    summary = manifest.get("summary")
    _require(isinstance(summary, Mapping), "project package summary is missing")
    _require(summary.get("file_count") == len(files) + 1,
             "project package summary file count is inconsistent")
    _require(summary.get("asset_count") == len(assets),
             "project package summary asset count is inconsistent")
    for kind, field in (("template", "template_count"), ("profile", "profile_count"),
                        ("documentation", "documentation_count")):
        _require(summary.get(field) == resource_counts[kind],
                 f"project package summary {field} is inconsistent")
    return {"manifest_kind": "project-package", "file_count": len(files),
            "asset_count": len(assets), "resource_count": len(resources),
            "template_count": resource_counts["template"],
            "profile_count": resource_counts["profile"],
            "documentation_count": resource_counts["documentation"]}


def restore_project_package(
    package_root: pathlib.Path | str,
    output_root: pathlib.Path | str,
    destination: pathlib.Path | str = DEFAULT_DESTINATION,
) -> dict[str, Any]:
    """Verify and materialize a project package into a new local directory.

    The operation is copy-only: the source package remains untouched, the
    destination must not already exist, and publication occurs only after every
    copied payload is re-hashed. The returned paths are package-relative so a
    caller can open ``project_path`` and enumerate the carried resources
    without depending on the source machine's absolute paths.
    """

    source_root = _resolve_directory(package_root, "project package")
    verification = verify_package(source_root)
    manifest = _read_json(source_root / DEFAULT_MANIFEST_NAME, "project package manifest")
    destination_root = _resolve_directory(output_root, "restore output root", create=True)
    destination_relative = canonical_relative(destination, "restore destination")
    destination_path = _output_path(destination_root, destination_relative)
    _reject_link_chain(destination_path, "restore destination")
    _require(not destination_path.exists() and not _is_link(destination_path),
             "restore destination already exists or is a symlink/junction")

    staging = pathlib.Path(tempfile.mkdtemp(prefix=f".{destination_path.name}.restore-",
                                             dir=str(destination_root)))
    try:
        for row in manifest["files"]:
            relative = canonical_relative(row.get("path"), "project package file.path")
            source = _output_path(source_root, relative)
            target = _output_path(staging, relative)
            actual, size = _copy_file(source, target, relative)
            _require(actual == row["sha256"] and size == row["size"],
                     f"restored project package payload changed during copy: {relative}")
        manifest_source = source_root / DEFAULT_MANIFEST_NAME
        _copy_file(manifest_source, _output_path(staging, DEFAULT_MANIFEST_NAME),
                   DEFAULT_MANIFEST_NAME)
        staging.replace(destination_path)
    except Exception:
        shutil.rmtree(staging, ignore_errors=True)
        raise

    return {
        **verification,
        "destination": destination_relative,
        "project_path": manifest["project"]["path"],
        "resource_paths": [resource["path"] for resource in manifest["resources"]],
    }


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--project", required=True, type=pathlib.Path,
                        help="local .bldproj file")
    parser.add_argument("--source-root", required=True, type=pathlib.Path,
                        help="explicit root for all input paths")
    parser.add_argument("--output-root", required=True, type=pathlib.Path,
                        help="root under which the package destination is created")
    parser.add_argument("--destination", default=DEFAULT_DESTINATION,
                        help=f"relative package directory under output root (default: {DEFAULT_DESTINATION})")
    parser.add_argument("--resource-manifest", type=pathlib.Path,
                        help="optional explicit template/profile/documentation manifest")
    parser.add_argument("--manifest-name", default=DEFAULT_MANIFEST_NAME,
                        help=f"relative package manifest path (default: {DEFAULT_MANIFEST_NAME})")
    args = parser.parse_args(argv)
    try:
        manifest = stage_project_package(args.project, args.source_root, args.output_root,
                                         args.destination, args.resource_manifest,
                                         args.manifest_name)
    except ProjectPackageError as exc:
        print(f"project package staging: {exc}", file=os.sys.stderr)
        return 1
    print(f"Wrote project package under {args.output_root} "
          f"({manifest['summary']['file_count']} files; audit incomplete)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
