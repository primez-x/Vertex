import importlib.util
import hashlib
import json
import pathlib
import re
import shutil
import subprocess
import sys
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


def load_provenance():
    spec = importlib.util.spec_from_file_location(
        "workflow_test_provenance", ROOT / "scripts/workflow_test_provenance.py")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def write_fixture_receipts(root):
    """Synthetic receipts for parser tests; real runner coverage is below."""
    provenance = load_provenance()
    for configuration, kind in (("windows-debug", "Debug"), ("windows-release", "Release")):
        build = root / "build" / configuration
        (build / "CMakeCache.txt").write_text(
            f"CMAKE_HOME_DIRECTORY:INTERNAL={root}\nCMAKE_BUILD_TYPE:STRING={kind}\n", encoding="utf-8")
        (build / "fixture.exe").write_bytes(b"compiled fixture")
        artifacts = {path.relative_to(root).as_posix(): hashlib.sha256(path.read_bytes()).hexdigest()
                     for path in (build / "CMakeCache.txt", build / "CTestTestfile.cmake", build / "fixture.exe")}
        provenance_log = build / provenance.PROVENANCE_LOG_FILE
        provenance_log.write_bytes((build / "Testing/Temporary/LastTest.log").read_bytes())
        record = {"schema_version": 1, "configuration": configuration,
                  "build_method": "cmake-clean-first",
                  "source_tree_sha256": provenance.source_fingerprint(root),
                  "artifacts": artifacts,
                  "log_sha256": hashlib.sha256(provenance_log.read_bytes()).hexdigest()}
        (build / "workflow-test-provenance.json").write_text(json.dumps(record), encoding="utf-8")


def write_requirement_contract(root):
    destination = root / "docs/requirements/apex-parity.json"
    destination.parent.mkdir(parents=True, exist_ok=True)
    shutil.copyfile(ROOT / "docs/requirements/apex-parity.json", destination)


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

            write_requirement_contract(root)
            write_fixture_receipts(root)
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
                self.assertEqual(record["result"], "supporting_evidence")
                self.assertEqual(record["local_checks_result"], "pass")
                self.assertEqual(record["acceptance_status"], "in_progress")
                self.assertEqual(record["requirement"], next(
                    row["requirement"] for row in json.loads(
                        (root / "docs/requirements/apex-parity.json").read_text(encoding="utf-8")
                    )["requirements"] if row["id"] == requirement_id))
                self.assertTrue(all(value == "not_assessed"
                                    for value in record["evidence_dimensions"].values()))
                self.assertEqual(len(record["ctest"]), 2)
                self.assertEqual(record["requirement_id"], requirement_id)
                self.assertTrue(record["source_files"])
                self.assertTrue(all(item["provenance"]["sha256"] for item in record["ctest"]))

            for relative in ("workflow-test-provenance.json", "workflow-test-provenance.log",
                             "fixture.exe", "CMakeCache.txt", "CTestTestfile.cmake"):
                path = root / "build/windows-debug" / relative
                original = path.read_bytes()
                with self.subTest(changed_artifact=relative):
                    path.write_bytes(original + b"\nchanged")
                    with self.assertRaisesRegex(RuntimeError, "provenance"):
                        generator.build_evidence(root)
                    path.write_bytes(original)
            working_log = root / "build/windows-debug/Testing/Temporary/LastTest.log"
            working_log.write_text("a later unrelated CTest run\n", encoding="utf-8")
            self.assertEqual(set(generator.build_evidence(root)), set(generator.WORKFLOW_RULES))
            receipt = root / "build/windows-release/workflow-test-provenance.json"
            original = receipt.read_bytes()
            receipt.unlink()
            with self.assertRaisesRegex(RuntimeError, "provenance"):
                generator.build_evidence(root)
            receipt.write_bytes(original)

            # A new source fingerprint must never legitimize old passing logs.
            changed_source = root / "src/core/room_relationships.cpp"
            changed_source.write_text(changed_source.read_text(encoding="utf-8") +
                                      "\n// changed after the successful run\n", encoding="utf-8")
            with self.assertRaisesRegex(RuntimeError, "provenance|source.*changed|source.*match"):
                generator.build_evidence(root)

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
            write_requirement_contract(root)
            write_fixture_receipts(root)
            with self.assertRaisesRegex(RuntimeError, "invalid CTest inventory"):
                generator.build_evidence(root)


class WorkflowProvenanceRunnerTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cache = ROOT / "build/windows-debug/CMakeCache.txt"
        text = cache.read_text(encoding="utf-8") if cache.is_file() else ""
        def tool(name, key):
            match = re.search(rf"(?m)^{key}:[^=]+=(.+)$", text)
            candidate = shutil.which(name) or (match.group(1).strip() if match else None)
            if not candidate or not pathlib.Path(candidate).is_file():
                raise unittest.SkipTest(f"{name} is unavailable for real CMake/CTest runner tests")
            return candidate
        cls.cmake = tool("cmake", "CMAKE_COMMAND")
        cls.ctest = tool("ctest", "CMAKE_CTEST_COMMAND")
        cls.ninja = tool("ninja", "CMAKE_MAKE_PROGRAM")

    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.root = pathlib.Path(self.temporary.name)
        (self.root / "src").mkdir()
        (self.root / "src/input.txt").write_text("current", encoding="utf-8")
        (self.root / "src/check.py").write_text(
            "import pathlib, sys\nassert pathlib.Path(sys.argv[1]).read_text() == 'current'\n",
            encoding="utf-8")
        (self.root / "CMakeLists.txt").write_text(
            'cmake_minimum_required(VERSION 3.24)\nproject(ProvenanceFixture LANGUAGES NONE)\n'
            'enable_testing()\n'
            'add_custom_command(OUTPUT "${CMAKE_BINARY_DIR}/fixture.exe"\n'
            ' COMMAND "${CMAKE_COMMAND}" -E copy "${CMAKE_SOURCE_DIR}/src/input.txt" '
            '"${CMAKE_BINARY_DIR}/fixture.exe" DEPENDS "${CMAKE_SOURCE_DIR}/src/input.txt")\n'
            'add_custom_target(fixture ALL DEPENDS "${CMAKE_BINARY_DIR}/fixture.exe")\n'
            f'add_test(NAME fixture COMMAND "{pathlib.Path(sys.executable).as_posix()}" '
            '"${CMAKE_SOURCE_DIR}/src/check.py" "${CMAKE_BINARY_DIR}/fixture.exe")\n', encoding="utf-8")
        self.provenance = load_provenance()

    def configure(self, configuration="windows-debug"):
        build = self.root / "build" / configuration
        kind = "Debug" if configuration == "windows-debug" else "Release"
        subprocess.run([self.cmake, "-S", str(self.root), "-B", str(build), "-G", "Ninja",
                        f"-DCMAKE_MAKE_PROGRAM={self.ninja}", f"-DCMAKE_BUILD_TYPE:STRING={kind}"],
                       check=True, capture_output=True,
                       creationflags=getattr(subprocess, "CREATE_NO_WINDOW", 0))
        return build

    def run_fixture(self, configuration="windows-debug", names=("fixture",)):
        return self.provenance.run(self.root, configuration, names, cmake=self.cmake, ctest=self.ctest)

    def test_current_debug_and_release_runs_are_deterministic_and_bound(self):
        for configuration in ("windows-debug", "windows-release"):
            self.configure(configuration)
            record = self.run_fixture(configuration)
            self.assertEqual(record, self.provenance.verify(self.root, configuration))
            self.assertEqual(self.provenance.verify(self.root, configuration),
                             self.provenance.verify(self.root, configuration))

    def test_clean_rebuild_does_not_trust_newer_stale_binary(self):
        build = self.configure()
        self.run_fixture()
        (build / "fixture.exe").write_text("stale", encoding="utf-8")
        self.run_fixture()
        self.assertEqual((build / "fixture.exe").read_text(), "current")

    def test_failed_tests_remove_prior_receipt(self):
        build = self.configure()
        self.run_fixture()
        (self.root / "src/input.txt").write_text("broken", encoding="utf-8")
        with self.assertRaises(subprocess.CalledProcessError):
            self.run_fixture()
        self.assertFalse((build / "workflow-test-provenance.json").exists())

    def test_source_change_during_testing_is_rejected(self):
        build = self.configure()
        (self.root / "src/check.py").write_text(
            "import pathlib\npathlib.Path(__file__).with_name('input.txt').write_text('changed')\n",
            encoding="utf-8")
        with self.assertRaisesRegex(RuntimeError, "source/build changed"):
            self.run_fixture()
        self.assertFalse((build / "workflow-test-provenance.json").exists())

    def test_build_failure_does_not_attest_old_log(self):
        build = self.configure()
        self.run_fixture()
        (self.root / "src/input.txt").unlink()
        with self.assertRaises(subprocess.CalledProcessError):
            self.run_fixture()
        self.assertFalse((build / "workflow-test-provenance.json").exists())

    def test_missing_test_is_rejected_even_when_another_passes(self):
        build = self.configure()
        with self.assertRaisesRegex(RuntimeError, "absent"):
            self.run_fixture(names=("fixture", "nonexistent"))
        self.assertFalse((build / "workflow-test-provenance.json").exists())

    def test_disabled_test_is_not_attested(self):
        source = self.root / "CMakeLists.txt"
        source.write_text(source.read_text(encoding="utf-8") +
                          'set_tests_properties(fixture PROPERTIES DISABLED TRUE)\n', encoding="utf-8")
        build = self.configure()
        with self.assertRaises((RuntimeError, subprocess.CalledProcessError)):
            self.run_fixture()
        self.assertFalse((build / "workflow-test-provenance.json").exists())

    def test_wrong_configuration_is_rejected_before_cleaning(self):
        build = self.configure()
        cache = build / "CMakeCache.txt"
        cache.write_text(cache.read_text(encoding="utf-8").replace(
            "CMAKE_BUILD_TYPE:STRING=Debug", "CMAKE_BUILD_TYPE:STRING=Release"), encoding="utf-8")
        (build / "fixture.exe").write_text("must survive", encoding="utf-8")
        with self.assertRaisesRegex(RuntimeError, "configuration mismatch"):
            self.run_fixture()
        self.assertEqual((build / "fixture.exe").read_text(), "must survive")

    def test_build_payload_change_during_testing_is_rejected(self):
        build = self.configure()
        (self.root / "src/check.py").write_text(
            "import pathlib, sys\npathlib.Path(sys.argv[1]).write_text('modified binary')\n", encoding="utf-8")
        with self.assertRaisesRegex(RuntimeError, "source/build changed"):
            self.run_fixture()
        self.assertFalse((build / "workflow-test-provenance.json").exists())


if __name__ == "__main__":
    unittest.main()
