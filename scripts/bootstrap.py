"""Explicit, pinned dependency bootstrap. Normal CMake builds never download."""

import argparse
import hashlib
import json
import pathlib
import shutil
import stat
import urllib.request
import zipfile

ROOT = pathlib.Path(__file__).resolve().parents[1]


def verify(path, sha256):
    with path.open("rb") as source:
        actual = hashlib.file_digest(source, "sha256").hexdigest()
    if actual.lower() != sha256.lower():
        raise ValueError(f"Hash mismatch for {path.name}: expected {sha256}, received {actual}")


def extract_zip(archive, destination):
    # Validate the entire table before writing anything. Archive names are not
    # trusted OS paths, even when a package comes from an official source.
    entries = []
    total = 0
    names = set()
    for entry in archive.infolist():
        name = entry.filename.replace("\\", "/")
        path = pathlib.PurePosixPath(name)
        mode = entry.external_attr >> 16
        if (path.is_absolute() or ".." in path.parts or ":" in name
                or stat.S_ISLNK(mode) or any(part.endswith((" ", ".")) for part in path.parts)
                or not path.parts):
            raise ValueError(f"Unsafe archive member: {name}")
        key = name.rstrip("/").casefold()
        if key in names:
            raise ValueError(f"Duplicate archive member: {name}")
        names.add(key)
        total += entry.file_size
        if total > 1_000_000_000:
            raise ValueError("Source archive exceeds one gigabyte expansion limit")
        entries.append((entry, destination.joinpath(*path.parts)))
    destination.mkdir(parents=True, exist_ok=True)
    for entry, target in entries:
        if entry.is_dir():
            target.mkdir(parents=True, exist_ok=True)
        else:
            target.parent.mkdir(parents=True, exist_ok=True)
            with archive.open(entry) as source, target.open("wb") as output:
                shutil.copyfileobj(source, output)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--offline", action="store_true", help="Require all pinned archives already in .deps/downloads")
    args = parser.parse_args()
    manifest = json.loads((ROOT / "third_party/dependencies.json").read_text(encoding="utf-8"))
    for asset in manifest.get("bundled_assets", []):
        verify(ROOT / asset["path"], asset["sha256"])
    downloads = ROOT / ".deps/downloads"
    downloads.mkdir(parents=True, exist_ok=True)
    for dependency in manifest["bootstrap_dependencies"]:
        artifact = downloads / dependency["filename"]
        if not artifact.exists():
            if args.offline:
                raise SystemExit(f"Offline dependency missing: {artifact.name}")
            partial = artifact.with_suffix(artifact.suffix + ".partial")
            with urllib.request.urlopen(dependency["url"], timeout=60) as response, partial.open("wb") as output:
                shutil.copyfileobj(response, output)
            verify(partial, dependency["sha256"])
            partial.replace(artifact)
        verify(artifact, dependency["sha256"])
        target = ROOT / ".deps/src" / dependency["destination"]
        if dependency["kind"] == "zip":
            with zipfile.ZipFile(artifact) as archive:
                extract_zip(archive, target)
        else:
            target.parent.mkdir(parents=True, exist_ok=True)
            shutil.copyfile(artifact, target)
        print(f"Verified {dependency['name']} {dependency['version']}")


if __name__ == "__main__":
    main()
