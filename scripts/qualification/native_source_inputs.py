"""Replay typed native source evidence through an explicit, offline resolver.

No extraction, executable invocation, network access or workspace fallback is
permitted. Receipts preserve their original identities even in a frozen kit.
"""
from __future__ import annotations

import hashlib
import json
import os
from pathlib import Path
import re
import stat
import sys
import tarfile
import zipfile

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import stage_ifc_sdk_sources as safe

MAX_FILE = 1024 * 1024 * 1024
MAX_JSON = 32 * 1024 * 1024
MAX_ARCHIVE_ENTRIES = 65_536
MAX_ARCHIVE_EXPANDED = 8 * 1024 * 1024 * 1024
MAX_MEMBER = 64 * 1024 * 1024
MAX_RETURNED = 256 * 1024 * 1024
ROLES = {"native_input", "source", "notice", "recipe", "metadata", "binary", "inventory"}
PROFILES = {"cpython_spdx": "cad-cpython", "geos_recipe": "cad-shapely",
            "openblas_wheel_recipe": "cad-numpy"}
PARENT_PROFILES = {
    "cpython_spdx": ("CPython", "3.13.15", "python-3.13.15-embed-amd64.zip",
                      "d1f04d990aee1253d8569e8e5104e30fa9f5fa830899f14843448872d936a2cf"),
    "geos_recipe": ("shapely", "2.1.2", "shapely-2.1.2-cp313-cp313-win_amd64.whl",
                    "ca2591bff6645c216695bdf1614fca9c82ea1144d4a7591a466fef64f28f0715"),
    "openblas_wheel_recipe": ("numpy", "2.5.3", "numpy-2.5.3-cp313-cp313-win_amd64.whl",
                              "71cad2b2a7451ab79d8f5e71b453485b6775963d5cf794179144a7463fe6e8ec"),
}


def require(condition, message):
    if not condition:
        raise ValueError(message)


def receipt(value):
    require(isinstance(value, dict) and set(value) <= {"path", "sha256", "sha512", "bytes"}
            and {"path", "sha256", "bytes"} <= set(value), "invalid native input receipt")
    result = {"path": safe.relative_path(value["path"]), "sha256": value["sha256"], "bytes": value["bytes"]}
    require(isinstance(result["sha256"], str) and re.fullmatch(r"[0-9a-f]{64}", result["sha256"]),
            "invalid native input SHA256")
    require(type(result["bytes"]) is int and 0 <= result["bytes"] <= MAX_FILE, "invalid native input byte count")
    if "sha512" in value:
        require(isinstance(value["sha512"], str) and re.fullmatch(r"[0-9a-f]{128}", value["sha512"]),
                "invalid native input SHA512")
        result["sha512"] = value["sha512"]
    return result


