"""Offline, separate Boost.Thread 1.86.0 rebuild from the retained exact kit.

Use a fresh short generic local --output, never an SDK/package/build directory.
--prepare-only executes no native tool. Build mode requires the root caller to
freeze the kit, SDK, tools and this script for the whole operation and acknowledge
that contract with --inputs-quiescent. Inventories detect ordinary changes; they
are not a snapshot guarantee against arbitrary concurrent writers. No install,
download, cache reuse, binary rewriting or production qualification is performed.
Failures retain owned partial output and a failed receipt, never a success claim.
"""
from __future__ import annotations

import argparse
import ctypes
import gzip
import hashlib
import io
import json
import os
import pathlib
import re
import stat
import subprocess
import tarfile
import threading
import time
import xml.etree.ElementTree as ET

MANIFEST = "ifc-sdk-source-manifest.json"
MANIFEST_SHA256 = "7fc047eb929d3b034769698465628fc1927d262c69828a98ab103bcbb179786a"
THREAD_RECIPE = "recipes/4859fd5b6e005b5129c0f552de4017ec6f76546b"
HELPER_RECIPE = "recipes/aca8ab6ec76d120c4482a629de23cad5ad7e4643"
ARCHIVE_SHA512 = "0c10698176e695011b70aea5b0f427bb4265032349297fd6d71cb8b4d82ff4144ce8b5a4bc7ed9587485e51e46764820d3d4688a1e576dd8ec375140bce1f708"
ARCHIVE = f"archives/sha512/{ARCHIVE_SHA512}.archive"
ARCHIVE_SHA256 = "ce2fc152afea10e8badd934343f40ed7c5e38d987aaa4709f3f43756ab9fc4c0"
ARCHIVE_ROOT = "thread-boost-1.86.0"
SDK_PINS = {
    "share/boost/cmake-build/BoostRoot.cmake": "62aaa6f90dbd1a34bf073cfc6c8cb4045c6a440deebbe82bcaceb76fd3928dbe",
    "share/boost/cmake-build/BoostInstall.cmake": "cb2e2f65b4f80e986dc18a8882546c78f4cdf04ebd7dc62fe047f4b4a75badd9",
    "lib/boost_thread-vc143-mt-x64-1_86.lib": "2b7ff73950acb2ff78f720c13d14b0ffefa855ca08d6e97c4a50090bcb20e3a6",
}
LIBRARY = "build/stage/lib/Release/boost_thread-vc143-mt-x64-1_86.lib"
SOURCES = {"src/win32/thread.cpp", "src/win32/tss_dll.cpp", "src/win32/tss_pe.cpp",
           "src/win32/thread_primitives.cpp", "src/future.cpp"}
COMPILER_VERSION = "19.44.35228.0"
THREAD_HEADER_COUNT = 167
MAX_FILE = 512 * 1024 * 1024
MAX_TOTAL = 6 * 1024 * 1024 * 1024
MAX_FILES = 25_000
MAX_ENTRIES = 100_000
MAX_JSON = 16 * 1024 * 1024
MAX_ARCHIVE = 8 * 1024 * 1024
MAX_EXTRACT = 64 * 1024 * 1024
MAX_TAR = 80 * 1024 * 1024
MAX_LOG = 64 * 1024 * 1024
RESERVED = {"con", "prn", "aux", "nul", "conin$", "conout$",
            *(f"com{i}" for i in "123456789¹²³"), *(f"lpt{i}" for i in "123456789¹²³")}


def no_links(path):
    for candidate in (path, *path.parents):
        try:
            info = candidate.lstat()
        except FileNotFoundError:
            continue
        if stat.S_ISLNK(info.st_mode) or getattr(info, "st_file_attributes", 0) & 0x400:
            raise ValueError(f"Link/reparse path refused: {candidate}")


