"""Create and verify one reviewed IFC source derivative; never build or qualify.

The original preparation must first pass prepare_ifc_source.py --offline --check.
This command verifies its byte inventory and lock binding without invoking Git.
It never edits the original checkout or reuses an output tree. Failed partial
outputs are retained for inspection; the external manifest is published last.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import os
import pathlib
import re
import stat

ROOT = pathlib.Path(__file__).resolve().parents[1]
SOURCE_PATH = ".deps/ifc-src"
LOCK_PATH = "third_party/ifc-source-lock.json"
PREPARATION_PATH = ".deps/ifc-source-preparation.json"
PATCH_PATH = "third_party/ifc-source/patches/opaque-coordinate-output.i"
MODIFIED_PATH = "src/ifcwrap/utils/typemaps_out.i"
SOURCE_REVISION = "ff3c5b849eee2ef6343b537c885b971ae6bba452"
SOURCE_REPOSITORY = "https://github.com/IfcOpenShell/IfcOpenShell.git"
SUBMODULE_REPOSITORIES = {
    "src/svgfill": "https://github.com/IfcOpenShell/svgfill",
    "src/ifcopenshell-python/ifcopenshell/mvd": "https://github.com/opensourceBIM/python-mvdxml/",
    "src/ifcopenshell-python/ifcopenshell/simple_spf": "https://github.com/IfcOpenShell/step-file-parser",
}
MAX_FILES = 10_000
MAX_ENTRIES = 50_000
MAX_FILE_BYTES = 64 * 1024 * 1024
MAX_TOTAL_BYTES = 1024 * 1024 * 1024
MAX_JSON_BYTES = 8 * 1024 * 1024
RESERVED = {"con", "prn", "aux", "nul", *(f"com{i}" for i in range(1, 10)),
            *(f"lpt{i}" for i in range(1, 10))}
# A checked-in input cannot select arbitrary changes merely by changing its own
# claimed digest. Pin the reviewed canonical bytes as well as recording its SHA.
PATCH_BYTES = b"""
// Preserve owned by-value coordinate returns without copying SwigValueWrapper.
%typemap(out, noblock=1)
IfcGeom::OpaqueCoordinate<3>,
IfcGeom::OpaqueCoordinate<4> {
  $result = SWIG_NewPointerObj(
      %new_copy($1, $1_ltype),
      $&descriptor,
      SWIG_POINTER_OWN | %newpointer_flags);
}
"""


def is_link(path: pathlib.Path) -> bool:
    try:
        info = path.lstat()
    except FileNotFoundError:
        return False
    return stat.S_ISLNK(info.st_mode) or bool(getattr(info, "st_file_attributes", 0) & 0x400)


def no_links(path: pathlib.Path) -> None:
    if any(is_link(p) for p in (path, *path.parents)):
        raise ValueError(f"Path crosses a link or reparse point: {path}")


def relative_path(value: str) -> pathlib.PurePosixPath:
    if not isinstance(value, str) or not value or len(value) > 1024 or "\\" in value:
        raise ValueError("Unsafe source relative path")
    try:
        value.encode("utf-8", errors="strict")
    except UnicodeError as error:
        raise ValueError("Source relative path is not valid UTF-8") from error
    parts = value.split("/")
    if len(parts) > 64 or any(
        not p or p in {".", ".."} or len(p) > 255 or p.endswith((" ", ".")) or
        any(ord(c) < 32 or c in ':*?"<>|' for c in p) or
        p.split(".", 1)[0].casefold() in RESERVED or p.casefold() == ".git"
        for p in parts
    ):
        raise ValueError(f"Unsafe source relative path: {value}")
    return pathlib.PurePosixPath(value)


def validate_file_table(records: list[dict]) -> list[dict]:
    if not isinstance(records, list) or not 0 < len(records) <= MAX_FILES:
        raise ValueError("Source inventory file count limit")
    names = {}
    explicit = set()
    total = 0
    for record in records:
        if not isinstance(record, dict) or set(record) != {"path", "bytes", "sha256"}:
            raise ValueError("Invalid source inventory entry")
        relative = relative_path(record["path"])
        size, digest = record["bytes"], record["sha256"]
        if type(size) is not int or not 0 <= size <= MAX_FILE_BYTES:
            raise ValueError("Source inventory file size limit")
        if not isinstance(digest, str) or not re.fullmatch(r"[0-9a-f]{64}", digest):
            raise ValueError("Invalid source inventory SHA-256")
        total += size
        if total > MAX_TOTAL_BYTES:
            raise ValueError("Source inventory total byte limit")
        key = relative.as_posix().casefold()
        if key in explicit:
            raise ValueError("Source inventory duplicate/case collision")
        explicit.add(key)
        for index in range(1, len(relative.parts) + 1):
            spelling = "/".join(relative.parts[:index])
            key = spelling.casefold()
            kind = index < len(relative.parts)
            if key in names and names[key] != (spelling, kind):
                raise ValueError("Source inventory ancestor/case collision")
            names[key] = (spelling, kind)
    if records != sorted(records, key=lambda r: r["path"]):
        raise ValueError("Source inventory must be canonically sorted")
    return records


def _stamp(info: os.stat_result) -> tuple:
    # Windows lstat reports creation time in st_ctime while fstat can report
    # modification time there. Compare stable identity/content metadata instead;
    # Windows lstat also synthesizes execute permission from .bat/.exe suffixes,
    # whereas fstat has no filename and omits those bits. File kind is stable.
    # Every consumed byte is separately checked against its expected SHA-256.
    mode = stat.S_IFMT(info.st_mode) if os.name == "nt" else info.st_mode
    return (info.st_dev, info.st_ino, mode, info.st_size,
            info.st_mtime_ns)


def read_bounded(path: pathlib.Path, limit: int) -> bytes:
    try:
        no_links(path)
        before = path.lstat()
        if not stat.S_ISREG(before.st_mode) or before.st_size > limit:
            raise ValueError(f"Nonregular or oversized input: {path}")
        with path.open("rb") as stream:
            if _stamp(os.fstat(stream.fileno())) != _stamp(before):
                raise ValueError(f"Input changed before reading: {path}")
            data = stream.read(limit + 1)
            after = os.fstat(stream.fileno())
        no_links(path)
        if len(data) > limit or len(data) != before.st_size or _stamp(after) != _stamp(before) or _stamp(path.lstat()) != _stamp(before):
            raise ValueError(f"Input changed while reading: {path}")
        return data
    except OSError as error:
        raise ValueError(f"Cannot read input: {path}") from error


def _json_object(pairs):
    result = {}
    for key, value in pairs:
        if key in result:
            raise ValueError(f"Duplicate JSON key: {key}")
        result[key] = value
    return result


def parse_json(data: bytes) -> dict:
    try:
        value = json.loads(data, object_pairs_hook=_json_object,
                           parse_constant=lambda _: (_ for _ in ()).throw(ValueError("Non-finite JSON number")))
    except (UnicodeError, json.JSONDecodeError) as error:
        raise ValueError("Invalid UTF-8 JSON input") from error
    if not isinstance(value, dict):
        raise ValueError("Expected a JSON object")
    return value


def _input(path: pathlib.Path, data: bytes) -> dict:
    return {"path": str(path), "bytes": len(data), "sha256": hashlib.sha256(data).hexdigest()}


def load_bindings(workspace: pathlib.Path) -> dict:
    paths = {"lock": workspace / LOCK_PATH, "preparation": workspace / PREPARATION_PATH,
             "patch": workspace / PATCH_PATH, "script": pathlib.Path(__file__).resolve()}
    data = {key: read_bounded(path, MAX_JSON_BYTES if key in {"lock", "preparation"} else MAX_FILE_BYTES)
            for key, path in paths.items()}
    lock, preparation = parse_json(data["lock"]), parse_json(data["preparation"])
    if data["patch"] != PATCH_BYTES:
        raise ValueError("Reviewed patch byte binding differs")
    expected_source = {"path": SOURCE_PATH, "repository": SOURCE_REPOSITORY, "revision": SOURCE_REVISION}
    if (type(lock.get("schema_version")) is not int or lock["schema_version"] != 1 or
        lock.get("platform") != "win_amd64" or lock.get("source") != expected_source or
        type(preparation.get("schema_version")) is not int or preparation["schema_version"] != 1):
        raise ValueError("Unsupported source lock or preparation")
    for value in (lock, preparation):
        if value.get("build_qualified") is not False or value.get("source_closure_qualified") is not False:
            raise ValueError("Source derivation cannot qualify a build or source closure")
    if preparation.get("lock_sha256") != hashlib.sha256(data["lock"]).hexdigest():
        raise ValueError("Preparation lock SHA-256 binding differs")
    source = preparation.get("source")
    if not isinstance(source, dict) or set(source) != {*expected_source, "files"} or any(source[k] != v for k, v in expected_source.items()):
        raise ValueError("Preparation source identity differs from lock")
    records = validate_file_table(source["files"])
    if MODIFIED_PATH not in {record["path"] for record in records}:
        raise ValueError("Source inventory lacks the reviewed patch target")
    submodules = lock.get("submodules")
    if not isinstance(submodules, list) or len(submodules) != len(SUBMODULE_REPOSITORIES):
        raise ValueError("Invalid locked submodule set")
    identities = []
    for item in submodules:
        if (not isinstance(item, dict) or
            any(not isinstance(item.get(key), str) for key in ("path", "repository", "revision")) or
            item["repository"] != SUBMODULE_REPOSITORIES.get(item["path"]) or
            not re.fullmatch(r"[0-9a-f]{40}", item["revision"])):
            raise ValueError("Invalid locked submodule identity")
        identities.append({k: item[k] for k in ("path", "repository", "revision")})
    if {item["path"] for item in identities} != set(SUBMODULE_REPOSITORIES) or preparation.get("submodules") != identities:
        raise ValueError("Preparation submodule identity binding differs")
    if any(not any(record["path"].startswith(item["path"] + "/") for record in records) for item in identities):
        raise ValueError("Source inventory lacks a selected submodule")
    swig, prepared_swig = lock.get("swig"), preparation.get("swig")
    if (not isinstance(swig, dict) or swig.get("version") != "4.3.1" or
        not isinstance(swig.get("sha512"), str) or not re.fullmatch(r"[0-9a-f]{128}", swig["sha512"]) or
        not isinstance(prepared_swig, dict) or prepared_swig.get("version") != "4.3.1" or
        prepared_swig.get("path") != ".deps/ifc-tools/swigwin-4.3.1" or prepared_swig.get("archive_sha512") != swig["sha512"]):
        raise ValueError("Preparation SWIG archive binding differs")
    return {"inputs": {key: _input(path, data[key]) for key, path in paths.items()},
            "records": records, "patch": data["patch"]}


def inventory(root: pathlib.Path, *, allow_git_metadata: bool = False) -> list[dict]:
    no_links(root)
    if not root.is_dir():
        raise ValueError(f"Source tree is missing: {root}")
    records = []
    entries = total = 0
    git_roots = {"", *SUBMODULE_REPOSITORIES} if allow_git_metadata else set()
    for directory, dirs, files in os.walk(root, followlinks=False):
        parent = pathlib.Path(directory)
        no_links(parent)
        for name in [*dirs, *files]:
            target = parent / name
            no_links(target)
            entries += 1
            if entries > MAX_ENTRIES:
                raise ValueError("Source tree entry count limit")
            if name.casefold() == ".git":
                relative_parent = parent.relative_to(root).as_posix()
                if relative_parent == ".":
                    relative_parent = ""
                if name != ".git" or relative_parent not in git_roots:
                    raise ValueError("Foreign Git metadata in source tree")
                if name in dirs:
                    dirs.remove(name)
                if name in files:
                    files.remove(name)
                continue
            relative = target.relative_to(root).as_posix()
            relative_path(relative)
            info = target.lstat()
            if name in dirs:
                if not stat.S_ISDIR(info.st_mode):
                    raise ValueError("Nonregular source directory")
                continue
            data = read_bounded(target, MAX_FILE_BYTES)
            total += len(data)
            if len(records) >= MAX_FILES or total > MAX_TOTAL_BYTES:
                raise ValueError("Source tree file count or byte limit")
            records.append({"path": relative, "bytes": len(data), "sha256": hashlib.sha256(data).hexdigest()})
    return validate_file_table(sorted(records, key=lambda r: r["path"]))


def verify_tree(root: pathlib.Path, records: list[dict], *, allow_git_metadata: bool = False) -> None:
    if inventory(root, allow_git_metadata=allow_git_metadata) != records:
        raise ValueError("Source tree differs from its exact byte inventory")


def checked_bytes(path: pathlib.Path, record: dict) -> bytes:
    data = read_bounded(path, MAX_FILE_BYTES)
    if len(data) != record["bytes"] or hashlib.sha256(data).hexdigest() != record["sha256"]:
        raise ValueError(f"Source file differs from preparation: {record['path']}")
    return data


def copy_checked_file(source: pathlib.Path, destination: pathlib.Path, record: dict,
                      patch_bytes: bytes = b"") -> None:
    data = checked_bytes(source, record) + patch_bytes
    no_links(destination)
    destination.parent.mkdir(parents=True, exist_ok=True)
    no_links(destination)
    with destination.open("xb") as stream:
        stream.write(data)


def _paths(workspace, output, manifest_path, *, fresh: bool) -> tuple[pathlib.Path, ...]:
    values = [pathlib.Path(p) for p in (workspace, output, manifest_path)]
    if any(not p.is_absolute() for p in values):
        raise ValueError("Workspace, output and manifest paths must be absolute")
    for path in values:
        no_links(path)
    workspace, output, manifest_path = [p.resolve() for p in values]
    overlap = lambda a, b: a.is_relative_to(b) or b.is_relative_to(a)
    if (not workspace.is_dir() or overlap(workspace, output) or overlap(workspace, manifest_path) or
        overlap(output, manifest_path) or any(part.casefold() == ".git" for p in (output, manifest_path) for part in p.parts)):
        raise ValueError("Source/workspace/output/manifest paths overlap or target Git metadata")
    if not output.parent.is_dir() or not manifest_path.parent.is_dir():
        raise ValueError("Output and manifest parent directories must already exist")
    if fresh and (output.exists() or manifest_path.exists()):
        raise ValueError("Existing foreign output or manifest is preserved; fresh paths required")
    return workspace, output, manifest_path


def expected_manifest(workspace: pathlib.Path, output: pathlib.Path,
                      manifest_path: pathlib.Path, binding: dict) -> dict:
    records = [dict(record) for record in binding["records"]]
    modified = next(record for record in records if record["path"] == MODIFIED_PATH)
    original = checked_bytes(workspace / SOURCE_PATH / MODIFIED_PATH, modified)
    patched = original + binding["patch"]
    delta = {"path": MODIFIED_PATH, "original_bytes": len(original),
             "original_sha256": modified["sha256"], "derived_bytes": len(patched),
             "derived_sha256": hashlib.sha256(patched).hexdigest()}
    modified.update(bytes=delta["derived_bytes"], sha256=delta["derived_sha256"])
    validate_file_table(records)
    return {"schema_version": 1, "build_qualified": False, "source_closure_qualified": False,
            "product_runtime_replaced": False, "workspace": str(workspace),
            "source_path": str(workspace / SOURCE_PATH), "output_path": str(output),
            "manifest_path": str(manifest_path), "source_revision": SOURCE_REVISION,
            "source_repository": SOURCE_REPOSITORY, "inputs": binding["inputs"],
            "modified_file": delta, "files": records}


def derive(workspace, output, manifest_path) -> dict:
    workspace, output, manifest_path = _paths(workspace, output, manifest_path, fresh=True)
    binding = load_bindings(workspace)
    source = workspace / SOURCE_PATH
    verify_tree(source, binding["records"], allow_git_metadata=True)
    result = expected_manifest(workspace, output, manifest_path, binding)
    # Reserve a new tree exclusively. Never delete it, even after a failed copy.
    no_links(output)
    output.mkdir()
    for record in binding["records"]:
        relative = relative_path(record["path"])
        copy_checked_file(source.joinpath(*relative.parts), output.joinpath(*relative.parts), record,
                          binding["patch"] if record["path"] == MODIFIED_PATH else b"")
    verify_tree(output, result["files"])
    verify_tree(source, binding["records"], allow_git_metadata=True)
    if load_bindings(workspace) != binding:
        raise ValueError("Derivation input binding changed during copy")
    _paths(workspace, output, manifest_path, fresh=False)
    payload = (json.dumps(result, indent=2) + "\n").encode("utf-8")
    if len(payload) > MAX_JSON_BYTES:
        raise ValueError("Derivation manifest size limit")
    # A complete inventory is published only after the final input/tree checks.
    with manifest_path.open("xb") as stream:
        stream.write(payload)
    return result


def verify_derivation(workspace, output, manifest_path) -> dict:
    """Recompute the allowed delta and validate every original/derived file.

    Callers must separately bind this verifier and the external manifest to
    their build evidence; an editable local preparation is not authentication.
    """
    workspace, output, manifest_path = _paths(workspace, output, manifest_path, fresh=False)
    manifest_bytes = read_bounded(manifest_path, MAX_JSON_BYTES)
    claimed = parse_json(manifest_bytes)
    # JSON booleans and integers compare equal in Python; check boundary types
    # before comparing the claimed document with the recomputed inventory.
    if (type(claimed.get("schema_version")) is not int or claimed["schema_version"] != 1 or
        any(claimed.get(key) is not False for key in
            ("build_qualified", "source_closure_qualified", "product_runtime_replaced"))):
        raise ValueError("Invalid derivation manifest schema or qualification flags")
    validate_file_table(claimed.get("files"))
    binding = load_bindings(workspace)
    expected = expected_manifest(workspace, output, manifest_path, binding)
    if claimed != expected:
        raise ValueError("Derivation manifest differs from bound inputs or reviewed delta")
    verify_tree(workspace / SOURCE_PATH, binding["records"], allow_git_metadata=True)
    verify_tree(output, expected["files"])
    if load_bindings(workspace) != binding or read_bounded(manifest_path, MAX_JSON_BYTES) != manifest_bytes:
        raise ValueError("Derivation bindings changed during verification")
    return expected


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--workspace", type=pathlib.Path, required=True)
    parser.add_argument("--output", type=pathlib.Path, required=True)
    parser.add_argument("--manifest", type=pathlib.Path, required=True)
    args = parser.parse_args()
    result = derive(args.workspace, args.output, args.manifest)
    print(json.dumps({"source_revision": result["source_revision"], "files": len(result["files"]),
                      "manifest_path": result["manifest_path"], "build_qualified": False,
                      "source_closure_qualified": False, "product_runtime_replaced": False}))


if __name__ == "__main__":
    main()