class Reader:
    """Every access calls the supplied resolver and registers its declared role."""
    def __init__(self, resolver):
        require(callable(resolver), "native input resolver is required")
        self.resolver = resolver
        self.consumed = {}
        self._roles = {}
        self._verified = {}

    def _resolve(self, value, role):
        row = receipt(value)
        require(role in ROLES, "unknown native input role")
        path = Path(self.resolver(row, role)).absolute()
        safe.no_links(path)
        before = path.lstat()
        require(stat.S_ISREG(before.st_mode) and before.st_nlink == 1
                and before.st_size == row["bytes"], "native input is not an ordinary exact-size file")
        key = (row["path"], row["sha256"], row.get("sha512"), row["bytes"])
        cached = self._verified.get(key)
        if cached is None:
            hashes = {name: hashlib.new(name) for name in ("sha256", "sha512") if name in row}
            with path.open("rb") as stream:
                require(safe.stamp(os.fstat(stream.fileno())) == safe.stamp(before), "native input changed before hashing")
                count = 0
                while block := stream.read(1024 * 1024):
                    count += len(block)
                    require(count <= MAX_FILE, "native input exceeds bound")
                    for value_hash in hashes.values():
                        value_hash.update(block)
                require(safe.stamp(os.fstat(stream.fileno())) == safe.stamp(before), "native input changed during hashing")
            require(count == row["bytes"] and all(h.hexdigest() == row[name] for name, h in hashes.items()),
                    "native input hash/bytes differ")
            self._verified[key] = (path, safe.stamp(before))
        else:
            require(cached == (path, safe.stamp(before)), "native input identity changed during replay")
        safe.no_links(path)
        require(safe.stamp(path.lstat()) == safe.stamp(before), "native input changed after hashing")
        existing = self.consumed.get(row["path"])
        require(existing is None or existing == row, "conflicting native input receipts")
        self.consumed[row["path"]] = row
        self._roles.setdefault(row["path"], set()).add(role)
        return row, path, before

    def record(self, value, role):
        return self._resolve(value, role)[0]

    def bytes(self, value, role, limit=MAX_JSON):
        require(type(limit) is int and 0 <= limit <= MAX_RETURNED, "invalid native byte read bound")
        row, path, before = self._resolve(value, role)
        data = safe.read_bounded(path, limit)
        require(hashlib.sha256(data).hexdigest() == row["sha256"]
                and safe.stamp(path.lstat()) == safe.stamp(before), "native input changed during read")
        return data

    def json(self, value, role):
        return safe.parse_json(self.bytes(value, role))

    def archive_members(self, value, names, role):
        require(isinstance(names, (list, tuple, set)) and len(names) <= MAX_ARCHIVE_ENTRIES,
                "invalid requested archive members")
        requested = [safe.relative_path(name) for name in names]
        safe.check_names(requested)
        wanted = set(requested)
        row, path, before = self._resolve(value, role)
        result, seen, explicit = {}, {}, set()
        count = expanded = returned = 0

        def member(name, size, kind, opener):
            nonlocal count, expanded, returned
            count += 1
            require(count <= MAX_ARCHIVE_ENTRIES, "native archive entry count exceeds bound")
            name = safe.relative_path(name.rstrip("/") if kind == "directory" else name)
            require(name.casefold() not in explicit, "duplicate/case-colliding native archive member")
            explicit.add(name.casefold())
            parts = name.split("/")
            for index in range(1, len(parts) + 1):
                spelling = "/".join(parts[:index])
                entry = (spelling, kind if index == len(parts) else "directory")
                require(seen.get(spelling.casefold(), entry) == entry, "native archive ancestor/case collision")
                seen[spelling.casefold()] = entry
            require(type(size) is int and 0 <= size <= MAX_FILE, "native archive member size exceeds bound")
            expanded += size
            require(expanded <= MAX_ARCHIVE_EXPANDED, "native archive expanded bytes exceed bound")
            if name in wanted:
                require(kind == "file" and size <= MAX_MEMBER, "requested native archive member is not a bounded ordinary file")
                returned += size
                require(returned <= MAX_RETURNED, "native archive returned bytes exceed bound")
                with opener() as stream:
                    data = stream.read(size + 1)
                require(len(data) == size, "native archive member byte count differs")
                result[name] = data

        try:
            with path.open("rb") as source:
                require(safe.stamp(os.fstat(source.fileno())) == safe.stamp(before), "native archive changed before read")
                if zipfile.is_zipfile(source):
                    with zipfile.ZipFile(source) as archive:
                        require(len(archive.infolist()) <= MAX_ARCHIVE_ENTRIES, "native archive entry count exceeds bound")
                        for info in archive.infolist():
                            mode = (info.external_attr >> 16) & 0xFFFF
                            file_type = stat.S_IFMT(mode)
                            if info.is_dir():
                                kind = "directory" if file_type in (0, stat.S_IFDIR) else "link"
                            else:
                                kind = "file" if file_type in (0, stat.S_IFREG) else "link"
                            require(not info.flag_bits & 1, "encrypted native archive member")
                            member(info.filename, info.file_size, kind, lambda info=info: archive.open(info))
                else:
                    source.seek(0)
                    with tarfile.open(fileobj=source, mode="r|*") as archive:
                        for info in archive:
                            kind = "file" if info.isfile() else ("directory" if info.isdir() else "link")
                            member(info.name, info.size, kind, lambda info=info: archive.extractfile(info))
                require(safe.stamp(os.fstat(source.fileno())) == safe.stamp(before), "native archive changed during read")
        except (zipfile.BadZipFile, tarfile.TarError, EOFError, RuntimeError) as error:
            raise ValueError("invalid native source archive") from error
        safe.no_links(path)
        require(safe.stamp(path.lstat()) == safe.stamp(before), "native archive changed after read")
        require(set(result) == wanted, "missing requested native archive member")
        return {name: result[name] for name in sorted(result)}

    def verify(self):
        # Rehash, rather than relying on the cached stamp at the delivery boundary.
        rows = list(self.consumed.values())
        self._verified.clear()
        for row in rows:
            self.record(row, sorted(self._roles[row["path"]])[0])


