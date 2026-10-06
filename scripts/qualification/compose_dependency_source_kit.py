"""Compose declared dependency inputs offline; copying never qualifies source.

Schema 1 output contains a source-report receipt, verified candidate bindings,
explicit component records, and a sorted SHA256/byte-bound payload table. Paths
are relative to the supplied workspace (source_path) or output (payload_path).
Shared source identities have one payload and multiple owners/roles. Vertex/CLI
workspace sources reference the enclosing project source kit instead of copying
it again. Missing-source statuses and remaining obligations are retained.

No network, archive extraction, dependency execution or build is performed.
"""
from __future__ import annotations

import argparse
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import re
import shutil
import stat
import sys
import tempfile
from urllib.parse import urlparse

sys.path.insert(0, str(Path(__file__).resolve().parent))
import dependency_source_closure as closure
import native_source_inputs as native

_SOURCE_KIT_SPEC = importlib.util.spec_from_file_location(
    "vertex_source_kit_for_dependency_payload", Path(__file__).resolve().parent.parent / "source_kit_manifest.py")
project_manifest = importlib.util.module_from_spec(_SOURCE_KIT_SPEC)
_SOURCE_KIT_SPEC.loader.exec_module(project_manifest)

safe = closure.safe
require = closure.require
MANIFEST = "dependency-source-kit-manifest.json"
# Physical copied inputs and receipt replay have distinct bounded workloads.
# Replay covers a full frozen bundle plus dependency inputs and metadata.
MAX_PAYLOAD_FILES = 20_000
MAX_RECORDS = 65_536
MAX_TOTAL = 16 * 1024 * 1024 * 1024
MAX_INPUT_BYTES = 32 * 1024 * 1024 * 1024
MAX_JSON = 32 * 1024 * 1024
SOURCE_KINDS = {"vcpkg", "qt", "bootstrap", "workspace", "planegcs", "locked-archive", "redistributable", "controlled-runtime"}
SOURCE_STATUSES = {"exact_local_source_present", "missing_source", "distributed_asset_recorded", "redistributable_rights_review"}


def fields(value, allowed, required=()):
    require(isinstance(value, dict) and set(value) <= set(allowed) and set(required) <= set(value),
            "unknown or missing schema fields")
    return value


def text(value):
    require(isinstance(value, str) and len(value) <= 4096 and not any(ord(c) < 32 for c in value),
            "invalid metadata text")
    require(not re.search(r"(?<![A-Za-z])[A-Za-z]:[/\\]|\\\\", value) and not value.startswith("/"),
            "absolute machine path in metadata")
    require(not re.search(r"(?:^|[\s=\"'(])/(?!/)", value), "absolute machine path in metadata")
    return value


def url(value):
    if value is None:
        return None
    text(value)
    parsed = urlparse(value)
    require(parsed.scheme in {"https", "http", "git+https", "git"} and parsed.netloc
            and parsed.username is None and parsed.password is None, "invalid source URL")
    return value


def strings(values, *, paths=False):
    require(isinstance(values, list) and len(values) <= MAX_RECORDS, "invalid metadata list")
    result = [(safe.relative_path(v) if paths else text(v)) for v in values]
    if paths:
        safe.check_names(result)
    return result


def table(values, *, limit=None):
    require(isinstance(values, list) and len(values) <= (MAX_RECORDS if limit is None else limit),
            "invalid record table")
    safe.check_names([fields(row, row.keys(), ("path",))["path"] for row in values if isinstance(row, dict)])
    require(all(isinstance(row, dict) for row in values), "invalid record")
    return values


def directory_files(root, name):
    """Use the same bounded/stable SDK tree enumeration as the source audit."""
    return closure.source_tree_files(root, name)


