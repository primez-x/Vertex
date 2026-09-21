"""Audit the tracked source boundary between application and third-party material.

The repository publishes the application source under GPL-3.0-or-later while retaining
third-party notices and provenance for later distribution or optional publication.
This audit checks that every tracked source-kit path has exactly one declared
ownership class.  It does not decide contributor copyright, legal title, or the
license of an upstream dependency; those remain a separate review boundary.
"""

from __future__ import annotations

import argparse
import json
import os
import pathlib
import re
import subprocess
import sys
from collections import Counter
from collections.abc import Mapping, Sequence


ROOT = pathlib.Path(__file__).resolve().parents[1]
MANIFEST_RELATIVE = "third_party/source-provenance.json"
SCHEMA_VERSION = "1.0"
_WINDOWS_ABSOLUTE_RE = re.compile(r"^[A-Za-z]:[\\/]")


class ProvenanceError(ValueError):
    """Raised when the ownership manifest is malformed or unsafe."""


def _unique_pairs(pairs):
    result = {}
    for key, value in pairs:
        if key in result:
            raise ProvenanceError(f"duplicate JSON key: {key}")
        result[key] = value
    return result


def _invalid_constant(value):
    raise ProvenanceError(f"nonfinite JSON number: {value}")


def load_manifest(path: pathlib.Path | str) -> dict:
    """Load a strict UTF-8 ownership manifest."""

    selected = pathlib.Path(path)
    if selected.stat().st_size > 4 * 1024 * 1024:
        raise ProvenanceError("source provenance manifest exceeds 4 MiB")
    value = json.loads(selected.read_text(encoding="utf-8"),
                       object_pairs_hook=_unique_pairs,
                       parse_constant=_invalid_constant)
    if not isinstance(value, dict):
        raise ProvenanceError("source provenance manifest must be an object")
    return value


def canonical_relative(value: str, field: str = "path") -> str:
    if not isinstance(value, str) or not value.strip():
        raise ProvenanceError(f"{field} must be a nonempty repository-relative path")
    text = value.strip().replace("\\", "/")
    if text.startswith("/") or _WINDOWS_ABSOLUTE_RE.match(text) or ":" in text:
        raise ProvenanceError(f"{field} must be repository-relative")
    path = pathlib.PurePosixPath(text)
    if path == pathlib.PurePosixPath(".") or ".." in path.parts:
        raise ProvenanceError(f"{field} contains an unsafe traversal segment")
    return path.as_posix()


def _paths(value, field: str) -> list[str]:
    if not isinstance(value, list) or not value:
        raise ProvenanceError(f"{field} must be a nonempty list")
    result = []
    for index, item in enumerate(value):
        result.append(canonical_relative(item, f"{field}[{index}]"))
    if len({item.casefold() for item in result}) != len(result):
        raise ProvenanceError(f"{field} contains duplicate paths")
    return result