def targets(inventory, binary_receipts):
    """Use the original inventory and separately bound binary sizes."""
    require(isinstance(inventory, dict) and isinstance(inventory.get("binaries"), list),
            "native targets require an original inventory")
    bound = {}
    for row in binary_receipts:
        original = row.get("inventory_path", row["path"])
        key = (row["component_id"], safe.relative_path(original))
        require(key not in bound, "duplicate native binary identity")
        bound[key] = row
    result = {}
    for row in inventory["binaries"]:
        owner, name = row["component_id"], safe.relative_path(row["path"])
        actual = bound.get((owner, name))
        require(actual is not None and actual["sha256"] == row["sha256"], "native inventory binary binding differs")
        destination = safe.relative_path(row["destination"])
        require(isinstance(actual["sha256"], str) and re.fullmatch(r"[0-9a-f]{64}", actual["sha256"])
                and type(actual["bytes"]) is int and 0 <= actual["bytes"] <= MAX_FILE,
                "invalid native binary tuple")
        record = {"component_id": owner, "path": name, "destination": destination,
                  "sha256": actual["sha256"], "bytes": actual["bytes"]}
        result.setdefault(owner, []).append(record)
    require(sum(map(len, result.values())) == len(bound), "extra native binary binding")
    return {owner: sorted(rows, key=lambda row: (row["destination"], row["path"])) for owner, rows in result.items()}


def applicability(kind, owner, inventory, selected_targets, target_bindings=None):
    """Bind a fixed evidence profile to the authoritative selected parent."""
    require(isinstance(kind, str) and PROFILES.get(kind) == owner, "native parent evidence kind/owner differs")
    components = inventory.get("components")
    require(isinstance(components, list) and len(components) <= 512, "native parent inventory components missing")
    matches = [row for row in components if isinstance(row, dict) and row.get("id") == owner]
    require(len(matches) == 1, "native parent inventory owner missing or duplicated")
    package = matches[0].get("package")
    require(isinstance(package, dict) and isinstance(package.get("source"), dict), "native parent package/source missing")
    source = package["source"]
    name, version, filename, checksum = PARENT_PROFILES[kind]
    require(package.get("name") == name and package.get("version") == version
            and isinstance(package.get("license"), str), "native parent package identity differs")
    require(source.get("kind") == "locked-archive" and source.get("archive_filename") == filename
            and source.get("archive_sha256") == checksum
            and source.get("licensing_clearance") is False and source.get("source_closure_qualified") is False,
            "native parent locked archive identity differs")
    require(safe.relative_path(source.get("archive_path")).rsplit("/", 1)[-1] == filename,
            "native parent archive filename/path differs")
    members = source.get("archive_members")
    require(isinstance(members, dict) and len(members) <= MAX_ARCHIVE_ENTRIES,
            "native parent archive member mapping missing")
    require(isinstance(selected_targets, list) and 0 < len(selected_targets) <= MAX_ARCHIVE_ENTRIES,
            "native parent targets missing")
    for target in selected_targets:
        require(target.get("component_id") == owner, "native parent selected target owner differs")
        original_path, destination = safe.relative_path(target["path"]), safe.relative_path(target["destination"])
        member = safe.relative_path(members.get(original_path))
        require(original_path.endswith("/" + member) and destination.endswith("/" + member),
                "native parent selected archive member mapping differs")
    if target_bindings is not None:
        require(isinstance(target_bindings, list) and 0 < len(target_bindings) <= len(selected_targets),
                "native parent typed target bindings missing")
        selected = {row["path"]: row for row in selected_targets}
        seen = set()
        for binding in target_bindings:
            require(isinstance(binding, dict), "invalid native parent typed target binding")
            if "target" in binding:
                target, member = binding["target"], binding.get("member")
            else:
                target = {key: binding.get(key) for key in
                          ("component_id", "path", "destination", "sha256", "bytes")}
                member = binding.get("archive_member")
            require(isinstance(target, dict) and selected.get(target.get("path")) == target
                    and target["path"] not in seen, "native parent typed target identity differs")
            seen.add(target["path"])
            require(members.get(target["path"]) == safe.relative_path(member),
                    "native parent typed archive member differs")
    return {key: package[key] for key in ("name", "version", "license")}