def copy_file(root, receipt, destination):
    """Stream hash-bound bytes; never load a source archive into memory."""
    source = closure.checked_path(root, receipt["path"])
    before = source.lstat()
    require(stat.S_ISREG(before.st_mode) and before.st_size <= closure.MAX_FILE, "invalid copy source")
    safe.no_links(destination)
    destination.parent.mkdir(parents=True, exist_ok=True)
    digest = hashlib.sha256()
    count = 0
    with source.open("rb") as incoming, destination.open("xb") as outgoing:
        require(safe.stamp(os.fstat(incoming.fileno())) == safe.stamp(before), "source changed before copy")
        while chunk := incoming.read(1024 * 1024):
            count += len(chunk)
            require(count <= closure.MAX_FILE, "copy bytes exceed bound")
            digest.update(chunk)
            outgoing.write(chunk)
        outgoing.flush()
        os.fsync(outgoing.fileno())
        require(safe.stamp(os.fstat(incoming.fileno())) == safe.stamp(before), "source changed during copy")
    safe.no_links(source)
    require(safe.stamp(source.lstat()) == safe.stamp(before), "source changed after copy")
    require(digest.hexdigest() == receipt["sha256"] and count == receipt["bytes"], "copy hash/bytes differ")


def publish_directory(stage, output):
    """Only replace absent/empty output; restore an empty target on failure."""
    safe.no_links(output)
    removed = False
    if output.exists():
        require(output.is_dir() and not any(output.iterdir()), "output must be empty")
        output.rmdir()
        removed = True
    try:
        os.replace(stage, output)
    except BaseException:
        if removed and not output.exists():
            output.mkdir()
        raise


