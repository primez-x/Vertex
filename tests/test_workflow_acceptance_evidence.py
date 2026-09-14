import importlib.util
import pathlib
import tempfile
import unittest


ROOT = pathlib.Path(__file__).resolve().parents[1]
SCRIPT = ROOT / "scripts" / "generate_workflow_acceptance_evidence.py"


def load_generator():
    spec = importlib.util.spec_from_file_location("workflow_acceptance_evidence", SCRIPT)
    if spec is None or spec.loader is None:
        raise RuntimeError(f"could not load {SCRIPT}")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


class WorkflowAcceptanceEvidenceTests(unittest.TestCase):
    def test_build_evidence_records_both_configurations_and_workflows(self):
        generator = load_generator()
        with tempfile.TemporaryDirectory() as directory:
            root = pathlib.Path(directory)
            source_anchors = {}
            for rule in generator.WORKFLOW_RULES.values():
                for relative in rule["sources"]:
                    source_anchors.setdefault(relative, set()).update(rule["anchors"])
            for relative, anchors in source_anchors.items():
                source = root / relative
                source.parent.mkdir(parents=True, exist_ok=True)
                source.write_text("\n".join(sorted(anchors)) + "\n", encoding="utf-8")
            for configuration in ("windows-debug", "windows-release"):
                build = root / "build" / configuration
                build.mkdir(parents=True)
                tests = sorted({
                    test_name
                    for rule in generator.WORKFLOW_RULES.values()
                    for test_name in rule["tests"]
                })
                inventory = [f"# Source directory: {root}", f"# Build directory: {build}"]
                inventory.extend(
                    f'add_test([=[{test_name}]=] "{build}/{test_name}.exe")'
                    for test_name in tests
                )
                (build / "CTestTestfile.cmake").write_text(
                    "\n".join(inventory) + "\n", encoding="utf-8"
                )
                log = ["Start testing: now"]
                for index, test_name in enumerate(tests, 1):
                    log.extend([
                        f"{index}/{len(tests)} Testing: {test_name}",
                        f"{index}/{len(tests)} Test: {test_name}",
                        f'Command: "{build}/{test_name}.exe"',
                        f"Directory: {build}",
                        "Test Passed.",
                    ])
                log.append("End testing: now")
                temporary = build / "Testing" / "Temporary"
                temporary.mkdir(parents=True)
                (temporary / "LastTest.log").write_text(
                    "\n".join(log) + "\n", encoding="utf-8"
                )

            evidence = generator.build_evidence(root)
            self.assertEqual(set(evidence), set(generator.WORKFLOW_RULES))
            self.assertTrue({
                "CORE-DOC-003", "CORE-DOC-004", "CORE-DOC-005", "CORE-DOC-006", "CORE-DOC-008",
                "GEO-BASE-001", "GEO-BASE-002", "GEO-BASE-003", "GEO-BASE-004",
                "GEO-CON-001", "GEO-CON-002", "GEO-CON-006",
                "APX-KEY-002", "APX-KEY-003",
                "APX-EDIT-001", "APX-EDIT-002", "APX-EDIT-003", "APX-EDIT-005",
                "APX-AREA-001", "APX-AREA-002", "APX-AREA-003", "APX-AREA-004",
                "CORE-SCOPE-002", "CORE-DOC-001", "CORE-DOC-002", "CORE-DOC-007", "CORE-DOC-010",
                "COMP-GEO-001", "COMP-GEO-002", "COMP-IO-001", "COMP-IO-002", "COMP-IO-003",
                "APX-WF-001", "APX-WF-002", "APX-WF-003", "APX-KEY-001", "APX-CURVE-001",
                "APX-ANNO-001", "APX-ANNO-002", "APX-ANNO-003", "APX-SYM-001", "APX-TRACE-001",
                "CORE-OFF-001", "CORE-OFF-002", "CORE-OFF-003", "CORE-OWN-001", "CORE-OWN-002",
                "CORE-SCOPE-001", "COMP-LIC-001", "COMP-LIC-002", "COMP-LIC-003",
                "GEO-CON-003", "GEO-CON-004", "GEO-CON-005", "APX-EDIT-004", "APX-TRACE-002",
                "APX-TRACE-003", "APX-DOC-001", "APX-DOC-002", "APX-DOC-003", "APX-UI-001",
                "APX-UI-002",
                "APX-UI-003", "APX-SPEC-001", "APX-SPEC-002",
                "ARCH-MOD-001", "ARCH-MOD-002", "ARCH-MOD-003", "ARCH-MOD-004",
                "ARCH-MOD-005", "ARCH-MOD-006", "ARCH-MOD-007", "ARCH-MOD-008", "ARCH-MOD-010",
                "ARCH-3D-001", "ARCH-VIEW-001", "ARCH-VIEW-002", "ARCH-SCH-001", "ARCH-SCH-002",
                "ARCH-SHEET-001", "ARCH-EDIT-001", "ARCH-REL-001", "ARCH-OUTPUT-001",
                "UX-WORK-001", "UX-WORK-002", "UX-WORK-003", "UX-ACCESS-001", "UX-INPUT-001",
                "ASSIST-001", "ASSIST-002", "ASSIST-003", "ASSIST-004", "ASSIST-005",
                "REC-001", "REC-002", "REC-003", "REC-005",
                "IO-IFC-001", "IO-DXF-001", "IO-PDF-001", "IO-OUTPUT-001", "IO-OUTPUT-002", "IO-OUTPUT-003",
                "SEC-WORKER-001", "SEC-WORKER-002", "SEC-PROJ-001",
            }.issubset(set(evidence)))
            for requirement_id, record in evidence.items():
                self.assertEqual(record["result"], "pass")
                self.assertEqual(len(record["ctest"]), 2)
                self.assertEqual(record["requirement_id"], requirement_id)
                self.assertTrue(record["source_files"])

    def test_build_evidence_rejects_missing_workflow_test(self):
        generator = load_generator()
        with tempfile.TemporaryDirectory() as directory:
            root = pathlib.Path(directory)
            source_anchors = {}
            for rule in generator.WORKFLOW_RULES.values():
                for relative in rule["sources"]:
                    source_anchors.setdefault(relative, set()).update(rule["anchors"])
            for relative, anchors in source_anchors.items():
                source = root / relative
                source.parent.mkdir(parents=True, exist_ok=True)
                source.write_text("\n".join(sorted(anchors)) + "\n", encoding="utf-8")
            for configuration in ("windows-debug", "windows-release"):
                build = root / "build" / configuration
                build.mkdir(parents=True)
                (build / "CTestTestfile.cmake").write_text(
                    f"# Source directory: {root}\n# Build directory: {build}\n",
                    encoding="utf-8",
                )
                temporary = build / "Testing" / "Temporary"
                temporary.mkdir(parents=True)
                (temporary / "LastTest.log").write_text(
                    "Start testing: now\nEnd testing: now\n", encoding="utf-8"
                )
            with self.assertRaises(RuntimeError):
                generator.build_evidence(root)


if __name__ == "__main__":
    unittest.main()