def normalize(kind, owner, input_receipt, reader, selected_targets):
    require(PROFILES.get(kind) == owner, "native evidence kind/component differs")
    if kind == "cpython_spdx":
        import native_cpython_source_inputs as profile
        value = profile.validate_inputs(input_receipt, reader, selected_targets)
    else:
        import native_wheel_source_inputs as profile
        function = profile.validate_geos_inputs if kind == "geos_recipe" else profile.validate_openblas_inputs
        value = function(input_receipt, reader, selected_targets)
    require(isinstance(value, dict) and set(value) == {"sources", "notices", "artifacts", "recipe", "remaining",
                                                      "dependency_dispositions", "target_bindings"},
            "invalid normalized native contribution")
    return value


def digest(value):
    return hashlib.sha256(json.dumps(value, sort_keys=True, separators=(",", ":"),
                                    ensure_ascii=True, allow_nan=False).encode("utf-8")).hexdigest()


def original(value):
    """Undo only the explicit frozen-workspace receipt relocation."""
    if isinstance(value, list):
        return [original(item) for item in value]
    if not isinstance(value, dict):
        return value
    result = {key: original(item) for key, item in value.items() if key != "inventory_path"}
    if "inventory_path" in value:
        result["path"] = value["inventory_path"]
    return result


def descriptor(kind, input_receipt, contribution, consumed):
    return {"evidence_kind": kind, "input_receipt": receipt(input_receipt), "contribution_sha256": digest(contribution),
            "dependency_dispositions": contribution["dependency_dispositions"],
            "target_bindings": contribution["target_bindings"],
            "consumed_inputs": [receipt(consumed[name]) for name in sorted(consumed)]}


def envelope(value, owners):
    require(isinstance(value, dict) and set(value) == {"schema_version", "entries"}
            and type(value["schema_version"]) is int and value["schema_version"] == 1,
            "invalid native source input envelope")
    entries = value["entries"]
    require(isinstance(entries, list) and len(entries) <= len(PROFILES), "invalid native source entries")
    result = {}
    for row in entries:
        require(isinstance(row, dict) and set(row) == {"component_id", "evidence_kind", "input_receipt"},
                "invalid native source entry")
        owner, kind = row["component_id"], row["evidence_kind"]
        require(isinstance(owner, str) and isinstance(kind, str) and owner in owners and PROFILES.get(kind) == owner
                and owner not in result, "unknown/duplicate native source owner or evidence kind")
        result[owner] = {"evidence_kind": kind, "input_receipt": receipt(row["input_receipt"])}
    return result


def validate_contribution(component, contribution):
    """The report must actually include every derived source, notice and recipe."""
    for key in ("sources", "notices", "artifacts"):
        declared = original(component[key])
        require(all(row in declared for row in contribution[key]), "native contribution missing or altered " + key)
    require(all(row in component["remaining"] for row in contribution["remaining"]),
            "native unresolved obligation missing")
    expected, actual = contribution["recipe"], original(component["recipe"])
    require(actual is not None and expected is not None and actual["status"] == expected["status"]
            and all(row in actual["files"] for row in expected["files"])
            and all(row in actual["recipe_options"] for row in expected["recipe_options"])
            and actual.get("options_role") == expected.get("options_role"), "native recipe contribution differs")
