"""Prepare locked IFC source and SWIG inputs; never build or qualify a binary.

Only this explicit developer command acquires missing inputs. --offline forbids
acquisition; --check verifies an already prepared manifest using cached bytes.
Existing mismatched repositories, cache files and tool trees are never reset.
"""
from __future__ import annotations

import argparse
import configparser
import hashlib
import io
import json
import os
import pathlib
import re
import shutil
import stat
import subprocess
import tempfile
import urllib.request
import zipfile

ROOT = pathlib.Path(__file__).resolve().parents[1]
MAX_COMPRESSED = 128_000_000
MAX_EXPANSION = 512_000_000
MAX_MEMBERS = 30_000
RESERVED = {"con", "prn", "aux", "nul", *(f"com{i}" for i in range(1, 10)),
            *(f"lpt{i}" for i in range(1, 10))}
SOURCE_PATH = ".deps/ifc-src"
SUBMODULE_PATHS = {"src/svgfill", "src/ifcopenshell-python/ifcopenshell/mvd",
                   "src/ifcopenshell-python/ifcopenshell/simple_spf"}


def is_link(path: pathlib.Path) -> bool:
    try:
        info = path.lstat()
    except FileNotFoundError:
        return False
    return stat.S_ISLNK(info.st_mode) or bool(getattr(info, "st_file_attributes", 0) & 0x400)


def no_links(path: pathlib.Path, *, tree: bool = False) -> None:
    if any(is_link(p) for p in (path, *path.parents)):
        raise ValueError(f"Input path crosses a link or reparse point: {path}")
    if tree and path.is_dir():
        for directory, dirs, files in os.walk(path, followlinks=False):
            for name in (*dirs, *files):
                if is_link(pathlib.Path(directory) / name):
                    raise ValueError(f"Input tree contains a link or reparse point: {name}")


def member_path(name: str) -> pathlib.PurePosixPath:
    if not name or "\\" in name or any(ord(c) < 32 for c in name):
        raise ValueError(f"Unsafe Windows archive path: {name}")
    parts = name.split("/")
    if any(p in {"", ".", ".."} or p.endswith((" ", ".")) or
           any(c in p for c in ':*?"<>|') or p.split(".", 1)[0].casefold() in RESERVED
           for p in parts):
        raise ValueError(f"Unsafe Windows archive path: {name}")
    return pathlib.PurePosixPath(name)


def validate_lock(lock: dict) -> None:
    if lock.get("schema_version") != 1 or lock.get("platform") != "win_amd64":
        raise ValueError("Unsupported IFC source lock")
    if lock.get("build_qualified") is not False or lock.get("source_closure_qualified") is not False:
        raise ValueError("Preparation cannot qualify a build or source closure")
    source = lock["source"]
    if source["path"] != SOURCE_PATH or source["repository"] != "https://github.com/IfcOpenShell/IfcOpenShell.git":
        raise ValueError("Source must use the fixed official repository and dependency path")
    expected_repos = {
        "src/svgfill": "https://github.com/IfcOpenShell/svgfill",
        "src/ifcopenshell-python/ifcopenshell/mvd": "https://github.com/opensourceBIM/python-mvdxml/",
        "src/ifcopenshell-python/ifcopenshell/simple_spf": "https://github.com/IfcOpenShell/step-file-parser"}
    if len(lock["submodules"]) != 3 or {s["path"] for s in lock["submodules"]} != SUBMODULE_PATHS:
        raise ValueError("Incomplete or duplicate prepared submodule set")
    for item in [source, *lock["submodules"]]:
        if not re.fullmatch("[0-9a-f]{40}", item["revision"]):
            raise ValueError("Expected a full Git revision")
    if any(s["repository"] != expected_repos[s["path"]] for s in lock["submodules"]):
        raise ValueError("Submodule must use its official repository")
    swig = lock["swig"]
    if (swig["version"] != "4.3.1" or swig["filename"] != "swigwin-4.3.1.zip" or
        swig["url"] != "https://downloads.sourceforge.net/project/swig/swigwin/swigwin-4.3.1/swigwin-4.3.1.zip" or
        not re.fullmatch("[0-9a-f]{128}", swig["sha512"])):
        raise ValueError("Unsupported pinned SWIG archive")


