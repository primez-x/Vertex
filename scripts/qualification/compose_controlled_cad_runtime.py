"""Compose an unqualified Windows CAD runtime from exact local byte receipts.

No import of CAD/native libraries, extraction, execution, download, installation,
or qualification occurs. Publication requires Windows' no-replace directory
rename. Original private build receipts stay at their input location; portable
derived receipts bind their exact bytes without disclosing machine paths.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import os
from pathlib import Path, PurePosixPath
import re
import shutil
import stat
import tempfile
import unicodedata
from urllib.parse import urlsplit

MANIFEST = "controlled-runtime-manifest.json"
OUTPUT_RELATIVE = ".deps/cad-runtime/3.13.15-ifc-source-ff3c5b849eee"
BASE_RELATIVE = ".deps/cad-runtime/3.13.15"
LOCK_RELATIVE = "third_party/cad-runtime-lock.json"
SOURCE_REPOSITORY = "https://github.com/IfcOpenShell/IfcOpenShell.git"
SOURCE_REVISION = "ff3c5b849eee2ef6343b537c885b971ae6bba452"
PACKAGE_SOURCE = "src/ifcopenshell-python/ifcopenshell"
SCHEMAS = ["2x3", "4", "4x1", "4x2", "4x3", "4x3_tc1", "4x3_add1", "4x3_add2"]
SUBMODULES = [
    ("src/svgfill", "https://github.com/IfcOpenShell/svgfill", "29fbc17edec61b4f774ba8e87d4308983a75fc90"),
    (PACKAGE_SOURCE + "/mvd", "https://github.com/opensourceBIM/python-mvdxml/", "83c12fa494b9d6a5a370d8525dc1f76aa5c89d8d"),
    (PACKAGE_SOURCE + "/simple_spf", "https://github.com/IfcOpenShell/step-file-parser", "2849a31788c4f82edca7d1b1046d0606fdf8b9be")]
EXTENSION_NAME = "_ifcopenshell_wrapper.cp313-win_amd64.pyd"
EXTENSION_SHA256 = "710c14599b1243d7a032668f6cc21796af2399c72b2a515f75e325d7a6f58cdf"
WRAPPER_SHA256 = "f9ed4c81bd97868960110e280993a75765ab5ec36c5e5db937e5b458e24add48"
EXPECTED_CANDIDATE_FILES = 563
MAX_FILES = 40_000
MAX_ENTRIES = MAX_FILES * 4
MAX_FILE_BYTES = 512_000_000
MAX_TOTAL_BYTES = 4_000_000_000
MAX_JSON_BYTES = 16_000_000
MAX_INPUT_BYTES = 2_000_000_000
MAX_INPUT_TOTAL = 16_000_000_000
CANDIDATE_FLAGS = ("build_qualified", "source_closure_qualified", "product_runtime_replaced",
                   "cpp_corresponding_source_delivered", "dependency_license_closure_delivered")
RESULT_FLAGS = CANDIDATE_FLAGS + ("runtime_qualified", "production_qualified", "production_worker_integrated",
    "distribution_qualified", "corresponding_source_qualified", "licensing_clearance",
    "dependency_license_closure_qualified", "source_qualified")
PROVENANCE_NAMES = ("build-evidence.json", "build-recipe.ps1", "opaque-coordinate-output.i",
    "source-derivation-verifier.py", "source-derivation.json", "source-lock.json", "source-preparation.json")
BUILD_COMMAND_NAMES = {"source-check", "source-derive", "python-version", "swig-version", "vcpkg-head",
    "vcpkg-origin", "vcpkg-status", "visual-studio", "cmake-version", "configure", "build-release"}
WORKSPACE_ROOTS = {
    "workspace": "", "python": BASE_RELATIVE,
    "occt_sdk": ".deps/ifc-kernel/x64-windows-ifc-static",
    "support_sdk": ".deps/ifc-support/x64-windows-ifc-static",
    "occt_store": ".deps/ifc-kernel", "support_store": ".deps/ifc-support",
    "swig": ".deps/ifc-tools/swigwin-4.3.1"}
CMAKE_TOOL_PATH = "Common7/IDE/CommonExtensions/Microsoft/CMake/CMake/bin/cmake.exe"
INPUT_LAYOUT = [
    ("workspace", "scripts/build-ifc-source.ps1", "build-recipe"),
    ("workspace", "third_party/ifc-source-lock.json", "source-lock"),
    ("workspace", "scripts/prepare_ifc_source.py", "source-verifier"),
    ("workspace", ".deps/ifc-source-preparation.json", "source-preparation"),
    ("workspace", "third_party/ifc-source/vcpkg.json", "occt-dependency-manifest"),
    ("workspace", "third_party/ifc-source/support/vcpkg.json", "support-dependency-manifest"),
    ("workspace", "third_party/ifc-source/triplets/x64-windows-ifc-static.cmake", "static-sdk-triplet"),
    ("python", "python.exe", "cpython-interpreter"),
    ("python", "libs/python313.lib", "cpython-import-library"),
    ("python", "include/Python.h", "cpython-development-header"),
    ("python", "include/patchlevel.h", "cpython-version-header"),
    ("python", "runtime-manifest.json", "cpython-runtime-receipt"),
    ("swig", "swig.exe", "wrapper-generator"),
    ("workspace", "scripts/prepare_ifc_derived_source.py", "source-derivation-verifier"),
    ("workspace", "third_party/ifc-source/patches/opaque-coordinate-output.i", "source-patch"),
    ("build", "source-derivation.json", "source-derivation-receipt"),
    ("git_tool", "git.exe", "source-manager-tool"),
    ("occt_store", "vcpkg/status", "occt-package-status"),
    ("support_store", "vcpkg/status", "support-package-status"),
    ("occt_sdk", "include/opencascade/Standard_Version.hxx", "occt-version-header"),
    ("support_sdk", "include/boost/version.hpp", "boost-version-header"),
    ("support_sdk", "include/eigen3/Eigen/src/Core/util/Macros.h", "eigen-version-header"),
    *(("occt_sdk", "lib/" + name + ".lib", "occt-static-library") for name in
      ("TKernel", "TKMath", "TKBRep", "TKGeomBase", "TKGeomAlgo", "TKG3d", "TKG2d", "TKShHealing",
       "TKTopAlgo", "TKMesh", "TKPrim", "TKBool", "TKBO", "TKFillet", "TKXSBase", "TKOffset",
       "TKHLR", "TKBin", "TKDESTEP", "TKDEIGES")),
    *(("support_sdk", "lib/boost_" + name + "-vc143-mt-x64-1_86.lib", "boost-static-library") for name in
      ("atomic", "chrono", "container", "date_time", "graph", "locale", "program_options", "random", "regex",
       "serialization", "system", "thread", "wserialization")),
    ("visual_studio", CMAKE_TOOL_PATH, "cmake-tool"),
    ("build", "build/CMakeCache.txt", "configured-cmake-cache")]
CURRENT_WORKSPACE_ROLES = {"build-recipe", "source-lock", "source-verifier", "source-preparation",
    "occt-dependency-manifest", "support-dependency-manifest", "static-sdk-triplet",
    "source-derivation-verifier", "source-patch"}
SCALAR_CMAKE_OPTIONS = {
    "CMAKE_CONFIGURATION_TYPES": "Release", "CMAKE_MSVC_RUNTIME_LIBRARY": "MultiThreadedDLL",
    "CMAKE_POLICY_DEFAULT_CMP0091": "NEW", "BUILD_IFCGEOM": "ON", "BUILD_IFCPYTHON": "ON",
    "WITH_OPENCASCADE": "ON", "SCHEMA_VERSIONS": ";".join(SCHEMAS), "OCCT_STATIC": "OFF",
    "Boost_NO_SYSTEM_PATHS": "ON", "Boost_NO_BOOST_CMAKE": "OFF", "Boost_USE_STATIC_LIBS": "ON",
    "Boost_USE_STATIC_RUNTIME": "OFF", "CMAKE_FIND_USE_PACKAGE_REGISTRY": "OFF",
    "CMAKE_FIND_USE_SYSTEM_PACKAGE_REGISTRY": "OFF", "CMAKE_FIND_USE_CMAKE_ENVIRONMENT_PATH": "OFF",
    "CCACHE_FOUND": "OFF", **{name: "OFF" for name in
        ("MINIMAL_BUILD", "BUILD_SHARED_LIBS", "BUILD_CONVERT", "BUILD_GEOMSERVER", "BUILD_EXAMPLES",
         "BUILD_DOCUMENTATION", "BUILD_IFCMAX", "BUILD_QTVIEWER", "BUILD_PACKAGE", "WITH_CGAL", "COLLADA_SUPPORT",
         "GLTF_SUPPORT", "HDF5_SUPPORT", "WITH_PROJ", "IFCXML_SUPPORT", "USD_SUPPORT", "CITYJSON_SUPPORT",
         "WITH_RELATIONSHIP_VALIDATION", "USE_MMAP", "USE_VLD", "WASM_BUILD", "ADD_COMMIT_SHA",
         "MSVC_PARALLEL_BUILD", "ENABLE_BUILD_OPTIMIZATIONS")}}
PATH_CMAKE_OPTIONS = {
    "CMAKE_GENERATOR_INSTANCE": [("visual_studio", "")],
    "OCC_INCLUDE_DIR": [("occt_sdk", "include/opencascade")], "OCC_LIBRARY_DIR": [("occt_sdk", "lib")],
    "BOOST_ROOT": [("support_sdk", "")], "BOOST_LIBRARYDIR": [("support_sdk", "lib")],
    "Boost_INCLUDE_DIR": [("support_sdk", "include")], "Boost_DIR": [("support_sdk", "share/boost")],
    "EIGEN_DIR": [("support_sdk", "include/eigen3")], "PYTHON_EXECUTABLE": [("python", "python.exe")],
    "PYTHON_INCLUDE_DIR": [("python", "include")], "PYTHON_LIBRARY": [("python", "libs/python313.lib")],
    "SWIG_EXECUTABLE": [("swig", "swig.exe")], "SWIG_DIR": [("swig", "Lib")],
    "CMAKE_PREFIX_PATH": [("occt_sdk", ""), ("support_sdk", "")],
    "CMAKE_INSTALL_PREFIX": [("build", "uninstalled")], "PYTHON_MODULE_INSTALL_DIR": [("build", "uninstalled/python")]}
RESERVED = {"con", "prn", "aux", "nul", "conin$", "conout$",
            *(f"com{i}" for i in "123456789¹²³"), *(f"lpt{i}" for i in "123456789¹²³")}
CACHE_PARTS = {"__pycache__", ".cache", ".pytest_cache", ".mypy_cache", ".ruff_cache", "node_modules"}


def relative_path(name: str) -> PurePosixPath:
    if (not isinstance(name, str) or not name or len(name) > 220 or "\\" in name or
            unicodedata.normalize("NFC", name) != name or
            any(unicodedata.category(c) == "Cc" for c in name)):
        raise ValueError("Unsafe or non-normalized relative path")
    parts = name.split("/")
    if any(p in {"", ".", ".."} or p.endswith((" ", ".")) or
           any(c in p for c in ':*?"<>|') or p.split(".", 1)[0].casefold() in RESERVED or
           p.casefold() == ".git" or p.casefold() in CACHE_PARTS for p in parts):
        raise ValueError("Unsafe Windows relative member")
    if PurePosixPath(name).suffix.casefold() in {".pyc", ".pyo"}:
        raise ValueError("Cached bytecode is forbidden")
    return PurePosixPath(name)


def no_links(path: Path) -> None:
    for item in (path, *path.parents):
        try:
            info = item.lstat()
        except FileNotFoundError:
            continue
        if stat.S_ISLNK(info.st_mode) or getattr(info, "st_file_attributes", 0) & 0x400:
            raise ValueError("Path crosses a link or reparse point")


def local_path(value: str | Path, workspace: Path | None = None) -> Path:
    text = str(value)
    if (text.startswith(("\\\\", "//")) or ";" in text or
            any(unicodedata.category(c) == "Cc" for c in text)):
        raise ValueError("Expected a local path without control characters")
    # Check raw components before Path can normalize away a traversal.
    tail = text.replace("\\", "/")
    if re.match(r"^[A-Za-z]:/", tail):
        tail = tail[3:]
    else:
        tail = tail.lstrip("/")
    relative_path(tail)
    path = Path(value)
    if not path.is_absolute():
        if workspace is None:
            raise ValueError("Workspace must be an absolute local directory")
        path = workspace / path
    no_links(path)
    return path.resolve()


def checked_record(value: dict, *, limit: int = MAX_FILE_BYTES) -> dict:
    if (not isinstance(value, dict) or type(value.get("bytes")) is not int or
            not 0 <= value["bytes"] <= limit or not isinstance(value.get("sha256"), str) or
            not re.fullmatch(r"[0-9a-f]{64}", value["sha256"])):
        raise ValueError("Invalid bounded byte receipt")
    return {"bytes": value["bytes"], "sha256": value["sha256"]}


def digest(path: Path, *, limit: int = MAX_FILE_BYTES) -> dict:
    no_links(path)
    before = path.stat()
    if (not stat.S_ISREG(before.st_mode) or before.st_size > limit or
            getattr(before, "st_nlink", 1) != 1):
        raise ValueError("Missing, linked, nonregular or oversized file")
    hasher, size = hashlib.sha256(), 0
    with path.open("rb") as stream:
        opened = os.fstat(stream.fileno())
        if (opened.st_dev, opened.st_ino) != (before.st_dev, before.st_ino):
            raise ValueError("File changed before hashing")
        while chunk := stream.read(1 << 20):
            size += len(chunk)
            if size > limit:
                raise ValueError("File grew beyond bound")
            hasher.update(chunk)
        after = os.fstat(stream.fileno())
    no_links(path)
    current = path.stat()
    if ((before.st_dev, before.st_ino, before.st_size, before.st_mtime_ns) !=
            (after.st_dev, after.st_ino, after.st_size, after.st_mtime_ns) or
            (current.st_dev, current.st_ino, current.st_size, current.st_mtime_ns) !=
            (before.st_dev, before.st_ino, before.st_size, before.st_mtime_ns) or size != before.st_size):
        raise ValueError("File changed during hashing")
    return {"bytes": size, "sha256": hasher.hexdigest()}


def verify_file(path: Path, receipt: dict) -> None:
    if digest(path) != checked_record(receipt):
        raise ValueError("Current bytes differ from receipt: " + path.name)


def unique_object(pairs: list) -> dict:
    result = {}
    for key, value in pairs:
        if key in result:
            raise ValueError("Duplicate JSON key")
        result[key] = value
    return result


def read_json(path: Path) -> tuple[dict, dict]:
    receipt = digest(path, limit=MAX_JSON_BYTES)
    with path.open("rb") as stream:
        data = stream.read(MAX_JSON_BYTES + 1)
    if {"bytes": len(data), "sha256": hashlib.sha256(data).hexdigest()} != receipt:
        raise ValueError("JSON bytes changed during read")
    value = json.loads(data.decode("utf-8-sig"), object_pairs_hook=unique_object,
                       parse_constant=lambda value: (_ for _ in ()).throw(ValueError("Nonfinite JSON")))
    if not isinstance(value, dict):
        raise ValueError("Receipt must be a JSON object")
    return value, receipt


def file_table(records: list, *, extra_keys: set | None = None) -> dict:
    if not isinstance(records, list) or not 0 < len(records) <= MAX_FILES:
        raise ValueError("Missing or oversized exact file table")
    table, spellings, total = {}, {}, 0
    allowed = {"path", "sha256", "bytes"} | (extra_keys or set())
    for item in records:
        if not isinstance(item, dict) or set(item) != allowed:
            raise ValueError("Unsupported file record schema")
        name = item["path"]
        relative = relative_path(name)
        if name in table:
            raise ValueError("Duplicate file record")
        for index in range(1, len(relative.parts) + 1):
            spelling = "/".join(relative.parts[:index])
            entry = (spelling, index < len(relative.parts))
            previous = spellings.get(spelling.casefold())
            if previous is not None and previous != entry:
                raise ValueError("Case or file/directory collision")
            spellings[spelling.casefold()] = entry
        total += checked_record(item)["bytes"]
        if total > MAX_TOTAL_BYTES:
            raise ValueError("File table exceeds aggregate byte bound")
        table[name] = item
    if list(table) != sorted(table):
        raise ValueError("File table must be normalized and sorted")
    return table


def verify_tree(root: Path, table: dict, *, auxiliary: dict | None = None) -> None:
    if set(table).intersection(auxiliary or {}):
        raise ValueError("Receipt cannot include its own auxiliary manifest")
    expected = {**table, **(auxiliary or {})}
    # Also validates remapped output/auxiliary names and their collisions.
    file_table(sorted(({"path": name, **checked_record(r)} for name, r in expected.items()),
                      key=lambda r: r["path"]))
    expected_dirs = {parent.as_posix() for name in expected for parent in relative_path(name).parents
                     if parent.as_posix() != "."}
    actual_files, actual_dirs, count = set(), set(), 0
    no_links(root)
    if not root.is_dir():
        raise ValueError("Missing inventory root")
    for directory, dirs, files in os.walk(root, followlinks=False):
        count += len(dirs) + len(files)
        if count > MAX_ENTRIES:
            raise ValueError("Inventory exceeds entry bound")
        for name in (*dirs, *files):
            path = Path(directory) / name
            no_links(path)
            relative_path(path.relative_to(root).as_posix())
        actual_dirs.update((Path(directory) / name).relative_to(root).as_posix() for name in dirs)
        for name in files:
            path = Path(directory) / name
            member = path.relative_to(root).as_posix()
            if member not in expected:
                raise ValueError("Inventory contains unreviewed file: " + member)
            verify_file(path, expected[member])
            actual_files.add(member)
    if actual_files != expected.keys() or actual_dirs != expected_dirs:
        raise ValueError("Inventory differs from exact files/directories")


def validate_lock(lock: dict) -> None:
    if (lock.get("schema_version") != 1 or lock.get("platform") != "win_amd64" or
            lock.get("python_version") != "3.13.15" or lock.get("python_abi") != "cp313"):
        raise ValueError("Unsupported baseline CPython lock")
    assets = lock.get("assets")
    if not isinstance(assets, list) or not 1 <= len(assets) <= 64:
        raise ValueError("Missing or excessive locked assets")
    names, wheels, kinds = set(), {}, []
    for asset in assets:
        if not isinstance(asset, dict):
            raise ValueError("Invalid locked asset")
        name = relative_path(asset["filename"])
        if len(name.parts) != 1 or name.name.casefold() in names:
            raise ValueError("Duplicate or nested locked asset")
        names.add(name.name.casefold())
        if not isinstance(asset.get("sha256"), str) or not re.fullmatch(r"[0-9a-f]{64}", asset["sha256"]):
            raise ValueError("Invalid locked asset digest")
        url = urlsplit(asset["url"])
        if (url.scheme != "https" or url.hostname not in
                {"www.python.org", "files.pythonhosted.org", "www.gnu.org", "api.nuget.org"} or
                url.username or url.password):
            raise ValueError("Invalid locked publisher")
        kind = asset["kind"]
        if kind not in {"interpreter", "headers", "development", "wheel", "notice"}:
            raise ValueError("Unsupported locked asset kind")
        kinds.append(kind)
        if kind in {"interpreter", "headers", "development"} and asset["version"] != "3.13.15":
            raise ValueError("Interpreter/development version differs")
        if kind == "wheel":
            if (not isinstance(asset.get("name"), str) or not re.fullmatch(r"[a-z][a-z0-9_-]{0,63}", asset["name"]) or
                    not isinstance(asset.get("version"), str) or
                    not re.fullmatch(r"[0-9][A-Za-z0-9.+_-]{0,63}", asset["version"]) or asset["name"] in wheels):
                raise ValueError("Duplicate wheel identity")
            wheels[asset["name"]] = asset["version"]
    if (any(kinds.count(k) != 1 for k in ("interpreter", "headers", "development")) or
            wheels != lock.get("library_versions") or len(wheels) != 11 or
            wheels.get("ifcopenshell") != "0.8.3.post2" or wheels.get("ezdxf") != "1.4.3"):
        raise ValueError("Baseline lock does not bind the exact interpreter and 11 wheels")


def require_false(value: dict, fields: tuple) -> None:
    if type(value.get("schema_version")) is not int or value["schema_version"] != 1 or any(
            value.get(field) is not False for field in fields):
        raise ValueError("Receipt must retain schema 1 and explicitly unqualified flags")


def private_identity(value: str) -> str:
    """Compare receipt-selected paths, never follow them or emit them."""
    if (not isinstance(value, str) or not re.match(r"^[A-Za-z]:[/\\]", value) or
            any(unicodedata.category(c) == "Cc" for c in value)):
        raise ValueError("Invalid private evidence path")
    tail = value.replace("\\", "/")
    # No traversal/alternate-stream aliases in evidence comparisons.
    relative_path(tail[3:])
    return tail.casefold()


def input_table(records: list) -> dict:
    if not isinstance(records, list) or not 0 < len(records) <= MAX_FILES:
        raise ValueError("Missing bounded build input receipts")
    result, total = {}, 0
    for record in records:
        if not isinstance(record, dict) or set(record) != {"path", "bytes", "sha256"}:
            raise ValueError("Invalid build input schema")
        key = private_identity(record["path"])
        if key in result:
            raise ValueError("Duplicate private build input")
        result[key] = checked_record(record, limit=MAX_INPUT_BYTES)
        total += result[key]["bytes"]
        if total > MAX_INPUT_TOTAL:
            raise ValueError("Build inputs exceed aggregate bound")
    return result


def cmake_tokens(arguments: list) -> tuple[dict, dict]:
    """Parse only the controlled generator invocation; normalize -D aliases."""
    if (not isinstance(arguments, list) or not 1 <= len(arguments) <= 256 or any(
            not isinstance(a, str) or not a or len(a) > 4096 or
            any(unicodedata.category(c) == "Cc" for c in a) for a in arguments)):
        raise ValueError("Invalid bounded CMake arguments")
    flags, options, seen = {}, {}, set()
    canonical = {name.casefold(): name for name in (*SCALAR_CMAKE_OPTIONS, *PATH_CMAKE_OPTIONS)}
    index = 0
    while index < len(arguments):
        argument = arguments[index]
        index += 1
        if argument in {"-S", "-B", "-G", "-A"}:
            if argument in flags or index == len(arguments):
                raise ValueError("Duplicate or incomplete CMake generator flag")
            flags[argument] = arguments[index]
            index += 1
            continue
        if argument == "-D":
            if index == len(arguments):
                raise ValueError("Incomplete split CMake variable")
            variable = arguments[index]
            index += 1
        elif argument.startswith("-D"):
            variable = argument[2:]
        else:
            raise ValueError("Unbound CMake switch/preset/cache/toolchain option")
        match = re.fullmatch(r"([A-Za-z_][A-Za-z0-9_]*)(?::([A-Za-z]+))?=(.*)", variable)
        if not match or match[1].casefold() in seen:
            raise ValueError("Invalid or duplicate normalized CMake variable")
        key = match[1].casefold()
        seen.add(key)
        if key not in canonical:
            raise ValueError("Unsupported CMake variable")
        name = canonical[key]
        if match[1] != name:
            raise ValueError("Case-sensitive CMake control uses a noncanonical name")
        kind = (match[2] or "").upper()
        if kind and kind not in {"BOOL", "STRING", "PATH", "FILEPATH"}:
            raise ValueError("Unsupported CMake cache type")
        if kind and ((name in PATH_CMAKE_OPTIONS and kind not in {"PATH", "FILEPATH", "STRING"}) or
                     (name in SCALAR_CMAKE_OPTIONS and kind not in {"BOOL", "STRING"}) or
                     (name in SCALAR_CMAKE_OPTIONS and SCALAR_CMAKE_OPTIONS[name] not in {"ON", "OFF"} and kind != "STRING")):
            raise ValueError("CMake type disagrees with controlled value")
        options[name] = match[3]
    if set(flags) != {"-S", "-B", "-G", "-A"} or set(options) != set(canonical.values()):
        raise ValueError("Incomplete controlled CMake configuration")
    return flags, options


def portable_build_contract(build: dict, derivation: dict, inputs: dict) -> dict:
    """Assign exact receipt paths to documented portable roots, never scrub text."""
    workspace = private_identity(build.get("workspace"))
    build_root = private_identity(build.get("build_root"))
    if (private_identity(derivation.get("workspace")) != workspace or
            private_identity(derivation.get("source_path")) != workspace + "/.deps/ifc-src" or
            private_identity(derivation.get("output_path")) != build_root + "/source"):
        raise ValueError("Historical source/workspace/build roots disagree")
    flags, options = cmake_tokens(build["configure_arguments"])
    roots = {name: workspace + ("/" + suffix if suffix else "") for name, suffix in WORKSPACE_ROOTS.items()}
    roots.update(build=build_root, derived_source=build_root + "/source", upstream_source=workspace + "/.deps/ifc-src",
                 visual_studio=private_identity(options["CMAKE_GENERATOR_INSTANCE"]))
    # Optional explicit SDK roots are supported only when the recipe receipt
    # declares them; arbitrary configure argument paths cannot create a root.
    if "sdk_roots" in build:
        selected = build["sdk_roots"]
        if not isinstance(selected, dict) or set(selected) != {"kernel", "support"}:
            raise ValueError("Unsupported selected SDK root schema")
        for field, root_name, store_name in (("kernel", "occt_sdk", "occt_store"),
                                            ("support", "support_sdk", "support_store")):
            roots[root_name] = private_identity(selected[field])
            roots[store_name] = roots[root_name].rsplit("/", 1)[0]
    commands = {command["name"]: command for command in build["commands"]}
    git = private_identity(commands.get("vcpkg-head", {}).get("executable"))
    if not git.endswith("/git.exe"):
        raise ValueError("Unsupported source-manager executable identity")
    roots["git_tool"] = git.rsplit("/", 1)[0]
    if (flags["-G"] != "Visual Studio 17 2022" or flags["-A"] != "x64" or
            private_identity(flags["-S"]) != roots["derived_source"] + "/cmake" or
            private_identity(flags["-B"]) != roots["build"] + "/build"):
        raise ValueError("CMake generator or source/build mapping differs")
    portable = ["-S", "${derived_source}/cmake", "-B", "${build}/build", "-G", flags["-G"], "-A", flags["-A"]]
    for name, expected in SCALAR_CMAKE_OPTIONS.items():
        if options[name] != expected:
            raise ValueError("Controlled CMake scalar/build/schema option differs")
        portable.append("-D" + name + "=" + expected)
    for name, components in PATH_CMAKE_OPTIONS.items():
        values = options[name].split(";")
        expected = [roots[root] + ("/" + path.casefold() if path else "") for root, path in components]
        if len(values) != len(expected) or [private_identity(v) for v in values] != expected:
            raise ValueError("CMake component/path mapping differs: " + name)
        portable.append("-D" + name + "=" + ";".join("${" + root + "}" + ("/" + path if path else "")
                                                      for root, path in components))
    cmake = roots["visual_studio"] + "/" + CMAKE_TOOL_PATH.casefold()
    if (private_identity(commands["configure"].get("executable")) != cmake or
            commands["configure"].get("arguments") != build["configure_arguments"] or
            private_identity(commands["build-release"].get("executable")) != cmake):
        raise ValueError("Configure/build command executable or arguments differ")
    release = commands["build-release"].get("arguments")
    if (not isinstance(release, list) or len(release) != 8 or release[0] != "--build" or
            private_identity(release[1]) != roots["build"] + "/build" or
            release[2:7] != ["--config", "Release", "--target", "ifcopenshell_wrapper", "--parallel"] or
            not isinstance(release[7], str) or not re.fullmatch(r"[0-9]+", release[7]) or not 1 <= int(release[7]) <= 32):
        raise ValueError("Unsupported Release wrapper command")
    derive = commands["source-derive"]
    arguments = derive.get("arguments")
    if (private_identity(derive.get("executable")) != roots["python"] + "/python.exe" or
            not isinstance(arguments, list) or len(arguments) != 9 or
            [arguments[i] for i in (0, 1, 3, 5, 7)] != ["-I", "-B", "--workspace", "--output", "--manifest"] or
            [private_identity(arguments[i]) for i in (2, 4, 6, 8)] != [workspace + "/scripts/prepare_ifc_derived_source.py",
                workspace, roots["derived_source"], roots["build"] + "/source-derivation.json"]):
        raise ValueError("Unsupported source derivation command")
    expected_inputs, portable_inputs, bindings = {}, [], []
    for root_name, path, role in INPUT_LAYOUT:
        identity = roots[root_name] + "/" + path.casefold()
        if identity not in inputs:
            raise ValueError("Historical build lacks a required named input: " + role)
        expected_inputs[identity] = inputs[identity]
        portable_inputs.append({"root": root_name, "path": path, "role": role, **inputs[identity]})
        if role in CURRENT_WORKSPACE_ROLES or root_name == "python":
            relative = (WORKSPACE_ROOTS[root_name] + "/" if WORKSPACE_ROOTS[root_name] else "") + path
            bindings.append({"path": relative, "role": role, **inputs[identity]})
    if inputs.keys() != expected_inputs.keys() or len(portable_inputs) != 57:
        raise ValueError("Unknown or overlapping historical build input identity")
    recipe_arguments = ["-NoProfile", "-File", "${workspace}/scripts/build-ifc-source.ps1",
                        "-BuildRoot", "${build}", "-Parallel", release[7],
                        "-CMakeExecutable", "${visual_studio}/" + CMAKE_TOOL_PATH]
    if "sdk_roots" in build:
        recipe_arguments += ["-KernelRoot", "${occt_sdk}", "-SupportRoot", "${support_sdk}"]
    return {"inputs": sorted(portable_inputs, key=lambda r: (r["root"], r["path"])),
            "current_workspace_bindings": sorted(bindings, key=lambda r: r["path"]),
            "configure_arguments": portable,
            "recipe_arguments": recipe_arguments,
            "build_arguments": ["--build", "${build}/build", *release[2:]],
            "derivation_arguments": ["-I", "-B", "${workspace}/scripts/prepare_ifc_derived_source.py",
                "--workspace", "${workspace}", "--output", "${derived_source}",
                "--manifest", "${build}/source-derivation.json"],
            "roots": {name: {"kind": "consumer-selected-local-root", "absolute_input_path_shipped": False}
                      for name in sorted(roots)}}


def verify_current_bindings(workspace: Path, bindings: list) -> None:
    for receipt in bindings:
        verify_file(workspace / relative_path(receipt["path"]), receipt)


def directory_identity(path: Path) -> tuple:
    no_links(path)
    info = path.lstat()
    if not stat.S_ISDIR(info.st_mode) or not info.st_ino:
        raise ValueError("Private stage requires a regular directory identity")
    return info.st_dev, info.st_ino


def verify_directory_identity(path: Path, expected: tuple) -> None:
    if directory_identity(path) != expected:
        raise ValueError("Private stage was substituted; foreign directory preserved")


def validate_candidate(candidate: Path, value: dict, table: dict) -> dict:
    keys = {"schema_version", "purpose", "platform", "python_abi", "configuration", "schemas", "source",
            "submodules", "build_evidence_sha256", "source_derivation_sha256", "files", *CANDIDATE_FLAGS}
    require_false(value, CANDIDATE_FLAGS)
    if (set(value) != keys or len(table) != EXPECTED_CANDIDATE_FILES or
            value["purpose"] != "isolated-source-built-python-import-candidate" or
            value["platform"] != "win_amd64" or value["python_abi"] != "cp313" or
            value["configuration"] != "Release" or value["schemas"] != SCHEMAS or
            value["source"] != {"path": ".deps/ifc-src", "repository": SOURCE_REPOSITORY, "revision": SOURCE_REVISION}):
        raise ValueError("Unsupported candidate identity/schema")
    submodules = value["submodules"]
    if (not isinstance(submodules, list) or len(submodules) != len(SUBMODULES) or any(
            not isinstance(s, dict) or set(s) != {"path", "repository", "revision", "purpose"} or
            (s["path"], s["repository"], s["revision"]) != expected or
            not isinstance(s["purpose"], str) or len(s["purpose"]) > 512
            for s, expected in zip(submodules, SUBMODULES))):
        raise ValueError("Candidate submodule identity differs")
    required = {"NOTICES.txt", "ifcopenshell/__init__.py", "ifcopenshell/" + EXTENSION_NAME,
                "ifcopenshell/ifcopenshell_wrapper.py", "ifcopenshell/mvd/LICENSE", "ifcopenshell/simple_spf/LICENSE",
                "ifcopenshell/mvd/__init__.py", "ifcopenshell/simple_spf/__init__.py",
                *("licenses/ifcopenshell/" + name for name in ("COPYING", "COPYING.LESSER", "VERSION")),
                *("provenance/" + name for name in PROVENANCE_NAMES)}
    if not required.issubset(table):
        raise ValueError("Candidate lacks required source, notices or provenance")
    evidence = {}
    for name in ("source-lock.json", "source-preparation.json", "source-derivation.json", "build-evidence.json"):
        evidence[name], receipt = read_json(candidate / "provenance" / name)
        if receipt != checked_record(table["provenance/" + name]):
            raise ValueError("Evidence changed after candidate inventory")
    lock, prep, derivation, build = (evidence[n] for n in
        ("source-lock.json", "source-preparation.json", "source-derivation.json", "build-evidence.json"))
    for receipt in (lock, prep, derivation, build):
        require_false(receipt, ("build_qualified", "source_closure_qualified"))
    require_false(derivation, ("product_runtime_replaced",))
    require_false(build, ("product_runtime_replaced",))
    if (lock.get("source") != value["source"] or lock.get("platform") != "win_amd64" or
            lock.get("submodules") != submodules or
            prep.get("lock_sha256") != table["provenance/source-lock.json"]["sha256"] or
            not isinstance(prep.get("source"), dict) or
            {k: prep["source"].get(k) for k in value["source"]} != value["source"] or
            prep.get("submodules") != [{k: s[k] for k in ("path", "repository", "revision")} for s in submodules] or
            derivation.get("source_revision") != SOURCE_REVISION or
            derivation.get("source_repository") != SOURCE_REPOSITORY or
            value["source_derivation_sha256"] != table["provenance/source-derivation.json"]["sha256"] or
            value["build_evidence_sha256"] != table["provenance/build-evidence.json"]["sha256"]):
        raise ValueError("Source/preparation/derivation receipt identity disagrees")
    source_files = file_table(prep["source"]["files"])
    derived_files = file_table(derivation["files"])
    if source_files.keys() != derived_files.keys():
        raise ValueError("Derived source membership differs")
    modified = derivation.get("modified_file")
    differences = {name for name in source_files if checked_record(source_files[name]) != checked_record(derived_files[name])}
    if (not isinstance(modified, dict) or set(modified) != {"path", "original_bytes", "original_sha256",
                "derived_bytes", "derived_sha256"} or differences != {"src/ifcwrap/utils/typemaps_out.i"} or
                modified["path"] != "src/ifcwrap/utils/typemaps_out.i" or
                checked_record(source_files[modified["path"]]) !=
                {"bytes": modified["original_bytes"], "sha256": modified["original_sha256"]} or
                checked_record(derived_files[modified["path"]]) !=
                {"bytes": modified["derived_bytes"], "sha256": modified["derived_sha256"]}):
        raise ValueError("Unsupported derived source delta")
    inputs = input_table(build.get("inputs"))
    for name in PROVENANCE_NAMES:
        if name == "build-evidence.json":
            continue
        item = table["provenance/" + name]
        if inputs.get(private_identity(item["origin"].get("path"))) != checked_record(item):
            raise ValueError("Build receipt does not bind staged provenance bytes")
    derivation_inputs = derivation.get("inputs")
    if not isinstance(derivation_inputs, dict) or set(derivation_inputs) != {"lock", "preparation", "patch", "script"}:
        raise ValueError("Unsupported derivation inputs")
    for label, name in (("lock", "source-lock.json"), ("preparation", "source-preparation.json"),
                        ("patch", "opaque-coordinate-output.i"), ("script", "source-derivation-verifier.py")):
        staged = table["provenance/" + name]
        bound = derivation_inputs[label]
        if (not isinstance(bound, dict) or set(bound) != {"path", "bytes", "sha256"} or
                checked_record(bound) != checked_record(staged) or
                private_identity(bound["path"]) != private_identity(staged["origin"].get("path"))):
            raise ValueError("Derivation does not bind staged source/recipe bytes")
    build_root = private_identity(build.get("build_root"))
    if (build.get("state") != "built-unqualified" or build.get("configuration") != "Release" or
            private_identity(build.get("source_derivation")) != build_root + "/source-derivation.json" or
            private_identity(derivation.get("manifest_path")) != build_root + "/source-derivation.json"):
        raise ValueError("Build/derivation location or state disagrees")
    commands = build.get("commands")
    if not isinstance(commands, list) or not 1 <= len(commands) <= 64:
        raise ValueError("Missing bounded build command receipt")
    names = set()
    for command in commands:
        if (not isinstance(command, dict) or not isinstance(command.get("name"), str) or
                command["name"] not in BUILD_COMMAND_NAMES or
                command["name"] in names or type(command.get("exit_code")) is not int or command["exit_code"] != 0):
            raise ValueError("Duplicate or unsuccessful build command")
        names.add(command["name"])
    if not {"configure", "build-release", "source-derive"}.issubset(names):
        raise ValueError("Required successful build commands absent")
    arguments = build.get("configure_arguments")
    if (not isinstance(arguments, list) or len(arguments) > 256 or any(
            not isinstance(a, str) or len(a) > 4096 for a in arguments)):
        raise ValueError("Build schema or IFC configuration differs")
    contract = portable_build_contract(build, derivation, inputs)
    native = table["ifcopenshell/" + EXTENSION_NAME]
    wrapper = table["ifcopenshell/ifcopenshell_wrapper.py"]
    outputs = build.get("outputs")
    if (not isinstance(outputs, list) or len(outputs) != 1 or
            not isinstance(outputs[0], dict) or set(outputs[0]) != {"path", "bytes", "sha256"} or
            checked_record(outputs[0]) != checked_record(native) or native["sha256"] != EXTENSION_SHA256 or
            wrapper["sha256"] != WRAPPER_SHA256 or
            private_identity(outputs[0]["path"]) != build_root + "/build/ifcwrap/release/" + EXTENSION_NAME.casefold()):
        raise ValueError("Stale or unbound native/generated wrapper bytes")
    wrapper_bound = "generated_wrapper" in build
    original_wrapper_receipt = None
    if wrapper_bound:
        original_wrapper = build["generated_wrapper"]
        if (not isinstance(original_wrapper, dict) or set(original_wrapper) != {"path", "bytes", "sha256"} or
                private_identity(original_wrapper["path"]) != build_root + "/build/ifcwrap/ifcopenshell_wrapper.py" or
                checked_record(original_wrapper) != checked_record(wrapper)):
            raise ValueError("Generated wrapper differs from original bound build receipt")
        original_wrapper_receipt = {"root": "build", "path": "build/ifcwrap/ifcopenshell_wrapper.py",
                                    **checked_record(original_wrapper)}
    for name, item in table.items():
        origin = item["origin"]
        if not isinstance(origin, dict):
            raise ValueError("Invalid candidate origin")
        if name == "ifcopenshell/" + EXTENSION_NAME:
            if (set(origin) != {"kind", "path"} or origin["kind"] != "bound-build-output" or
                    private_identity(origin["path"]) != private_identity(outputs[0]["path"])):
                raise ValueError("Native origin disagrees with build")
        elif name == "ifcopenshell/ifcopenshell_wrapper.py":
            if (set(origin) != {"kind", "path", "bound_in_original_build_evidence"} or
                    origin["kind"] != "generated-build-output" or
                    origin["bound_in_original_build_evidence"] is not wrapper_bound or
                    private_identity(origin["path"]) != build_root + "/build/ifcwrap/ifcopenshell_wrapper.py"):
                raise ValueError("Generated wrapper receipt limitation/location differs")
        elif name == "NOTICES.txt":
            if origin != {"kind": "generated-stage-notice"}:
                raise ValueError("Unsupported notice origin")
        elif name.startswith("provenance/"):
            if (name[11:] not in PROVENANCE_NAMES or set(origin) != {"kind", "path"} or
                    origin["kind"] != "local-evidence"):
                raise ValueError("Unknown candidate provenance")
            private_identity(origin["path"])
        else:
            if (set(origin) != {"kind", "repository", "revision", "path"} or origin["kind"] != "locked-source"):
                raise ValueError("Unsupported source origin")
            if name.startswith("ifcopenshell/"):
                source_name = PACKAGE_SOURCE + "/" + name[len("ifcopenshell/"):]
            elif name in {"licenses/ifcopenshell/" + n for n in ("COPYING", "COPYING.LESSER", "VERSION")}:
                source_name = name.rsplit("/", 1)[1]
            else:
                raise ValueError("Arbitrary candidate extra refused")
            owner = next((s for s in submodules if source_name.startswith(s["path"] + "/")), value["source"])
            owner_name = source_name[len(owner["path"]) + 1:] if owner is not value["source"] else source_name
            if (origin != {"kind": "locked-source", "repository": owner["repository"],
                           "revision": owner["revision"], "path": owner_name} or
                    source_name not in source_files or checked_record(item) != checked_record(source_files[source_name]) or
                    Path(name).suffix.casefold() in {".pyd", ".dll", ".exe", ".so", ".obj", ".lib", ".pdb", ".a"}):
                raise ValueError("Candidate source origin/hash differs from preparation")
    init_path = candidate / "ifcopenshell/__init__.py"
    if table["ifcopenshell/__init__.py"]["bytes"] > MAX_JSON_BYTES:
        raise ValueError("Oversized Python version source")
    text = init_path.read_text(encoding="utf-8-sig")
    if len(re.findall(r'^__version__\s*=\s*version\s*=\s*[\'"]0\.0\.0[\'"]\s*$', text, re.MULTILINE)) != 1:
        raise ValueError("Upstream informational version changed; no relabel permitted")
    expected_source_members = {"ifcopenshell/" + name[len(PACKAGE_SOURCE) + 1:]
        for name in source_files if name.startswith(PACKAGE_SOURCE + "/") and
        not any(p.casefold().startswith(".git") for p in relative_path(name[len(PACKAGE_SOURCE) + 1:]).parts)}
    actual_source_members = {name for name in table if name.startswith("ifcopenshell/")}
    if actual_source_members != expected_source_members | {
            "ifcopenshell/" + EXTENSION_NAME, "ifcopenshell/ifcopenshell_wrapper.py"}:
        raise ValueError("Candidate omits or adds prepared package source")
    portable_modules = [{k: s[k] for k in ("path", "repository", "revision")} for s in submodules]
    return {"source": value["source"], "submodules": portable_modules, "schemas": SCHEMAS,
            "modified_file": modified, "commands": sorted(names),
            "source_files": list(source_files.values()), "derived_source_files": list(derived_files.values()),
            "build_contract": contract,
            "generated_wrapper_bound": wrapper_bound,
            "original_build_generated_wrapper_receipt": original_wrapper_receipt,
            "original_build_evidence_receipt": checked_record(table["provenance/build-evidence.json"]),
            "configuration": "Release", "build_input_count": len(inputs),
            "all_build_input_receipts": sorted(inputs.values(), key=lambda r: (r["sha256"], r["bytes"])),
            "configure_arguments_receipt": {"sha256": hashlib.sha256(json_bytes({"arguments": arguments})).hexdigest(),
                                            "bytes": len(json_bytes({"arguments": arguments})),
                                            "encoding": "canonical-json-object-arguments"}}


def copy_verified(source: Path, destination: Path, evidence: dict) -> None:
    no_links(source)
    verify_file(source, evidence)
    no_links(destination)
    destination.parent.mkdir(parents=True, exist_ok=True)
    hasher, size = hashlib.sha256(), 0
    with source.open("rb") as stream, destination.open("xb") as output:
        if os.fstat(stream.fileno()).st_nlink != 1:
            raise ValueError("Copy input is hard-linked")
        while chunk := stream.read(1 << 20):
            size += len(chunk)
            if size > evidence["bytes"]:
                raise ValueError("Input grew during copy")
            hasher.update(chunk)
            output.write(chunk)
    if {"bytes": size, "sha256": hasher.hexdigest()} != checked_record(evidence):
        raise ValueError("Input drifted during copy")
    verify_file(destination, evidence)


def json_bytes(value: dict) -> bytes:
    return (json.dumps(value, indent=2, sort_keys=True, ensure_ascii=True, allow_nan=False) + "\n").encode("utf-8")


def compose(workspace: str | Path, baseline_runtime: str | Path,
            candidate_package: str | Path, output_root: str | Path) -> dict:
    root = local_path(workspace)
    baseline, candidate, output = (local_path(p, root) for p in (baseline_runtime, candidate_package, output_root))
    if (not root.is_dir() or baseline != root / BASE_RELATIVE or output != root / OUTPUT_RELATIVE or
            not output.parent.is_dir() or output.exists() or candidate.is_relative_to(output) or
            output.is_relative_to(candidate) or baseline.is_relative_to(candidate) or candidate.is_relative_to(baseline)):
        raise ValueError("Expected exact baseline and fresh separate controlled runtime sibling")
    if os.name != "nt":
        raise ValueError("Atomic no-replace directory publication requires Windows")
    baseline_manifest, baseline_receipt = read_json(baseline / "runtime-manifest.json")
    lock, lock_receipt = read_json(root / LOCK_RELATIVE)
    validate_lock(lock)
    if (set(baseline_manifest) != {"schema_version", "lock_sha256", "python_version", "library_versions",
            "qualification", "production_worker_integrated", "files"} or
            type(baseline_manifest["schema_version"]) is not int or baseline_manifest["schema_version"] != 1 or
            baseline_manifest["lock_sha256"] != lock_receipt["sha256"] or
            baseline_manifest["python_version"] != "3.13.15" or
            baseline_manifest["library_versions"] != lock["library_versions"] or
            baseline_manifest["qualification"] != "incomplete" or baseline_manifest["production_worker_integrated"] is not False):
        raise ValueError("Baseline manifest differs from supported lock state")
    baseline_table = file_table(baseline_manifest["files"])
    verify_tree(baseline, baseline_table, auxiliary={"runtime-manifest.json": baseline_receipt})
    candidate_manifest, candidate_receipt = read_json(candidate / "candidate-manifest.json")
    candidate_table = file_table(candidate_manifest.get("files"), extra_keys={"origin"})
    verify_tree(candidate, candidate_table, auxiliary={"candidate-manifest.json": candidate_receipt})
    info = validate_candidate(candidate, candidate_manifest, candidate_table)
    contract = info["build_contract"]
    verify_current_bindings(root, contract["current_workspace_bindings"])
    old_package = "Lib/site-packages/ifcopenshell/"
    old_identity = "Lib/site-packages/ifcopenshell-0.8.3.post2.dist-info/"
    if not any(p.startswith(old_package) for p in baseline_table) or not any(p.startswith(old_identity) for p in baseline_table):
        raise ValueError("Baseline lacks the expected replaced wheel identity")
    # Refuse alternate identities before removal; no disguised stale IFC module.
    for name in baseline_table:
        parts = relative_path(name).parts
        if len(parts) >= 3 and parts[:2] == ("Lib", "site-packages"):
            first = parts[2].casefold()
            if first.startswith("ifcopenshell") and parts[2] not in {"ifcopenshell", "ifcopenshell-0.8.3.post2.dist-info"}:
                raise ValueError("Unknown baseline IFC identity")
    plan, generated = {}, {}
    def add(name: str, source: Path, receipt: dict, role: str) -> None:
        relative_path(name)
        if name.casefold() == MANIFEST.casefold() or name in plan or name in generated:
            raise ValueError("Output collision")
        plan[name] = (source, checked_record(receipt), role)
    def derive(name: str, original: dict, payload: dict) -> dict:
        path = "receipts/" + name
        if path in plan or path in generated:
            raise ValueError("Derived receipt collides with retained runtime payload")
        value = {"schema_version": 1, "kind": "portable_controlled_cad_receipt",
                 "original_receipt": checked_record(original), "original_retained_privately": True,
                 "qualification_conferred": False, **payload}
        data = json_bytes(value)
        if len(data) > MAX_JSON_BYTES:
            raise ValueError("Derived receipt exceeds bound")
        generated[path] = data
        return {"path": path, "bytes": len(data), "sha256": hashlib.sha256(data).hexdigest(),
                "original_receipt": checked_record(original)}
    for name, receipt in baseline_table.items():
        if not name.startswith((old_package, old_identity)):
            add(name, baseline / name, receipt, "verified-baseline")
    for name, receipt in candidate_table.items():
        if name.startswith("ifcopenshell/"):
            add("Lib/site-packages/" + name, candidate / name, receipt, "source-built-ifcopenshell")
        elif name.startswith("licenses/") or name == "NOTICES.txt":
            add("notices/ifcopenshell/" + name, candidate / name, receipt, "verbatim-source-notice")
    artifacts = []
    for restore, output_name in (
            ("scripts/build-ifc-source.ps1", "receipts/recipes/build-ifc-source.ps1"),
            ("scripts/prepare_ifc_source.py", "receipts/recipes/prepare_ifc_source.py"),
            ("scripts/prepare_ifc_derived_source.py", "receipts/recipes/prepare_ifc_derived_source.py"),
            ("third_party/ifc-source/patches/opaque-coordinate-output.i", "receipts/patches/opaque-coordinate-output.i"),
            ("third_party/ifc-source/vcpkg.json", "receipts/recipes/occt-vcpkg.json"),
            ("third_party/ifc-source/support/vcpkg.json", "receipts/recipes/support-vcpkg.json"),
            ("third_party/ifc-source/triplets/x64-windows-ifc-static.cmake", "receipts/recipes/x64-windows-ifc-static.cmake"),
            ("third_party/ifc-source-lock.json", "receipts/recipes/ifc-source-lock.json")):
        receipt = next(r for r in contract["current_workspace_bindings"] if r["path"] == restore)
        add(output_name, root / restore, receipt, "verbatim-bound-build-recipe")
        artifacts.append({"path": output_name, "restore_root": "workspace", "restore_path": restore,
                          "role": receipt["role"], **checked_record(receipt)})
    portable_inputs = [{"name": name, **checked_record(candidate_table["provenance/" + name])}
                       for name in PROVENANCE_NAMES]
    receipts = {
        "baseline": derive("baseline-runtime.json", baseline_receipt,
            {"python_version": "3.13.15", "library_versions": lock["library_versions"],
             "lock_receipt": lock_receipt, "baseline_file_count": len(baseline_table),
             "removed_subtrees": [old_package.rstrip("/"), old_identity.rstrip("/")]}),
        "candidate": derive("candidate-package.json", candidate_receipt,
            {"source": info["source"], "submodules": info["submodules"], "schemas": SCHEMAS,
             "file_count": len(candidate_table), "original_files": [{"path": n, **checked_record(r)}
                 for n, r in candidate_table.items()]}),
        "build": derive("build.json", candidate_table["provenance/build-evidence.json"],
            {"configuration": "Release", "schemas": SCHEMAS, "successful_commands": info["commands"],
             "bound_provenance_inputs": portable_inputs, "original_build_input_count": info["build_input_count"],
             "all_original_build_input_receipts": info["all_build_input_receipts"],
             "configure_arguments_receipt": info["configure_arguments_receipt"],
             "inputs": contract["inputs"], "roots": contract["roots"],
             "current_workspace_bindings": contract["current_workspace_bindings"],
             "configure_arguments": contract["configure_arguments"], "build_arguments": contract["build_arguments"],
             "derivation_arguments": contract["derivation_arguments"], "recipe_arguments": contract["recipe_arguments"],
             "recipe_artifacts": artifacts,
             "generated_wrapper_bound_in_original_build_evidence": info["generated_wrapper_bound"],
             "original_build_generated_wrapper_receipt": info["original_build_generated_wrapper_receipt"],
             "native_output": {"path": "Lib/site-packages/ifcopenshell/" + EXTENSION_NAME,
                               **checked_record(candidate_table["ifcopenshell/" + EXTENSION_NAME])}}),
        "derivation": derive("source-derivation.json", candidate_table["provenance/source-derivation.json"],
            {"source": info["source"], "modified_file": info["modified_file"],
             "derived_source_files": info["derived_source_files"], "submodules": info["submodules"],
             "inputs": [r for r in portable_inputs if r["name"] in {"source-lock.json", "source-preparation.json",
                                        "opaque-coordinate-output.i", "source-derivation-verifier.py"}]}),
        "generated_wrapper": derive("generated-wrapper.json", candidate_table["ifcopenshell/ifcopenshell_wrapper.py"],
            {"path": "Lib/site-packages/ifcopenshell/ifcopenshell_wrapper.py",
             "bound_in_original_build_evidence": info["generated_wrapper_bound"],
             "original_build_generated_wrapper_receipt": info["original_build_generated_wrapper_receipt"],
             "original_build_evidence_receipt": info["original_build_evidence_receipt"],
             "bound_by_candidate_staging_receipt": True})}
    # One explicit portable derivative per remaining provenance file; never copy
    # arbitrary original JSON, recipe text, or origin-selected machine paths.
    for name in PROVENANCE_NAMES:
        if name not in {"build-evidence.json", "source-derivation.json"}:
            receipts[name] = derive(name + ".receipt.json", candidate_table["provenance/" + name],
                {"evidence_name": name, "source": info["source"],
                 **({"source_files": info["source_files"], "submodules": info["submodules"]}
                    if name == "source-preparation.json" else {}),
                 "content_role": "original_provenance_hash_only", "source_closure_qualified": False})
    result_files = [{"path": n, **r, "origin": role} for n, (_, r, role) in plan.items()]
    result_files.extend({"path": n, "bytes": len(data), "sha256": hashlib.sha256(data).hexdigest(),
                         "origin": "portable-derived-receipt"} for n, data in generated.items())
    result_files.sort(key=lambda r: r["path"])
    result_table = file_table(result_files, extra_keys={"origin"})
    result = {"schema_version": 1, "kind": "controlled_cad_runtime", "platform": "win_amd64",
        "python_version": "3.13.15", "python_abi": "cp313", "ezdxf_version": "1.4.3",
        "library_versions": {k: v for k, v in lock["library_versions"].items() if k != "ifcopenshell"},
        "ifcopenshell": {"source": info["source"], "submodules": info["submodules"], "schemas": SCHEMAS,
            "upstream_version_informative_only": "0.0.0", "wheel_identity_claimed": False,
            "extension": {"path": "Lib/site-packages/ifcopenshell/" + EXTENSION_NAME,
                          **checked_record(candidate_table["ifcopenshell/" + EXTENSION_NAME])},
            "generated_wrapper_bound_in_original_build_evidence": info["generated_wrapper_bound"]},
        "qualification": "incomplete", **{field: False for field in RESULT_FLAGS},
        "receipts": receipts, "files": result_files}
    manifest_data = json_bytes(result)
    if len(manifest_data) > MAX_JSON_BYTES:
        raise ValueError("Controlled runtime manifest exceeds bound")
    manifest_receipt = {"bytes": len(manifest_data), "sha256": hashlib.sha256(manifest_data).hexdigest()}
    # Verify every mapped path before reserving the private sibling.
    no_links(output)
    if output.exists():
        raise ValueError("Output appeared before staging")
    stage = Path(tempfile.mkdtemp(prefix=".controlled-cad-stage-", dir=output.parent))
    stage_identity = directory_identity(stage)
    try:
        for name, (source, receipt, _) in sorted(plan.items()):
            if len(str(stage / name)) > 240:
                raise ValueError("Output exceeds Windows path bound")
            copy_verified(source, stage / name, receipt)
        for name, data in sorted(generated.items()):
            path = stage / name
            path.parent.mkdir(parents=True, exist_ok=True)
            with path.open("xb") as stream:
                stream.write(data)
        with (stage / MANIFEST).open("xb") as stream:
            stream.write(manifest_data)
        # The manifest is excluded from its own recursive file table, but its
        # bytes and exact membership are checked separately before publication.
        verify_tree(stage, result_table, auxiliary={MANIFEST: manifest_receipt})
        verify_file(root / LOCK_RELATIVE, lock_receipt)
        verify_tree(baseline, baseline_table, auxiliary={"runtime-manifest.json": baseline_receipt})
        verify_tree(candidate, candidate_table, auxiliary={"candidate-manifest.json": candidate_receipt})
        verify_current_bindings(root, contract["current_workspace_bindings"])
        no_links(output)
        no_links(stage)
        if output.exists() or stage.parent.resolve() != output.parent.resolve():
            raise ValueError("Publication target changed; prior target preserved")
        # Repeat output checks after input rehashes, closing the staging window.
        verify_tree(stage, result_table, auxiliary={MANIFEST: manifest_receipt})
        verify_directory_identity(stage, stage_identity)
        os.rename(stage, output)  # Windows refuses an existing target, even empty.
        return result
    finally:
        if stage.exists():
            verify_directory_identity(stage, stage_identity)
            if stage.resolve().parent != output.parent.resolve() or not stage.name.startswith(".controlled-cad-stage-"):
                raise ValueError("Unsafe private stage cleanup target")
            shutil.rmtree(stage)


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ("workspace", "baseline-runtime", "candidate-package", "output-root"):
        parser.add_argument("--" + name, required=True, type=Path)
    args = parser.parse_args()
    try:
        result = compose(args.workspace, args.baseline_runtime, args.candidate_package, args.output_root)
    except (ValueError, OSError, KeyError, TypeError, IndexError, UnicodeError, RecursionError) as error:
        parser.exit(1, "Controlled CAD runtime composition refused: " + str(error) + "\n")
    print(json.dumps({"kind": result["kind"], "files": len(result["files"]), "runtime_qualified": False,
                      "product_runtime_replaced": False, "licensing_clearance": False}))


if __name__ == "__main__":
    main()
