import importlib.util
import json
import pathlib
import tempfile
import unittest


SCRIPT = pathlib.Path(__file__).resolve().parents[1] / "scripts/source_provenance_audit.py"
SPEC = importlib.util.spec_from_file_location("source_provenance_audit", SCRIPT)
audit = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(audit)


class SourceProvenanceAuditTests(unittest.TestCase):
    def test_repository_manifest_is_deterministic_and_complete(self):
        root = pathlib.Path(__file__).resolve().parents[1]
        first = audit.audit_repository(root)
        second = audit.audit_repository(root)
        self.assertEqual(first, second)
        self.assertEqual(first["audit_status"], "pass")
        self.assertGreater(first["tracked_path_count"], 0)
        self.assertGreater(first["ownership_counts"]["first_party"], 0)
        self.assertGreater(first["ownership_counts"]["third_party_provenance"], 0)
        self.assertEqual(first["errors"], [])

    def test_paths_must_have_exactly_one_declared_owner(self):
        manifest = audit.default_manifest()
        report = audit.audit_paths(["src/main.cpp", "third_party/dependencies.json"], manifest)
        self.assertEqual(report["audit_status"], "pass")

        overlapping = json.loads(json.dumps(manifest))
        overlapping["ownership"]["first_party"]["roots"].append("third_party/")
        report = audit.audit_paths(["third_party/dependencies.json"], overlapping)
        self.assertEqual(report["audit_status"], "blocked")
        self.assertTrue(any("overlap" in item for item in report["errors"]))

        prefix_collision = json.loads(json.dumps(manifest))
        prefix_collision["ownership"]["first_party"]["roots"].append("asset/")
        report = audit.audit_paths(["assets-extra/readme.txt"], prefix_collision)
        self.assertEqual(report["audit_status"], "blocked")
        self.assertTrue(any("no ownership rule" in item for item in report["errors"]))

        root_file_overlap = json.loads(json.dumps(manifest))
        root_file_overlap["ownership"]["external_excluded"]["files"].append("src/owned.cpp")
        report = audit.audit_paths(["src/owned.cpp"], root_file_overlap)
        self.assertEqual(report["audit_status"], "blocked")
        self.assertTrue(any("overlap" in item for item in report["errors"]))

    def test_unclassified_and_external_paths_fail_closed(self):
        manifest = audit.default_manifest()
        report = audit.audit_paths(["unknown/file.txt"], manifest)
        self.assertEqual(report["audit_status"], "blocked")
        self.assertTrue(any("no ownership rule" in item for item in report["errors"]))

        report = audit.audit_paths([".deps/native/x64-windows/bin/TKernel.dll"], manifest)
        self.assertEqual(report["audit_status"], "blocked")
        self.assertTrue(any("external" in item for item in report["errors"]))

    def test_unsafe_paths_and_duplicate_json_keys_are_rejected(self):
        manifest = audit.default_manifest()
        report = audit.audit_paths(["../escape.txt"], manifest)
        self.assertEqual(report["audit_status"], "blocked")
        self.assertTrue(any("unsafe" in item for item in report["errors"]))

        with tempfile.TemporaryDirectory() as directory:
            path = pathlib.Path(directory) / "duplicate.json"
            path.write_text('{"schema_version":"1.0","schema_version":"1.0"}', encoding="utf-8")
            with self.assertRaises(ValueError):
                audit.load_manifest(path)

    def test_missing_provenance_manifest_is_reported(self):
        with tempfile.TemporaryDirectory() as directory:
            root = pathlib.Path(directory)
            (root / "scripts").mkdir()
            report = audit.audit_repository(root)
        self.assertEqual(report["audit_status"], "blocked")
        self.assertTrue(report["errors"])


if __name__ == "__main__":
    unittest.main()
