from __future__ import annotations

import hashlib
import importlib.util
import json
import pathlib
import tempfile
import unittest
from unittest import mock


ROOT = pathlib.Path(__file__).resolve().parents[1]
SPEC = importlib.util.spec_from_file_location(
    "source_kit_manifest", ROOT / "scripts" / "source_kit_manifest.py"
)
assert SPEC is not None and SPEC.loader is not None
manifest_module = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(manifest_module)


def digest(path: pathlib.Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def write_json(path: pathlib.Path, value: object) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(value, indent=2) + "\n", encoding="utf-8")


class SourceKitManifestTests(unittest.TestCase):
    def fixture(self):
        directory = tempfile.TemporaryDirectory()
        root = pathlib.Path(directory.name)
        files = {
            "source": root / "src" / "main.cpp",
            "build": root / "build" / "property-studio.exe",
            "docs": root / "docs" / "offline-build.md",
            "licenses": root / "licenses" / "NOTICE.txt",
            "fixtures": root / "tests" / "fixtures" / "sample.json",
        }
        contents = {
            "source": b"int main() {}\n",
            "build": b"windows binary\n",
            "docs": b"offline build notes\n",
            "licenses": b"license notice\n",
            "fixtures": b"{\"fixture\": true}\n",
        }
        for category, path in files.items():
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_bytes(contents[category])

        allowlist = root / "packaging" / "source-kit-allowlist.json"
        entries = [
            {"category": "docs", "path": "docs/offline-build.md"},
            {"category": "fixtures", "path": "tests/fixtures/sample.json"},
            {
                "category": "source",
                "path": "src/main.cpp",
                "sha256": digest(files["source"]).upper(),
            },
            {"category": "licenses", "path": "licenses/NOTICE.txt"},
            {"category": "build", "path": "build/property-studio.exe"},
        ]
        write_json(allowlist, {"schema_version": 1, "entries": entries})
        return directory, root, files, allowlist

    def test_happy_path_writes_hashed_sized_relative_files(self):
        directory, root, files, allowlist = self.fixture()
        self.addCleanup(directory.cleanup)
        output = root / "artifacts" / "source-kit-manifest.json"

        result = manifest_module.generate_manifest(root, allowlist, output)

        self.assertEqual(result["schema_version"], 1)
        self.assertEqual(result["manifest_version"], 1)
        self.assertEqual(result["audit_status"], "incomplete")
        self.assertEqual(
            set(result["category_counts"]),
            {"source", "build", "docs", "licenses", "fixtures"},
        )
        self.assertEqual(result["summary"]["entry_count"], 5)
        paths = [entry["path"] for entry in result["files"]]
        self.assertEqual(paths, sorted(paths, key=str.casefold))
        self.assertEqual(
            {entry["category"] for entry in result["files"]},
            {"source", "build", "docs", "licenses", "fixtures"},
        )
        for entry in result["files"]:
            source = root / pathlib.PurePosixPath(entry["path"])
            self.assertEqual(entry["sha256"], digest(source))
            self.assertEqual(entry["size"], source.stat().st_size)
            self.assertFalse(pathlib.PurePath(entry["path"]).is_absolute())
            self.assertNotIn("\\", entry["path"])
        self.assertNotIn(str(root), json.dumps(result))
        boundary = result["boundary"].lower()
        self.assertIn("complete source kit", boundary)
        self.assertIn("licensing", boundary)
        self.assertIn("sbom", boundary)
        self.assertIn("rebuild", boundary)
        self.assertEqual(result, json.loads(output.read_text(encoding="utf-8")))

    def test_tracked_build_inputs_receive_build_category(self):
        for path in (
            "CMakeLists.txt",
            "CMakePresets.json",
            "cmake/PlaneGCS.cmake",
            "scripts/build.ps1",
            "vcpkg.json",
        ):
            with self.subTest(path=path):
                self.assertEqual(manifest_module.classify_source_kit_path(path), "build")

        self.assertEqual(manifest_module.classify_source_kit_path("src/main.cpp"), "source")

    def test_traversal_is_rejected_without_writing_output(self):
        directory, root, _, allowlist = self.fixture()
        self.addCleanup(directory.cleanup)
        output = root / "artifacts" / "source-kit-manifest.json"
        data = json.loads(allowlist.read_text(encoding="utf-8"))
        data["entries"][0]["path"] = "../outside.txt"
        write_json(allowlist, data)

        with self.assertRaises(manifest_module.ManifestError) as context:
            manifest_module.generate_manifest(root, allowlist, output)

        self.assertIn("unsafe", str(context.exception).lower())
        self.assertFalse(output.exists())

    def test_absolute_path_is_rejected(self):
        directory, root, files, allowlist = self.fixture()
        self.addCleanup(directory.cleanup)
        output = root / "artifacts" / "source-kit-manifest.json"
        data = json.loads(allowlist.read_text(encoding="utf-8"))
        data["entries"][0]["path"] = str(files["docs"])
        write_json(allowlist, data)

        with self.assertRaises(manifest_module.ManifestError) as context:
            manifest_module.generate_manifest(root, allowlist, output)

        self.assertIn("relative", str(context.exception).lower())
        self.assertFalse(output.exists())

    def test_symlink_is_rejected(self):
        directory, root, _, allowlist = self.fixture()
        self.addCleanup(directory.cleanup)
        link = root / "src" / "linked.cpp"
        try:
            link.symlink_to(root / "src" / "main.cpp")
        except (OSError, NotImplementedError) as exc:
            self.skipTest(f"symlinks unavailable: {exc}")
        output = root / "artifacts" / "source-kit-manifest.json"
        data = json.loads(allowlist.read_text(encoding="utf-8"))
        data["entries"] = [{"category": "source", "path": "src/linked.cpp"}]
        write_json(allowlist, data)

        with self.assertRaises(manifest_module.ManifestError) as context:
            manifest_module.generate_manifest(root, allowlist, output)

        self.assertIn("symlink", str(context.exception).lower())
        self.assertFalse(output.exists())

    def test_duplicate_paths_are_rejected_case_insensitively(self):
        directory, root, _, allowlist = self.fixture()
        self.addCleanup(directory.cleanup)
        output = root / "artifacts" / "source-kit-manifest.json"
        data = json.loads(allowlist.read_text(encoding="utf-8"))
        data["entries"] = [
            {"category": "source", "path": "src/main.cpp"},
            {"category": "source", "path": "SRC\\MAIN.CPP"},
        ]
        write_json(allowlist, data)

        with self.assertRaises(manifest_module.ManifestError) as context:
            manifest_module.generate_manifest(root, allowlist, output)

        self.assertIn("duplicate", str(context.exception).lower())
        self.assertFalse(output.exists())

    def test_missing_file_is_rejected(self):
        directory, root, _, allowlist = self.fixture()
        self.addCleanup(directory.cleanup)
        output = root / "artifacts" / "source-kit-manifest.json"
        data = json.loads(allowlist.read_text(encoding="utf-8"))
        data["entries"] = [{"category": "source", "path": "src/missing.cpp"}]
        write_json(allowlist, data)

        with self.assertRaises(manifest_module.ManifestError) as context:
            manifest_module.generate_manifest(root, allowlist, output)

        self.assertIn("missing", str(context.exception).lower())
        self.assertFalse(output.exists())

    def test_actual_filename_casing_is_verified(self):
        directory, root, _, allowlist = self.fixture()
        self.addCleanup(directory.cleanup)
        output = root / "artifacts" / "source-kit-manifest.json"
        data = json.loads(allowlist.read_text(encoding="utf-8"))
        data["entries"] = [{"category": "source", "path": "src/MAIN.cpp"}]
        write_json(allowlist, data)

        with self.assertRaises(manifest_module.ManifestError) as context:
            manifest_module.generate_manifest(root, allowlist, output)

        self.assertIn("actual filename", str(context.exception).lower())
        self.assertFalse(output.exists())

    def test_verify_actual_filename_returns_resolved_file(self):
        directory, root, files, _ = self.fixture()
        self.addCleanup(directory.cleanup)

        resolved = manifest_module.verify_actual_filename(root, "src/main.cpp")

        self.assertEqual(resolved, files["source"].resolve())

    def test_tracked_source_scope_uses_git_and_ignores_untracked_or_excluded_paths(self):
        directory, root, _, _ = self.fixture()
        self.addCleanup(directory.cleanup)
        tracked = (
            b"temp.txt\x00"
            b"build/property-studio.exe\x00"
            b".deps/qt/bin/Qt6Core.dll\x00"
            b"secrets/signing.pem\x00"
            b"src/main.cpp\x00"
            b"include/sketch/boundary_transform.hpp\x00"
        )
        completed = mock.Mock(returncode=0, stdout=tracked, stderr=b"")
        with mock.patch.object(manifest_module.subprocess, "run", return_value=completed) as run:
            paths = manifest_module.tracked_source_kit_files(root)

        self.assertEqual(paths, ["include/sketch/boundary_transform.hpp", "src/main.cpp"])
        run.assert_called_once()
        command = run.call_args.args[0]
        self.assertEqual(command[-4:], ["ls-files", "--cached", "--full-name", "-z"])

    def test_completeness_check_reports_missing_and_category_drift(self):
        directory, root, _, allowlist = self.fixture()
        self.addCleanup(directory.cleanup)
        data = json.loads(allowlist.read_text(encoding="utf-8"))
        data["entries"] = [
            {"category": "source", "path": "src/main.cpp"},
            {"category": "source", "path": "tests/fixtures/sample.json"},
        ]
        write_json(allowlist, data)
        completed = mock.Mock(
            returncode=0,
            stdout=(
                b"src/main.cpp\x00"
                b"include/sketch/boundary_transform.hpp\x00"
                b"tests/fixtures/sample.json\x00"
            ),
            stderr=b"",
        )
        with mock.patch.object(manifest_module.subprocess, "run", return_value=completed):
            report = manifest_module.check_allowlist_completeness(root, allowlist)

        self.assertFalse(report["complete"])
        self.assertEqual(report["missing"], ["include/sketch/boundary_transform.hpp"])
        self.assertEqual(report["classification_mismatches"], [{
            "path": "tests/fixtures/sample.json",
            "expected": "fixtures",
            "actual": "source",
        }])

    def test_stale_optional_hash_is_rejected(self):
        directory, root, _, allowlist = self.fixture()
        self.addCleanup(directory.cleanup)
        output = root / "artifacts" / "source-kit-manifest.json"
        data = json.loads(allowlist.read_text(encoding="utf-8"))
        data["entries"] = [
            {"category": "source", "path": "src/main.cpp", "sha256": "0" * 64}
        ]
        write_json(allowlist, data)

        with self.assertRaises(manifest_module.ManifestError) as context:
            manifest_module.generate_manifest(root, allowlist, output)

        self.assertIn("hash", str(context.exception).lower())
        self.assertFalse(output.exists())

    def test_allowlist_order_does_not_change_manifest_bytes(self):
        directory, root, _, allowlist = self.fixture()
        self.addCleanup(directory.cleanup)
        first_output = root / "artifacts" / "first.json"
        second_output = root / "artifacts" / "second.json"
        original = json.loads(allowlist.read_text(encoding="utf-8"))

        manifest_module.generate_manifest(root, allowlist, first_output)
        original["entries"].reverse()
        write_json(allowlist, original)
        manifest_module.generate_manifest(root, allowlist, second_output)

        self.assertEqual(
            first_output.read_bytes(), second_output.read_bytes(),
            "manifest serialization must be stable when allowlist order changes",
        )


if __name__ == "__main__":
    unittest.main()