def run_git(path: pathlib.Path, *args: str) -> str:
    # Read-only commands must not run repository fsmonitor hooks. Acquisition
    # uses the same isolated configuration; no recursive submodule/update scripts.
    env = {k: v for k, v in os.environ.items() if not k.upper().startswith("GIT_")}
    env.update(GIT_CONFIG_NOSYSTEM="1", GIT_CONFIG_GLOBAL=os.devnull,
               GIT_TERMINAL_PROMPT="0", GIT_OPTIONAL_LOCKS="0", GIT_NO_LAZY_FETCH="1",
               GIT_NO_REPLACE_OBJECTS="1")
    command = ["git", "--no-replace-objects", "-c", "core.fsmonitor=false", "-c", "core.untrackedCache=false",
               "-c", "core.hooksPath=" + os.devnull,
               "-c", "core.autocrlf=false", "-c", "submodule.recurse=false",
               "-c", "protocol.file.allow=never", "-C", str(path), *args]
    result = subprocess.run(command, env=env, capture_output=True, text=True,
                            encoding="utf-8", errors="strict", timeout=600,
                            creationflags=getattr(subprocess, "CREATE_NO_WINDOW", 0))
    if result.returncode:
        raise ValueError(f"Git {args[0]} failed in {path}: {result.stderr.strip()}")
    return result.stdout.strip()


def preflight_git_metadata(path: pathlib.Path, source: pathlib.Path) -> pathlib.Path:
    """Resolve only standalone/submodule metadata before Git reads its config."""
    no_links(path, tree=True)
    entry = path / ".git"
    if not path.is_dir() or not entry.exists():
        raise ValueError(f"Missing or foreign source repository: {path}")
    if entry.is_dir():
        gitdir = entry
    elif entry.is_file() and entry.stat().st_size <= 4096:
        match = re.fullmatch(r"gitdir: ([^\r\n]+)\r?\n?", entry.read_text(encoding="utf-8"))
        if not match:
            raise ValueError("Invalid source submodule gitfile")
        gitdir = pathlib.Path(match[1])
        if not gitdir.is_absolute():
            gitdir = path / gitdir
        if path == source or not gitdir.resolve().is_relative_to((source / ".git/modules").resolve()):
            raise ValueError("Repository metadata is outside the controlled source path")
    else:
        raise ValueError("Invalid source repository metadata")
    no_links(gitdir, tree=True)
    if not gitdir.is_dir() or (gitdir / "commondir").exists():
        raise ValueError("Linked worktrees and redirected common Git metadata are unsupported")
    # Status can apply clean filters while comparing worktree bytes. Refuse
    # both normal and extension-enabled worktree config before any Git query.
    # Git permits repeated sections, so scan headers rather than parse as INI.
    for config_path in (gitdir / "config", gitdir / "config.worktree"):
        if config_path.exists():
            if not config_path.is_file():
                raise ValueError("Source Git config is not a regular file")
            text = config_path.read_text(encoding="utf-8-sig")
            if re.search(r'^\s*\[\s*(?:filter|include|includeIf)\b', text, re.MULTILINE | re.IGNORECASE):
                raise ValueError("Source Git config has executable filters or external includes")
    return gitdir.resolve()


