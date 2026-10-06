"""Inert Windows fixtures: real filesystem checks, no CAD imports or builds.

The SDK inspector is mocked at its qualification boundary. Root must separately
exercise an actual controlled stage after the clean native extension build.
"""
import copy
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import tempfile
import unittest
from unittest import mock

SCRIPT = Path(__file__).resolve().parents[1] / "scripts/controlled_cad_distribution.py"


def receipt(data):
    return {"bytes": len(data), "sha256": hashlib.sha256(data).hexdigest()}


@unittest.skipUnless(os.name == "nt", "Retained stage verification requires Windows handles")
class ControlledDistributionTests(unittest.TestCase):
    def setUp(self):
        self.assertTrue(SCRIPT.is_file(), "Shared controlled distribution verifier is missing")
        spec = importlib.util.spec_from_file_location("controlled_distribution", SCRIPT)
        self.v = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(self.v)
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.workspace = Path(self.temp.name) / "workspace"
        self.sdk = self.workspace / ".deps/sdk"
        self.stage = self.workspace / "build/cad-runtime"
        self.selection = self.workspace / "build/cad-runtime-selection.json"
        self.adapter = self.workspace / "src/desktop/cad_library_adapter.py"
        self.native = "Lib/site-packages/ifcopenshell/_ifcopenshell_wrapper.cp313-win_amd64.pyd"
        self.wrapper = "Lib/site-packages/ifcopenshell/ifcopenshell_wrapper.py"
        self.manifest_name = "controlled-runtime-manifest.json"
        self.data = {
            self.native: b"inert native fixture", self.wrapper: b"inert wrapper fixture",
            "Lib/site-packages/ifcopenshell/__init__.py": b"inert package",
            "Lib/site-packages/ifcopenshell/express/recipe.exp": b"inert recipe",
            "notices/ifcopenshell/COPYING": b"inert notice",
            "receipts/build.json": b'{"qualification_conferred":false}',
            "include/Python.h": b"inert header", "libs/python313.lib": b"inert dev lib",
            "python.exe": b"inert interpreter", "Lib/site-packages/ezdxf/__init__.py": b"inert unrelated wheel"}
        self.origins = {n: "portable-derived-receipt" if n.startswith("receipts/") else
                        "source-built-ifcopenshell" if "ifcopenshell" in n else "verified-baseline"
                        for n in self.data}
        self.source = {"kind": "controlled_cad_runtime", "schema_version": 1,
            "ifcopenshell": {"source": {"revision": "f" * 40}},
            "qualification": "incomplete", **{f: False for f in self.v.receipts.RESULT_FLAGS}}
        self.write_fixture()
        self.identity = {"kind": "controlled", "manifest_name": self.manifest_name,
            "manifest_sha256": receipt(self.manifest_data)["sha256"],
            "ifc_extension_sha256": receipt(self.data[self.native])["sha256"],
            "ifc_wrapper_sha256": receipt(self.data[self.wrapper])["sha256"],
            "python_version": "3.13.15", "ezdxf_version": "1.4.3",
            "qualification": "incomplete", "source_revision": "f" * 40}
        self.put(self.selection, json.dumps(self.identity).encode())
        patch = mock.patch.object(self.v.inspector, "inspect", side_effect=self.inspect_fixture)
        self.inspect_mock = patch.start()
        self.addCleanup(patch.stop)

    def put(self, path, data):
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(data)

    def write_fixture(self):
        self.source["files"] = [{"path": n, **receipt(d), "origin": self.origins[n]}
                                 for n, d in sorted(self.data.items())]
        self.manifest_data = json.dumps(self.source).encode()
        for root in (self.sdk, self.stage):
            for n, d in self.data.items():
                self.put(root / n, d)
            self.put(root / self.manifest_name, self.manifest_data)
        self.put(self.adapter, b"# inert current adapter\n")
        self.put(self.stage / "cad_library_adapter.py", self.adapter.read_bytes())
        self.rewrite_marker()

    def rewrite_marker(self):
        files = [{"path": p.relative_to(self.stage).as_posix(), **receipt(p.read_bytes())}
                 for p in self.stage.rglob("*") if p.is_file() and p.name != "cad-stage-manifest.json"]
        marker = {"schema_version": 1, "kind": "vertex_cad_runtime_stage", "stage_id": "a" * 32,
            "source": {"manifest": self.manifest_name, **receipt(self.manifest_data)},
            "adapter": {"path": "cad_library_adapter.py", **receipt(self.adapter.read_bytes())},
            "files": sorted(files, key=lambda r: r["path"]), "qualification": "incomplete",
            "runtime_qualified": False, "production_worker_integrated": False}
        self.put(self.stage / "cad-stage-manifest.json", json.dumps(marker).encode())

    def inspect_fixture(self, runtime_root, workspace):
        self.v.receipts.verify_tree(Path(runtime_root),
            {n: {"path": n, **receipt(d)} for n, d in self.data.items()},
            auxiliary={self.manifest_name: receipt(self.manifest_data)})
        return copy.deepcopy(self.identity)

    def verify(self, **kwargs):
        return self.v.verify_selected_stage(kwargs.get("workspace", self.workspace),
            kwargs.get("runtime_root", self.sdk), kwargs.get("staged_root", self.stage),
            kwargs.get("selection_path", self.selection))

    def test_good_returns_only_exact_stage_ifc_payload_and_portable_provenance(self):
        result = self.verify()
        expected = {self.native, self.wrapper, "Lib/site-packages/ifcopenshell/__init__.py",
            "Lib/site-packages/ifcopenshell/express/recipe.exp", "notices/ifcopenshell/COPYING",
            "receipts/build.json", self.manifest_name}
        self.assertEqual({r["path"] for r in result["payload"]}, expected)
        for item in result["payload"]:
            self.assertEqual({k: item[k] for k in ("bytes", "sha256")}, receipt((self.stage / item["path"]).read_bytes()))
        self.assertEqual(result["sdk_root"], ".deps/sdk")
        self.assertEqual(result["staged_root"], "build/cad-runtime")
        self.assertEqual(result["identity"], self.identity)
        self.assertEqual(result["selection_record"], {"path": "build/cad-runtime-selection.json", **receipt(self.selection.read_bytes())})
        self.assertFalse(result["source_closure_qualified"])
        self.assertFalse(result["corresponding_source_qualified"])
        self.assertTrue(result["provenance_only"])
        self.assertNotIn(str(self.workspace), json.dumps(result))

    def test_compiled_selection_mismatch_rejected(self):
        for key in ("manifest_sha256", "ifc_extension_sha256", "ifc_wrapper_sha256", "source_revision"):
            with self.subTest(key=key):
                changed = {**self.identity, key: "0" * len(self.identity[key])}
                self.put(self.selection, json.dumps(changed).encode())
                with self.assertRaises(ValueError):
                    self.verify()

    def test_changed_manifest_native_wrapper_or_adapter_rejected(self):
        for path in (self.sdk / self.manifest_name, self.stage / self.manifest_name,
                     self.stage / self.native, self.stage / self.wrapper,
                     self.adapter, self.stage / "cad_library_adapter.py"):
            with self.subTest(path=path):
                original = path.read_bytes()
                path.write_bytes(original + b"changed")
                with self.assertRaises(ValueError):
                    self.verify()
                path.write_bytes(original)

    def test_owned_but_foreign_extra_or_stale_distinfo_rejected(self):
        for name in ("foreign.txt", "Lib/site-packages/ifcopenshell-0.8.dist-info/METADATA"):
            with self.subTest(name=name):
                extra = self.stage / name
                self.put(extra, b"foreign")
                self.rewrite_marker()
                with self.assertRaises(ValueError):
                    self.verify()
                extra.unlink()
                if extra.parent.name.endswith("dist-info"):
                    extra.parent.rmdir()
                self.rewrite_marker()

    def test_unowned_foreign_file_and_empty_directory_rejected(self):
        for name, directory in (("foreign.txt", False), ("empty", True)):
            target = self.stage / name
            target.mkdir() if directory else self.put(target, b"foreign")
            with self.assertRaises(ValueError):
                self.verify()
            target.rmdir() if directory else target.unlink()

    def test_case_collision_in_owned_table_rejected(self):
        path = self.stage / "cad-stage-manifest.json"
        marker = json.loads(path.read_bytes())
        marker["files"].append({"path": "lib/other.txt", **receipt(b"foreign")})
        marker["files"].sort(key=lambda r: r["path"])
        self.put(path, json.dumps(marker).encode())
        with self.assertRaises(ValueError):
            self.verify()

    def test_file_directory_collision_in_owned_table_rejected(self):
        path = self.stage / "cad-stage-manifest.json"
        marker = json.loads(path.read_bytes())
        marker["files"].append({"path": self.wrapper + "/foreign", **receipt(b"foreign")})
        marker["files"].sort(key=lambda r: r["path"])
        self.put(path, json.dumps(marker).encode())
        with self.assertRaises(ValueError):
            self.verify()

    def test_hardlinked_stage_selection_sdk_and_adapter_rejected(self):
        for path in (self.stage / self.native, self.selection, self.sdk / self.wrapper, self.adapter):
            with self.subTest(path=path):
                alias = self.workspace / "hardlink"
                os.link(path, alias)
                with self.assertRaises(ValueError):
                    self.verify()
                alias.unlink()

    def test_stage_symlink_rejected_when_available(self):
        target = self.stage / self.wrapper
        data = target.read_bytes()
        target.unlink()
        try:
            target.symlink_to(self.sdk / self.wrapper)
        except OSError as error:
            self.put(target, data)
            self.skipTest("Windows symlink creation unavailable: " + str(error))
        with self.assertRaises(ValueError):
            self.verify()

    def test_repository_children_and_disjoint_roots_required(self):
        for kwargs in ({"runtime_root": self.workspace.parent}, {"staged_root": self.sdk},
                       {"staged_root": self.sdk / "nested"}, {"selection_path": self.workspace.parent / "outside.json"},
                       {"runtime_root": ".deps/../sdk"}, {"runtime_root": "//server/share"},
                       {"selection_path": self.stage}):
            with self.subTest(kwargs=kwargs), self.assertRaises((ValueError, OSError)):
                self.verify(**kwargs)

    def test_baseline_identity_rejected_even_if_compiled_selection_matches(self):
        self.identity["kind"] = "baseline"
        self.identity.pop("source_revision")
        self.put(self.selection, json.dumps(self.identity).encode())
        with self.assertRaises(ValueError):
            self.verify()

    def test_duplicate_nonfinite_and_oversized_selection_rejected(self):
        for data in (b'{"kind":"controlled","kind":"controlled"}', b'{"number":NaN}',
                     b" " * (self.v.receipts.MAX_JSON_BYTES + 1)):
            self.put(self.selection, data)
            with self.assertRaises(ValueError):
                self.verify()

    def test_inspector_change_during_final_recheck_rejected(self):
        def inspect(root, workspace):
            value = self.inspect_fixture(root, workspace)
            if self.inspect_mock.call_count == 2:
                value["source_revision"] = "e" * 40
            return value
        self.inspect_mock.side_effect = inspect
        with self.assertRaises(ValueError):
            self.verify()

    def test_sdk_write_during_verification_is_blocked_or_rejected(self):
        def inspect(root, workspace):
            value = self.inspect_fixture(root, workspace)
            if self.inspect_mock.call_count == 2:
                (self.sdk / self.wrapper).write_bytes(b"changed during verification")
            return value
        self.inspect_mock.side_effect = inspect
        with self.assertRaises((ValueError, OSError)):
            self.verify()

    def test_adapter_write_during_verification_is_blocked_or_rejected(self):
        def inspect(root, workspace):
            value = self.inspect_fixture(root, workspace)
            if self.inspect_mock.call_count == 2:
                self.adapter.write_bytes(b"changed during verification")
            return value
        self.inspect_mock.side_effect = inspect
        with self.assertRaises((ValueError, OSError)):
            self.verify()

    def test_same_byte_file_identity_substitution_is_rejected(self):
        for root in (self.sdk, self.stage):
            with self.subTest(root=root):
                self.inspect_mock.reset_mock()
                target = root / self.wrapper
                data = target.read_bytes()
                moved = self.workspace / "retained-original"
                def inspect(runtime, workspace):
                    value = self.inspect_fixture(runtime, workspace)
                    if self.inspect_mock.call_count == 2:
                        target.rename(moved)
                        self.put(target, data)
                    return value
                self.inspect_mock.side_effect = inspect
                with self.assertRaises((ValueError, OSError)):
                    self.verify()
                if moved.exists():
                    self.assertEqual(moved.read_bytes(), data)
                    moved.unlink()
                self.inspect_mock.side_effect = self.inspect_fixture

    def test_explicit_unqualified_source_flags_required(self):
        for value in (True, None, "false"):
            self.source["source_closure_qualified"] = value
            self.write_fixture()
            self.identity["manifest_sha256"] = receipt(self.manifest_data)["sha256"]
            self.put(self.selection, json.dumps(self.identity).encode())
            with self.assertRaises(ValueError):
                self.verify()

    def test_selected_source_revision_must_bind_the_manifest_revision(self):
        self.source["ifcopenshell"]["source"]["revision"] = "e" * 40
        self.write_fixture()
        self.identity["manifest_sha256"] = receipt(self.manifest_data)["sha256"]
        self.put(self.selection, json.dumps(self.identity).encode())
        with self.assertRaises(ValueError):
            self.verify()


if __name__ == "__main__":
    unittest.main()
