"""Stage an unqualified, source-built IFC Python candidate in a fresh directory.

Consumes completed local source-build evidence. Does not install, import, build,
download, create a wheel, or modify the product runtime. C++ corresponding source
and dependency license closure require separate delivery and qualification.
"""
from __future__ import annotations

import argparse
import hashlib
import types
import json
import os
import pathlib
import re
import stat
import struct

ROOT = pathlib.Path(__file__).resolve().parents[1]
SOURCE = ".deps/ifc-src"
PACKAGE = "src/ifcopenshell-python/ifcopenshell"
SCHEMAS = "2x3;4;4x1;4x2;4x3;4x3_tc1;4x3_add1;4x3_add2"
MAX_FILES = 40_000
MAX_JSON_BYTES = 16_000_000
MAX_FILE_BYTES = 512_000_000
MAX_TOTAL_BYTES = 2_000_000_000
# Static OCCT/Boost SDK inputs are hashed, never copied into the Python payload.
# The observed TKDESTEP archive is ~987 MB and SDK inputs total ~3.6 GB.
MAX_INPUT_FILE_BYTES = 2_000_000_000
MAX_INPUT_TOTAL_BYTES = 16_000_000_000
PYTHON_VERSION_CODE = 'import sys,struct; assert sys.version_info[:3] == (3,13,15) and struct.calcsize("P") == 8; print(sys.version)'
RESERVED = {"con", "prn", "aux", "nul", "conin$", "conout$", *(f"com{i}" for i in "123456789¹²³"),
            *(f"lpt{i}" for i in "123456789¹²³")}
FORBIDDEN_PARTS = {"__pycache__", ".cache", ".pytest_cache", ".mypy_cache", ".ruff_cache", "node_modules"}
FORBIDDEN_SUFFIXES = {".pyc", ".pyo", ".pyd", ".dll", ".exe", ".obj", ".lib", ".pdb", ".so", ".a"}


def is_link(path: pathlib.Path) -> bool:
    try:
        info = path.lstat()
    except FileNotFoundError:
        return False
    return stat.S_ISLNK(info.st_mode) or bool(getattr(info, "st_file_attributes", 0) & 0x400)


def no_links(path: pathlib.Path) -> None:
    if any(is_link(item) for item in (path, *path.parents)):
        raise ValueError(f"Path crosses a link or reparse point: {path}")


def relative_path(name: str) -> pathlib.PurePosixPath:
    if not isinstance(name, str) or not name or "\\" in name or any(ord(c) < 32 for c in name):
        raise ValueError("Unsafe Windows relative path")
    parts = name.split("/")
    if any(p in {"", ".", ".."} or p.endswith((" ", ".")) or
           any(c in p for c in ':*?"<>|') or p.split(".", 1)[0].casefold() in RESERVED for p in parts):
        raise ValueError(f"Unsafe Windows relative path: {name}")
    return pathlib.PurePosixPath(name)


def absolute_path(value: str | pathlib.Path) -> pathlib.Path:
    text = str(value)
    path = pathlib.Path(value)
    if (not path.is_absolute() or text.startswith(("\\\\", "//")) or
            any(ord(c) < 32 for c in text) or ";" in text):
        raise ValueError("Expected an absolute local path without control characters or list separators")
    # pathlib normalizes some dangerous components, so inspect the original too.
    tail = text.replace("\\", "/")
    if re.match(r"^[A-Za-z]:/", tail):
        tail = tail[3:]
    else:
        tail = tail.lstrip("/")
    relative_path(tail)
    no_links(path)
    return path.resolve()


def same_path(left: str | pathlib.Path, right: pathlib.Path) -> bool:
    return absolute_path(left) == right


def digest(path: pathlib.Path, *, input_file: bool = False) -> dict:
    no_links(path)
    if not path.is_file():
        raise ValueError(f"Missing or nonregular file: {path}")
    info = path.stat()
    limit = MAX_INPUT_FILE_BYTES if input_file else MAX_FILE_BYTES
    if not stat.S_ISREG(info.st_mode) or info.st_size > limit:
        raise ValueError(f"Missing, nonregular or oversized file: {path}")
    hasher = hashlib.sha256()
    size = 0
    with path.open("rb") as stream:
        while chunk := stream.read(1 << 20):
            size += len(chunk)
            if size > limit:
                raise ValueError("File grew beyond the size bound")
            hasher.update(chunk)
    if size != info.st_size:
        raise ValueError("File changed during hashing")
    return {"bytes": size, "sha256": hasher.hexdigest()}


