import importlib.util
import json
import os
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
            (root / "CTestTestfile.cmake").write_text(
                "# Source directory: {0}\n"
                "# Build directory: {1}\n"
                "add_test([=[one]=] \"{1}/one.exe\")\n"
                "add_test([=[two]=] \"{1}/two.exe\")\n".format(root, root),
                encoding="utf-8")
            log = root / "LastTest.log"
            log.write_text(
                "Start testing: now\n"
                "1/2 Testing: one\n"
                "1/2 Test: one\n"
                "Command: \"{0}/one.exe\"\n"
                "Directory: {0}\n"
                "Test Passed.\n"
                "2/2 Testing: two\n"
                "2/2 Test: two\n"
                "Command: \"{0}/two.exe\"\n"
                "Directory: {0}\n"
                "Test Passed.\n"
                "End testing: now\n".format(root), encoding="utf-8")
            self.assertEqual(audit.test_log_status(log, root)["status"], "pass")
            log.write_text("Test Passed.\nTest Failed.\nEnd testing: now\n", encoding="utf-8")
            self.assertEqual(audit.test_log_status(log, root)["status"], "blocked")

    def test_test_log_status_rejects_focused_and_skipped_runs(self):
        with tempfile.TemporaryDirectory() as directory:
            root = pathlib.Path(directory)
            (root / "CTestTestfile.cmake").write_text(
                "# Source directory: {0}\n# Build directory: {1}\n"
                "add_test([=[one]=] \"{1}/one.exe\")\n"
                "add_test([=[two]=] \"{1}/two.exe\")\n"
                "set_tests_properties([=[two]=] PROPERTIES SKIP_RETURN_CODE \"77\")\n"
                .format(root, root), encoding="utf-8")
            log = root / "LastTest.log"
            log.write_text(
                "Start testing: now\n1/2 Testing: one\n1/2 Test: one\n"
                "Command: \"{0}/one.exe\"\nDirectory: {0}\nTest Passed.\n"
                "End testing: now\n".format(root), encoding="utf-8")
            result = audit.test_log_status(log, root)
            self.assertEqual(result["status"], "partial")
            self.assertTrue(any("missing" in detail for detail in result["details"]))

            log.write_text(
                "Start testing: now\n1/2 Testing: one\n1/2 Test: one\n"
                "Command: \"{0}/one.exe\"\nDirectory: {0}\nTest Passed.\n"
                "2/2 Testing: two\n2/2 Test: two\nCommand: \"{0}/two.exe\"\n"
                "Directory: {0}\nOutput:\nworker fixture skipped\nTest Passed.\n"
                "End testing: now\n".format(root), encoding="utf-8")
            result = audit.test_log_status(log, root)
            self.assertEqual(result["status"], "partial")
            self.assertTrue(any("skipped" in detail for detail in result["details"]))

    def test_latest_runtime_report_discovers_task_owned_reports(self):
        with tempfile.TemporaryDirectory() as directory:
            root = pathlib.Path(directory)
            legacy = root / "artifacts/installed-runtime/current/run-legacy/report.json"
            current = root / "artifacts/installed-runtime/run-task-owned/report.json"
            legacy.parent.mkdir(parents=True)
            current.parent.mkdir(parents=True)
            legacy.write_text("{}", encoding="utf-8")
            current.write_text("{}", encoding="utf-8")
            os.utime(legacy, (100, 100))
            os.utime(current, (200, 200))

            self.assertEqual(audit._latest_runtime_report(root), current)

    def test_qa_fixture_coverage_requires_passing_fixtures_in_both_configs(self):
        required_tests = sorted({
            test_name
            for rule in audit.QA_FIXTURE_RULES
            for test_name in rule["tests"]
        })
        with tempfile.TemporaryDirectory() as directory:
            root = pathlib.Path(directory)
            source_files = sorted({
                source
                for rule in audit.QA_FIXTURE_RULES
                for source in rule["sources"]
            })
            for source in source_files:
                path = root / source
                path.parent.mkdir(parents=True, exist_ok=True)
                path.write_text(
                    "\n".join(
                        anchor
                        for rule in audit.QA_FIXTURE_RULES
                        if source in rule["sources"]
                        for anchor in rule["anchors"]
                    ) + "\n",
                    encoding="utf-8",
                )
            for configuration in ("windows-debug", "windows-release"):
                build = root / "build" / configuration
                build.mkdir(parents=True)
                inventory = [
                    f"# Source directory: {root}",
                    f"# Build directory: {build}",
                ]
                for test_name in required_tests:
                    inventory.append(
                        f'add_test([=[{test_name}]=] "{build}/{test_name}.exe")'
                    )
                (build / "CTestTestfile.cmake").write_text(
                    "\n".join(inventory) + "\n", encoding="utf-8"
                )
                log = ["Start testing: now"]
                for index, test_name in enumerate(required_tests, 1):
                    log.extend([
                        f"{index}/{len(required_tests)} Testing: {test_name}",
                        f"{index}/{len(required_tests)} Test: {test_name}",
                        f'Command: "{build}/{test_name}.exe"',
                        f"Directory: {build}",
                        "Test Passed.",
                    ])
                log.append("End testing: now")
                (build / "Testing" / "Temporary").mkdir(parents=True)
                (build / "Testing" / "Temporary" / "LastTest.log").write_text(
                    "\n".join(log) + "\n", encoding="utf-8"
                )

            result = audit.qa_fixture_coverage_check(root)
            self.assertEqual(result["status"], "pass")
            self.assertIn("geometry", result["summary"])
            self.assertIn("windows-debug", result["evidence"][0])

    def test_qa_fixture_coverage_fails_closed_when_a_required_fixture_is_skipped(self):
        with tempfile.TemporaryDirectory() as directory:
            root = pathlib.Path(directory)
            for rule in audit.QA_FIXTURE_RULES:
                for source in rule["sources"]:
                    path = root / source
                    path.parent.mkdir(parents=True, exist_ok=True)
                    path.write_text("\n".join(rule["anchors"]) + "\n", encoding="utf-8")
            required_tests = sorted({
                test_name
                for rule in audit.QA_FIXTURE_RULES
                for test_name in rule["tests"]
            })
            for configuration in ("windows-debug", "windows-release"):
                build = root / "build" / configuration
                build.mkdir(parents=True)
                inventory = [f"# Source directory: {root}", f"# Build directory: {build}"]
                for test_name in required_tests:
                    inventory.append(
                        f'add_test([=[{test_name}]=] "{build}/{test_name}.exe")'
                    )
                (build / "CTestTestfile.cmake").write_text(
                    "\n".join(inventory) + "\n", encoding="utf-8"
                )
                log = ["Start testing: now"]
                for index, test_name in enumerate(required_tests, 1):
                    log.extend([
                        f"{index}/{len(required_tests)} Testing: {test_name}",
                        f"{index}/{len(required_tests)} Test: {test_name}",
                        f'Command: "{build}/{test_name}.exe"',
                        f"Directory: {build}",
                        "Test Skipped." if test_name == required_tests[0] else "Test Passed.",
                    ])
                log.append("End testing: now")
                (build / "Testing" / "Temporary").mkdir(parents=True)
                (build / "Testing" / "Temporary" / "LastTest.log").write_text(
                    "\n".join(log) + "\n", encoding="utf-8"
                )

            result = audit.qa_fixture_coverage_check(root)
            self.assertEqual(result["status"], "partial")
            self.assertTrue(any("skipped" in detail for detail in result["details"]))


if __name__ == "__main__":
    unittest.main()
