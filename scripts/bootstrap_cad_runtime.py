"""Build a hash-pinned Windows CAD interpreter without pip or runtime downloads.

Normal application builds do not call this script. --offline uses only the
locked local archives; --check verifies an existing generated runtime.
"""
from __future__ import annotations

import argparse
import hashlib
import io
import json
import os
import pathlib
import re
import shutil
import stat
import tarfile
import tempfile
import urllib.parse
import urllib.request
import zipfile

ROOT = pathlib.Path(__file__).resolve().parents[1]
MAX_EXPANSION = 1_000_000_000
MAX_COMPRESSED = 256_000_000
RESERVED = {"con", "prn", "aux", "nul", *(f"com{i}" for i in range(1, 10)),
            *(f"lpt{i}" for i in range(1, 10))}


def digest(path: pathlib.Path) -> str:
    with path.open("rb") as source:
        return hashlib.file_digest(source, "sha256").hexdigest()


def member_path(name: str) -> pathlib.PurePosixPath:
    if "\\" in name or any(ord(c) < 32 for c in name) or any(p in {"", "."} for p in name.split("/")):
        raise ValueError("Backslash archive path")
    path = pathlib.PurePosixPath(name)
    if path.is_absolute() or not path.parts or any(
        part in {"..", "."} or part.endswith((" ", ".")) or
        any(c in part for c in ':*?"<>|') or
        part.split(".", 1)[0].casefold() in RESERVED
        for part in path.parts
    ):
        raise ValueError(f"Unsafe Windows archive path: {name}")
    return path


def validate_lock(lock: dict) -> None:
    if lock.get("schema_version") != 1 or lock.get("platform") != "win_amd64":
        raise ValueError("Unsupported CAD runtime lock")
    if not re.fullmatch(r"3\.13\.\d+", lock.get("python_version", "")) or lock.get("python_abi") != "cp313":
        raise ValueError("Unsupported CPython ABI")
    names = set()
    for asset in lock.get("assets", []):
        path = member_path(asset["filename"])
        if len(path.parts) != 1 or path.name.casefold() in names:
            raise ValueError("Duplicate or nested locked archive")
        names.add(path.name.casefold())
        if asset["kind"] not in {"interpreter", "headers", "development", "wheel", "notice"}:
            raise ValueError("Unknown archive kind")
        if not re.fullmatch(r"[0-9a-f]{64}", asset["sha256"]):
            raise ValueError("Invalid archive SHA-256")
        url = urllib.parse.urlsplit(asset["url"])
        if url.scheme != "https" or url.hostname not in {"www.python.org", "files.pythonhosted.org", "www.gnu.org", "api.nuget.org"}:
            raise ValueError("Archive is not on an approved HTTPS publisher")
    if sum(a["kind"] == "interpreter" for a in lock["assets"]) != 1:
        raise ValueError("Exactly one interpreter is required")
    if sum(a["kind"] == "headers" for a in lock["assets"]) != 1:
        raise ValueError("Exactly one matching CPython header archive is required")
    if sum(a["kind"] == "development" for a in lock["assets"]) != 1 or any(
        a["version"] != lock["python_version"] for a in lock["assets"]
        if a["kind"] in {"interpreter", "headers", "development"}
    ):
        raise ValueError("Interpreter and development headers must match exactly")
    wheels = {a["name"]: a["version"] for a in lock["assets"] if a["kind"] == "wheel"}
    if wheels != lock.get("library_versions") or len(wheels) != sum(a["kind"] == "wheel" for a in lock["assets"]):
        raise ValueError("Wheel versions do not match the complete lock")


def obtain(asset: dict, downloads: pathlib.Path, offline: bool) -> pathlib.Path:
    path = downloads / asset["filename"]
    if is_link(path):
        raise ValueError("Archive cache cannot contain links")
    if not path.exists():
        if offline:
            raise ValueError(f"Offline CAD archive missing: {path.name}")
        partial = None
        try:
            with tempfile.NamedTemporaryFile(dir=downloads, prefix=path.name+".", suffix=".partial", delete=False) as output:
                partial = pathlib.Path(output.name)
                count = 0
                with urllib.request.urlopen(asset["url"], timeout=60) as source:
                    while chunk := source.read(65536):
                        count += len(chunk)
                        if count > MAX_COMPRESSED:
                            raise ValueError("CAD download exceeds compressed size limit")
                        output.write(chunk)
            if digest(partial) != asset["sha256"]:
                raise ValueError(f"CAD archive checksum mismatch: {path.name}")
            if path.exists():
                raise ValueError("Archive cache changed during download; retry verification")
            partial.rename(path)
        finally:
            if partial is not None and partial.exists():
                partial.unlink()
    if not path.is_file() or digest(path) != asset["sha256"]:
        raise ValueError(f"CAD archive checksum mismatch: {path.name}")
    return path


