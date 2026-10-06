"""Explicit native evidence resolution and bounded inert archive replay."""
import hashlib
import importlib.util
import io
import json
import os
from pathlib import Path
import stat
import sys
import tarfile
import tempfile
import unittest
from unittest import mock
import zipfile

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "scripts/qualification"))
import native_source_inputs as native


class ReaderTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.calls = []
        def resolve(row, role):
            self.calls.append((dict(row), role))
            return self.root / row["path"]
        self.reader = native.Reader(resolve)

    def write(self, name, data):
        path = self.root / name
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(data)
        return {"path": name, "sha256": hashlib.sha256(data).hexdigest(), "bytes": len(data)}

    def zip(self, members):
        buffer = io.BytesIO()
        with zipfile.ZipFile(buffer, "w") as archive:
            for name, content in members:
                archive.writestr(name, content)
        return self.write("source.zip", buffer.getvalue())

    def test_explicit_frozen_resolver_uses_original_identity_without_workspace(self):
        row = self.write("delivered/only.json", b'{"value":42}')
        original = {**row, "path": "missing-original/metadata.json"}
        frozen = native.Reader(lambda receipt, role: self.root / "delivered/only.json")
        self.assertEqual(frozen.json(original, "metadata"), {"value": 42})
        self.assertEqual(frozen.record(original, "metadata"), original)
        frozen.verify()
        self.assertFalse((self.root / original["path"]).exists())

    def test_hash_size_hardlinks_and_unsafe_receipts_refused(self):
        row = self.write("ordinary.json", b'{}')
        for bad in ({**row, "sha256": "0" * 64}, {**row, "bytes": 3},
                    {**row, "path": "../ordinary.json"}, {**row, "bytes": True}):
            with self.subTest(bad=bad), self.assertRaises(ValueError):
                self.reader.record(bad, "metadata")
        os.link(self.root / row["path"], self.root / "hardlink.json")
        with self.assertRaisesRegex(ValueError, "ordinary"):
            self.reader.record(row, "metadata")

    def test_duplicate_json_and_unknown_roles_refused(self):
        row = self.write("metadata.json", b'{"x":1,"x":2}')
        with self.assertRaisesRegex(ValueError, "Duplicate JSON"):
            self.reader.json(row, "metadata")
        with self.assertRaisesRegex(ValueError, "role"):
            self.reader.record(row, "not-real")

    def test_selected_archive_bytes_exact_missing_and_return_bounds(self):
        row = self.zip([("root/license.txt", b"original\r\n"), ("root/code.c", b"code")])
        self.assertEqual(self.reader.archive_members(row, ["root/license.txt"], "notice"),
                         {"root/license.txt": b"original\r\n"})
        with self.assertRaisesRegex(ValueError, "missing requested"):
            self.reader.archive_members(row, ["root/missing"], "source")
        with mock.patch.object(native, "MAX_RETURNED", 4), self.assertRaisesRegex(ValueError, "returned"):
            self.reader.archive_members(row, ["root/license.txt"], "source")
        with mock.patch.object(native, "MAX_ARCHIVE_ENTRIES", 1), self.assertRaisesRegex(ValueError, "entry count"):
            self.reader.archive_members(row, ["root/code.c"], "source")

    def test_implicit_directory_case_collision_and_traversal_refused(self):
        for members in ([ ("Root/a", b"a"), ("root/b", b"b") ],
                        [ ("../bad", b"a") ],
                        [ ("root/file", b"a"), ("root/file/a", b"b") ]):
            with self.subTest(members=members):
                row = self.zip(members)
                reader = native.Reader(lambda value, role: self.root / value["path"])
                with self.assertRaises(ValueError):
                    reader.archive_members(row, [members[-1][0]], "source")

    def test_requested_tar_link_and_nonregular_zip_refused(self):
        buffer = io.BytesIO()
        with tarfile.open(fileobj=buffer, mode="w:gz") as archive:
            link = tarfile.TarInfo("root/link")
            link.type = tarfile.SYMTYPE
            link.linkname = "other"
            archive.addfile(link)
        row = self.write("source.tar.gz", buffer.getvalue())
        with self.assertRaisesRegex(ValueError, "ordinary file"):
            self.reader.archive_members(row, ["root/link"], "source")
        for mode in (stat.S_IFLNK, stat.S_IFIFO, stat.S_IFCHR, stat.S_IFDIR):
            with self.subTest(mode=mode):
                buffer = io.BytesIO()
                with zipfile.ZipFile(buffer, "w") as archive:
                    info = zipfile.ZipInfo("root/nonregular")
                    info.create_system = 3
                    info.external_attr = (mode | 0o600) << 16
                    archive.writestr(info, b"data")
                row = self.write("nonregular.zip", buffer.getvalue())
                reader = native.Reader(lambda value, role: self.root / value["path"])
                with self.assertRaisesRegex(ValueError, "ordinary file"):
                    reader.archive_members(row, ["root/nonregular"], "source")

    def test_changed_input_refused_and_final_check_preserves_role(self):
        row = self.write("source.bin", b"first")
        self.reader.record(row, "source")
        self.reader.verify()
        self.assertTrue(all(role == "source" for _, role in self.calls))
        (self.root / row["path"]).write_bytes(b"other")
        with self.assertRaisesRegex(ValueError, "hash/bytes"):
            self.reader.verify()

    def test_target_binding_uses_inventory_destination_and_exact_binary_tuple(self):
        row = {"component_id": "cad-numpy", "path": "runtime/numpy.dll", "sha256": "a" * 64, "bytes": 17}
        inventory = {"binaries": [{**row, "destination": "bin/cad/numpy.dll"}]}
        self.assertEqual(native.targets(inventory, [row])["cad-numpy"], [{**row, "destination": "bin/cad/numpy.dll"}])
        with self.assertRaisesRegex(ValueError, "binding differs"):
            native.targets(inventory, [{**row, "sha256": "b" * 64}])
        with self.assertRaisesRegex(ValueError, "duplicate"):
            native.targets(inventory, [row, row])

    def test_original_receipt_relocation_and_contribution_omission_refused(self):
        row = {"path": "frozen/source.zip", "inventory_path": "original/source.zip", "sha256": "a" * 64, "bytes": 12}
        self.assertEqual(native.original(row)["path"], "original/source.zip")
        contribution = {"sources": [], "notices": [], "artifacts": [], "remaining": ["pending"],
                        "recipe": {"status": "exact_local_recipe_present", "files": [], "recipe_options": [],
                                   "options_role": "unqualified"}}
        component = dict(contribution)
        native.validate_contribution(component, contribution)
        component["remaining"] = []
        with self.assertRaisesRegex(ValueError, "obligation"):
            native.validate_contribution(component, contribution)


