"""Inert archive fixtures: byte preservation, notice references and failure paths."""
import hashlib
import importlib.util
import io
import json
from pathlib import Path
import tarfile
import tempfile
import unittest
from unittest import mock

SCRIPT = Path(__file__).resolve().parents[1] / "scripts/qualification/materialize_qt_notices.py"
SPEC = importlib.util.spec_from_file_location("materialize_qt_notices", SCRIPT)
notices = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(notices)


class MaterializeQtNoticesTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.cache = self.root / "cache"
        self.cache.mkdir()
        self.output = self.root / "output"
        self.pins = {}
        self.payloads = {}
        for module in ("qtbase", "qtsvg", "qtwebengine"):
            files = {".tag": b"upstream-revision\n",
                     "LICENSES/LGPL-3.0-only.txt": b"LGPL\r\n\xff",
                     "LICENSES/GPL-3.0-only.txt": b"GPL\n",
                     "src/3rdparty/a/qt_attribution.json": json.dumps({
                         "Id": "a", "LicenseFile": "custom.txt", "Copyright": "Original author"}).encode(),
                     "src/3rdparty/a/custom.txt": b"Original copyright\r\n"}
            if module == "qtsvg":
                files["src/svg/XSVG_LICENSE.txt"] = b"XSVG original permission"
            if module == "qtwebengine":
                files.update({"CHROMIUM_VERSION": b"122\n", "LICENSE.Chromium": b"Chromium BSD",
                    "src/3rdparty/chromium/LICENSE": b"Chromium BSD",
                    "src/3rdparty/chromium/third_party/pdfium/LICENSE": b"PDFium and embedded notices",
                    "src/3rdparty/chromium/third_party/example/README.chromium": b"License File: terms.txt\n",
                    "src/3rdparty/chromium/third_party/example/terms.txt": b"Original terms"})
            self.payloads[module] = files
            self.archive(module, files)
        self.patch = mock.patch.object(notices, "PINNED_ARCHIVES", self.pins)
        self.patch.start()
        self.addCleanup(self.patch.stop)

    def archive(self, module, files, extras=()):
        name = f"{module}-everywhere-src-6.8.3.tar.xz"
        path = self.cache / name
        with tarfile.open(path, "w:xz") as stream:
            for relative, data in files.items():
                entry = tarfile.TarInfo(f"{module}-everywhere-src-6.8.3/{relative}")
                entry.size = len(data)
                stream.addfile(entry, io.BytesIO(data))
            for entry, data in extras:
                stream.addfile(entry, io.BytesIO(data) if data is not None else None)
        self.pins[module] = {"name": name, "bytes": path.stat().st_size,
                            "sha256": hashlib.sha256(path.read_bytes()).hexdigest(),
                            "url": f"https://download.qt.io/archive/qt/6.8/6.8.3/submodules/{name}"}

    def run_materializer(self):
        return notices.materialize(self.cache, self.output)

    def test_preserves_original_bytes_and_portable_deterministic_index(self):
        original = {p.name: p.read_bytes() for p in self.cache.iterdir()}
        report = self.run_materializer()
        self.assertFalse(report["licensing_clearance"])
        self.assertFalse(report["shipped_notice_closure_qualified"])
        self.assertFalse(report["binary_source_derivation_qualified"])
        rows = report["files"]
        row = next(r for r in rows if r["archive_member"].endswith("src/3rdparty/a/custom.txt"))
        self.assertEqual((self.output / row["path"]).read_bytes(), b"Original copyright\r\n")
        gnu = next(r for r in rows if r["archive_member"].endswith("LICENSES/LGPL-3.0-only.txt"))
        self.assertEqual((self.output / gnu["path"]).read_bytes(), b"LGPL\r\n\xff")
        self.assertNotIn(str(self.root), json.dumps(report))
        first = (self.output / "notice-index.json").read_bytes()
        self.output = self.root / "output2"
        self.run_materializer()
        self.assertEqual(first, (self.output / "notice-index.json").read_bytes())
        self.assertEqual(original, {p.name: p.read_bytes() for p in self.cache.iterdir()})
        self.assertTrue(any(r["archive_member"].endswith("example/terms.txt") for r in rows))

    def test_does_not_extract_ordinary_source(self):
        self.payloads["qtbase"]["src/corelib/ordinary.cpp"] = b"not an attribution"
        self.archive("qtbase", self.payloads["qtbase"])
        report = self.run_materializer()
        self.assertFalse(any("ordinary.cpp" in r["archive_member"] for r in report["files"]))

    def test_inline_attribution_files_and_confined_parent_reference(self):
        files = self.payloads["qtbase"]
        files["src/3rdparty/a/qt_attribution.json"] = json.dumps([
            {"Id": "a", "LicenseFile": "../shared/terms.data"},
            {"Id": "b", "Files": ["embedded.h"], "Copyright": "Inline author"}]).encode()
        files["src/3rdparty/shared/terms.data"] = b"Confined parent license"
        files["src/3rdparty/a/embedded.h"] = b"/* original inline notice */"
        self.archive("qtbase", files)
        report = self.run_materializer()
        self.assertTrue(any(r["archive_member"].endswith("embedded.h") for r in report["files"]))
        self.assertTrue(any(r["archive_member"].endswith("terms.data") for r in report["files"]))

    def test_missing_chromium_reference_is_explicit_and_never_fetched(self):
        files = self.payloads["qtwebengine"]
        files["src/3rdparty/chromium/third_party/example/README.chromium"] = b"License File: missing.txt\n"
        self.archive("qtwebengine", files)
        report = self.run_materializer()
        self.assertTrue(any(r["reference"] == "missing.txt" for r in report["unresolved_references"]))
        self.assertFalse(report["shipped_notice_closure_qualified"])

    def test_existing_empty_and_populated_outputs_are_preserved(self):
        self.output.mkdir()
        with self.assertRaises(ValueError):
            self.run_materializer()
        (self.output / "unknown").write_bytes(b"preserve")
        with self.assertRaises(ValueError):
            self.run_materializer()
        self.assertEqual((self.output / "unknown").read_bytes(), b"preserve")

    def test_hash_drift_fails_before_output(self):
        path = self.cache / self.pins["qtbase"]["name"]
        path.write_bytes(path.read_bytes() + b"drift")
        with self.assertRaises(ValueError):
            self.run_materializer()
        self.assertFalse(self.output.exists())

    def test_missing_mandatory_gnu_or_pdfium_text_fails(self):
        for module, member in (("qtbase", "LICENSES/LGPL-3.0-only.txt"),
                               ("qtwebengine", "src/3rdparty/chromium/third_party/pdfium/LICENSE")):
            with self.subTest(member=member):
                files = dict(self.payloads[module])
                files.pop(member)
                self.archive(module, files)
                with self.assertRaises(ValueError):
                    self.run_materializer()
                self.assertFalse(self.output.exists())
                self.archive(module, self.payloads[module])

    def test_unsafe_members_even_unselected_are_rejected(self):
        bad_names = ["../escape", "/absolute", "C:/drive", "qtbase-everywhere-src-6.8.3/a/../escape",
                     "qtbase-everywhere-src-6.8.3/a\\escape", "qtbase-everywhere-src-6.8.3/a:stream",
                     "qtbase-everywhere-src-6.8.3/CON", "qtbase-everywhere-src-6.8.3/trailing."]
        for name in bad_names:
            with self.subTest(name=name):
                entry = tarfile.TarInfo(name)
                entry.size = 1
                self.archive("qtbase", self.payloads["qtbase"], [(entry, b"x")])
                with self.assertRaises(ValueError):
                    self.run_materializer()
                self.assertFalse(self.output.exists())

    def test_links_devices_sparse_and_duplicate_case_names_are_rejected(self):
        for kind in (tarfile.SYMTYPE, tarfile.LNKTYPE, tarfile.CHRTYPE, tarfile.GNUTYPE_SPARSE):
            with self.subTest(kind=kind):
                entry = tarfile.TarInfo("qtbase-everywhere-src-6.8.3/unused")
                entry.type = kind
                entry.linkname = "LICENSES/GPL-3.0-only.txt"
                self.archive("qtbase", self.payloads["qtbase"], [(entry, None)])
                with self.assertRaises((ValueError, tarfile.TarError)):
                    self.run_materializer()
                self.assertFalse(self.output.exists())
        duplicate = tarfile.TarInfo("qtbase-everywhere-src-6.8.3/LICENSES/gpl-3.0-only.txt")
        duplicate.size = 1
        self.archive("qtbase", self.payloads["qtbase"], [(duplicate, b"x")])
        with self.assertRaises(ValueError):
            self.run_materializer()

    def test_missing_qt_attribution_license_and_escaping_reference_fail(self):
        for reference in ("missing.txt", "../../../../escape", "/absolute"):
            with self.subTest(reference=reference):
                files = dict(self.payloads["qtbase"])
                files["src/3rdparty/a/qt_attribution.json"] = json.dumps({"LicenseFile": reference}).encode()
                self.archive("qtbase", files)
                with self.assertRaises(ValueError):
                    self.run_materializer()
                self.assertFalse(self.output.exists())

    def test_archive_entry_and_notice_bounds_fail_before_output(self):
        for constant, limit in (("MAX_ENTRIES", 2), ("MAX_MEMBER_BYTES", 10),
                                 ("MAX_ARCHIVE_EXPANDED_BYTES", 10), ("MAX_NOTICE_BYTES", 5),
                                 ("MAX_TOTAL_NOTICE_BYTES", 20)):
            with self.subTest(constant=constant), mock.patch.object(notices, constant, limit):
                with self.assertRaises(ValueError):
                    self.run_materializer()
                self.assertFalse(self.output.exists())

    def test_file_directory_conflicts_are_rejected(self):
        files = dict(self.payloads["qtbase"])
        files["src/3rdparty/a"] = b"file cannot contain metadata children"
        self.archive("qtbase", files)
        with self.assertRaisesRegex(ValueError, "file/directory conflict"):
            self.run_materializer()
        self.assertFalse(self.output.exists())

    def test_literal_newlines_in_qt_attribution_preserve_original(self):
        files = dict(self.payloads["qtbase"])
        original = b'{"LicenseFile":"custom.txt","Copyright":"First author\nSecond author"}'
        files["src/3rdparty/a/qt_attribution.json"] = original
        self.archive("qtbase", files)
        report = self.run_materializer()
        row = next(r for r in report["files"] if r["archive"] == self.pins["qtbase"]["name"]
                   and r["archive_member"].endswith("qt_attribution.json"))
        self.assertEqual((self.output / row["path"]).read_bytes(), original)

    def test_url_and_chromium_root_references_are_retained_without_network(self):
        files = dict(self.payloads["qtwebengine"])
        files["src/3rdparty/chromium/third_party/example/README.chromium"] = (
            b"License File: //third_party/example/terms.txt\nLicense File: https://example.invalid/terms\n")
        self.archive("qtwebengine", files)
        report = self.run_materializer()
        self.assertTrue(any(r["archive_member"].endswith("example/terms.txt") for r in report["files"]))
        self.assertTrue(any(r["reference"] == "https://example.invalid/terms" for r in report["unresolved_references"]))

    def test_readme_relative_comma_filenames_are_retained_exactly_and_deterministically(self):
        metadata = "src/3rdparty/chromium/third_party/example/README.chromium"
        parent = "src/3rdparty/chromium/third_party/example/"
        files = dict(self.payloads["qtwebengine"])
        files[metadata] = b"License File: LICENSE.apache20, LICENSE\n"
        files[parent + "LICENSE.apache20"] = b"Apache terms\r\n"
        files[parent + "LICENSE"] = b"Second license\x00bytes"
        self.archive("qtwebengine", files)

        report = self.run_materializer()
        for member, content in ((parent + "LICENSE.apache20", b"Apache terms\r\n"),
                                (parent + "LICENSE", b"Second license\x00bytes")):
            row = next(row for row in report["files"] if row["archive_member"].endswith(member))
            self.assertEqual((self.output / row["path"]).read_bytes(), content)
            self.assertEqual(row["selection_reasons"],
                             ["chromium-license-reference", "notice-name-superset"])
        self.assertFalse(any(row["metadata_member"] == metadata for row in report["unresolved_references"]))
        for flag in notices.FLAGS:
            self.assertFalse(report[flag])
        first_report = report
        first_output = {path.relative_to(self.output).as_posix(): path.read_bytes()
                        for path in self.output.rglob("*") if path.is_file()}

        self.output = self.root / "output2"
        replay = self.run_materializer()
        second_output = {path.relative_to(self.output).as_posix(): path.read_bytes()
                         for path in self.output.rglob("*") if path.is_file()}
        self.assertEqual(replay, first_report)
        self.assertEqual(second_output, first_output)

    def test_readme_chromium_root_comma_filenames_are_retained_exactly(self):
        metadata = "src/3rdparty/chromium/third_party/example/README.chromium"
        files = dict(self.payloads["qtwebengine"])
        files[metadata] = b"License File: //a/LICENSE, //b/LICENSE\n"
        files["src/3rdparty/chromium/a/LICENSE"] = b"Chromium root A\r\n"
        files["src/3rdparty/chromium/b/LICENSE"] = b"Chromium root B\x00"
        self.archive("qtwebengine", files)

        report = self.run_materializer()
        for member, content in (("src/3rdparty/chromium/a/LICENSE", b"Chromium root A\r\n"),
                                ("src/3rdparty/chromium/b/LICENSE", b"Chromium root B\x00")):
            row = next(row for row in report["files"] if row["archive_member"].endswith(member))
            self.assertEqual((self.output / row["path"]).read_bytes(), content)
            self.assertEqual(row["selection_reasons"],
                             ["chromium-license-reference", "notice-name-superset"])
        self.assertFalse(any(row["metadata_member"] == metadata for row in report["unresolved_references"]))
        for flag in notices.FLAGS:
            self.assertFalse(report[flag])

    def test_readme_comma_lists_keep_present_and_missing_references_individual(self):
        metadata = "src/3rdparty/chromium/third_party/example/README.chromium"
        parent = "src/3rdparty/chromium/third_party/example/"
        files = dict(self.payloads["qtwebengine"])
        files[metadata] = (b"License File: available.dat, absent.dat\n"
                           b"License File: //present/LICENSE, //missing/LICENSE\n")
        files[parent + "available.dat"] = b"Available exact bytes"
        files["src/3rdparty/chromium/present/LICENSE"] = b"Present root bytes"
        self.archive("qtwebengine", files)

        report = self.run_materializer()
        relative = next(row for row in report["files"]
                        if row["archive_member"].endswith(parent + "available.dat"))
        chromium_root = next(row for row in report["files"]
                             if row["archive_member"].endswith("src/3rdparty/chromium/present/LICENSE"))
        self.assertEqual((self.output / relative["path"]).read_bytes(), b"Available exact bytes")
        self.assertEqual(relative["selection_reasons"], ["chromium-license-reference"])
        self.assertEqual((self.output / chromium_root["path"]).read_bytes(), b"Present root bytes")
        self.assertEqual(chromium_root["selection_reasons"],
                         ["chromium-license-reference", "notice-name-superset"])
        self.assertEqual(
            [(row["metadata_member"], row["resolved_member"], row["reason"])
             for row in report["unresolved_references"]],
            [(metadata, parent + "absent.dat", "not-present-in-pinned-archive"),
             (metadata, "src/3rdparty/chromium/missing/LICENSE", "not-present-in-pinned-archive")])
        for flag in notices.FLAGS:
            self.assertFalse(report[flag])

    def test_readme_urls_and_prose_remain_unresolved_and_unsafe_tokens_fail(self):
        metadata = "src/3rdparty/chromium/third_party/example/README.chromium"
        files = dict(self.payloads["qtwebengine"])
        files[metadata] = (b"License File: https://example.invalid/a, https://example.invalid/b\n"
                           b"License File: prose about a license, see release notes\n")
        self.archive("qtwebengine", files)
        report = self.run_materializer()
        self.assertEqual(report["unresolved_references"], [
            {"module": "qtwebengine", "metadata_member": metadata,
             "reference": "https://example.invalid/a, https://example.invalid/b",
             "reason": "external-or-ambiguous-reference"},
            {"module": "qtwebengine", "metadata_member": metadata,
             "reference": "prose about a license, see release notes",
             "reason": "external-or-ambiguous-reference"}])
        self.assertFalse(any(row["archive_member"].endswith("/a") or
                             row["archive_member"].endswith("/b") for row in report["files"]))

        files[metadata] = b"License File: ../../../../../../escape\n"
        self.archive("qtwebengine", files)
        self.output = self.root / "unsafe-output"
        with self.assertRaisesRegex(ValueError, "escapes archive"):
            self.run_materializer()
        self.assertFalse(self.output.exists())

    def test_output_race_preserves_foreign_directory(self):
        original_mkdir = Path.mkdir

        def concurrent_mkdir(path, *args, **kwargs):
            if path == self.output:
                original_mkdir(path)
                (path / "foreign.txt").write_bytes(b"someone else's output")
            return original_mkdir(path, *args, **kwargs)

        with mock.patch.object(Path, "mkdir", concurrent_mkdir):
            with self.assertRaisesRegex(ValueError, "Output appeared"):
                self.run_materializer()
        self.assertEqual((self.output / "foreign.txt").read_bytes(), b"someone else's output")
        self.assertFalse((self.output / "notice-index.json").exists())

    def test_input_mutation_before_publication_fails(self):
        original_read = notices.read_archive

        def read_then_mutate(cache, module, pin):
            result = original_read(cache, module, pin)
            if module == "qtwebengine":
                path = self.cache / self.pins["qtbase"]["name"]
                path.write_bytes(path.read_bytes() + b"late mutation")
            return result

        with mock.patch.object(notices, "read_archive", read_then_mutate):
            with self.assertRaisesRegex(ValueError, "Input changed before publication"):
                self.run_materializer()
        self.assertFalse(self.output.exists())

    def test_hardlinked_input_archive_is_rejected(self):
        archive = self.cache / self.pins["qtbase"]["name"]
        (self.root / "other-link").hardlink_to(archive)
        with self.assertRaisesRegex(ValueError, "ordinary file"):
            self.run_materializer()
        self.assertFalse(self.output.exists())


if __name__ == "__main__":
    unittest.main()