def checked_record(record: dict, *, input_file: bool = False) -> dict:
    limit = MAX_INPUT_FILE_BYTES if input_file else MAX_FILE_BYTES
    if (not isinstance(record, dict) or type(record.get("bytes")) is not int or
            not 0 <= record["bytes"] <= limit or
            not isinstance(record.get("sha256"), str) or not re.fullmatch(r"[0-9a-f]{64}", record["sha256"])):
        raise ValueError("Invalid file evidence record")
    return {"bytes": record["bytes"], "sha256": record["sha256"]}


def verify_file(path: pathlib.Path, record: dict, *, input_file: bool = False) -> None:
    if digest(path, input_file=input_file) != checked_record(record, input_file=input_file):
        raise ValueError(f"File differs from bound evidence: {path}")


def unique_object(pairs: list) -> dict:
    result = {}
    for key, value in pairs:
        if key in result:
            raise ValueError("Duplicate JSON key")
        result[key] = value
    return result


def read_json(path: pathlib.Path) -> dict:
    no_links(path)
    if not path.is_file() or path.stat().st_size > MAX_JSON_BYTES:
        raise ValueError("Missing or oversized JSON evidence")
    with path.open("rb") as stream:
        data = stream.read(MAX_JSON_BYTES + 1)
    if len(data) > MAX_JSON_BYTES:
        raise ValueError("JSON evidence grew beyond bound")
    result = json.loads(data.decode("utf-8-sig"), object_pairs_hook=unique_object)
    if not isinstance(result, dict):
        raise ValueError("Expected JSON object")
    return result


def unqualified(value: dict, *, schema: bool = True) -> None:
    if ((schema and value.get("schema_version") != 1) or value.get("build_qualified") is not False or
            value.get("source_closure_qualified") is not False):
        raise ValueError("Evidence must retain the supported unqualified state")


def source_table(source: pathlib.Path, records: list) -> dict:
    if not isinstance(records, list) or not 0 < len(records) <= MAX_FILES:
        raise ValueError("Missing or oversized source file table")
    table = {}
    spellings = {}
    total = 0
    for item in records:
        relative = relative_path(item["path"])
        if any(part.casefold() == ".git" for part in relative.parts):
            raise ValueError("Source table contains Git metadata")
        if item["path"] in table:
            raise ValueError("Duplicate source file")
        # Check implicit ancestors too, to catch A/x versus a/y on Windows.
        for index in range(1, len(relative.parts) + 1):
            spelling = "/".join(relative.parts[:index])
            kind = index < len(relative.parts)
            previous = spellings.get(spelling.casefold())
            if previous is not None and previous != (spelling, kind):
                raise ValueError("Source case or file/directory collision")
            spellings[spelling.casefold()] = (spelling, kind)
        total += checked_record(item)["bytes"]
        if total > MAX_TOTAL_BYTES:
            raise ValueError("Source snapshot exceeds total size bound")
        table[item["path"]] = item
    actual = set()
    entries = 0
    for directory, dirs, files in os.walk(source, followlinks=False):
        entries += len(dirs) + len(files)
        if entries > MAX_FILES * 3:
            raise ValueError("Source tree exceeds entry bound")
        for name in (*dirs, *files):
            no_links(pathlib.Path(directory) / name)
        dirs[:] = [name for name in dirs if name.casefold() != ".git"]
        for name in files:
            if name.casefold() == ".git":
                continue
            path = pathlib.Path(directory) / name
            relative = path.relative_to(source).as_posix()
            if relative not in table:
                raise ValueError(f"Source contains unreviewed file: {relative}")
            verify_file(path, table[relative])
            actual.add(relative)
    if actual != table.keys():
        raise ValueError("Source preparation snapshot has missing files")
    return table


def validate_pe(path: pathlib.Path) -> None:
    # Structural platform check only; no load/import and no dependency claim.
    with path.open("rb") as stream:
        header = stream.read(64)
        if len(header) != 64 or header[:2] != b"MZ":
            raise ValueError("Candidate extension lacks a PE DOS header")
        offset = struct.unpack_from("<I", header, 0x3c)[0]
        if not 64 <= offset <= min(MAX_FILE_BYTES, path.stat().st_size - 24):
            raise ValueError("Invalid PE header offset")
        stream.seek(offset)
        pe = stream.read(24)
    if (pe[:4] != b"PE\0\0" or struct.unpack_from("<H", pe, 4)[0] != 0x8664 or
            not struct.unpack_from("<H", pe, 22)[0] & 0x2000):
        raise ValueError("Expected an AMD64 PE DLL extension")