def verify_repository(path: pathlib.Path, item: dict, source: pathlib.Path) -> dict:
    gitdir = preflight_git_metadata(path, source)
    if pathlib.Path(run_git(path, "rev-parse", "--show-toplevel")).resolve() != path.resolve():
        raise ValueError("Source path is not the repository root")
    if pathlib.Path(run_git(path, "rev-parse", "--absolute-git-dir")).resolve() != gitdir:
        raise ValueError("Git resolved unexpected source repository metadata")
    origin = run_git(path, "config", "--local", "--no-includes", "--get-all", "remote.origin.url")
    if origin != item["repository"]:
        raise ValueError("Source origin differs from the lock")
    revision = run_git(path, "rev-parse", "HEAD")
    if revision != item["revision"]:
        raise ValueError("Source HEAD differs from the lock; no reset will be performed")
    for record in run_git(path, "ls-files", "-v", "-z").split("\0"):
        if record and (len(record) < 3 or record[1] != " " or record[0].islower() or record[0] == "S"):
            raise ValueError("Source index has assume-unchanged or skip-worktree flags")
    selected = SUBMODULE_PATHS if item["path"] == SOURCE_PATH else set()
    for record in run_git(path, "ls-tree", "-r", "-z", "HEAD").split("\0"):
        if not record:
            continue
        metadata, filename = record.split("\t", 1)
        if metadata.startswith("160000 ") and filename not in selected:
            relative = member_path(filename)
            child = path.joinpath(*relative.parts)
            no_links(child, tree=True)
            if child.exists() and (not child.is_dir() or any(child.iterdir())):
                raise ValueError("Unselected gitlink contains foreign source content")
    # Never recurse into a child before its own metadata/config preflight.
    # Selected children receive explicit identity, index and status checks.
    if run_git(path, "status", "--porcelain=v1", "--untracked-files=all", "--ignored=matching", "--ignore-submodules=all"):
        raise ValueError("Source contains dirty, ignored or foreign content")
    return {"path": item["path"], "repository": origin, "revision": revision}


def remove_stage(stage: pathlib.Path, parent: pathlib.Path) -> None:
    # Only unique directories created by this invocation may be removed.
    no_links(stage, tree=True)
    if stage.resolve().parent != parent.resolve() or not stage.name.startswith(".vertex-ifc-stage-"):
        raise ValueError("Unsafe staging cleanup target")
    shutil.rmtree(stage)


def acquire_repository(path: pathlib.Path, item: dict, source: pathlib.Path, *, offline: bool) -> None:
    no_links(path, tree=True)
    if (path / ".git").exists():
        verify_repository(path, item, source)
        return
    if path.exists() and (not path.is_dir() or any(path.iterdir())):
        raise ValueError(f"Preexisting foreign source content: {path}")
    if offline:
        raise ValueError(f"Offline source repository missing: {path}")
    path.parent.mkdir(parents=True, exist_ok=True)
    stage = pathlib.Path(tempfile.mkdtemp(prefix=".vertex-ifc-stage-", dir=path.parent))
    try:
        checkout = stage / "checkout"
        run_git(stage, "clone", "--no-checkout", "--no-recurse-submodules", "--", item["repository"], str(checkout))
        # The newly created repository alone is mutated. Never fetch/reset an
        # existing checkout merely because it has the wrong HEAD.
        run_git(checkout, "checkout", "--detach", item["revision"])
        verify_repository(checkout, item, checkout)
        no_links(path, tree=True)
        if path.exists():
            if not path.is_dir() or any(path.iterdir()):
                raise ValueError("Source destination changed during preparation")
            path.rmdir()  # Only an empty, rechecked gitlink placeholder.
        checkout.rename(path)
    finally:
        if stage.exists():
            remove_stage(stage, path.parent)


def verify_gitlinks(source: pathlib.Path, submodules: list[dict]) -> None:
    config = configparser.ConfigParser(interpolation=None, strict=True)
    config.read_string((source / ".gitmodules").read_text(encoding="utf-8"))
    for item in submodules:
        section = f'submodule "{item["path"]}"'
        if (not config.has_section(section) or config[section].get("path") != item["path"] or
            config[section].get("url") != item["repository"]):
            raise ValueError("Pinned submodule does not match source .gitmodules")
        expected = f'160000 commit {item["revision"]}\t{item["path"]}'
        if run_git(source, "ls-tree", "HEAD", "--", item["path"]) != expected:
            raise ValueError("Source gitlink differs from the submodule lock")