def verified_archive_bytes(asset: dict, path: pathlib.Path) -> bytes:
    with path.open("rb") as source:
        data = source.read(MAX_COMPRESSED + 1)
    if len(data) > MAX_COMPRESSED or hashlib.sha256(data).hexdigest() != asset["sha256"]:
        raise ValueError("Extraction bytes differ from pinned archive")
    return data


def unpack_zip(path: pathlib.Path | bytes, destination: pathlib.Path, occupied: set[str]) -> None:
    with zipfile.ZipFile(io.BytesIO(path) if isinstance(path, bytes) else path) as archive:
        entries = []
        total = 0
        local = set()
        for item in archive.infolist():
            relative = member_path(item.filename.rstrip("/"))
            if stat.S_ISLNK(item.external_attr >> 16):
                raise ValueError("Archive symlink")
            key = relative.as_posix().casefold()
            if key in local or (not item.is_dir() and key in occupied):
                raise ValueError(f"Archive collision: {relative}")
            local.add(key)
            total += item.file_size
            if total > MAX_EXPANSION:
                raise ValueError("CAD archive expansion limit")
            # Library-only runtime: retain console entry points as inert wheel
            # data, never put them on PATH or synthesize launchers. Other wheel
            # installation schemes require a separately reviewed mapping.
            if relative.parts[0].endswith(".data"):
                script = len(relative.parts) >= 3 and relative.parts[1] == "scripts"
                manual = len(relative.parts) >= 6 and relative.parts[1:4] == ("data", "share", "man")
                if not (script or manual):
                    raise ValueError("Wheel install scheme requires explicit mapping")
                relative = pathlib.PurePosixPath("wheel-data") / relative
                key = relative.as_posix().casefold()
                if key in occupied:
                    raise ValueError("Mapped wheel script collision")
            entries.append((item, destination.joinpath(*relative.parts), key))
        for item, target, key in entries:
            if item.is_dir():
                target.mkdir(parents=True, exist_ok=True)
            else:
                target.parent.mkdir(parents=True, exist_ok=True)
                with archive.open(item) as source, target.open("xb") as output:
                    shutil.copyfileobj(source, output)
                occupied.add(key)


def unpack_headers(path: pathlib.Path | bytes, destination: pathlib.Path, version: str) -> None:
    prefix = f"Python-{version}"
    with (tarfile.open(fileobj=io.BytesIO(path), mode="r:xz") if isinstance(path, bytes)
          else tarfile.open(path, "r:xz")) as archive:
        entries = []
        names = set()
        total = 0
        for item in archive:
            # Extract only C headers from the matching source, never scripts.
            if not (item.name.startswith(prefix + "/Include/") or item.name == prefix + "/PC/pyconfig.h"):
                continue
            relative = member_path(item.name)
            if item.isdir():
                continue
            if not item.isfile():
                raise ValueError("Header archive links are not allowed")
            parts = relative.parts[2:] if relative.parts[1] == "Include" else ("pyconfig.h",)
            key = "/".join(parts).casefold()
            if key in names:
                raise ValueError("Header archive collision")
            names.add(key)
            total += item.size
            if total > 20_000_000:
                raise ValueError("Header expansion limit")
            entries.append((item, destination.joinpath(*parts)))
        for item, target in entries:
            target.parent.mkdir(parents=True, exist_ok=True)
            with archive.extractfile(item) as source, target.open("xb") as output:
                shutil.copyfileobj(source, output)
    if not (destination / "Python.h").is_file():
        raise ValueError("Matching CPython embedding headers missing")


def unpack_development(path: pathlib.Path | bytes, destination: pathlib.Path) -> None:
    wanted = {"tools/include/pyconfig.h": "include/pyconfig.h",
              "tools/libs/python313.lib": "libs/python313.lib"}
    with zipfile.ZipFile(io.BytesIO(path) if isinstance(path, bytes) else path) as archive:
        for source, target in wanted.items():
            item = archive.getinfo(source)
            if item.is_dir() or stat.S_ISLNK(item.external_attr >> 16) or item.file_size > 10_000_000:
                raise ValueError("Invalid CPython development member")
            output = destination / target
            output.parent.mkdir(parents=True, exist_ok=True)
            with archive.open(item) as stream, output.open("xb") as sink:
                shutil.copyfileobj(stream, sink)


def is_link(path: pathlib.Path) -> bool:
    try:
        info = path.lstat()
    except FileNotFoundError:
        return False
    return stat.S_ISLNK(info.st_mode) or bool(getattr(info,"st_file_attributes",0) & 0x400)