def validate_output(output: pathlib.Path, protected: list[pathlib.Path]) -> None:
    if (output == pathlib.Path(output.anchor) or len(str(output)) > 120 or not output.parent.is_dir() or output.exists()):
        raise ValueError("Output requires a fresh short directory with an existing parent")
    for path in protected:
        if output == path or output.is_relative_to(path) or path.is_relative_to(output):
            raise ValueError("Output overlaps workspace, build or evidence inputs")


def bounded_text(path: pathlib.Path) -> str:
    no_links(path)
    if not path.is_file() or path.stat().st_size > MAX_JSON_BYTES:
        raise ValueError("Missing or oversized text evidence")
    with path.open("rb") as stream:
        data = stream.read(MAX_JSON_BYTES + 1)
    if len(data) > MAX_JSON_BYTES:
        raise ValueError("Text evidence grew beyond bound")
    return data.decode("utf-8-sig")


def validate_configuration(root: pathlib.Path, build: pathlib.Path, evidence: dict,
                           inputs: dict, by_name: dict, configure: list, options: dict,
                           source: pathlib.Path | None = None) -> None:
    cache_path = build / "build/CMakeCache.txt"
    cache = {}
    for line in bounded_text(cache_path).splitlines():
        if not line or line.startswith(("#", "//")):
            continue
        match = re.fullmatch(r"([^:]+):[^=]+=(.*)", line)
        if not match or match[1] in cache:
            raise ValueError("Invalid or duplicate CMake cache entry")
        cache[match[1]] = match[2]
    verify_file(cache_path, inputs[cache_path], input_file=True)
    for key, value in options.items():
        if cache.get(key, "").replace("\\", "/") != value.replace("\\", "/"):
            raise ValueError(f"Bound CMake cache differs from configure argument: {key}")
    for flag, expected in (("-G", "Visual Studio 17 2022"), ("-A", "x64")):
        if configure.count(flag) != 1 or configure.index(flag) + 1 >= len(configure) or configure[configure.index(flag) + 1] != expected:
            raise ValueError("Expected Visual Studio 17 x64 configure command")
    if cache.get("CMAKE_GENERATOR") != "Visual Studio 17 2022" or cache.get("CMAKE_GENERATOR_PLATFORM") != "x64":
        raise ValueError("Bound CMake cache generator/platform differs")
    for key, path in (("CMAKE_HOME_DIRECTORY", (source or root / SOURCE) / "cmake"), ("CMAKE_CACHEFILE_DIR", build / "build")):
        if key not in cache or not same_path(cache[key], path):
            raise ValueError("Bound CMake cache source/build root differs")
    if "CMAKE_COMMAND" not in cache:
        raise ValueError("Bound CMake cache has no executable")
    cmake = absolute_path(cache["CMAKE_COMMAND"])
    if cmake not in inputs:
        raise ValueError("CMake executable must be a bound build input")
    for name in ("configure", "build-release"):
        command = by_name[name]
        if "executable" not in command or not same_path(command["executable"], cmake):
            raise ValueError("CMake command differs from the bound cache executable")
    python = root / ".deps/cad-runtime/3.13.15"
    executable = python / "python.exe"
    for key, path in (("PYTHON_EXECUTABLE", executable), ("PYTHON_INCLUDE_DIR", python / "include"),
                      ("PYTHON_LIBRARY", python / "libs/python313.lib")):
        if key not in options or not same_path(options[key], path):
            raise ValueError("Configured Python path differs from locked CPython 3.13.15")
    required = {executable, python / "libs/python313.lib", python / "include/Python.h",
                python / "include/patchlevel.h", python / "runtime-manifest.json"}
    if not required.issubset(inputs):
        raise ValueError("Python executable, library, headers and runtime manifest must be bound inputs")
    header = python / "include/patchlevel.h"
    if not re.search(r'^\s*#define\s+PY_VERSION\s+"3\.13\.15"\s*$', bounded_text(header), re.MULTILINE):
        raise ValueError("Bound Python development header version differs")
    runtime_manifest = python / "runtime-manifest.json"
    if read_json(runtime_manifest).get("python_version") != "3.13.15":
        raise ValueError("Bound Python runtime manifest version differs")
    for path in (header, runtime_manifest):
        verify_file(path, inputs[path], input_file=True)
    for name in ("source-check", "python-version"):
        if name not in by_name or "executable" not in by_name[name] or not same_path(by_name[name]["executable"], executable):
            raise ValueError("Python check command does not use the bound interpreter")
    version = by_name["python-version"]
    if version.get("arguments") != ["-I", "-B", "-c", PYTHON_VERSION_CODE]:
        raise ValueError("Python version command must assert 3.13.15 and 64-bit pointers")
    version_log = build / "python-version.stdout.log"
    if "stdout" not in version or not same_path(version["stdout"], version_log):
        raise ValueError("Python version stdout is not the controlled build log")
    logs = evidence.get("logs")
    if not isinstance(logs, list) or len(logs) > MAX_FILES:
        raise ValueError("Missing or excessive build log evidence")
    log_records = {}
    for item in logs:
        path = absolute_path(item["path"])
        if path in log_records or not isinstance(item.get("sha256"), str) or not re.fullmatch(r"[0-9a-f]{64}", item["sha256"]):
            raise ValueError("Invalid or duplicate log evidence")
        log_records[path] = item["sha256"]
    text = bounded_text(version_log)
    if log_records.get(version_log) != digest(version_log)["sha256"] or not re.match(r"3\.13\.15(?:\s|$)", text):
        raise ValueError("Bound Python version log differs or reports another version")