def relative_path(name):
    if not isinstance(name, str) or not name or len(name) > 1024 or "\\" in name:
        raise ValueError("Unsafe relative path")
    name.encode("utf-8", "strict")
    parts = name.split("/")
    if len(parts) > 64 or any(not p or p in {".", ".."} or len(p) > 255 or
                            p.endswith((" ", ".")) or p.casefold() == ".git" or
                            p.split(".", 1)[0].casefold() in RESERVED or
                            any(ord(c) < 32 or c in ':*?"<>|' for c in p) for p in parts):
        raise ValueError(f"Unsafe relative path: {name}")
    return name


def check_names(entries):
    """Validate files and explicit directories including implicit ancestors."""
    seen, explicit = {}, set()
    for name, is_directory in entries:
        parts = relative_path(name).split("/")
        if name.casefold() in explicit:
            raise ValueError("Duplicate/case-colliding path")
        explicit.add(name.casefold())
        for index in range(1, len(parts) + 1):
            spelling = "/".join(parts[:index])
            kind = is_directory or index < len(parts)
            key = spelling.casefold()
            if key in seen and seen[key] != (spelling, kind):
                raise ValueError("Ancestor/case-colliding path")
            seen[key] = (spelling, kind)


def stamp(info):
    # Windows lstat/fstat differ in synthesized executable bits and ctime.
    return (info.st_dev, info.st_ino, stat.S_IFMT(info.st_mode), info.st_size,
            info.st_mtime_ns, info.st_nlink)


def consume(path, limit=MAX_FILE, retain=False):
    no_links(path)
    before = path.lstat()
    if not stat.S_ISREG(before.st_mode) or before.st_nlink != 1 or before.st_size > limit:
        raise ValueError(f"Nonregular, hardlinked or oversized file: {path}")
    sha = hashlib.sha256()
    chunks, count = [], 0
    with path.open("rb") as stream:
        if stamp(os.fstat(stream.fileno())) != stamp(before):
            raise ValueError(f"Input changed before read: {path}")
        while data := stream.read(1024 * 1024):
            count += len(data)
            if count > limit:
                raise ValueError(f"Input size limit: {path}")
            sha.update(data)
            if retain:
                chunks.append(data)
        after = os.fstat(stream.fileno())
    no_links(path)
    if stamp(after) != stamp(before) or stamp(path.lstat()) != stamp(before) or count != before.st_size:
        raise ValueError(f"Input changed while reading: {path}")
    return {"bytes": count, "sha256": sha.hexdigest()}, b"".join(chunks)


def file_record(path, name):
    record, _ = consume(path)
    return {"path": relative_path(name), **record}


def inventory(root):
    no_links(root)
    if not root.is_dir():
        raise ValueError(f"Missing input directory: {root}")
    entries, files, total = [], [], 0
    pending = [root]
    while pending:
        directory = pending.pop()
        no_links(directory)
        for path in directory.iterdir():
            no_links(path)
            info = path.lstat()
            name = path.relative_to(root).as_posix()
            is_directory = stat.S_ISDIR(info.st_mode)
            entries.append((name, is_directory))
            if len(entries) > MAX_ENTRIES:
                raise ValueError("Inventory entry limit")
            if is_directory:
                pending.append(path)
            else:
                record = file_record(path, name)
                files.append(record)
                total += record["bytes"]
                if len(files) > MAX_FILES or total > MAX_TOTAL:
                    raise ValueError("Inventory file/byte limit")
    check_names(entries)
    return sorted(files, key=lambda item: item["path"])


def parse_json(data):
    def object_pairs(pairs):
        value = {}
        for key, item in pairs:
            if key in value:
                raise ValueError("Duplicate JSON key")
            value[key] = item
        return value
    value = json.loads(data, object_pairs_hook=object_pairs,
                       parse_constant=lambda _: (_ for _ in ()).throw(ValueError("Nonfinite JSON")))
    if not isinstance(value, dict):
        raise ValueError("Expected JSON object")
    return value