def inventory(destination: pathlib.Path, lock: dict, lock_digest: str) -> dict:
    records = []
    for directory, directories, files in os.walk(destination, followlinks=False):
        for name in (*directories, *files):
            if is_link(pathlib.Path(directory) / name):
                raise ValueError("Runtime contains a link")
        for name in files:
            path = pathlib.Path(directory) / name
            if path == destination / "runtime-manifest.json":
                continue
            relative = path.relative_to(destination).as_posix()
            records.append({"path": relative, "sha256": digest(path), "bytes": path.stat().st_size})
    records.sort(key=lambda record: record["path"])
    return {"schema_version": 1, "lock_sha256": lock_digest, "python_version": lock["python_version"],
            "library_versions": lock["library_versions"], "qualification": "incomplete",
            "production_worker_integrated": False, "files": records}


def check_runtime(destination: pathlib.Path, lock: dict, lock_digest: str) -> dict:
    manifest = json.loads((destination / "runtime-manifest.json").read_text(encoding="utf-8"))
    manifest["files"].sort(key=lambda record: record["path"])
    actual = inventory(destination, lock, lock_digest)
    if manifest != actual:
        raise ValueError("CAD runtime differs from its pinned file manifest")
    return actual


def bootstrap(root: pathlib.Path, lock_path: pathlib.Path, destination: pathlib.Path,
              *, offline: bool = False, check: bool = False) -> dict:
    root = root.resolve()
    base = root / ".deps/cad-runtime"
    # The only writable destination is a new child of this task's dependency root.
    if not destination.is_absolute():
        destination = root / destination
    for parent in (destination, *destination.parents):
        if is_link(parent):
            raise ValueError("CAD destination must not cross a link")
    destination = destination.resolve()
    if not destination.is_relative_to(base.resolve()) or destination == base.resolve():
        raise ValueError("CAD destination must be below .deps/cad-runtime")
    lock = json.loads(lock_path.read_text(encoding="utf-8"))
    validate_lock(lock)
    lock_digest = digest(lock_path)
    if check and not destination.exists():
        raise ValueError("CAD runtime does not exist")
    downloads = root / ".deps/downloads/cad-runtime"
    if any(is_link(parent) for parent in (downloads, *downloads.parents)):
        raise ValueError("CAD archive cache must not cross a link")
    downloads.mkdir(parents=True, exist_ok=True)
    archives = [(a, obtain(a, downloads, offline)) for a in lock["assets"]]
    destination.parent.mkdir(parents=True, exist_ok=True)
    stage = pathlib.Path(tempfile.mkdtemp(prefix=".vertex-cad-stage-", dir=destination.parent))
    try:
        occupied: dict[str, set[str]] = {"interpreter": set(), "wheel": set()}
        for asset, path in archives:
            data = verified_archive_bytes(asset, path)
            if asset["kind"] == "interpreter":
                unpack_zip(data, stage, occupied["interpreter"])
            elif asset["kind"] == "wheel":
                unpack_zip(data, stage / "Lib/site-packages", occupied["wheel"])
            elif asset["kind"] == "headers":
                unpack_headers(data, stage / "include", lock["python_version"])
            elif asset["kind"] == "development":
                unpack_development(data, stage)
            else:
                target = stage / "notices" / asset["filename"]
                target.parent.mkdir(parents=True, exist_ok=True)
                target.write_bytes(data)
        # _pth makes paths relative to this interpreter and disables user/site
        # discovery. No import-site line, pip, registry or PYTHONPATH dependency.
        (stage / "python313._pth").write_text("python313.zip\n.\nLib/site-packages\n", encoding="utf-8")
        result = inventory(stage, lock, lock_digest)
        (stage / "runtime-manifest.json").write_text(json.dumps(result, indent=2) + "\n", encoding="utf-8")
        if destination.exists():
            actual = check_runtime(destination, lock, lock_digest)
            if actual != result:
                raise ValueError("Runtime manifest does not match verified archive contents")
            return result
        stage.rename(destination)
        return result
    finally:
        if stage.exists():
            if is_link(stage) or stage.resolve().parent != destination.parent.resolve():
                raise ValueError("Unsafe staging cleanup target")
            shutil.rmtree(stage)


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--offline", action="store_true")
    parser.add_argument("--check", action="store_true")
    parser.add_argument("--destination", type=pathlib.Path, default=pathlib.Path(".deps/cad-runtime/3.13.15"))
    args = parser.parse_args()
    report = bootstrap(ROOT, ROOT / "third_party/cad-runtime-lock.json", args.destination,
                       offline=args.offline, check=args.check)
    print(json.dumps({"python": report["python_version"], "files": len(report["files"]),
                      "qualification": report["qualification"], "production_worker_integrated": False}))


if __name__ == "__main__":
    main()
