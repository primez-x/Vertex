"""Stage a bounded portable package from an evidence-backed inventory.

The command copies runtime binaries named by a generated distribution inventory
and source, asset, and notice files named by an explicit allowlist.  Every
input is resolved below the caller-provided source root and every output is
resolved below the caller-provided output root.  The resulting manifest is an
integrity record only; it deliberately leaves installer and offline
qualification incomplete.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import pathlib
import re
import shutil
import sys
from typing import Any


SCHEMA_VERSION = 1
MANIFEST_VERSION = 1
SHA256_RE = re.compile(r"^[0-9a-fA-F]{64}$")
WINDOWS_ABSOLUTE_RE = re.compile(r"^[A-Za-z]:[\\/]")
ALLOWLIST_KINDS = {"asset", "source", "notice"}
DEFAULT_DESTINATION = "portable"
DEFAULT_MANIFEST_NAME = "portable-package-manifest.json"


class StagingError(ValueError):
    """Raised when package inputs are incomplete, stale, or unsafe."""


def _error(message: str) -> None:
    raise StagingError(message)


def _require(condition: bool, message: str) -> None:
    if not condition:
        _error(message)


def _require_string(value: Any, field: str) -> str:
    _require(isinstance(value, str) and bool(value.strip()),
             f"{field} must be a nonempty string")
    return value.strip()


def canonical_relative(value: Any, field: str) -> str:
    """Return a safe, POSIX relative path or raise ``StagingError``."""

    text = _require_string(value, field)
    if "\x00" in text:
        _error(f"{field} contains a forbidden NUL")
    if text.startswith(("/", "\\")) or WINDOWS_ABSOLUTE_RE.match(text):
        _error(f"{field} must be relative")
    normalized = text.replace("\\", "/")
    if ":" in normalized:
        _error(f"{field} contains a forbidden colon")
    path = pathlib.PurePosixPath(normalized)
    if path == pathlib.PurePosixPath(".") or not path.parts or ".." in path.parts:
        _error(f"{field} contains an unsafe path")
    return path.as_posix()


def _validate_sha256(value: Any, field: str) -> str:
    _require(isinstance(value, str) and SHA256_RE.fullmatch(value) is not None,
             f"{field} must be a 64-digit SHA-256 value")
    return value.lower()


def sha256_file(path: pathlib.Path) -> str:
    try:
        with path.open("rb") as source:
            return hashlib.file_digest(source, "sha256").hexdigest()
    except OSError as exc:
        _error(f"could not hash {path}: {exc}")
    raise AssertionError("unreachable")


def _read_json(path: pathlib.Path, field: str) -> dict[str, Any]:
    try:
        value = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, UnicodeError, json.JSONDecodeError) as exc:
        _error(f"malformed {field} JSON at {path}: {exc}")
    _require(isinstance(value, dict), f"{field} JSON must contain an object")
    return value


def _resolve_directory(value: pathlib.Path | str, field: str, *, create: bool = False) -> pathlib.Path:
    candidate = pathlib.Path(value)
    if candidate.is_symlink():
        _error(f"{field} cannot be a symlink: {candidate}")
    if create:
        if candidate.exists() and not candidate.is_dir():
            _error(f"{field} is not a directory: {candidate}")
        try:
            return candidate.resolve()
        except OSError as exc:
            _error(f"could not resolve {field} {candidate}: {exc}")
    try:
        resolved = candidate.resolve(strict=True)
    except (OSError, RuntimeError) as exc:
        _error(f"{field} is missing: {candidate} ({exc})")
    if not resolved.is_dir():
        _error(f"{field} is not a directory: {candidate}")
    return resolved


def _resolve_inventory(path: pathlib.Path | str, source_root: pathlib.Path) -> tuple[pathlib.Path, str]:
    candidate = pathlib.Path(path)
    if not candidate.is_absolute():
        relative = canonical_relative(str(candidate), "distribution inventory argument")
        candidate = source_root.joinpath(*pathlib.PurePosixPath(relative).parts)
    try:
        resolved = candidate.resolve(strict=True)
    except (OSError, RuntimeError) as exc:
        _error(f"distribution inventory is missing: {candidate} ({exc})")
    if not resolved.is_file():
        _error(f"distribution inventory is not a file: {candidate}")
    try:
        relative = resolved.relative_to(source_root).as_posix()
    except ValueError:
        _error("distribution inventory must be inside the explicit source root")
    return resolved, canonical_relative(relative, "distribution inventory path")


def _resolve_input_file(source_root: pathlib.Path, relative: str, field: str) -> pathlib.Path:
    """Resolve an input file without following an input symlink."""

    candidate = source_root.joinpath(*pathlib.PurePosixPath(relative).parts)
    current = source_root
    for part in pathlib.PurePosixPath(relative).parts:
        current = current / part
        if current.is_symlink():
            _error(f"{field} cannot contain a symlink: {relative}")
    if not candidate.exists():
        _error(f"{field} missing input: {relative}")
    if candidate.is_symlink():
        _error(f"{field} cannot be a symlink: {relative}")
    try:
        resolved = candidate.resolve(strict=True)
        resolved.relative_to(source_root)
    except (OSError, RuntimeError, ValueError) as exc:
        _error(f"{field} must resolve inside the explicit source root: {relative} ({exc})")
    if not resolved.is_file():
        _error(f"{field} is not a file: {relative}")
    return resolved


def _validate_inventory(inventory: dict[str, Any]) -> tuple[list[dict[str, Any]], dict[str, dict[str, Any]]]:
    _require(inventory.get("schema_version") == SCHEMA_VERSION,
             f"unsupported distribution inventory schema_version: {inventory.get('schema_version')!r}")
    _require(inventory.get("inventory_version") == SCHEMA_VERSION,
             f"unsupported distribution inventory inventory_version: {inventory.get('inventory_version')!r}")
    _require(inventory.get("audit_status") == "incomplete",
             "distribution inventory audit_status must remain 'incomplete'")
    _require(inventory.get("distribution_qualified", False) is False,
             "distribution inventory cannot claim distribution qualification")
    summary = inventory.get("summary", {})
    _require(isinstance(summary, dict), "distribution inventory summary must be an object")
    _require(summary.get("installer_qualified", False) is False,
             "distribution inventory cannot claim installer qualification")
    _require(summary.get("offline_qualified", False) is False,
             "distribution inventory cannot claim offline qualification")

    components = inventory.get("components")
    _require(isinstance(components, list), "distribution inventory components must be a list")
    component_index: dict[str, dict[str, Any]] = {}
    for index, component in enumerate(components):
        field = f"inventory.components[{index}]"
        _require(isinstance(component, dict), f"{field} must be an object")
        identifier = _require_string(component.get("id"), f"{field}.id")
        _require(identifier not in component_index,
                 f"distribution inventory repeats component {identifier}")
        component_index[identifier] = component

    binaries = inventory.get("binaries")
    _require(isinstance(binaries, list) and bool(binaries),
             "distribution inventory binaries must be a nonempty list")
    return binaries, component_index


def _path_record(value: Any, field: str) -> tuple[str, str]:
    _require(isinstance(value, dict), f"{field} must be an object")
    relative = canonical_relative(value.get("path"), f"{field}.path")
    digest = _validate_sha256(value.get("sha256"), f"{field}.sha256")
    return relative, digest


def _add_known_record(records: dict[tuple[str, str], str], component_id: str,
                      path: str, digest: str, field: str) -> None:
    key = (component_id, path.casefold())
    previous = records.get(key)
    if previous is not None and previous != digest:
        _error(f"{field} has conflicting hashes for {component_id}:{path}")
    records[key] = digest


def _inventory_file_records(
    inventory: dict[str, Any],
    component_index: dict[str, dict[str, Any]],
) -> tuple[dict[tuple[str, str], str], dict[tuple[str, str], str]]:
    """Index source inputs and notices that an allowlist may name."""

    source_records: dict[tuple[str, str], str] = {}
    notice_records: dict[tuple[str, str], str] = {}

    def add_component_records(component_id: str, row: dict[str, Any], field: str) -> None:
        for name in ("source_inputs", "sources", "assets"):
            values = row.get(name, [])
            _require(isinstance(values, list), f"{field}.{name} must be a list")
            for index, value in enumerate(values):
                path, digest = _path_record(value, f"{field}.{name}[{index}]")
                _add_known_record(source_records, component_id, path, digest,
                                  f"{field}.{name}[{index}]")
        values = row.get("notices", [])
        _require(isinstance(values, list), f"{field}.notices must be a list")
        for index, value in enumerate(values):
            path, digest = _path_record(value, f"{field}.notices[{index}]")
            _add_known_record(notice_records, component_id, path, digest,
                              f"{field}.notices[{index}]")

    for component_id, component in component_index.items():
        add_component_records(component_id, component, f"inventory.components[{component_id}]")

    static_inputs = inventory.get("static_inputs", [])
    _require(isinstance(static_inputs, list), "distribution inventory static_inputs must be a list")
    for index, item in enumerate(static_inputs):
        field = f"inventory.static_inputs[{index}]"
        _require(isinstance(item, dict), f"{field} must be an object")
        component_id = _require_string(item.get("component_id"), f"{field}.component_id")
        _require(component_id in component_index,
                 f"{field} names an unknown inventory component {component_id}")
        add_component_records(component_id, item, field)
    return source_records, notice_records


def _allowlist_entries(allowlist: dict[str, Any]) -> list[dict[str, Any]]:
    version = allowlist.get("schema_version", allowlist.get("allowlist_version"))
    _require(version == SCHEMA_VERSION,
             f"unsupported portable allowlist schema_version: {version!r}")
    if "entries" in allowlist:
        raw_entries = allowlist["entries"]
        _require(isinstance(raw_entries, list), "portable allowlist entries must be a list")
        entries = list(raw_entries)
    else:
        entries = []
        for name, kind in (("sources", "source"), ("assets", "asset"), ("notices", "notice")):
            values = allowlist.get(name, [])
            _require(isinstance(values, list), f"portable allowlist {name} must be a list")
            for value in values:
                _require(isinstance(value, dict), f"portable allowlist {name} entries must be objects")
                copy = dict(value)
                copy.setdefault("kind", kind)
                entries.append(copy)
    result: list[dict[str, Any]] = []
    for index, entry in enumerate(entries):
        _require(isinstance(entry, dict), f"portable allowlist entries[{index}] must be an object")
        kind = _require_string(entry.get("kind"), f"portable allowlist entries[{index}].kind").lower()
        _require(kind in ALLOWLIST_KINDS,
                 f"portable allowlist entries[{index}] has unknown kind {kind!r}")
        inventory_entry = entry.get("inventory_entry", entry.get("component_id"))
        inventory_entry = _require_string(
            inventory_entry, f"portable allowlist entries[{index}].inventory_entry")
        source_value = entry.get("path")
        if source_value is None:
            source_value = entry.get("source_path", entry.get("source"))
        source_path = canonical_relative(
            source_value, f"portable allowlist entries[{index}].path")
        destination = canonical_relative(
            entry.get("destination"), f"portable allowlist entries[{index}].destination")
        supplied_hash = entry.get("sha256")
        if supplied_hash is not None:
            supplied_hash = _validate_sha256(
                supplied_hash, f"portable allowlist entries[{index}].sha256")
        result.append({
            "kind": kind,
            "inventory_entry": inventory_entry,
            "source": source_path,
            "destination": destination,
            "sha256": supplied_hash,
            "index": index,
        })
    return result


def _output_path(root: pathlib.Path, relative: str) -> pathlib.Path:
    candidate = root.joinpath(*pathlib.PurePosixPath(relative).parts)
    try:
        resolved = candidate.resolve()
        resolved.relative_to(root)
    except (OSError, RuntimeError, ValueError) as exc:
        _error(f"output path escapes the output root: {relative} ({exc})")
    return candidate


def _reject_output_symlinks(root: pathlib.Path, relative: str) -> None:
    current = root
    for part in pathlib.PurePosixPath(relative).parts:
        current = current / part
        if current.is_symlink():
            _error(f"output path contains a symlink: {relative}")


def _copy_file(source: pathlib.Path, destination: pathlib.Path, relative: str) -> None:
    try:
        destination.parent.mkdir(parents=True, exist_ok=True)
        if destination.is_symlink():
            _error(f"output path contains a symlink: {relative}")
        shutil.copy2(source, destination)
    except StagingError:
        raise
    except OSError as exc:
        _error(f"could not copy {source} to {relative}: {exc}")


def _verify_copied_file(destination: pathlib.Path, expected: str, relative: str) -> None:
    """Verify the bytes that were actually staged before publishing a manifest."""

    actual = sha256_file(destination)
    _require(actual == expected,
             f"staged hash mismatch for {relative}: expected {expected}, received {actual}")


def stage_package(
    inventory_path: pathlib.Path | str,
    allowlist_path: pathlib.Path | str,
    source_root: pathlib.Path | str,
    output_root: pathlib.Path | str,
    destination: pathlib.Path | str = DEFAULT_DESTINATION,
    manifest_name: pathlib.Path | str = DEFAULT_MANIFEST_NAME,
) -> dict[str, Any]:
    """Stage and manifest a portable package from explicit local inputs.

    ``source_root`` and ``output_root`` are required intentionally.  The
    function never searches the current repository, a build directory, PATH,
    or an installed developer SDK for omitted inputs.
    """

    source_root_path = _resolve_directory(source_root, "source root")
    output_root_path = _resolve_directory(output_root, "output root", create=True)
    inventory_file, inventory_relative = _resolve_inventory(inventory_path, source_root_path)
    allowlist_file = pathlib.Path(allowlist_path)
    if not allowlist_file.is_absolute():
        relative = canonical_relative(str(allowlist_file), "portable allowlist argument")
        allowlist_file = source_root_path.joinpath(*pathlib.PurePosixPath(relative).parts)
    try:
        allowlist_file = allowlist_file.resolve(strict=True)
    except (OSError, RuntimeError) as exc:
        _error(f"portable allowlist is missing: {allowlist_path} ({exc})")
    if not allowlist_file.is_file():
        _error(f"portable allowlist is not a file: {allowlist_path}")
    try:
        allowlist_file.relative_to(source_root_path)
    except ValueError:
        _error("portable allowlist must be inside the explicit source root")

    inventory = _read_json(inventory_file, "distribution inventory")
    binaries, component_index = _validate_inventory(inventory)
    source_records, notice_records = _inventory_file_records(inventory, component_index)
    allowlist_entries = _allowlist_entries(_read_json(allowlist_file, "portable allowlist"))

    package_relative = canonical_relative(destination, "package destination")
    package_root = _output_path(output_root_path, package_relative)
    manifest_relative = canonical_relative(manifest_name, "manifest name")

    destination_keys: dict[str, str] = {}
    binary_names: set[str] = set()
    plan: list[dict[str, Any]] = []

    def reserve(relative: str, field: str) -> None:
        key = relative.casefold()
        for previous_key, previous in destination_keys.items():
            if (key == previous_key or key.startswith(previous_key + "/")
                    or previous_key.startswith(key + "/")):
                _error(f"duplicate destination path: {relative} conflicts with {previous}")
        destination_keys[key] = relative

    for index, binary in enumerate(binaries):
        field = f"inventory.binaries[{index}]"
        _require(isinstance(binary, dict), f"{field} must be an object")
        name = _require_string(binary.get("name"), f"{field}.name")
        binary_key = name.casefold()
        _require(binary_key not in binary_names,
                 f"distribution inventory repeats runtime binary {name}")
        binary_names.add(binary_key)
        component_id = _require_string(binary.get("component_id"), f"{field}.component_id")
        _require(component_id in component_index,
                 f"{field} names an unknown inventory component {component_id}")
        component = component_index[component_id]
        _require(component.get("distribution_status", "included") != "excluded",
                 f"{field} belongs to an excluded inventory component {component_id}")
        source = canonical_relative(binary.get("path"), f"{field}.path")
        _require(pathlib.PurePosixPath(source).name.casefold() == name.casefold(),
                 f"{field}.name does not match its runtime path")
        destination_path = canonical_relative(binary.get("destination"), f"{field}.destination")
        expected = _validate_sha256(binary.get("sha256"), f"{field}.sha256")
        reserve(destination_path, f"{field}.destination")
        source_file = _resolve_input_file(source_root_path, source, field)
        actual = sha256_file(source_file)
        _require(actual == expected,
                 f"stale runtime hash for {source}: expected {expected}, received {actual}")
        plan.append({
            "kind": "binary",
            "inventory_entry": component_id,
            "component_id": component_id,
            "source": source,
            "path": destination_path,
            "sha256": actual,
            "source_file": source_file,
        })

    for entry in allowlist_entries:
        component_id = entry["inventory_entry"]
        component = component_index.get(component_id)
        if component is None:
            _error(f"unknown inventory entry: {component_id}")
        if component.get("distribution_status", "included") == "excluded":
            _error(f"allowlist entry names an excluded inventory entry: {component_id}")
        source = entry["source"]
        if entry["kind"] == "notice":
            expected = notice_records.get((component_id, source.casefold()))
        else:
            expected = source_records.get((component_id, source.casefold()))
        if expected is None:
            _error(f"unknown inventory entry: {component_id}:{source}")
        if entry["sha256"] is not None and entry["sha256"] != expected:
            _error(
                f"allowlist hash disagrees with inventory for {component_id}:{source}")
        destination_path = entry["destination"]
        reserve(destination_path, f"allowlist entries[{entry['index']}].destination")
        source_file = _resolve_input_file(
            source_root_path, source, f"allowlist entries[{entry['index']}]")
        actual = sha256_file(source_file)
        _require(actual == expected,
                 f"stale allowlist input hash for {source}: expected {expected}, received {actual}")
        plan.append({
            "kind": entry["kind"],
            "inventory_entry": component_id,
            "component_id": component_id,
            "source": source,
            "path": destination_path,
            "sha256": actual,
            "source_file": source_file,
        })

    reserve(manifest_relative, "manifest name")
    inventory_hash = sha256_file(inventory_file)
    files = [
        {
            "kind": row["kind"],
            "inventory_entry": row["inventory_entry"],
            "component_id": row["component_id"],
            "source": row["source"],
            "path": row["path"],
            "sha256": row["sha256"],
        }
        for row in sorted(plan, key=lambda item: item["path"].casefold())
    ]
    counts = {kind: sum(row["kind"] == kind for row in plan)
              for kind in ("binary", "source", "asset", "notice")}
    manifest = {
        "schema_version": SCHEMA_VERSION,
        "manifest_version": MANIFEST_VERSION,
        "audit_status": "incomplete",
        "installer_qualified": False,
        "offline_qualified": False,
        "source_inventory": {
            "path": inventory_relative,
            "sha256": inventory_hash,
        },
        "source_inventory_sha256": inventory_hash,
        "files": files,
        "summary": {
            "file_count": len(files),
            "binary_count": counts["binary"],
            "source_count": counts["source"],
            "asset_count": counts["asset"],
            "notice_count": counts["notice"],
            "installer_qualified": False,
            "offline_qualified": False,
        },
        "boundary": (
            "This is a staged file set with integrity hashes only. It does not "
            "qualify an installer, clean-machine installation, offline behavior, "
            "complete dynamic-load coverage, licensing, or redistributability."
        ),
    }

    _reject_output_symlinks(output_root_path, package_relative)
    if package_root.exists():
        if not package_root.is_dir():
            _error(f"package destination is not a directory: {package_root}")
        try:
            if any(package_root.iterdir()):
                _error("package destination must be empty before staging")
        except OSError as exc:
            _error(f"could not inspect package destination {package_root}: {exc}")
    else:
        package_root.mkdir(parents=True, exist_ok=False)
    for row in plan:
        _reject_output_symlinks(output_root_path, package_relative + "/" + row["path"])
        destination_file = _output_path(package_root, row["path"])
        _copy_file(row["source_file"], destination_file, row["path"])
        _verify_copied_file(destination_file, row["sha256"], row["path"])
    _reject_output_symlinks(output_root_path, package_relative + "/" + manifest_relative)
    manifest_path = _output_path(package_root, manifest_relative)
    try:
        manifest_path.parent.mkdir(parents=True, exist_ok=True)
        manifest_path.write_text(json.dumps(manifest, indent=2, sort_keys=True) + "\n",
                                 encoding="utf-8")
    except OSError as exc:
        _error(f"could not write portable package manifest {manifest_path}: {exc}")
    return manifest


def stage_portable_package(
    inventory_path: pathlib.Path | str,
    allowlist_path: pathlib.Path | str,
    source_root: pathlib.Path | str,
    output_root: pathlib.Path | str,
    destination: pathlib.Path | str = DEFAULT_DESTINATION,
    manifest_name: pathlib.Path | str = DEFAULT_MANIFEST_NAME,
) -> dict[str, Any]:
    """Compatibility-named wrapper for :func:`stage_package`."""

    return stage_package(inventory_path, allowlist_path, source_root, output_root,
                         destination, manifest_name)


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--inventory", "--inventory-path", dest="inventory", required=True,
                        type=pathlib.Path, help="generated distribution inventory JSON")
    parser.add_argument("--allowlist", dest="allowlist", required=True,
                        type=pathlib.Path, help="explicit source/asset/notice allowlist JSON")
    parser.add_argument("--source-root", "--root", dest="source_root", required=True,
                        type=pathlib.Path, help="explicit root for all input paths")
    parser.add_argument("--output-root", "--output", dest="output_root", required=True,
                        type=pathlib.Path, help="root under which the package destination is created")
    parser.add_argument("--destination", default=DEFAULT_DESTINATION,
                        help=f"relative package directory under output root (default: {DEFAULT_DESTINATION})")
    parser.add_argument("--manifest-name", "--manifest", dest="manifest_name",
                        default=DEFAULT_MANIFEST_NAME,
                        help=f"relative manifest path inside the package (default: {DEFAULT_MANIFEST_NAME})")
    args = parser.parse_args(argv)
    try:
        manifest = stage_package(args.inventory, args.allowlist, args.source_root,
                                 args.output_root, args.destination, args.manifest_name)
    except StagingError as exc:
        print(f"portable package staging: {exc}", file=sys.stderr)
        return 1
    print(f"Wrote portable package manifest under {args.output_root} "
          f"({manifest['summary']['file_count']} files; audit incomplete)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