def extract_thread(data, output):
    """Preflight all tar members, then manually copy regular bytes; never extractall."""
    if len(data) > MAX_ARCHIVE:
        raise ValueError("Compressed archive size limit")
    no_links(output)
    if output.exists():
        raise ValueError("Archive output already exists")
    # Bound the whole tar, including PAX/GNU metadata, before tarfile parses it.
    with gzip.GzipFile(fileobj=io.BytesIO(data)) as compressed:
        expanded = compressed.read(MAX_TAR + 1)
    if len(expanded) > MAX_TAR:
        raise ValueError("Expanded tar/metadata byte limit")
    entries, members, total = [], [], 0
    with tarfile.open(fileobj=io.BytesIO(expanded), mode="r:") as archive:
        for member in archive:
            if len(members) >= 5000:
                raise ValueError("Archive entry limit")
            name = member.name
            if member.isdir() and name.endswith("/"):
                name = name[:-1]
            relative_path(name)
            if name != ARCHIVE_ROOT and not name.startswith(ARCHIVE_ROOT + "/"):
                raise ValueError("Archive has unexpected root")
            if not (member.isdir() or member.isreg()) or member.issparse():
                raise ValueError("Archive nonregular/link/sparse entry")
            if name == ARCHIVE_ROOT and not member.isdir():
                raise ValueError("Archive root is not a directory")
            if member.size < 0 or member.size > MAX_EXTRACT or (member.isdir() and member.size):
                raise ValueError("Archive member size limit")
            total += member.size
            if total > MAX_EXTRACT:
                raise ValueError("Expanded archive byte limit")
            entries.append((name, member.isdir()))
            members.append((member, name))
        check_names(entries)
        if not members:
            raise ValueError("Empty archive")
        output.mkdir(parents=True)
        for member, name in members:
            if name == ARCHIVE_ROOT:
                continue
            target = output / name[len(ARCHIVE_ROOT) + 1:]
            no_links(target)
            if member.isdir():
                target.mkdir(parents=True, exist_ok=True)
            else:
                target.parent.mkdir(parents=True, exist_ok=True)
                source = archive.extractfile(member)
                if source is None:
                    raise ValueError("Missing archive file stream")
                with source, target.open("xb") as stream:
                    remaining = member.size
                    while remaining:
                        chunk = source.read(min(1024 * 1024, remaining))
                        if not chunk:
                            raise ValueError("Truncated archive file")
                        stream.write(chunk)
                        remaining -= len(chunk)
    return inventory(output)


def local_absolute(path):
    path = pathlib.Path(path)
    if not path.is_absolute() or str(path).startswith(("\\\\", "//")):
        raise ValueError("Paths must be absolute local paths")
    no_links(path)
    relative_path("/".join(path.parts[1:]))
    if os.name == "nt":
        kernel = ctypes.WinDLL("kernel32", use_last_error=True)
        kernel.GetDriveTypeW.argtypes = [ctypes.c_wchar_p]
        kernel.GetDriveTypeW.restype = ctypes.c_uint
        if kernel.GetDriveTypeW(path.anchor) != 3:
            raise ValueError("Only fixed local drives are accepted")
    return path.resolve()


def overlaps(left, right):
    a, b = str(left).replace("\\", "/").casefold().rstrip("/"), str(right).replace("\\", "/").casefold().rstrip("/")
    return a == b or a.startswith(b + "/") or b.startswith(a + "/")


def generic_output(path):
    # This additional CLI policy prevents private profile/build paths in binaries.
    path = local_absolute(path)
    if len(str(path)) > 100 or not re.fullmatch(r"[A-Za-z0-9_./:\\-]+", str(path)):
        raise ValueError("Output must be a short (<=100 characters) generic ASCII path")
    private = {"users", "documents", "desktop", "onedrive", "appdata"}
    if any(part.casefold() in private for part in path.parts):
        raise ValueError("Output must be outside personal profile directories")
    return path


