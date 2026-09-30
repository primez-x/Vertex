"""Stage installed IFC SDK recipe/source inputs; never build or qualify them.

Historical recipes come from recorded Git tree objects, not current ports.
Archives are opaque, hash-matched inputs: no dependency source is executed or
extracted. A failure retains partial output and publishes no success manifest.
Manager source, toolchain redistribution and a clean offline rebuild remain
separate work; this is not complete product corresponding-source qualification.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import os
import pathlib
import re
import shutil
import stat
import subprocess
import threading

MANIFEST = "ifc-sdk-source-manifest.json"
MAX_PACKAGES = 512
MAX_FILES = 20_000
MAX_JSON = 16 * 1024 * 1024
MAX_MANIFEST_BYTES = MAX_JSON
MAX_RECIPE = 8 * 1024 * 1024
MAX_ARCHIVE = 512 * 1024 * 1024
MAX_TOTAL = 4 * 1024 * 1024 * 1024
MAX_CACHE_FILES = 4096
MAX_CACHE_BYTES = 16 * 1024 * 1024 * 1024
MANAGER_REPOSITORY = "https://github.com/microsoft/vcpkg.git"
TARGET = "x64-windows-ifc-static"
HOST = "x64-windows"
RESERVED = {"con", "prn", "aux", "nul", "conin$", "conout$",
            *(f"com{i}" for i in "123456789¹²³"), *(f"lpt{i}" for i in "123456789¹²³")}
INPUT_NAMES = ("third_party/ifc-source-lock.json", "third_party/ifc-source/vcpkg.json",
               "third_party/ifc-source/support/vcpkg.json",
               "third_party/ifc-source/triplets/x64-windows-ifc-static.cmake")


def digest(data: bytes, algorithm="sha256") -> str:
    return hashlib.new(algorithm, data).hexdigest()


def no_links(path: pathlib.Path) -> None:
    for candidate in (path, *path.parents):
        try:
            info = candidate.lstat()
        except FileNotFoundError:
            continue
        if stat.S_ISLNK(info.st_mode) or getattr(info, "st_file_attributes", 0) & 0x400:
            raise ValueError(f"Path crosses a link/reparse point: {candidate}")


def relative_path(value: str) -> str:
    if not isinstance(value, str) or not value or len(value) > 1024 or "\\" in value:
        raise ValueError("Unsafe relative path")
    value.encode("utf-8", "strict")
    parts = value.split("/")
    if len(parts) > 64 or any(not p or p in {".", ".."} or len(p) > 255 or
                            p.endswith((" ", ".")) or p.casefold() == ".git" or
                            p.split(".", 1)[0].casefold() in RESERVED or
                            any(ord(c) < 32 or c in ':*?"<>|' for c in p) for p in parts):
        raise ValueError(f"Unsafe relative path: {value}")
    return value


def check_names(names) -> None:
    seen = {}
    explicit = set()
    for name in names:
        parts = relative_path(name).split("/")
        if name.casefold() in explicit:
            raise ValueError("Duplicate/case-colliding file")
        explicit.add(name.casefold())
        for i in range(1, len(parts) + 1):
            spelling = "/".join(parts[:i])
            kind = i < len(parts)
            key = spelling.casefold()
            if key in seen and seen[key] != (spelling, kind):
                raise ValueError("Ancestor/case-colliding path")
            seen[key] = (spelling, kind)


def stamp(info):
    mode = stat.S_IFMT(info.st_mode) if os.name == "nt" else info.st_mode
    return info.st_dev, info.st_ino, mode, info.st_size, info.st_mtime_ns


def read_bounded(path: pathlib.Path, limit: int) -> bytes:
    try:
        no_links(path)
        before = path.lstat()
        if not stat.S_ISREG(before.st_mode) or before.st_size > limit:
            raise ValueError(f"Nonregular/oversized input: {path}")
        with path.open("rb") as stream:
            if stamp(os.fstat(stream.fileno())) != stamp(before):
                raise ValueError("Input changed before read")
            data = stream.read(limit + 1)
            after = os.fstat(stream.fileno())
        no_links(path)
        if (len(data) != before.st_size or len(data) > limit or stamp(after) != stamp(before) or
                stamp(path.lstat()) != stamp(before)):
            raise ValueError(f"Input changed while reading: {path}")
        return data
    except OSError as error:
        raise ValueError(f"Cannot read input: {path}") from error


def _object(pairs):
    value = {}
    for key, item in pairs:
        if key in value:
            raise ValueError("Duplicate JSON key")
        value[key] = item
    return value


def parse_json(data):
    try:
        value = json.loads(data, object_pairs_hook=_object,
                           parse_constant=lambda _: (_ for _ in ()).throw(ValueError("Nonfinite JSON")))
    except (UnicodeError, json.JSONDecodeError) as error:
        raise ValueError("Invalid UTF-8 JSON") from error
    if not isinstance(value, dict):
        raise ValueError("Expected JSON object")
    return value


def checksum(record, algorithm):
    values = record.get("checksums")
    if not isinstance(values, list):
        raise ValueError("Missing checksum table")
    matches = [v.get("checksumValue") for v in values if isinstance(v, dict) and v.get("algorithm") == algorithm]
    length = 64 if algorithm == "SHA256" else 128
    if len(matches) != 1 or not isinstance(matches[0], str) or not re.fullmatch(f"[0-9a-f]{{{length}}}", matches[0]):
        raise ValueError(f"Invalid {algorithm} binding")
    return matches[0]


class GitObjects:
    def __init__(self, workspace):
        self.root = workspace / ".deps/vcpkg"
        no_links(self.root)
        metadata = self.root / ".git"
        no_links(metadata)
        if not metadata.is_dir() or (metadata / "commondir").exists() or (metadata / "objects/info/alternates").exists():
            raise ValueError("Manager must have standalone local Git objects")
        for index, candidate in enumerate(metadata.rglob("*")):
            if index >= 100_000:
                raise ValueError("Manager metadata entry limit")
            no_links(candidate)
        for name in ("config", "config.worktree"):
            path = metadata / name
            if path.exists() and re.search(rb"(?im)^\s*\[\s*(?:include|includeif|filter)\b", read_bounded(path, MAX_RECIPE)):
                raise ValueError("Manager Git config has external includes or filters")
        executable = shutil.which("git.exe" if os.name == "nt" else "git")
        if not executable:
            raise ValueError("Git executable unavailable")
        self.executable = pathlib.Path(executable).absolute()
        no_links(self.executable)
        self.executable = self.executable.resolve()
        if self.executable == workspace or workspace in self.executable.parents:
            raise ValueError("Git executable resolves inside the input workspace")
        self.executable_sha256 = digest(read_bounded(self.executable, MAX_ARCHIVE))
        self.environment = {k: v for k, v in os.environ.items() if not k.upper().startswith("GIT_")}
        self.environment.update(GIT_CONFIG_NOSYSTEM="1", GIT_CONFIG_GLOBAL=os.devnull,
                                GIT_NO_LAZY_FETCH="1", GIT_TERMINAL_PROMPT="0")

    def run(self, *args, limit=MAX_RECIPE):
        command = [str(self.executable), "--no-replace-objects", "-c", "core.fsmonitor=false",
                   "-c", "core.untrackedCache=false", "-c", "core.hooksPath=" + os.devnull,
                   "-c", "core.attributesFile=" + os.devnull, "-C", str(self.root), *args]
        options = {"creationflags": subprocess.CREATE_NO_WINDOW} if os.name == "nt" else {}
        try:
            process = subprocess.Popen(command, env=self.environment, stdin=subprocess.DEVNULL,
                                       stdout=subprocess.PIPE, stderr=subprocess.PIPE, **options)
        except OSError as error:
            raise ValueError("Cannot launch local Git") from error
        buffers = [bytearray(), bytearray()]
        failed = []
        def drain(stream, buffer, bound):
            try:
                while True:
                    chunk = stream.read(65536)
                    if not chunk:
                        break
                    if len(buffer) + len(chunk) > bound:
                        failed.append("Git output exceeds bound")
                        process.kill()
                        break
                    buffer.extend(chunk)
            except OSError:
                failed.append("Cannot read Git output")
            finally:
                stream.close()
        readers = [threading.Thread(target=drain, args=(process.stdout, buffers[0], limit)),
                   threading.Thread(target=drain, args=(process.stderr, buffers[1], 65536))]
        for reader in readers:
            reader.start()
        try:
            code = process.wait(timeout=30)
        except subprocess.TimeoutExpired:
            process.kill()
            process.wait()
            failed.append("Local Git timed out")
            code = -1
        finally:
            for reader in readers:
                reader.join()
        if failed or code:
            raise ValueError("Local Git object read failed: " + (failed[0] if failed else bytes(buffers[1]).decode("utf-8", "replace")[:512]))
        return bytes(buffers[0])

    def identity(self, expected):
        head = self.run("rev-parse", "--verify", "HEAD", limit=1024).decode().strip()
        origin = self.run("config", "--local", "--no-includes", "--get-all", "remote.origin.url", limit=2048).decode().strip()
        if head != expected or origin != MANAGER_REPOSITORY:
            raise ValueError("Manager revision/origin binding differs")
        return {"repository": origin, "revision": head, "git_executable_sha256": self.executable_sha256,
                "manager_source_included": False, "offline_sdk_rebuild_qualified": False}

    def recipe(self, tree, table):
        if self.run("cat-file", "-t", tree, limit=64).strip() != b"tree":
            raise ValueError("Historical recipe object is not a tree")
        raw = self.run("ls-tree", "-rz", "--full-tree", tree)
        records = raw.split(b"\0")
        if records[-1] != b"" or not 0 < len(records) - 1 <= MAX_FILES:
            raise ValueError("Invalid Git recipe inventory")
        objects = {}
        for record in records[:-1]:
            try:
                header, name_bytes = record.split(b"\t", 1)
                mode, kind, oid = header.decode("ascii").split(" ")
                name = relative_path(name_bytes.decode("utf-8", "strict"))
            except (ValueError, UnicodeError) as error:
                raise ValueError("Malformed/unsafe Git recipe entry") from error
            if mode not in {"100644", "100755"} or kind != "blob" or not re.fullmatch(r"[0-9a-f]{40}", oid):
                raise ValueError("Nonregular Git recipe entry")
            if name in objects:
                raise ValueError("Duplicate Git recipe entry")
            objects[name] = oid
        check_names(objects)
        if set(objects) != set(table):
            raise ValueError("Git recipe file set differs from receipt")
        result = {}
        for name, oid in sorted(objects.items()):
            size_bytes = self.run("cat-file", "-s", oid, limit=64).strip()
            if not re.fullmatch(rb"\d+", size_bytes) or int(size_bytes) > MAX_RECIPE:
                raise ValueError("Recipe blob exceeds limit")
            data = self.run("cat-file", "blob", oid, limit=MAX_RECIPE)
            if len(data) != int(size_bytes) or digest(data) != table[name]:
                raise ValueError("Historical recipe SHA-256 differs from receipt")
            result[name] = data
        return result

    def manager_license(self, revision):
        raw = self.run("ls-tree", "-z", revision, "--", "LICENSE.txt")
        match = re.fullmatch(rb"100644 blob ([0-9a-f]{40})\tLICENSE\.txt\0", raw)
        if not match:
            raise ValueError("Locked manager LICENSE.txt blob missing")
        oid = match[1].decode()
        data = self.run("cat-file", "blob", oid, limit=MAX_RECIPE)
        if b"MIT License" not in data or b"Microsoft" not in data:
            raise ValueError("Locked manager license is not the expected MIT notice")
        return oid, data


def parse_status(data):
    try:
        text = data.decode("utf-8", "strict")
    except UnicodeError as error:
        raise ValueError("Invalid status encoding") from error
    result, features = {}, {}
    for paragraph in re.split(r"\r?\n\s*\r?\n", text.strip()):
        fields = {}
        last = None
        for line in paragraph.splitlines():
            if line[:1].isspace() and last:
                fields[last] += " " + line.strip()
                continue
            if ": " not in line:
                raise ValueError("Malformed installed status")
            key, value = line.split(": ", 1)
            if key in fields:
                raise ValueError("Duplicate status field")
            fields[key], last = value, key
        name, architecture = fields.get("Package"), fields.get("Architecture")
        if not isinstance(name, str) or not re.fullmatch(r"[a-z0-9]+(?:-[a-z0-9]+)*", name) or architecture not in {TARGET, HOST}:
            raise ValueError("Unsupported package identity")
        if fields.get("Status") != "install ok installed":
            raise ValueError("SDK status contains noninstalled package")
        key = name, architecture
        if "Feature" in fields:
            feature = fields["Feature"]
            if not re.fullmatch(r"[a-z0-9]+(?:-[a-z0-9]+)*", feature) or feature == "core":
                raise ValueError("Invalid installed feature")
            features.setdefault(key, []).append(fields)
            continue
        if key in result or not fields.get("Version") or not re.fullmatch(r"[0-9a-f]{64}", fields.get("Abi", "")):
            raise ValueError("Duplicate/invalid core package status")
        pv = fields.get("Port-Version", "0")
        if not re.fullmatch(r"0|[1-9][0-9]{0,5}", pv):
            raise ValueError("Invalid port revision")
        result[key] = {"name": name, "architecture": architecture, "version": fields["Version"],
                       "port_version": int(pv), "abi": fields["Abi"], "features": ["core"],
                       "dependency_text": fields.get("Depends", "")}
    if not 0 < len(result) <= MAX_PACKAGES:
        raise ValueError("SDK package count limit")
    for key, paragraphs in features.items():
        if key not in result:
            raise ValueError("Feature lacks installed core package")
        for fields in paragraphs:
            feature = fields["Feature"]
            if feature in result[key]["features"]:
                raise ValueError("Duplicate installed feature")
            result[key]["features"].append(feature)
            if fields.get("Depends"):
                result[key]["dependency_text"] += ", " + fields["Depends"]
    for package in result.values():
        dependencies = []
        for item in package.pop("dependency_text").split(","):
            item = item.strip()
            if not item:
                continue
            match = re.fullmatch(r"([a-z0-9]+(?:-[a-z0-9]+)*)(?::([a-z0-9-]+)| \(([a-z0-9-]+)\))?", item)
            if not match:
                raise ValueError("Malformed installed dependency")
            dep = match[1], match[2] or match[3] or package["architecture"]
            if dep not in result:
                raise ValueError("Installed dependency target missing")
            value = {"name": dep[0], "architecture": dep[1], "abi": result[dep]["abi"]}
            if value not in dependencies:
                dependencies.append(value)
        package["dependencies"] = sorted(dependencies, key=lambda d: (d["name"], d["architecture"]))
        package["features"].sort()
    return result


def resources_from(value):
    records = value.get("packages")
    if not isinstance(records, list) or len(records) > MAX_PACKAGES:
        raise ValueError("Invalid resource package table")
    result = []
    for item in records:
        if not isinstance(item, dict) or not isinstance(item.get("SPDXID"), str):
            raise ValueError("Invalid SPDX package")
        if not item["SPDXID"].startswith("SPDXRef-resource-"):
            continue
        url = item.get("downloadLocation")
        if not isinstance(url, str) or not re.fullmatch(r"git\+https://[^\s@?#]+@[^\s?#]+", url):
            raise ValueError("Unsupported source-resource URL")
        result.append({"name": item.get("name"), "url": url, "sha512": checksum(item, "SHA512")})
    return sorted(result, key=lambda r: (r["sha512"], r["url"]))


def known_uninstall_payload(receipt):
    expected = {"./BUILD_INFO", "./share/boost/vcpkg-cmake-wrapper.cmake"}
    files = receipt.get("files")
    if not isinstance(files, list) or any(not isinstance(record, dict) for record in files):
        return False
    records = [record for record in files
               if str(record.get("SPDXID", "")).startswith("SPDXRef-binary-file-")]
    if (len(records) != len(expected) or
            {record.get("fileName") for record in records} != expected or
            len({record["SPDXID"] for record in records}) != len(records)):
        return False
    try:
        for record in records:
            checksum(record, "SHA256")
    except ValueError:
        return False
    return True


def write_exclusive(path: pathlib.Path, data: bytes) -> None:
    no_links(path)
    path.parent.mkdir(parents=True, exist_ok=True)
    no_links(path)
    with path.open("xb") as stream:
        stream.write(data)


def file_record(name, data):
    return {"path": relative_path(name), "bytes": len(data), "sha256": digest(data)}


def inventory(root, ignore_manifest=False):
    no_links(root)
    records = []
    entries = 0
    total = 0
    for directory, subdirs, filenames in os.walk(root, followlinks=False):
        for name in subdirs + filenames:
            path = pathlib.Path(directory) / name
            no_links(path)
            entries += 1
            if entries > MAX_FILES * 2:
                raise ValueError("Output entry limit")
        for name in filenames:
            path = pathlib.Path(directory) / name
            relative = path.relative_to(root).as_posix()
            if ignore_manifest and relative == MANIFEST:
                continue
            data = read_bounded(path, MAX_ARCHIVE)
            total += len(data)
            if total > MAX_TOTAL:
                raise ValueError("Output byte limit")
            records.append(file_record(relative, data))
    if len(records) > MAX_FILES:
        raise ValueError("Output file count limit")
    check_names(r["path"] for r in records)
    return sorted(records, key=lambda r: r["path"])


def verify(output):
    output = pathlib.Path(output).absolute()
    value = parse_json(read_bounded(output / MANIFEST, MAX_MANIFEST_BYTES))
    if (type(value.get("schema_version")) is not int or value["schema_version"] != 1 or
            any(value.get(name) is not False for name in ("build_qualified", "source_closure_qualified", "product_runtime_replaced"))):
        raise ValueError("Manifest qualification/schema differs")
    if not isinstance(value.get("resources"), list) or not value["resources"]:
        raise ValueError("Manifest lacks source archive closure")
    records = value.get("files")
    if not isinstance(records, list) or not 0 < len(records) <= MAX_FILES:
        raise ValueError("Invalid manifest file table")
    for record in records:
        if (not isinstance(record, dict) or set(record) != {"path", "bytes", "sha256"} or
                type(record["bytes"]) is not int or not 0 <= record["bytes"] <= MAX_ARCHIVE or
                not isinstance(record["sha256"], str) or not re.fullmatch(r"[0-9a-f]{64}", record["sha256"])):
            raise ValueError("Invalid manifest file binding")
        relative_path(record["path"])
    check_names(record["path"] for record in records)
    if value.get("files") != inventory(output, ignore_manifest=True):
        raise ValueError("Staged file inventory differs")
    verify_metadata(output, value)
    return value


def verify_metadata(output, value):
    files = {record["path"]: record for record in value["files"]}
    inputs = value.get("inputs")
    if (value.get("scope") != "installed-ifc-sdk-recipe-and-source-inputs" or not isinstance(inputs, list) or
            any(not isinstance(record, dict) or type(record.get("bytes")) is not int for record in inputs) or
            inputs != [record for record in value["files"] if record["path"].startswith("inputs/")]):
        raise ValueError("Manifest scope/input binding differs")
    def backed(path):
        relative_path(path)
        if path not in files:
            raise ValueError("Manifest reference lacks staged file")
        return files[path]
    manager = value.get("manager")
    if (not isinstance(manager, dict) or manager.get("repository") != MANAGER_REPOSITORY or
            not isinstance(manager.get("revision"), str) or not re.fullmatch(r"[0-9a-f]{40}", manager["revision"]) or
            manager.get("manager_source_included") is not False or manager.get("offline_sdk_rebuild_qualified") is not False):
        raise ValueError("Invalid identity-only manager metadata")
    license_record = manager.get("license")
    if (not isinstance(license_record, dict) or license_record.get("path") != "notices/manager/LICENSE.txt" or
            not isinstance(license_record.get("git_blob"), str) or not re.fullmatch(r"[0-9a-f]{40}", license_record["git_blob"]) or
            type(license_record.get("bytes")) is not int):
        raise ValueError("Invalid manager license binding")
    actual_license = backed(license_record["path"])
    if any(actual_license[k] != license_record.get(k) for k in ("bytes", "sha256")):
        raise ValueError("Manager license inventory binding differs")
    license_data = read_bounded(output / license_record["path"], MAX_RECIPE)
    if hashlib.sha1(b"blob " + str(len(license_data)).encode() + b"\0" + license_data).hexdigest() != license_record["git_blob"]:
        raise ValueError("Manager license Git blob binding differs")
    lock_path = "inputs/third_party/ifc-source-lock.json"
    backed(lock_path)
    locked = parse_json(read_bounded(output / lock_path, MAX_JSON))
    swig = locked.get("swig")
    provenance = swig.get("hash_provenance") if isinstance(swig, dict) else None
    if (type(locked.get("schema_version")) is not int or locked["schema_version"] != 1 or
            locked.get("build_qualified") is not False or locked.get("source_closure_qualified") is not False or
            not isinstance(provenance, dict) or provenance.get("repository") != manager["repository"] or
            provenance.get("revision") != manager["revision"]):
        raise ValueError("Manager identity differs from staged lock")
    packages = value.get("packages")
    resources = value.get("resources")
    if not isinstance(packages, list) or not 0 < len(packages) <= MAX_PACKAGES * 2:
        raise ValueError("Invalid package closure table")
    statuses = {label: parse_status(read_bounded(output / f"receipts/{label}/status", MAX_JSON))
                for label in ("kernel", "support")}
    seen = set()
    urls = {}
    for package in packages:
        if not isinstance(package, dict):
            raise ValueError("Invalid package record")
        label, name, architecture = package.get("sdk"), package.get("name"), package.get("architecture")
        if not all(isinstance(item, str) for item in (label, name, architecture)) or label not in statuses:
            raise ValueError("Invalid package identity")
        key = (label, name, architecture)
        if key in seen or (name, architecture) not in statuses[label]:
            raise ValueError("Duplicate/unknown package identity")
        seen.add(key)
        expected = statuses[label][name, architecture]
        if any(package.get(k) != v or (k == "port_version" and type(package.get(k)) is not int)
               for k, v in expected.items()):
            raise ValueError("Package metadata differs from staged status")
        prefix = f"receipts/{label}/{architecture}/{name}"
        receipts = package.get("receipts")
        mandatory = [prefix + "/vcpkg.spdx.json", prefix + "/vcpkg_abi_info.txt"]
        if not isinstance(receipts, list) or receipts not in (mandatory, mandatory + [prefix + "/vcpkg-spdx-resources.json"]):
            raise ValueError("Invalid package receipt references")
        for path in receipts:
            backed(path)
        receipt = parse_json(read_bounded(output / mandatory[0], MAX_JSON))
        items = receipt.get("packages")
        if not isinstance(items, list) or any(not isinstance(item, dict) for item in items):
            raise ValueError("Invalid staged receipt package table")
        ports = [item for item in items if item.get("SPDXID") == "SPDXRef-port"]
        binaries = [item for item in items if item.get("SPDXID") == "SPDXRef-binary"]
        version = expected["version"] + (f"#{expected['port_version']}" if expected["port_version"] else "")
        if (len(ports) != 1 or len(binaries) != 1 or ports[0].get("name") != name or
                ports[0].get("versionInfo") != version or binaries[0].get("name") != f"{name}:{architecture}" or
                binaries[0].get("versionInfo") != expected["abi"]):
            raise ValueError("Staged receipt identity differs")
        recipe = package.get("recipe")
        if not isinstance(recipe, dict) or not isinstance(recipe.get("git_tree"), str) or not re.fullmatch(r"[0-9a-f]{40}", recipe["git_tree"]):
            raise ValueError("Invalid recipe identity")
        tree = recipe["git_tree"]
        if recipe.get("path") != f"recipes/{tree}" or ports[0].get("downloadLocation") not in (
                f"git+https://github.com/Microsoft/vcpkg@{tree}", f"git+https://github.com/microsoft/vcpkg@{tree}"):
            raise ValueError("Recipe tree/receipt binding differs")
        recipe_files = recipe.get("files")
        if not isinstance(recipe_files, list) or not recipe_files:
            raise ValueError("Recipe file table missing")
        table = {}
        for record in recipe_files:
            if not isinstance(record, dict) or set(record) != {"path", "bytes", "sha256"} or type(record["bytes"]) is not int:
                raise ValueError("Invalid recipe file binding")
            path = relative_path(record["path"])
            if path in table:
                raise ValueError("Duplicate recipe path")
            actual = backed(recipe["path"] + "/" + path)
            if any(actual[k] != record[k] for k in ("bytes", "sha256")):
                raise ValueError("Recipe inventory binding differs")
            table[path] = record["sha256"]
        receipt_files = receipt.get("files")
        if not isinstance(receipt_files, list):
            raise ValueError("Staged recipe receipt file table missing")
        expected_table = {}
        for record in receipt_files:
            if isinstance(record, dict) and str(record.get("SPDXID", "")).startswith("SPDXRef-port-file-"):
                name_value = record.get("fileName")
                if not isinstance(name_value, str) or not name_value.startswith("./"):
                    raise ValueError("Invalid staged recipe receipt path")
                path = relative_path(name_value[2:])
                if path in expected_table:
                    raise ValueError("Duplicate staged recipe receipt path")
                expected_table[path] = checksum(record, "SHA256")
        if table != expected_table:
            raise ValueError("Recipe table differs from staged receipt")
        package_resources = resources_from(receipt)
        for resource in package_resources:
            resource["path"] = f"archives/sha512/{resource['sha512']}.archive"
            urls.setdefault(resource["sha512"], set()).add(resource["url"])
        if package.get("resources") != package_resources:
            raise ValueError("Package resource binding differs")
        notices = package.get("notices")
        origin = package.get("notice_origin")
        if not isinstance(notices, list) or not notices or not isinstance(origin, dict):
            raise ValueError("Missing notice references/origin")
        for path in notices:
            backed(path)
        if origin.get("kind") == "installed-package":
            if origin != {"kind": "installed-package", "paths": notices} or not any(p == prefix + "/copyright" for p in notices) or any(not p.startswith(prefix + "/") for p in notices):
                raise ValueError("Installed notice origin differs")
        elif origin == {"kind": "locked-manager-license", "revision": manager["revision"], "path": license_record["path"]}:
            port_json = parse_json(read_bounded(output / recipe["path"] / "vcpkg.json", MAX_RECIPE))
            if (name != "boost-uninstall" or (architecture, expected["version"], expected["port_version"]) != (TARGET, "1.86.0", 0) or
                    package_resources or notices != [license_record["path"]] or not known_uninstall_payload(receipt) or
                    ports[0].get("licenseConcluded") != "MIT" or binaries[0].get("licenseConcluded") != "MIT" or
                    port_json.get("name") != name or port_json.get("license") != "MIT"):
                raise ValueError("Invalid manager-license fallback")
        else:
            raise ValueError("Unknown notice origin")
    if seen != {(label, name, architecture) for label, status in statuses.items() for name, architecture in status}:
        raise ValueError("Package closure omits installed package")
    seen_resources = set()
    for resource in resources:
        if not isinstance(resource, dict) or set(resource) != {"sha512", "path", "urls", "bytes", "sha256"}:
            raise ValueError("Invalid source archive binding")
        key = resource["sha512"]
        if not isinstance(key, str) or not re.fullmatch(r"[0-9a-f]{128}", key) or key in seen_resources or key not in urls:
            raise ValueError("Unknown/duplicate archive digest")
        seen_resources.add(key)
        if resource["path"] != f"archives/sha512/{key}.archive" or resource["urls"] != sorted(urls[key]) or type(resource["bytes"]) is not int:
            raise ValueError("Source archive path/URL binding differs")
        actual = backed(resource["path"])
        if any(actual[k] != resource[k] for k in ("bytes", "sha256")) or digest(read_bounded(output / resource["path"], MAX_ARCHIVE), "sha512") != key:
            raise ValueError("Source archive bytes/digest binding differs")
    if seen_resources != set(urls):
        raise ValueError("Source resource closure differs")


def stage(workspace, output):
    workspace, output = pathlib.Path(workspace).absolute(), pathlib.Path(output).absolute()
    no_links(workspace)
    no_links(output)
    workspace, output = workspace.resolve(), output.resolve()
    if not workspace.is_dir() or not output.parent.is_dir() or output.exists():
        raise ValueError("Workspace/parent missing or output already exists")
    if workspace == output or workspace in output.parents or output in workspace.parents:
        raise ValueError("Workspace/output overlap")
    payload = {}
    source_inputs = {}
    def add_file(source, destination, limit=MAX_JSON):
        relative_path(destination)
        data = read_bounded(source, limit)
        if destination in payload and payload[destination] != data:
            raise ValueError("Conflicting staged input")
        payload[destination] = data
        source_inputs[source] = (limit, digest(data))
        return data
    bound = {name: add_file(workspace / name, "inputs/" + name) for name in INPUT_NAMES}
    # Record the actual shipped helper bytes, not an arbitrary workspace plugin.
    helper = pathlib.Path(__file__).absolute()
    add_file(helper, "inputs/stage_ifc_sdk_sources.py", MAX_RECIPE)
    lock = parse_json(bound[INPUT_NAMES[0]])
    swig = lock.get("swig")
    provenance = swig.get("hash_provenance") if isinstance(swig, dict) else None
    if not isinstance(provenance, dict):
        raise ValueError("Missing manager provenance binding")
    revision = provenance.get("revision")
    if (type(lock.get("schema_version")) is not int or lock["schema_version"] != 1 or
            lock.get("build_qualified") is not False or lock.get("source_closure_qualified") is not False or
            provenance.get("repository") != MANAGER_REPOSITORY or not isinstance(revision, str) or
            not re.fullmatch(r"[0-9a-f]{40}", revision)):
        raise ValueError("Unsupported/unbound manager lock")
    git = GitObjects(workspace)
    manager_identity = git.identity(revision)
    manager = dict(manager_identity)
    license_oid, license_bytes = git.manager_license(revision)
    license_path = "notices/manager/LICENSE.txt"
    payload[license_path] = license_bytes
    manager["license"] = {"path": license_path, "git_blob": license_oid,
                          "bytes": len(license_bytes), "sha256": digest(license_bytes)}
    packages = []
    recipes = {}
    all_resources = {}
    for label, manifest_name in (("kernel", INPUT_NAMES[1]), ("support", INPUT_NAMES[2])):
        sdkroot = workspace / f".deps/ifc-{label}"
        status = add_file(sdkroot / "vcpkg/status", f"receipts/{label}/status")
        installed = parse_status(status)
        manifest = parse_json(bound[manifest_name])
        baseline = manifest.get("builtin-baseline")
        if (not isinstance(manifest.get("dependencies"), list) or not isinstance(baseline, str) or
                not re.fullmatch(r"[0-9a-f]{40}", baseline) or not isinstance(manifest.get("overrides", []), list)):
            raise ValueError("Invalid SDK manifest")
        for dependency in manifest["dependencies"]:
            name = dependency if isinstance(dependency, str) else dependency.get("name") if isinstance(dependency, dict) else None
            if (name, TARGET) not in installed:
                raise ValueError("SDK manifest dependency missing from installed status")
        for override in manifest.get("overrides", []):
            if not isinstance(override, dict):
                raise ValueError("Invalid SDK override")
            package = installed.get((override.get("name"), TARGET))
            version = override.get("version", override.get("version-string"))
            pv = override.get("port-version", 0)
            if not package or type(pv) is not int or (package["version"], package["port_version"]) != (version, pv):
                raise ValueError("SDK installed version differs from manifest override")
        for key, package in sorted(installed.items()):
            name, architecture = key
            share = sdkroot / architecture / "share" / name
            prefix = f"receipts/{label}/{architecture}/{name}"
            receipt_bytes = add_file(share / "vcpkg.spdx.json", prefix + "/vcpkg.spdx.json")
            abi_bytes = add_file(share / "vcpkg_abi_info.txt", prefix + "/vcpkg_abi_info.txt")
            receipt = parse_json(receipt_bytes)
            items = receipt.get("packages")
            if not isinstance(items, list) or any(not isinstance(p, dict) for p in items):
                raise ValueError("Invalid installed SPDX package table")
            ports = [p for p in items if p.get("SPDXID") == "SPDXRef-port"]
            binaries = [p for p in items if p.get("SPDXID") == "SPDXRef-binary"]
            expected_version = package["version"] + (f"#{package['port_version']}" if package["port_version"] else "")
            if (len(ports) != 1 or len(binaries) != 1 or ports[0].get("name") != name or
                    ports[0].get("versionInfo") != expected_version or binaries[0].get("name") != f"{name}:{architecture}" or
                    binaries[0].get("versionInfo") != package["abi"]):
                raise ValueError("Status/SPDX package identity or ABI differs")
            location = ports[0].get("downloadLocation", "")
            match = re.fullmatch(r"git\+https://github\.com/(?:Microsoft|microsoft)/vcpkg@([0-9a-f]{40})", location)
            if not match:
                raise ValueError("Untrusted historical recipe origin")
            tree = match[1]
            table = {}
            files = receipt.get("files")
            if not isinstance(files, list) or len(files) > MAX_FILES:
                raise ValueError("Invalid SPDX file table")
            for record in files:
                if not isinstance(record, dict):
                    raise ValueError("Invalid SPDX file")
                if not str(record.get("SPDXID", "")).startswith("SPDXRef-port-file-"):
                    continue
                relative = record.get("fileName")
                if not isinstance(relative, str) or not relative.startswith("./"):
                    raise ValueError("Unsafe receipt recipe path")
                relative = relative_path(relative[2:])
                if relative in table:
                    raise ValueError("Duplicate receipt recipe path")
                table[relative] = checksum(record, "SHA256")
            if not table:
                raise ValueError("Receipt lacks complete recipe file table")
            check_names(table)
            recipe = git.recipe(tree, table)
            if tree in recipes and recipes[tree] != recipe:
                raise ValueError("Conflicting recipe tree binding")
            recipes[tree] = recipe
            recipe_prefix = f"recipes/{tree}"
            for relative, data in recipe.items():
                payload[recipe_prefix + "/" + relative] = data
            abi_fields = {}
            try:
                for line in abi_bytes.decode("utf-8", "strict").splitlines():
                    field, value = line.split(" ", 1)
                    if field in abi_fields:
                        raise ValueError("Duplicate ABI field")
                    abi_fields[field] = value
            except (UnicodeError, ValueError) as error:
                raise ValueError("Malformed ABI receipt") from error
            if sorted(abi_fields.get("features", "").split(";")) != package["features"]:
                raise ValueError("Installed features differ from ABI receipt")
            for relative, expected in table.items():
                if abi_fields.get(relative) != expected:
                    raise ValueError("ABI recipe file binding differs")
            for dependency in package["dependencies"]:
                if abi_fields.get(dependency["name"]) != dependency["abi"]:
                    raise ValueError("ABI dependency binding differs")
            notices = []
            no_links(share)
            for candidate in sorted(share.iterdir()):
                low = candidate.name.casefold()
                if low == "copyright" or low.startswith(("license", "copying")):
                    destination = prefix + "/" + candidate.name
                    add_file(candidate, destination, MAX_RECIPE)
                    notices.append(destination)
            resources = resources_from(receipt)
            notice_origin = {"kind": "installed-package", "paths": notices}
            if not any(path.endswith("/copyright") for path in notices):
                port_json = parse_json(recipe.get("vcpkg.json", b"{}"))
                if (name != "boost-uninstall" or (architecture, package["version"], package["port_version"]) != (TARGET, "1.86.0", 0) or
                        resources or notices or not known_uninstall_payload(receipt) or
                        ports[0].get("licenseConcluded") != "MIT" or binaries[0].get("licenseConcluded") != "MIT" or
                        port_json.get("name") != name or port_json.get("license") != "MIT"):
                    raise ValueError("Package copyright notice missing")
                notices = [license_path]
                notice_origin = {"kind": "locked-manager-license", "revision": revision, "path": license_path}
            resource_file = share / "vcpkg-spdx-resources.json"
            receipts = [prefix + "/vcpkg.spdx.json", prefix + "/vcpkg_abi_info.txt"]
            if resource_file.exists():
                recorded = parse_json(add_file(resource_file, prefix + "/vcpkg-spdx-resources.json"))
                if resources_from(recorded) != resources:
                    raise ValueError("Source resource receipts disagree")
                receipts.append(prefix + "/vcpkg-spdx-resources.json")
            elif resources:
                raise ValueError("Source-resource receipt missing")
            for resource in resources:
                resource["path"] = f"archives/sha512/{resource['sha512']}.archive"
                entry = all_resources.setdefault(resource["sha512"], {"sha512": resource["sha512"], "path": resource["path"], "urls": []})
                if resource["url"] not in entry["urls"]:
                    entry["urls"].append(resource["url"])
            packages.append({"sdk": label, **package, "recipe": {"git_tree": tree, "path": recipe_prefix,
                             "files": [file_record(n, d) for n, d in sorted(recipe.items())]},
                             "resources": resources, "notices": notices, "notice_origin": notice_origin, "receipts": receipts})
    if not all_resources:
        raise ValueError("SDK source archive closure is empty")
    cache = workspace / ".deps/vcpkg/downloads"
    no_links(cache)
    if not cache.is_dir():
        raise ValueError("Explicit source archive cache missing")
    matched = set()
    cache_files = sorted(cache.iterdir())
    if len(cache_files) > MAX_CACHE_FILES:
        raise ValueError("Archive cache entry limit")
    scanned_bytes = 0
    for candidate in cache_files:
        no_links(candidate)
        if candidate.is_dir():
            continue  # Tool subdirectories are outside the explicit flat archive cache.
        data = read_bounded(candidate, MAX_ARCHIVE)
        scanned_bytes += len(data)
        if scanned_bytes > MAX_CACHE_BYTES:
            raise ValueError("Archive cache total byte limit")
        key = digest(data, "sha512")
        if key in all_resources and key not in matched:
            entry = all_resources[key]
            payload[entry["path"]] = data
            entry.update(bytes=len(data), sha256=digest(data))
            source_inputs[candidate] = (MAX_ARCHIVE, digest(data))
            matched.add(key)
    if matched != set(all_resources):
        raise ValueError("Missing or altered corresponding source archive")
    if len(payload) > MAX_FILES or sum(map(len, payload.values())) > MAX_TOTAL:
        raise ValueError("Staged payload limit")
    check_names(payload)
    records = [file_record(name, data) for name, data in sorted(payload.items())]
    result = {"schema_version": 1, "build_qualified": False, "source_closure_qualified": False,
              "product_runtime_replaced": False, "scope": "installed-ifc-sdk-recipe-and-source-inputs",
              "manager": manager, "packages": packages,
              "resources": [dict(entry, urls=sorted(entry["urls"])) for _, entry in sorted(all_resources.items())],
              "inputs": [r for r in records if r["path"].startswith("inputs/")], "files": records}
    encoded = (json.dumps(result, indent=2, sort_keys=True) + "\n").encode()
    if len(encoded) > MAX_MANIFEST_BYTES:
        raise ValueError("Manifest byte limit")
    # All validation precedes reservation; mkdir is exclusive, with no cleanup.
    no_links(output)
    try:
        output.mkdir()
    except OSError as error:
        raise ValueError("Cannot reserve fresh output") from error
    for name, data in sorted(payload.items()):
        write_exclusive(output / name, data)
    for path, (limit, expected) in source_inputs.items():
        if digest(read_bounded(path, limit)) != expected:
            raise ValueError("Input changed during staging")
    if git.identity(revision) != manager_identity or git.manager_license(revision) != (license_oid, license_bytes):
        raise ValueError("Manager changed during staging")
    for tree, recipe in recipes.items():
        if git.recipe(tree, {name: digest(data) for name, data in recipe.items()}) != recipe:
            raise ValueError("Historical recipe changed during staging")
    if inventory(output) != records:
        raise ValueError("Output changed during staging")
    write_exclusive(output / MANIFEST, encoded)
    return verify(output)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--workspace", required=True, type=pathlib.Path)
    parser.add_argument("--output", required=True, type=pathlib.Path)
    args = parser.parse_args()
    try:
        result = stage(args.workspace, args.output)
    except (OSError, ValueError, UnicodeError) as error:
        parser.exit(1, f"IFC SDK source staging failed: {error}\n")
    print(f"Staged {len(result['packages'])} package instances, {len(result['resources'])} source archives; qualification remains false.")


if __name__ == "__main__":
    main()
