"""Offline timing evidence validation; no application or hardware benchmark."""
import copy
import importlib.util
import json
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

SCRIPT = Path(__file__).resolve().parents[1] / "scripts" / "performance_benchmark.py"
spec = importlib.util.spec_from_file_location("performance_benchmark", SCRIPT)
benchmark = importlib.util.module_from_spec(spec)
spec.loader.exec_module(benchmark)


def fixture():
    return {
        "workload": {"id": "drawing-50k", "entities": 50000, "objects": 0,
                     "triangles": 0, "sheets": 0, "project_bytes": 0},
        "reference_hardware": "Recorded test machine: CPU/GPU/RAM/OS description",
        "samples_ms": {"navigation": [16.7], "input": [50], "edit": [250],
                       "open": [5000], "save": [5000]},
    }


class PerformanceBenchmarkTests(unittest.TestCase):
    def test_threshold_boundaries_never_qualify_requirements(self):
        report = benchmark.build_report(fixture())
        self.assertEqual(report["audit_status"], "incomplete")
        self.assertTrue(all(m["within_threshold"] for m in report["metrics"].values()))
        self.assertEqual(len(report["unresolved_gates"]), 3)

    def test_nearest_rank_p95_and_failure(self):
        data = fixture()
        data["samples_ms"]["navigation"] = list(range(1, 21))
        report = benchmark.build_report(data)
        self.assertEqual(report["metrics"]["navigation"]["p95_ms"], 19)
        self.assertFalse(report["metrics"]["navigation"]["within_threshold"])
        self.assertEqual(report["threshold_status"], "failed")

    def test_invalid_samples_fail_closed(self):
        for value in ([], [True], [-1], [float("nan")], [float("inf")], ["1"], None):
            with self.subTest(value=value):
                data = fixture()
                data["samples_ms"]["input"] = value
                with self.assertRaises(ValueError):
                    benchmark.build_report(data)

    def test_missing_metric_and_unknown_metric_are_rejected(self):
        for key in ("save", "typo"):
            data = fixture()
            if key == "save":
                del data["samples_ms"][key]
            else:
                data["samples_ms"][key] = [1]
            with self.assertRaises(ValueError):
                benchmark.build_report(data)

    def test_workload_and_hardware_are_explicit(self):
        for key, value in (("reference_hardware", "  "), ("workload", {})):
            data = fixture()
            data[key] = value
            with self.assertRaises(ValueError):
                benchmark.build_report(data)
        for value in (True, -1, 1.5):
            data = fixture()
            data["workload"]["entities"] = value
            with self.assertRaises(ValueError):
                benchmark.build_report(data)

    def test_report_is_deterministic_and_preserves_input(self):
        data = fixture()
        original = copy.deepcopy(data)
        self.assertEqual(benchmark.build_report(data), benchmark.build_report(data))
        self.assertEqual(data, original)

    def test_cli_report_and_invalid_json(self):
        with tempfile.TemporaryDirectory() as directory:
            source = Path(directory) / "samples.json"
            report = Path(directory) / "report.json"
            source.write_text(json.dumps(fixture()), encoding="utf-8")
            result = subprocess.run([sys.executable, str(SCRIPT), "--input", str(source),
                                     "--output", str(report)], capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertEqual(json.loads(report.read_text())["audit_status"], "incomplete")
            slow = fixture()
            slow["samples_ms"]["save"] = [5001]
            source.write_text(json.dumps(slow), encoding="utf-8")
            result = subprocess.run([sys.executable, str(SCRIPT), "--input", str(source)],
                                    capture_output=True, text=True)
            self.assertEqual(result.returncode, 1)
            self.assertEqual(json.loads(result.stdout)["threshold_status"], "failed")
            result = subprocess.run([sys.executable, str(SCRIPT), "--input", str(source),
                                     "--output", str(source)], capture_output=True, text=True)
            self.assertEqual(result.returncode, 2)
            self.assertEqual(json.loads(source.read_text()), slow)
            source.write_text('{"duplicate": 1, "duplicate": 2}', encoding="utf-8")
            result = subprocess.run([sys.executable, str(SCRIPT), "--input", str(source)],
                                    capture_output=True, text=True)
            self.assertEqual(result.returncode, 2)
            self.assertIn("duplicate", result.stderr)


if __name__ == "__main__":
    unittest.main()