def source_inputs(kit):
    metadata, data = consume(kit / MANIFEST, MAX_JSON, retain=True)
    if metadata["sha256"] != MANIFEST_SHA256:
        raise ValueError("Exact source-kit manifest hash differs")
    manifest = parse_json(data)
    if manifest.get("schema_version") != 1 or not isinstance(manifest.get("files"), list):
        raise ValueError("Invalid source-kit manifest")
    table = manifest["files"]
    if not 0 < len(table) <= MAX_FILES:
        raise ValueError("Source-kit table limit")
    check_names([(item["path"], False) for item in table])
    total = 0
    for item in table:
        if (set(item) != {"path", "bytes", "sha256"} or type(item["bytes"]) is not int or
                not 0 <= item["bytes"] <= MAX_FILE or
                not isinstance(item["sha256"], str) or not re.fullmatch(r"[0-9a-f]{64}", item["sha256"])):
            raise ValueError("Invalid source-kit file binding")
        total += item["bytes"]
    if total > MAX_TOTAL or table != sorted(table, key=lambda item: item["path"]):
        raise ValueError("Source-kit noncanonical/oversized table")
    packages = manifest.get("packages", [])
    def package(name):
        matches = [p for p in packages if p.get("sdk") == "support" and p.get("name") == name]
        if len(matches) != 1:
            raise ValueError(f"Missing/ambiguous historical package: {name}")
        return matches[0]
    thread, helper = package("boost-thread"), package("vcpkg-boost")
    if (thread.get("version") != "1.86.0" or thread.get("architecture") != "x64-windows-ifc-static" or
            thread.get("recipe", {}).get("path") != THREAD_RECIPE or
            helper.get("recipe", {}).get("path") != HELPER_RECIPE or
            thread.get("resources") != [{"name": "boostorg/thread", "path": ARCHIVE,
                                         "sha512": ARCHIVE_SHA512,
                                         "url": "git+https://github.com/boostorg/thread@boost-1.86.0"}]):
        raise ValueError("Historical thread/helper/archive relationship differs")
    selected = [item for item in table if item["path"] == ARCHIVE or
                any(item["path"].startswith(prefix + "/") for prefix in (THREAD_RECIPE, HELPER_RECIPE)) or
                item["path"].startswith(("receipts/support/x64-windows-ifc-static/boost-thread/",
                                         "receipts/support/x64-windows-ifc-static/boost-cmake/",
                                         "receipts/support/x64-windows/vcpkg-boost/"))]
    required = {ARCHIVE, THREAD_RECIPE + "/portfile.cmake", THREAD_RECIPE + "/vcpkg.json",
                HELPER_RECIPE + "/boost-install.cmake"}
    if not required <= {item["path"] for item in selected}:
        raise ValueError("Missing exact recipe/archive input")
    copies = {MANIFEST: data}
    for item in selected:
        record, content = consume(kit / item["path"], MAX_ARCHIVE if item["path"] == ARCHIVE else MAX_JSON, retain=True)
        if record != {k: item[k] for k in ("bytes", "sha256")}:
            raise ValueError(f"Source-kit byte binding differs: {item['path']}")
        copies[item["path"]] = content
    if (hashlib.sha256(copies[ARCHIVE]).hexdigest() != ARCHIVE_SHA256 or
            hashlib.sha512(copies[ARCHIVE]).hexdigest() != ARCHIVE_SHA512):
        raise ValueError("Exact thread archive digest differs")
    # Refuse unexplained local recipe substitutions even when not consumed.
    for prefix in (THREAD_RECIPE, HELPER_RECIPE):
        expected = [{**item, "path": item["path"][len(prefix) + 1:]}
                    for item in selected if item["path"].startswith(prefix + "/")]
        if inventory(kit / prefix) != expected:
            raise ValueError("Historical recipe directory differs from manifest")
    records = [{"path": MANIFEST, **metadata}, *selected]
    return sorted(records, key=lambda item: item["path"]), copies