def validate_build_source(root: pathlib.Path, build: pathlib.Path, evidence: dict,
                          inputs: dict, by_name: dict) -> tuple[pathlib.Path, list[pathlib.Path]]:
    """Accept only the bound, independently replayable single-file source delta."""
    if "source_derivation" not in evidence:
        if "source-derive" in by_name:
            raise ValueError("Derived source command has no bound derivation manifest")
        return root / SOURCE, []
    manifest = absolute_path(evidence["source_derivation"])
    helper = root / "scripts/prepare_ifc_derived_source.py"
    patch = root / "third_party/ifc-source/patches/opaque-coordinate-output.i"
    source = build / "source"
    required = {manifest, helper, patch}
    if manifest != build / "source-derivation.json" or not required.issubset(inputs):
        raise ValueError("Derived source manifest, helper and patch must be controlled bound inputs")
    for path in required:
        verify_file(path, inputs[path], input_file=True)
    # Execute only this stager's shipped verifier, never an evidence-selected
    # Python file. Its exact bytes must also match the build's bound helper.
    verifier_path = ROOT / "scripts/prepare_ifc_derived_source.py"
    verify_file(verifier_path, inputs[helper], input_file=True)
    command = by_name.get("source-derive", {})
    expected = ["-I", "-B", str(helper), "--workspace", str(root),
                "--output", str(source), "--manifest", str(manifest)]
    arguments = command.get("arguments")
    if (not isinstance(arguments, list) or len(arguments) != len(expected) or
            any(not same_path(arguments[index], pathlib.Path(expected[index])) for index in (2, 4, 6, 8)) or
            any(arguments[index] != expected[index] for index in (0, 1, 3, 5, 7)) or
            not same_path(command.get("executable", ""), root / ".deps/cad-runtime/3.13.15/python.exe")):
        raise ValueError("Derived source command differs from its bound isolated invocation")
    # SourceFileLoader may execute timestamp-valid unbound cached bytecode,
    # even with -B. Compile the actual bounded, hash-checked source instead.
    with verifier_path.open("rb") as stream:
        verifier_bytes = stream.read(inputs[helper]["bytes"] + 1)
    if (len(verifier_bytes) != inputs[helper]["bytes"] or
            hashlib.sha256(verifier_bytes).hexdigest() != inputs[helper]["sha256"]):
        raise ValueError("Shipped source derivation verifier bytes changed before execution")
    verifier = types.ModuleType("vertex_ifc_derivation_verifier")
    verifier.__file__ = str(verifier_path)
    exec(compile(verifier_bytes, str(verifier_path), "exec"), verifier.__dict__)
    verifier.verify_derivation(root, source, manifest)
    for path in required:
        verify_file(path, inputs[path], input_file=True)
    return source, [manifest, helper, patch]


