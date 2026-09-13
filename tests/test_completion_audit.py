import importlib.util
import json
import pathlib
import tempfile
import unittest


SCRIPT = pathlib.Path(__file__).resolve().parents[1] / "scripts/completion_audit.py"
SPEC = importlib.util.spec_from_file_location("completion_audit", SCRIPT)
audit = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(audit)


class CompletionAuditTests(unittest.TestCase):
    def test_empty_tree_fails_closed_with_stable_checklist(self):
        with tempfile.TemporaryDirectory() as directory:
            root = pathlib.Path(directory)
            first = audit.build_report(root)
            second = audit.build_report(root)

        self.assertEqual(first, second)
        self.assertFalse(first["production_ready"])
        self.assertEqual(first["schema_version"], "1.0")
        self.assertGreaterEqual(len(first["checks"]), 8)
        self.assertTrue(all({"id", "status", "summary", "evidence"} <= set(check)
                            for check in first["checks"]))
        self.assertGreater(first["summary"]["blocked"] + first["summary"]["missing"], 0)

    def test_runtime_status_requires_clean_offline_boundary(self):
        complete = {
            "passed": True,
            "production_qualified": True,
            "clean_machine": True,
            "network_denied": True,
            "environment": {
                "registry_isolated": True,
                "other_environment_inherited": False,
            },
        }
        self.assertEqual(audit.runtime_status(complete)["status"], "pass")
        for field, value in (("clean_machine", False), ("network_denied", False),
                             ("production_qualified", False)):
            candidate = dict(complete)
            candidate[field] = value
            self.assertEqual(audit.runtime_status(candidate)["status"], "partial")
        inherited = json.loads(json.dumps(complete))
        inherited["environment"]["other_environment_inherited"] = True
        self.assertEqual(audit.runtime_status(inherited)["status"], "partial")

    def test_test_log_status_rejects_failures_and_accepts_complete_log(self):
        with tempfile.TemporaryDirectory() as directory:
            root = pathlib.Path(directory)
            log = root / "LastTest.log"
            log.write_text("Test Passed.\nEnd testing: now\n", encoding="utf-8")
            self.assertEqual(audit.test_log_status(log)["status"], "pass")
            log.write_text("Test Passed.\nTest Failed.\nEnd testing: now\n", encoding="utf-8")
            self.assertEqual(audit.test_log_status(log)["status"], "blocked")


if __name__ == "__main__":
    unittest.main()