class Composer:
    def __init__(self, root, output):
        self.root, self.output = root, output
        self.payload = {}
        self.identities = {}
        self.names = {}
        self.receipts = {}
        self.trees = {}
        self.payload_trees = {}
        self.project_prefix = None
        self.project_rows = None
        self.total = 0
        self.input_total = 0
        self.records = 0
        self.native_binding = None
        self.native_targets = None
        self.native_inventory = None

    def resolve_record(self, row, owner, role):
        self.record(row, owner, role)
        return closure.checked_path(self.root, row["path"])

    def native_component(self, item):
        descriptor = item["native_source_inputs"]
        fields(descriptor, {"evidence_kind", "input_receipt", "contribution_sha256", "dependency_dispositions",
                            "target_bindings", "consumed_inputs"},
               {"evidence_kind", "input_receipt", "contribution_sha256", "dependency_dispositions",
                "target_bindings", "consumed_inputs"})
        require(self.native_binding is not None, "native source evidence requires candidate binding")
        owner = item["id"]
        if self.native_targets is None:
            binding_reader = native.Reader(lambda row, role: self.resolve_record(row, None, role))
            inventory = binding_reader.json(native.original(self.native_binding["inventory"]), "inventory")
            self.native_inventory = inventory
            self.native_targets = native.targets(inventory, native.original(self.native_binding["binaries"]))
            binding_reader.verify()
        package = native.applicability(descriptor["evidence_kind"], owner, self.native_inventory,
                                       self.native_targets.get(owner, []))
        require(package == item["package"] and item["declared_source_kind"] == "locked-archive",
                "native parent report package/source differs from inventory")
        consumed = table(descriptor["consumed_inputs"], limit=4096)
        locations = {row.get("inventory_path", row["path"]): row for row in consumed}
        def resolve(row, role):
            bound = locations.get(row["path"])
            require(bound is not None and native.original(bound) == row,
                    "native consumed input is missing or changed")
            return self.resolve_record(bound, owner, role)
        reader = native.Reader(resolve)
        contribution = native.normalize(descriptor["evidence_kind"], owner,
                                        native.original(descriptor["input_receipt"]), reader,
                                        self.native_targets.get(owner, []))
        native.applicability(descriptor["evidence_kind"], owner, self.native_inventory,
                             self.native_targets.get(owner, []), contribution["target_bindings"])
        expected = native.descriptor(descriptor["evidence_kind"], native.original(descriptor["input_receipt"]),
                                     contribution, reader.consumed)
        require(native.original(descriptor) == expected, "native source descriptor differs from original evidence")
        native.validate_contribution(item, contribution)
        reader.verify()
        result = dict(expected)
        result["input_receipt"] = self.record(descriptor["input_receipt"], owner, "native_input")
        # These receipts are explicit portable mappings for the next replay.
        result["consumed_inputs"] = [self.record(locations[row["path"]], owner, "native_consumed")
                                     for row in expected["consumed_inputs"]]
        return result

    def record(self, row, owner, role, *, copy=True, project=False, extra=()):
        fields(row, {"path", "sha256", "sha512", "bytes", "inventory_path", "kind", *extra}, ("path", "sha256"))
        self.records += 1
        require(self.records <= MAX_RECORDS, "record count exceeds bound")
        location = safe.relative_path(row["path"])
        identity = safe.relative_path(row.get("inventory_path", location))
        # A source tree containing output/staging would change during composition.
        target = closure.checked_path(self.root, location)
        require(target != self.output and target not in self.output.parents, "input overlaps output")
        existing = self.identities.get(identity)
        kind = row.get("kind", "file")
        require(existing is None or existing == (row["sha256"], kind), "conflicting shared source hash/kind")
        parts = identity.split("/")
        for index in range(1, len(parts) + 1):
            spelling = "/".join(parts[:index])
            entry = (spelling, "directory" if index < len(parts) else kind)
            require(self.names.get(spelling.casefold(), entry) == entry, "ancestor/case-colliding source identity")
            self.names[spelling.casefold()] = entry
        self.identities[identity] = (row["sha256"], kind)
        if row.get("kind") == "directory":
            require(role in {"source", "sdk_source"}, "directory receipt is only admitted as source input")
            require(not ({"bytes", "sha512"} & set(row)), "invalid directory receipt")
            children = directory_files(self.root, location)
            if project and self.project_rows is not None:
                selected = {name for name in self.project_rows if name.startswith(identity + "/")}
                actual = {identity + "/" + name[len(location) + 1:] for name in children}
                require(selected == actual, "project tree differs from selected source kit")
            require(closure.source_tree_hash(self.root, location) == row["sha256"], "source tree hash differs")
            self.trees[location] = row["sha256"]
            result = {"source_path": identity, "sha256": row["sha256"], "kind": "directory"}
            if project:
                result["project_source_kit_path"] = identity
            else:
                result["payload_path"] = "inputs/" + identity
                self.payload_trees[result["payload_path"]] = row["sha256"]
                for child in children:
                    child_identity = identity + "/" + child[len(location) + 1:]
                    self.record({**closure.file_record(self.root, child), "inventory_path": child_identity}, owner, role)
            return result
        require(row.get("kind", "file") == "file", "unknown record kind")
        if location not in self.receipts:
            self.input_total += target.lstat().st_size
            require(self.input_total <= MAX_INPUT_BYTES, "input bytes exceed bound")
        actual = closure.file_record(self.root, location, row["sha256"])
        if "bytes" in row:
            require(type(row["bytes"]) is int and row["bytes"] == actual["bytes"], "record bytes differ")
        if "sha512" in row:
            closure.file_record(self.root, location, row["sha512"], algorithm="sha512")
        self.receipts[location] = actual
        result = {"source_path": identity, "sha256": actual["sha256"], "bytes": actual["bytes"]}
        if "sha512" in row:
            result["sha512"] = row["sha512"]
        if project:
            if self.project_rows is not None:
                selected = self.project_rows.get(identity)
                require(selected is not None and selected["sha256"].lower() == actual["sha256"]
                        and selected["size"] == actual["bytes"],
                        "project source differs from selected source kit")
            result["project_source_kit_path"] = identity
        elif copy:
            result["payload_path"] = "inputs/" + identity
            if identity not in self.payload:
                require(len(self.payload) < MAX_PAYLOAD_FILES, "payload file count exceeds bound")
                self.total += actual["bytes"]
                require(self.total <= MAX_TOTAL, "payload bytes exceed bound")
                self.payload[identity] = {**result, "path": result["payload_path"],
                                          "_receipt": actual, "component_ids": set(), "roles": set()}
            payload = self.payload[identity]
            if owner is not None:
                payload["component_ids"].add(owner)
            payload["roles"].add(role)
        return result

    def attach_source_kit(self, path, binding):
        """Bind an explicit first source kit without needing an existing bundle."""
        manifest, receipt = closure.json_input(self.root, path)
        project_manifest.validate_manifest(manifest)
        require(len(manifest["files"]) <= closure.MAX_BUNDLE_FILES, "selected source kit file count exceeds bound")
        self.project_rows = {safe.relative_path(row["path"]): row for row in manifest["files"]}
        for identity, row in self.project_rows.items():
            self.record({"path": identity, "sha256": row["sha256"].lower(), "bytes": row["size"]},
                        None, "source", project=True)
        frozen = binding["offline_bundle"]
        if frozen is not None:
            require(frozen["source_kit"]["sha256"] == receipt["sha256"]
                    and frozen["source_kit"]["bytes"] == receipt["bytes"],
                    "selected source kit differs from frozen candidate binding")
        binding["source_kit"] = self.record(receipt, None, "source_kit")

    def provenance(self, value, owner):
        if value is None:
            return None
        if isinstance(value, dict) and "path" in value:
            return self.record(value, owner, "provenance")
        fields(value, {"spdx", "installed_status", "declared_recipe_origin", "sbom", "declared_revision_locator"})
        result = {}
        for key, item in value.items():
            result[key] = url(item) if key.startswith("declared_") else (
                self.record(item, owner, "provenance") if item is not None else None)
        return result

    def source(self, source, owner, project, workspace):
        fields(source, {"url", "status", "local_files", "declared_checksum", "provenance", "name",
                        "provenance_url", "binding_role", "missing_or_changed_paths"}, ("url", "status", "local_files"))
        require(source["status"] in {"exact_local_source_present", "missing_source"}, "unknown source status")
        result = {"url": url(source["url"]), "status": source["status"]}
        rows = table(source["local_files"])
        if workspace and self.project_prefix is not None:
            mapped = []
            for row in rows:
                require(row["path"].startswith(self.project_prefix + "/"), "workspace source differs from frozen source location")
                original = row["path"][len(self.project_prefix) + 1:]
                require(row.get("inventory_path", original) == original, "workspace original source identity differs")
                mapped.append({**row, "inventory_path": original})
            rows = mapped
        result["local_files"] = [self.record(row, owner, "source", project=project) for row in rows]
        require(source["status"] != "exact_local_source_present" or bool(rows), "exact source status without local source")
        if "declared_checksum" in source:
            checksum = fields(source["declared_checksum"], {"algorithm", "value"}, ("algorithm", "value"))
            require(checksum["algorithm"] in {"sha256", "sha512"}, "unknown checksum algorithm")
            require(isinstance(checksum["value"], str) and re.fullmatch(
                r"[0-9a-f]{" + str(64 if checksum["algorithm"] == "sha256" else 128) + "}", checksum["value"]), "invalid source checksum")
            for row in result["local_files"]:
                require(row.get(checksum["algorithm"]) == checksum["value"], "declared source checksum differs")
            result["declared_checksum"] = dict(checksum)
        for key in ("name", "binding_role"):
            if key in source:
                result[key] = text(source[key]) if source[key] is not None else None
        if "provenance_url" in source:
            result["provenance_url"] = url(source["provenance_url"])
        if "provenance" in source:
            result["provenance"] = self.provenance(source["provenance"], owner)
        if "missing_or_changed_paths" in source:
            result["missing_or_changed_paths"] = strings(source["missing_or_changed_paths"], paths=True)
        return result

    def component(self, item):
        fields(item, {"id", "package", "declared_source_kind", "sources", "notices", "assets", "artifacts",
                      "source_status", "recipe", "remaining", "licensing_clearance", "corresponding_source_qualified",
                      "provenance", "configuration", "upstream_asset", "binary_archive", "upstream_metadata", "native_source_inputs"},
               ("id", "package", "declared_source_kind", "sources", "notices", "assets", "artifacts", "source_status", "recipe", "remaining"))
        owner = item["id"]
        require(item["declared_source_kind"] in SOURCE_KINDS and item["source_status"] in SOURCE_STATUSES, "unknown component source kind/status")
        require(item.get("licensing_clearance") is False and item.get("corresponding_source_qualified") is False,
                "closure cannot confer qualification")
        package = fields(item["package"], {"name", "version", "license"}, ("name", "version", "license"))
        result = {"id": owner, "package": {k: text(v) for k, v in package.items()},
                  "declared_source_kind": item["declared_source_kind"], "source_status": item["source_status"],
                  "remaining": strings(item["remaining"]), "licensing_clearance": False, "corresponding_source_qualified": False}
        if "native_source_inputs" in item:
            result["native_source_inputs"] = self.native_component(item)
        project = owner in {"vertex", "cli"} and item["declared_source_kind"] == "workspace"
        require(isinstance(item["sources"], list) and len(item["sources"]) <= 512, "invalid source table")
        result["sources"] = [self.source(source, owner, project, item["declared_source_kind"] == "workspace") for source in item["sources"]]
        for key in ("notices", "assets", "artifacts"):
            rows = []
            for row in table(item[key]):
                role = "sdk_source" if key == "assets" and row.get("kind") == "directory" else key
                record = self.record(row, owner, role, extra={"content_role"} if key == "notices" else ())
                if "content_role" in row:
                    record["content_role"] = text(row["content_role"])
                rows.append(record)
            result[key] = rows
        recipe = item["recipe"]
        result["recipe"] = None
        if recipe is not None:
            fields(recipe, {"status", "files", "recipe_options", "reason", "options_role"}, ("status", "files", "recipe_options"))
            require(recipe["status"] in {"exact_local_recipe_present", "missing_or_stale_recipe"}, "unknown recipe status")
            result["recipe"] = {"status": recipe["status"], "files": [self.record(row, owner, "recipe") for row in table(recipe["files"])],
                                "recipe_options": strings(recipe["recipe_options"])}
            for key in ("reason", "options_role"):
                if key in recipe:
                    result["recipe"][key] = text(recipe[key])
        if "provenance" in item:
            result["provenance"] = self.provenance(item["provenance"], owner)
        if "configuration" in item:
            configuration = item["configuration"]
            result["configuration"] = self.record(configuration, owner, "configuration", extra={"observed_variables", "role"})
            variables = configuration["observed_variables"]
            require(isinstance(variables, dict) and len(variables) <= 128 and all(re.fullmatch(r"[A-Za-z0-9_.]+", k) for k in variables), "invalid configuration variables")
            result["configuration"].update(observed_variables={k: strings(v) for k, v in variables.items()}, role=text(configuration["role"]))
        if "binary_archive" in item:
            result["binary_archive"] = self.record(item["binary_archive"], owner, "binary_archive")
        if "upstream_asset" in item:
            asset = item["upstream_asset"]
            result["upstream_asset"] = self.record(asset, owner, "upstream_asset", extra={"url", "provenance", "binding_role"})
            result["upstream_asset"].update(url=url(asset["url"]), provenance=self.provenance(asset["provenance"], owner), binding_role=text(asset["binding_role"]))
        if "upstream_metadata" in item:
            metadata = fields(item["upstream_metadata"], {"metadata_sha256", "metadata_url", "project", "source_distributions", "status", "version"},
                              ("metadata_sha256", "metadata_url", "project", "source_distributions", "status", "version"))
            require(re.fullmatch(r"[0-9a-f]{64}", metadata["metadata_sha256"]) and metadata["status"] == "observed", "invalid metadata receipt")
            distributions = []
            require(isinstance(metadata["source_distributions"], list) and len(metadata["source_distributions"]) <= 512, "invalid source distributions")
            for entry in metadata["source_distributions"]:
                fields(entry, {"filename", "sha256", "size", "url"}, ("filename", "sha256", "size", "url"))
                require(re.fullmatch(r"[0-9a-f]{64}", entry["sha256"]) and type(entry["size"]) is int and 0 <= entry["size"] <= closure.MAX_FILE, "invalid upstream archive receipt")
                distributions.append({"filename": safe.relative_path(entry["filename"]), "sha256": entry["sha256"], "size": entry["size"], "url": url(entry["url"])})
            safe.check_names([entry["filename"] for entry in distributions])
            result["upstream_metadata"] = {**{k: text(metadata[k]) for k in ("project", "version", "status", "metadata_sha256")},
                                           "metadata_url": url(metadata["metadata_url"]), "source_distributions": distributions}
        return result

    def binding(self, binding, owners):
        fields(binding, {"inventory", "component_manifest", "runtime", "binaries", "build_receipts", "upstream_metadata", "offline_bundle"},
               ("inventory", "component_manifest", "runtime", "binaries", "build_receipts", "upstream_metadata", "offline_bundle"))
        self.native_binding = binding
        result = {key: self.record(binding[key], None, key) for key in ("inventory", "component_manifest", "runtime")}
        result["binaries"] = []
        for row in table(binding["binaries"]):
            require(row.get("component_id") in owners, "unknown binary owner")
            result["binaries"].append({"component_id": row["component_id"], **self.record(row, row["component_id"], "binary", copy=False, extra={"component_id"})})
        result["build_receipts"] = [self.record(row, None, "build_receipt") for row in table(binding["build_receipts"])]
        result["upstream_metadata"] = self.record(binding["upstream_metadata"], None, "upstream_metadata") if binding["upstream_metadata"] is not None else None
        result["offline_bundle"] = None
        if binding["offline_bundle"] is not None:
            bundle = fields(binding["offline_bundle"], {"manifest", "payload", "portable_manifest", "runtime_manifest", "source_inventory", "source_kit", "source_location"},
                            ("manifest", "payload", "portable_manifest", "runtime_manifest", "source_inventory", "source_kit", "source_location"))
            result["offline_bundle"] = {key: self.record(bundle[key], None, "offline_bundle_receipt")
                                        for key in ("manifest", "portable_manifest", "runtime_manifest", "source_inventory", "source_kit")}
            result["offline_bundle"]["source_location"] = safe.relative_path(bundle["source_location"])
            self.project_prefix = result["offline_bundle"]["source_location"]
            result["offline_bundle"]["payload"] = [self.record(row, None, "frozen_payload", copy=False)
                                                   for row in table(bundle["payload"], limit=closure.MAX_BUNDLE_FILES)]
        return result

    def verify(self):
        for row in self.receipts.values():
            closure.file_record(self.root, row["path"], row["sha256"])
        for location, expected in self.trees.items():
            directory_files(self.root, location)
            require(closure.source_tree_hash(self.root, location) == expected, "source tree hash differs after composition")


