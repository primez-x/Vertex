"""Dependency payload copying is receipt binding, never source qualification."""
import copy
from contextlib import redirect_stderr, redirect_stdout
import hashlib
import importlib.util
import io
import json
from pathlib import Path
import stat
import tempfile
from types import SimpleNamespace
import unittest
from unittest import mock

ROOT = Path(__file__).resolve().parents[1]
SPEC = importlib.util.spec_from_file_location("composer", ROOT / "scripts/qualification/compose_dependency_source_kit.py")
composer = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(composer)


def tree_v2_digest(files):
    value = hashlib.sha256(b"Vertex-source-tree-v2\0")
    value.update(len(files).to_bytes(8, "big"))
    for name, content in sorted(files.items()):
        path = name.encode("utf-8")
        value.update(len(path).to_bytes(8, "big"))
        value.update(path)
        value.update(len(content).to_bytes(8, "big"))
        value.update(hashlib.sha256(content).digest())
    return value.hexdigest()


class ComposerTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        for name, data in (("candidate/app.dll", b"binary"), ("cache/source.tar.xz", b"archive"),
                           ("recipe/portfile.cmake", b"configure()"), ("recipe/fix.patch", b"patch"),
                           ("notices/license.txt", b"Copyright Permission is hereby granted"),
                           ("receipts/inventory.json", b"{}"), ("receipts/components.json", b"{}"),
                           ("receipts/runtime.json", b"{}"), ("receipts/spdx.json", b"{}"),
                           ("build/CMakeCache.txt", b"COMPILER=C:/private/compiler.exe")):
            self.write(name, data)
        self.report = {
            "schema_version": 1, "audit_kind": "candidate_dependency_source_closure",
            "licensing_clearance": False, "corresponding_source_qualified": False,
            "boundary": "availability only", "summary": {},
            "candidate_binding": {
                "inventory": self.record("receipts/inventory.json"),
                "component_manifest": self.record("receipts/components.json"),
                "runtime": self.record("receipts/runtime.json"),
                "binaries": [{"component_id": "dependency", **self.record("candidate/app.dll")}],
                "build_receipts": [self.record("build/CMakeCache.txt")],
                "upstream_metadata": None, "offline_bundle": None},
            "components": [{
                "id": "dependency", "package": {"name": "Dependency", "version": "1", "license": "MIT"},
                "declared_source_kind": "vcpkg", "source_status": "exact_local_source_present",
                "sources": [{"url": "https://example.invalid/source.tar.xz",
                    "status": "exact_local_source_present", "declared_checksum": {
                        "algorithm": "sha256", "value": self.record("cache/source.tar.xz")["sha256"]},
                    "local_files": [self.record("cache/source.tar.xz")]}],
                "recipe": {"status": "exact_local_recipe_present", "recipe_options": [],
                           "files": [self.record("recipe/portfile.cmake"), self.record("recipe/fix.patch")]},
                "notices": [{**self.record("notices/license.txt"), "content_role": "notice_text_present_review_required"}],
                "assets": [], "artifacts": [], "provenance": {"spdx": self.record("receipts/spdx.json"),
                    "installed_status": None, "declared_recipe_origin": "https://example.invalid/recipe"},
                "remaining": ["build_and_relinking_evidence_not_qualified"],
                "licensing_clearance": False, "corresponding_source_qualified": False}]}

    def write(self, name, data):
        path = self.root / name
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(data)

    def record(self, name):
        data = (self.root / name).read_bytes()
        return {"path": name, "sha256": hashlib.sha256(data).hexdigest(), "bytes": len(data)}

    def run_compose(self, output="out", report=None):
        self.write("report.json", (json.dumps(report or self.report) + "\n").encode())
        return composer.compose(self.root, "report.json", output)

    def test_declared_sdk_source_input_directory_is_delivered_with_explicit_role(self):
        self.write("sdk/include/unit.hpp", b"header source")
        row = {"path": "sdk/include", "kind": "directory",
               "sha256": tree_v2_digest({"unit.hpp": b"header source"})}
        self.report["components"][0]["assets"] = [row]
        with mock.patch.object(composer.closure, "MAX_FILES", 1):
            result = self.run_compose()
        delivered = result["components"][0]["assets"][0]
        self.assertEqual(delivered["kind"], "directory")
        self.assertEqual(delivered["sha256"], row["sha256"])
        self.assertEqual((self.root / "out/inputs/sdk/include/unit.hpp").read_bytes(), b"header source")
        payload = next(entry for entry in result["files"] if entry["source_path"] == "sdk/include/unit.hpp")
        self.assertEqual(payload["roles"], ["sdk_source"])
        self.assertFalse(result["offline_rebuild_qualified"])

    def test_sdk_source_input_drift_and_directory_file_only_roles_refuse_publication(self):
        self.write("sdk/include/unit.hpp", b"header source")
        row = {"path": "sdk/include", "kind": "directory",
               "sha256": tree_v2_digest({"unit.hpp": b"header source"})}
        self.report["components"][0]["assets"] = [row]
        original = composer.copy_file
        def drift(*args, **kwargs):
            result = original(*args, **kwargs)
            self.write("sdk/include/late.hpp", b"late addition")
            return result
        with mock.patch.object(composer, "copy_file", side_effect=drift):
            with self.assertRaisesRegex(ValueError, "tree hash"):
                self.run_compose()
        self.assertFalse((self.root / "out").exists())
        self.report["components"][0]["assets"] = []
        for field in ("notices", "artifacts"):
            with self.subTest(field=field):
                original_rows = self.report["components"][0][field]
                self.report["components"][0][field] = [row]
                with self.assertRaisesRegex(ValueError, "source input"):
                    self.run_compose()
                self.report["components"][0][field] = original_rows

    def test_ambiguous_tree_substitution_and_legacy_receipts_refuse_publication(self):
        files = {"a.hpp": b"A", "b.hpp": b"B"}
        row = {"path": "sdk/include", "kind": "directory", "sha256": tree_v2_digest(files)}
        self.report["components"][0]["assets"] = [row]
        self.write("sdk/include/a.hpp", b"A\0b.hpp\0B")
        with self.assertRaisesRegex(ValueError, "tree hash"):
            self.run_compose()
        self.assertFalse((self.root / "out").exists())
        row["sha256"] = hashlib.sha256(b"a.hpp\0A\0b.hpp\0B\0").hexdigest()
        with self.assertRaisesRegex(ValueError, "tree hash"):
            self.run_compose()
        self.assertFalse((self.root / "out").exists())

    def test_bound_payload_is_deterministic_and_keeps_obligations(self):
        first = self.run_compose("one")
        second = self.run_compose("two")
        self.assertEqual(first, second)
        self.assertEqual((self.root / "one" / composer.MANIFEST).read_bytes(),
                         (self.root / "two" / composer.MANIFEST).read_bytes())
        self.assertEqual((self.root / "one/inputs/cache/source.tar.xz").read_bytes(), b"archive")
        self.assertTrue((self.root / "one/inputs/recipe/fix.patch").is_file())
        self.assertFalse(first["licensing_clearance"])
        self.assertFalse(first["corresponding_source_qualified"])
        self.assertFalse(first["offline_rebuild_qualified"])
        self.assertEqual(first["components"][0]["remaining"], self.report["components"][0]["remaining"])
        self.assertNotIn("C:/private", json.dumps(first))
        self.assertNotIn(str(self.root), json.dumps(first))
        self.assertEqual(first["source_report"]["sha256"], self.record("report.json")["sha256"])

    def test_stale_source_binary_receipt_and_size_fail_without_output(self):
        for path in ("cache/source.tar.xz", "candidate/app.dll", "build/CMakeCache.txt"):
            with self.subTest(path=path):
                original = (self.root / path).read_bytes()
                self.write(path, b"drift")
                with self.assertRaisesRegex(ValueError, "hash"):
                    self.run_compose()
                self.assertFalse((self.root / "out").exists())
                self.write(path, original)
        self.report["components"][0]["sources"][0]["local_files"][0]["bytes"] += 1
        with self.assertRaisesRegex(ValueError, "bytes"):
            self.run_compose()

    def test_missing_source_and_recipe_obligations_survive(self):
        item = self.report["components"][0]
        item["source_status"] = "missing_source"
        item["sources"][0]["status"] = "missing_source"
        item["sources"][0]["local_files"] = []
        item["sources"][0]["missing_or_changed_paths"] = ["unavailable/source.cpp"]
        item["recipe"] = {"status": "missing_or_stale_recipe", "files": [], "recipe_options": [], "reason": "missing"}
        item["remaining"] += ["exact_corresponding_source_missing", "exact_historical_recipe_or_modifications_missing"]
        output = self.run_compose()
        self.assertEqual(output["components"][0]["source_status"], "missing_source")
        self.assertEqual(output["components"][0]["remaining"], item["remaining"])

    def test_shared_inputs_copy_once_with_both_owners(self):
        other = copy.deepcopy(self.report["components"][0])
        other["id"] = "other"
        self.report["components"].append(other)
        result = self.run_compose()
        records = [row for row in result["files"] if row["source_path"] == "cache/source.tar.xz"]
        self.assertEqual(len(records), 1)
        self.assertEqual(records[0]["component_ids"], ["dependency", "other"])

    def test_project_tree_verifies_without_copying_and_tree_exclusions_match(self):
        self.write("src/unit.cpp", b"unit")
        self.write("src/__pycache__/unit.pyc", b"cache")
        self.write("src/skip.PYO", b"cache")
        self.write("src/keep.pyd", b"retained")
        item = self.report["components"][0]
        item["id"] = "vertex"
        item["declared_source_kind"] = "workspace"
        self.report["candidate_binding"]["binaries"][0]["component_id"] = "vertex"
        tree = {"path": "src", "kind": "directory", "sha256": composer.closure.source_tree_hash(self.root, "src")}
        item["sources"] = [{"url": None, "status": "exact_local_source_present", "local_files": [tree], "missing_or_changed_paths": []}]
        result = self.run_compose()
        bound = result["components"][0]["sources"][0]["local_files"][0]
        self.assertEqual(bound["project_source_kit_path"], "src")
        self.assertFalse((self.root / "out/inputs/src").exists())
        self.write("src/unit.cpp", b"changed")
        with self.assertRaisesRegex(ValueError, "tree hash"):
            self.run_compose("next")

    def test_dependency_directory_copies_only_included_files(self):
        self.write("vendor/src/unit.cpp", b"unit")
        self.write("vendor/src/__pycache__/unit.pyc", b"cache")
        self.write("vendor/src/skip.PYO", b"cache")
        self.write("vendor/src/keep.pyd", b"retained")
        item = self.report["components"][0]
        item["declared_source_kind"] = "planegcs"
        item["sources"][0]["local_files"] = [{"path": "vendor/src", "kind": "directory", "sha256": composer.closure.source_tree_hash(self.root, "vendor/src")}]
        del item["sources"][0]["declared_checksum"]
        result = self.run_compose()
        self.assertTrue((self.root / "out/inputs/vendor/src/unit.cpp").is_file())
        self.assertTrue((self.root / "out/inputs/vendor/src/keep.pyd").is_file())
        self.assertFalse((self.root / "out/inputs/vendor/src/skip.PYO").exists())
        self.assertFalse((self.root / "out/inputs/vendor/src/__pycache__").exists())
        self.assertEqual(result["components"][0]["sources"][0]["local_files"][0]["kind"], "directory")

    def test_unsafe_paths_unknown_fields_duplicate_and_case_collisions_rejected(self):
        for change in ("traversal", "absolute", "case", "duplicate", "unknown", "unknown_record", "unknown_binary_owner"):
            with self.subTest(change=change):
                report = copy.deepcopy(self.report)
                files = report["components"][0]["recipe"]["files"]
                if change == "traversal": files[0]["path"] = "../escape"
                elif change == "absolute": files[0]["path"] = "C:/secret.txt"
                elif change == "case": files.append({**files[0], "path": "Recipe/portfile.cmake"})
                elif change == "duplicate": files.append(copy.deepcopy(files[0]))
                elif change == "unknown": report["components"][0]["secret"] = {"path": "secret"}
                elif change == "unknown_record": files[0]["secret"] = "C:/private"
                else: report["candidate_binding"]["binaries"][0]["component_id"] = "unknown"
                with self.assertRaises(ValueError):
                    self.run_compose(report=report)
                self.assertFalse((self.root / "out").exists())

    def test_links_in_included_excluded_and_output_paths_rejected(self):
        original = Path.lstat
        targets = [self.root / "cache/source.tar.xz", self.root / "out"]
        for target in targets:
            def fake(path, *args, **kwargs):
                if path == target:
                    return SimpleNamespace(st_mode=stat.S_IFLNK, st_file_attributes=0)
                return original(path, *args, **kwargs)
            with self.subTest(target=target.name), mock.patch.object(Path, "lstat", fake):
                with self.assertRaisesRegex(ValueError, "link|reparse"):
                    self.run_compose()

    def test_atomic_copy_and_publish_failures_preserve_existing_empty_output(self):
        (self.root / "out").mkdir()
        for method in ("copy_file", "publish_directory"):
            with self.subTest(method=method), mock.patch.object(composer, method, side_effect=OSError("simulated")):
                with self.assertRaisesRegex(OSError, "simulated"):
                    self.run_compose()
                self.assertTrue((self.root / "out").is_dir())
                self.assertEqual(list((self.root / "out").iterdir()), [])
                self.assertEqual(list(self.root.glob(".out.*.tmp")), [])

    def test_nonempty_output_is_preserved_and_output_outside_workspace_rejected(self):
        self.write("out/manual.txt", b"manual")
        with self.assertRaisesRegex(ValueError, "empty"):
            self.run_compose()
        self.assertEqual((self.root / "out/manual.txt").read_bytes(), b"manual")
        with self.assertRaises(ValueError):
            self.run_compose("../escape")

    def test_late_report_and_tree_drift_never_publish(self):
        real = composer.copy_file
        def drift(*args, **kwargs):
            result = real(*args, **kwargs)
            self.write("report.json", b"{}")
            return result
        with mock.patch.object(composer, "copy_file", side_effect=drift):
            with self.assertRaisesRegex(ValueError, "hash"):
                self.run_compose()
        self.assertFalse((self.root / "out").exists())

    def test_frozen_project_source_retains_original_identity(self):
        item = self.report["components"][0]
        item["id"] = "cli"
        item["declared_source_kind"] = "workspace"
        self.report["candidate_binding"]["binaries"][0]["component_id"] = "cli"
        self.write("bundle/source-kit/src/unit.cpp", b"frozen unit")
        tree = {"path": "bundle/source-kit/src", "kind": "directory", "sha256": composer.closure.source_tree_hash(self.root, "bundle/source-kit/src")}
        item["sources"] = [{"url": None, "status": "exact_local_source_present", "local_files": [tree], "missing_or_changed_paths": []}]
        binding = self.report["candidate_binding"]
        binding["offline_bundle"] = {key: self.record("receipts/runtime.json") for key in
            ("manifest", "portable_manifest", "runtime_manifest", "source_inventory", "source_kit")}
        binding["offline_bundle"].update(payload=[], source_location="bundle/source-kit")
        result = self.run_compose()
        source = result["components"][0]["sources"][0]["local_files"][0]
        self.assertEqual(source["source_path"], "src")
        self.assertEqual(source["project_source_kit_path"], "src")
        self.assertFalse((self.root / "out/inputs/src").exists())

    def test_empty_dependency_tree_remains_a_bound_directory(self):
        (self.root / "vendor/empty").mkdir(parents=True)
        item = self.report["components"][0]
        item["sources"] = [{"url": None, "status": "exact_local_source_present", "local_files": [{
            "path": "vendor/empty", "kind": "directory", "sha256": composer.closure.source_tree_hash(self.root, "vendor/empty")}]}]
        self.run_compose()
        self.assertTrue((self.root / "out/inputs/vendor/empty").is_dir())

    def test_actual_publish_failure_restores_empty_output(self):
        (self.root / "out").mkdir()
        with mock.patch.object(composer.os, "replace", side_effect=OSError("publication failure")):
            with self.assertRaisesRegex(OSError, "publication failure"):
                self.run_compose()
        self.assertTrue((self.root / "out").is_dir())
        self.assertEqual(list((self.root / "out").iterdir()), [])
        self.assertEqual(list(self.root.glob(".out.*.tmp")), [])

    def test_late_tree_addition_and_excluded_link_fail(self):
        self.write("vendor/src/unit.cpp", b"unit")
        self.write("vendor/src/__pycache__/skip.pyc", b"cache")
        item = self.report["components"][0]
        item["sources"] = [{"url": None, "status": "exact_local_source_present", "local_files": [{
            "path": "vendor/src", "kind": "directory", "sha256": composer.closure.source_tree_hash(self.root, "vendor/src")}]}]
        real = composer.copy_file
        def drift(*args, **kwargs):
            result = real(*args, **kwargs)
            self.write("vendor/src/new.cpp", b"late addition")
            return result
        with mock.patch.object(composer, "copy_file", side_effect=drift):
            with self.assertRaisesRegex(ValueError, "tree hash"):
                self.run_compose()
        (self.root / "vendor/src/new.cpp").unlink()
        original = Path.lstat
        def reparse(path, *args, **kwargs):
            if path == self.root / "vendor/src/__pycache__":
                return SimpleNamespace(st_mode=stat.S_IFDIR, st_file_attributes=0x400)
            return original(path, *args, **kwargs)
        with mock.patch.object(Path, "lstat", reparse):
            with self.assertRaisesRegex(ValueError, "link|reparse"):
                self.run_compose()

    def test_resource_budgets_and_machine_metadata_fail_closed(self):
        for constant, limit in (("MAX_RECORDS", 2), ("MAX_PAYLOAD_FILES", 2), ("MAX_TOTAL", 1), ("MAX_INPUT_BYTES", 1), ("MAX_JSON", 1)):
            with self.subTest(constant=constant), mock.patch.object(composer, constant, limit):
                with self.assertRaises(ValueError):
                    self.run_compose()
                self.assertFalse((self.root / "out").exists())
        self.report["components"][0]["recipe"]["recipe_options"] = ["-DPATH=/home/private/tools"]
        with self.assertRaisesRegex(ValueError, "machine path"):
            self.run_compose()

    def test_frozen_payload_table_accepts_more_than_physical_payload_budget(self):
        binding = copy.deepcopy(self.report["candidate_binding"])
        binding["offline_bundle"] = {key: self.record("receipts/runtime.json") for key in
            ("manifest", "portable_manifest", "runtime_manifest", "source_inventory", "source_kit")}
        rows = [{"path": f"bundle/file-{index:05}.hpp", "sha256": "0" * 64, "bytes": 0}
                for index in range(20_001)]
        binding["offline_bundle"].update(payload=rows, source_location="bundle/source-kit")
        instance = composer.Composer(self.root, self.root / "out")
        # Isolate filesystem work: table admission and binding traversal remain real.
        with mock.patch.object(instance, "record", side_effect=lambda row, *args, **kwargs: row):
            result = instance.binding(binding, {"dependency"})
            self.assertEqual(len(result["offline_bundle"]["payload"]), len(rows))
            with mock.patch.object(composer.closure, "MAX_BUNDLE_FILES", len(rows) - 1):
                with self.assertRaisesRegex(ValueError, "record table"):
                    instance.binding(binding, {"dependency"})
            rows.extend({"path": f"bundle/file-{index:05}.hpp", "sha256": "0" * 64, "bytes": 0}
                        for index in range(len(rows), 32_768))
            self.assertEqual(len(instance.binding(binding, {"dependency"})["offline_bundle"]["payload"]), 32_768)
            rows.append({"path": "bundle/overflow.hpp", "sha256": "0" * 64, "bytes": 0})
            with self.assertRaisesRegex(ValueError, "record table"):
                instance.binding(binding, {"dependency"})
        self.assertEqual(instance.payload, {})

    def test_general_receipt_tables_and_metadata_keep_finite_replay_bounds(self):
        rows = [{"path": f"receipt/file-{index:05}"} for index in range(65_536)]
        self.assertEqual(len(composer.table(rows)), 65_536)
        rows.append({"path": "receipt/overflow"})
        with self.assertRaisesRegex(ValueError, "record table"):
            composer.table(rows)
        values = ["bounded metadata"] * 65_536
        self.assertEqual(len(composer.strings(values)), 65_536)
        values.append("overflow")
        with self.assertRaisesRegex(ValueError, "metadata list"):
            composer.strings(values)

    def test_receipt_replay_budget_is_separate_from_unique_payload_budget(self):
        instance = composer.Composer(self.root, self.root / "out")
        with mock.patch.object(composer, "MAX_PAYLOAD_FILES", 1):
            row = self.record("recipe/fix.patch")
            instance.record(row, "dependency", "recipe")
            instance.record(row, "dependency", "recipe")
            instance.record(self.record("candidate/app.dll"), None, "frozen_payload", copy=False)
            self.assertEqual(len(instance.payload), 1)
            with self.assertRaisesRegex(ValueError, "payload file count"):
                instance.record(self.record("recipe/portfile.cmake"), "dependency", "recipe")
        instance.records = 65_535
        instance.record(row, None, "frozen_payload", copy=False)
        with self.assertRaisesRegex(ValueError, "record count"):
            instance.record(row, None, "frozen_payload", copy=False)

    def test_duplicate_json_keys_are_rejected(self):
        self.write("report.json", b'{"schema_version":1,"schema_version":1}')
        with self.assertRaisesRegex(ValueError, "Duplicate"):
            composer.compose(self.root, "report.json", "out")

    def test_sha512_source_and_unknown_directory_notices_fail_closed(self):
        row = self.report["components"][0]["sources"][0]["local_files"][0]
        row["sha512"] = "0" * 128
        with self.assertRaisesRegex(ValueError, "hash"):
            self.run_compose()
        del row["sha512"]
        self.report["components"][0]["notices"][0]["kind"] = "directory"
        with self.assertRaisesRegex(ValueError, "source input"):
            self.run_compose()

    def test_case_collision_in_empty_source_directories_rejected(self):
        (self.root / "vendor/Sub").mkdir(parents=True)
        with mock.patch.object(composer.os, "walk", return_value=[(str(self.root / "vendor"), ["Sub", "sub"], [])]):
            with self.assertRaisesRegex(ValueError, "case-colliding"):
                composer.directory_files(self.root, "vendor")

    def test_real_closure_schema_composes_with_tiny_vcpkg_fixture(self):
        port = ".deps/vcpkg/ports/dependency/portfile.cmake"
        self.write(port, b"configure(-DBUILD_SHARED_LIBS=ON)\n")
        archive = "cache/source.tar.xz"
        archive_data = (self.root / archive).read_bytes()
        spdx = {"packages": [{"SPDXID": "SPDXRef-resource-0", "name": "Example/dependency",
            "downloadLocation": "git+https://github.com/Example/dependency@v1.0",
            "checksums": [{"algorithm": "SHA512", "checksumValue": hashlib.sha512(archive_data).hexdigest()}]}],
            "files": [{"SPDXID": "SPDXRef-port-file-0", "fileName": "./portfile.cmake",
                       "checksums": [{"algorithm": "SHA256", "checksumValue": self.record(port)["sha256"]}]}]}
        self.write("receipts/spdx.json", json.dumps(spdx).encode())
        self.write("sdk/include/unit.hpp", b"header source")
        inventory = {"schema_version": 1, "evidence": {
            "component_manifest": self.record("receipts/components.json"), "runtime": self.record("receipts/runtime.json")},
            "components": [{"id": "dependency", "package": {"name": "dependency", "version": "1", "license": "MIT",
                "source": {"kind": "vcpkg", "package_name": "dependency", "spdx_path": "receipts/spdx.json",
                           "spdx_sha256": self.record("receipts/spdx.json")["sha256"]}},
                "notices": [self.record("notices/license.txt")],
                "source_inputs": [{"path": "sdk/include", "kind": "directory",
                    "sha256": tree_v2_digest({"unit.hpp": b"header source"})}]}],
            "binaries": [{"component_id": "dependency", **self.record("candidate/app.dll")}]}
        self.write("receipts/inventory.json", json.dumps(inventory).encode())
        audited = composer.closure.audit(self.root, "receipts/inventory.json", cache_dirs=("cache",),
                                          build_receipts=("build/CMakeCache.txt",))
        result = self.run_compose(report=audited)
        self.assertEqual(result["components"][0]["sources"][0]["local_files"][0]["sha512"], hashlib.sha512(archive_data).hexdigest())
        self.assertEqual((self.root / "out/inputs/sdk/include/unit.hpp").read_bytes(), b"header source")
        self.assertFalse(result["offline_rebuild_qualified"])

    def test_controlled_runtime_sources_and_receipts_compose_without_qualification(self):
        self.write("vendor/source/unit.cpp", b"editable controlled source")
        self.write("staged/ifc-wrapper.pyd", b"exact controlled payload")
        self.write("receipts/sdk.json", b"sdk receipt")
        self.write("receipts/selection.json", b"selection receipt")
        source = {"kind": "controlled-runtime", "source_revision": "a" * 40,
                  "runtime_manifest": self.record("receipts/sdk.json"),
                  "selection": self.record("receipts/selection.json"),
                  "source_paths": [self.record("staged/ifc-wrapper.pyd")],
                  "corresponding_source_paths": [
                      {"path": "vendor/source", "kind": "directory",
                       "sha256": composer.closure.source_tree_hash(self.root, "vendor/source")},
                      {**self.record("recipe/portfile.cmake"), "kind": "file"}],
                  "licensing_clearance": False, "source_closure_qualified": False,
                  "ifcopenshell_version_informative_only": True}
        inventory = {"schema_version": 1, "evidence": {
            "component_manifest": self.record("receipts/components.json"), "runtime": self.record("receipts/runtime.json")},
            "components": [{"id": "dependency", "package": {"name": "ifcopenshell", "version": "0.8.5", "license": "LGPL-3.0-or-later",
                            "source": source}, "notices": [self.record("notices/license.txt")]}],
            "binaries": [{"component_id": "dependency", **self.record("candidate/app.dll")}]}
        self.write("receipts/inventory.json", json.dumps(inventory).encode())
        audited = composer.closure.audit(self.root, "receipts/inventory.json", cache_dirs=())
        result = self.run_compose(report=audited)
        item = result["components"][0]
        self.assertEqual(item["declared_source_kind"], "controlled-runtime")
        self.assertEqual((self.root / "out/inputs/vendor/source/unit.cpp").read_bytes(), b"editable controlled source")
        self.assertEqual((self.root / "out/inputs/staged/ifc-wrapper.pyd").read_bytes(), b"exact controlled payload")
        self.assertEqual(item["provenance"]["source_path"], "receipts/sdk.json")
        self.assertEqual(item["sources"][0]["provenance"]["source_path"], "receipts/selection.json")
        self.assertFalse(result["offline_rebuild_qualified"])
        self.assertFalse(item["licensing_clearance"])
        self.assertFalse(item["corresponding_source_qualified"])
        self.write("receipts/selection.json", b"stale selection")
        with self.assertRaisesRegex(ValueError, "hash"):
            self.run_compose("stale", report=audited)
        self.assertFalse((self.root / "stale").exists())

    def test_cli_publishes_manifest_receipt_and_refuses_existing_payload(self):
        self.write("report.json", json.dumps(self.report).encode())
        arguments = ["composer", "--workspace", str(self.root), "--report", "report.json", "--output-root", "out"]
        output, error = io.StringIO(), io.StringIO()
        with mock.patch.object(composer.sys, "argv", arguments), redirect_stdout(output), redirect_stderr(error):
            self.assertEqual(composer.main(), 0)
        receipt = json.loads(output.getvalue())
        self.assertEqual(receipt["sha256"], self.record("out/" + composer.MANIFEST)["sha256"])
        self.assertEqual(error.getvalue(), "")
        output, error = io.StringIO(), io.StringIO()
        with mock.patch.object(composer.sys, "argv", arguments), redirect_stdout(output), redirect_stderr(error):
            self.assertEqual(composer.main(), 1)
        self.assertEqual(output.getvalue(), "")
        self.assertIn("empty", error.getvalue())

    def test_excluded_tree_inputs_still_count_against_limits(self):
        self.write("vendor/unit.cpp", b"unit")
        self.write("vendor/__pycache__/skip.pyc", b"cache exceeding bound")
        with mock.patch.object(composer.closure, "MAX_SOURCE_TREE_FILES", 1):
            with self.assertRaisesRegex(ValueError, "file count"):
                composer.directory_files(self.root, "vendor")
        with mock.patch.object(composer.closure, "MAX_FILE", 8):
            with self.assertRaisesRegex(ValueError, "invalid source tree file"):
                composer.directory_files(self.root, "vendor")

    def test_archive_declared_checksum_is_also_bound(self):
        self.report["components"][0]["sources"][0]["declared_checksum"]["value"] = "0" * 64
        with self.assertRaisesRegex(ValueError, "checksum"):
            self.run_compose()

    def test_upstream_asset_locked_binary_and_configuration_are_explicit(self):
        item = self.report["components"][0]
        item["upstream_asset"] = {**self.record("cache/source.tar.xz"), "url": "https://example.invalid/model",
                                  "provenance": self.record("receipts/spdx.json"), "binding_role": "upstream asset"}
        item["binary_archive"] = self.record("cache/source.tar.xz")
        item["configuration"] = {**self.record("build/CMakeCache.txt"), "observed_variables": {"QT_VERSION": ["6.8.3"]}, "role": "configuration"}
        result = self.run_compose()
        self.assertEqual(result["components"][0]["configuration"]["observed_variables"], {"QT_VERSION": ["6.8.3"]})
        self.assertIn("payload_path", result["components"][0]["upstream_asset"])

    def selected_source_kit(self, paths):
        # The actual source-kit generator supplies its own validated schema.
        import sys
        sys.path.insert(0, str(ROOT / "scripts"))
        import source_kit_manifest
        manifest = source_kit_manifest.build_manifest(self.root, [
            {"path": path, "category": "source"} for path in paths])
        self.write("receipts/source-kit.json", json.dumps(manifest).encode())
        return "receipts/source-kit.json"

    def test_direct_selected_source_kit_is_copied_and_bound_without_frozen_bundle(self):
        selected = self.selected_source_kit(["recipe/portfile.cmake"])
        self.write("report.json", json.dumps(self.report).encode())
        result = composer.compose(self.root, "report.json", "out", source_kit_manifest=selected)
        bound = result["candidate_binding"]["source_kit"]
        self.assertEqual(bound, {"source_path": selected, "payload_path": "inputs/" + selected,
                                "sha256": self.record(selected)["sha256"],
                                "bytes": self.record(selected)["bytes"]})
        row = next(row for row in result["files"] if row["source_path"] == selected)
        self.assertEqual(row["roles"], ["source_kit"])
        self.assertIsNone(result["candidate_binding"]["offline_bundle"])

    def test_direct_project_reference_must_be_in_selected_source_kit(self):
        self.write("src/unit.cpp", b"unit")
        item = self.report["components"][0]
        item["id"] = "vertex"
        item["declared_source_kind"] = "workspace"
        self.report["candidate_binding"]["binaries"][0]["component_id"] = "vertex"
        item["sources"] = [{"url": None, "status": "exact_local_source_present",
            "local_files": [{"path": "src", "kind": "directory",
                             "sha256": composer.closure.source_tree_hash(self.root, "src")}]}]
        selected = self.selected_source_kit(["recipe/portfile.cmake"])
        self.write("report.json", json.dumps(self.report).encode())
        with self.assertRaisesRegex(ValueError, "selected source kit"):
            composer.compose(self.root, "report.json", "missing", source_kit_manifest=selected)
        self.assertFalse((self.root / "missing").exists())
        selected = self.selected_source_kit(["src/unit.cpp"])
        result = composer.compose(self.root, "report.json", "out", source_kit_manifest=selected)
        self.assertEqual(result["components"][0]["sources"][0]["local_files"][0]
                         ["project_source_kit_path"], "src")
        self.assertFalse((self.root / "out/inputs/src").exists())

    def test_stale_direct_manifest_and_late_source_drift_do_not_publish(self):
        selected = self.selected_source_kit(["recipe/portfile.cmake"])
        self.write("report.json", json.dumps(self.report).encode())
        self.write("recipe/portfile.cmake", b"changed")
        with self.assertRaisesRegex(ValueError, "hash|selected source kit"):
            composer.compose(self.root, "report.json", "stale", source_kit_manifest=selected)
        self.assertFalse((self.root / "stale").exists())
        self.write("recipe/portfile.cmake", b"configure()")
        copy_input = composer.copy_file
        def drift(*args, **kwargs):
            value = copy_input(*args, **kwargs)
            self.write("receipts/source-kit.json", b"{}")
            return value
        with mock.patch.object(composer, "copy_file", side_effect=drift):
            with self.assertRaisesRegex(ValueError, "hash"):
                composer.compose(self.root, "report.json", "drift", source_kit_manifest=selected)
        self.assertFalse((self.root / "drift").exists())

    def test_direct_source_kit_cli_option_is_forwarded(self):
        selected = self.selected_source_kit(["recipe/portfile.cmake"])
        self.write("report.json", json.dumps(self.report).encode())
        arguments = ["composer", "--workspace", str(self.root), "--report", "report.json",
                     "--output-root", "out", "--source-kit-manifest", selected]
        output = io.StringIO()
        with mock.patch.object(composer.sys, "argv", arguments), redirect_stdout(output):
            self.assertEqual(composer.main(), 0)
        result = json.loads((self.root / "out" / composer.MANIFEST).read_text())
        self.assertEqual(result["candidate_binding"]["source_kit"]["source_path"], selected)


if __name__ == "__main__":
    unittest.main()
