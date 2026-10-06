"""Candidate/source receipt integrity, without asserting licensing clearance."""

import copy
import hashlib
import importlib.util
import json
from pathlib import Path
import stat
import tempfile
from types import SimpleNamespace
import unittest
from unittest import mock

ROOT = Path(__file__).resolve().parents[1]
SPEC = importlib.util.spec_from_file_location("dependency_source_closure", ROOT / "scripts/qualification/dependency_source_closure.py")
assert SPEC and SPEC.loader
audit = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(audit)
INVENTORY_SPEC = importlib.util.spec_from_file_location("distribution_inventory", ROOT / "scripts/distribution_inventory.py")
assert INVENTORY_SPEC and INVENTORY_SPEC.loader
inventory = importlib.util.module_from_spec(INVENTORY_SPEC)
INVENTORY_SPEC.loader.exec_module(inventory)


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


class SourceClosureTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.write("candidate/app.dll", b"candidate binary")
        self.write("notices/dependency.txt", b"Copyright Example\nMIT License\nPermission is hereby granted, free of charge, to use this software.\n")
        self.write("cache/dependency.tar.gz", b"opaque exact source archive")
        self.write(".deps/vcpkg/ports/dependency/portfile.cmake", b"configure(-DBUILD_SHARED_LIBS=ON)\n")
        spdx = {"packages": [{"SPDXID": "SPDXRef-resource-0", "name": "Example/dependency",
                 "downloadLocation": "git+https://github.com/Example/dependency@v1.0",
                 "checksums": [{"algorithm": "SHA512", "checksumValue": self.sha("cache/dependency.tar.gz", "sha512")}]}],
                "files": [{"SPDXID": "SPDXRef-port-file-0", "fileName": "./portfile.cmake",
                           "checksums": [{"algorithm": "SHA256", "checksumValue": self.sha(".deps/vcpkg/ports/dependency/portfile.cmake")}]}]}
        self.write_json("spdx/dependency.json", spdx)
        self.write_json("receipts/component.json", {"components": []})
        self.write_json("receipts/runtime.json", {"modules": []})
        self.inventory = {
            "schema_version": 1, "distribution_qualified": False,
            "evidence": {"component_manifest": self.record("receipts/component.json"),
                         "runtime": self.record("receipts/runtime.json")},
            "components": [{"id": "dependency", "kind": "runtime", "distribution_status": "included",
                            "package": {"name": "dependency", "version": "1.0", "license": "MIT",
                                        "source": {"kind": "vcpkg", "package_name": "dependency",
                                                   "spdx_path": "spdx/dependency.json", "spdx_sha256": self.sha("spdx/dependency.json")}},
                            "destinations": {"app.dll": "bin/app.dll"},
                            "notices": [self.record("notices/dependency.txt")]}],
            "binaries": [{"component_id": "dependency", "name": "app.dll", "destination": "bin/app.dll",
                          **self.record("candidate/app.dll")}],
        }
        self.write_json("inventory.json", self.inventory)

    def write(self, path, data):
        target = self.root / path
        target.parent.mkdir(parents=True, exist_ok=True)
        target.write_bytes(data)

    def write_json(self, path, value):
        self.write(path, (json.dumps(value) + "\n").encode())

    def sha(self, path, algorithm="sha256"):
        return hashlib.new(algorithm, (self.root / path).read_bytes()).hexdigest()

    def record(self, path):
        return {"path": path, "sha256": self.sha(path)}

    def run_audit(self, **kwargs):
        return audit.audit(self.root, "inventory.json", cache_dirs=("cache",), **kwargs)

    def qt_configuration_fixture(self, configuration):
        config_path = "qt/prefix/mkspecs/qconfig.pri"
        self.write(config_path, configuration.encode("utf-8"))
        self.write_json("spdx/qt.json", {"packages": [], "files": []})
        component = self.inventory["components"][0]
        component["id"] = "qtbase"
        self.inventory["binaries"][0]["component_id"] = "qtbase"
        component["package"].update(name="qtbase", version="6.8.3")
        component["package"]["source"] = {
            "kind": "qt", "prefix_path": "qt/prefix", "spdx_path": "spdx/qt.json",
            "spdx_sha256": self.sha("spdx/qt.json")}
        self.write_json("inventory.json", self.inventory)
        return self.root / config_path

    def test_qt_qconfig_preserves_plus_tokens_assignment_order_and_compiler_keys(self):
        self.qt_configuration_fixture(
            "QT_CONFIG = initial\n"
            "QT_CONFIG += superseded\n"
            "QT_CONFIG = release c++20 feature+\n"
            "QT_CONFIG += exceptions\n"
            "QT_COMPILER_STDCXX = c++20\n"
            "QT_ARCH = x86_64\n"
            "QT_BUILDABI = x86_64-little_endian-llp64\n"
            "QT_MSVC_MAJOR_VERSION = 19\n"
            "QT_MSVC_MINOR_VERSION = 41\n"
            "QT_MSVC_PATCH_VERSION = 0\n")
        report = self.run_audit()
        component = report["components"][0]
        observed = component["configuration"]["observed_variables"]
        self.assertEqual(observed["QT_CONFIG"], ["release", "c++20", "feature+", "exceptions"])
        self.assertEqual(observed["QT_COMPILER_STDCXX"], ["c++20"])
        self.assertEqual(observed["QT_ARCH"], ["x86_64"])
        self.assertEqual(observed["QT_BUILDABI"], ["x86_64-little_endian-llp64"])
        self.assertEqual(observed["QT_MSVC_MAJOR_VERSION"], ["19"])
        self.assertEqual(observed["QT_MSVC_MINOR_VERSION"], ["41"])
        self.assertEqual(observed["QT_MSVC_PATCH_VERSION"], ["0"])
        self.assertFalse(report["licensing_clearance"])
        self.assertFalse(report["corresponding_source_qualified"])

    def test_qt_qconfig_ignores_unselected_and_unsafe_expressions_without_evaluation(self):
        marker = self.root / "qconfig-expression-ran"
        self.qt_configuration_fixture(
            "QT_CONFIG = c++20\n"
            f"QT_CONFIG += $$system(echo unexpected > {marker})\n"
            f"UNSELECTED = $$system(echo unexpected > {marker})\n"
            "QT_FEATURE_dynamic = $$[QT_INSTALL_PREFIX]\n")
        report = self.run_audit()
        observed = report["components"][0]["configuration"]["observed_variables"]
        self.assertEqual(observed, {"QT_CONFIG": ["c++20"]})
        self.assertFalse(marker.exists())
        self.assertFalse(report["licensing_clearance"])
        self.assertFalse(report["corresponding_source_qualified"])

    def test_qt_qconfig_receipt_is_rechecked_after_configuration_read(self):
        config_path = self.qt_configuration_fixture("QT_CONFIG = release c++20\n")
        read_bounded = audit.safe.read_bounded
        drifted = False

        def read_then_drift(path, *args, **kwargs):
            nonlocal drifted
            data = read_bounded(path, *args, **kwargs)
            if Path(path) == config_path and not drifted:
                drifted = True
                config_path.write_bytes(data + b"QT_VERSION = changed\n")
            return data

        with mock.patch.object(audit.safe, "read_bounded", side_effect=read_then_drift):
            with self.assertRaisesRegex(ValueError, "hash"):
                self.run_audit()

    def declared_tree_fixture(self):
        self.write("sdk/include/unit.hpp", b"header source")
        digest = tree_v2_digest({"unit.hpp": b"header source"})
        row = {"path": "sdk/include", "kind": "directory", "sha256": digest}
        self.inventory["components"][0]["source_inputs"] = [row]
        self.write_json("inventory.json", self.inventory)
        return row

    def test_declared_source_input_tree_is_bound_and_rechecked(self):
        row = self.declared_tree_fixture()
        result = self.run_audit()
        self.assertEqual(result["components"][0]["assets"], [row])
        self.assertFalse(result["corresponding_source_qualified"])
        self.write("sdk/include/unit.hpp", b"changed header")
        with self.assertRaisesRegex(ValueError, "tree hash"):
            self.run_audit()

    def test_declared_source_input_tree_late_addition_is_refused(self):
        self.declared_tree_fixture()
        original = audit.source_tree_hash
        def drift(*args, **kwargs):
            result = original(*args, **kwargs)
            self.write("sdk/include/late.hpp", b"late source")
            return result
        with mock.patch.object(audit, "source_tree_hash", side_effect=drift):
            with self.assertRaisesRegex(ValueError, "tree hash"):
                self.run_audit()

    def test_directory_receipts_are_explicit_and_source_input_only(self):
        row = self.declared_tree_fixture()
        component = self.inventory["components"][0]
        for field in ("notices", "artifacts"):
            with self.subTest(field=field):
                original = component.get(field)
                component[field] = [row]
                self.write_json("inventory.json", self.inventory)
                with self.assertRaisesRegex(ValueError, "directory|file"):
                    self.run_audit()
                if original is None:
                    del component[field]
                else:
                    component[field] = original
        for change in ({"kind": "file"}, {"kind": "unknown"}, {"bytes": 0}, {"sha256": "0" * 64}):
            component["source_inputs"] = [{**row, **change}]
            self.write_json("inventory.json", self.inventory)
            with self.subTest(change=change), self.assertRaises(ValueError):
                self.run_audit()

    def test_frozen_directory_binding_requires_exact_directory_mapping(self):
        row = self.declared_tree_fixture()
        self.write("bundle/sdk/unit.hpp", b"header source")
        mapping = {("dependency", "sdk/include", "source"): {
            "path": "bundle/sdk", "kind": "directory", "sha256": row["sha256"]}}
        self.write("sdk/include/unit.hpp", b"live changed source")
        record = audit.bound_component_record(self.root, "dependency", row, mapping, ("source",),
                                              required=True, source_input=True)
        self.assertEqual(record, {**row, "path": "bundle/sdk", "inventory_path": "sdk/include"})
        with self.assertRaisesRegex(ValueError, "missing exact frozen"):
            audit.bound_component_record(self.root, "dependency", row, {}, ("source",),
                                         required=True, source_input=True)
        del mapping[("dependency", "sdk/include", "source")]["kind"]
        with self.assertRaisesRegex(ValueError, "kind"):
            audit.bound_component_record(self.root, "dependency", row, mapping, ("source",),
                                         required=True, source_input=True)

    def test_source_tree_uses_separate_finite_file_and_entry_bounds(self):
        self.write("sdk/include/a.hpp", b"a")
        self.write("sdk/include/b.hpp", b"b")
        expected = tree_v2_digest({"a.hpp": b"a", "b.hpp": b"b"})
        with mock.patch.object(audit, "MAX_FILES", 1):
            self.assertEqual(audit.source_tree_hash(self.root, "sdk/include"), expected)
            self.write("cache/second.tar.gz", b"archive")
            with self.assertRaisesRegex(ValueError, "too many cache"):
                audit.source_cache(self.root, ("cache",))
        with mock.patch.object(audit, "MAX_SOURCE_TREE_FILES", 1):
            with self.assertRaisesRegex(ValueError, "file count"):
                audit.source_tree_hash(self.root, "sdk/include")
        (self.root / "sdk/include/empty").mkdir()
        with mock.patch.object(audit, "MAX_SOURCE_TREE_ENTRIES", 2):
            with self.assertRaisesRegex(ValueError, "entry count"):
                audit.source_tree_hash(self.root, "sdk/include")

    def test_source_tree_refuses_addition_during_hashing(self):
        self.write("sdk/include/unit.hpp", b"header source")
        original = Path.open
        def add_during_read(path, *args, **kwargs):
            stream = original(path, *args, **kwargs)
            if path == self.root / "sdk/include/unit.hpp" and args == ("rb",):
                self.write("sdk/include/late.hpp", b"late source")
            return stream
        with mock.patch.object(Path, "open", add_during_read):
            with self.assertRaisesRegex(ValueError, "tree changed"):
                audit.source_tree_hash(self.root, "sdk/include")

    def test_source_tree_refuses_empty_directory_case_collision_and_nonregular_file(self):
        (self.root / "sdk/include/Sub").mkdir(parents=True)
        with mock.patch.object(audit.os, "walk", return_value=[(str(self.root / "sdk/include"), ["Sub", "sub"], [])]):
            with self.assertRaisesRegex(ValueError, "case-colliding"):
                audit.source_tree_hash(self.root, "sdk/include")
        self.write("sdk/include/unit.hpp", b"header source")
        original = Path.lstat
        def nonregular(path, *args, **kwargs):
            actual = original(path, *args, **kwargs)
            if path == self.root / "sdk/include/unit.hpp":
                return SimpleNamespace(st_mode=stat.S_IFIFO, st_dev=actual.st_dev, st_ino=actual.st_ino,
                    st_size=actual.st_size, st_mtime_ns=actual.st_mtime_ns, st_file_attributes=0)
            return actual
        with mock.patch.object(Path, "lstat", nonregular):
            with self.assertRaisesRegex(ValueError, "invalid source tree file"):
                audit.source_tree_hash(self.root, "sdk/include")

    def model_asset_fixture(self):
        self.write("assets/model.traineddata", b"official trained model")
        self.write_json("receipts/dependencies.json", {"bundled_assets": []})
        component = self.inventory["components"][0]
        component["package"]["source"] = {
            "kind": "bootstrap", "asset_path": "assets/model.traineddata",
            "declared_sha256": self.sha("assets/model.traineddata"),
            "dependencies_path": "receipts/dependencies.json",
            "dependencies_sha256": self.sha("receipts/dependencies.json"),
            "url": "https://example.invalid/model/pinned-commit", "kind_detail": None}
        component["source_inputs"] = [self.record("assets/model.traineddata")]
        self.write_json("inventory.json", self.inventory)

    def controlled_runtime_fixture(self):
        self.write("staged/ifcopenshell/ifcopenshell_wrapper.py", b"controlled generated wrapper")
        self.write_json("receipts/sdk.json", {"source_revision": "a" * 40})
        self.write_json("receipts/selection.json", {"selected": "controlled"})
        self.write("dependency-src/unit.cpp", b"preferred editable source")
        self.write("dependency-recipe/build.cmake", b"controlled build recipe")
        component = self.inventory["components"][0]
        component["package"].update(name="ifcopenshell", version="0.8.5")
        component["source_inputs"] = [self.record("staged/ifcopenshell/ifcopenshell_wrapper.py")]
        component["package"]["source"] = {
            "kind": "controlled-runtime", "source_revision": "a" * 40,
            "runtime_manifest": self.record("receipts/sdk.json"),
            "selection": self.record("receipts/selection.json"),
            "source_paths": component["source_inputs"],
            "corresponding_source_paths": [
                {"path": "dependency-src", "kind": "directory",
                 "sha256": audit.source_tree_hash(self.root, "dependency-src")},
                {**self.record("dependency-recipe/build.cmake"), "kind": "file"}],
            "licensing_clearance": False, "source_closure_qualified": False,
            "ifcopenshell_version_informative_only": True}
        self.write_json("inventory.json", self.inventory)

    def test_controlled_runtime_binds_declared_sources_without_wheel_metadata(self):
        self.controlled_runtime_fixture()
        with mock.patch.object(audit, "cached_source", side_effect=AssertionError("wheel metadata consulted")):
            result = self.run_audit()["components"][0]
        self.assertEqual(result["source_status"], "exact_local_source_present")
        self.assertEqual([row["path"] for row in result["sources"][0]["local_files"]],
                         ["dependency-src", "dependency-recipe/build.cmake"])
        self.assertEqual(result["provenance"]["sha256"], self.sha("receipts/sdk.json"))
        self.assertEqual(result["sources"][0]["provenance"]["sha256"], self.sha("receipts/selection.json"))
        self.assertNotIn("binary_archive", result)
        self.assertNotIn("upstream_metadata", result)
        self.assertFalse(result["corresponding_source_qualified"])
        self.assertFalse(result["licensing_clearance"])
        self.assertIn("controlled_runtime_transitive_source_closure_not_qualified", result["remaining"])

    def test_controlled_runtime_empty_or_missing_sources_remain_missing(self):
        self.controlled_runtime_fixture()
        source = self.inventory["components"][0]["package"]["source"]
        for entries in ([], [{**self.record("dependency-recipe/build.cmake"),
                              "path": "dependency-recipe/missing.cmake", "kind": "file"}]):
            with self.subTest(entries=entries):
                source["corresponding_source_paths"] = entries
                self.write_json("inventory.json", self.inventory)
                result = self.run_audit()["components"][0]
                self.assertEqual(result["source_status"], "missing_source")
                self.assertEqual(result["sources"][0]["local_files"], [])
                self.assertIn("exact_corresponding_source_missing", result["remaining"])
                if not entries:
                    self.assertIn("controlled_runtime_corresponding_source_paths_not_declared", result["remaining"])

    def test_controlled_runtime_changed_or_undeclared_sources_remain_missing(self):
        self.controlled_runtime_fixture()
        source = self.inventory["components"][0]["package"]["source"]
        self.write("dependency-src/unit.cpp", b"changed preferred source")
        self.write("dependency-recipe/build.cmake", b"changed recipe")
        result = self.run_audit()["components"][0]
        self.assertEqual(result["source_status"], "missing_source")
        self.assertEqual(result["sources"][0]["missing_or_changed_paths"],
                         ["dependency-src", "dependency-recipe/build.cmake"])
        del source["corresponding_source_paths"]
        self.write_json("inventory.json", self.inventory)
        result = self.run_audit()["components"][0]
        self.assertEqual(result["sources"][0]["local_files"], [])
        self.assertIn("controlled_runtime_corresponding_source_paths_not_declared", result["remaining"])

    def test_controlled_runtime_uses_frozen_stage_mapping_and_live_dependency_sources(self):
        self.controlled_runtime_fixture()
        self.frozen_bundle()
        self.write("staged/ifcopenshell/ifcopenshell_wrapper.py", b"later staged payload")
        result = self.run_audit(bundle_root="bundle")["components"][0]
        self.assertEqual(result["assets"][0]["path"], "bundle/assets/asset-0.bin")
        self.assertEqual(result["sources"][0]["local_files"][0]["path"], "dependency-src")
        self.assertEqual(result["source_status"], "exact_local_source_present")

    def test_controlled_runtime_rechecks_all_provenance_and_source_inputs(self):
        for changed in ("receipts/sdk.json", "receipts/selection.json",
                        "staged/ifcopenshell/ifcopenshell_wrapper.py",
                        "dependency-src/unit.cpp", "dependency-recipe/build.cmake"):
            with self.subTest(changed=changed):
                self.controlled_runtime_fixture()
                original = audit.file_record
                fired = False
                def drift(*args, **kwargs):
                    nonlocal fired
                    result = original(*args, **kwargs)
                    if args[1] == "dependency-recipe/build.cmake" and not fired:
                        fired = True
                        self.write(changed, b"late changed input")
                    return result
                with mock.patch.object(audit, "file_record", side_effect=drift):
                    with self.assertRaisesRegex(ValueError, "hash"):
                        self.run_audit()

    def test_controlled_runtime_rejects_unsafe_receipts_and_source_tree_limits(self):
        self.controlled_runtime_fixture()
        source = self.inventory["components"][0]["package"]["source"]
        original = copy.deepcopy(source)
        for field in ("runtime_manifest", "selection", "source_paths", "corresponding_source_paths"):
            with self.subTest(field=field):
                source.clear()
                source.update(copy.deepcopy(original))
                row = source[field][0] if isinstance(source[field], list) else source[field]
                row["path"] = "../private"
                self.write_json("inventory.json", self.inventory)
                with self.assertRaises(ValueError):
                    self.run_audit()
        source.clear()
        source.update(original)
        self.write("dependency-src/second.cpp", b"second source")
        source["corresponding_source_paths"][0]["sha256"] = audit.source_tree_hash(self.root, "dependency-src")
        source["corresponding_source_paths"] = source["corresponding_source_paths"][:1]
        self.write_json("inventory.json", self.inventory)
        with mock.patch.object(audit, "MAX_SOURCE_TREE_FILES", 1):
            with self.assertRaisesRegex(ValueError, "file count"):
                self.run_audit()

    def test_controlled_runtime_links_and_stale_receipt_bytes_fail_closed(self):
        self.controlled_runtime_fixture()
        original_lstat = Path.lstat
        for name in ("receipts/sdk.json", "receipts/selection.json",
                     "staged/ifcopenshell/ifcopenshell_wrapper.py",
                     "dependency-src", "dependency-src/unit.cpp", "dependency-recipe/build.cmake"):
            with self.subTest(link=name):
                def reparse(path, *args, **kwargs):
                    if path == self.root / name:
                        return SimpleNamespace(st_mode=stat.S_IFLNK, st_file_attributes=0)
                    return original_lstat(path, *args, **kwargs)
                with mock.patch.object(Path, "lstat", reparse):
                    with self.assertRaisesRegex(ValueError, "link|reparse"):
                        self.run_audit()
        source = self.inventory["components"][0]["package"]["source"]
        for field in ("runtime_manifest", "selection", "source_paths", "corresponding_source_paths"):
            with self.subTest(bytes=field):
                original = copy.deepcopy(source)
                row = source[field][-1] if isinstance(source[field], list) else source[field]
                row["bytes"] = 0
                self.write_json("inventory.json", self.inventory)
                with self.assertRaisesRegex(ValueError, "bytes"):
                    self.run_audit()
                source.clear()
                source.update(original)

    def test_model_artifact_is_bound_without_claiming_training_source(self):
        self.model_asset_fixture()
        result = self.run_audit()["components"][0]
        self.assertEqual(result["upstream_asset"]["sha256"], self.sha("assets/model.traineddata"))
        self.assertEqual(result["source_status"], "distributed_asset_recorded")
        self.assertNotIn("exact_corresponding_source_missing", result["remaining"])
        self.assertNotIn("build_and_relinking_evidence_not_qualified", result["remaining"])
        self.assertFalse(result["corresponding_source_qualified"])

    def test_model_artifact_uses_frozen_mapping_and_rejects_pin_mismatch(self):
        self.model_asset_fixture()
        self.frozen_bundle()
        self.write("assets/model.traineddata", b"subsequent workspace model")
        result = self.run_audit(bundle_root="bundle")["components"][0]
        self.assertEqual(result["upstream_asset"]["path"], "bundle/assets/asset-0.bin")
        self.inventory["components"][0]["package"]["source"]["declared_sha256"] = "0" * 64
        self.inventory["components"][0]["source_inputs"] = [self.record("assets/model.traineddata")]
        self.write_json("inventory.json", self.inventory)
        with self.assertRaisesRegex(ValueError, "hash"):
            self.run_audit()

    def test_unknown_spdx_license_remains_an_explicit_qualification_gap(self):
        component = self.inventory["components"][0]
        component["package"]["license"] = "LicenseRef-vcpkg-null"
        component["package"]["source"]["license_concluded"] = "LicenseRef-vcpkg-null"
        self.write_json("inventory.json", self.inventory)
        result = self.run_audit()["components"][0]
        self.assertEqual(result["package"]["license"], "LicenseRef-vcpkg-null")
        self.assertIn("upstream_spdx_license_conclusion_unresolved", result["remaining"])
        self.assertEqual(result["source_status"], "exact_local_source_present")
        self.assertFalse(result["licensing_clearance"])

    def frozen_bundle(self, sources=()):
        self.write_json("inventory.json", self.inventory)
        self.write("bundle/metadata/inventory.json", (self.root / "inventory.json").read_bytes())
        portable = {"schema_version": 1, "source_inventory": self.record("inventory.json"), "files": []}
        rows = []
        component = self.inventory["components"][0]
        inputs = [("binary", self.inventory["binaries"][0]["path"], "bin/app.dll")]
        inputs += [("notice", entry["path"], f"licenses/notice-{index}.txt")
                   for index, entry in enumerate(component["notices"])]
        inputs += [("asset", entry["path"], f"assets/asset-{index}.bin")
                   for index, entry in enumerate(component.get("source_inputs", []))]
        for kind, original, destination in inputs:
            self.write("bundle/" + destination, (self.root / original).read_bytes())
            record = {"path": destination, "sha256": self.sha("bundle/" + destination),
                      "component_id": "dependency", "kind": kind, "install": True}
            rows.append(record)
            portable["files"].append({**record, "source": original, "inventory_entry": "dependency"})
        for source in sources:
            self.write("bundle/source-kit/" + source, (self.root / source).read_bytes())
            rows.append({"path": "source-kit/" + source, "sha256": self.sha(source),
                         "kind": "source-kit", "install": False})
        self.write_json("bundle/metadata/portable.json", portable)
        self.write_json("bundle/runtime.json", {"schema_version": 1, "files": [row for row in rows if row["install"]]})
        self.write_json("bundle/source-kit.json", {})
        for path, kind in (("metadata/inventory.json", "distribution-inventory"),
                           ("metadata/portable.json", "portable-package-manifest"),
                           ("runtime.json", "runtime-manifest"), ("source-kit.json", "source-kit-manifest")):
            rows.append({"path": path, "sha256": self.sha("bundle/" + path), "kind": kind, "install": False})
        manifest = {"schema_version": 1, "files": rows,
                    "source_inventory": {"path": "metadata/inventory.json", "sha256": self.sha("inventory.json"), "original_path": "inventory.json"},
                    "runtime_manifest": {"path": "runtime.json", "sha256": self.sha("bundle/runtime.json")},
                    "source_kit": {"path": "source-kit.json", "sha256": self.sha("bundle/source-kit.json")}}
        self.write_json("bundle/offline-bundle-manifest.json", manifest)
        return manifest, portable

    def rewrite_mapping(self, manifest, portable):
        self.write_json("bundle/metadata/portable.json", portable)
        next(row for row in manifest["files"] if row["kind"] == "portable-package-manifest")["sha256"] = self.sha("bundle/metadata/portable.json")
        self.write_json("bundle/offline-bundle-manifest.json", manifest)

    def dependency_bundle_fixture(self):
        manifest, portable = self.frozen_bundle()
        tree = self.declared_tree_fixture()
        self.write("sdk/file.hpp", b"editable standalone source")
        self.inventory["components"][0]["source_inputs"].append(self.record("sdk/file.hpp"))
        self.write_json("inventory.json", self.inventory)
        self.write("bundle/metadata/inventory.json", (self.root / "inventory.json").read_bytes())
        manifest["source_inventory"]["sha256"] = self.sha("inventory.json")
        next(row for row in manifest["files"] if row["path"] == "metadata/inventory.json")["sha256"] = self.sha("inventory.json")
        portable["source_inventory"] = self.record("inventory.json")
        # Exercise the real composer schema rather than a partial synthetic kit.
        spec = importlib.util.spec_from_file_location(
            "closure_fixture_composer", ROOT / "scripts/qualification/compose_dependency_source_kit.py")
        composer = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(composer)
        source_manifest = self.root / "selected-source-kit.json"
        composer.project_manifest.build_manifest(
            self.root, {"entries": [{"category": "source", "path": "sdk/file.hpp"}]}, source_manifest)
        self.write("bundle/source-kit.json", source_manifest.read_bytes())
        self.write("bundle/source-kit/sdk/file.hpp", (self.root / "sdk/file.hpp").read_bytes())
        next(row for row in manifest["files"] if row["path"] == "source-kit.json")["sha256"] = self.sha("bundle/source-kit.json")
        manifest["source_kit"]["sha256"] = self.sha("bundle/source-kit.json")
        report = self.run_audit()
        self.write_json("source-report.json", report)
        kit = composer.compose(self.root, "source-report.json", "dependency-kit",
                               source_kit_manifest="selected-source-kit.json")
        kit_root = audit.DEPENDENCY_KIT_PREFIX
        for row in kit["files"]:
            frozen = "bundle/" + kit_root + "/" + row["path"]
            self.write(frozen, (self.root / "dependency-kit" / row["path"]).read_bytes())
            manifest["files"].append({"path": kit_root + "/" + row["path"], "sha256": row["sha256"],
                                      "size": row["bytes"], "kind": "dependency-source", "install": False})
        self.bind_dependency_fixture(manifest, portable, kit)
        return manifest, portable, kit

    def bind_dependency_fixture(self, manifest, portable, kit):
        relative = audit.DEPENDENCY_KIT_PREFIX + "/dependency-source-kit-manifest.json"
        self.write_json("bundle/" + relative, kit)
        row = {"path": relative, "sha256": self.sha("bundle/" + relative),
               "size": (self.root / "bundle" / relative).stat().st_size,
               "kind": "dependency-source", "install": False}
        manifest["files"] = [entry for entry in manifest["files"] if entry["path"] != relative] + [row]
        manifest["dependency_source_kit"] = {key: row[key] for key in ("path", "sha256", "size")}
        self.rewrite_mapping(manifest, portable)

    def test_delivered_sdk_tree_and_file_ignore_changed_live_sdk(self):
        self.dependency_bundle_fixture()
        self.write("sdk/include/unit.hpp", b"changed live SDK")
        self.write("sdk/file.hpp", b"changed live standalone source")
        result = self.run_audit(bundle_root="bundle")
        assets = result["components"][0]["assets"]
        self.assertTrue(all(row["path"].startswith("bundle/" + audit.DEPENDENCY_KIT_PREFIX) for row in assets))
        self.assertEqual(assets[0]["kind"], "directory")
        self.assertFalse(result["corresponding_source_qualified"])

    def test_frozen_dependency_source_mapping_rejects_relabelled_bindings(self):
        manifest, portable, original = self.dependency_bundle_fixture()
        mutations = (
            lambda kit: kit["candidate_binding"]["inventory"].update(sha256="0" * 64),
            lambda kit: kit["candidate_binding"]["source_kit"].update(sha256="0" * 64),
            lambda kit: kit["components"][0]["assets"][0].update(payload_path="inputs/sdk/other"),
            lambda kit: kit["components"][0]["assets"][0].update(kind="file"),
            lambda kit: next(row for row in kit["files"] if row["source_path"] == "sdk/include/unit.hpp").update(component_ids=[]),
            lambda kit: next(row for row in kit["files"] if row["source_path"] == "sdk/include/unit.hpp").update(roles=["notices"]),
            lambda kit: kit["files"][1].update(sha256="0" * 64),
            lambda kit: kit.update(corresponding_source_qualified=True),
            lambda kit: kit["files"].pop())
        for mutate in mutations:
            kit = copy.deepcopy(original)
            mutate(kit)
            self.bind_dependency_fixture(manifest, portable, kit)
            with self.subTest(mutate=mutate), self.assertRaises(ValueError):
                self.run_audit(bundle_root="bundle")

    def test_frozen_dependency_tree_hash_and_files_are_verified(self):
        manifest, portable, kit = self.dependency_bundle_fixture()
        original = "sdk/include/unit.hpp"
        relative = audit.DEPENDENCY_KIT_PREFIX + "/inputs/" + original
        self.write("bundle/" + relative, b"different delivered source")
        row = next(entry for entry in kit["files"] if entry["source_path"] == original)
        row.update(sha256=self.sha("bundle/" + relative), bytes=(self.root / "bundle" / relative).stat().st_size)
        next(entry for entry in manifest["files"] if entry["path"] == relative).update(sha256=row["sha256"], size=row["bytes"])
        self.bind_dependency_fixture(manifest, portable, kit)
        with self.assertRaisesRegex(ValueError, "tree hash"):
            self.run_audit(bundle_root="bundle")

    def test_frozen_closure_replays_archive_recipe_notice_and_exact_owners(self):
        manifest, portable, original = self.dependency_bundle_fixture()
        for mode in ("orphan-recipe", "recipe-owner", "recipe-role", "recipe-hash",
                     "archive-bytes", "notice-hash"):
            with self.subTest(mode=mode):
                kit = copy.deepcopy(original)
                component = kit["components"][0]
                recipe = next(row for row in kit["files"] if "recipe" in row["roles"])
                if mode == "orphan-recipe":
                    component["recipe"] = None
                    recipe["component_ids"] = []
                elif mode == "recipe-owner":
                    recipe["component_ids"] = []
                elif mode == "recipe-role":
                    recipe["roles"] = ["assets"]
                elif mode == "recipe-hash":
                    component["recipe"]["files"][0]["sha256"] = "0" * 64
                elif mode == "archive-bytes":
                    component["sources"][0]["local_files"][0]["bytes"] += 1
                else:
                    component["notices"][0]["sha256"] = "0" * 64
                self.bind_dependency_fixture(manifest, portable, kit)
                with self.assertRaises(ValueError):
                    self.run_audit(bundle_root="bundle")

    def test_source_tree_v2_prevents_ambiguous_two_tree_substitution(self):
        files = {"a.hpp": b"A", "b.hpp": b"B"}
        substitute = {"a.hpp": b"A\0b.hpp\0B"}
        legacy = lambda entries: hashlib.sha256(b"".join(
            name.encode() + b"\0" + content + b"\0" for name, content in sorted(entries.items()))).hexdigest()
        self.assertEqual(legacy(files), legacy(substitute))
        for name, content in files.items():
            self.write("sdk/include/" + name, content)
        row = {"path": "sdk/include", "kind": "directory", "sha256": tree_v2_digest(files)}
        self.assertEqual(audit.source_tree_hash(self.root, row["path"]), row["sha256"])
        self.inventory["components"][0]["source_inputs"] = [row]
        self.write_json("inventory.json", self.inventory)
        self.run_audit()
        self.write("sdk/include/a.hpp", substitute["a.hpp"])
        (self.root / "sdk/include/b.hpp").unlink()
        self.assertEqual(audit.source_tree_hash(self.root, row["path"]), tree_v2_digest(substitute))
        self.assertNotEqual(row["sha256"], tree_v2_digest(substitute))
        with self.assertRaisesRegex(ValueError, "tree hash"):
            self.run_audit()
        row["sha256"] = legacy(substitute)
        self.write_json("inventory.json", self.inventory)
        with self.assertRaisesRegex(ValueError, "tree hash"):
            self.run_audit()

    def test_future_source_tree_hash_matches_inventory_and_ignores_python_caches(self):
        self.write("src/adapter.py", b"value = 1\n")
        self.write("src/adapter.pyd", b"retained runtime artifact")
        source = self.root / "src"
        def matching_hash():
            value = audit.source_tree_hash(self.root, "src")
            self.assertEqual(value, inventory._tree_sha256(source))
            return value
        original = matching_hash()
        caches = ("src/desktop/__pycache__/adapter.cpython-313.pyc",
                  "src/desktop/__pycache__/cache-metadata", "src/legacy.pyc", "src/optimized.PYO")
        for path in caches:
            self.write(path, b"generated cache")
        self.assertEqual(matching_hash(), original)
        for path in caches:
            self.write(path, b"changed generated cache")
        self.assertEqual(matching_hash(), original)
        for path in caches:
            (self.root / path).unlink()
        self.assertEqual(matching_hash(), original)
        self.write("src/adapter.py", b"value = 2\n")
        self.assertNotEqual(matching_hash(), original)
        self.write("src/adapter.py", b"value = 1\n")
        (source / "adapter.py").rename(source / "renamed.py")
        self.assertNotEqual(matching_hash(), original)

    def test_source_tree_hash_rejects_links_at_excluded_cache_paths(self):
        self.write("src/__pycache__/adapter.cpython-313.pyc", b"cache")
        original_lstat = Path.lstat
        for target in (self.root / "src/__pycache__", self.root / "src/__pycache__/adapter.cpython-313.pyc"):
            with self.subTest(target=target.name):
                def link_metadata(path, *args, **kwargs):
                    if path == target:
                        return SimpleNamespace(st_mode=stat.S_IFLNK, st_file_attributes=0)
                    return original_lstat(path, *args, **kwargs)
                with mock.patch.object(Path, "lstat", link_metadata):
                    with self.assertRaisesRegex(ValueError, "link/reparse"):
                        audit.source_tree_hash(self.root, "src")
                    with self.assertRaisesRegex(inventory.InventoryError, "Symlink"):
                        inventory._tree_sha256(self.root / "src")

    def test_source_tree_hash_retains_limits_for_excluded_cache_files(self):
        self.write("src/adapter.py", b"source")
        self.write("src/__pycache__/adapter.pyc", b"cache exceeding small test limits")
        for limit, value, message in (("MAX_SOURCE_TREE_FILES", 1, "file count"), ("MAX_SOURCE_TREE_BYTES", 1, "bytes"),
                                      ("MAX_FILE", 8, "invalid source tree file")):
            with self.subTest(limit=limit), mock.patch.object(audit, limit, value):
                with self.assertRaisesRegex(ValueError, message):
                    audit.source_tree_hash(self.root, "src")

    def test_exact_source_archive_and_recipe_are_bound_and_deterministic(self):
        first = self.run_audit()
        self.assertEqual(first, self.run_audit())
        component = first["components"][0]
        self.assertEqual(component["source_status"], "exact_local_source_present")
        self.assertEqual(component["sources"][0]["local_files"][0]["sha256"], self.sha("cache/dependency.tar.gz"))
        self.assertEqual(component["recipe"]["status"], "exact_local_recipe_present")
        self.assertFalse(first["licensing_clearance"])
        self.assertFalse(first["corresponding_source_qualified"])
        self.assertEqual(first["candidate_binding"]["binaries"][0]["sha256"], self.sha("candidate/app.dll"))

    def test_missing_archive_and_changed_recipe_do_not_become_complete(self):
        (self.root / "cache/dependency.tar.gz").unlink()
        self.assertEqual(self.run_audit()["components"][0]["source_status"], "missing_source")
        self.write("cache/dependency.tar.gz", b"opaque exact source archive")
        self.write(".deps/vcpkg/ports/dependency/portfile.cmake", b"new unrelated recipe")
        self.assertEqual(self.run_audit()["components"][0]["recipe"]["status"], "missing_or_stale_recipe")

    def test_binary_and_notice_hash_drift_are_rejected(self):
        self.write("candidate/app.dll", b"changed executable")
        with self.assertRaisesRegex(ValueError, "hash"):
            self.run_audit()
        self.write("candidate/app.dll", b"candidate binary")
        self.write("notices/dependency.txt", b"changed notice")
        with self.assertRaisesRegex(ValueError, "hash"):
            self.run_audit()

    def test_pointer_is_recorded_but_never_counted_as_a_notice(self):
        self.write("notices/dependency.txt", b"For the latest license, please visit https://example.invalid/license\n")
        self.inventory["components"][0]["notices"] = [self.record("notices/dependency.txt")]
        self.write_json("inventory.json", self.inventory)
        self.assertEqual(self.run_audit()["components"][0]["notices"][0]["content_role"], "pointer_only")
        self.assertIn("complete_notice_text_missing", self.run_audit()["components"][0]["remaining"])

    def test_wheel_is_not_native_corresponding_source(self):
        self.write("cache/library.whl", b"opaque binary wheel")
        self.write_json("receipts/lock.json", {"assets": []})
        component = self.inventory["components"][0]
        component["package"]["source"] = {"kind": "locked-archive", "archive_path": "cache/library.whl",
                                            "archive_sha256": self.sha("cache/library.whl"),
                                            "url": "https://files.pythonhosted.org/library.whl",
                                            "lock_path": "receipts/lock.json", "lock_sha256": self.sha("receipts/lock.json")}
        self.write_json("inventory.json", self.inventory)
        result = self.run_audit()["components"][0]
        self.assertEqual(result["source_status"], "missing_source")
        self.assertEqual(result["binary_archive"]["sha256"], self.sha("cache/library.whl"))

    def test_publication_failure_preserves_existing_report(self):
        target = self.root / "previous-report.json"
        target.write_bytes(b"previous manual evidence\n")
        with mock.patch.object(audit.os, "replace", side_effect=OSError("simulated publication failure")):
            with self.assertRaisesRegex(OSError, "simulated"):
                audit.publish(target, self.run_audit())
        self.assertEqual(target.read_bytes(), b"previous manual evidence\n")
        self.assertEqual(list(self.root.glob("previous-report.json.*.tmp")), [])

    def test_frozen_bundle_source_is_bound_independently_of_live_workspace(self):
        self.write("src/unit.cpp", b"original source")
        component = self.inventory["components"][0]
        component["package"]["source"] = {"kind": "workspace", "source_paths": [
            {"kind": "file", **self.record("src/unit.cpp")}]}
        self.frozen_bundle(("src/unit.cpp",))
        self.write("src/unit.cpp", b"subsequent source edit")
        result = self.run_audit(bundle_root="bundle")
        self.assertEqual(result["components"][0]["source_status"], "exact_local_source_present")
        self.assertEqual(result["candidate_binding"]["binaries"][0]["path"], "bundle/bin/app.dll")
        self.write("bundle/source-kit/src/unit.cpp", b"corrupted staged source")
        with self.assertRaisesRegex(ValueError, "hash"):
            self.run_audit(bundle_root="bundle")

    def test_frozen_binary_notice_and_asset_ignore_live_changes(self):
        self.write("candidate/config.txt", b"candidate asset")
        self.inventory["components"][0]["source_inputs"] = [self.record("candidate/config.txt")]
        self.frozen_bundle()
        expected = self.sha("candidate/app.dll")
        for path in ("candidate/app.dll", "notices/dependency.txt", "candidate/config.txt"):
            self.write(path, b"subsequent live build")
        result = self.run_audit(bundle_root="bundle")
        binary = result["candidate_binding"]["binaries"][0]
        self.assertEqual((binary["path"], binary["inventory_path"], binary["sha256"]),
                         ("bundle/bin/app.dll", "candidate/app.dll", expected))
        self.assertEqual(result["components"][0]["notices"][0]["path"], "bundle/licenses/notice-0.txt")
        self.assertEqual(result["components"][0]["assets"][0]["path"], "bundle/assets/asset-0.bin")
        with self.assertRaisesRegex(ValueError, "hash"):
            self.run_audit()

    def test_frozen_payload_changes_and_late_drift_fail(self):
        self.write("src/extra.txt", b"unconsumed frozen source payload")
        self.write("candidate/config.txt", b"candidate asset")
        self.inventory["components"][0]["source_inputs"] = [self.record("candidate/config.txt")]
        self.frozen_bundle(("src/extra.txt",))
        for path in ("bundle/bin/app.dll", "bundle/licenses/notice-0.txt", "bundle/assets/asset-0.bin"):
            with self.subTest(path=path):
                original = (self.root / path).read_bytes()
                self.write(path, b"tampered frozen bytes")
                with self.assertRaisesRegex(ValueError, "hash"):
                    self.run_audit(bundle_root="bundle")
                self.write(path, original)
        cache = audit.source_cache
        for path in ("bundle/source-kit/src/extra.txt", "bundle/metadata/portable.json"):
            with self.subTest(late_drift=path):
                original = (self.root / path).read_bytes()
                def mutate_after_binding(*args, **kwargs):
                    result = cache(*args, **kwargs)
                    self.write(path, b"late tamper after frozen mapping admission")
                    return result
                with mock.patch.object(audit, "source_cache", side_effect=mutate_after_binding):
                    with self.assertRaisesRegex(ValueError, "hash"):
                        self.run_audit(bundle_root="bundle")
                self.write(path, original)

    def test_frozen_mapping_missing_ambiguous_and_mismatched_fail(self):
        manifest, portable = self.frozen_bundle()
        for change in ("missing", "notice_missing", "duplicate_source", "source", "component", "destination", "kind", "hash"):
            with self.subTest(change=change):
                mutated = copy.deepcopy(portable)
                binary = mutated["files"][0]
                if change == "missing": mutated["files"].pop(0)
                elif change == "notice_missing": mutated["files"].pop(1)
                elif change == "duplicate_source": mutated["files"].append(copy.deepcopy(binary))
                elif change == "source": binary["source"] = "other/app.dll"
                elif change == "component": binary["component_id"] = "other"
                elif change == "destination": binary["path"] = "other/app.dll"
                elif change == "kind": binary["kind"] = "asset"
                elif change == "hash": binary["sha256"] = "0" * 64
                self.rewrite_mapping(copy.deepcopy(manifest), mutated)
                with self.assertRaises(ValueError):
                    self.run_audit(bundle_root="bundle")
        self.rewrite_mapping(manifest, portable)

    def test_frozen_mapping_rejects_one_original_notice_at_two_destinations(self):
        manifest, portable = self.frozen_bundle()
        extra = {**portable["files"][1], "path": "licenses/alternate.txt"}
        portable["files"].append(extra)
        self.write("bundle/" + extra["path"], (self.root / "notices/dependency.txt").read_bytes())
        manifest["files"].append({key: extra[key] for key in ("path", "sha256", "component_id", "kind", "install")})
        runtime = {"schema_version": 1, "files": [row for row in manifest["files"] if row.get("install")]}
        self.write_json("bundle/runtime.json", runtime)
        manifest["runtime_manifest"]["sha256"] = self.sha("bundle/runtime.json")
        next(row for row in manifest["files"] if row["kind"] == "runtime-manifest")["sha256"] = self.sha("bundle/runtime.json")
        self.rewrite_mapping(manifest, portable)
        with self.assertRaisesRegex(ValueError, "ambiguous"):
            self.run_audit(bundle_root="bundle")

    def test_frozen_mapping_rejects_component_destination_disagreement(self):
        self.inventory["components"][0]["destinations"]["app.dll"] = "bin/other.dll"
        self.frozen_bundle()
        with self.assertRaisesRegex(ValueError, "destination"):
            self.run_audit(bundle_root="bundle")

    def test_duplicate_ids_unsafe_paths_and_duplicate_json_keys_fail_closed(self):
        self.inventory["components"].append(copy.deepcopy(self.inventory["components"][0]))
        self.write_json("inventory.json", self.inventory)
        with self.assertRaisesRegex(ValueError, "duplicate"):
            self.run_audit()
        self.inventory["components"].pop()
        self.inventory["binaries"][0]["path"] = "../outside.dll"
        self.write_json("inventory.json", self.inventory)
        with self.assertRaises(ValueError):
            self.run_audit()
        self.write("inventory.json", b'{"schema_version":1,"schema_version":1}')
        with self.assertRaisesRegex(ValueError, "Duplicate"):
            self.run_audit()

    def test_build_receipts_are_bound_without_exposing_machine_paths(self):
        self.write("build/CMakeCache.txt", b"CMAKE_BUILD_TYPE:STRING=Release\nCMAKE_CXX_COMPILER:FILEPATH=C:/private/compiler.exe\n")
        result = self.run_audit(build_receipts=("build/CMakeCache.txt",))
        self.assertEqual(result["candidate_binding"]["build_receipts"][0]["sha256"], self.sha("build/CMakeCache.txt"))
        self.assertNotIn("C:/private", json.dumps(result))
        self.assertIn("build_and_relinking_evidence_not_qualified", result["components"][0]["remaining"])

    def test_links_and_reparse_inputs_are_rejected(self):
        target = self.root / "notice-link.txt"
        try:
            target.symlink_to(self.root / "notices/dependency.txt")
        except OSError:
            self.skipTest("symlink permission unavailable")
        self.inventory["components"][0]["notices"] = [{"path": "notice-link.txt", "sha256": self.sha("notices/dependency.txt")}]
        self.write_json("inventory.json", self.inventory)
        with self.assertRaisesRegex(ValueError, "link|reparse"):
            self.run_audit()


if __name__ == "__main__":
    unittest.main()
