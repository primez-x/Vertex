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
        for limit, value, message in (("MAX_FILES", 1, "file count"), ("MAX_CACHE_BYTES", 1, "bytes"),
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