class ApplicabilityTests(unittest.TestCase):
    def fixture(self):
        target = {"component_id": "cad-cpython", "path": "candidate/python313.dll",
                  "destination": "bin/cad-runtime/python313.dll", "sha256": "a" * 64, "bytes": 42}
        source = {"kind": "locked-archive", "archive_filename": "python-3.13.15-embed-amd64.zip",
                  "archive_path": "cache/python-3.13.15-embed-amd64.zip",
                  "archive_sha256": "d1f04d990aee1253d8569e8e5104e30fa9f5fa830899f14843448872d936a2cf",
                  "archive_members": {target["path"]: "python313.dll"},
                  "licensing_clearance": False, "source_closure_qualified": False}
        component = {"id": "cad-cpython", "package": {"name": "CPython", "version": "3.13.15",
                                                       "license": "PSF-2.0", "source": source}}
        return {"components": [component]}, target

    def test_known_parent_and_selected_archive_member_match(self):
        inventory, target = self.fixture()
        package = native.applicability("cpython_spdx", "cad-cpython", inventory, [target])
        self.assertEqual(package, {"name": "CPython", "version": "3.13.15", "license": "PSF-2.0"})

    def test_parent_relabelling_refused_with_identical_target_bytes(self):
        changes = (("name", "OtherPython"), ("version", "3.13.14"), ("kind", "workspace"),
                   ("archive_sha256", "b" * 64), ("archive_filename", "different.zip"))
        for key, value in changes:
            with self.subTest(key=key):
                inventory, target = self.fixture()
                package = inventory["components"][0]["package"]
                (package if key in ("name", "version") else package["source"])[key] = value
                with self.assertRaisesRegex(ValueError, "native parent"):
                    native.applicability("cpython_spdx", "cad-cpython", inventory, [target])

    def test_missing_duplicate_or_relabelled_parent_member_refused(self):
        inventory, target = self.fixture()
        for mapping in ({}, {target["path"]: "other.dll"}, {target["path"]: "../python313.dll"}):
            inventory["components"][0]["package"]["source"]["archive_members"] = mapping
            with self.assertRaises(ValueError):
                native.applicability("cpython_spdx", "cad-cpython", inventory, [target])
        inventory, target = self.fixture()
        inventory["components"].append(dict(inventory["components"][0]))
        with self.assertRaisesRegex(ValueError, "native parent"):
            native.applicability("cpython_spdx", "cad-cpython", inventory, [target])

    def test_typed_wheel_member_rejects_shortened_suffix_with_same_target(self):
        for kind, owner, member in (("geos_recipe", "cad-shapely", "shapely.libs/geos.dll"),
                                   ("openblas_wheel_recipe", "cad-numpy", "numpy.libs/openblas.dll")):
            with self.subTest(kind=kind):
                name, version, filename, checksum = native.PARENT_PROFILES[kind]
                target = {"component_id": owner, "path": "candidate/" + member,
                          "destination": "bin/cad/" + member, "sha256": "a" * 64, "bytes": 42}
                source = {"kind": "locked-archive", "archive_filename": filename,
                          "archive_path": "cache/" + filename, "archive_sha256": checksum,
                          "archive_members": {target["path"]: member},
                          "licensing_clearance": False, "source_closure_qualified": False}
                inventory = {"components": [{"id": owner, "package": {
                    "name": name, "version": version, "license": "fixture", "source": source}}]}
                bindings = [{"target": target, "member": member}]
                native.applicability(kind, owner, inventory, [target], bindings)
                source["archive_members"][target["path"]] = member.rsplit("/", 1)[-1]
                # The suffix-only precheck admits this; exact typed replay must refuse it.
                native.applicability(kind, owner, inventory, [target])
                with self.assertRaisesRegex(ValueError, "typed archive member differs"):
                    native.applicability(kind, owner, inventory, [target], bindings)


if __name__ == "__main__":
    unittest.main()