def commands(output, sdk, cmake, compiler, vs_instance):
    return [[str(cmake), "--version"], [str(compiler), "/Bv", "/?"],
            [str(cmake), "-S", str(output / "source"), "-B", str(output / "build"),
             "-G", "Visual Studio 17 2022", "-A", "x64",
             f"-DCMAKE_GENERATOR_INSTANCE={vs_instance.as_posix()}",
             "-DCMAKE_CONFIGURATION_TYPES=Release", "-DBUILD_SHARED_LIBS=OFF", "-DBUILD_TESTING=OFF",
             "-DBOOST_INCLUDE_LIBRARIES=thread", "-DBOOST_RUNTIME_LINK=dynamic",
             "-DBOOST_INSTALL_INCLUDE_SUBDIR=", "-DCMAKE_MSVC_RUNTIME_LIBRARY=MultiThreadedDLL",
             "-DCMAKE_MSVC_DEBUG_INFORMATION_FORMAT=",
             "-DCMAKE_CXX_FLAGS=/nologo /DWIN32 /D_WINDOWS /utf-8 /GR /EHsc /MP",
             "-DCMAKE_CXX_FLAGS_RELEASE=/MD /O2 /Oi /Gy /DNDEBUG /Z7",
             f"-DBoost_DIR={sdk.as_posix()}/share/boost", f"-DCMAKE_PREFIX_PATH={sdk.as_posix()}",
             "-DCMAKE_FIND_USE_PACKAGE_REGISTRY=OFF", "-DCMAKE_FIND_USE_SYSTEM_PACKAGE_REGISTRY=OFF",
             "-DFETCHCONTENT_FULLY_DISCONNECTED=ON", "-DFETCHCONTENT_UPDATES_DISCONNECTED=ON"],
            [str(cmake), "--build", str(output / "build"), "--config", "Release",
             "--target", "boost_thread", "--parallel", "4"]]


def command_environment(output):
    env = os.environ.copy()
    # Inherited flags can silently alter this supposedly exact recipe.
    for key in list(env):
        upper = key.upper()
        if upper in {"CL", "_CL_", "INCLUDE", "LIB", "LIBPATH", "CC", "CXX", "CFLAGS", "CXXFLAGS", "LDFLAGS"} or upper.startswith(("CMAKE_", "VCPKG_", "BOOST_")):
            del env[key]
    temp = output / "temp"
    temp.mkdir(exist_ok=True)
    env["TMP"] = env["TEMP"] = str(temp)
    return env


def run_command(argv, cwd, logs, index, env):
    """No-shell/no-window child with bounded independently drained byte streams."""
    logs.mkdir(exist_ok=True)
    paths = [logs / f"{index:02d}.{name}.log" for name in ("stdout", "stderr")]
    record = {"argv": argv, "cwd": str(cwd), "exit_code": None, "streams": [], "error": None}
    started = time.monotonic()
    failures, overflow = [], threading.Event()
    try:
        process = subprocess.Popen(argv, cwd=cwd, env=env, stdin=subprocess.DEVNULL,
                                   stdout=subprocess.PIPE, stderr=subprocess.PIPE, shell=False,
                                   creationflags=getattr(subprocess, "CREATE_NO_WINDOW", 0))
    except OSError as error:
        record["error"] = str(error)
        record["elapsed_seconds"] = round(time.monotonic() - started, 3)
        return record
    def drain(pipe, path):
        try:
            count = 0
            with pipe, path.open("xb") as stream:
                while chunk := pipe.read(64 * 1024):
                    allowed = max(0, MAX_LOG - count)
                    stream.write(chunk[:allowed])
                    count += len(chunk)
                    if count > MAX_LOG:
                        overflow.set()
                        break
        except Exception as error:
            failures.append(str(error))
            overflow.set()
    threads = [threading.Thread(target=drain, args=(pipe, path), daemon=True)
               for pipe, path in zip((process.stdout, process.stderr), paths)]
    for thread in threads:
        thread.start()
    while process.poll() is None:
        if overflow.wait(0.1):
            process.kill()  # Only the exact task-owned child.
            break
    record["exit_code"] = process.wait()
    for thread in threads:
        thread.join()
    record["elapsed_seconds"] = round(time.monotonic() - started, 3)
    record["streams"] = [file_record(path, path.relative_to(cwd).as_posix()) for path in paths]
    if overflow.is_set():
        record["error"] = "Command stream limit/read failure: " + "; ".join(failures)
    return record


def write_json(path, value):
    no_links(path)
    with path.open("x", encoding="utf-8", newline="\n") as stream:
        json.dump(value, stream, indent=2, sort_keys=True, allow_nan=False)
        stream.write("\n")


