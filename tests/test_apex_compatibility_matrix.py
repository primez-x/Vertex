import copy
import hashlib
import importlib.util
import pathlib
import tempfile
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[1]
SPEC = importlib.util.spec_from_file_location("matrix", ROOT / "scripts/apex_compatibility_matrix.py")
matrix = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(matrix)


class CompatibilityMatrixTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = pathlib.Path(self.temp.name)
        (self.root / "synthetic.bin").write_bytes(b"synthetic evidence, not an Apex file")
        self.file = {"path": "synthetic.bin", "sha256": hashlib.sha256((self.root / "synthetic.bin").read_bytes()).hexdigest()}
        descriptor = {"version": "synthetic", "build": "test", "settings": "test",
                      "observed_behavior": "synthetic test only", "modules": [], "inventory_evidence": self.file}
        self.manifest = {"schema_version": "1.0", "editions": {"Standard": copy.deepcopy(descriptor), "Pro": copy.deepcopy(descriptor)}, "fixtures": []}
        for index, requirement in enumerate(matrix.REQUIREMENTS):
            fixture = {key: "synthetic test" for key in ("provenance", "permissions", "operation", "settings", "observed_behavior", "loss_report")}
            fixture.update({"id": str(index), "requirement": requirement, "edition": "Standard" if index % 2 else "Pro", "modules": [],
                            "native_version": "v5" if "AX5" in requirement else "v7",
                            "fields": {field: {"expected": "test", "observed": "test", "classification": "match", "reason": "synthetic test"} for field in matrix.FIELDS}})
            for key in ("native_source", "replacement_project", "expected_output", "observed_output"):
                fixture[key] = copy.deepcopy(self.file)
            self.manifest["fixtures"].append(fixture)

    def report(self):
        return matrix.validate_and_build(self.manifest, root=self.root)

    def test_complete_synthetic_evidence_never_certifies(self):
        report = self.report()
        self.assertTrue(report["evidence_complete"], report["errors"])
        self.assertEqual(report["audit_status"], "incomplete")
        self.assertFalse(report["compatibility_passed"])
        self.assertEqual(report, self.report())
        self.manifest["fixtures"].reverse()
        self.assertEqual(report, self.report())

    def test_missing_editions_modules_native_files_and_rows_fail_closed(self):
        mutations = (
            lambda m: m["editions"].pop("Standard"),
            lambda m: m["editions"]["Pro"].pop("modules"),
            lambda m: m["fixtures"][0].pop("native_source"),
            lambda m: m["fixtures"].pop(),
            lambda m: m["fixtures"][0].update(modules=["absent"]),
        )
        original = copy.deepcopy(self.manifest)
        for mutation in mutations:
            with self.subTest(mutation=mutation):
                self.manifest = copy.deepcopy(original)
                mutation(self.manifest)
                self.assertFalse(self.report()["evidence_complete"])

    def test_hash_and_path_validation(self):
        for path in ("../synthetic.bin", "C:synthetic.bin", "\\\\host\\share\\file", "/tmp/file", "missing.bin"):
            with self.subTest(path=path):
                self.manifest["fixtures"][0]["native_source"]["path"] = path
                self.assertFalse(self.report()["evidence_complete"])
        self.manifest["fixtures"][0]["native_source"] = dict(self.file, sha256="0" * 64)
        self.assertFalse(self.report()["evidence_complete"])

    def test_output_mismatch_is_reported_without_certification(self):
        (self.root / "different.bin").write_bytes(b"different")
        self.manifest["fixtures"][0]["observed_output"] = {"path": "different.bin", "sha256": hashlib.sha256(b"different").hexdigest()}
        report = self.report()
        self.assertTrue(report["evidence_complete"])
        self.assertFalse(report["comparisons"][0]["output_hash_match"])
        self.assertFalse(report["compatibility_passed"])

    def test_missing_behavior_classification_and_native_version_rejected(self):
        self.manifest["fixtures"][0]["fields"]["geometry"].pop("classification")
        self.manifest["fixtures"][0].pop("observed_behavior")
        self.manifest["fixtures"][2]["native_version"] = "unknown"
        self.assertFalse(self.report()["evidence_complete"])

    def test_duplicate_json_keys_rejected(self):
        path = self.root / "manifest.json"
        path.write_text('{"schema_version":"1.0","schema_version":"1.0"}')
        with self.assertRaises(ValueError):
            matrix.load_manifest(path)

    def test_malformed_structures_fail_closed(self):
        for invalid in (None, [], {"schema_version": "1.0", "editions": [], "fixtures": [None]}):
            with self.subTest(invalid=invalid):
                self.assertFalse(matrix.validate_and_build(invalid, root=self.root)["evidence_complete"])


if __name__ == "__main__":
    unittest.main()
