"""Inert fixtures verify preferred source copying and refusal boundaries."""
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import tempfile
import unittest
from unittest import mock

ROOT = Path(__file__).resolve().parents[1]
SPEC = importlib.util.spec_from_file_location("source_materializer",
    ROOT / "scripts/qualification/materialize_ifc_corresponding_source.py")
materializer = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(materializer)


def record(name, data):
    return {"path": name, "bytes": len(data), "sha256": hashlib.sha256(data).hexdigest()}


class MaterializationTests(unittest.TestCase):
    def test_tree_identity_is_unambiguous_and_inventory_compatible(self):
        first, second = self.root / "first", self.root / "second"
        first.mkdir()
        second.mkdir()
        expected = {row["path"]: row for row in (
            self.write(first, "a.hpp", b"A"), self.write(first, "b.hpp", b"B"))}
        other = self.write(second, "a.hpp", b"A\0b.hpp\0B")
        self.assertNotEqual(materializer.tree_hash(first, expected),
                            materializer.tree_hash(second, {other["path"]: other}))
        framed = hashlib.sha256(b"Vertex-source-tree-v2\0" + (2).to_bytes(8, "big"))
        for name, value in ((b"a.hpp", b"A"), (b"b.hpp", b"B")):
            framed.update(len(name).to_bytes(8, "big") + name + len(value).to_bytes(8, "big") + hashlib.sha256(value).digest())
        self.assertEqual(materializer.tree_hash(first, expected), framed.hexdigest())

    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name) / "workspace"
        self.runtime = self.root / ".deps/runtime"
        self.derived = Path(self.temp.name) / "derived"
        self.kit = Path(self.temp.name) / "kit"
        self.boost = Path(self.temp.name) / "boost"
        self.sdk = Path(self.temp.name) / "sdk"
        self.out = self.root / materializer.OUTPUT_RELATIVE
        for path in (self.runtime, self.derived, self.kit, self.boost, self.sdk, self.out.parent):
            path.mkdir(parents=True, exist_ok=True)
        self.source = {"path": ".deps/ifc-src", "repository": materializer.safe.SOURCE_REPOSITORY,
                       "revision": materializer.safe.SOURCE_REVISION}
        self.modules = [{"path": p, "repository": u, "revision": r}
                        for p, u, r in materializer.safe.SUBMODULES]
        old = record("src/ifcwrap/utils/typemaps_out.i", b"old")
        new = self.write(self.derived, old["path"], b"old+patch")
        other = self.write(self.derived, "COPYING", b"license")
        derived_files = sorted([new, other], key=lambda r: r["path"])
        original_files = sorted([old, other], key=lambda r: r["path"])
        kit_files = [self.write(self.kit, "recipes/thread/portfile.cmake", b"thread recipe"),
                     self.write(self.kit, "recipes/helper/boost-install.cmake", b"helper recipe")]
        self.kit_manifest = {"schema_version": 1, "files": sorted(kit_files, key=lambda r: r["path"])}
        self.kit_record = self.write_json(self.kit, materializer.SDK_MANIFEST, self.kit_manifest)
        helper = self.write(self.root, "scripts/rebuild_ifc_boost_thread.py", b"# inert helper recipe\n")
        self.library = self.write(self.boost, materializer.BOOST_LIBRARY, b"static-library")
        self.write(self.sdk, materializer.BOOST_SDK_LIBRARY, b"static-library")
        controls = self.write(self.sdk, "share/boost/cmake-build/BoostRoot.cmake", b"# build controls")
        boost_files = [self.write(self.boost / "source", "CMakeLists.txt", b"# generic build root\n")]
        for name in materializer.BOOST_REQUIRED_SOURCES:
            boost_files.append(self.write(self.boost / "source", "libs/thread/" + name, name.encode()))
        inputs = sorted([self.kit_record, *kit_files], key=lambda r: r["path"])
        for r in inputs:
            self.write(self.boost / "evidence/inputs", r["path"], (self.kit / r["path"]).read_bytes())
        self.rebuild = {"schema_version": 1, "status": "built", "build_qualified": False,
            "source_closure_qualified": False, "offline_sdk_rebuild_qualified": False,
            "product_runtime_replaced": False, "redistribution_qualified": False,
            "source_kit": str(self.kit), "support_sdk": str(self.sdk), "output_root": str(self.boost),
            "expected_library": materializer.BOOST_LIBRARY, "library": self.library,
            "tools": {"script": {"path": str(self.root / helper["path"]), **materializer.safe.checked_record(helper)}},
            "source_files": sorted(boost_files, key=lambda r: r["path"]),
            "sdk_files": sorted([controls, self.library | {"path": materializer.BOOST_SDK_LIBRARY}], key=lambda r: r["path"]),
            "source_kit_inputs": inputs, "evidence_inputs": inputs,
            "command_results": [{"exit_code": 0, "error": None}] * 4}
        self.save_rebuild()
        modified = {"path": old["path"], "original_bytes": old["bytes"], "original_sha256": old["sha256"],
                    "derived_bytes": new["bytes"], "derived_sha256": new["sha256"]}
        self.portable = {
            "receipts/source-derivation.json": {"source": self.source, "submodules": self.modules,
                "modified_file": modified, "derived_source_files": derived_files},
            "receipts/source-preparation.json.receipt.json": {"source": self.source,
                "submodules": self.modules, "source_files": original_files},
            "receipts/build.json": {"inputs": [{"root": "support_sdk", "path": materializer.BOOST_SDK_LIBRARY,
                "role": "boost-static-library", **materializer.safe.checked_record(self.library)}]}}
        runtime_files = [self.write_json(self.runtime, name, value) for name, value in self.portable.items()]
        runtime_files.append(self.write(self.runtime, "receipts/recipes/build-ifc-source.ps1", b"# inert IFC recipe"))
        self.runtime_manifest = {"kind": "controlled_cad_runtime", "files": sorted(
            [r | {"origin": "fixture"} for r in runtime_files], key=lambda r: r["path"])}
        self.runtime_record = self.write_json(self.runtime, materializer.safe.MANIFEST, self.runtime_manifest)
        self.identity = {"kind": "controlled", "manifest_name": materializer.safe.MANIFEST,
            "manifest_sha256": self.runtime_record["sha256"], "source_revision": materializer.safe.SOURCE_REVISION}
        for name, value in (("SDK_MANIFEST_SHA256", self.kit_record["sha256"]),
                            ("BOOST_LIBRARY_SHA256", self.library["sha256"])):
            patcher = mock.patch.object(materializer, name, value)
            patcher.start()
            self.addCleanup(patcher.stop)
        patcher = mock.patch.object(materializer.inspector, "inspect", return_value=self.identity)
        self.inspect = patcher.start()
        self.addCleanup(patcher.stop)

    def write(self, root, name, data):
        path = root / name
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(data)
        return record(name, data)

    def write_json(self, root, name, value):
        return self.write(root, name, materializer.safe.json_bytes(value))

    def save_rebuild(self):
        self.write_json(self.boost, "evidence/rebuild.json", self.rebuild)

    def run_materializer(self):
        return materializer.materialize(self.root, self.runtime, self.derived, self.kit, self.boost, self.out)

    @unittest.skipUnless(os.name == "nt", "Windows no-replace publication")
    def test_actual_sources_portable_manifest_and_flags(self):
        result = self.run_materializer()
        self.assertEqual((self.out / "derived-ifc/COPYING").read_bytes(), b"license")
        self.assertTrue((self.out / "boost-thread/libs/thread/src/future.cpp").is_file())
        self.assertTrue((self.out / "build-controls/rebuild_ifc_boost_thread.py").is_file())
        manifest = json.loads((self.out / materializer.SOURCE_PATHS).read_text())
        self.assertEqual(len(manifest), 5)
        for row in manifest:
            path = self.root / row["path"]
            if row["kind"] == "file":
                self.assertEqual(materializer.safe.digest(path)["sha256"], row["sha256"])
            else:
                self.assertEqual(materializer.tree_hash(path, materializer.table(result["trees"][path.name]["files"])), row["sha256"])
        for name in materializer.FLAGS:
            self.assertIs(result[name], False)
        text = (self.out / materializer.INDEX).read_text()
        self.assertNotIn(str(self.temp.name), text)
        self.assertNotIn("C:\\", text)
        self.assertTrue((self.boost / "evidence/rebuild.json").exists())

    def test_refuses_extra_source(self):
        self.write(self.derived, "extra", b"unreviewed")
        with self.assertRaisesRegex(ValueError, "unreviewed"):
            self.run_materializer()

    def test_refuses_baseline_runtime(self):
        self.inspect.return_value = self.identity | {"kind": "baseline"}
        with self.assertRaisesRegex(ValueError, "controlled runtime"):
            self.run_materializer()

    def test_refuses_changed_source_kit_manifest(self):
        self.kit_manifest["schema_version"] = 2
        self.write_json(self.kit, materializer.SDK_MANIFEST, self.kit_manifest)
        with self.assertRaisesRegex(ValueError, "source-kit manifest"):
            self.run_materializer()

    def test_refuses_private_root_mismatch(self):
        self.rebuild["source_kit"] = str(self.derived)
        self.save_rebuild()
        with self.assertRaisesRegex(ValueError, "roots differ"):
            self.run_materializer()

    def test_refuses_selected_build_library_mismatch(self):
        self.portable["receipts/build.json"]["inputs"][0]["sha256"] = "0" * 64
        changed = self.write_json(self.runtime, "receipts/build.json", self.portable["receipts/build.json"])
        for row in self.runtime_manifest["files"]:
            if row["path"] == changed["path"]:
                row.update(changed)
        self.runtime_record = self.write_json(self.runtime, materializer.safe.MANIFEST, self.runtime_manifest)
        self.identity["manifest_sha256"] = self.runtime_record["sha256"]
        with self.assertRaisesRegex(ValueError, "selected IFC build input"):
            self.run_materializer()

    def test_refuses_library_mismatch(self):
        self.rebuild["library"]["sha256"] = "0" * 64
        self.save_rebuild()
        with self.assertRaisesRegex(ValueError, "library"):
            self.run_materializer()

    def test_refuses_failed_build_evidence(self):
        self.rebuild["command_results"][0] = {"exit_code": 1, "error": None}
        self.save_rebuild()
        with self.assertRaisesRegex(ValueError, "command evidence"):
            self.run_materializer()

    def test_refuses_qualified_receipt(self):
        self.rebuild["source_closure_qualified"] = True
        self.save_rebuild()
        with self.assertRaises(ValueError):
            self.run_materializer()

    def test_refuses_helper_drift(self):
        (self.root / "scripts/rebuild_ifc_boost_thread.py").write_bytes(b"changed")
        with self.assertRaises(ValueError):
            self.run_materializer()

    def test_refuses_kit_receipt_drift(self):
        (self.kit / "recipes/helper/boost-install.cmake").write_bytes(b"changed")
        with self.assertRaises(ValueError):
            self.run_materializer()

    def test_refuses_preexisting_output(self):
        self.out.mkdir()
        self.write(self.out, "preserved", b"old")
        with self.assertRaises(ValueError):
            self.run_materializer()
        self.assertEqual((self.out / "preserved").read_bytes(), b"old")

    def test_refuses_source_tree_over_distribution_limit(self):
        with mock.patch.object(materializer, "MAX_TREE_FILES", 1):
            with self.assertRaisesRegex(ValueError, "tree.*bound"):
                self.run_materializer()

    def test_rejects_case_collision_traversal_and_bad_byte_receipt(self):
        for rows in ([record("a", b"a"), record("A", b"a")],
                     [record("../a", b"a")], [record("a", b"a") | {"bytes": True}]):
            with self.subTest(rows=rows), self.assertRaises(ValueError):
                materializer.table(rows)

    @unittest.skipUnless(os.name == "nt", "Windows no-replace publication")
    def test_refuses_source_drift_during_copy(self):
        real_copy = materializer.safe.copy_verified
        changed = False
        def copy_and_change(source, destination, evidence):
            nonlocal changed
            real_copy(source, destination, evidence)
            if source == self.derived / "COPYING" and not changed:
                changed = True
                source.write_bytes(b"changed after copy")
        with mock.patch.object(materializer.safe, "copy_verified", side_effect=copy_and_change):
            with self.assertRaises(ValueError):
                self.run_materializer()
        self.assertFalse(self.out.exists())

    @unittest.skipUnless(os.name == "nt", "Windows no-replace publication")
    def test_refuses_added_cmake_control_after_copy(self):
        real_copy = materializer.safe.copy_verified
        changed = False
        def copy_and_change(source, destination, evidence):
            nonlocal changed
            real_copy(source, destination, evidence)
            if source.name == "BoostRoot.cmake" and not changed:
                changed = True
                self.write(self.sdk, "share/boost/cmake-build/extra.cmake", b"extra")
        with mock.patch.object(materializer.safe, "copy_verified", side_effect=copy_and_change):
            with self.assertRaises(ValueError):
                self.run_materializer()
        self.assertFalse(self.out.exists())

    @unittest.skipUnless(os.name == "nt", "Windows no-replace publication")
    def test_publication_race_preserves_foreign_output(self):
        real_rename = materializer.os.rename
        def race(source, destination):
            self.out.mkdir()
            self.write(self.out, "foreign", b"preserved")
            return real_rename(source, destination)
        with mock.patch.object(materializer.os, "rename", side_effect=race):
            with self.assertRaises(OSError):
                self.run_materializer()
        self.assertEqual((self.out / "foreign").read_bytes(), b"preserved")

    @unittest.skipUnless(os.name == "nt", "Windows no-replace publication")
    def test_tree_hash_matches_distribution_inventory_encoding(self):
        result = self.run_materializer()
        for name, tree in result["trees"].items():
            expected = hashlib.sha256(b"Vertex-source-tree-v2\0")
            expected.update(len(tree["files"]).to_bytes(8, "big"))
            for row in tree["files"]:
                path = row["path"].encode("utf-8")
                content = (self.out / name / row["path"]).read_bytes()
                expected.update(len(path).to_bytes(8, "big") + path + len(content).to_bytes(8, "big")
                                + hashlib.sha256(content).digest())
            self.assertEqual(tree["sha256"], expected.hexdigest())

    @unittest.skipUnless(os.name == "nt", "Windows no-replace publication")
    def test_failure_preserves_private_partial_and_original(self):
        real_copy = materializer.safe.copy_verified
        def fail(source, destination, evidence):
            real_copy(source, destination, evidence)
            raise OSError("synthetic interrupted copy")
        with mock.patch.object(materializer.safe, "copy_verified", side_effect=fail):
            with self.assertRaises(OSError):
                self.run_materializer()
        self.assertFalse(self.out.exists())
        stages = list(self.out.parent.glob(materializer.STAGE_PREFIX + "*"))
        self.assertEqual(len(stages), 1)
        self.assertTrue(any(p.is_file() for p in stages[0].rglob("*")))
        self.assertEqual((self.derived / "COPYING").read_bytes(), b"license")

    @unittest.skipUnless(os.name == "nt", "Windows hardlink fixture")
    def test_refuses_hardlink(self):
        target = self.derived / "COPYING"
        target.unlink()
        original = Path(self.temp.name) / "original"
        original.write_bytes(b"license")
        os.link(original, target)
        with self.assertRaises(ValueError):
            self.run_materializer()


if __name__ == "__main__":
    unittest.main()
