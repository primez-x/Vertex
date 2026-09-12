"""Refresh or verify the tracked source-kit allowlist.

The updater discovers paths only through ``git ls-files``.  It emits a stable,
explicit allowlist and has a read-only ``--check`` mode for repository checks.
The source-kit manifest generator remains the fail-closed staging boundary:
this helper never expands an allowlist during package staging.
"""

from __future__ import annotations

import argparse
import importlib.util
import json
import os
import pathlib
import sys
from collections.abc import Mapping, Sequence
from typing import Any


def _load_manifest_module():
    try:
        import source_kit_manifest as module
        return module
    except ImportError:
        path = pathlib.Path(__file__).with_name("source_kit_manifest.py")
        spec = importlib.util.spec_from_file_location("source_kit_manifest_for_allowlist", path)
        if spec is None or spec.loader is None:
            raise ImportError(f"could not load {path}")
        module = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(module)
        return module


manifest = _load_manifest_module()
ManifestError = manifest.ManifestError

DEFAULT_ALLOWLIST = pathlib.Path("packaging/source-kit-allowlist.json")


def build_allowlist(
    source_root: pathlib.Path | str,
    *,
    git_executable: pathlib.Path | str = "git",
    tracked_paths: Sequence[str] | None = None,
) -> dict[str, Any]:
    """Build a deterministic allowlist from the tracked Git path set.

    ``tracked_paths`` is available for callers that already hold a verified
    Git path list and for unit tests.  The command-line path always obtains
    its input through ``git ls-files``.
    """

    root = manifest._resolve_source_root(source_root)
    paths = list(tracked_paths) if tracked_paths is not None else manifest.list_tracked_files(
        root, git_executable
    )
    entries = [
        {
            "category": manifest.classify_source_kit_path(path),
            "path": manifest.canonical_relative(path, "tracked path"),
        }
        for path in paths
        if manifest.is_required_source_kit_path(path)
    ]
    entries.sort(key=lambda entry: (entry["path"].casefold(), entry["path"]))
    return {"schema_version": manifest.SCHEMA_VERSION, "entries": entries}


def _resolve_output_inside_root(source_root: pathlib.Path,
                                output_path: pathlib.Path | str) -> pathlib.Path:
    output = manifest._resolve_output_path(source_root, output_path)
    try:
        output.resolve(strict=False).relative_to(source_root)
    except (OSError, RuntimeError, ValueError):
        raise ManifestError("allowlist output must be inside the source root")
    return output


def write_allowlist(
    source_root: pathlib.Path | str,
    output_path: pathlib.Path | str,
    document: Mapping[str, Any],
) -> pathlib.Path:
    """Atomically write a deterministic allowlist inside *source_root*."""

    root = manifest._resolve_source_root(source_root)
    output = _resolve_output_inside_root(root, output_path)
    entries = document.get("entries")
    if not isinstance(entries, Sequence) or isinstance(entries, (str, bytes, bytearray)):
        raise ManifestError("source-kit allowlist entries must be a list")
    normalized_entries = []
    for index, entry in enumerate(entries):
        if not isinstance(entry, Mapping):
            raise ManifestError(f"source-kit allowlist entries[{index}] must be an object")
        category = manifest._require_string(
            entry.get("category"), f"source-kit allowlist entries[{index}].category"
        ).lower()
        path = manifest.canonical_relative(
            entry.get("path"), f"source-kit allowlist entries[{index}].path"
        )
        normalized_entries.append({"category": category, "path": path})
    normalized_entries.sort(key=lambda entry: (entry["path"].casefold(), entry["path"]))
    canonical = {"schema_version": manifest.SCHEMA_VERSION, "entries": normalized_entries}
    # Reuse the manifest parser so generated output obeys the same category,
    # path, and duplicate rules as the fail-closed consumer.
    manifest._normalize_entries(canonical)
    temporary = output.with_name(f".{output.name}.tmp")
    try:
        output.parent.mkdir(parents=True, exist_ok=True)
        manifest._reject_link_ancestors(output, "allowlist output path")
        if output.exists() and manifest._is_link(output):
            raise ManifestError(f"allowlist output cannot be a symlink or junction: {output}")
        if output.exists() and output.is_dir():
            raise ManifestError(f"allowlist output is a directory: {output}")
        manifest._reject_link_ancestors(temporary, "allowlist temporary path")
        if temporary.exists() and manifest._is_link(temporary):
            raise ManifestError(f"allowlist temporary path cannot be a symlink: {temporary}")
        temporary.write_text(
            json.dumps(canonical, ensure_ascii=False, indent=2) + "\n",
            encoding="utf-8",
        )
        os.replace(temporary, output)
    except ManifestError:
        raise
    except OSError as exc:
        try:
            if temporary.exists() and not manifest._is_link(temporary):
                temporary.unlink()
        except OSError:
            pass
        raise ManifestError(f"could not write allowlist {output}: {exc}")
    return output


def refresh_allowlist(
    source_root: pathlib.Path | str,
    output_path: pathlib.Path | str = DEFAULT_ALLOWLIST,
    *,
    git_executable: pathlib.Path | str = "git",
) -> dict[str, Any]:
    """Generate and write the current tracked source-kit allowlist."""

    document = build_allowlist(source_root, git_executable=git_executable)
    write_allowlist(source_root, output_path, document)
    return document


def check_allowlist(
    source_root: pathlib.Path | str,
    allowlist_path: pathlib.Path | str = DEFAULT_ALLOWLIST,
    *,
    git_executable: pathlib.Path | str = "git",
) -> dict[str, Any]:
    """Return a read-only completeness report for an existing allowlist."""

    return manifest.check_allowlist_completeness(
        source_root,
        allowlist_path,
        git_executable=git_executable,
    )


def _print_check_report(report: Mapping[str, Any]) -> None:
    status = "PASS" if report["complete"] else "FAIL"
    print(
        f"source-kit allowlist check: {status}; "
        f"{report['allowlisted_count']} allowlisted, "
        f"{report['required_count']} required tracked, "
        f"{report['tracked_count']} tracked total"
    )
    for field, label in (
        ("missing", "missing required tracked paths"),
        ("case_mismatches", "actual filename casing mismatches"),
        ("classification_mismatches", "classification mismatches"),
        ("excluded", "tracked paths excluded by policy"),
        ("extras", "allowlist entries outside tracked source scope"),
    ):
        values = report.get(field, [])
        if values:
            print(f"{label}:")
            for value in values:
                print(f"  {value}")


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", "--source-root", dest="source_root", required=True,
                        type=pathlib.Path, help="repository/source root")
    parser.add_argument(
        "--output", "--allowlist", dest="output", type=pathlib.Path,
        default=DEFAULT_ALLOWLIST,
        help="allowlist path (default: packaging/source-kit-allowlist.json)",
    )
    parser.add_argument("--git-executable", default="git",
                        help="Git executable used for read-only ls-files discovery")
    parser.add_argument("--check", action="store_true",
                        help="verify the existing allowlist without writing it")
    args = parser.parse_args(argv)
    try:
        if args.check:
            report = check_allowlist(
                args.source_root,
                args.output,
                git_executable=args.git_executable,
            )
            _print_check_report(report)
            return 0 if report["complete"] else 1
        document = refresh_allowlist(
            args.source_root,
            args.output,
            git_executable=args.git_executable,
        )
    except ManifestError as exc:
        print(f"source-kit allowlist: {exc}", file=sys.stderr)
        return 1
    print(f"Wrote {args.output} ({len(document['entries'])} explicit tracked files)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
