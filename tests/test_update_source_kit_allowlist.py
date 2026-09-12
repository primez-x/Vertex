from __future__ import annotations

import contextlib
import importlib.util
import io
import json
import pathlib
import tempfile
import unittest
from unittest import mock


ROOT = pathlib.Path(__file__).resolve().parents[1]
SPEC = importlib.util.spec_from_file_location(
    "update_source_kit_allowlist", ROOT / "scripts" / "update_source_kit_allowlist.py"
)
assert SPEC is not None and SPEC.loader is not None
allowlist_module = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(allowlist_module)


class SourceKitAllowlistTests(unittest.TestCase):
    def test_build_uses_explicit_git_paths_and_excludes_local_inputs(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = pathlib.Path(temporary)
            tracked = [
                "temp.txt",
                "build/property-studio.exe",
                ".deps/qt/bin/Qt6Core.dll",
                "secrets/signing.pem",
                "src/main.cpp",
                "include/sketch/boundary_transform.hpp",
                "docs/packaging.md",
                "assets/fonts/Inter.ttf",
                "third_party/example/LICENSE",
            ]

            document = allowlist_module.build_allowlist(root, tracked_paths=tracked)

            self.assertEqual(
                [entry["path"] for entry in document["entries"]],
                [
                    "assets/fonts/Inter.ttf",
                    "docs/packaging.md",
                    "include/sketch/boundary_transform.hpp",
                    "src/main.cpp",
                    "third_party/example/LICENSE",
                ],
            )
            self.assertEqual(
                [entry["category"] for entry in document["entries"]],
                ["fixtures", "docs", "source", "source", "licenses"],
            )

    def test_write_is_stable_and_validates_generated_shape(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = pathlib.Path(temporary)
            output = root / "packaging" / "source-kit-allowlist.json"
            document = allowlist_module.build_allowlist(
                root,
                tracked_paths=["src/main.cpp", "README.md", "LICENSE"],
            )

            allowlist_module.write_allowlist(root, output, document)
            first = output.read_bytes()
            allowlist_module.write_allowlist(root, output, document)
            second = output.read_bytes()

            self.assertEqual(first, second)
            self.assertEqual(json.loads(first), document)

    def test_check_cli_reports_missing_tracked_path_then_passes(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = pathlib.Path(temporary)
            output = root / "packaging" / "source-kit-allowlist.json"
            tracked = ["src/main.cpp", "include/sketch/boundary_transform.hpp"]
            incomplete = allowlist_module.build_allowlist(root, tracked_paths=[tracked[0]])
            allowlist_module.write_allowlist(root, output, incomplete)

            with mock.patch.object(allowlist_module.manifest, "list_tracked_files", return_value=tracked):
                captured = io.StringIO()
                with contextlib.redirect_stdout(captured):
                    result = allowlist_module.main([
                        "--root", str(root), "--output", str(output), "--check"
                    ])
            self.assertEqual(result, 1)
            self.assertIn("boundary_transform.hpp", captured.getvalue())

            complete = allowlist_module.build_allowlist(root, tracked_paths=tracked)
            allowlist_module.write_allowlist(root, output, complete)
            with mock.patch.object(allowlist_module.manifest, "list_tracked_files", return_value=tracked):
                captured = io.StringIO()
                with contextlib.redirect_stdout(captured):
                    result = allowlist_module.main([
                        "--root", str(root), "--output", str(output), "--check"
                    ])
            self.assertEqual(result, 0)
            self.assertIn("PASS", captured.getvalue())


if __name__ == "__main__":
    unittest.main()