def _owner_rules(manifest: Mapping) -> dict[str, dict[str, list[str]]]:
    if manifest.get("schema_version") != SCHEMA_VERSION:
        raise ProvenanceError(f"schema_version must be {SCHEMA_VERSION}")
    ownership = manifest.get("ownership")
    if not isinstance(ownership, Mapping):
        raise ProvenanceError("ownership must be an object")
    result: dict[str, dict[str, list[str]]] = {}
    for owner in ("first_party", "third_party_provenance", "external_excluded"):
        value = ownership.get(owner)
        if not isinstance(value, Mapping):
            raise ProvenanceError(f"ownership.{owner} must be an object")
        roots = _paths(value.get("roots"), f"ownership.{owner}.roots")
        files_value = value.get("files", [])
        if not isinstance(files_value, list):
            raise ProvenanceError(f"ownership.{owner}.files must be a list")
        files = [canonical_relative(item, f"ownership.{owner}.files[{index}]")
                 for index, item in enumerate(files_value)]
        if len({item.casefold() for item in files}) != len(files):
            raise ProvenanceError(f"ownership.{owner}.files contains duplicate paths")
        result[owner] = {"roots": roots, "files": files}

    declarations = manifest.get("third_party_manifests")
    if not isinstance(declarations, list) or not declarations:
        raise ProvenanceError("third_party_manifests must be a nonempty list")
    declared = [canonical_relative(item, f"third_party_manifests[{index}]")
                for index, item in enumerate(declarations)]
    if len({item.casefold() for item in declared}) != len(declared):
        raise ProvenanceError("third_party_manifests contains duplicate paths")
    result["_declared_manifests"] = {"roots": [], "files": declared}

    # Every rule is tested case-insensitively because source kits are consumed
    # on Windows, where a case-only path collision is not a separate file.
    locations: list[tuple[str, str, str]] = []
    for owner, rule in result.items():
        if owner.startswith("_"):
            continue
        for kind in ("roots", "files"):
            for path in rule[kind]:
                locations.append((path.casefold(), owner, kind))
    def _overlaps(left_kind: str, left: str, right_kind: str, right: str) -> bool:
        if left_kind == right_kind == "files":
            return left == right
        if left_kind == "root":
            left_base, right_base = left.rstrip("/"), right.rstrip("/")
        else:
            left_base, right_base = right.rstrip("/"), left.rstrip("/")
        if left_kind == right_kind == "roots":
            return (left_base == right_base or left_base.startswith(right_base + "/") or
                    right_base.startswith(left_base + "/"))
        file_path = right if left_kind == "root" else left
        root_path = left_base
        return file_path == root_path or file_path.startswith(root_path + "/")

    for index, (left, left_owner, left_kind) in enumerate(locations):
        for right, right_owner, right_kind in locations[index + 1:]:
            if _overlaps("root" if left_kind == "roots" else left_kind,
                         left, "root" if right_kind == "roots" else right_kind, right):
                if left_owner != right_owner:
                    raise ProvenanceError(
                        f"ownership rules overlap: {left_owner}.{left_kind} {left} and "
                        f"{right_owner}.{right_kind} {right}")
    return result


def default_manifest() -> dict:
    """Return the checked-in policy used by the repository."""

    return {
        "schema_version": SCHEMA_VERSION,
        "ownership": {
            "first_party": {
                "roots": [
                    "assets/", "cmake/", "docs/", "include/", "packaging/",
                    "scripts/", "src/", "tests/",
                ],
                "files": [
                    ".gitattributes", ".gitignore", "CMakeLists.txt",
                    "CMakePresets.json", "LICENSE", "README.md", "vcpkg.json",
                ],
            },
            "third_party_provenance": {"roots": ["third_party/"], "files": []},
            "external_excluded": {
                "roots": [
                    ".deps/", "artifacts/", "build/", "dist/", "generated/",
                    "local-generated/", "out/", "tmp/",
                ],
                "files": [],
            },
        },
        "third_party_manifests": [
            "third_party/dependencies.json",
            "third_party/distribution-components.json",
        ],
        "boundary": (
            "First-party application source and project-authored metadata are "
            "separated from third-party provenance records. Contributor rights, "
            "legal title, and upstream license review remain independent checks."
        ),
    }


def _classify(path: str, rules: Mapping) -> tuple[str | None, bool]:
    normalized = canonical_relative(path)
    folded = normalized.casefold()
    matches: list[tuple[str, bool]] = []
    for owner in ("first_party", "third_party_provenance", "external_excluded"):
        rule = rules[owner]
        if any(folded == item.casefold() for item in rule["files"]):
            matches.append((owner, False))
        if any(_root_matches(folded, item) for item in rule["roots"]):
            matches.append((owner, True))
    owners = {owner for owner, _ in matches}
    if len(owners) > 1:
        return None, False
    if not matches:
        return None, False
    owner = next(iter(owners))
    return owner, owner == "external_excluded"


def _root_matches(path: str, root: str) -> bool:
    """Match a repository path against a directory root at a segment boundary."""

    base = root.rstrip("/").casefold()
    return path == base or path.startswith(base + "/")