def verify_generated_build(output, compiler):
    # Do not accept whatever library/configuration happened to be produced.
    identifiers = list((output / "build/CMakeFiles").glob("*/CMakeCXXCompiler.cmake"))
    if len(identifiers) != 1:
        raise ValueError("Missing/ambiguous generated compiler identity")
    _, raw = consume(identifiers[0], MAX_JSON, retain=True)
    text = raw.decode("utf-8")
    def value(name):
        match = re.findall(r'set\(' + name + r' "([^"\r\n]+)"\)', text)
        if len(match) != 1:
            raise ValueError(f"Missing/ambiguous generated {name}")
        return match[0]
    if (local_absolute(value("CMAKE_CXX_COMPILER")) != compiler or
            value("CMAKE_CXX_COMPILER_VERSION") != COMPILER_VERSION):
        raise ValueError("Generated compiler differs from requested exact compiler")
    project = output / "build/libs/thread/boost_thread.vcxproj"
    _, project_data = consume(project, MAX_JSON, retain=True)
    tree = ET.fromstring(project_data)
    actual = []
    for node in tree.iter():
        if node.tag.rsplit("}", 1)[-1] == "ClCompile" and "Include" in node.attrib:
            source = local_absolute(project.parent / node.attrib["Include"])
            try:
                actual.append(source.relative_to(output / "source/libs/thread").as_posix())
            except ValueError as error:
                raise ValueError("Generated project compiles foreign source") from error
    if len(actual) != len(SOURCES) or set(actual) != SOURCES:
        raise ValueError("Generated thread compilation source set differs")
    return {"compiler": file_record(identifiers[0], identifiers[0].relative_to(output).as_posix()),
            "project": file_record(project, project.relative_to(output).as_posix()),
            "sources": sorted(actual), "compiler_version": COMPILER_VERSION}


