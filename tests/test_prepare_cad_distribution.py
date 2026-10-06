"""Focused policy tests for preparing manifests from pinned CAD archive bytes.

The archives and staged runtime here are synthetic. These tests exercise path
mapping, hash binding, notices, and fail-closed output behavior; they do not
qualify the real SDK archives or prove extraction by the SDK bootstrapper.
"""

from __future__ import annotations

import hashlib
import importlib.util
import io
import json
from pathlib import Path
import sys
import tempfile
import unittest
import zipfile
from types import SimpleNamespace
from unittest import mock


ROOT = Path(__file__).resolve().parents[1]


def _load_module(name: str, path: Path):
    spec = importlib.util.spec_from_file_location(name, path)
    assert spec is not None and spec.loader is not None
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


bootstrap = _load_module("prepare_cad_distribution_bootstrap", ROOT / "scripts" / "bootstrap_cad_runtime.py")
inventory = _load_module("prepare_cad_distribution_inventory", ROOT / "scripts" / "distribution_inventory.py")
with mock.patch.dict(sys.modules, {
    "bootstrap_cad_runtime": bootstrap,
    "distribution_inventory": inventory,
}):
    prepare_distribution = _load_module(
        "prepare_cad_distribution_test_module", ROOT / "scripts" / "prepare_cad_distribution.py"
    )


CANONICAL_PTH = b"python313.zip\n.\nLib/site-packages\n"


def _zip_bytes(entries: dict[str, bytes]) -> bytes:
    output = io.BytesIO()
    with zipfile.ZipFile(output, "w", compression=zipfile.ZIP_STORED) as archive:
        for name, data in entries.items():
            archive.writestr(name, data)
    return output.getvalue()


def _write_json(path: Path, value: object) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(value, indent=2) + "\n", encoding="utf-8")


