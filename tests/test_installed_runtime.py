"""Portable checks for the installed-runtime smoke policy; never launch an app."""

import importlib.util
import copy
import io
import json
import os
from pathlib import Path
import struct
import subprocess
import sys
import tempfile
import unittest
from unittest import mock


SPEC = importlib.util.spec_from_file_location(
    "installed_runtime", Path(__file__).resolve().parents[1] / "scripts" / "test_installed_runtime.py")
runtime = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(runtime)


class InstalledRuntimeTests(unittest.TestCase):
    def performance_report(self):
        return {
            "schema_version": 1, "audit_status": "incomplete",
            "workload": {"id": "interactive-session", "entities": 4, "objects": 2,
                         "triangles": None, "sheets": 1, "project_bytes": 4096},
            "reference_hardware": "unspecified", "threshold_status": "incomplete",
            "metrics": {name: {"sample_count": 0, "dropped_sample_count": 0,
                                "threshold_ms": threshold, "has_samples": False,
                                "within_threshold": False, "p95_ms": None}
                        for name, threshold in [("navigation", 16.7), ("input", 50),
                                                ("edit", 250), ("open", 5000), ("save", 5000)]},
        }

    def declarations(self):
        return {name: {"C:\\installed\\bin\\" + name} for name in runtime.REQUIRED_MODULES}

    def test_required_modules_match_case_insensitive_installed_paths(self):
        declared = self.declarations()
        observed = [next(iter(paths)).upper() for paths in declared.values()]
        observed.append("C:\\Windows\\System32\\kernel32.dll")
        result = runtime.validate_module_paths(observed, declared)
        self.assertEqual(result["errors"], [])
        self.assertEqual(len(result["required_modules_observed"]), 6)
        self.assertEqual(result["other_module_paths_unqualified"],
                         ["C:\\Windows\\System32\\kernel32.dll"])

    def test_sdk_copy_fails_even_if_same_module_also_seen_in_install(self):
        declared = self.declarations()
        observed = [next(iter(paths)) for paths in declared.values()]
        observed.append("C:\\SDK\\bin\\msvcp140.dll")
        result = runtime.validate_module_paths(observed, declared)
        self.assertTrue(any("undeclared path" in error for error in result["errors"]))

    def test_optional_packaged_module_cannot_load_from_sdk(self):
        declared = self.declarations()
        declared["qt6core.dll"] = {"C:\\installed\\bin\\Qt6Core.dll"}
        observed = [next(iter(paths)) for paths in self.declarations().values()]
        observed.append("C:\\SDK\\bin\\Qt6Core.dll")
        self.assertTrue(runtime.validate_module_paths(observed, declared)["errors"])

    def test_missing_or_undeclared_required_modules_fail(self):
        declared = self.declarations()
        observed = [next(iter(paths)) for paths in declared.values()]
        self.assertTrue(runtime.validate_module_paths(observed[:-1], declared)["errors"])
        del declared["qwindows.dll"]
        self.assertTrue(runtime.validate_module_paths(observed, declared)["errors"])

    def test_environment_removes_sdk_searches_without_dumping_inherited_values(self):
        original = {"Path": "C:\\SDK", "QT_PLUGIN_PATH": "C:\\SDK\\plugins",
                    "qt_custom": "custom", "QML2_IMPORT_PATH": "C:\\SDK\\qml",
                    "APPDATA": "user profile", "UnrelatedSecret": "not for evidence"}
        env, evidence = runtime.isolated_environment(original, Path("C:/Windows"), Path("C:/evidence/private"))
        self.assertNotIn("Path", env)
        self.assertNotIn("QT_PLUGIN_PATH", env)
        self.assertNotIn("qt_custom", env)
        self.assertNotIn("QML2_IMPORT_PATH", env)
        self.assertNotIn("SDK", env["PATH"])
        self.assertEqual(env["QT_QPA_PLATFORM"], "windows")
        self.assertIn("private", env["APPDATA"])
        self.assertNotIn("not for evidence", json.dumps(evidence))
        self.assertEqual(original["APPDATA"], "user profile")

    def test_capture_drains_but_retains_only_bounded_bytes(self):
        capture = runtime.BoundedCapture(io.BytesIO(b"x" * (runtime.MAX_CAPTURE_BYTES + 5000)))
        result = capture.evidence()
        self.assertEqual(len(result["text"]), runtime.MAX_CAPTURE_BYTES)
        self.assertEqual(result["bytes_seen"], runtime.MAX_CAPTURE_BYTES + 5000)
        self.assertTrue(result["truncated"])
        self.assertTrue(result["drain_complete"])

    def test_png_checks_signature_nonzero_dimensions_and_hash(self):
        with tempfile.TemporaryDirectory() as temporary:
            path = Path(temporary) / "test.png"
            path.write_bytes(b"\x89PNG\r\n\x1a\n\0\0\0\rIHDR" + struct.pack(">II", 32, 16) + b"test")
            result = runtime.png_evidence(path)
            self.assertEqual((result["width"], result["height"]), (32, 16))
            self.assertEqual(len(result["sha256"]), 64)
            path.write_bytes(b"not a PNG")
            with self.assertRaises(ValueError):
                runtime.png_evidence(path)

    def test_project_evidence_requires_a_nonempty_sqlite_project(self):
        with tempfile.TemporaryDirectory() as temporary:
            path = Path(temporary) / "project.bldproj"
            path.write_bytes(b"SQLite format 3\0" + b"project bytes")
            result = runtime.project_evidence(path)
            self.assertEqual(result["path"], str(path))
            self.assertGreater(result["bytes"], 16)
            self.assertEqual(len(result["sha256"]), 64)
            path.write_bytes(b"not a project")
            with self.assertRaisesRegex(ValueError, "SQLite"):
                runtime.project_evidence(path)

    def test_performance_evidence_validates_application_report_boundary(self):
        with tempfile.TemporaryDirectory() as temporary:
            path = Path(temporary) / "performance.json"
            path.write_text(json.dumps(self.performance_report()), encoding="utf-8")
            result = runtime.performance_evidence(path)
            self.assertEqual(result["threshold_status"], "incomplete")
            self.assertEqual(len(result["sha256"]), 64)
            path.write_text("{}", encoding="utf-8")
            with self.assertRaisesRegex(ValueError, "schema"):
                runtime.performance_evidence(path)

    def test_performance_report_rejects_corruption_and_fabricated_passes(self):
        valid = self.performance_report()
        invalid = [[], None, True, {}, {**valid, "schema_version": True},
                   {**valid, "schema_version": 1.0}, {**valid, "metrics": {}},
                   {**valid, "workload": {}}, {**valid, "reference_hardware": ""},
                   {**valid, "audit_status": "passed"},
                   {**valid, "threshold_status": "within_targets"}]
        for field in ("entities", "objects", "sheets", "project_bytes", "triangles"):
            for value in (-1, True, 0.5, "0"):
                report = copy.deepcopy(valid)
                report["workload"][field] = value
                invalid.append(report)
        for field in ("entities", "objects", "sheets"):
            report = copy.deepcopy(valid)
            report["workload"][field] = None
            invalid.append(report)
        invalid.append({**valid, "workload": {**valid["workload"], "objects": 5}})
        for field, values in {
            "sample_count": [-1, True, 0.5, None, 1],
            "dropped_sample_count": [-1, True, 0.5, None],
            "threshold_ms": [True, -1, 0, 999999, float("nan"), float("inf")],
            "p95_ms": [0, -1, True, "0", float("nan"), float("inf")],
            "has_samples": [True, 0, "false"], "within_threshold": [True, 0, "false"],
        }.items():
            for value in values:
                report = copy.deepcopy(valid)
                report["metrics"]["input"][field] = value
                invalid.append(report)
        for field in valid["metrics"]["input"]:
            report = copy.deepcopy(valid)
            del report["metrics"]["input"][field]
            invalid.append(report)
        raw_reports = [json.dumps(report) for report in invalid]
        raw_reports += ['{"schema_version":1,' + json.dumps(valid)[1:],
                        json.dumps(valid).replace('"sample_count": 0', '"sample_count": 1, "sample_count": 0', 1),
                        '{', json.dumps(valid).replace('"entities": 4', '"entities": 1e999')]
        with tempfile.TemporaryDirectory() as temporary:
            path = Path(temporary) / "performance.json"
            for raw in raw_reports:
                with self.subTest(raw=raw):
                    path.write_text(raw, encoding="utf-8")
                    with self.assertRaises(ValueError):
                        runtime.performance_evidence(path)

    def test_performance_report_accepts_sheets_inside_one_model_entity(self):
        report = self.performance_report()
        report["workload"].update(entities=3, objects=2, sheets=12)
        with tempfile.TemporaryDirectory() as temporary:
            path = Path(temporary) / "performance.json"
            path.write_text(json.dumps(report), encoding="utf-8")
            self.assertEqual(runtime.performance_evidence(path)["threshold_status"], "incomplete")

    def test_performance_report_accepts_unknown_project_size(self):
        report = self.performance_report()
        report["workload"].update(project_bytes=None, metadata_scope="final_document_state")
        report["sampling_scope"] = "document evolution since replacement or explicit begin-run"
        with tempfile.TemporaryDirectory() as temporary:
            path = Path(temporary) / "performance.json"
            path.write_text(json.dumps(report), encoding="utf-8")
            self.assertEqual(runtime.performance_evidence(path)["threshold_status"], "incomplete")

    def test_performance_report_computes_threshold_outcome_from_samples(self):
        report = self.performance_report()
        for metric in report["metrics"].values():
            metric.update(sample_count=1, has_samples=True, p95_ms=0, within_threshold=True)
        report["threshold_status"] = "within_targets"
        with tempfile.TemporaryDirectory() as temporary:
            path = Path(temporary) / "performance.json"
            def check(status):
                path.write_text(json.dumps(report), encoding="utf-8")
                self.assertEqual(runtime.performance_evidence(path)["threshold_status"], status)
            check("within_targets")
            report["metrics"]["input"]["p95_ms"] = 50
            check("within_targets")
            report["metrics"]["input"].update(p95_ms=50.1, within_threshold=False)
            path.write_text(json.dumps(report), encoding="utf-8")
            with self.assertRaises(ValueError):
                runtime.performance_evidence(path)
            report["threshold_status"] = "exceeds_targets"
            check("exceeds_targets")
            report["metrics"]["save"].update(sample_count=0, has_samples=False,
                                              within_threshold=False, p95_ms=None)
            report["threshold_status"] = "incomplete"
            check("incomplete")

    def test_performance_cli_returns_evidence_or_failure_without_application(self):
        with tempfile.TemporaryDirectory() as temporary:
            path = Path(temporary) / "report with spaces.json"
            report = self.performance_report()
            report["metrics"]["input"]["measurement_scope"] = "handler only"
            report["workload"]["triangles_note"] = "not measured"
            path.write_text(json.dumps(report), encoding="utf-8")
            command = [sys.executable, str(Path(runtime.__file__).with_name("performance_report.py")), str(path)]
            options = {"capture_output": True, "text": True, "timeout": 15}
            if os.name == "nt":
                options["creationflags"] = subprocess.CREATE_NO_WINDOW
            result = subprocess.run(command, **options)
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertEqual(json.loads(result.stdout)["threshold_status"], "incomplete")
            path.write_text('{"schema_version":true}', encoding="utf-8")
            result = subprocess.run(command, **options)
            self.assertEqual(result.returncode, 1)
            self.assertEqual(result.stdout, "")
            self.assertIn("invalid performance report", result.stderr)
            path.unlink()
            result = subprocess.run(command, **options)
            self.assertEqual(result.returncode, 1)
            self.assertNotIn("Traceback", result.stderr)

    def test_unsafe_manifest_path_rejected(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary).resolve()
            (root / "runtime-manifest.json").write_text(json.dumps({
                "manifest_kind": "runtime", "files": [{"path": "../outside.dll", "sha256": "a" * 64}]}))
            with self.assertRaisesRegex(ValueError, "unsafe"):
                runtime.load_runtime_manifest(root)

    @unittest.skipUnless(os.name == "nt" and runtime.ctypes.sizeof(runtime.ctypes.c_void_p) == 8,
                         "Windows 64-bit module API check")
    def test_windows_module_probe_reads_only_own_test_runner(self):
        monitor = runtime.WindowsModules(os.getpid())
        try:
            self.assertTrue(monitor.snapshot())
        finally:
            monitor.close()
        self.assertIsNone(monitor.handle)

    def test_hidden_workspace_launch_and_report_with_mock_child(self):
        declared = self.declarations()
        paths = [next(iter(rows)) for rows in declared.values()]
        records = {runtime.windows_key(path): {"path": Path(path), "sha256": "a" * 64} for path in paths}
        child = mock.Mock(pid=12345)
        child.stdout = io.BytesIO(b"stdout")
        child.stderr = io.BytesIO(b"stderr")
        child.poll.side_effect = [None, 0, 0]
        child.wait.return_value = 0
        with mock.patch.object(runtime.subprocess, "Popen", return_value=child) as launch, \
             mock.patch.object(runtime.subprocess, "CREATE_NO_WINDOW", 0x08000000, create=True), \
             mock.patch.object(runtime, "WindowsModules") as monitor, \
             mock.patch.object(runtime, "sha256_file", return_value="a" * 64), \
             mock.patch.object(runtime, "png_evidence", return_value={"valid_header": True}), \
             mock.patch.object(runtime, "performance_evidence", return_value={"valid": True}), \
             mock.patch.object(runtime.time, "sleep"):
            monitor.return_value.snapshot.return_value = paths
            with mock.patch.object(runtime, "project_evidence", return_value={"valid": True}) as project:
                result = runtime.run_workspace(
                    Path("installed/bin/property-studio.exe"), "architectural",
                    Path("evidence"), {"PATH": "Windows"}, declared, records,
                    project_output=Path("evidence/source.bldproj"),
                    project_input=Path("evidence/original.bldproj"),
                    market="light-commercial")
        self.assertTrue(result["passed"], result["errors"])
        self.assertEqual(result["market"], "light-commercial")
        self.assertEqual(len(result["screenshots"]), 2)
        project.assert_called_once_with(Path("evidence/source.bldproj"))
        self.assertEqual(launch.call_args.kwargs["creationflags"], 0x08000000)
        self.assertFalse(launch.call_args.kwargs["shell"])
        self.assertEqual(launch.call_args.kwargs["cwd"], Path("installed/bin"))
        self.assertIn("--smoke-3d-output", launch.call_args.args[0])
        self.assertIn("--smoke-project-output", launch.call_args.args[0])
        self.assertIn("--smoke-project-input", launch.call_args.args[0])
        self.assertIn("--smoke-performance-output", launch.call_args.args[0])
        self.assertIn("--smoke-assistance-disabled", launch.call_args.args[0])
        self.assertIn("--smoke-market", launch.call_args.args[0])
        self.assertIn("light-commercial", launch.call_args.args[0])
        self.assertTrue(result["assistance_disabled_requested"])
        monitor.assert_called_once_with(12345)
        monitor.return_value.close.assert_called_once()
        child.kill.assert_not_called()

    def test_timeout_stops_only_owned_mock_child_and_records_failure(self):
        child = mock.Mock(pid=12345)
        child.stdout = io.BytesIO()
        child.stderr = io.BytesIO()
        child.poll.return_value = None
        child.wait.return_value = -9
        with mock.patch.object(runtime.subprocess, "Popen", return_value=child), \
             mock.patch.object(runtime.subprocess, "CREATE_NO_WINDOW", 0x08000000, create=True), \
             mock.patch.object(runtime, "WindowsModules") as monitor, \
             mock.patch.object(runtime.time, "monotonic", side_effect=[0.0, 16.0, 16.0]), \
             mock.patch.object(runtime, "png_evidence", side_effect=FileNotFoundError("missing output")):
            result = runtime.run_workspace(Path("installed/bin/property-studio.exe"), "measurement",
                                           Path("evidence"), {}, self.declarations(), {})
        self.assertFalse(result["passed"])
        self.assertTrue(result["timed_out"])
        child.kill.assert_called_once()
        child.wait.assert_called_once_with(timeout=2.0)
        monitor.return_value.snapshot.assert_not_called()

    def test_smoke_market_is_bounded(self):
        with self.assertRaisesRegex(ValueError, "unsupported smoke market"):
            runtime.run_workspace(Path("installed/bin/property-studio.exe"), "architectural",
                                  Path("evidence"), {}, self.declarations(), {},
                                  market="industrial")

    def test_capture_label_keeps_source_and_reopened_outputs_separate(self):
        declared = self.declarations()
        paths = [next(iter(rows)) for rows in declared.values()]
        records = {runtime.windows_key(path): {"path": Path(path), "sha256": "a" * 64} for path in paths}
        child = mock.Mock(pid=12345)
        child.stdout = io.BytesIO()
        child.stderr = io.BytesIO()
        child.poll.side_effect = [None, 0, 0]
        child.wait.return_value = 0
        with mock.patch.object(runtime.subprocess, "Popen", return_value=child), \
             mock.patch.object(runtime.subprocess, "CREATE_NO_WINDOW", 0x08000000, create=True), \
             mock.patch.object(runtime, "WindowsModules") as monitor, \
             mock.patch.object(runtime, "sha256_file", return_value="a" * 64), \
             mock.patch.object(runtime, "png_evidence", return_value={"sha256": "a" * 64}), \
             mock.patch.object(runtime, "performance_evidence", return_value={"valid": True}), \
             mock.patch.object(runtime.time, "sleep"):
            monitor.return_value.snapshot.return_value = paths
            result = runtime.run_workspace(
                Path("installed/bin/property-studio.exe"), "architectural",
                Path("evidence"), {"PATH": "Windows"}, declared, records,
                market="residential", capture_label="source")
        self.assertTrue(result["passed"], result["errors"])
        self.assertTrue(any(arg.endswith("architectural-residential-source.png") for arg in
                            result["arguments"]))
        self.assertTrue(any(arg.endswith("architectural-residential-source-model.png") for arg in
                            result["arguments"]))
        self.assertEqual(len(result["native_3d"]), 1)

    def test_compare_output_evidence_reports_per_artifact_hashes(self):
        source = {
            "project": {"sha256": "p"},
            "screenshots": [{"path": "drawing.png", "sha256": "d"},
                            {"path": "model.png", "sha256": "m"}],
        }
        reopened = {
            "project": {"sha256": "p"},
            "screenshots": [{"path": "drawing-reopened.png", "sha256": "d"},
                            {"path": "model-reopened.png", "sha256": "m"}],
        }
        result = runtime.compare_output_evidence(source, reopened)
        self.assertTrue(result["project_hash_match"])
        self.assertTrue(result["screenshot_hashes_match"])
        self.assertTrue(result["all_hashes_match"])

    def test_presentation_hash_difference_does_not_fail_stable_roundtrip(self):
        source = {
            "project": {"sha256": "p"},
            "screenshots": [{"sha256": "selected"}, {"sha256": "model"}],
            "native_3d": [{"sha256": "model"}],
        }
        reopened = {
            "project": {"sha256": "p"},
            "screenshots": [{"sha256": "unselected"}, {"sha256": "model"}],
            "native_3d": [{"sha256": "model"}],
        }
        result = runtime.compare_output_evidence(source, reopened)
        self.assertFalse(result["screenshot_hashes_match"])
        self.assertTrue(result["stable_hashes_match"])
        self.assertFalse(result["all_hashes_match"])

    def test_failure_still_writes_report_without_launching(self):
        with tempfile.TemporaryDirectory() as temporary:
            evidence = Path(temporary) / "evidence"
            with mock.patch.object(runtime.subprocess, "Popen") as launch:
                result = runtime.main(["--install-root", str(Path(temporary) / "missing"),
                                       "--evidence-root", str(evidence)])
            self.assertEqual(result, 1)
            launch.assert_not_called()
            reports = list(evidence.glob("run-*/report.json"))
            self.assertEqual(len(reports), 1)
            report = json.loads(reports[0].read_text())
            self.assertFalse(report["passed"])
            for key in ("network_denied", "clean_machine", "production_qualified"):
                self.assertIs(report[key], False)
            self.assertTrue(report["errors"])


if __name__ == "__main__":
    unittest.main()
