"""Desktop report validation tests; no application or qualification claims."""
import hashlib
import importlib.util
import json
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

SCRIPT = Path(__file__).resolve().parents[1] / "scripts" / "performance_report.py"
spec = importlib.util.spec_from_file_location("performance_report", SCRIPT)
validator = importlib.util.module_from_spec(spec)
spec.loader.exec_module(validator)


def fixture():
    return {
        "schema_version": 1,
        "audit_status": "incomplete",
        "evidence_scope": "application-local samples; hardware qualification excluded",
        "reference_hardware": "unspecified; caller assertion only",
        "workload": {"id": "interactive-session", "document_id": "test-document",
                     "revision": 3, "entities": 1, "objects": 0, "sheets": 20,
                     "triangles": None, "project_bytes": None},
        "metrics": {
            name: {"sample_count": 1, "dropped_sample_count": 0,
                   "threshold_ms": limit, "p95_ms": limit,
                   "has_samples": True, "within_threshold": True}
            for name, limit in (("navigation", 16.7), ("input", 50), ("edit", 250),
                                ("open", 5000), ("save", 5000))
        },
        "threshold_status": "within_targets",
    }


class PerformanceReportTests(unittest.TestCase):
    def setUp(self):
        self.directory = tempfile.TemporaryDirectory()
        self.addCleanup(self.directory.cleanup)
        self.path = Path(self.directory.name) / "report.json"

    def write(self, report):
        data = (json.dumps(report, indent=2) + "\n").encode("utf-8")
        self.path.write_bytes(data)
        return data

    def test_hash_covers_exact_validated_bytes(self):
        data = self.write(fixture())
        evidence = validator.performance_evidence(self.path)
        self.assertEqual(evidence["sha256"], hashlib.sha256(data).hexdigest())
        self.assertEqual(evidence["bytes"], len(data))
        self.assertEqual(evidence["threshold_status"], "within_targets")

    def test_native_sample_capacity_is_enforced(self):
        report = fixture()
        report["metrics"]["input"]["sample_count"] = 4096
        report["metrics"]["input"]["dropped_sample_count"] = 100
        validator.validate_report(report)
        report["metrics"]["input"]["sample_count"] = 4097
        with self.assertRaises(ValueError):
            validator.validate_report(report)

    def test_missing_samples_and_exceeded_targets_are_valid_evidence(self):
        report = fixture()
        report["metrics"]["open"].update(p95_ms=5001, within_threshold=False)
        report["threshold_status"] = "exceeds_targets"
        validator.validate_report(report)
        report["metrics"]["input"].update(sample_count=0, p95_ms=None,
                                           has_samples=False, within_threshold=False)
        report["threshold_status"] = "incomplete"
        validator.validate_report(report)

    def test_qualification_and_inconsistent_status_are_rejected(self):
        for key, value in (("audit_status", "complete"), ("threshold_status", "incomplete"),
                           ("schema_version", True), ("reference_hardware", " \t")):
            with self.subTest(key=key):
                report = fixture()
                report[key] = value
                with self.assertRaises(ValueError):
                    validator.validate_report(report)

    def test_metric_types_and_consistency_fail_closed(self):
        for key, value in (("sample_count", True), ("sample_count", -1),
                           ("dropped_sample_count", -1), ("has_samples", False),
                           ("within_threshold", False), ("threshold_ms", 51),
                           ("p95_ms", None), ("p95_ms", True), ("p95_ms", "1"),
                           ("p95_ms", float("inf")), ("p95_ms", -1)):
            with self.subTest(key=key, value=value):
                report = fixture()
                report["metrics"]["input"][key] = value
                with self.assertRaises(ValueError):
                    validator.validate_report(report)

    def test_workload_counts_preserve_unknowns_and_sheet_children(self):
        validator.validate_report(fixture())  # Sheets can outnumber entities.
        for key, value in (("entities", None), ("objects", 2), ("sheets", True),
                           ("project_bytes", -1), ("triangles", 1.5), ("id", " ")):
            with self.subTest(key=key):
                report = fixture()
                report["workload"][key] = value
                with self.assertRaises(ValueError):
                    validator.validate_report(report)

    def test_missing_and_unknown_metrics_are_rejected(self):
        for missing in (True, False):
            report = fixture()
            if missing:
                del report["metrics"]["save"]
            else:
                report["metrics"]["other"] = report["metrics"]["save"]
            with self.assertRaises(ValueError):
                validator.validate_report(report)

    def test_invalid_json_is_a_controlled_error(self):
        valid = json.dumps(fixture()).encode()
        for data in (b"", b"\xff", b"[]", b"{} {}",
                     valid.replace(b'"schema_version": 1', b'"schema_version": 1, "schema_version": 1'),
                     valid.replace(b'"p95_ms": 50', b'"p95_ms": NaN'),
                     valid.replace(b'"p95_ms": 50', b'"p95_ms": 1e999'),
                     b"[" * 2000 + b"]" * 2000):
            with self.subTest(data=data[:50]):
                self.path.write_bytes(data)
                with self.assertRaises(ValueError):
                    validator.performance_evidence(self.path)

    def test_size_limit_and_missing_file(self):
        data = self.write(fixture())
        self.path.write_bytes(data + b" " * (validator.MAX_REPORT_BYTES - len(data)))
        self.assertEqual(validator.performance_evidence(self.path)["bytes"], validator.MAX_REPORT_BYTES)
        with self.path.open("ab") as stream:
            stream.write(b" ")
        with self.assertRaises(ValueError):
            validator.performance_evidence(self.path)
        self.path.unlink()
        with self.assertRaises(ValueError):
            validator.performance_evidence(self.path)

    def test_cli_validates_without_treating_targets_as_qualification(self):
        report = fixture()
        report["metrics"]["save"].update(p95_ms=5001, within_threshold=False)
        report["threshold_status"] = "exceeds_targets"
        self.write(report)
        result = subprocess.run([sys.executable, str(SCRIPT), str(self.path)],
                                capture_output=True, text=True, check=False)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(json.loads(result.stdout)["threshold_status"], "exceeds_targets")
        self.path.write_text("{}", encoding="utf-8")
        result = subprocess.run([sys.executable, str(SCRIPT), str(self.path)],
                                capture_output=True, text=True, check=False)
        self.assertEqual(result.returncode, 1)
        self.assertEqual(result.stdout, "")
        self.assertIn("invalid performance report", result.stderr)
        self.assertNotIn("Traceback", result.stderr)


if __name__ == "__main__":
    unittest.main()
