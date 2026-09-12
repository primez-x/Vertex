import copy
import hashlib
import importlib.util
import json
import pathlib
import subprocess
import sys
import tempfile
import unittest

SCRIPT = pathlib.Path(__file__).resolve().parents[1] / "scripts/production_qualification.py"
SPEC = importlib.util.spec_from_file_location("qualification", SCRIPT)
qa = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(qa)


class QualificationTests(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory()
        self.addCleanup(temporary.cleanup)
        self.root = pathlib.Path(temporary.name)
        (self.root / "synthetic.bin").write_bytes(b"synthetic contract test only")
        artifact = {"path": "synthetic.bin", "sha256": hashlib.sha256(b"synthetic contract test only").hexdigest()}
        def run(requirement, scale=None):
            checks = qa.PRODUCTION.get(requirement, qa.ACCESSIBILITY + qa.CORE)
            item = {"id": requirement + str(scale), "requirement": requirement,
                    "evidence_kind": "synthetic", "operator": "unit test", "recorded_at": "2026-09-11T12:00:00Z",
                    "environment": "synthetic Windows device inventory", "device": "synthetic input device",
                    "provenance": "generated unit test; no runtime evidence", "application": copy.deepcopy(artifact),
                    "source_project": copy.deepcopy(artifact), "reopened_project": copy.deepcopy(artifact),
                    "observations": {name: {"expected": "synthetic expected", "observed": "synthetic observed",
                        "status": "pass", "evidence": copy.deepcopy(artifact)} for name in checks}}
            if scale is not None:
                item["dpi_percent"] = scale
            return item
        self.manifest = {"schema_version": "1.0", "runs": [run(key) for key in qa.PRODUCTION] +
                         [run("OPS-QA-005", scale) for scale in (100, 150, 200)]}

    def report(self):
        return qa.validate_and_build(self.manifest, root=self.root)

    def declare_real_role_artifacts(self):
        role_artifacts = {}
        for name, payload in (("application.exe", b"application binary"),
                              ("source.bldproj", b"source project"),
                              ("reopened.bldproj", b"reopened project"),
                              ("observation.json", b"observation evidence")):
            path = self.root / name
            path.write_bytes(payload)
            role_artifacts[name] = {"path": name, "sha256": hashlib.sha256(payload).hexdigest()}
        for run in self.manifest["runs"]:
            run["evidence_kind"] = "real"
            run["application"] = copy.deepcopy(role_artifacts["application.exe"])
            run["source_project"] = copy.deepcopy(role_artifacts["source.bldproj"])
            run["reopened_project"] = copy.deepcopy(role_artifacts["reopened.bldproj"])
            for observation in run["observations"].values():
                observation["evidence"] = copy.deepcopy(role_artifacts["observation.json"])

    def test_synthetic_never_qualifies_and_is_deterministic(self):
        report = self.report()
        self.assertTrue(report["contract_valid"], report["errors"])
        self.assertFalse(report["real_evidence_complete"])
        self.assertFalse(report["qualification_passed"])
        self.assertEqual(report["audit_status"], "incomplete")
        self.manifest["runs"].reverse()
        self.assertEqual(report, self.report())

    def test_declared_real_evidence_still_needs_independent_review(self):
        self.declare_real_role_artifacts()
        report = self.report()
        self.assertTrue(report["real_evidence_complete"])
        self.assertFalse(report["qualification_passed"])
        self.assertEqual(report["audit_status"], "incomplete")

    def test_real_evidence_requires_distinct_nonempty_role_artifacts(self):
        for run in self.manifest["runs"]:
            run["evidence_kind"] = "real"
        report = self.report()
        self.assertFalse(report["contract_valid"])
        self.assertTrue(any("distinct" in error for error in report["errors"]))

        empty = self.root / "empty.bin"
        empty.write_bytes(b"")
        self.manifest["runs"][0]["application"] = {
            "path": "empty.bin", "sha256": hashlib.sha256(b"").hexdigest()
        }
        report = self.report()
        self.assertFalse(report["contract_valid"])
        self.assertTrue(any("empty" in error for error in report["errors"]))

    def test_real_observation_evidence_cannot_reuse_role_artifacts(self):
        self.declare_real_role_artifacts()
        run = self.manifest["runs"][0]
        run["observations"]["create"]["evidence"] = copy.deepcopy(run["application"])
        report = self.report()
        self.assertFalse(report["contract_valid"])
        self.assertTrue(any("observation evidence" in error for error in report["errors"]))

    def test_missing_coverage_identity_and_observations_fail_closed(self):
        original = copy.deepcopy(self.manifest)
        mutations = [lambda m: m["runs"].pop(), lambda m: m["runs"][0].pop("source_project"),
                     lambda m: m["runs"][0]["observations"].pop("recovery"),
                     lambda m: m["runs"][2].pop("device"),
                     lambda m: m["runs"][2].update(dpi_percent=True),
                     lambda m: m["runs"][0].update(recorded_at="yesterday"),
                     lambda m: m["runs"].append(copy.deepcopy(m["runs"][0]))]
        for mutation in mutations:
            self.manifest = copy.deepcopy(original)
            mutation(self.manifest)
            self.assertFalse(self.report()["contract_valid"])

    def test_failure_and_blocked_observations_prevent_complete_evidence(self):
        self.declare_real_role_artifacts()
        for status in ("fail", "blocked", "not_run"):
            self.manifest["runs"][0]["observations"]["recovery"]["status"] = status
            report = self.report()
            self.assertTrue(report["contract_valid"], report["errors"])
            self.assertFalse(report["real_evidence_complete"])
            self.assertTrue(report["blockers"])

    def test_unsafe_missing_and_tampered_artifacts(self):
        original = copy.deepcopy(self.manifest)
        for path in ("../synthetic.bin", "C:synthetic.bin", "//host/share/file", "/tmp/file", "missing", "file:stream"):
            self.manifest = copy.deepcopy(original)
            self.manifest["runs"][0]["application"]["path"] = path
            self.assertFalse(self.report()["contract_valid"])
        self.manifest = original
        (self.root / "synthetic.bin").write_bytes(b"changed")
        self.assertFalse(self.report()["contract_valid"])

    def test_json_duplicate_keys_nonfinite_and_malformed_values(self):
        path = self.root / "manifest.json"
        for content in ('{"runs":[],"runs":[]}', '{"number":NaN}', '{"number":Infinity}', '{"number":1e999}'):
            path.write_text(content)
            with self.assertRaises(ValueError):
                qa.load_manifest(path)
        for value in (None, [], {"schema_version": "1.0", "runs": [None, {"requirement": []}]}):
            self.assertFalse(qa.validate_and_build(value, root=self.root)["contract_valid"])

    def test_unknown_observation_cannot_hide_failure(self):
        self.manifest["runs"][0]["observations"]["misspelled_check"] = {"status": "fail"}
        self.assertFalse(self.report()["contract_valid"])

    def test_cli_synthetic_and_parse_errors_are_nonzero_json(self):
        path = self.root / "manifest.json"
        for content in (json.dumps(self.manifest), "{"):
            path.write_text(content)
            result = subprocess.run([sys.executable, str(SCRIPT), str(path), "--root", str(self.root)], capture_output=True, text=True)
            self.assertNotEqual(result.returncode, 0)
            self.assertFalse(json.loads(result.stdout)["qualification_passed"])


if __name__ == "__main__":
    unittest.main()
