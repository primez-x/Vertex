import hashlib
import importlib.util
import pathlib
import tempfile
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location(
    "apex_fixture_evidence", ROOT / "scripts" / "apex_fixture_evidence.py"
)
module = importlib.util.module_from_spec(spec)
spec.loader.exec_module(module)


def _fixture_hash(path: pathlib.Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def _base_manifest(root: pathlib.Path, fixture: pathlib.Path):
    return {
        "schema_version": "1.0",
        "native_format": "unknown",
        "provenance": {
            "observed_identity": "apex-sketch-v7-pro"
        },
        "producing": {
            "build": "9.9.9",
            "modules": ["m1", "m2"],
            "settings": ["metric"],
            "permissions": ["read"],
        },
        "evidence_refs": [
            {"kind": "file", "value": str(fixture.relative_to(root))},
            {"kind": "uri", "value": "https://apex.example/fixture"},
        ],
        "files": [
            {
                "path": str(fixture.relative_to(root)),
                "expected_sha256": _fixture_hash(fixture),
            }
        ],
    }


class ApexFixtureEvidenceTests(unittest.TestCase):
    def test_valid_capture_keeps_native_unknown_and_defers_compatibility(self):
        with tempfile.TemporaryDirectory() as directory:
            root = pathlib.Path(directory)
            fixture = root / "sample.ax7"
            fixture.write_text("native fixture bytes", encoding="utf-8")
            manifest = _base_manifest(root, fixture)

            output = module.validate_and_build(manifest, root=root)
            self.assertEqual(output["native_format"], "unknown")
            self.assertTrue(output["capture_complete"])
            self.assertFalse(output["compatibility_passed"])
            self.assertIn("Not claiming Apex import compatibility", output["compatibility_reason"])
            self.assertEqual(output["captures"][0]["bytes"], fixture.stat().st_size)
            self.assertEqual(output["captures"][0]["path"], str(fixture.relative_to(root)))

    def test_duplicate_json_key_is_rejected(self):
        with tempfile.TemporaryDirectory() as directory:
            manifest = pathlib.Path(directory) / "manifest.json"
            payload = (
                '{\n'
                '  "schema_version": "1.0",\n'
                '  "schema_version": "0.1",\n'
                '  "native_format": "unknown",\n'
                '  "provenance": {"observed_identity": "x"},\n'
                '  "producing": {"build":"x","modules":[],"settings":[],"permissions":[]},\n'
                '  "evidence_refs": [],\n'
                '  "files": []\n'
                '}'
            )
            manifest.write_text(payload, encoding="utf-8")
            with self.assertRaises(ValueError):
                module.load_json_no_duplicates(manifest)

    def test_native_format_must_be_unknown(self):
        with tempfile.TemporaryDirectory() as directory:
            root = pathlib.Path(directory)
            fixture = root / "format.ax7"
            fixture.write_text("fixture", encoding="utf-8")
            manifest = {
                "schema_version": "1.0",
                "native_format": "ax7",
                "provenance": {"observed_identity": "x"},
                "producing": {"build": "x", "modules": [], "settings": [], "permissions": []},
                "evidence_refs": [{"kind": "uri", "value": "https://apex.example/fixture"}],
                "files": [{"path": "format.ax7", "expected_sha256": _fixture_hash(fixture)}],
            }
            with self.assertRaises(ValueError):
                module.validate_and_build(manifest, root=root)

    def test_malformed_hash_is_rejected(self):
        with tempfile.TemporaryDirectory() as directory:
            root = pathlib.Path(directory)
            fixture = root / "bad-hash.ax7"
            fixture.write_text("fixture", encoding="utf-8")
            manifest = {
                "schema_version": "1.0",
                "native_format": "unknown",
                "provenance": {"observed_identity": "x"},
                "producing": {"build": "x", "modules": [], "settings": [], "permissions": []},
                "evidence_refs": [],
                "files": [{"path": "bad-hash.ax7", "expected_sha256": "not-a-sha"}],
            }
            with self.assertRaises(ValueError):
                module.validate_and_build(manifest, root=root)

    def test_missing_observed_identity_is_rejected(self):
        with tempfile.TemporaryDirectory() as directory:
            root = pathlib.Path(directory)
            fixture = root / "missing-id.ax7"
            fixture.write_text("fixture", encoding="utf-8")
            manifest = {
                "schema_version": "1.0",
                "native_format": "unknown",
                "provenance": {},
                "producing": {"build": "x", "modules": [], "settings": [], "permissions": []},
                "evidence_refs": [{"kind": "uri", "value": "https://apex.example/fixture"}],
                "files": [{"path": "missing-id.ax7", "expected_sha256": _fixture_hash(fixture)}],
            }
            with self.assertRaises(ValueError):
                module.validate_and_build(manifest, root=root)

    def test_unsupported_schema_version_is_rejected(self):
        with tempfile.TemporaryDirectory() as directory:
            root = pathlib.Path(directory)
            fixture = root / "unsupported.ax7"
            fixture.write_text("fixture", encoding="utf-8")
            manifest = {
                "schema_version": "2.0",
                "native_format": "unknown",
                "provenance": {"observed_identity": "x"},
                "producing": {"build": "x", "modules": [], "settings": [], "permissions": []},
                "evidence_refs": [{"kind": "uri", "value": "https://apex.example/fixture"}],
                "files": [{"path": "unsupported.ax7", "expected_sha256": _fixture_hash(fixture)}],
            }
            with self.assertRaises(ValueError):
                module.validate_and_build(manifest, root=root)

    def test_unsupported_evidence_ref_is_rejected(self):
        with tempfile.TemporaryDirectory() as directory:
            root = pathlib.Path(directory)
            fixture = root / "invalid-ref.ax7"
            fixture.write_text("fixture", encoding="utf-8")
            manifest = {
                "schema_version": "1.0",
                "native_format": "unknown",
                "provenance": {"observed_identity": "x"},
                "producing": {"build": "x", "modules": [], "settings": [], "permissions": []},
                "evidence_refs": [{"kind": "file", "value": "../outside.txt"}],
                "files": [{"path": "invalid-ref.ax7", "expected_sha256": _fixture_hash(fixture)}],
            }
            with self.assertRaises(ValueError):
                module.validate_and_build(manifest, root=root)
