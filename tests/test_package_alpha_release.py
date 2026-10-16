import hashlib
import importlib.util
import json
from pathlib import Path
import sys
import tempfile
import unittest
from unittest.mock import patch
import zipfile

SCRIPTS = Path(__file__).resolve().parents[1] / "scripts"
sys.path.insert(0, str(SCRIPTS))
SPEC = importlib.util.spec_from_file_location("package_alpha_release", SCRIPTS / "package_alpha_release.py")
MODULE = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(MODULE)


class AlphaReleaseTests(unittest.TestCase):
    def fixture(self, root):
        bundle = root / "bundle"
        guide = bundle / "source-kit/docs/alpha-testing.md"
        guide.parent.mkdir(parents=True)
        guide.write_text("Tester guide\n")
        (bundle / "bin").mkdir()
        (bundle / "bin/vertex.exe").write_bytes(b"test runtime")
        (bundle / "runtime-manifest.json").write_text("{}")
        (bundle / "verify-offline-bundle.ps1").write_text("# verifier")
        rows = [{"path": "bin/vertex.exe", "install": True},
                {"path": "source-kit/docs/alpha-testing.md", "install": False}]
        (bundle / "offline-bundle-manifest.json").write_text(json.dumps(
            {"files": rows, "dependency_source_kit": {"path": "source-kit"}}))
        return bundle

    def test_archive_has_launcher_source_and_verifiable_file_records(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            bundle = self.fixture(root)
            with patch.object(MODULE.bundle_tools, "verify_bundle") as verify:
                archive_path = MODULE.package(bundle, root / "out", "test-alpha.1", "a" * 40)
                self.assertEqual(verify.call_count, 2)
            with zipfile.ZipFile(archive_path) as archive:
                prefix = "Vertex-test-alpha.1-windows-x64/"
                release = json.loads(archive.read(prefix + "alpha-release.json"))
                self.assertFalse(release["production_qualified"])
                self.assertIn(b"payload\\bin\\vertex.exe", archive.read(prefix + "Start-Vertex.cmd"))
                self.assertIn(prefix + "SOURCE-BUNDLE.zip", archive.namelist())
                self.assertNotIn(prefix + "payload/source-kit/docs/alpha-testing.md", archive.namelist())
                for row in release["files"]:
                    data = archive.read(prefix + row["path"])
                    self.assertEqual(len(data), row["size"])
                    self.assertEqual(hashlib.sha256(data).hexdigest(), row["sha256"])
            checksum = archive_path.with_suffix(".zip.sha256").read_text().split()[0]
            self.assertEqual(checksum, MODULE.digest(archive_path))
            with patch.object(MODULE.bundle_tools, "verify_bundle"):
                with self.assertRaisesRegex(ValueError, "already exists"):
                    MODULE.package(bundle, root / "out", "test-alpha.1", "a" * 40)

    def test_unsafe_identity_and_missing_sources_are_rejected(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            for identity in ("../escape", "bad/name", 'bad&name'):
                with self.assertRaises(ValueError):
                    MODULE.package(root, root / "out", identity, "a" * 40)
            bundle = self.fixture(root)
            manifest = bundle / "offline-bundle-manifest.json"
            value = json.loads(manifest.read_text())
            del value["dependency_source_kit"]
            manifest.write_text(json.dumps(value))
            with patch.object(MODULE.bundle_tools, "verify_bundle"):
                with self.assertRaisesRegex(ValueError, "source kit"):
                    MODULE.package(bundle, root / "out", "test-alpha", "a" * 40)


if __name__ == "__main__":
    unittest.main()