def rebuild(*, source_kit, support_sdk, output, cmake, compiler, vs_instance,
            inputs_quiescent=False, prepare_only=False):
    if not inputs_quiescent:
        raise ValueError("Root caller must acknowledge frozen/quiescent inputs")
    kit, sdk, output, cmake, compiler, vs_instance = map(
        local_absolute, (source_kit, support_sdk, output, cmake, compiler, vs_instance))
    if any(any(char in str(path) for char in ';${}[]') for path in (sdk, output, cmake, compiler, vs_instance)):
        raise ValueError("CMake-facing paths contain expansion/list syntax")
    protected = [kit, sdk, cmake.parent, compiler.parent, vs_instance,
                 pathlib.Path(__file__).resolve().parents[1]]
    if output.exists() or any(overlaps(output, root) for root in protected):
        raise ValueError("Output must be fresh and disjoint from inputs/tools/workspace")
    if len(str(output)) > 100 or not output.parent.is_dir():
        raise ValueError("Output needs existing local parent and short path")
    inputs, copies = source_inputs(kit)
    sdk_files = inventory(sdk)
    sdk_table = {item["path"]: item for item in sdk_files}
    for name, digest in SDK_PINS.items():
        if sdk_table.get(name, {}).get("sha256") != digest:
            raise ValueError(f"Exact installed SDK binding differs: {name}")
    tools = {name: {"path": str(path), **consume(path)[0]} for name, path in
             (("cmake", cmake), ("compiler", compiler), ("script", pathlib.Path(__file__).resolve()))}
    result = {"schema_version": 1, "status": "preparing", "build_qualified": False,
              "source_closure_qualified": False, "offline_sdk_rebuild_qualified": False,
              "product_runtime_replaced": False, "redistribution_qualified": False,
              "inputs_quiescent": True, "input_snapshot_guarantee": False,
              "source_kit": str(kit), "support_sdk": str(sdk), "output_root": str(output),
              "source_kit_inputs": inputs, "sdk_files": sdk_files, "tools": tools,
              "vs_instance": str(vs_instance), "commands": commands(output, sdk, cmake, compiler, vs_instance),
              "command_results": [], "expected_library": LIBRARY}
    output.mkdir()
    evidence = output / "evidence"
    evidence.mkdir()
    try:
        for name, data in copies.items():
            target = evidence / "inputs" / name
            target.parent.mkdir(parents=True, exist_ok=True)
            target.write_bytes(data)
        source = output / "source"
        thread_records = extract_thread(copies[ARCHIVE], source / "libs/thread")
        headers = [item for item in thread_records if item["path"].startswith("include/")]
        if len(headers) != THREAD_HEADER_COUNT:
            raise ValueError("Exact thread header count differs")
        for item in headers:
            if sdk_table.get(item["path"]) != item:
                raise ValueError(f"Installed thread header differs from exact archive: {item['path']}")
        result["thread_headers"] = headers
        if not SOURCES <= {item["path"] for item in thread_records}:
            raise ValueError("Exact archive is missing Windows compilation sources")
        (source / "CMakeLists.txt").write_text(
            'cmake_minimum_required(VERSION 3.25)\n'
            'project(Boost VERSION 1.86.0 LANGUAGES CXX)\n'
            'set(BOOST_SUPERPROJECT_VERSION ${PROJECT_VERSION})\n'
            'set(BOOST_SUPERPROJECT_SOURCE_DIR "${PROJECT_SOURCE_DIR}")\n'
            f'list(APPEND CMAKE_MODULE_PATH "{sdk.as_posix()}/share/boost/cmake-build")\n'
            'include(BoostRoot)\n', encoding="utf-8", newline="\n")
        result["source_files"] = inventory(source)
        result["evidence_inputs"] = inventory(evidence / "inputs")
        write_json(evidence / "prepared.json", result)
        if not prepare_only:
            env = command_environment(output)
            result["environment"] = {"removed_inherited_build_flags": True,
                                     "temp": env["TEMP"], "path": env.get("PATH", "")}
            for index, argv in enumerate(result["commands"]):
                record = run_command(argv, output, evidence / "logs", index, env)
                result["command_results"].append(record)
                write_json(evidence / f"command-{index:02d}.json", record)
                if record["exit_code"] != 0 or record["error"]:
                    raise ValueError(f"Native command {index} failed; inspect captured receipt")
                if index == 2:
                    result["generated_build"] = verify_generated_build(output, compiler)
            result["library"] = file_record(output / LIBRARY, LIBRARY)
            if result["library"]["bytes"] == 0:
                raise ValueError("Empty rebuilt thread library")
        # Recheck closure after native commands/preparation while caller freeze holds.
        if (inventory(sdk) != sdk_files or inventory(source) != result["source_files"] or
                source_inputs(kit)[0] != inputs or inventory(evidence / "inputs") != result["evidence_inputs"]):
            raise ValueError("Consumed inputs/source snapshot changed during recipe")
        for name, record in tools.items():
            if consume(pathlib.Path(record["path"]))[0] != {k: record[k] for k in ("bytes", "sha256")}:
                raise ValueError(f"Consumed tool changed during recipe: {name}")
        result["status"] = "prepared" if prepare_only else "built"
        write_json(evidence / "rebuild.json", result)
        return result
    except Exception as error:
        result["status"] = "failed"
        result["error"] = f"{type(error).__name__}: {error}"
        write_json(evidence / "rebuild-failed.json", result)
        raise


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ("source-kit", "support-sdk", "output", "cmake", "compiler", "vs-instance"):
        parser.add_argument("--" + name, type=pathlib.Path, required=True)
    parser.add_argument("--prepare-only", action="store_true")
    parser.add_argument("--inputs-quiescent", action="store_true",
                        help="Root caller attests inputs/tools remain frozen until child completion")
    args = parser.parse_args(argv)
    try:
        args.output = generic_output(args.output)
        result = rebuild(**vars(args))
    except (ValueError, OSError, tarfile.TarError, ET.ParseError) as error:
        parser.exit(1, f"Rebuild refused/failed: {error}\n")
    print(json.dumps({"status": result["status"], "output_root": result["output_root"],
                      "build_qualified": False, "product_runtime_replaced": False}))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
