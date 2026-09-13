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
        def reference(run, role, payload):
            name = run["id"] + "-" + role.replace(":", "-") + ".bin"
            path = self.root / name
            path.write_bytes(payload)
            return {"path": name, "sha256": hashlib.sha256(payload).hexdigest(),
                    "run_id": run["id"], "role": role, "media_type": "application/octet-stream",
                    "content_description": "Synthetic contract fixture for " + role}
        for run in self.manifest["runs"]:
            run["evidence_kind"] = "real"
            for role in ("application", "source_project", "reopened_project"):
                run[role] = reference(run, role, role.encode())
            for name, observation in run["observations"].items():
                observation["evidence"] = reference(run, "observation:" + name, (run["id"] + name).encode())
            record = {"schema_version": "1.0", "run_id": run["id"], "requirement": run["requirement"],
                      "recorded_at": run["recorded_at"], "dpi_percent": run.get("dpi_percent"),
                      "process": {"pid": 123, "exit_code": 0, "application_sha256": run["application"]["sha256"]},
                      "artifacts": {role: run[role]["sha256"] for role in ("application", "source_project", "reopened_project")},
                      "observations": {name: {"evidence_sha256": item["evidence"]["sha256"],
                          "status": item["status"], "expected": item["expected"], "observed": item["observed"]}
                          for name, item in run["observations"].items()}}
            run["execution"] = reference(run, "execution", json.dumps(record).encode())
            run["execution"]["media_type"] = "application/json"

    def rewrite_execution(self, run, mutate):
        path = self.root / run["execution"]["path"]
        record = json.loads(path.read_text())
        mutate(record)
        payload = json.dumps(record).encode()
        path.write_bytes(payload)
        run["execution"]["sha256"] = hashlib.sha256(payload).hexdigest()

    def test_synthetic_never_qualifies_and_is_deterministic(self):
        report = self.report()
        self.assertTrue(report["contract_valid"], report["errors"])
        self.assertFalse(report["real_evidence_complete"])
        self.assertFalse(report["qualification_passed"])
        self.assertFalse(report["production_accepted"])
        self.assertEqual(report["audit_status"], "incomplete")
        self.manifest["runs"].reverse()
        self.assertEqual(report, self.report())

    def test_declared_real_evidence_still_needs_independent_review(self):
        self.declare_real_role_artifacts()
        report = self.report()
        self.assertTrue(report["real_evidence_complete"])
        self.assertFalse(report["qualification_passed"])
        self.assertEqual(report["audit_status"], "incomplete")
        self.assertFalse(report["production_accepted"])
        self.manifest["runs"].reverse()
        self.assertEqual(report, self.report())

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

    def test_real_references_require_run_role_and_content_declarations(self):
        self.declare_real_role_artifacts()
        original = copy.deepcopy(self.manifest)
        for field in ("run_id", "role", "media_type", "content_description"):
            self.manifest = copy.deepcopy(original)
            self.manifest["runs"][0]["application"].pop(field)
            self.assertFalse(self.report()["contract_valid"], field)
        self.manifest = copy.deepcopy(original)
        self.manifest["runs"][0]["observations"]["create"]["evidence"]["role"] = "observation:edit"
        self.assertFalse(self.report()["contract_valid"])

    def test_real_execution_required_and_bound_to_run_process_and_observations(self):
        for mutate in (lambda run: run.pop("execution"),
                       lambda run: self.rewrite_execution(run, lambda record: record.update(run_id="other-run")),
                       lambda run: self.rewrite_execution(run, lambda record: record["process"].update(pid=True)),
                       lambda run: self.rewrite_execution(run, lambda record: record["process"].update(application_sha256="0" * 64)),
                       lambda run: self.rewrite_execution(run, lambda record: record["observations"].pop("recovery")),
                       lambda run: self.rewrite_execution(run, lambda record: record["observations"]["create"].update(status="fail"))):
            self.declare_real_role_artifacts()
            mutate(self.manifest["runs"][0])
            self.assertFalse(self.report()["contract_valid"])

    def test_real_observation_content_cannot_be_reused_across_runs(self):
        self.declare_real_role_artifacts()
        first, second = self.manifest["runs"][:2]
        evidence = second["observations"]["create"]["evidence"]
        original = first["observations"]["create"]["evidence"]
        (self.root / evidence["path"]).write_bytes((self.root / original["path"]).read_bytes())
        evidence["sha256"] = original["sha256"]
        self.rewrite_execution(second, lambda record: record["observations"]["create"].update(evidence_sha256=evidence["sha256"]))
        report = self.report()
        self.assertFalse(report["contract_valid"])
        self.assertTrue(any("reused across runs" in error for error in report["errors"]))

    def test_renamed_application_content_cannot_be_a_project(self):
        self.declare_real_role_artifacts()
        run = self.manifest["runs"][0]
        application, project = run["application"], run["source_project"]
        (self.root / project["path"]).write_bytes((self.root / application["path"]).read_bytes())
        project["sha256"] = application["sha256"]
        self.rewrite_execution(run, lambda record: record["artifacts"].update(source_project=project["sha256"]))
        self.assertFalse(self.report()["contract_valid"])

    def test_execution_invalid_json_and_nonzero_exit_fail_closed(self):
        for payload in (b"not JSON", b'{"run_id":"one","run_id":"two"}', b"\xff"):
            self.declare_real_role_artifacts()
            execution = self.manifest["runs"][0]["execution"]
            (self.root / execution["path"]).write_bytes(payload)
            execution["sha256"] = hashlib.sha256(payload).hexdigest()
            self.assertFalse(self.report()["contract_valid"])
        self.declare_real_role_artifacts()
        self.rewrite_execution(self.manifest["runs"][0], lambda record: record["process"].update(exit_code=1))
        report = self.report()
        self.assertTrue(report["contract_valid"], report["errors"])
        self.assertFalse(report["real_evidence_complete"])

    def test_missing_coverage_identity_and_observations_fail_closed(self):
        original = copy.deepcopy(self.manifest)
        mutations = [lambda m: m["runs"].pop(), lambda m: m["runs"][0].pop("source_project"),
                     lambda m: m["runs"][0]["observations"].pop("recovery"),
                     lambda m: next(run for run in m["runs"]
                                    if run["requirement"] == "OPS-QA-005" and run.get("dpi_percent") == 100).pop("device"),
                     lambda m: next(run for run in m["runs"]
                                    if run["requirement"] == "OPS-QA-005" and run.get("dpi_percent") == 100).update(dpi_percent=True),
                     lambda m: m["runs"][0].update(recorded_at="yesterday"),
                     lambda m: m["runs"].append(copy.deepcopy(m["runs"][0]))]
        for mutation in mutations:
            self.manifest = copy.deepcopy(original)
            mutation(self.manifest)
            self.assertFalse(self.report()["contract_valid"])

    def test_integrated_offline_workflow_requires_explicit_boundary_observations(self):
        run = next(run for run in self.manifest["runs"] if run["requirement"] == "OPS-QA-004")
        self.assertEqual(set(run["observations"]), set(qa.PRODUCTION["OPS-QA-004"]))
        for name in ("packaged_install", "network_denied", "assistance_disabled"):
            manifest = copy.deepcopy(self.manifest)
            target = next(item for item in manifest["runs"] if item["requirement"] == "OPS-QA-004")
            target["observations"].pop(name)
            self.assertFalse(qa.validate_and_build(manifest, root=self.root)["contract_valid"], name)

    def test_symbol_library_observations_are_mandatory_for_production_workspaces(self):
        required = {"symbol_library", "symbol_resize", "symbol_output"}
        for requirement in ("OPS-QA-002", "OPS-QA-003"):
            self.assertTrue(required.issubset(set(qa.PRODUCTION[requirement])))
            manifest = copy.deepcopy(self.manifest)
            run = next(item for item in manifest["runs"] if item["requirement"] == requirement)
            for name in required:
                run["observations"].pop(name, None)
            report = qa.validate_and_build(manifest, root=self.root)
            self.assertFalse(report["contract_valid"], (requirement, report["errors"]))

    def test_failure_and_blocked_observations_prevent_complete_evidence(self):
        self.declare_real_role_artifacts()
        for status in ("fail", "blocked", "not_run"):
            self.manifest["runs"][0]["observations"]["recovery"]["status"] = status
            self.rewrite_execution(self.manifest["runs"][0], lambda record: record["observations"]["recovery"].update(status=status))
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
