"""Portable checks for the installed-runtime smoke policy; never launch an app."""

import importlib.util
import io
import json
import os
from pathlib import Path
import struct
import tempfile
import unittest
from unittest import mock


SPEC = importlib.util.spec_from_file_location(
    "installed_runtime", Path(__file__).resolve().parents[1] / "scripts" / "test_installed_runtime.py")
runtime = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(runtime)


class InstalledRuntimeTests(unittest.TestCase):
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
             mock.patch.object(runtime.time, "sleep"):
            monitor.return_value.snapshot.return_value = paths
            with mock.patch.object(runtime, "project_evidence", return_value={"valid": True}) as project:
                result = runtime.run_workspace(
                    Path("installed/bin/property-studio.exe"), "architectural",
                    Path("evidence"), {"PATH": "Windows"}, declared, records,
                    project_output=Path("evidence/source.bldproj"),
                    project_input=Path("evidence/original.bldproj"))
        self.assertTrue(result["passed"], result["errors"])
        self.assertEqual(len(result["screenshots"]), 2)
        project.assert_called_once_with(Path("evidence/source.bldproj"))
        self.assertEqual(launch.call_args.kwargs["creationflags"], 0x08000000)
        self.assertFalse(launch.call_args.kwargs["shell"])
        self.assertEqual(launch.call_args.kwargs["cwd"], Path("installed/bin"))
        self.assertIn("--smoke-3d-output", launch.call_args.args[0])
        self.assertIn("--smoke-project-output", launch.call_args.args[0])
        self.assertIn("--smoke-project-input", launch.call_args.args[0])
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