def compose(workspace: Path, report_path: str, output_root: str, *,
            source_kit_manifest: str | None = None) -> dict:
    """Publish a deterministic new/empty directory; return the portable manifest."""
    root = Path(workspace).absolute()
    safe.no_links(root)
    require(root.is_dir(), "workspace must be a directory")
    output = closure.checked_path(root, output_root)
    require(output.parent.is_dir(), "output parent must exist")
    require(not output.exists() or (output.is_dir() and not any(output.iterdir())), "output must be new or empty")
    document, report_receipt = closure.json_input(root, report_path)
    fields(document, {"schema_version", "audit_kind", "candidate_binding", "components", "licensing_clearance", "corresponding_source_qualified", "boundary", "summary"},
           ("schema_version", "audit_kind", "candidate_binding", "components", "licensing_clearance", "corresponding_source_qualified"))
    require(type(document["schema_version"]) is int and document["schema_version"] == 1
            and document["audit_kind"] == "candidate_dependency_source_closure", "unsupported source closure report")
    require(document["licensing_clearance"] is False and document["corresponding_source_qualified"] is False, "closure cannot confer qualification")
    items = document["components"]
    require(isinstance(items, list) and 0 < len(items) <= 512, "invalid component table")
    owners = [row["id"] for row in items]
    require(all(isinstance(owner, str) and re.fullmatch(r"[a-z0-9][a-z0-9-]*", owner) for owner in owners), "invalid component ID")
    require(len(set(owners)) == len(owners), "duplicate component IDs")
    composer = Composer(root, output)
    source_report = composer.record(report_receipt, None, "source_report", copy=False)
    binding = composer.binding(document["candidate_binding"], owners)
    if source_kit_manifest is not None:
        composer.attach_source_kit(source_kit_manifest, binding)
    components = [composer.component(item) for item in sorted(items, key=lambda row: row["id"])]
    payload = []
    for identity, row in sorted(composer.payload.items()):
        payload.append({key: (sorted(value) if isinstance(value, set) else value)
                        for key, value in row.items() if key not in {"_receipt", "payload_path"}})
    safe.check_names([MANIFEST, *[row["path"] for row in payload]])
    result = {"schema_version": 1, "payload_kind": "candidate_dependency_source_payload", "source_report": source_report,
              "candidate_binding": binding, "components": components, "files": payload,
              "licensing_clearance": False, "corresponding_source_qualified": False, "offline_rebuild_qualified": False,
              "boundary": "Verified copies of explicitly declared local inputs only; unresolved source, notice, licensing and rebuild obligations remain."}
    encoded = (json.dumps(result, sort_keys=True, indent=2) + "\n").encode("utf-8")
    require(len(encoded) <= MAX_JSON, "manifest bytes exceed bound")
    stage = Path(tempfile.mkdtemp(prefix="." + output.name + ".", suffix=".tmp", dir=output.parent))
    try:
        for name in sorted(composer.payload_trees):
            (stage / name).mkdir(parents=True, exist_ok=True)
        for identity, row in sorted(composer.payload.items()):
            copy_file(root, row["_receipt"], stage / row["path"])
        with (stage / MANIFEST).open("xb") as stream:
            stream.write(encoded)
            stream.flush()
            os.fsync(stream.fileno())
        for row in payload:
            closure.file_record(stage, row["path"], row["sha256"])
        for name, expected in composer.payload_trees.items():
            require(closure.source_tree_hash(stage, name) == expected, "staged source tree hash differs")
        composer.verify()
        publish_directory(stage, output)
    finally:
        if stage.exists():
            # This is the task-created sibling, never an input or published output.
            require(stage.parent == output.parent and root in stage.parents and stage.name.startswith("." + output.name + "."), "unsafe staging cleanup target")
            safe.no_links(stage)
            shutil.rmtree(stage)
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--workspace", type=Path, required=True)
    parser.add_argument("--report", required=True, help="Workspace-relative schema 1 source closure report.")
    parser.add_argument("--output-root", required=True, help="Workspace-relative new/empty dependency payload directory; parent must exist.")
    parser.add_argument("--source-kit-manifest", help="Explicit workspace-relative source-kit manifest to verify and bind before first bundle staging.")
    args = parser.parse_args()
    try:
        manifest = compose(args.workspace, args.report, args.output_root,
                           source_kit_manifest=args.source_kit_manifest)
        manifest_receipt = closure.file_record(args.workspace.absolute(), args.output_root + "/" + MANIFEST)
        print(json.dumps({"manifest": MANIFEST, "sha256": manifest_receipt["sha256"], "bytes": manifest_receipt["bytes"],
                          "files": len(manifest["files"]), "corresponding_source_qualified": False,
                          "licensing_clearance": False, "offline_rebuild_qualified": False}, sort_keys=True))
        return 0
    except (ValueError, OSError, KeyError, TypeError) as error:
        print(f"dependency source payload: {error}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