def validate_inventory(output: pathlib.Path, records: list[dict]) -> None:
    """Check copied bytes and exact files/directories before manifest publication."""
    expected = {item["path"]: item for item in records}
    if len(expected) != len(records):
        raise ValueError("Duplicate candidate output record")
    expected_dirs = set()
    for name in expected:
        relative = relative_path(name)
        expected_dirs.update(parent.as_posix() for parent in relative.parents if parent.as_posix() != ".")
    no_links(output)
    actual = set()
    actual_dirs = set()
    entries = 0
    for directory, dirs, files in os.walk(output, followlinks=False):
        entries += len(dirs) + len(files)
        if entries > MAX_FILES * 3:
            raise ValueError("Candidate output inventory exceeds bound; partial candidate preserved")
        for name in (*dirs, *files):
            path = pathlib.Path(directory) / name
            no_links(path)
            relative = path.relative_to(output).as_posix()
            if name in dirs:
                actual_dirs.add(relative)
                if relative not in expected_dirs:
                    raise ValueError("Foreign directory in candidate; partial candidate preserved")
            else:
                if relative not in expected:
                    raise ValueError("Foreign file in candidate; partial candidate preserved")
                verify_file(path, expected[relative])
                actual.add(relative)
    if actual != expected.keys() or actual_dirs != expected_dirs:
        raise ValueError("Candidate inventory differs from planned payload; partial candidate preserved")