def verified_swig_bytes(path: pathlib.Path, asset: dict) -> bytes:
    no_links(path)
    if not path.is_file():
        raise ValueError("SWIG cached archive missing")
    with path.open("rb") as source:
        data = source.read(MAX_COMPRESSED + 1)
    if len(data) > MAX_COMPRESSED or hashlib.sha512(data).hexdigest() != asset["sha512"]:
        raise ValueError("SWIG archive checksum or compressed size mismatch")
    return data


def obtain_swig(path: pathlib.Path, asset: dict, *, offline: bool) -> bytes:
    no_links(path)
    if path.exists():
        return verified_swig_bytes(path, asset)
    if offline:
        raise ValueError("Offline SWIG cached archive missing")
    path.parent.mkdir(parents=True, exist_ok=True)
    partial = None
    try:
        with tempfile.NamedTemporaryFile(dir=path.parent, prefix=".vertex-swig-", suffix=".partial", delete=False) as output:
            partial = pathlib.Path(output.name)
            size = 0
            with urllib.request.urlopen(asset["url"], timeout=60) as response:
                while chunk := response.read(65536):
                    size += len(chunk)
                    if size > MAX_COMPRESSED:
                        raise ValueError("SWIG download compressed size limit")
                    output.write(chunk)
        data = verified_swig_bytes(partial, asset)
        no_links(path)
        # Hardlink publication is exclusive even on POSIX: rename may overwrite.
        os.link(partial, path)
        return data
    finally:
        if partial is not None and partial.exists():
            no_links(partial)
            partial.unlink()


def extract_swig(data: bytes, destination: pathlib.Path) -> None:
    no_links(destination)
    if destination.exists():
        raise ValueError("SWIG extraction requires a new destination")
    with zipfile.ZipFile(io.BytesIO(data)) as archive:
        entries = []
        names = {}
        explicit = set()
        total = 0
        if len(archive.infolist()) > MAX_MEMBERS:
            raise ValueError("SWIG archive member limit")
        for item in archive.infolist():
            relative = member_path(item.filename[:-1] if item.is_dir() else item.filename)
            mode = item.external_attr >> 16
            if (relative.parts[0] != "swigwin-4.3.1" or stat.S_ISLNK(mode) or
                stat.S_IFMT(mode) not in {0, stat.S_IFREG, stat.S_IFDIR} or item.flag_bits & 1):
                raise ValueError("Invalid SWIG archive root, link, type or encryption")
            key = relative.as_posix().casefold()
            if key in explicit:
                raise ValueError("SWIG archive collision")
            explicit.add(key)
            # Include implicit parent directories: A/x and a/y also collide on
            # Windows, as do file and file/child regardless of entry order.
            for index in range(1, len(relative.parts) + 1):
                spelling = "/".join(relative.parts[:index])
                prefix = spelling.casefold()
                is_dir = index < len(relative.parts) or item.is_dir()
                previous = names.get(prefix)
                if previous is not None and previous != (spelling, is_dir):
                    raise ValueError("SWIG archive ancestor/case collision")
                names[prefix] = (spelling, is_dir)
            total += item.file_size
            if total > MAX_EXPANSION:
                raise ValueError("SWIG archive expansion limit")
            entries.append((item, relative.parts[1:]))
        required = {"swigwin-4.3.1/swig.exe", "swigwin-4.3.1/lib/python/python.swg"}
        if not required.issubset(explicit) or any(names[p][1] for p in required):
            raise ValueError("SWIG executable or Python support files missing")
        destination.mkdir(parents=True)
        for item, parts in entries:
            target = destination.joinpath(*parts)
            if item.is_dir():
                target.mkdir(parents=True, exist_ok=True)
            else:
                target.parent.mkdir(parents=True, exist_ok=True)
                with archive.open(item) as source, target.open("xb") as output:
                    shutil.copyfileobj(source, output)


