"""Generate a deterministic source/build handoff manifest.

The command consumes an explicit JSON allowlist and an explicit source root.
Every selected file is checked before the output is written.  The resulting
manifest is a reviewable integrity record; it intentionally does not qualify a
complete source kit, licensing, SBOM, or reproducible rebuild.
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
from collections.abc import Mapping, Sequence
from typing import Any


SCHEMA_VERSION = 1
MANIFEST_VERSION = 1
CATEGORIES = ("source", "build", "docs", "licenses", "fixtures")
_CATEGORY_SET = frozenset(CATEGORIES)
_SHA256_RE = re.compile(r"^[0-9a-fA-F]{64}$")
_WINDOWS_ABSOLUTE_RE = re.compile(r"^[A-Za-z]:[\\/]")

# These are the tracked repository areas that make up the source/build
# handoff.  The list is deliberately explicit: local build trees, dependency
# checkouts, and generated output are not source-kit inputs.
SOURCE_KIT_REQUIRED_ROOTS = (
    "assets/",
    "cmake/",
    "docs/",
    "include/",
    "packaging/",
    "scripts/",
    "src/",
    "tests/",
    "third_party/",
)
SOURCE_KIT_REQUIRED_FILES = frozenset({
    ".gitattributes",
    ".gitignore",
    "CMakeLists.txt",
    "CMakePresets.json",
    "LICENSE",
    "README.md",
    "vcpkg.json",
})
_SOURCE_KIT_REQUIRED_FILE_KEYS = frozenset(
    path.casefold() for path in SOURCE_KIT_REQUIRED_FILES
)
SOURCE_KIT_EXCLUDED_DIRECTORY_NAMES = frozenset({
    ".cache",
    ".deps",
    ".git",
    ".pytest_cache",
    ".venv",
    "artifacts",
    "build",
    "dist",
    "generated",
    "local-generated",
    "out",
    "tmp",
})
SOURCE_KIT_EXCLUDED_FILE_EXTENSIONS = frozenset({
    ".env",
    ".key",
    ".pem",
    ".pfx",
    ".p12",
})
_SOURCE_KIT_EXCLUDED_FILE_EXTENSIONS_CASEFOLD = frozenset(
    extension.casefold() for extension in SOURCE_KIT_EXCLUDED_FILE_EXTENSIONS
)
_SECRET_FILENAME_RE = re.compile(
    r"(?:^|[._-])(secret|secrets|credential|credentials|password|passwd|token)(?:[._-]|$)",
    re.IGNORECASE,
)

BOUNDARY = (
    "This manifest records only explicitly allowlisted files with observed "
    "SHA-256 hashes and byte sizes. audit_status remains incomplete; it does "
    "not claim a complete source kit, complete licensing review, complete "
    "SBOM, or a reproducible Windows rebuild."
)


class ManifestError(ValueError):
    """Raised when source-kit input is unsafe, missing, or stale."""


# A more descriptive compatibility name for callers that prefer it.
SourceKitManifestError = ManifestError


def is_excluded_source_kit_path(path: str) -> bool:
    """Return whether a tracked path is outside the source-kit policy.

    This policy is applied to names returned by ``git ls-files``.  It does not
    walk the checkout and therefore cannot accidentally discover a local
    generated file or an untracked secret while refreshing the allowlist.
    """

    normalized = canonical_relative(path, "tracked path")
    parts = normalized.split("/")
    if any(part.casefold() in SOURCE_KIT_EXCLUDED_DIRECTORY_NAMES for part in parts[:-1]):
        return True
    filename = parts[-1]
    lowered = filename.casefold()
    if (
        lowered.startswith(".env")
        or pathlib.PurePosixPath(filename).suffix.casefold()
        in _SOURCE_KIT_EXCLUDED_FILE_EXTENSIONS_CASEFOLD
    ):
        return True
    return _SECRET_FILENAME_RE.search(filename) is not None


def is_required_source_kit_path(path: str) -> bool:
    """Return whether *path* belongs to the tracked source-kit scope."""

    normalized = canonical_relative(path, "tracked path")
    if is_excluded_source_kit_path(normalized):
        return False
    lowered = normalized.casefold()
    return lowered in _SOURCE_KIT_REQUIRED_FILE_KEYS or any(
        lowered.startswith(root.casefold()) for root in SOURCE_KIT_REQUIRED_ROOTS
    )


def classify_source_kit_path(path: str) -> str:
    """Assign a stable manifest category to a source-kit path.

    The classifier intentionally leaves dependency source boundaries visible:
    tracked ``third_party`` provenance is source material, while installed
    Qt/SDK and other bootstrap trees under ``.deps`` are excluded and remain
    represented by their separate dependency inventory.
    """

    normalized = canonical_relative(path, "tracked path")
    lowered = normalized.casefold()
    filename = lowered.rsplit("/", 1)[-1]
    if (
        filename == "license"
        or filename.startswith("license.")
        or "license" in filename
        or filename == "copying"
        or filename.startswith("copying.")
        or filename.startswith("notice")
        or lowered == "assets/fonts/ofl.txt"
    ):
        return "licenses"
    if lowered == "readme.md" or lowered.startswith("docs/") or lowered == "packaging/readme.md":
        return "docs"
    if lowered.startswith("assets/") or lowered.startswith("tests/fixtures/"):
        return "fixtures"
    return "source"


def _error(message: str) -> None:
    raise ManifestError(message)


def _require(condition: bool, message: str) -> None:
    if not condition:
        _error(message)


def _require_string(value: Any, field: str) -> str:
    _require(isinstance(value, str) and bool(value.strip()),
             f"{field} must be a nonempty string")
    if "\x00" in value:
        _error(f"{field} contains a forbidden NUL")
    return value.strip()


def canonical_relative(value: Any, field: str = "path") -> str:
    """Return a canonical repository-relative POSIX path.

    The check is intentionally independent of the host operating system so a
    Windows path cannot be smuggled through a Linux test runner (or vice
    versa).  Dot segments are normalized by ``PurePosixPath``; traversal
    segments are rejected before normalization can hide them.
    """

    text = _require_string(value, field)
    if text.startswith(("/", "\\")) or _WINDOWS_ABSOLUTE_RE.match(text):
        _error(f"{field} must be repository-relative")
    normalized = text.replace("\\", "/")
    # A colon is not valid in a repository-relative Windows filename (and also
    # prevents drive-relative paths and alternate data streams).
    if ":" in normalized:
        _error(f"{field} contains a forbidden colon")
    path = pathlib.PurePosixPath(normalized)
    if not path.parts or path == pathlib.PurePosixPath("."):
        _error(f"{field} must name a file")
    if ".." in path.parts:
        _error(f"{field} contains an unsafe traversal segment")
    return path.as_posix()


def list_tracked_files(source_root: pathlib.Path | str,
                       git_executable: pathlib.Path | str = "git") -> list[str]:
    """Read tracked path names from the Git index without walking the tree.

    ``git ls-files`` is intentionally the only discovery input.  In
    particular, ``--others`` is not used, so an untracked local file such as
    ``temp.txt`` cannot enter a generated source-kit allowlist.
    """

    root = _resolve_source_root(source_root)
    executable = _require_string(os.fspath(git_executable), "git executable")
    command = [executable, "-C", os.fspath(root), "ls-files", "--cached", "--full-name", "-z"]
    try:
        completed = subprocess.run(
            command,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            check=False,
        )
    except OSError as exc:
        _error(f"could not run git ls-files: {exc}")
    if completed.returncode != 0:
        try:
            detail = completed.stderr.decode("utf-8", errors="replace").strip()
        except AttributeError:
            detail = str(completed.stderr).strip()
        suffix = f": {detail}" if detail else ""
        _error(f"git ls-files failed with exit code {completed.returncode}{suffix}")
    try:
        text = completed.stdout.decode("utf-8")
    except (AttributeError, UnicodeDecodeError) as exc:
        _error(f"git ls-files returned a non-UTF-8 path: {exc}")
    paths: list[str] = []
    seen: set[str] = set()
    for value in text.split("\x00"):
        if not value:
            continue
        path = canonical_relative(value, "git ls-files path")
        key = path.casefold()
        _require(key not in seen, f"git ls-files returned duplicate path: {path}")
        seen.add(key)
        paths.append(path)
    return sorted(paths, key=lambda item: (item.casefold(), item))


def tracked_source_kit_files(source_root: pathlib.Path | str,
                             git_executable: pathlib.Path | str = "git") -> list[str]:
    """Return the required tracked source-kit paths in stable order."""

    return [
        path for path in list_tracked_files(source_root, git_executable)
        if is_required_source_kit_path(path)
    ]


def _validate_sha256(value: Any, field: str) -> str:
    _require(isinstance(value, str) and _SHA256_RE.fullmatch(value) is not None,
             f"{field} must be a 64-digit SHA-256 value")
    return value.lower()


def _is_link(path: pathlib.Path) -> bool:
    """Return whether *path* is a symlink or Windows junction."""

    try:
        if path.is_symlink():
            return True
        is_junction = getattr(path, "is_junction", None)
        return bool(is_junction is not None and is_junction())
    except OSError as exc:
        _error(f"could not inspect path {path}: {exc}")
    return False


def _reject_link_ancestors(path: pathlib.Path, field: str) -> None:
    """Reject links in an existing path chain before following it."""

    for ancestor in (path, *path.parents):
        if _is_link(ancestor):
            _error(f"{field} cannot contain a symlink or junction: {path}")


def _resolve_source_root(source_root: pathlib.Path | str) -> pathlib.Path:
    _require(source_root is not None, "source root is required")
    text = _require_string(os.fspath(source_root), "source root")
    candidate = pathlib.Path(text)
    if _is_link(candidate):
        _error(f"source root cannot be a symlink or junction: {candidate}")
    try:
        resolved = candidate.resolve(strict=True)
    except (OSError, RuntimeError) as exc:
        _error(f"source root is missing: {candidate} ({exc})")
    if not resolved.is_dir():
        _error(f"source root is not a directory: {candidate}")
    return resolved


def _resolve_allowlist_path(source_root: pathlib.Path,
                            allowlist_path: pathlib.Path | str) -> pathlib.Path:
    _require(allowlist_path is not None, "allowlist is required")
    text = _require_string(os.fspath(allowlist_path), "allowlist path")
    candidate = pathlib.Path(text)
    if not candidate.is_absolute():
        relative = canonical_relative(text, "allowlist path")
        candidate = source_root.joinpath(*pathlib.PurePosixPath(relative).parts)
    if _is_link(candidate):
        _error(f"allowlist cannot be a symlink or junction: {candidate}")
    _reject_link_ancestors(candidate, "allowlist path")
    try:
        resolved = candidate.resolve(strict=True)
    except (OSError, RuntimeError) as exc:
        _error(f"allowlist is missing: {candidate} ({exc})")
    if not resolved.is_file():
        _error(f"allowlist is not a file: {candidate}")
    return resolved


def _read_allowlist(path: pathlib.Path) -> Any:
    try:
        return json.loads(path.read_text(encoding="utf-8"))
    except (OSError, UnicodeError, json.JSONDecodeError) as exc:
        _error(f"could not read allowlist {path}: {exc}")
    raise AssertionError("unreachable")


def _allowlist_entries(document: Any) -> list[Any]:
    """Extract raw entries from the documented JSON shape.

    The normal shape is ``{"schema_version": 1, "entries": [...]}``.  A
    plain list is accepted for small programmatic callers, and category-keyed
    lists are accepted as a convenience while still normalizing to one
    canonical entry representation.
    """

    if isinstance(document, Sequence) and not isinstance(document, (str, bytes, bytearray)):
        raw_entries = document
    elif isinstance(document, Mapping):
        version = document.get("schema_version", document.get("allowlist_version"))
        if version is not None:
            _require(version == SCHEMA_VERSION,
                     f"unsupported source-kit allowlist schema_version: {version!r}")
        if "entries" in document:
            raw_entries = document["entries"]
        elif "files" in document:
            raw_entries = document["files"]
        elif "path" in document or "relative_path" in document:
            raw_entries = [document]
        else:
            raw_entries = []
            for category in CATEGORIES:
                values = document.get(category, [])
                _require(isinstance(values, list),
                         f"source-kit allowlist {category} must be a list")
                for value in values:
                    if isinstance(value, Mapping):
                        item = dict(value)
                        item.setdefault("category", category)
                    else:
                        item = {"category": category, "path": value}
                    raw_entries.append(item)
    else:
        _error("source-kit allowlist must contain an object or list")
    _require(isinstance(raw_entries, Sequence)
             and not isinstance(raw_entries, (str, bytes, bytearray))
             and bool(raw_entries),
             "source-kit allowlist entries must be a nonempty list")
    return list(raw_entries)


def _normalize_entries(document: Any) -> list[dict[str, Any]]:
    records: list[dict[str, Any]] = []
    seen: set[str] = set()
    for index, raw in enumerate(_allowlist_entries(document)):
        field = f"allowlist entries[{index}]"
        _require(isinstance(raw, Mapping), f"{field} must be an object")

        category_value = raw.get("category", raw.get("kind"))
        category = _require_string(category_value, f"{field}.category").lower()
        _require(category in _CATEGORY_SET,
                 f"{field}.category is unknown: {category!r}")

        path_value = raw.get("path", raw.get("relative_path"))
        path = canonical_relative(path_value, f"{field}.path")
        key = path.casefold()
        _require(key not in seen, f"duplicate allowlist entry: {path}")
        seen.add(key)

        expected_hash = raw.get("sha256", raw.get("expected_sha256"))
        if expected_hash is not None:
            expected_hash = _validate_sha256(expected_hash, f"{field}.sha256")

        expected_size = raw.get("size", raw.get("expected_size"))
        if expected_size is not None:
            _require(isinstance(expected_size, int) and not isinstance(expected_size, bool)
                     and expected_size >= 0,
                     f"{field}.size must be a nonnegative integer")

        records.append({
            "category": category,
            "path": path,
            "expected_sha256": expected_hash,
            "expected_size": expected_size,
        })
    return records


def check_allowlist_completeness(
    source_root: pathlib.Path | str,
    allowlist: pathlib.Path | str | Mapping[str, Any] | Sequence[Any],
    *,
    git_executable: pathlib.Path | str = "git",
) -> dict[str, Any]:
    """Compare an explicit allowlist with required tracked source-kit paths.

    The result is an inventory check, not a package qualification claim.  It
    deliberately ignores untracked files and paths excluded by policy.  A
    required path is covered only when its spelling and deterministic
    category both agree; this catches case-only filename drift on Windows as
    well as newly tracked files omitted from the reviewed allowlist.
    """

    root = _resolve_source_root(source_root)
    if isinstance(allowlist, (str, pathlib.Path, os.PathLike)):
        allowlist_file = _resolve_allowlist_path(root, allowlist)
        document = _read_allowlist(allowlist_file)
    else:
        document = allowlist
    records = _normalize_entries(document)
    tracked = list_tracked_files(root, git_executable)
    required = [path for path in tracked if is_required_source_kit_path(path)]
    excluded = [path for path in tracked if is_excluded_source_kit_path(path)]

    listed_by_casefold: dict[str, dict[str, Any]] = {}
    for record in records:
        listed_by_casefold[record["path"].casefold()] = record

    missing: list[str] = []
    case_mismatches: list[dict[str, str]] = []
    classification_mismatches: list[dict[str, str]] = []
    for path in required:
        record = listed_by_casefold.get(path.casefold())
        if record is None:
            missing.append(path)
            continue
        listed_path = record["path"]
        if listed_path != path:
            case_mismatches.append({"tracked": path, "allowlisted": listed_path})
        expected_category = classify_source_kit_path(path)
        if record["category"] != expected_category:
            classification_mismatches.append({
                "path": path,
                "expected": expected_category,
                "actual": record["category"],
            })

    required_keys = {path.casefold() for path in required}
    extras = sorted(
        (record["path"] for record in records if record["path"].casefold() not in required_keys),
        key=lambda item: (item.casefold(), item),
    )
    return {
        "complete": not (missing or case_mismatches or classification_mismatches),
        "tracked_count": len(tracked),
        "required_count": len(required),
        "allowlisted_count": len(records),
        "tracked": tracked,
        "required": required,
        "excluded": excluded,
        "missing": missing,
        "case_mismatches": case_mismatches,
        "classification_mismatches": classification_mismatches,
        "extras": extras,
    }


# Concise aliases for repository checks and external packaging callers.
verify_allowlist_completeness = check_allowlist_completeness
check_tracked_allowlist = check_allowlist_completeness


def _resolve_actual_case_path(source_root: pathlib.Path,
                              relative: str,
                              field: str) -> pathlib.Path:
    """Resolve a path while requiring each on-disk name to match its spelling.

    Windows path lookup is case-insensitive.  Enumerating each directory
    level prevents an allowlist containing ``Main.cpp`` from silently
    referring to a tracked ``main.cpp`` and makes source-kit destinations
    portable to case-sensitive consumers.
    """

    current = source_root
    for part in pathlib.PurePosixPath(relative).parts:
        if _is_link(current):
            _error(f"{field} cannot contain a symlink or junction: {relative}")
        try:
            matches = [entry for entry in os.scandir(current)
                       if entry.name.casefold() == part.casefold()]
        except OSError as exc:
            _error(f"could not inspect {field} parent for {relative}: {exc}")
        if not matches:
            _error(f"{field} missing file: {relative}")
        if len(matches) > 1:
            _error(f"{field} has ambiguous case-insensitive names: {relative}")
        actual = matches[0]
        if actual.name != part:
            _error(
                f"{field} actual filename casing differs for {relative}: "
                f"filesystem has {actual.name}"
            )
        current = pathlib.Path(actual.path)
        if _is_link(current):
            _error(f"{field} cannot contain a symlink or junction: {relative}")
    return current


def _resolve_input_file(source_root: pathlib.Path, relative: str, field: str) -> pathlib.Path:
    """Resolve one allowlisted file without following links."""

    candidate = _resolve_actual_case_path(source_root, relative, field)
    if not candidate.exists():
        _error(f"{field} missing file: {relative}")
    if not candidate.is_file():
        _error(f"{field} is not a file: {relative}")
    try:
        resolved = candidate.resolve(strict=True)
        resolved.relative_to(source_root)
    except (OSError, RuntimeError, ValueError) as exc:
        _error(f"{field} must resolve inside the source root: {relative} ({exc})")
    return resolved


def verify_actual_filename(source_root: pathlib.Path | str,
                           relative: str) -> pathlib.Path:
    """Resolve one source-kit path and verify its exact on-disk spelling."""

    root = _resolve_source_root(source_root)
    canonical = canonical_relative(relative, "path")
    return _resolve_input_file(root, canonical, "path")


def _hash_file(path: pathlib.Path) -> tuple[str, int]:
    digest = hashlib.sha256()
    size = 0
    try:
        with path.open("rb") as source:
            while chunk := source.read(1024 * 1024):
                digest.update(chunk)
                size += len(chunk)
    except OSError as exc:
        _error(f"could not read {path}: {exc}")
    return digest.hexdigest(), size


def _build_manifest(source_root: pathlib.Path,
                    records: list[dict[str, Any]]) -> tuple[dict[str, Any], set[pathlib.Path]]:
    files: list[dict[str, Any]] = []
    input_paths: set[pathlib.Path] = set()
    for index, record in enumerate(records):
        relative = record["path"]
        resolved = _resolve_input_file(source_root, relative, f"allowlist entries[{index}]")
        actual_hash, actual_size = _hash_file(resolved)
        expected_hash = record["expected_sha256"]
        if expected_hash is not None and actual_hash != expected_hash:
            _error(
                f"stale allowlist hash for {relative}: expected {expected_hash}, "
                f"received {actual_hash}"
            )
        expected_size = record["expected_size"]
        if expected_size is not None and actual_size != expected_size:
            _error(
                f"stale allowlist size for {relative}: expected {expected_size}, "
                f"received {actual_size}"
            )
        input_paths.add(resolved)
        files.append({
            "category": record["category"],
            "path": relative,
            "sha256": actual_hash,
            "size": actual_size,
        })

    files.sort(key=lambda item: (item["path"].casefold(), item["path"]))
    category_counts = {
        category: sum(item["category"] == category for item in files)
        for category in CATEGORIES
    }
    manifest: dict[str, Any] = {
        "schema_version": SCHEMA_VERSION,
        "manifest_version": MANIFEST_VERSION,
        "audit_status": "incomplete",
        "categories": list(CATEGORIES),
        "files": files,
        "category_counts": category_counts,
        "summary": {
            "entry_count": len(files),
            "total_size": sum(item["size"] for item in files),
            "category_counts": category_counts,
        },
        "boundary": BOUNDARY,
    }
    validate_manifest(manifest)
    return manifest, input_paths


def _resolve_output_path(source_root: pathlib.Path,
                         output_path: pathlib.Path | str) -> pathlib.Path:
    _require(output_path is not None, "output manifest path is required")
    text = _require_string(os.fspath(output_path), "output manifest path")
    candidate = pathlib.Path(text)
    if not candidate.is_absolute():
        relative = canonical_relative(text, "output manifest path")
        candidate = source_root.joinpath(*pathlib.PurePosixPath(relative).parts)
    _reject_link_ancestors(candidate, "output manifest path")
    if candidate.exists() and candidate.is_dir():
        _error(f"output manifest path is a directory: {candidate}")
    if candidate.exists() and _is_link(candidate):
        _error(f"output manifest path cannot be a symlink or junction: {candidate}")
    return candidate


def write_manifest(output_path: pathlib.Path | str, manifest: Mapping[str, Any]) -> None:
    """Validate and write a manifest using deterministic JSON formatting."""

    validate_manifest(manifest)
    output = pathlib.Path(output_path)
    _reject_link_ancestors(output, "output manifest path")
    if output.exists() and _is_link(output):
        _error(f"output manifest path cannot be a symlink or junction: {output}")
    if output.exists() and output.is_dir():
        _error(f"output manifest path is a directory: {output}")
    try:
        output.parent.mkdir(parents=True, exist_ok=True)
        output.write_text(
            json.dumps(manifest, ensure_ascii=False, indent=2, sort_keys=True) + "\n",
            encoding="utf-8",
        )
    except OSError as exc:
        _error(f"could not write output manifest {output}: {exc}")


def validate_manifest(manifest: Mapping[str, Any]) -> None:
    """Validate the deterministic structure emitted by this module."""

    _require(isinstance(manifest, Mapping), "source-kit manifest must contain an object")
    _require(manifest.get("schema_version") == SCHEMA_VERSION,
             f"unsupported source-kit manifest schema_version: {manifest.get('schema_version')!r}")
    _require(manifest.get("manifest_version") == MANIFEST_VERSION,
             f"unsupported source-kit manifest manifest_version: {manifest.get('manifest_version')!r}")
    _require(manifest.get("audit_status") == "incomplete",
             "source-kit manifest audit_status must remain 'incomplete'")
    _require(manifest.get("categories") == list(CATEGORIES),
             "source-kit manifest categories do not match the schema")

    files = manifest.get("files")
    _require(isinstance(files, list) and bool(files),
             "source-kit manifest files must be a nonempty list")
    seen: set[str] = set()
    previous_key: tuple[str, str] | None = None
    counts = {category: 0 for category in CATEGORIES}
    for index, entry in enumerate(files):
        field = f"source-kit manifest files[{index}]"
        _require(isinstance(entry, Mapping), f"{field} must be an object")
        category = _require_string(entry.get("category"), f"{field}.category").lower()
        _require(category in _CATEGORY_SET, f"{field}.category is unknown: {category!r}")
        path = canonical_relative(entry.get("path"), f"{field}.path")
        key = path.casefold()
        _require(key not in seen, f"source-kit manifest contains duplicate path: {path}")
        seen.add(key)
        sort_key = (key, path)
        _require(previous_key is None or sort_key >= previous_key,
                 "source-kit manifest files are not deterministically ordered")
        previous_key = sort_key
        _validate_sha256(entry.get("sha256"), f"{field}.sha256")
        size = entry.get("size")
        _require(isinstance(size, int) and not isinstance(size, bool) and size >= 0,
                 f"{field}.size must be a nonnegative integer")
        counts[category] += 1

    category_counts = manifest.get("category_counts")
    _require(category_counts == counts,
             "source-kit manifest category_counts do not match files")
    summary = manifest.get("summary")
    _require(isinstance(summary, Mapping), "source-kit manifest summary must be an object")
    _require(summary.get("entry_count") == len(files),
             "source-kit manifest summary entry_count does not match files")
    _require(summary.get("total_size") == sum(entry["size"] for entry in files),
             "source-kit manifest summary total_size does not match files")
    _require(summary.get("category_counts") == counts,
             "source-kit manifest summary category_counts do not match files")
    boundary = manifest.get("boundary")
    _require(isinstance(boundary, str) and "complete source kit" in boundary.lower()
             and "licensing" in boundary.lower() and "sbom" in boundary.lower()
             and "rebuild" in boundary.lower(),
             "source-kit manifest boundary must state its incomplete scope")


def build_manifest(source_root: pathlib.Path | str,
                   allowlist: pathlib.Path | str | Mapping[str, Any] | Sequence[Any],
                   output_path: pathlib.Path | str | None = None) -> dict[str, Any]:
    """Build a manifest from an explicit root and allowlist.

    ``allowlist`` may be the path to the JSON allowlist or an already-loaded
    object useful to callers and tests.  If ``output_path`` is supplied, the
    validated manifest is written there after all input checks pass.
    """

    root = _resolve_source_root(source_root)
    allowlist_file: pathlib.Path | None = None
    if isinstance(allowlist, (str, pathlib.Path, os.PathLike)):
        allowlist_file = _resolve_allowlist_path(root, allowlist)
        document = _read_allowlist(allowlist_file)
    else:
        document = allowlist
    records = _normalize_entries(document)
    manifest, input_paths = _build_manifest(root, records)
    if output_path is not None:
        output = _resolve_output_path(root, output_path)
        output_resolved = output.resolve(strict=False)
        output_key = pathlib.Path(os.path.normcase(str(output_resolved)))
        _require(output_key not in input_paths,
                 "output manifest path must not replace an allowlisted input file")
        if allowlist_file is not None and output_key == pathlib.Path(
                os.path.normcase(str(allowlist_file))):
            _error("output manifest path must not replace the allowlist")
        write_manifest(output, manifest)
    return manifest


def generate_manifest(source_root: pathlib.Path | str,
                      allowlist_path: pathlib.Path | str,
                      output_path: pathlib.Path | str | None = None) -> dict[str, Any]:
    """Load an explicit allowlist, build its manifest, and optionally write it."""

    return build_manifest(source_root, allowlist_path, output_path)


# Descriptive aliases for external build/handoff callers.
build_source_kit_manifest = build_manifest
generate_source_kit_manifest = generate_manifest


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source-root", "--root", dest="source_root", required=True,
                        type=pathlib.Path, help="explicit repository/source root")
    parser.add_argument("--allowlist", "--allowlist-path", dest="allowlist", required=True,
                        type=pathlib.Path, help="JSON file of explicitly allowlisted files")
    parser.add_argument("--output", "--manifest", "--output-manifest", dest="output",
                        required=False, type=pathlib.Path,
                        help="output JSON manifest path")
    parser.add_argument("--check-tracked", "--check", dest="check_tracked", action="store_true",
                        help="verify the allowlist covers required tracked source paths")
    args = parser.parse_args(argv)
    try:
        if args.check_tracked:
            report = check_allowlist_completeness(args.source_root, args.allowlist)
            status = "PASS" if report["complete"] else "FAIL"
            print(
                f"source-kit tracked check: {status}; "
                f"{report['allowlisted_count']} allowlisted, "
                f"{report['required_count']} required tracked"
            )
            for field in ("missing", "case_mismatches", "classification_mismatches"):
                for value in report[field]:
                    print(f"{field}: {value}", file=sys.stderr)
            return 0 if report["complete"] else 1
        if args.output is None:
            parser.error("--output is required unless --check-tracked is used")
        manifest = generate_manifest(args.source_root, args.allowlist, args.output)
    except ManifestError as exc:
        print(f"source-kit manifest: {exc}", file=sys.stderr)
        return 1
    print(f"Wrote {args.output} ({manifest['summary']['entry_count']} files; audit incomplete)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