def prepare(root: pathlib.Path, evidence_path: pathlib.Path, output: pathlib.Path,
            *, recipe_snapshot: pathlib.Path | None = None) -> dict:
    root, evidence_path, output = map(absolute_path, (root, evidence_path, output))
    evidence_record = digest(evidence_path)
    evidence = read_json(evidence_path)
    verify_file(evidence_path, evidence_record)
    unqualified(evidence)
    build = absolute_path(evidence["build_root"])
    if (evidence.get("state") != "built-unqualified" or evidence.get("configuration") != "Release" or
            evidence.get("product_runtime_replaced") is not False or not same_path(evidence["workspace"], root) or
            evidence_path != build / "build-evidence.json" or build == root or build.is_relative_to(root) or root.is_relative_to(build)):
        raise ValueError("Expected completed separate Release build evidence for this workspace")
    validate_output(output, [root, build])
    lock_path = root / "third_party/ifc-source-lock.json"
    preparation_path = root / ".deps/ifc-source-preparation.json"
    source_checker = root / "scripts/prepare_ifc_source.py"
    recipe_path = root / "scripts/build-ifc-source.ps1"
    input_records = evidence.get("inputs")
    if not isinstance(input_records, list) or not 0 < len(input_records) <= MAX_FILES:
        raise ValueError("Missing or excessive build input table")
    inputs = {}
    total = 0
    for item in input_records:
        path = absolute_path(item["path"])
        if path in inputs:
            raise ValueError("Duplicate build input")
        inputs[path] = item
        total += checked_record(item, input_file=True)["bytes"]
        if total > MAX_INPUT_TOTAL_BYTES:
            raise ValueError("Build inputs exceed total size bound")
    if recipe_path not in inputs and build / "build-recipe.ps1" in inputs:
        recipe_path = build / "build-recipe.ps1"
    required = {lock_path, preparation_path, source_checker, recipe_path, build / "build/CMakeCache.txt"}
    if not required.issubset(inputs):
        raise ValueError("Build evidence does not bind preparation, lock, recipe, checker and configured cache")
    snapshot = None if recipe_snapshot is None else absolute_path(recipe_snapshot)
    if snapshot is not None and snapshot != build / "build-recipe.ps1":
        raise ValueError("Recipe snapshot must be the build's explicit build-recipe.ps1")
    effective_recipe = snapshot or recipe_path
    validate_output(output, [*inputs, effective_recipe])
    for path, record in inputs.items():
        # Only the recipe may use its saved invocation bytes after a tracked
        # recipe edit. The snapshot must match the original build input hash.
        verify_file(effective_recipe if path == recipe_path else path, record, input_file=True)
    lock, preparation = read_json(lock_path), read_json(preparation_path)
    verify_file(lock_path, inputs[lock_path])
    verify_file(preparation_path, inputs[preparation_path])
    unqualified(lock)
    unqualified(preparation)
    if (lock.get("platform") != "win_amd64" or lock["source"].get("path") != SOURCE or
            lock["source"].get("repository") != "https://github.com/IfcOpenShell/IfcOpenShell.git" or
            not re.fullmatch(r"[0-9a-f]{40}", lock["source"].get("revision", "")) or
            preparation.get("lock_sha256") != inputs[lock_path]["sha256"]):
        raise ValueError("Source lock or preparation binding differs")
    if any(preparation["source"].get(key) != lock["source"].get(key) for key in ("path", "repository", "revision")):
        raise ValueError("Prepared source identity differs from lock")
    selected_submodules = {"src/svgfill", f"{PACKAGE}/mvd", f"{PACKAGE}/simple_spf"}
    submodule_repositories = {"src/svgfill": "https://github.com/IfcOpenShell/svgfill",
                             f"{PACKAGE}/mvd": "https://github.com/opensourceBIM/python-mvdxml/",
                             f"{PACKAGE}/simple_spf": "https://github.com/IfcOpenShell/step-file-parser"}
    submodules = lock.get("submodules", [])
    if (len(submodules) != 3 or {s["path"] for s in submodules} != selected_submodules or
            preparation.get("submodules") != [{key: s[key] for key in ("path", "repository", "revision")} for s in submodules] or
            any(s["repository"] != submodule_repositories[s["path"]] or not re.fullmatch(r"[0-9a-f]{40}", s["revision"]) for s in submodules) or
            lock["swig"].get("version") != "4.3.1" or preparation["swig"].get("version") != "4.3.1" or
            preparation["swig"].get("archive_sha512") != lock["swig"].get("sha512")):
        raise ValueError("Prepared submodules or SWIG differ from lock")
    commands = evidence.get("commands", [])
    if not isinstance(commands, list) or len(commands) > 100:
        raise ValueError("Invalid build command table")
    by_name = {}
    for command in commands:
        if command["name"] in by_name or type(command.get("exit_code")) is not int or command["exit_code"] != 0:
            raise ValueError("Duplicate, failed or unfinished build command")
        by_name[command["name"]] = command
    if not {"source-check", "configure", "build-release"}.issubset(by_name):
        raise ValueError("Required successful source-check/configure/build-release evidence missing")
    source_args = by_name["source-check"].get("arguments")
    if (not isinstance(source_args, list) or len(source_args) != 5 or source_args[:2] != ["-I", "-B"] or
            not same_path(source_args[2], source_checker) or source_args[3:] != ["--offline", "--check"]):
        raise ValueError("Source-check does not match bound offline preparation checker")
    configure = evidence.get("configure_arguments")
    if not isinstance(configure, list) or configure != by_name["configure"].get("arguments") or len(configure) > 200:
        raise ValueError("Configure command differs from recorded arguments")
    options = {}
    for argument in configure:
        if not isinstance(argument, str):
            raise ValueError("Invalid configure argument")
        if argument.startswith("-D"):
            key, separator, value = argument[2:].partition("=")
            if not separator or key in options:
                raise ValueError("Duplicate or invalid configure option")
            options[key] = value
    for key, expected in {"SCHEMA_VERSIONS": SCHEMAS, "CMAKE_CONFIGURATION_TYPES": "Release", "BUILD_IFCPYTHON": "ON", "BUILD_IFCGEOM": "ON"}.items():
        if options.get(key) != expected:
            raise ValueError(f"Required configure option differs: {key}")
    build_source, derivation_inputs = validate_build_source(root, build, evidence, inputs, by_name)
    for flag, expected in (("-S", build_source / "cmake"), ("-B", build / "build")):
        if configure.count(flag) != 1 or configure.index(flag) + 1 >= len(configure) or not same_path(configure[configure.index(flag) + 1], expected):
            raise ValueError("Configure source or build root differs")
    validate_configuration(root, build, evidence, inputs, by_name, configure, options, build_source)
    build_args = by_name["build-release"].get("arguments", [])
    if (not isinstance(build_args, list) or len(build_args) != 8 or build_args[0] != "--build" or
            not same_path(build_args[1], build / "build") or
            build_args[2:7] != ["--config", "Release", "--target", "ifcopenshell_wrapper", "--parallel"] or
            not isinstance(build_args[7], str) or not re.fullmatch(r"[0-9]+", build_args[7]) or
            not 1 <= int(build_args[7]) <= 32):
        raise ValueError("Expected bound Release wrapper build command")
    outputs = evidence.get("outputs", [])
    if not isinstance(outputs, list) or len(outputs) != 1:
        raise ValueError("Expected exactly one bound build output")
    extension = absolute_path(outputs[0]["path"])
    release = build / "build/ifcwrap/Release"
    if extension.parent != release or extension.name != "_ifcopenshell_wrapper.cp313-win_amd64.pyd":
        raise ValueError("Expected CPython 3.13 AMD64 Release extension")
    no_links(release)
    children = list(release.iterdir())
    if len(children) > MAX_FILES:
        raise ValueError("Release directory entry bound")
    for child in children:
        no_links(child)
    if [p for p in children if p.suffix.casefold() == ".pyd"] != [extension]:
        raise ValueError("Release directory must contain exactly one extension")
    verify_file(extension, outputs[0])
    validate_pe(extension)
    wrapper = build / "build/ifcwrap/ifcopenshell_wrapper.py"
    wrapper_record = digest(wrapper)
    wrapper_bound = "generated_wrapper" in evidence
    if wrapper_bound:
        original_wrapper = evidence["generated_wrapper"]
        if (not isinstance(original_wrapper, dict) or set(original_wrapper) != {"path", "bytes", "sha256"} or
                not same_path(original_wrapper["path"], wrapper) or
                checked_record(original_wrapper) != wrapper_record):
            raise ValueError("Generated wrapper differs from the exact original build receipt")
        verify_file(wrapper, original_wrapper)
    table = source_table(root / SOURCE, preparation["source"]["files"])
    plan = []

    def add(destination: str, path: pathlib.Path, record: dict, origin: dict) -> None:
        relative_path(destination)
        if len(str(output / destination)) > 240:
            raise ValueError("Candidate member exceeds Windows path bound")
        plan.append((destination, path, checked_record(record), origin))

    for name, item in sorted(table.items()):
        if not name.startswith(PACKAGE + "/"):
            continue
        relative = relative_path(name[len(PACKAGE) + 1:])
        if any(part.casefold().startswith(".git") for part in relative.parts):
            continue
        if (any(part.casefold() in FORBIDDEN_PARTS for part in relative.parts) or
                relative.suffix.casefold() in FORBIDDEN_SUFFIXES or relative.name.casefold() == "ifcopenshell_wrapper.py"):
            raise ValueError(f"Package source contains cache, native or generated artifact: {relative}")
        owner = next((s for s in submodules if name.startswith(s["path"] + "/")), lock["source"])
        owner_relative = name[len(owner["path"]) + 1:] if owner is not lock["source"] else name
        add("ifcopenshell/" + relative.as_posix(), root / SOURCE / name, item,
            {"kind": "locked-source", "repository": owner["repository"], "revision": owner["revision"], "path": owner_relative})
    names = {item[0] for item in plan}
    if not {"ifcopenshell/__init__.py", "ifcopenshell/mvd/__init__.py", "ifcopenshell/mvd/LICENSE", "ifcopenshell/simple_spf/__init__.py", "ifcopenshell/simple_spf/LICENSE"}.issubset(names):
        raise ValueError("Incomplete selected Python package or submodule licenses")
    for name in ("COPYING", "COPYING.LESSER", "VERSION"):
        if name not in table:
            raise ValueError("Required upstream legal/version source file missing")
        add("licenses/ifcopenshell/" + name, root / SOURCE / name, table[name],
            {"kind": "locked-source", **lock["source"], "path": name})
    add("ifcopenshell/" + extension.name, extension, outputs[0], {"kind": "bound-build-output", "path": str(extension)})
    add("ifcopenshell/ifcopenshell_wrapper.py", wrapper, wrapper_record,
        {"kind": "generated-build-output", "path": str(wrapper), "bound_in_original_build_evidence": wrapper_bound})
    for name, path, item in (("source-lock.json", lock_path, inputs[lock_path]),
                             ("source-preparation.json", preparation_path, inputs[preparation_path]),
                             ("build-recipe.ps1", effective_recipe, inputs[recipe_path]),
                             ("build-evidence.json", evidence_path, evidence_record)):
        add("provenance/" + name, path, item, {"kind": "local-evidence", "path": str(path)})
    for name, path in zip(("source-derivation.json", "source-derivation-verifier.py",
                           "opaque-coordinate-output.i"), derivation_inputs):
        add("provenance/" + name, path, inputs[path], {"kind": "local-evidence", "path": str(path)})
    wrapper_notice = ("The generated wrapper is bound by the original build evidence and was reverified at staging.\n"
                      if wrapper_bound else
                      "The generated wrapper was hashed at staging, not in build evidence.\n")
    notice = ("Unqualified local IFC Python candidate; no wheel or PyPI archive provenance is claimed.\n"
              "Contains locked upstream Python package source, its selected mvd/simple_spf submodules,\n"
              "the generated wrapper and the bound Release extension. Original licenses are preserved.\n"
              "C++ corresponding source and dependency license closure are NOT delivered or qualified\n"
              "by this stage. Native load, dependency closure, functionality and source closure require\n"
              "separate verification. " + wrapper_notice)
    notice_bytes = notice.encode("utf-8")
    result = {"schema_version": 1, "purpose": "isolated-source-built-python-import-candidate",
              "build_qualified": False, "source_closure_qualified": False, "product_runtime_replaced": False,
              "cpp_corresponding_source_delivered": False, "dependency_license_closure_delivered": False,
              "platform": "win_amd64", "python_abi": "cp313", "configuration": "Release",
              "schemas": SCHEMAS.split(";"), "source": lock["source"], "submodules": submodules,
              "build_evidence_sha256": evidence_record["sha256"], "files": []}
    if derivation_inputs:
        result["source_derivation_sha256"] = inputs[derivation_inputs[0]]["sha256"]
    if len(plan) + 1 > MAX_FILES or sum(item[2]["bytes"] for item in plan) + len(notice_bytes) > MAX_TOTAL_BYTES:
        raise ValueError("Candidate exceeds file/size bounds")
    # Validate all input bytes and names before reserving a destination. Rehash
    # copied bytes too: a concurrent source change fails without overwriting any
    # foreign file. On write failure the partial candidate is preserved for review.
    for _, path, item, _ in plan:
        verify_file(path, item)
    no_links(output)
    validate_output(output, [root, build, *inputs])
    output.mkdir()  # Exclusive: never adopt an existing empty or foreign directory.
    for name, path, item, origin in plan:
        target = output.joinpath(*relative_path(name).parts)
        no_links(target)
        target.parent.mkdir(parents=True, exist_ok=True)
        hasher = hashlib.sha256()
        size = 0
        no_links(path)
        with path.open("rb") as source_stream, target.open("xb") as target_stream:
            while chunk := source_stream.read(1 << 20):
                size += len(chunk)
                if size > item["bytes"]:
                    raise ValueError("Input grew during candidate copy; partial candidate preserved")
                hasher.update(chunk)
                target_stream.write(chunk)
        if {"bytes": size, "sha256": hasher.hexdigest()} != item:
            raise ValueError("Input changed during candidate copy; partial candidate preserved")
        result["files"].append({"path": name, **item, "origin": origin})
    with (output / "NOTICES.txt").open("xb") as stream:
        stream.write(notice_bytes)
    result["files"].append({"path": "NOTICES.txt", "bytes": len(notice_bytes),
                            "sha256": hashlib.sha256(notice_bytes).hexdigest(),
                            "origin": {"kind": "generated-stage-notice"}})
    result["files"].sort(key=lambda item: item["path"])
    if derivation_inputs:
        validate_build_source(root, build, evidence, inputs, by_name)
    verify_file(wrapper, wrapper_record)
    validate_inventory(output, result["files"])
    with (output / "candidate-manifest.json").open("x", encoding="utf-8", newline="\n") as stream:
        stream.write(json.dumps(result, indent=2) + "\n")
    return result


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build-evidence", type=pathlib.Path, required=True)
    parser.add_argument("--output", type=pathlib.Path, required=True)
    parser.add_argument("--recipe-snapshot", type=pathlib.Path,
                        help="build_root/build-recipe.ps1 matching the original recipe input hash")
    args = parser.parse_args()
    try:
        result = prepare(ROOT, args.build_evidence, args.output, recipe_snapshot=args.recipe_snapshot)
    except (ValueError, OSError, KeyError, TypeError, IndexError) as error:
        parser.exit(1, f"IFC candidate staging refused: {error}\n")
    print(json.dumps({"output": str(args.output), "files": len(result["files"]),
                      "build_qualified": False, "source_closure_qualified": False}))


if __name__ == "__main__":
    main()
