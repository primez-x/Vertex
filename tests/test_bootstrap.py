import importlib.util
import io
import pathlib
import re
import tempfile
import unittest
import zipfile


ROOT = pathlib.Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location("bootstrap", ROOT / "scripts" / "bootstrap.py")
bootstrap = importlib.util.module_from_spec(spec)
spec.loader.exec_module(bootstrap)


class DependencyIntegrityTests(unittest.TestCase):
    def test_pinned_planegcs_source_bytes_match_provenance(self):
        vendor = ROOT / "third_party" / "planegcs"
        upstream = (vendor / "upstream").resolve()
        records = re.findall(r"^([0-9a-f]{64})  (upstream/[^\r\n]+)$",
                             (vendor / "SOURCE.md").read_text(encoding="utf-8"),
                             flags=re.MULTILINE)
        self.assertEqual(len(records), 13, "Every pinned upstream source must be accounted for")
        self.assertEqual(len({name for _, name in records}), len(records))
        for digest, name in records:
            with self.subTest(source=name):
                source = (vendor / name).resolve()
                self.assertTrue(source.is_relative_to(upstream))
                bootstrap.verify(source, digest)

    def test_hash_mismatch_never_extracts(self):
        with tempfile.TemporaryDirectory() as directory:
            archive = pathlib.Path(directory) / "archive.zip"
            archive.write_bytes(b"tampered")
            with self.assertRaises(ValueError):
                bootstrap.verify(archive, "0" * 64)

    def test_traversal_and_windows_absolute_paths_are_rejected(self):
        for name in ("../outside", "a/../../outside", "C:/outside", "/outside", "a\\..\\..\\outside", "file:stream"):
            with self.subTest(name=name), tempfile.TemporaryDirectory() as directory:
                data = io.BytesIO()
                with zipfile.ZipFile(data, "w") as archive:
                    archive.writestr(name, "bad")
                data.seek(0)
                with zipfile.ZipFile(data) as archive, self.assertRaises(ValueError):
                    bootstrap.extract_zip(archive, pathlib.Path(directory) / "destination")
                self.assertFalse((pathlib.Path(directory) / "destination").exists())

    def test_symlink_entry_is_rejected(self):
        with tempfile.TemporaryDirectory() as directory:
            data = io.BytesIO()
            with zipfile.ZipFile(data, "w") as archive:
                entry = zipfile.ZipInfo("link")
                entry.create_system = 3
                entry.external_attr = 0o120777 << 16
                archive.writestr(entry, "../../outside")
            data.seek(0)
            with zipfile.ZipFile(data) as archive, self.assertRaises(ValueError):
                bootstrap.extract_zip(archive, pathlib.Path(directory) / "destination")

    def test_verified_tree_is_written(self):
        with tempfile.TemporaryDirectory() as directory:
            data = io.BytesIO()
            with zipfile.ZipFile(data, "w") as archive:
                archive.writestr("source/header.h", "safe")
            data.seek(0)
            with zipfile.ZipFile(data) as archive:
                bootstrap.extract_zip(archive, pathlib.Path(directory) / "destination")
            self.assertEqual((pathlib.Path(directory) / "destination/source/header.h").read_text(), "safe")


if __name__ == "__main__":
    unittest.main()
