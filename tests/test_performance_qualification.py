import copy
import hashlib
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
from unittest import mock

SCRIPTS = Path(__file__).resolve().parents[1] / "scripts"
sys.path.insert(0, str(SCRIPTS))
import performance_qualification as qualification
from performance_report import THRESHOLDS_MS


class QualificationTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.manifest = {
            "schema": "vertex-performance-qualification", "schema_version": 1,
            "run_id": "run-1", "timestamp": "2026-09-19T12:00:00Z",
            "operator": "test operator", "source_revision": "abc123",
            "hardware": {"id": "machine-1", "cpu": "CPU", "gpu": "GPU",
                         "memory": "64 GiB", "os_build": "Windows 26100", "agreed": True},
            "build_artifact": self.bind("build.bin", b"build"),
            "offline_package_manifest": self.bind("offline.json", b'{"files": []}'),
            "workloads": [],
            "cancellation": {"request": "cancel long regeneration", "observation": "worker stopped",
                "latency_ms": 25, "document_revision_before": 7, "document_revision_after": 7,
                "stale_result_discarded": True, "valid_revision_preserved": True},
        }
        for profile, entities, objects, triangles, sheets, asset_bytes in (
                ("drawing", 50000, 0, 0, 0, 0), ("architecture", 10000, 10000, 1000000, 0, 0),
                ("sheets", 21, 0, 0, 20, 250000000)):
            facts = {"id": "vertex-representative-" + profile + "-v1", "entities": entities,
                     "objects": objects, "triangles": triangles, "sheets": sheets, "project_bytes": 7}
            desktop = {"schema_version": 1, "audit_status": "incomplete", "reference_hardware": "machine-1",
                       "workload": facts, "threshold_status": "within_targets", "metrics": {
                           name: {"sample_count": 100 if name in ("navigation", "input", "edit") else 20,
                                  "dropped_sample_count": 0, "has_samples": True, "threshold_ms": threshold,
                                  "p95_ms": threshold, "within_threshold": True}
                           for name, threshold in THRESHOLDS_MS.items()}}
            project = self.bind(profile + ".bldproj", b"project")
            roundtrip = self.bind(profile + ".roundtrip.bldproj", b"project")
            storage = {"schema_version": 1, "audit_status": "incomplete",
                       "generator": "vertex-representative-workload-v1",
                       "provenance": {"reference_hardware": "machine-1", "source_revision": "abc123", "labels_verified": False},
                       "workload": dict(facts, placed_asset_bytes=asset_bytes, drawing_entities=entities if profile == "drawing" else 0,
                                        asset_bytes=asset_bytes, assets=[] if not asset_bytes else [
                                            {"id": "asset-1", "bytes": asset_bytes, "sha256": "a" * 64}], semantic_sha256="b" * 64),
                       "integrity": {"authoring_source_sha256": "c" * 64, "document_id": profile + "-document", "revision": 1,
                                     "snapshot_and_assets_equal_after_both_reopens": True,
                                     "project_sha256": project["sha256"], "roundtrip_sha256": roundtrip["sha256"]},
                       "samples_ms": {"open": [1, 2], "save": [1, 2]}}
            self.manifest["workloads"].append({"profile": profile, "desktop_report": self.bind(profile + "-desktop.json", desktop),
                "storage_report": self.bind(profile + "-storage.json", storage), "project": project, "roundtrip": roundtrip,
                "document_id": profile + "-document", "revision": 1,
                "integrity": {"semantic": True, "snapshot": True, "assets": True, "manifest_validation": True}})
        self.original_manifest = copy.deepcopy(self.manifest)
        self.original_files = {path.name: path.read_bytes() for path in self.root.iterdir()}

    def restore(self):
        self.manifest = copy.deepcopy(self.original_manifest)
        for name, data in self.original_files.items():
            (self.root / name).write_bytes(data)

    def bind(self, name, value):
        data = json.dumps(value).encode() if isinstance(value, dict) else value
        (self.root / name).write_bytes(data)
        return {"path": name, "sha256": hashlib.sha256(data).hexdigest()}

    def change_report(self, index, kind, change):
        entry = self.manifest["workloads"][index]
        path = entry[kind]["path"]
        report = json.loads((self.root / path).read_bytes())
        change(report)
        entry[kind] = self.bind(path, report)

    def run_contract(self):
        self.bind("qualification.json", self.manifest)
        return qualification.qualify(self.root / "qualification.json")

    def assert_gap(self):
        report, code = self.run_contract()
        self.assertEqual(code, 1, report)
        self.assertFalse(report["contract_passed"])
        self.assertTrue(report["errors"])

    def test_success_and_stable_output(self):
        first, code = self.run_contract()
        self.assertEqual(code, 0, first)
        self.assertTrue(first["contract_passed"])
        self.assertFalse(first["production_qualified"])
        self.assertEqual(first["audit_status"], "incomplete")
        self.assertEqual(first, self.run_contract()[0])
        self.assertEqual(len(first["evidence"]), 15)
        self.assertEqual(first["workloads"][0]["sample_counts"]["navigation"], 100)

    def test_each_workload_minimum(self):
        for index, field, value in ((0, "entities", 49999), (1, "objects", 9999), (1, "triangles", 999999),
                                     (2, "sheets", 19), (2, "placed_asset_bytes", 249999999)):
            with self.subTest(field=field):
                self.restore()
                self.change_report(index, "storage_report", lambda r: r["workload"].update({field: value}))
                self.assert_gap()

    def test_inadequate_samples(self):
        for name in THRESHOLDS_MS:
            with self.subTest(name=name):
                self.restore()
                self.change_report(0, "desktop_report", lambda r: r["metrics"][name].update(sample_count=1))
                self.assert_gap()

    def test_threshold_failure(self):
        self.change_report(0, "desktop_report", lambda r: (r["metrics"]["open"].update(p95_ms=5001, within_threshold=False),
                                                           r.update(threshold_status="exceeds_targets")))
        self.assert_gap()

    def test_tampering(self):
        (self.root / "build.bin").write_bytes(b"tampered")
        self.assert_gap()

    def test_traversal_and_windows_path_forms(self):
        for path in ("../build.bin", "C:/build.bin", "\\\\server\\share", "/build.bin", "x:stream", "a/./b", "a//b", "a./b"):
            with self.subTest(path=path):
                self.manifest["build_artifact"]["path"] = path
                report, code = self.run_contract()
                self.assertEqual(code, 2, report)

    def test_link_rejected(self):
        link = self.root / "link.bin"
        try:
            link.symlink_to(self.root / "build.bin")
        except OSError:
            self.skipTest("symlink creation unavailable")
        self.manifest["build_artifact"]["path"] = link.name
        self.assertEqual(self.run_contract()[1], 2)

    def test_duplicate_and_oversized_json(self):
        path = self.root / "qualification.json"
        for data in (b'{"a":1,"a":2}', b'{"x":NaN}', b'{"x":1e999}', b" " * (qualification.MAX_JSON_BYTES + 1)):
            path.write_bytes(data)
            self.assertEqual(qualification.qualify(path)[1], 2)

    def test_report_duplicate_json(self):
        self.manifest["workloads"][0]["storage_report"] = self.bind("bad.json", b'{"a":1,"a":2}')
        self.assertEqual(self.run_contract()[1], 2)

    def test_consistency_and_integrity(self):
        for field, value in (("source_revision", "wrong"), ("reference_hardware", "wrong")):
            self.restore()
            self.change_report(0, "storage_report", lambda r: r["provenance"].update({field: value}))
            self.assert_gap()
        self.restore()
        self.manifest["workloads"][1]["revision"] = 2
        self.assert_gap()
        self.restore()
        self.manifest["workloads"][2]["integrity"]["manifest_validation"] = False
        self.assert_gap()

    def test_storage_integrity_and_shape_failures_individually(self):
        changes = [
            lambda r: r.update(schema_version=True),
            lambda r: r.update(workload=[]),
            lambda r: r.update(provenance=[]),
            lambda r: r["integrity"].update(document_id="wrong"),
            lambda r: r["integrity"].update(revision=True),
            lambda r: r["integrity"].update(project_sha256="a" * 64),
            lambda r: r["integrity"].update(roundtrip_sha256="a" * 64),
            lambda r: r["integrity"].update(authoring_source_sha256="bad"),
            lambda r: r["integrity"].update(snapshot_and_assets_equal_after_both_reopens=False),
            lambda r: r["workload"].update(semantic_sha256="bad"),
            lambda r: r["workload"].update(asset_bytes=1),
            lambda r: r["workload"].update(assets=[None]),
            lambda r: r["samples_ms"].update(open=[-1, 2]),
            lambda r: r["samples_ms"].update(save=[5001, 5002]),
        ]
        for index, change in enumerate(changes):
            with self.subTest(index=index):
                self.restore()
                self.change_report(0, "storage_report", change)
                self.assert_gap()

    def test_each_binding_hash_checked(self):
        for name in self.original_files:
            with self.subTest(file=name):
                self.restore()
                # Keep JSON parseable to isolate the binding failure.
                path = self.root / name
                path.write_bytes(path.read_bytes() + b" ")
                self.assert_gap()

    def test_duplicates_and_dropped_samples(self):
        self.manifest["workloads"][2] = copy.deepcopy(self.manifest["workloads"][0])
        self.assert_gap()
        self.restore()
        self.change_report(0, "desktop_report", lambda r: r["metrics"]["navigation"].update(dropped_sample_count=1))
        self.assert_gap()

    def test_missing_files_and_artifact_bounds(self):
        (self.root / "build.bin").unlink()
        self.assertEqual(self.run_contract()[1], 2)
        self.restore()
        with mock.patch.object(qualification, "MAX_ARTIFACT_BYTES", 4):
            self.assertEqual(self.run_contract()[1], 2)

    def test_manifest_link_and_directory_link(self):
        self.run_contract()
        link = self.root / "manifest-link.json"
        try:
            link.symlink_to(self.root / "qualification.json")
            (self.root / "alias").symlink_to(self.root, target_is_directory=True)
        except OSError:
            self.skipTest("symlink creation unavailable")
        self.assertEqual(qualification.qualify(link)[1], 2)
        self.manifest["build_artifact"]["path"] = "alias/build.bin"
        self.assertEqual(self.run_contract()[1], 2)

    def test_windows_reparse_attribute_rejected(self):
        fake = mock.Mock(st_mode=0o100644, st_file_attributes=0x400)
        with mock.patch.object(Path, "lstat", return_value=fake):
            with self.assertRaisesRegex(qualification.InputError, "reparse"):
                qualification._check_path(self.root / "build.bin")

    def test_placed_asset_claim_requires_assets(self):
        self.change_report(2, "storage_report", lambda r: r["workload"].update(assets=[], asset_bytes=0))
        self.assert_gap()
        self.restore()
        self.change_report(2, "storage_report", lambda r: r["workload"].update(
            assets=[{"id": "asset-1", "bytes": 1, "sha256": "a" * 64}], asset_bytes=1))
        self.assert_gap()

    def test_cancellation_failures(self):
        original = copy.deepcopy(self.manifest["cancellation"])
        for key, value in (("request", ""), ("observation", ""), ("latency_ms", -1), ("latency_ms", True),
                           ("document_revision_before", True), ("document_revision_after", 8),
                           ("stale_result_discarded", False), ("valid_revision_preserved", False)):
            self.manifest["cancellation"] = dict(original, **{key: value})
            self.assert_gap()

    def test_workload_identity_and_paths_must_be_distinct(self):
        self.manifest["workloads"][1]["document_id"] = self.manifest["workloads"][0]["document_id"]
        self.change_report(1, "storage_report", lambda report: report["integrity"].update(
            document_id=self.manifest["workloads"][0]["document_id"]))
        self.assert_gap()
        self.restore()
        self.manifest["workloads"][1]["project"] = copy.deepcopy(self.manifest["workloads"][0]["project"])
        self.assert_gap()
        self.restore()
        self.manifest["workloads"][0]["roundtrip"] = copy.deepcopy(self.manifest["workloads"][0]["project"])
        self.change_report(0, "storage_report", lambda report: report["integrity"].update(
            roundtrip_sha256=self.manifest["workloads"][0]["project"]["sha256"]))
        self.assert_gap()

    def test_hardlink_and_windows_case_alias_cannot_reuse_project_file(self):
        source = self.root / self.manifest["workloads"][0]["project"]["path"]
        alias = self.root / "architecture-project-alias.bldproj"
        try:
            os.link(source, alias)
        except OSError:
            self.skipTest("hard links unavailable")
        self.manifest["workloads"][1]["project"] = {
            "path": alias.name,
            "sha256": hashlib.sha256(alias.read_bytes()).hexdigest(),
        }
        self.assert_gap()

        if os.name == "nt":
            self.restore()
            original = self.manifest["workloads"][0]["project"]
            self.manifest["workloads"][1]["project"] = {
                "path": original["path"].upper(),
                "sha256": original["sha256"],
            }
            self.assert_gap()

    def test_labels_are_bounded(self):
        self.manifest["operator"] = "x" * 4097
        self.assert_gap()

    def test_missing_and_wrong_shapes(self):
        for key in ("schema", "schema_version", "run_id", "timestamp", "operator", "source_revision", "hardware", "workloads", "cancellation"):
            original = self.manifest.pop(key)
            self.assert_gap()
            self.manifest[key] = original
        self.manifest["workloads"] = [None]
        self.assert_gap()

    def test_cli_deterministic(self):
        self.run_contract()
        command = [sys.executable, str(SCRIPTS / "performance_qualification.py"), str(self.root / "qualification.json")]
        first = subprocess.run(command, capture_output=True, check=False)
        second = subprocess.run(command, capture_output=True, check=False)
        self.assertEqual(first.returncode, 0, first.stderr)
        self.assertEqual(first.stdout, second.stdout)
        self.assertTrue(json.loads(first.stdout)["contract_passed"])


if __name__ == "__main__":
    unittest.main()