def audit_paths(paths: Sequence[str], manifest: Mapping | None = None,
                *, root: pathlib.Path | None = None) -> dict:
    """Audit a supplied deterministic path list without invoking Git."""

    errors: list[str] = []
    try:
        rules = _owner_rules(manifest or default_manifest())
    except (ProvenanceError, TypeError) as error:
        return {
            "schema_version": SCHEMA_VERSION,
            "audit_status": "blocked",
            "tracked_path_count": 0,
            "ownership_counts": {},
            "paths": [],
            "errors": [str(error)],
            "boundary": "Tracked Git paths only; legal contributor and license review remain separate.",
        }

    normalized_paths: list[str] = []
    seen: set[str] = set()
    for index, raw in enumerate(paths):
        try:
            normalized = canonical_relative(raw, f"tracked_paths[{index}]")
        except ProvenanceError as error:
            errors.append(str(error))
            continue
        folded = normalized.casefold()
        if folded in seen:
            errors.append(f"tracked path appears more than once: {normalized}")
            continue
        seen.add(folded)
        normalized_paths.append(normalized)

    classified: list[dict[str, str]] = []
    counts = Counter()
    for normalized in sorted(normalized_paths, key=str.casefold):
        owner, external = _classify(normalized, rules)
        if owner is None:
            errors.append(f"{normalized}: no ownership rule or overlapping ownership rules")
            continue
        if external:
            errors.append(f"{normalized}: external generated/dependency path is tracked")
            continue
        counts[owner] += 1
        classified.append({"path": normalized, "owner": owner})

    # The manifest itself is third-party-provenance metadata by path, but each
    # declared manifest must also exist when auditing a real checkout.
    if root is not None:
        selected_root = pathlib.Path(root).resolve()
        for path in rules["_declared_manifests"]["files"]:
            candidate = (selected_root / pathlib.PurePosixPath(path)).resolve()
            if not candidate.is_relative_to(selected_root) or not candidate.is_file():
                errors.append(f"declared third-party manifest is missing: {path}")

    return {
        "schema_version": SCHEMA_VERSION,
        "audit_status": "pass" if not errors else "blocked",
        "tracked_path_count": len(normalized_paths),
        "ownership_counts": {key: counts[key] for key in
                              ("first_party", "third_party_provenance")},
        "paths": classified,
        "errors": sorted(set(errors)),
        "boundary": (
            "Tracked Git paths only; build/dependency trees and generated artifacts "
            "are excluded. This proves structural separation, not contributor title "
            "or legal license clearance."
        ),
    }


def list_tracked_files(root: pathlib.Path | str) -> list[str]:
    selected = pathlib.Path(root).resolve()
    completed = subprocess.run(
        ["git", "-C", os.fspath(selected), "ls-files", "--cached", "--full-name", "-z"],
        stdout=subprocess.PIPE, stderr=subprocess.PIPE, check=False,
    )
    if completed.returncode != 0:
        detail = completed.stderr.decode("utf-8", errors="replace").strip()
        raise ProvenanceError(f"git ls-files failed: {detail or completed.returncode}")
    raw = completed.stdout.decode("utf-8", errors="strict")
    return [item for item in raw.split("\0") if item]


def audit_repository(root: pathlib.Path | str = ROOT,
                     manifest_path: pathlib.Path | str | None = None) -> dict:
    selected_root = pathlib.Path(root).resolve()
    selected_manifest = (selected_root / MANIFEST_RELATIVE if manifest_path is None
                         else pathlib.Path(manifest_path))
    try:
        manifest = load_manifest(selected_manifest)
        paths = list_tracked_files(selected_root)
        return audit_paths(paths, manifest, root=selected_root)
    except (OSError, ValueError, UnicodeError) as error:
        return {
            "schema_version": SCHEMA_VERSION,
            "audit_status": "blocked",
            "tracked_path_count": 0,
            "ownership_counts": {},
            "paths": [],
            "errors": [str(error)],
            "boundary": "Tracked Git paths only; legal contributor and license review remain separate.",
        }


def main(argv: Sequence[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", type=pathlib.Path, default=ROOT)
    parser.add_argument("--manifest", type=pathlib.Path)
    args = parser.parse_args(argv)
    report = audit_repository(args.root, args.manifest)
    print(json.dumps(report, indent=2, sort_keys=True) + "\n")
    return 0 if report["audit_status"] == "pass" else 1


if __name__ == "__main__":
    sys.exit(main())