class PrepareCadDistributionTests(unittest.TestCase):
    def setUp(self) -> None:
        temporary = tempfile.TemporaryDirectory()
        self.addCleanup(temporary.cleanup)
        self.root = Path(temporary.name)
        self.output = self.root / "artifacts" / "cad-generated"
        self.stage = self.root / "build" / "windows-release" / "cad-runtime"
        self.downloads = self.root / ".deps" / "downloads" / "cad-runtime"
        self.downloads.mkdir(parents=True)

        self.interpreter_entries = {
            # Bootstrap rewrites this member to its canonical embedded-Python paths.
            "python313._pth": b"import site\r\n",
            "python.exe": b"launcher",
            "pythonw.exe": b"windowed launcher",
            "vcruntime140.dll": b"shared runtime",
            "vcruntime140_1.dll": b"shared runtime",
            "sqlite3.dll": b"shared sqlite",
            "DLLs/nested/IfcCore.pyd": b"synthetic nested interpreter PE",
            "Lib/site-packages/interpreter_dist/LICENSE.txt": b"synthetic interpreter notice\n",
        }
        self.wheel_entries = {
            "testpkg/__init__.py": b"version = 'fixture'\n",
            "testpkg/native/deep/module.dll": b"synthetic nested wheel PE",
            "testpkg/data/ui.json": b"{\"fixture\": true}\n",
            "testpkg/LICENSE": b"synthetic wheel notice\n",
            "test_wheel-1.0.data/scripts/fixture.exe": b"inert script",
            "test_wheel-1.0.data/data/share/man/man1/fixture.1": b"inert manpage",
        }
        interpreter_archive = _zip_bytes(self.interpreter_entries)
        wheel_archive = _zip_bytes(self.wheel_entries)
        assets = [
            self._asset("CPython", "interpreter", "python-3.13.7-embed-amd64.zip", "3.13.7",
                        interpreter_archive, "PSF-2.0"),
            self._asset("CPython source", "headers", "Python-3.13.7.tar.xz", "3.13.7",
                        b"unused header archive", "PSF-2.0"),
            self._asset("CPython development", "development", "python.3.13.7.nupkg", "3.13.7",
                        b"unused development archive", "PSF-2.0"),
            self._asset("test_wheel", "wheel", "test_wheel-1.0-py3-none-any.whl", "1.0",
                        wheel_archive, "MIT"),
        ]
        self.lock = {
            "schema_version": 1,
            "platform": "win_amd64",
            "python_version": "3.13.7",
            "python_abi": "cp313",
            "library_versions": {"test_wheel": "1.0"},
            "qualification": "incomplete",
            "assets": assets,
        }
        _write_json(self.root / "third_party" / "cad-runtime-lock.json", self.lock)
        _write_json(self.root / "third_party" / "distribution-components.json", {
            "schema_version": 1,
            "audit_status": "incomplete",
            "runtime_evidence": {"path": "artifacts/runtime/release-imports.json", "required": True},
            "package_managers": [],
            "components": [{
                "id": "existing-component",
                "kind": "asset",
                "package": {"name": "Existing fixture", "version": "1", "license": "MIT"},
                "source": {"kind": "workspace", "paths": ["README.md"]},
                "notice_paths": ["LICENSE"],
            }],
        })
        _write_json(self.root / "packaging" / "portable-allowlist.json", {
            "schema_version": 1,
            "entries": [],
        })

        for asset, archive_data in ((assets[0], interpreter_archive), (assets[3], wheel_archive)):
            (self.downloads / asset["filename"]).write_bytes(archive_data)

        # Reproduce the already-built SDK staging tree. Keep the expected
        # extraction list explicit so the fixture does not call the policy
        # function it is meant to verify.
        staged_files = {
            "python313._pth": CANONICAL_PTH,
            "DLLs/nested/IfcCore.pyd": self.interpreter_entries["DLLs/nested/IfcCore.pyd"],
            "Lib/site-packages/interpreter_dist/LICENSE.txt":
                self.interpreter_entries["Lib/site-packages/interpreter_dist/LICENSE.txt"],
            "Lib/site-packages/testpkg/__init__.py": self.wheel_entries["testpkg/__init__.py"],
            "Lib/site-packages/testpkg/native/deep/module.dll":
                self.wheel_entries["testpkg/native/deep/module.dll"],
            "Lib/site-packages/testpkg/data/ui.json": self.wheel_entries["testpkg/data/ui.json"],
            "Lib/site-packages/testpkg/LICENSE": self.wheel_entries["testpkg/LICENSE"],
        }
        for relative, data in staged_files.items():
            path = self.stage / Path(*relative.split("/"))
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_bytes(data)

    def _asset(self, name: str, kind: str, filename: str, version: str,
               archive: bytes, license_name: str) -> dict:
        path = self.downloads / filename
        path.write_bytes(archive)
        host = "files.pythonhosted.org" if kind == "wheel" else "www.python.org"
        return {
            "name": name,
            "version": version,
            "kind": kind,
            "filename": filename,
            "url": f"https://{host}/fixtures/{filename}",
            "sha256": hashlib.sha256(archive).hexdigest(),
            "license": license_name,
        }

    def test_selected_member_excludes_launchers_and_wheel_data_and_rejects_unsafe_schemes(self):
        for name in ("python.exe", "pythonw.exe", "vcruntime140.dll", "vcruntime140_1.dll", "sqlite3.dll"):
            with self.subTest(excluded=name):
                self.assertIsNone(prepare_distribution.selected_member(name, "interpreter"))

        self.assertIsNone(prepare_distribution.selected_member(
            "test_wheel-1.0.data/scripts/fixture.exe", "wheel"))
        self.assertIsNone(prepare_distribution.selected_member(
            "test_wheel-1.0.data/data/share/man/man1/fixture.1", "wheel"))
        with self.assertRaisesRegex(ValueError, "mapping"):
            prepare_distribution.selected_member("test_wheel-1.0.data/purelib/testpkg.py", "wheel")
        with self.assertRaisesRegex(ValueError, "Unsafe Windows archive path"):
            prepare_distribution.selected_member("../escape.py", "wheel")

    def test_payload_maps_nested_pe_and_assets_and_records_notices_without_qualification(self):
        with mock.patch.object(bootstrap, "bootstrap", return_value={}) as sdk_check:
            payload = prepare_distribution.prepare(self.root, self.output)

        sdk_check.assert_called_once_with(
            self.root, self.root / "third_party" / "cad-runtime-lock.json",
            self.root / ".deps" / "cad-runtime" / "3.13.7", offline=True, check=True,
        )
        self.assertEqual(payload["qualification"], "incomplete")
        self.assertFalse(payload["license_clearance"])
        self.assertFalse(payload["source_closure_qualified"])

        rows = {row["path"]: row for row in payload["files"]}
        interpreter_pe = "build/windows-release/cad-runtime/DLLs/nested/IfcCore.pyd"
        wheel_pe = "build/windows-release/cad-runtime/Lib/site-packages/testpkg/native/deep/module.dll"
        pth = "build/windows-release/cad-runtime/python313._pth"
        ui_asset = "build/windows-release/cad-runtime/Lib/site-packages/testpkg/data/ui.json"
        self.assertEqual(rows[interpreter_pe]["destination"], "bin/cad-runtime/DLLs/nested/IfcCore.pyd")
        self.assertTrue(rows[interpreter_pe]["runtime"])
        self.assertEqual(rows[wheel_pe]["destination"],
                         "bin/cad-runtime/Lib/site-packages/testpkg/native/deep/module.dll")
        self.assertTrue(rows[wheel_pe]["runtime"])
        self.assertEqual(rows[pth]["destination"], "bin/cad-runtime/python313._pth")
        self.assertFalse(rows[pth]["runtime"])
        self.assertEqual(rows[ui_asset]["destination"],
                         "bin/cad-runtime/Lib/site-packages/testpkg/data/ui.json")
        self.assertFalse(rows[ui_asset]["runtime"])
        self.assertEqual((self.stage / "python313._pth").read_bytes(), CANONICAL_PTH)

        excluded = {row["member"] for row in payload["excluded_files"]}
        self.assertTrue({"python.exe", "pythonw.exe", "vcruntime140.dll",
                         "vcruntime140_1.dll", "sqlite3.dll"}.issubset(excluded))
        self.assertIn("test_wheel-1.0.data/scripts/fixture.exe", excluded)
        self.assertIn("test_wheel-1.0.data/data/share/man/man1/fixture.1", excluded)

        components = json.loads((self.output / "cad-components.json").read_text(encoding="utf-8"))
        by_id = {component["id"]: component for component in components["components"]}
        self.assertIn("cad-cpython", by_id)
        self.assertIn("cad-test-wheel", by_id)
        wheel_notice = "build/windows-release/cad-runtime/Lib/site-packages/testpkg/LICENSE"
        self.assertIn(wheel_notice, by_id["cad-test-wheel"]["notice_paths"])
        allowlist = json.loads((self.output / "cad-allowlist.json").read_text(encoding="utf-8"))
        self.assertIn({
            "kind": "asset", "inventory_entry": "cad-test-wheel", "path": wheel_notice,
            "destination": "bin/cad-runtime/Lib/site-packages/testpkg/LICENSE",
        }, allowlist["entries"])
        self.assertIn({
            "kind": "notice", "inventory_entry": "cad-test-wheel", "path": wheel_notice,
            "destination": "licenses/cad-test-wheel/0-LICENSE",
        }, allowlist["entries"])
        self.assertEqual(json.loads((self.output / "cad-payload.json").read_text(encoding="utf-8")), payload)

    def test_staged_tamper_fails_before_creating_output(self):
        tampered = self.stage / "DLLs" / "nested" / "IfcCore.pyd"
        tampered.write_bytes(b"changed after archive extraction")

        with mock.patch.object(bootstrap, "bootstrap", return_value={}), self.assertRaisesRegex(
            ValueError, "differs from pinned member"
        ):
            prepare_distribution.prepare(self.root, self.output)

        self.assertFalse(self.output.exists())

    def test_existing_hardlinked_output_does_not_overwrite_input(self):
        self.output.mkdir(parents=True)
        source = self.root / "keep-input.txt"
        source.write_bytes(b"preserve this unrelated input")
        target = self.output / "cad-components.json"
        try:
            target.hardlink_to(source)
        except (OSError, NotImplementedError) as error:
            self.skipTest(f"hard links are unavailable: {error}")
        with mock.patch.object(bootstrap, "bootstrap", return_value={}):
            prepare_distribution.prepare(self.root, self.output)
        self.assertEqual(source.read_bytes(), b"preserve this unrelated input")
        self.assertIn("components", json.loads(target.read_text(encoding="utf-8")))

    def test_linked_output_is_rejected_before_sdk_verification(self):
        with tempfile.TemporaryDirectory() as outside:
            linked_output = self.root / "linked-output"
            try:
                linked_output.symlink_to(Path(outside), target_is_directory=True)
            except (OSError, NotImplementedError) as error:
                self.skipTest(f"directory symlinks are unavailable: {error}")

            with mock.patch.object(bootstrap, "bootstrap", side_effect=AssertionError("must reject first")):
                with self.assertRaisesRegex(ValueError, "must not cross a link"):
                    prepare_distribution.prepare(self.root, linked_output)
            self.assertEqual(list(Path(outside).iterdir()), [])

    def controlled_fixture(self):
        old_wheel = self._asset("ifcopenshell", "wheel", "ifcopenshell-0.8.5-cp313-cp313-win_amd64.whl",
                                "0.8.5", _zip_bytes({"ifcopenshell/old.py": b"old wheel"}), "LGPL-3.0-or-later")
        self.lock["assets"].append(old_wheel)
        self.lock["library_versions"]["ifcopenshell"] = "0.8.5"
        _write_json(self.root / "third_party/cad-runtime-lock.json", self.lock)
        entries = {
            "Lib/site-packages/ifcopenshell/_ifcopenshell_wrapper.cp313-win_amd64.pyd": b"MZselected IFC",
            "Lib/site-packages/ifcopenshell/ifcopenshell_wrapper.py": b"generated wrapper",
            "Lib/site-packages/ifcopenshell/__init__.py": b"version = '0.0.0'",
            "notices/ifcopenshell/COPYING.LESSER": b"GNU LESSER GENERAL PUBLIC LICENSE",
            "receipts/build.json": b'{"source_closure_qualified":false}',
            "controlled-runtime-manifest.json": b'{"qualification":"incomplete"}',
        }
        for name, data in entries.items():
            path = self.stage / name
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_bytes(data)
        selection = self.root / "build/windows-release/cad-runtime-selection.json"
        _write_json(selection, {"kind": "controlled", "source_revision": "a" * 40})
        verified = {
            "identity": {"kind": "controlled", "source_revision": "a" * 40},
            "sdk_root": ".deps/controlled-sdk", "staged_root": "build/windows-release/cad-runtime",
            "selection_record": {"path": selection.relative_to(self.root).as_posix(),
                                 "sha256": hashlib.sha256(selection.read_bytes()).hexdigest(), "bytes": selection.stat().st_size},
            "payload": [{"path": name, "sha256": hashlib.sha256(data).hexdigest(), "bytes": len(data),
                         "origin": "source-built-ifcopenshell"} for name, data in sorted(entries.items())],
        }
        return SimpleNamespace(verify_selected_stage=mock.Mock(return_value=verified)), verified

    def test_controlled_selection_replaces_wheel_and_keeps_payload_provenance(self):
        helper, verified = self.controlled_fixture()
        with mock.patch.object(bootstrap, "bootstrap", return_value={}), mock.patch.dict(
                sys.modules, {"controlled_cad_distribution": helper}):
            payload = prepare_distribution.prepare(self.root, self.output,
                runtime_root=Path(".deps/controlled-sdk"), staged_root=Path("build/windows-release/cad-runtime"))
        component = next(row for row in json.loads((self.output / "cad-components.json").read_text())["components"]
                         if row["id"] == "cad-ifcopenshell")
        self.assertEqual(component["source"]["kind"], "controlled-runtime")
        self.assertEqual(component["source"]["corresponding_source_paths"], [])
        self.assertNotIn("archive_filename", component["source"])
        self.assertEqual(component["package"]["version"], "source-" + "a" * 40)
        self.assertTrue(any(row["component"] == "cad-ifcopenshell" and row["path"].endswith("/receipts/build.json")
                            for row in payload["files"]))
        self.assertFalse(any(row["path"].endswith("/old.py") for row in payload["files"]))
        self.assertEqual(payload["selected_runtime"], verified["identity"])
        self.assertFalse(payload["source_closure_qualified"])
        self.assertEqual(helper.verify_selected_stage.call_count, 2)

    def test_controlled_identity_drift_is_refused_before_output_publication(self):
        helper, verified = self.controlled_fixture()
        helper.verify_selected_stage.side_effect = [verified, {**verified, "identity": {"kind": "changed"}}]
        with mock.patch.object(bootstrap, "bootstrap", return_value={}), mock.patch.dict(
                sys.modules, {"controlled_cad_distribution": helper}), self.assertRaisesRegex(ValueError, "changed during"):
            prepare_distribution.prepare(self.root, self.output, runtime_root=Path(".deps/controlled-sdk"))
        self.assertFalse(self.output.exists())

    def test_source_inputs_require_explicit_controlled_selection(self):
        with mock.patch.object(bootstrap, "bootstrap", return_value={}), self.assertRaisesRegex(ValueError, "explicit runtime"):
            prepare_distribution.prepare(self.root, self.output, corresponding_source_paths=[])
        self.assertFalse(self.output.exists())

    def test_malformed_falsy_source_inputs_are_rejected(self):
        helper, _ = self.controlled_fixture()
        for malformed in ({}, False, 0, ""):
            with self.subTest(value=malformed), mock.patch.object(bootstrap, "bootstrap", return_value={}), mock.patch.dict(
                    sys.modules, {"controlled_cad_distribution": helper}), self.assertRaises((ValueError, inventory.InventoryError)):
                prepare_distribution.prepare(self.root, self.output, runtime_root=Path(".deps/controlled-sdk"),
                                             corresponding_source_paths=malformed)
            self.assertFalse(self.output.exists())

    def test_output_inside_selected_input_tree_is_refused(self):
        helper, _ = self.controlled_fixture()
        original = {path: path.read_bytes() for path in self.stage.rglob("*") if path.is_file()}
        for output in (self.stage / "manifests", self.root / ".deps/controlled-sdk/manifests"):
            with self.subTest(output=output), mock.patch.object(bootstrap, "bootstrap", return_value={}), mock.patch.dict(
                    sys.modules, {"controlled_cad_distribution": helper}), self.assertRaisesRegex(ValueError, "overlap"):
                prepare_distribution.prepare(self.root, output, runtime_root=Path(".deps/controlled-sdk"))
            self.assertFalse(output.exists())
            self.assertTrue(all(path.read_bytes() == data for path, data in original.items()))

    def test_output_cannot_replace_compiled_selection(self):
        helper, verified = self.controlled_fixture()
        selection = self.output / "cad-components.json"
        _write_json(selection, verified["identity"])
        original = selection.read_bytes()
        verified["selection_record"] = {"path": selection.relative_to(self.root).as_posix(),
                                        "bytes": len(original), "sha256": hashlib.sha256(original).hexdigest()}
        with mock.patch.object(bootstrap, "bootstrap", return_value={}), mock.patch.dict(
                sys.modules, {"controlled_cad_distribution": helper}), self.assertRaisesRegex(ValueError, "overlap"):
            prepare_distribution.prepare(self.root, self.output, runtime_root=Path(".deps/controlled-sdk"),
                                         selection_path=selection)
        self.assertEqual(selection.read_bytes(), original)
        self.assertFalse((self.output / "cad-allowlist.json").exists())

    def test_output_cannot_replace_declared_source_file_or_enter_source_tree(self):
        helper, _ = self.controlled_fixture()
        source = self.output / "cad-payload.json"
        source.parent.mkdir(parents=True)
        source.write_bytes(b"preferred source must survive")
        for row in ({"path": source.relative_to(self.root).as_posix(), "kind": "file", "sha256": hashlib.sha256(source.read_bytes()).hexdigest()},
                    {"path": self.output.relative_to(self.root).as_posix(), "kind": "directory", "sha256": "a" * 64}):
            with self.subTest(kind=row["kind"]), mock.patch.object(bootstrap, "bootstrap", return_value={}), mock.patch.dict(
                    sys.modules, {"controlled_cad_distribution": helper}), self.assertRaisesRegex(ValueError, "overlap"):
                prepare_distribution.prepare(self.root, self.output, runtime_root=Path(".deps/controlled-sdk"),
                                             corresponding_source_paths=[row])
            self.assertEqual(source.read_bytes(), b"preferred source must survive")
            self.assertFalse((self.output / "cad-components.json").exists())


if __name__ == "__main__":
    unittest.main()