def file_table(path: pathlib.Path, *, exclude_git: bool = False) -> list[dict]:
    no_links(path, tree=True)
    records = []
    for directory, dirs, files in os.walk(path, followlinks=False):
        if exclude_git:
            dirs[:] = [d for d in dirs if d != ".git"]
            files = [f for f in files if f != ".git"]
        for name in files:
            target = pathlib.Path(directory) / name
            if not target.is_file():
                raise ValueError("Input tree contains a nonregular file")
            with target.open("rb") as stream:
                sha256 = hashlib.file_digest(stream, "sha256").hexdigest()
            records.append({"path": target.relative_to(path).as_posix(), "bytes": target.stat().st_size, "sha256": sha256})
    return sorted(records, key=lambda record: record["path"])


def prepare(root: pathlib.Path, lock_path: pathlib.Path, *, offline: bool = False, check: bool = False) -> dict:
    no_links(root)
    root = root.resolve()
    no_links(lock_path)
    lock_bytes = lock_path.read_bytes()
    lock = json.loads(lock_bytes)
    validate_lock(lock)
    source = root / SOURCE_PATH
    manifest = root / ".deps/ifc-source-preparation.json"
    cache = root / ".cache/ifc-source" / lock["swig"]["filename"]
    tools = root / ".deps/ifc-tools/swigwin-4.3.1"
    for path in (source, manifest, cache, tools):
        no_links(path)
    if check and (not manifest.is_file() or not tools.is_dir()):
        raise ValueError("IFC preparation manifest or tools missing")
    # Fail existing identity and content errors before any acquisition.
    if (source / ".git").exists():
        verify_repository(source, lock["source"], source)
        verify_gitlinks(source, lock["submodules"])
        for item in lock["submodules"]:
            path = source / item["path"]
            if (path / ".git").exists():
                verify_repository(path, item, source)
            elif path.exists() and (not path.is_dir() or any(path.iterdir())):
                raise ValueError("Preexisting foreign submodule content")
    acquire_repository(source, lock["source"], source, offline=offline or check)
    verify_gitlinks(source, lock["submodules"])
    for item in lock["submodules"]:
        acquire_repository(source / item["path"], item, source, offline=offline or check)
    source_record = verify_repository(source, lock["source"], source)
    submodule_records = [verify_repository(source / s["path"], s, source) for s in lock["submodules"]]
    data = obtain_swig(cache, lock["swig"], offline=offline or check)
    tools.parent.mkdir(parents=True, exist_ok=True)
    stage = pathlib.Path(tempfile.mkdtemp(prefix=".vertex-ifc-stage-", dir=tools.parent))
    try:
        expected = stage / "swig"
        extract_swig(data, expected)
        swig_files = file_table(expected)
        if tools.exists():
            if not tools.is_dir() or file_table(tools) != swig_files:
                raise ValueError("Existing SWIG tree differs from verified archive contents")
        source_record["files"] = file_table(source, exclude_git=True)
        result = {"schema_version": 1, "lock_sha256": hashlib.sha256(lock_bytes).hexdigest(),
                  "build_qualified": False, "source_closure_qualified": False,
                  "source": source_record, "submodules": submodule_records,
                  "swig": {"version": lock["swig"]["version"], "path": ".deps/ifc-tools/swigwin-4.3.1",
                           "archive_sha512": hashlib.sha512(data).hexdigest(), "files": swig_files}}
        if manifest.exists():
            if not manifest.is_file() or json.loads(manifest.read_bytes()) != result:
                raise ValueError("Existing IFC preparation manifest differs from verified inputs")
        if not tools.exists():
            no_links(tools)
            expected.rename(tools)
        if not manifest.exists():
            with manifest.open("x", encoding="utf-8", newline="\n") as output:
                output.write(json.dumps(result, indent=2) + "\n")
        return result
    finally:
        if stage.exists():
            remove_stage(stage, tools.parent)


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--offline", action="store_true")
    parser.add_argument("--check", action="store_true")
    args = parser.parse_args()
    result = prepare(ROOT, ROOT / "third_party/ifc-source-lock.json", offline=args.offline, check=args.check)
    print(json.dumps({"revision": result["source"]["revision"], "submodules": len(result["submodules"]),
                      "swig": result["swig"]["version"], "build_qualified": False, "source_closure_qualified": False}))


if __name__ == "__main__":
    main()
