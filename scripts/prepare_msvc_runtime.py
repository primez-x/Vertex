#!/usr/bin/env python3
"""Stage explicitly selected MSVC SDK redistributables for app-local packaging.

This records operator-declared versions and local provenance; it does not grant
redistribution rights or prove compatibility on a clean Windows installation.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import stat
import struct
import tempfile


RUNTIME_NAMES = (
    "msvcp140.dll",
    "msvcp140_1.dll",
    "msvcp140_2.dll",
    "vcruntime140.dll",
    "vcruntime140_1.dll",
)
_FORBIDDEN_PARTS = {"system32", "syswow64", "systemwow64", "debug_nonredist"}


def checked_path(path: Path) -> Path:
    """Reject symlinks/junctions before resolving any source or destination."""
    path = Path(os.path.abspath(path))
    for candidate in (path, *path.parents):
        try:
            info = candidate.lstat()
        except FileNotFoundError:
            continue
        if stat.S_ISLNK(info.st_mode) or getattr(info, "st_file_attributes", 0) & 0x400:
            raise ValueError(f"Reparse or symbolic-link path is not allowed: {candidate}")
    return path


def source_path(path: Path) -> Path:
    path = checked_path(path)
    if any(part.casefold() in _FORBIDDEN_PARTS for part in path.parts):
        raise ValueError("System and debug runtime sources are not allowed")
    return path


def validate_x64_dll(data: bytes, name: str) -> None:
    """Check PE/COFF architecture and DLL identity, not executable correctness."""
    if len(data) < 64 or data[:2] != b"MZ":
        raise ValueError(f"Not a PE DLL: {name}")
    pe = struct.unpack_from("<I", data, 60)[0]
    if pe < 64 or pe + 24 > len(data) or data[pe:pe + 4] != b"PE\0\0":
        raise ValueError(f"Invalid PE header: {name}")
    machine, sections = struct.unpack_from("<HH", data, pe + 4)
    optional_size, characteristics = struct.unpack_from("<HH", data, pe + 20)
    if machine != 0x8664 or not characteristics & 0x2000:
        raise ValueError(f"Expected an x64 DLL: {name}")
    optional = pe + 24
    if (sections == 0 or optional_size < 112 or optional + optional_size + sections * 40 > len(data)
            or struct.unpack_from("<H", data, optional)[0] != 0x20B):
        raise ValueError(f"Invalid PE32+ optional or section header: {name}")


def _file_record(path: str, data: bytes, version: str | None = None) -> dict:
    result = {"path": path, "bytes": len(data), "sha256": hashlib.sha256(data).hexdigest()}
    if version is not None:
        result.update(version=version, version_verification="operator-declared")
    return result


def _existing_matches(output: Path, expected: dict[str, bytes]) -> bool:
    if not output.is_dir():
        return False
    actual_files: set[str] = set()
    actual_dirs: set[str] = set()
    for item in output.rglob("*"):
        checked_path(item)
        relative = item.relative_to(output).as_posix()
        if item.is_dir():
            actual_dirs.add(relative)
        elif item.is_file():
            actual_files.add(relative)
            if relative not in expected or item.read_bytes() != expected[relative]:
                return False
        else:
            return False
    return actual_files == set(expected) and actual_dirs == {"bin", "notices"}


def prepare(crt_dir: Path, notice_files: list[Path], output: Path, version: str,
            *, workspace_root: Path) -> dict:
    if not re.fullmatch(r"[0-9]+(?:\.[0-9]+){3}", version):
        raise ValueError("Version must contain four numeric components")
    workspace = checked_path(workspace_root)
    output = checked_path(output)
    expected_output = workspace / ".deps" / "msvc-runtime" / version
    if output != expected_output:
        raise ValueError("Output must be .deps/msvc-runtime/<version> in this workspace")
    crt_dir = source_path(crt_dir)
    if (crt_dir.name.casefold() != "microsoft.vc143.crt" or crt_dir.parent.name.casefold() != "x64"
            or "redist" not in {part.casefold() for part in crt_dir.parts} or not crt_dir.is_dir()):
        raise ValueError("Select the SDK Redist x64/Microsoft.VC143.CRT directory explicitly")
    if not notice_files:
        raise ValueError("At least one explicit local notice file is required")

    payload: dict[str, bytes] = {}
    binaries = []
    for name in RUNTIME_NAMES:
        source = source_path(crt_dir / name)
        if not source.is_file():
            raise ValueError(f"Missing redistributable: {name}")
        data = source.read_bytes()
        validate_x64_dll(data, name)
        relative = f"bin/{name}"
        payload[relative] = data
        binaries.append(_file_record(relative, data, version))
    notices = []
    names: set[str] = set()
    for notice_file in notice_files:
        notice = source_path(notice_file)
        if not notice.is_file() or notice.name.casefold() in names:
            raise ValueError("Notice files must exist and have distinct basenames")
        names.add(notice.name.casefold())
        data = notice.read_bytes()
        if not data:
            raise ValueError("Notice files must not be empty")
        relative = f"notices/{notice.name}"
        payload[relative] = data
        notices.append(_file_record(relative, data))
    manifest = {
        "schema_version": 1,
        "component": "msvc-app-local-runtime",
        "version": version,
        "architecture": "x64",
        "licensing_clearance": False,
        "provenance": {
            "kind": "operator-selected-msvc-sdk-redistributable",
            "source_directory_role": "Redist/x64/Microsoft.VC143.CRT",
            "version_verification": "operator-declared",
            "notice_status": "local-evidence-only-not-redistribution-clearance",
        },
        "files": binaries,
        "notices": sorted(notices, key=lambda row: row["path"]),
    }
    payload["manifest.json"] = (json.dumps(manifest, indent=2, sort_keys=True) + "\n").encode("utf-8")
    # Re-read and validate every source above even for an idempotent rerun.
    if output.exists():
        if not _existing_matches(output, payload):
            raise ValueError("Existing output differs; refusing to replace it")
        return manifest

    output.parent.mkdir(parents=True, exist_ok=True)
    checked_path(output.parent)
    staging = Path(tempfile.mkdtemp(prefix=f".{version}-", dir=output.parent))
    try:
        for relative, data in payload.items():
            target = staging / relative
            target.parent.mkdir(parents=True, exist_ok=True)
            target.write_bytes(data)
        if output.exists():
            raise ValueError("Output appeared during staging; refusing to replace it")
        staging.rename(output)
    finally:
        if staging.exists():
            shutil.rmtree(staging)
    return manifest


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--crt-dir", required=True, type=Path)
    parser.add_argument("--notice-file", required=True, action="append", type=Path)
    parser.add_argument("--version", required=True)
    parser.add_argument("--output", required=True, type=Path)
    args = parser.parse_args()
    try:
        manifest = prepare(args.crt_dir, args.notice_file, args.output, args.version,
                           workspace_root=Path(__file__).resolve().parents[1])
    except (OSError, ValueError) as error:
        parser.exit(1, f"MSVC runtime staging failed: {error}\n")
    print(json.dumps({"component": manifest["component"], "version": manifest["version"],
                      "runtime_files": len(manifest["files"]), "licensing_clearance": False}))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
