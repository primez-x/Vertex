"""Build a deterministic SPDX 2.3 SBOM from a distribution inventory.

The inventory is the authoritative, hash-checked record for the Windows
distribution closure.  This module turns that record into a portable SPDX
document without contacting a registry or trying to infer ownership from a
machine's installed files.  The document remains an evidence artifact;
licensing clearance and commercial redistribution approval are separate
review gates.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import pathlib
import re
import sys
from collections.abc import Mapping
from typing import Any


SPDX_VERSION = "SPDX-2.3"
DATA_LICENSE = "CC0-1.0"
SCHEMA_VERSION = 1
DEFAULT_DOCUMENT_NAME = "Property Studio distribution"
DEFAULT_OUTPUT = pathlib.Path("artifacts/runtime/distribution-sbom.spdx.json")
SHA256_RE = re.compile(r"^[0-9a-fA-F]{64}$")
PATH_RE = re.compile(r"^[^\\/:\x00]+(?:/[^\\/:\x00]+)*$")


class SbomError(ValueError):
    """Raised when an inventory cannot produce a valid SPDX document."""


def _error(message: str) -> None:
    raise SbomError(message)


def _require(condition: bool, message: str) -> None:
    if not condition:
        _error(message)


def _text(value: Any, field: str) -> str:
    _require(isinstance(value, str) and bool(value.strip()),
             f"{field} must be a nonempty string")
    _require("\x00" not in value, f"{field} contains a NUL")
    return value.strip()


def _path(value: Any, field: str) -> str:
    text = _text(value, field).replace("\\", "/")
    _require(not text.startswith("/") and not re.match(r"^[A-Za-z]:/", text),
             f"{field} must be relative")
    _require(".." not in pathlib.PurePosixPath(text).parts,
             f"{field} contains traversal")
    _require(PATH_RE.fullmatch(text) is not None,
             f"{field} must be a canonical relative path")
    return pathlib.PurePosixPath(text).as_posix()


def _sha256(value: Any, field: str) -> str:
    _require(isinstance(value, str) and SHA256_RE.fullmatch(value) is not None,
             f"{field} must be a 64-digit SHA-256 value")
    return value.lower()


def _spdx_id(prefix: str, seed: str) -> str:
    slug = re.sub(r"[^A-Za-z0-9.-]+", "-", seed).strip("-")[:42] or "item"
    digest = hashlib.sha256(seed.encode("utf-8")).hexdigest()[:20]
    return f"SPDXRef-{prefix}-{slug}-{digest}"


def _source_location(package: Mapping[str, Any]) -> str:
    source = package.get("source")
    if not isinstance(source, Mapping):
        return "NOASSERTION"
    for key in ("url", "provenance", "homepage"):
        value = source.get(key)
        if isinstance(value, str) and value.strip():
            return value.strip()
    return "NOASSERTION"


def _inventory_components(inventory: Mapping[str, Any]) -> list[Mapping[str, Any]]:
    components = inventory.get("components")
    _require(isinstance(components, list) and bool(components),
             "inventory components must be a nonempty list")
    result: list[Mapping[str, Any]] = []
    seen: set[str] = set()
    for index, component in enumerate(components):
        field = f"inventory.components[{index}]"
        _require(isinstance(component, Mapping), f"{field} must be an object")
        identifier = _text(component.get("id"), f"{field}.id")
        _require(identifier not in seen, f"inventory repeats component {identifier}")
        seen.add(identifier)
        package = component.get("package")
        _require(isinstance(package, Mapping), f"{field}.package must be an object")
        _text(package.get("name"), f"{field}.package.name")
        _text(package.get("version"), f"{field}.package.version")
        _text(package.get("license"), f"{field}.package.license")
        result.append(component)
    return sorted(result, key=lambda item: (str(item["id"]).casefold(), str(item["id"])))


def _file_records(inventory: Mapping[str, Any], components: list[Mapping[str, Any]]) -> tuple[
    list[dict[str, Any]], dict[str, list[str]]
]:
    """Collect binary, source, and notice files and package memberships."""

    by_key: dict[tuple[str, str], dict[str, Any]] = {}
    memberships: dict[str, set[str]] = {str(item["id"]): set() for item in components}

    def add(component_id: str, path: Any, digest: Any, *, kind: str,
            name: str | None = None) -> None:
        relative = _path(path, f"{kind} path")
        sha = _sha256(digest, f"{kind} hash")
        key = (relative.casefold(), sha)
        row = by_key.get(key)
        if row is None:
            row = {"path": relative, "sha256": sha, "kind": kind,
                   "name": name or pathlib.PurePosixPath(relative).name,
                   "components": set()}
            by_key[key] = row
        row["components"].add(component_id)
        memberships.setdefault(component_id, set()).add(_file_identity(row))

    for component in components:
        component_id = str(component["id"])
        for notice_index, notice in enumerate(component.get("notices", [])):
            _require(isinstance(notice, Mapping),
                     f"component {component_id}.notices[{notice_index}] must be an object")
            add(component_id, notice.get("path"), notice.get("sha256"), kind="notice")
        for source_key in ("source_inputs", "sources", "assets"):
            values = component.get(source_key, [])
            _require(isinstance(values, list),
                     f"component {component_id}.{source_key} must be a list")
            for source_index, source in enumerate(values):
                _require(isinstance(source, Mapping),
                         f"component {component_id}.{source_key}[{source_index}] must be an object")
                add(component_id, source.get("path"), source.get("sha256"), kind="source")

    binaries = inventory.get("binaries", [])
    _require(isinstance(binaries, list), "inventory binaries must be a list")
    for index, binary in enumerate(binaries):
        field = f"inventory.binaries[{index}]"
        _require(isinstance(binary, Mapping), f"{field} must be an object")
        component_id = _text(binary.get("component_id"), f"{field}.component_id")
        _require(component_id in memberships, f"{field} names an unknown component {component_id}")
        add(component_id, binary.get("destination"), binary.get("sha256"),
            kind="binary", name=_text(binary.get("name"), f"{field}.name"))

    static_inputs = inventory.get("static_inputs", [])
    _require(isinstance(static_inputs, list), "inventory static_inputs must be a list")
    for index, item in enumerate(static_inputs):
        field = f"inventory.static_inputs[{index}]"
        _require(isinstance(item, Mapping), f"{field} must be an object")
        component_id = _text(item.get("component_id"), f"{field}.component_id")
        _require(component_id in memberships, f"{field} names an unknown component {component_id}")
        values = item.get("source_inputs", [])
        _require(isinstance(values, list), f"{field}.source_inputs must be a list")
        for source_index, source in enumerate(values):
            _require(isinstance(source, Mapping),
                     f"{field}.source_inputs[{source_index}] must be an object")
            add(component_id, source.get("path"), source.get("sha256"), kind="source")

    records = []
    for row in sorted(by_key.values(), key=lambda value: (
            str(value["path"]).casefold(), str(value["path"]), str(value["sha256"]))):
        row = dict(row)
        row["components"] = sorted(row["components"], key=lambda value: (value.casefold(), value))
        records.append(row)
    return records, {key: sorted(value) for key, value in memberships.items()}


def _file_identity(row: Mapping[str, Any]) -> str:
    return f"{row['path']}\n{row['sha256']}\n{row['kind']}"


def _inventory_hash(inventory: Mapping[str, Any], supplied: str | None) -> str:
    if supplied is not None:
        return _sha256(supplied, "inventory_sha256")
    payload = json.dumps(inventory, ensure_ascii=False, sort_keys=True,
                         separators=(",", ":")).encode("utf-8")
    return hashlib.sha256(payload).hexdigest()


def build_sbom(inventory: Mapping[str, Any], inventory_sha256: str | None = None,
               *, document_name: str = DEFAULT_DOCUMENT_NAME) -> dict[str, Any]:
    """Build and validate a deterministic SPDX 2.3 JSON document."""

    _require(isinstance(inventory, Mapping), "inventory must be an object")
    _require(inventory.get("audit_status") == "incomplete",
             "inventory audit_status must remain 'incomplete'")
    components = _inventory_components(inventory)
    inventory_digest = _inventory_hash(inventory, inventory_sha256)
    files, memberships = _file_records(inventory, components)

    package_ids = {str(item["id"]): _spdx_id("Package", str(item["id"]))
                   for item in components}
    file_ids = {_file_identity(row): _spdx_id("File", _file_identity(row))
                for row in files}
    packages: list[dict[str, Any]] = []
    for component in components:
        component_id = str(component["id"])
        package = component["package"]
        package_files = [file_ids[key] for key in memberships.get(component_id, [])]
        package_row: dict[str, Any] = {
            "SPDXID": package_ids[component_id],
            "name": str(package["name"]).strip(),
            "versionInfo": str(package["version"]).strip(),
            "downloadLocation": _source_location(package),
            "licenseConcluded": "NOASSERTION",
            "licenseDeclared": str(package["license"]).strip(),
            "copyrightText": "NOASSERTION",
            "filesAnalyzed": bool(package_files),
            "packageComment": (
                f"Inventory component {component_id}; audit_status remains incomplete. "
                "Source, notice, and redistribution review is maintained separately."
            ),
        }
        if package_files:
            package_row["hasFiles"] = sorted(package_files)
        packages.append(package_row)

    file_rows: list[dict[str, Any]] = []
    for row in files:
        file_rows.append({
            "SPDXID": file_ids[_file_identity(row)],
            "fileName": row["path"],
            "checksums": [{"algorithm": "SHA256", "checksumValue": row["sha256"]}],
            "licenseConcluded": "NOASSERTION",
            "licenseInfoInFiles": ["NOASSERTION"],
            "copyrightText": "NOASSERTION",
            "fileComment": f"Inventory file kind: {row['kind']}; package source path is relative.",
        })

    relationships: list[dict[str, str]] = []
    for component_id in sorted(package_ids, key=lambda value: (value.casefold(), value)):
        relationships.append({"spdxElementId": "SPDXRef-DOCUMENT",
                              "relationshipType": "DESCRIBES",
                              "relatedSpdxElement": package_ids[component_id]})
        for file_id in sorted(file_ids[key] for key in memberships.get(component_id, [])):
            relationships.append({"spdxElementId": package_ids[component_id],
                                  "relationshipType": "CONTAINS",
                                  "relatedSpdxElement": file_id})

    module_components: dict[str, str] = {}
    binaries = inventory.get("binaries", [])
    if isinstance(binaries, list):
        for binary in binaries:
            if isinstance(binary, Mapping):
                name = binary.get("name")
                owner = binary.get("component_id")
                if isinstance(name, str) and isinstance(owner, str):
                    module_components[name.casefold()] = owner
    dependency_pairs: set[tuple[str, str]] = set()
    imports = inventory.get("runtime_imports", [])
    _require(isinstance(imports, list), "inventory runtime_imports must be a list")
    for index, item in enumerate(imports):
        _require(isinstance(item, Mapping), f"inventory.runtime_imports[{index}] must be an object")
        source_name = item.get("from")
        target_component = item.get("component_id")
        if not isinstance(source_name, str) or not isinstance(target_component, str):
            continue
        source_component = module_components.get(source_name.casefold())
        if (source_component in package_ids and target_component in package_ids and
                source_component != target_component):
            dependency_pairs.add((source_component, target_component))
    for source_component, target_component in sorted(dependency_pairs,
                                                      key=lambda pair: (pair[0].casefold(), pair[1].casefold())):
        relationships.append({"spdxElementId": package_ids[source_component],
                              "relationshipType": "DEPENDS_ON",
                              "relatedSpdxElement": package_ids[target_component]})

    generated = inventory.get("generated_utc")
    created = generated.strip() if isinstance(generated, str) and generated.strip() else "2000-01-01T00:00:00Z"
    document: dict[str, Any] = {
        "spdxVersion": SPDX_VERSION,
        "dataLicense": DATA_LICENSE,
        "SPDXID": "SPDXRef-DOCUMENT",
        "name": _text(document_name, "document_name"),
        "documentNamespace": f"https://property-studio.invalid/spdx/distribution/{inventory_digest}",
        "creationInfo": {
            "created": created,
            "creators": ["Tool: Property Studio distribution SBOM exporter"],
        },
        "documentComment": (
            "Generated from distribution-inventory.json; audit_status is incomplete. "
            f"Source inventory SHA-256: {inventory_digest}. "
            "Windows system imports and API contracts remain explicit external boundaries."
        ),
        "packages": packages,
        "files": file_rows,
        "relationships": relationships,
    }
    validate_sbom(document)
    return document


def validate_sbom(document: Mapping[str, Any]) -> None:
    """Validate the structural and hash contract of one generated SPDX document."""

    _require(isinstance(document, Mapping), "SBOM must be an object")
    _require(document.get("spdxVersion") == SPDX_VERSION, "SBOM spdxVersion must be SPDX-2.3")
    _require(document.get("dataLicense") == DATA_LICENSE, "SBOM dataLicense must be CC0-1.0")
    _require(document.get("SPDXID") == "SPDXRef-DOCUMENT", "SBOM document SPDXID is invalid")
    _text(document.get("name"), "SBOM name")
    namespace = _text(document.get("documentNamespace"), "SBOM documentNamespace")
    _require(namespace.startswith("https://property-studio.invalid/spdx/distribution/"),
             "SBOM documentNamespace is not repository-owned")
    creation = document.get("creationInfo")
    _require(isinstance(creation, Mapping), "SBOM creationInfo must be an object")
    _text(creation.get("created"), "SBOM creationInfo.created")
    creators = creation.get("creators")
    _require(isinstance(creators, list) and all(isinstance(item, str) and item.strip() for item in creators),
             "SBOM creationInfo.creators must be nonempty strings")
    packages = document.get("packages")
    files = document.get("files")
    relationships = document.get("relationships")
    _require(isinstance(packages, list), "SBOM packages must be a list")
    _require(isinstance(files, list), "SBOM files must be a list")
    _require(isinstance(relationships, list), "SBOM relationships must be a list")
    ids = {"SPDXRef-DOCUMENT"}
    package_ids: set[str] = set()
    for index, package in enumerate(packages):
        field = f"SBOM packages[{index}]"
        _require(isinstance(package, Mapping), f"{field} must be an object")
        identifier = _text(package.get("SPDXID"), f"{field}.SPDXID")
        _require(identifier not in ids, f"SBOM repeats SPDXID {identifier}")
        ids.add(identifier)
        package_ids.add(identifier)
        _text(package.get("name"), f"{field}.name")
        _text(package.get("versionInfo"), f"{field}.versionInfo")
        _text(package.get("licenseDeclared"), f"{field}.licenseDeclared")
        _require(package.get("filesAnalyzed") in (True, False), f"{field}.filesAnalyzed must be boolean")
        if package.get("hasFiles") is not None:
            _require(isinstance(package["hasFiles"], list), f"{field}.hasFiles must be a list")
    file_ids: set[str] = set()
    for index, item in enumerate(files):
        field = f"SBOM files[{index}]"
        _require(isinstance(item, Mapping), f"{field} must be an object")
        identifier = _text(item.get("SPDXID"), f"{field}.SPDXID")
        _require(identifier not in ids, f"SBOM repeats SPDXID {identifier}")
        ids.add(identifier)
        file_ids.add(identifier)
        _path(item.get("fileName"), f"{field}.fileName")
        checksums = item.get("checksums")
        _require(isinstance(checksums, list) and checksums, f"{field}.checksums must be nonempty")
        sha_values = [checksum.get("checksumValue") for checksum in checksums
                      if isinstance(checksum, Mapping) and checksum.get("algorithm") == "SHA256"]
        _require(len(sha_values) == 1, f"{field} must contain one SHA256 checksum")
        _sha256(sha_values[0], f"{field}.SHA256")
    for index, relation in enumerate(relationships):
        field = f"SBOM relationships[{index}]"
        _require(isinstance(relation, Mapping), f"{field} must be an object")
        source = _text(relation.get("spdxElementId"), f"{field}.spdxElementId")
        target = _text(relation.get("relatedSpdxElement"), f"{field}.relatedSpdxElement")
        _require(source in ids and target in ids, f"{field} references an unknown SPDXID")
        _text(relation.get("relationshipType"), f"{field}.relationshipType")
    for package in packages:
        for identifier in package.get("hasFiles", []):
            _require(identifier in file_ids, "SBOM package hasFiles references an unknown file")


def load_inventory(path: pathlib.Path | str) -> tuple[dict[str, Any], str]:
    input_path = pathlib.Path(path)
    try:
        payload = input_path.read_bytes()
        inventory = json.loads(payload.decode("utf-8"))
    except (OSError, UnicodeError, json.JSONDecodeError) as exc:
        raise SbomError(f"could not read inventory {input_path}: {exc}") from exc
    _require(isinstance(inventory, dict), "inventory JSON must contain an object")
    return inventory, hashlib.sha256(payload).hexdigest()


def write_sbom(path: pathlib.Path | str, document: Mapping[str, Any]) -> None:
    validate_sbom(document)
    output = pathlib.Path(path)
    try:
        output.parent.mkdir(parents=True, exist_ok=True)
        output.write_text(json.dumps(document, ensure_ascii=False, indent=2, sort_keys=True) + "\n",
                          encoding="utf-8", newline="\n")
    except OSError as exc:
        raise SbomError(f"could not write SBOM {output}: {exc}") from exc


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--inventory", required=True, type=pathlib.Path,
                        help="generated distribution inventory JSON")
    parser.add_argument("--output", default=DEFAULT_OUTPUT, type=pathlib.Path,
                        help=f"SPDX JSON output path (default: {DEFAULT_OUTPUT})")
    parser.add_argument("--document-name", default=DEFAULT_DOCUMENT_NAME)
    args = parser.parse_args(argv)
    try:
        inventory, digest = load_inventory(args.inventory)
        write_sbom(args.output, build_sbom(inventory, digest, document_name=args.document_name))
    except (SbomError, OSError, ValueError) as exc:
        print(f"distribution SBOM: {exc}", file=sys.stderr)
        return 1
    print(f"Wrote SPDX 2.3 SBOM to {args.output}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
