"""Derive explicit offline CAD payload manifests from pinned archive bytes.

No downloads or package installers run here. Outputs augment the existing
distribution and portable allowlists; they never confer source/license clearance.
"""
from __future__ import annotations

import argparse
import copy
import hashlib
import io
import json
import pathlib
import tempfile
import zipfile

import bootstrap_cad_runtime as bootstrap
import distribution_inventory as inventory

ROOT = pathlib.Path(__file__).resolve().parents[1]
PE_SUFFIXES = {".dll", ".pyd", ".exe"}
# Embedded Python does not need launchers. Resolve shared CRT and SQLite imports
# to the application's separately inventoried copies, avoiding conflicting DLLs.
EXCLUSIONS = {
    "python.exe": "Embedded interpreter; no child Python launcher is used.",
    "pythonw.exe": "Embedded interpreter; no child Python launcher is used.",
    "vcruntime140.dll": "Use application's pinned MSVC runtime in bin/.",
    "vcruntime140_1.dll": "Use application's pinned MSVC runtime in bin/.",
    "sqlite3.dll": "Use application's inventoried SQLite runtime in bin/.",
}


def sha(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def selected_member(member: str, kind: str) -> str | None:
    relative = bootstrap.member_path(member)
    if kind == "interpreter":
        return None if relative.as_posix() in EXCLUSIONS else relative.as_posix()
    # Console scripts and manpages are retained in the build SDK, not installed
    # into the embedded library runtime. Other wheel installation schemes fail.
    if relative.parts[0].endswith(".data"):
        scripts = len(relative.parts) >= 3 and relative.parts[1] == "scripts"
        manual = len(relative.parts) >= 6 and relative.parts[1:4] == ("data", "share", "man")
        if not (scripts or manual):
            raise ValueError("Wheel installation scheme requires reviewed mapping")
        return None
    return "Lib/site-packages/" + relative.as_posix()


def check_publication_boundary(root: pathlib.Path, output: pathlib.Path, trees=(), files=()) -> None:
    """Publication must not add to immutable input trees or replace inputs."""
    def absolute(path):
        path = pathlib.Path(path)
        return (path if path.is_absolute() else root / path).resolve()
    destination = absolute(output)
    for tree in map(absolute, trees):
        if destination == tree or destination.is_relative_to(tree) or tree.is_relative_to(destination):
            raise ValueError("CAD manifest output and input trees must not overlap")
    targets = {destination / name for name in ("cad-components.json", "cad-allowlist.json", "cad-payload.json")}
    if any(path in targets for path in map(absolute, files)):
        raise ValueError("CAD manifest publication targets must not overlap input files")


def prepare(root: pathlib.Path, output: pathlib.Path, *, runtime_root: pathlib.Path | None = None,
            staged_root: pathlib.Path | None = None, selection_path: pathlib.Path | None = None,
            corresponding_source_paths: list[dict] | None = None) -> dict:
    root = root.resolve()
    if corresponding_source_paths is not None and not isinstance(corresponding_source_paths, list):
        raise ValueError("Controlled corresponding source inputs must be an explicit list")
    # Both inputs and outputs must remain in this repository without redirects.
    output = output if output.is_absolute() else root / output
    if any(bootstrap.is_link(p) for p in (output, *output.parents)):
        raise ValueError("CAD distribution output must not cross a link")
    if not output.resolve().is_relative_to(root) or output.resolve() == root:
        raise ValueError("CAD distribution output must be a repository child")
    lock_path = root / "third_party/cad-runtime-lock.json"
    lock = json.loads(lock_path.read_text(encoding="utf-8"))
    bootstrap.validate_lock(lock)
    sdk = root / ".deps/cad-runtime" / lock["python_version"]
    bootstrap.bootstrap(root, lock_path, sdk, offline=True, check=True)
    staged = staged_root if staged_root is not None else pathlib.Path("build/windows-release/cad-runtime")
    staged = staged if staged.is_absolute() else root / staged
    if any(bootstrap.is_link(path) for path in (staged, *staged.parents)):
        raise ValueError("CAD staged runtime must not cross a link")
    if not staged.resolve().is_relative_to(root) or staged.resolve() == root:
        raise ValueError("CAD staged runtime must be a repository child")
    staged = staged.resolve()
    controlled = None
    if runtime_root is not None:
        from controlled_cad_distribution import verify_selected_stage
        controlled = verify_selected_stage(root, runtime_root, staged,
            selection_path if selection_path is not None else root / "build/windows-release/cad-runtime-selection.json")
    elif selection_path is not None or corresponding_source_paths is not None:
        raise ValueError("Controlled selection/source inputs require an explicit runtime root")
    components = inventory.load_manifest(root / "third_party/distribution-components.json")
    allowlist = json.loads((root / "packaging/portable-allowlist.json").read_text(encoding="utf-8"))
    components = copy.deepcopy(components)
    files, excluded = [], []
    payload_names: set[str] = set()
    for asset in lock["assets"]:
        if asset["kind"] not in {"interpreter", "wheel"}:
            continue
        if controlled is not None and asset["name"] == "ifcopenshell":
            excluded.append({"archive": asset["filename"], "member": "*",
                             "reason": "Replaced by the selected source-built IFC package; wheel identity is not shipped."})
            continue
        archive = root / ".deps/downloads/cad-runtime" / asset["filename"]
        if any(bootstrap.is_link(p) for p in (archive, *archive.parents)):
            raise ValueError("CAD archive must not cross a link")
        data = bootstrap.verified_archive_bytes(asset, archive)
        identifier = "cad-" + asset["name"].lower().replace("_", "-")
        component = {
            "id": identifier, "kind": "runtime",
            "package": {key: asset[key] for key in ("name", "version", "license")},
            "source": {"kind": "locked-archive", "lock_path": "third_party/cad-runtime-lock.json",
                       "archive_filename": asset["filename"], "archive_path": archive.relative_to(root).as_posix(),
                       "paths": [], "archive_members": {}},
            "source_files": [], "notice_paths": [], "runtime_names": [], "destinations": {},
        }
        with zipfile.ZipFile(io.BytesIO(data)) as zipped:
            for entry in sorted(zipped.infolist(), key=lambda value: value.filename):
                if entry.is_dir():
                    continue
                local = selected_member(entry.filename, asset["kind"])
                if local is None:
                    excluded.append({"archive": asset["filename"], "member": entry.filename,
                                     "reason": EXCLUSIONS.get(entry.filename, "Inert console scripts/manpages remain in SDK.")})
                    continue
                path = staged / local
                if any(bootstrap.is_link(p) for p in (path, *path.parents)):
                    raise ValueError("CAD staged file must not cross a link")
                expected = zipped.read(entry)
                if asset["kind"] == "interpreter" and local == "python313._pth":
                    expected = b"python313.zip\n.\nLib/site-packages\n"
                if path.read_bytes() != expected:
                    raise ValueError(f"CAD staged file differs from pinned member: {local}")
                relative = path.relative_to(root).as_posix()
                destination = "bin/cad-runtime/" + local
                runtime = path.suffix.lower() in PE_SUFFIXES
                component["source_files"].append({"path": relative, "sha256": sha(expected)})
                component["source"]["archive_members"][relative] = entry.filename
                files.append({"path": relative, "sha256": sha(expected), "destination": destination,
                              "component": identifier, "runtime": runtime})
                if runtime:
                    if path.name.casefold() in payload_names:
                        raise ValueError(f"Ambiguous runtime basename: {path.name}")
                    payload_names.add(path.name.casefold())
                    component["runtime_names"].append(path.name)
                    component["destinations"][path.name] = destination
                else:
                    component["source"]["paths"].append(relative)
                    allowlist["entries"].append({"kind": "asset", "inventory_entry": identifier,
                                                "path": relative, "destination": destination})
                name = pathlib.PurePosixPath(local).name.lower()
                if (name.startswith(("license", "licence", "copying", "notice")) or
                        "/licenses/" in local.lower()):
                    component["notice_paths"].append(relative)
        if asset["name"] == "ifcopenshell":
            # The wheel lacks its own license file. Retain GNU license texts and
            # embedded copyright metadata; corresponding-source closure is pending.
            component["notice_paths"].extend([
                ".deps/cad-runtime/" + lock["python_version"] + "/notices/LGPL-3.0.txt",
                ".deps/cad-runtime/" + lock["python_version"] + "/notices/GPL-3.0.txt",
                next(row["path"] for row in component["source_files"] if row["path"].endswith("/ifcopenshell/__init__.py")),
            ])
            component["artifacts"] = [
                {"path": ".deps/cad-runtime/" + lock["python_version"] + "/notices/" + notice["filename"],
                 "sha256": notice["sha256"]}
                for notice in lock["assets"] if notice["kind"] == "notice"
            ]
        if not component["notice_paths"]:
            raise ValueError(f"Missing package notices: {asset['name']}")
        for index, notice in enumerate(component["notice_paths"]):
            allowlist["entries"].append({"kind": "notice", "inventory_entry": identifier,
                                        "path": notice, "destination": f"licenses/{identifier}/{index}-{pathlib.PurePosixPath(notice).name}"})
        components["components"].append(component)
    if controlled is not None:
        identifier = "cad-ifcopenshell"
        component = {
            "id": identifier, "kind": "runtime",
            "package": {"name": "IfcOpenShell", "version": "source-" + controlled["identity"]["source_revision"],
                        "license": "LGPL-3.0-or-later"},
            "source": {"kind": "controlled-runtime", "runtime_root": controlled["sdk_root"],
                       "staged_root": controlled["staged_root"],
                       "selection_path": controlled["selection_record"]["path"],
                       "selection_sha256": controlled["selection_record"]["sha256"],
                       "corresponding_source_paths": [] if corresponding_source_paths is None else corresponding_source_paths,
                       "paths": []},
            "source_files": [], "notice_paths": [], "runtime_names": [], "destinations": {},
            "artifacts": [{"path": controlled["selection_record"]["path"],
                           "sha256": controlled["selection_record"]["sha256"]}],
        }
        for item in controlled["payload"]:
            path = staged / item["path"]
            relative = path.relative_to(root).as_posix()
            destination = "bin/cad-runtime/" + item["path"]
            runtime = path.suffix.lower() in PE_SUFFIXES
            component["source_files"].append({"path": relative, "sha256": item["sha256"]})
            files.append({"path": relative, "sha256": item["sha256"], "destination": destination,
                          "component": identifier, "runtime": runtime})
            if runtime:
                if path.name.casefold() in payload_names:
                    raise ValueError(f"Ambiguous controlled runtime basename: {path.name}")
                payload_names.add(path.name.casefold())
                component["runtime_names"].append(path.name)
                component["destinations"][path.name] = destination
            else:
                component["source"]["paths"].append(relative)
                allowlist["entries"].append({"kind": "asset", "inventory_entry": identifier,
                                            "path": relative, "destination": destination})
            if item["path"].startswith("notices/ifcopenshell/"):
                component["notice_paths"].append(relative)
        if not component["notice_paths"] or component["runtime_names"] != ["_ifcopenshell_wrapper.cp313-win_amd64.pyd"]:
            raise ValueError("Controlled IFC requires its exact native module and source notices")
        for index, notice in enumerate(component["notice_paths"]):
            allowlist["entries"].append({"kind": "notice", "inventory_entry": identifier,
                "path": notice, "destination": f"licenses/{identifier}/{index}-{pathlib.PurePosixPath(notice).name}"})
        allowlist["entries"].append({"kind": "asset", "inventory_entry": identifier,
            "path": controlled["selection_record"]["path"], "destination": "metadata/cad-runtime-selection.json"})
        components["components"].append(component)
    components["components"].append({
        "id": "cad-library-adapter", "kind": "asset",
        "package": {"name": "Vertex CAD library adapter", "version": "workspace", "license": "GPL-3.0-or-later"},
        "source": {"kind": "workspace", "paths": ["src/desktop/cad_library_adapter.py", "third_party/cad-runtime-lock.json"]},
        "notice_paths": ["LICENSE"],
    })
    allowlist["entries"].extend([
        {"kind": "asset", "inventory_entry": "cad-library-adapter", "path": "src/desktop/cad_library_adapter.py", "destination": "bin/cad-runtime/cad_library_adapter.py"},
        {"kind": "asset", "inventory_entry": "cad-library-adapter", "path": "third_party/cad-runtime-lock.json", "destination": "metadata/cad-runtime-lock.json"},
        {"kind": "notice", "inventory_entry": "cad-library-adapter", "path": "LICENSE", "destination": "licenses/cad-library-adapter/Vertex-LICENSE"},
    ])
    inventory.validate_manifest(components)
    payload = {"schema_version": 1, "files": files, "excluded_files": excluded,
               "qualification": "incomplete", "license_clearance": False,
               "source_closure_qualified": False}
    if controlled is not None:
        from controlled_cad_distribution import verify_selected_stage
        if verify_selected_stage(root, runtime_root, staged,
                selection_path if selection_path is not None else root / "build/windows-release/cad-runtime-selection.json") != controlled:
            raise ValueError("Controlled runtime changed during distribution preparation")
        payload["selected_runtime"] = controlled["identity"]
    input_trees = [sdk, staged, root / ".deps/downloads/cad-runtime"]
    input_files = [lock_path, root / "third_party/distribution-components.json",
                   root / "packaging/portable-allowlist.json", root / "src/desktop/cad_library_adapter.py"]
    if controlled is not None:
        input_trees.append(root / controlled["sdk_root"])
        input_files.append(root / controlled["selection_record"]["path"])
        for row in corresponding_source_paths or []:
            (input_trees if row["kind"] == "directory" else input_files).append(root / row["path"])
    check_publication_boundary(root, output, input_trees, input_files)
    output.mkdir(parents=True, exist_ok=True)
    for name, value in (("cad-components.json", components), ("cad-allowlist.json", allowlist), ("cad-payload.json", payload)):
        target = output / name
        if bootstrap.is_link(target):
            raise ValueError("CAD manifest output must not be a link")
        # Replace the directory entry rather than truncating an existing file;
        # a pre-existing hard link must not turn an output into an input write.
        with tempfile.NamedTemporaryFile(mode="w", encoding="utf-8", newline="\n",
                                         prefix=".vertex-cad-manifest-", dir=output,
                                         delete=False) as pending:
            scratch = pathlib.Path(pending.name)
            pending.write(json.dumps(value, indent=2) + "\n")
        try:
            scratch.replace(target)
        finally:
            scratch.unlink(missing_ok=True)
    return payload


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", type=pathlib.Path, default=ROOT)
    parser.add_argument("--output", type=pathlib.Path, default=pathlib.Path("artifacts/runtime"))
    parser.add_argument("--runtime-root", type=pathlib.Path)
    parser.add_argument("--staged-root", type=pathlib.Path)
    parser.add_argument("--selection-path", type=pathlib.Path)
    parser.add_argument("--corresponding-source-manifest", type=pathlib.Path,
                        help="Explicit JSON list of verified preferred source paths; receipts alone are insufficient")
    args = parser.parse_args()
    source_paths = None
    if args.corresponding_source_manifest is not None:
        check_publication_boundary(args.root.resolve(), args.output, files=[args.corresponding_source_manifest])
        source_paths = json.loads(args.corresponding_source_manifest.read_text(encoding="utf-8"))
    result = prepare(args.root, args.output, runtime_root=args.runtime_root, staged_root=args.staged_root,
                     selection_path=args.selection_path, corresponding_source_paths=source_paths)
    print(f"Prepared {len(result['files'])} CAD runtime files; source/license qualification incomplete")


if __name__ == "__main__":
    main()
