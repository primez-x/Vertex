from __future__ import annotations

import copy
import importlib.util
import json
from pathlib import Path
import tempfile
import unittest


SPEC = importlib.util.spec_from_file_location(
    "offline_static_audit", Path(__file__).resolve().parents[1] / "scripts" / "offline_static_audit.py")
assert SPEC is not None and SPEC.loader is not None
audit = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(audit)


def fixture():
    return {
        "entry_points": [
            r"C:\build\property-studio.exe",
            r"C:\build\property-cli.exe",
            r"C:\build\property_planegcs.dll",
        ],
        "modules": [
            {"path": r"C:\build\property-studio.exe", "imports": [
                {"name": "Qt6Core.dll", "kind": "local-component"}]},
            {"path": r"C:\build\property-cli.exe", "imports": [
                {"name": "Qt6Core.dll", "kind": "local-component"}]},
            {"path": r"C:\build\property_planegcs.dll", "imports": [],},
            {"path": r"C:\deps\Qt6Core.dll", "imports": [
                {"name": "WS2_32.dll", "kind": "installed-system-runtime"}]},
        ],
    }


class OfflineStaticAuditTests(unittest.TestCase):
    def test_direct_entry_points_pass_and_transitive_network_import_is_reported(self):
        result = audit.audit_report(fixture())
        self.assertTrue(result["static_network_audit_passed"], result["errors"])
        self.assertEqual(result["network_import_policy"]["transitive_network_imports_observed"], ["ws2_32.dll"])
        self.assertEqual(result["application_entry_points"][0]["direct_network_imports"], [])

    def test_direct_network_import_fails_closed(self):
        value = fixture()
        value["modules"][0]["imports"].append({"name": "WINHTTP.dll", "kind": "installed-system-runtime"})
        result = audit.audit_report(value)
        self.assertFalse(result["static_network_audit_passed"])
        self.assertTrue(any("directly imports network" in item for item in result["errors"]))

    def test_unresolved_import_fails_closed(self):
        value = fixture()
        value["modules"][1]["imports"].append({"name": "missing.dll", "kind": "unresolved"})
        result = audit.audit_report(value)
        self.assertFalse(result["static_network_audit_passed"])
        self.assertTrue(any("unresolved static import" in item for item in result["errors"]))

    def test_application_entry_point_must_be_declared(self):
        value = fixture()
        value["entry_points"] = value["entry_points"][1:]
        result = audit.audit_report(value)
        self.assertFalse(result["static_network_audit_passed"])
        self.assertTrue(any("not declared" in item for item in result["errors"]))

    def test_missing_entry_point_and_duplicate_report_rows_fail(self):
        value = fixture()
        value["modules"].append(copy.deepcopy(value["modules"][0]))
        value["entry_points"] = [value["entry_points"][0], r"C:\build\missing.exe"]
        result = audit.audit_report(value, applications=("property-studio.exe", "missing.exe"))
        self.assertFalse(result["static_network_audit_passed"])
        self.assertTrue(any("duplicate module path" in item for item in result["errors"]))
        self.assertTrue(any("entry point is missing" in item for item in result["errors"]))

    def test_loader_rejects_duplicate_json_keys(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "imports.json"
            path.write_text('{"modules": [], "modules": [], "entry_points": ["x.exe"]}', encoding="utf-8")
            with self.assertRaisesRegex(ValueError, "duplicate JSON key"):
                audit.load_import_report(path)

    def test_output_is_deterministic(self):
        self.assertEqual(audit.audit_report(fixture()), audit.audit_report(fixture()))


if __name__ == "__main__":
    unittest.main()
