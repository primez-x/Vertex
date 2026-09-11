import copy
import hashlib
import importlib.util
import json
import pathlib
import sys
import tempfile
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location("requirement_audit", ROOT / "scripts/requirement_audit.py")
audit = importlib.util.module_from_spec(spec)
spec.loader.exec_module(audit)


class ProductionGateTests(unittest.TestCase):
    def setUp(self):
        self.ledger = {"requirements": [{"id": "REQ-1", "area": "core", "requirement": "Save exactly",
            "evidence_status": "documented", "implementation_status": "not_started", "acceptance": "Reopen same revision",
            "package": 3, "blocker": None, "source_urls": []}]}
        self.gates = {"gates": {"storage": {"required": True}}, "requirement_to_gate": {"REQ-1": "storage"}}

    def test_unstarted_work_is_not_a_release(self):
        self.assertEqual(audit.validate_contract(self.ledger, self.gates), [])
        self.assertTrue(audit.release_gaps(self.ledger, self.gates, {}, ROOT))

    def test_status_alone_cannot_certify_a_requirement(self):
        self.ledger["requirements"][0]["implementation_status"] = "verified"
        self.assertTrue(audit.release_gaps(self.ledger, self.gates, {}, ROOT))

    def test_unmapped_or_optional_gate_is_invalid(self):
        del self.gates["requirement_to_gate"]["REQ-1"]
        self.assertTrue(audit.validate_contract(self.ledger, self.gates))
        self.gates["requirement_to_gate"]["REQ-1"] = "storage"
        self.gates["gates"]["storage"]["required"] = False
        self.assertTrue(audit.validate_contract(self.ledger, self.gates))

    def test_duplicates_and_unknown_status_are_invalid(self):
        self.ledger["requirements"].append(copy.deepcopy(self.ledger["requirements"][0]))
        self.assertTrue(audit.validate_contract(self.ledger, self.gates))
        self.ledger["requirements"].pop()
        self.ledger["requirements"][0]["implementation_status"] = "deferred"
        self.assertTrue(audit.validate_contract(self.ledger, self.gates))

    def test_required_external_evidence_cannot_be_bypassed(self):
        row = self.ledger["requirements"][0]
        row.update(implementation_status="verified", evidence_status="needs_evidence", blocker="Native fixture missing")
        self.assertTrue(audit.release_gaps(self.ledger, self.gates, {}, ROOT))

    def test_evidence_outside_repository_is_rejected(self):
        self.ledger["requirements"][0]["implementation_status"] = "verified"
        evidence = {"REQ-1": {"result": "pass", "artifacts": [{"path": "../private", "sha256": "0" * 64}]}}
        self.assertTrue(audit.release_gaps(self.ledger, self.gates, evidence, ROOT))

    def test_stale_source_invalidates_previously_passing_evidence(self):
        self.ledger["requirements"][0]["implementation_status"] = "verified"
        with tempfile.TemporaryDirectory() as directory:
            root = pathlib.Path(directory)
            (root / "src").mkdir()
            source = root / "src/model.cpp"
            source.write_text("version 1")
            report = root / "acceptance.txt"
            report.write_text("passed against version 1")
            evidence = {"REQ-1": {"result": "pass", "source_tree_sha256": audit.source_fingerprint(root),
                "artifacts": [{"path": "acceptance.txt", "sha256": hashlib.sha256(report.read_bytes()).hexdigest()}]}}
            self.assertEqual(audit.release_gaps(self.ledger, self.gates, evidence, root), [])
            source.write_text("version 2")
            self.assertTrue(audit.release_gaps(self.ledger, self.gates, evidence, root))

    def test_duplicate_gate_member_views_cannot_drift(self):
        self.gates["gates"]["storage"]["requirement_ids"] = []
        self.assertTrue(audit.validate_contract(self.ledger, self.gates))

    def test_build_fragments_are_part_of_evidence_identity(self):
        with tempfile.TemporaryDirectory() as directory:
            root = pathlib.Path(directory)
            (root / "cmake").mkdir()
            fragment = root / "cmake/Solver.cmake"
            fragment.write_text("set(SOLVER_SAFE ON)")
            previous = audit.source_fingerprint(root)
            fragment.write_text("set(SOLVER_SAFE OFF)")
            self.assertNotEqual(previous, audit.source_fingerprint(root))
            attributes = root / ".gitattributes"
            attributes.write_text("* text=auto eol=lf")
            previous = audit.source_fingerprint(root)
            attributes.write_text("* text=auto eol=crlf")
            self.assertNotEqual(previous, audit.source_fingerprint(root))


class RequirementAuditCliTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.root = pathlib.Path(self.temporary.name)
        self.docs = self.root / "docs" / "requirements"
        self.docs.mkdir(parents=True)
        self.ledger_path = self.docs / "apex-parity.json"
        self.gates_path = self.docs / "production-gates.json"
        self.evidence_path = self.docs / "acceptance-evidence.json"

    def write_contract(self, *, implementation_status="not_started", evidence_status="documented"):
        requirements = [{"id": "REQ-1", "area": "core", "requirement": "Save exactly",
            "evidence_status": evidence_status, "implementation_status": implementation_status, "acceptance": "Reopen same revision",
            "package": 3, "blocker": None, "source_urls": []}]
        gates = {"schema_version": "1.0", "release_rule": {"required_gates": ["G1"]},
                 "gates": {"G1": {"required": True, "requirement_ids": ["REQ-1"]}},
                 "requirement_to_gate": {"REQ-1": "G1"}}
        self.ledger_path.write_text(json.dumps({"requirements": requirements}), encoding="utf-8")
        self.gates_path.write_text(json.dumps(gates), encoding="utf-8")
        return requirements[0]

    def run_audit(self, *args):
        original_root = audit.ROOT
        original_argv = sys.argv
        try:
            audit.ROOT = self.root
            sys.argv = ["requirement_audit.py", *args]
            return audit.main()
        finally:
            audit.ROOT = original_root
            sys.argv = original_argv

    def test_release_mode_only_gate_fails_when_gaps_exist(self):
        self.write_contract(implementation_status="not_started")
        self.assertEqual(self.run_audit(), 0)
        self.assertEqual(self.run_audit("--release"), 2)

    def test_release_mode_reports_pass_when_evidence_is_current(self):
        requirements = self.write_contract(implementation_status="verified")
        self.assertEqual(requirements["implementation_status"], "verified")
        artifact = self.root / "acceptance" / "proof.txt"
        artifact.parent.mkdir()
        artifact.write_text("proof artifact", encoding="utf-8")
        evidence = {"REQ-1": {"result": "pass",
            "source_tree_sha256": audit.source_fingerprint(self.root),
            "artifacts": [{"path": "acceptance/proof.txt",
                          "sha256": hashlib.sha256(artifact.read_bytes()).hexdigest()}]}}
        self.evidence_path.write_text(json.dumps(evidence), encoding="utf-8")
        self.assertEqual(self.run_audit(), 0)
        self.assertEqual(self.run_audit("--release"), 0)


if __name__ == "__main__":
    unittest.main()
