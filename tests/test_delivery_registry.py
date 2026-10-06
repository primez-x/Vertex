"""Registry integrity checks; these do not exercise Vertex product behavior."""

from __future__ import annotations

import copy
from collections import Counter
import importlib.util
import json
from pathlib import Path
import tempfile
import unittest
from unittest import mock


ROOT = Path(__file__).resolve().parents[1]
SPEC = importlib.util.spec_from_file_location("delivery_registry", ROOT / "scripts/delivery_registry.py")
assert SPEC and SPEC.loader
registry = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(registry)


class DeliveryRegistryTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.document = registry.build_registry(ROOT)

    def test_bundled_help_changes_invalidate_product_evidence(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            (root / "docs/delivery").mkdir(parents=True)
            help_file = root / "docs/user-guide.html"
            help_file.write_text("<p>Original drawing instructions</p>", encoding="utf-8")
            before = registry.product_source_fingerprint(root)
            (root / "docs/delivery/progress.md").write_text("Resume note", encoding="utf-8")
            self.assertEqual(before, registry.product_source_fingerprint(root))
            help_file.write_text("<p>Corrected drawing instructions</p>", encoding="utf-8")
            self.assertNotEqual(before, registry.product_source_fingerprint(root))

    def test_exact_source_coverage_and_single_gate_ownership(self):
        rows = self.document["requirements"]
        self.assertEqual(len(rows), 277)
        apex = [row for row in rows if row["source_kind"] == "apex"]
        sources = json.loads((ROOT / registry.APEX).read_text(encoding="utf-8"))["requirements"]
        self.assertEqual({row["id"] for row in apex}, {row["id"] for row in sources})
        gates = json.loads((ROOT / registry.GATES).read_text(encoding="utf-8"))["gates"]
        for row in apex:
            self.assertIn(row["id"], gates[row["required_gate"]]["requirement_ids"])
            self.assertEqual(row["owner_package"], "D" + row["required_gate"].split("_")[0][1:].zfill(2))
        self.assertEqual({row["id"] for row in rows if row["source_kind"] == "pinc_adoption"},
                         {f"PINC-{n:03}" for n in range(1, 14)})
        self.assertEqual({row["id"] for row in rows if row["source_kind"] == "pinc_operation"},
                         {f"PIN-{n:03}" for n in range(1, 135)})
        registry.validate_registry(self.document, ROOT)

    def test_pinc_accountability_and_interface_dependencies(self):
        pins = [row for row in self.document["requirements"] if row["source_kind"] == "pinc_operation"]
        self.assertEqual(Counter(row["owner_package"] for row in pins),
                         {"D04": 55, "D05": 62, "D06": 3, "D03": 6, "D08": 8})
        adoption = {row["id"]: row["owner_package"] for row in self.document["requirements"]
                    if row["source_kind"] == "pinc_adoption"}
        self.assertEqual(adoption["PINC-011"], "D08")
        self.assertEqual(adoption["PINC-013"], "D10")
        self.assertEqual(registry.DEPENDENCIES["D02"], [])
        self.assertEqual(registry.DEPENDENCIES["D03"], [])
        def visit(package, ancestors):
            self.assertNotIn(package, ancestors, "cyclic package dependency")
            for dependency in registry.DEPENDENCIES[package]:
                visit(dependency, ancestors | {package})
        for package in registry.DEPENDENCIES:
            visit(package, set())

    def test_source_acceptance_and_manual_text_are_preserved(self):
        sources = json.loads((ROOT / registry.APEX).read_text(encoding="utf-8"))["requirements"]
        apex = {row["id"]: row for row in self.document["requirements"] if row["source_kind"] == "apex"}
        for source in sources:
            self.assertEqual(apex[source["id"]]["source_snapshot"], source)
            self.assertEqual(apex[source["id"]]["expected_result"], source["acceptance"])
        manual = self.document["manual_scenarios"]
        self.assertEqual({row["id"] for row in manual},
                         {f"U{n:03}" for n in range(1, 450)} | {"U100a"})
        self.assertEqual(len(manual), 450)
        self.assertTrue(all(row["title"] and row["expected_result"] for row in manual))
        case = next(row for row in manual if row["id"] == "U434")
        self.assertIn("10.24", case["expected_result"])
        self.assertIn("0.2 m", case["steps"])
        self.assertIsNone(next(row for row in manual if row["id"] == "U001")["steps"])

    def test_all_dated_plans_and_no_initial_acceptance(self):
        self.assertEqual(len(self.document["dated_plans"]), 83)
        expected = {path.relative_to(ROOT).as_posix() for folder in registry.PLAN_DIRS
                    for path in (ROOT / folder).glob("*.md")}
        self.assertEqual({row["source_path"] for row in self.document["dated_plans"]}, expected)
        for row in registry.delivery_rows(self.document):
            self.assertEqual(row["final_acceptance"]["status"], "not_accepted")
            self.assertEqual(row["focused_verification"]["status"], "not_run")
            self.assertEqual(row["installed_verification"]["status"], "not_run")
            self.assertIsNone(row["final_acceptance"]["evidence_refs"])

    def test_duplicate_and_missing_coverage_fail_closed(self):
        changed = copy.deepcopy(self.document)
        changed["requirements"].append(copy.deepcopy(changed["requirements"][0]))
        with self.assertRaisesRegex(ValueError, "duplicate"):
            registry.validate_registry(changed, ROOT)
        changed = copy.deepcopy(self.document)
        changed["manual_scenarios"].pop()
        with self.assertRaisesRegex(ValueError, "manual"):
            registry.validate_registry(changed, ROOT)

    def test_acceptance_requires_actual_evidence_and_observation(self):
        changed = copy.deepcopy(self.document)
        row = changed["requirements"][0]
        row["final_acceptance"]["status"] = "accepted"
        with self.assertRaisesRegex(ValueError, "acceptance"):
            registry.validate_registry(changed, ROOT)
        changed = copy.deepcopy(self.document)
        changed["requirements"][0]["focused_verification"]["status"] = "passed"
        with self.assertRaisesRegex(ValueError, "evidence"):
            registry.validate_registry(changed, ROOT)

    def test_unknown_owner_status_and_machine_paths_are_rejected(self):
        for field, value in (("owner_package", "D99"), ("fixture_refs", ["C:/private/fixture.json"])):
            changed = copy.deepcopy(self.document)
            changed["requirements"][0][field] = value
            with self.assertRaises(ValueError):
                registry.validate_registry(changed, ROOT)
        changed = copy.deepcopy(self.document)
        changed["requirements"][0]["installed_verification"]["status"] = "verified"
        with self.assertRaisesRegex(ValueError, "status"):
            registry.validate_registry(changed, ROOT)

    def test_source_acceptance_and_gate_mapping_cannot_be_silently_rewritten(self):
        changed = copy.deepcopy(self.document)
        changed["requirements"][0]["expected_result"] = "Pass without performing the workflow."
        with self.assertRaisesRegex(ValueError, "source acceptance"):
            registry.validate_registry(changed, ROOT)
        changed = copy.deepcopy(self.document)
        changed["requirements"][0]["owner_package"] = "D04"
        changed["requirements"][0]["package_dependencies"] = registry.DEPENDENCIES["D04"]
        with self.assertRaisesRegex(ValueError, "gate ownership"):
            registry.validate_registry(changed, ROOT)

    def test_refresh_preserves_progress_without_promoting_historical_claims(self):
        previous = copy.deepcopy(self.document)
        previous["requirements"][0]["focused_verification"] = {
            "status": "blocked", "evidence_refs": ["docs/delivery/progress.md"],
            "notes": "Clean-machine qualification pending."}
        refreshed = registry.build_registry(ROOT, previous)
        self.assertEqual(refreshed["requirements"][0]["focused_verification"],
                         previous["requirements"][0]["focused_verification"])
        verified = next(row for row in refreshed["requirements"]
                        if row.get("source_snapshot", {}).get("implementation_status") == "verified")
        self.assertEqual(verified["implementation"]["status"], "source_reported_verified")
        self.assertEqual(verified["installed_verification"]["status"], "not_run")
        self.assertEqual(verified["final_acceptance"]["status"], "not_accepted")

    def accepted_document(self):
        document = copy.deepcopy(self.document)
        document["current_build_binding"] = "a" * 64
        item = next(row for row in document["requirements"] if row["id"] == "PINC-001")
        binding = registry.applicability_binding(item, document)
        for field in ("focused_verification", "installed_verification"):
            item[field] = {"status": "passed", "evidence_refs": ["docs/delivery/progress.md"],
                           "notes": "Observed test fixture.", "binding": binding}
        item["final_acceptance"] = {"status": "accepted", "evidence_refs": ["docs/delivery/progress.md"],
                                    "observed_by": "user", "notes": None, "binding": binding}
        return document

    def assert_invalidated(self, document):
        item = next(row for row in document["requirements"] if row["id"] == "PINC-001")
        self.assertEqual(item["focused_verification"]["status"], "not_run")
        self.assertEqual(item["installed_verification"]["status"], "not_run")
        self.assertEqual(item["final_acceptance"]["status"], "not_accepted")
        self.assertEqual(item["qualification_history"][-1]["final_acceptance"]["status"], "accepted")
        self.assertEqual(item["qualification_history"][-1]["final_acceptance"]["evidence_refs"],
                         ["docs/delivery/progress.md"])

    def test_changed_definition_invalidates_accepted_evidence(self):
        previous = self.accepted_document()
        original = registry.parse_adoption(ROOT)
        original[0]["expected_result"] += " Additional observed variant."
        original[0]["required_variants"].append("Additional observed variant.")
        with mock.patch.object(registry, "parse_adoption", return_value=original):
            refreshed = registry.build_registry(ROOT, previous)
        self.assert_invalidated(refreshed)

    def test_changed_source_or_candidate_build_invalidates_evidence(self):
        previous = self.accepted_document()
        with mock.patch.object(registry, "product_source_fingerprint", return_value="b" * 64):
            self.assert_invalidated(registry.build_registry(ROOT, previous))
        previous["current_build_binding"] = "c" * 64
        self.assert_invalidated(registry.build_registry(ROOT, previous))

    def test_unchanged_definition_and_bindings_preserve_accepted_progress(self):
        previous = self.accepted_document()
        refreshed = registry.build_registry(ROOT, previous)
        prior = next(row for row in previous["requirements"] if row["id"] == "PINC-001")
        after = next(row for row in refreshed["requirements"] if row["id"] == "PINC-001")
        self.assertEqual(after, prior)
        registry.validate_registry(refreshed, ROOT)

    def test_unbound_evidence_and_unidentified_installed_build_cannot_pass(self):
        changed = copy.deepcopy(self.document)
        item = changed["requirements"][0]
        item["focused_verification"].update(status="passed", evidence_refs=["docs/delivery/progress.md"])
        with self.assertRaisesRegex(ValueError, "applicability binding"):
            registry.validate_registry(changed, ROOT)
        item["focused_verification"]["binding"] = registry.applicability_binding(item, changed)
        item["installed_verification"].update(status="passed", evidence_refs=["docs/delivery/progress.md"],
                                              binding=registry.applicability_binding(item, changed))
        with self.assertRaisesRegex(ValueError, "candidate build binding"):
            registry.validate_registry(changed, ROOT)

    def test_atomic_replace_failure_preserves_old_manual_progress(self):
        with tempfile.TemporaryDirectory() as directory:
            target = Path(directory) / "requirements.json"
            original = b'{"manual_progress":"retain this observation"}\n'
            target.write_bytes(original)
            with mock.patch.object(registry.os, "replace", side_effect=OSError("simulated publication failure")):
                with self.assertRaisesRegex(OSError, "publication failure"):
                    registry.publish_registry(target, self.document, ROOT)
            self.assertEqual(target.read_bytes(), original)
            self.assertEqual(list(Path(directory).iterdir()), [target])


if __name__ == "__main__":
    unittest.main()
