"""Native wheel evidence must survive replay without trusting wrapper assertions."""
import base64
import copy
import hashlib
import importlib.util
import io
import json
from pathlib import Path
import tarfile
import unittest
from unittest import mock
import zipfile

ROOT = Path(__file__).resolve().parents[1]
SPEC = importlib.util.spec_from_file_location("native_wheel_source_inputs", ROOT / "scripts/qualification/native_wheel_source_inputs.py")
native = importlib.util.module_from_spec(SPEC)


def sha(data):
    return hashlib.sha256(data).hexdigest()


def tree_blob(path, data, mode="100644"):
    return {"path": path, "mode": mode, "type": "blob", "size": len(data),
            "sha": hashlib.sha1(b"blob " + str(len(data)).encode() + b"\0" + data).hexdigest()}


def zip_bytes(members):
    buffer = io.BytesIO()
    with zipfile.ZipFile(buffer, "w") as archive:
        for name, data in members.items():
            archive.writestr(name, data)
    return buffer.getvalue()


def tar_bytes(members):
    buffer = io.BytesIO()
    with tarfile.open(fileobj=buffer, mode="w:gz") as archive:
        for name, data in members.items():
            item = tarfile.TarInfo(name)
            item.size = len(data)
            archive.addfile(item, io.BytesIO(data))
    return buffer.getvalue()


class Reader:
    """An explicit content resolver; frozen snapshots never read the live fixture."""
    def __init__(self):
        self.files = {}

    def put(self, path, data):
        self.files[path] = data
        return {"path": path, "sha256": sha(data), "bytes": len(data)}

    def put_json(self, path, data):
        return self.put(path, json.dumps(data, sort_keys=True).encode())

    def record(self, receipt, role):
        if role not in {"native_input", "source", "notice", "recipe", "metadata", "binary", "fixture"}:
            raise ValueError("unsupported reader role")
        data = self.files[receipt["path"]]
        if sha(data) != receipt["sha256"] or len(data) != receipt["bytes"]:
            raise ValueError("changed receipt: " + role)
        return {key: receipt[key] for key in ("path", "sha256", "bytes")}

    def bytes(self, receipt, role, limit=32 << 20):
        self.record(receipt, role)
        data = self.files[receipt["path"]]
        if len(data) > limit:
            raise ValueError("read limit")
        return data

    def json(self, receipt, role):
        return json.loads(self.bytes(receipt, role))

    def archive_members(self, receipt, names, role):
        data = self.bytes(receipt, role)
        if zipfile.is_zipfile(io.BytesIO(data)):
            with zipfile.ZipFile(io.BytesIO(data)) as archive:
                return {name: archive.read(name) for name in names}
        with tarfile.open(fileobj=io.BytesIO(data)) as archive:
            return {name: archive.extractfile(name).read() for name in names}


class NativeWheelSourceInputsTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        if SPEC.loader is None or not Path(SPEC.origin).exists():
            raise AssertionError("native wheel normalizers are not implemented")
        SPEC.loader.exec_module(native)

    def setUp(self):
        self.reader = Reader()
        self.notice_rows = []
        self.files = {}
        self.target_bytes = {}

    def file(self, role, data):
        self.files[role] = self.reader.put("evidence/" + role, data)

    def metadata(self, project, version, filename, data):
        return {"info": {"name": project, "version": version}, "urls": [{
            "filename": filename, "packagetype": "bdist_wheel", "size": len(data),
            "digests": {"sha256": sha(data)}, "url": "https://files.pythonhosted.org/packages/" + filename}]}

    def notice(self, role, member, data):
        self.notice_rows.append({"archive_role": role, "member": member,
                                 "file": self.reader.put("notices/" + str(len(self.notice_rows)), data)})

    def wrapper(self, profile):
        return self.reader.put_json("inputs.json", {"schema": "vertex.native-wheel-inputs.v1", "profile": profile,
                                                    "files": self.files, "notices": self.notice_rows})

    def targets(self, component):
        return [{"component_id": component, "path": "runtime/" + name,
                 "destination": "cad-runtime/" + name, "sha256": sha(data), "bytes": len(data)}
                for name, data in self.target_bytes.items()]

    def geos(self):
        binaries = {"shapely.libs/geos-ae6efa0782962b98e358f10ea539ae5f.dll": b"GEOS 3.13.1\0",
                    "shapely.libs/geos_c-072b7a9224d16d3e4ab2395bb855b2d3.dll": b"3.13.1-CAPI-1.19.2\0"}
        self.target_bytes = binaries
        wheel_notice = b"GNU LESSER GENERAL PUBLIC LICENSE\r\nVersion 2.1\r\n"
        tag_notice = wheel_notice.replace(b"\r\n", b"\n")
        wheel_members = {**binaries, "shapely-2.1.2.dist-info/licenses/LICENSE_GEOS": wheel_notice,
                         "shapely-2.1.2.dist-info/licenses/LICENSE_win32": b"Microsoft runtime terms\r\n"}
        wheel = zip_bytes(wheel_members)
        source_members = {"geos-3.13.1/COPYING": tag_notice, "geos-3.13.1/Version.txt": b"3.13.1"}
        source = tar_bytes(source_members)
        recipe_members = {"shapely-2.1.2/.github/workflows/release.yml": b'GEOS_VERSION: "3.13.1"\nwindows-2022\nmsvc_arch: x64\ndelvewheel repair\n',
                          "shapely-2.1.2/ci/install_geos.cmd": b"cmake -GNinja -D CMAKE_BUILD_TYPE=Release\n",
                          "shapely-2.1.2/ci/wheelbuilder/LICENSE_GEOS": tag_notice}
        recipe = tar_bytes(recipe_members)
        self.file("parent_wheel", wheel)
        self.file("parent_pypi", json.dumps(self.metadata("shapely", "2.1.2", "shapely-2.1.2-cp313-cp313-win_amd64.whl", wheel)).encode())
        self.file("source_archive", source)
        self.file("recipe_archive", recipe)
        for name in ["shapely-2.1.2.dist-info/licenses/LICENSE_GEOS", "shapely-2.1.2.dist-info/licenses/LICENSE_win32"]:
            self.notice("parent_wheel", name, wheel_members[name])
        self.notice("source_archive", "geos-3.13.1/COPYING", tag_notice)
        self.notice("recipe_archive", "shapely-2.1.2/ci/wheelbuilder/LICENSE_GEOS", tag_notice)
        pins = {"shapely_wheel": sha(wheel), "geos_source": sha(source), "shapely_recipe": sha(recipe),
                "shapely_pypi": self.files["parent_pypi"]["sha256"]}
        self.patch_pins(pins)
        self.addCleanup(mock.patch.stopall)
        mock.patch.object(native, "GEOS_SHA512", hashlib.sha512(source).hexdigest()).start()
        return self.wrapper("geos"), self.targets("cad-shapely")

    def patch_pins(self, pins):
        mock.patch.dict(native.PINNED_SHA256, pins).start()
        self.addCleanup(mock.patch.stopall)

    def openblas(self):
        revision = "446c436e10450c348169808f1a6b3fae0925c9f7"
        recipe_revision = "78fc0eaf6de71d92fd98f74f7e5bfa356a2f83e3"
        binary = b"OpenBLAS 0.3.34.106.0 ILP64\0"
        member = "numpy.libs/libscipy_openblas64_-ed4f167a5330424524f45258e7ca2c8d.dll"
        self.target_bytes = {member: binary}
        numpy_notice = b"NumPy, OpenBLAS and compiler runtime notices\n"
        upstream_notice = b"OpenBLAS and GCC runtime license\n"
        source_notice = b"OpenBLAS BSD license\n"
        numpy_wheel = zip_bytes({member: binary, "numpy-2.5.3.dist-info/licenses/LICENSE.txt": numpy_notice})
        upstream_wheel = zip_bytes({"scipy_openblas64/lib/libscipy_openblas64_.dll": binary,
                                   "scipy_openblas64-0.3.34.106.0.dist-info/licenses/LICENSE.txt": upstream_notice})
        source_members = {"OpenBLAS-" + revision + "/LICENSE": source_notice,
                          "OpenBLAS-" + revision + "/lapack-netlib/LICENSE": b"LAPACK notice\n"}
        recipe_members = {"openblas-libs-" + recipe_revision + "/openblas_commit.txt": b"v0.3.34-106-g446c436e\n",
                          "openblas-libs-" + recipe_revision + "/.gitmodules": b'[submodule "OpenBLAS"]\npath = OpenBLAS\nurl = https://github.com/xianyi/OpenBLAS.git\n',
                          "openblas-libs-" + recipe_revision + "/pyproject.toml": b'name = "scipy-openblas64"\nversion = "0.3.34.106.0"\n',
                          "openblas-libs-" + recipe_revision + "/tools/build_steps_windows.sh": b"INTERFACE64=1 SYMBOLSUFFIX=64_ SYMBOLPREFIX=scipy_\nUSE_THREAD=1 USE_OPENMP=0\npatch -p1 < ../patches-windows/openblas-make-libs.patch\n",
                          "openblas-libs-" + recipe_revision + "/.github/workflows/windows.yml": b"INTERFACE64: ['1', '0']\nLDFLAGS=-lucrt -static -static-libgcc\n",
                          "openblas-libs-" + recipe_revision + "/patches-windows/openblas-make-libs.patch": b"Windows export patch\n",
                          "openblas-libs-" + recipe_revision + "/LICENSE.txt": upstream_notice,
                          "openblas-libs-" + recipe_revision + "/tools/LICENSE_win32.txt": b"Compiler runtime terms\n"}
        self.file("parent_wheel", numpy_wheel)
        self.file("parent_pypi", json.dumps(self.metadata("numpy", "2.5.3", "numpy-2.5.3-cp313-cp313-win_amd64.whl", numpy_wheel)).encode())
        self.file("upstream_wheel", upstream_wheel)
        self.file("upstream_pypi", json.dumps(self.metadata("scipy-openblas64", "0.3.34.106.0", "scipy_openblas64-0.3.34.106.0-py3-none-win_amd64.whl", upstream_wheel)).encode())
        self.file("source_archive", tar_bytes(source_members))
        self.file("recipe_archive", tar_bytes(recipe_members))
        source_tree = [tree_blob(name.split("/", 1)[1], data) for name, data in source_members.items()]
        recipe_tree = [tree_blob(name.split("/", 1)[1], data, "100755" if name.endswith("build_steps_windows.sh") else "100644")
                       for name, data in recipe_members.items()]
        recipe_tree.append({"path": "OpenBLAS", "mode": "160000", "type": "commit", "sha": revision})
        self.file("source_tree", json.dumps({"sha": revision, "truncated": False, "tree": source_tree}).encode())
        self.file("recipe_tree", json.dumps({"sha": recipe_revision, "truncated": False, "tree": recipe_tree}).encode())
        statement = {"subject": [{"name": "scipy_openblas64-0.3.34.106.0-py3-none-win_amd64.whl", "digest": {"sha256": sha(upstream_wheel)}}]}
        provenance = {"version": 1, "attestation_bundles": [{"publisher": {"kind": "GitHub", "repository": "MacPython/openblas-libs", "workflow": "publish.yml", "environment": "pypi"},
                      "attestations": [{"envelope": {"statement": base64.b64encode(json.dumps(statement).encode()).decode()}}]}]}
        self.file("provenance", json.dumps(provenance).encode())
        self.notice("parent_wheel", "numpy-2.5.3.dist-info/licenses/LICENSE.txt", numpy_notice)
        self.notice("upstream_wheel", "scipy_openblas64-0.3.34.106.0.dist-info/licenses/LICENSE.txt", upstream_notice)
        for name, data in source_members.items():
            self.notice("source_archive", name, data)
        for name, data in recipe_members.items():
            if name.endswith("LICENSE.txt") or name.endswith("LICENSE_win32.txt"):
                self.notice("recipe_archive", name, data)
        self.patch_pins({"numpy_wheel": sha(numpy_wheel), "openblas_wheel": sha(upstream_wheel),
                         "openblas_source": self.files["source_archive"]["sha256"], "openblas_recipe": self.files["recipe_archive"]["sha256"],
                         "openblas_source_tree": self.files["source_tree"]["sha256"], "openblas_recipe_tree": self.files["recipe_tree"]["sha256"],
                         "numpy_pypi": self.files["parent_pypi"]["sha256"], "openblas_pypi": self.files["upstream_pypi"]["sha256"],
                         "openblas_provenance": self.files["provenance"]["sha256"]})
        return self.wrapper("openblas"), self.targets("cad-numpy")

    def test_geos_pass_preserves_both_notice_bytes_and_false_trust_boundary(self):
        receipt, targets = self.geos()
        result = native.validate_geos_inputs(receipt, self.reader, targets)
        frozen = copy.deepcopy(self.reader)
        self.assertEqual(result, native.validate_geos_inputs(receipt, frozen, targets))
        self.assertEqual(result["sources"][0]["status"], "exact_local_source_present")
        self.assertEqual(len(result["notices"]), 4)
        self.assertEqual(result["dependency_dispositions"][0]["checksum_origin"], "assignment_pin")
        self.assertFalse(result["dependency_dispositions"][0]["publisher_authenticated"])

    def test_openblas_pass_binds_same_dll_and_exact_gitlink_with_frozen_replay(self):
        receipt, targets = self.openblas()
        result = native.validate_openblas_inputs(receipt, self.reader, targets)
        self.assertEqual(result, native.validate_openblas_inputs(receipt, copy.deepcopy(self.reader), targets))
        self.assertEqual(result["dependency_dispositions"][0]["component_id"], "cad-numpy")
        self.assertTrue(result["dependency_dispositions"][0]["upstream_dll_byte_identity"])
        self.assertFalse(result["dependency_dispositions"][0]["source_derivation_verified"])

    def test_wrong_inventory_owner_is_refused(self):
        receipt, targets = self.geos()
        targets[0]["component_id"] = "cad-numpy"
        with self.assertRaises(ValueError):
            native.validate_geos_inputs(receipt, self.reader, targets)

    def test_changed_selected_member_target_is_refused(self):
        receipt, targets = self.openblas()
        targets[0]["sha256"] = sha(b"another library")
        with self.assertRaises(ValueError):
            native.validate_openblas_inputs(receipt, self.reader, targets)

    def test_changed_notice_with_rehashed_wrapper_is_refused(self):
        receipt, targets = self.geos()
        row = self.notice_rows[0]
        row["file"] = self.reader.put(row["file"]["path"], b"Substituted notice\n")
        receipt = self.wrapper("geos")
        with self.assertRaises(ValueError):
            native.validate_geos_inputs(receipt, self.reader, targets)

    def test_missing_exact_recipe_gitlink_is_refused_after_wrapper_rehash(self):
        receipt, targets = self.openblas()
        self.file("recipe_tree", json.dumps({"sha": "78fc0eaf6de71d92fd98f74f7e5bfa356a2f83e3", "truncated": False, "tree": []}).encode())
        receipt = self.wrapper("openblas")
        with self.assertRaises(ValueError):
            native.validate_openblas_inputs(receipt, self.reader, targets)

    def test_unclosed_source_submodule_is_refused_after_wrapper_rehash(self):
        receipt, targets = self.openblas()
        self.file("source_tree", json.dumps({"sha": "446c436e10450c348169808f1a6b3fae0925c9f7", "truncated": False,
                  "tree": [{"path": "another", "mode": "160000", "type": "commit", "sha": "0" * 40}]}).encode())
        receipt = self.wrapper("openblas")
        with self.assertRaises(ValueError):
            native.validate_openblas_inputs(receipt, self.reader, targets)

    def test_wrong_parent_platform_in_original_metadata_is_refused(self):
        receipt, targets = self.geos()
        metadata = self.reader.json(self.files["parent_pypi"], "fixture")
        metadata["urls"][0]["filename"] = "shapely-2.1.2-cp313-cp313-win32.whl"
        self.file("parent_pypi", json.dumps(metadata).encode())
        receipt = self.wrapper("geos")
        with self.assertRaises(ValueError):
            native.validate_geos_inputs(receipt, self.reader, targets)

    def test_changed_upstream_member_with_rehashed_metadata_and_wrapper_is_refused(self):
        receipt, targets = self.openblas()
        changed = zip_bytes({"scipy_openblas64/lib/libscipy_openblas64_.dll": b"other OpenBLAS build",
                             "scipy_openblas64-0.3.34.106.0.dist-info/licenses/LICENSE.txt": b"OpenBLAS and GCC runtime license\n"})
        self.file("upstream_wheel", changed)
        self.file("upstream_pypi", json.dumps(self.metadata("scipy-openblas64", "0.3.34.106.0",
                  "scipy_openblas64-0.3.34.106.0-py3-none-win_amd64.whl", changed)).encode())
        receipt = self.wrapper("openblas")
        with self.assertRaises(ValueError):
            native.validate_openblas_inputs(receipt, self.reader, targets)

    def test_missing_required_notice_is_refused(self):
        receipt, targets = self.geos()
        self.notice_rows.pop()
        receipt = self.wrapper("geos")
        with self.assertRaises(ValueError):
            native.validate_geos_inputs(receipt, self.reader, targets)

    def test_changed_target_destination_is_refused(self):
        receipt, targets = self.geos()
        targets[0]["destination"] = "another/library.dll"
        with self.assertRaises(ValueError):
            native.validate_geos_inputs(receipt, self.reader, targets)

    def test_duplicate_selected_target_is_refused(self):
        receipt, targets = self.openblas()
        targets.append(copy.deepcopy(targets[0]))
        with self.assertRaises(ValueError):
            native.validate_openblas_inputs(receipt, self.reader, targets)

    def test_truncated_source_tree_is_refused(self):
        receipt, targets = self.openblas()
        tree = self.reader.json(self.files["source_tree"], "fixture")
        tree["truncated"] = True
        self.file("source_tree", json.dumps(tree).encode())
        receipt = self.wrapper("openblas")
        with self.assertRaises(ValueError):
            native.validate_openblas_inputs(receipt, self.reader, targets)

    def test_target_binding_keeps_archive_member_and_trust_boundary(self):
        receipt, targets = self.openblas()
        result = native.validate_openblas_inputs(receipt, self.reader, targets)
        row = result["target_bindings"][0]
        self.assertEqual(row["target"], targets[0])
        self.assertEqual(row["member"], "numpy.libs/libscipy_openblas64_-ed4f167a5330424524f45258e7ca2c8d.dll")
        self.assertEqual(row["upstream_member"], "scipy_openblas64/lib/libscipy_openblas64_.dll")
        self.assertTrue(row["byte_identity_verified"])
        for key in ["publisher_authenticated", "signature_verified", "source_derivation_verified",
                    "rights_qualified", "offline_rebuild_qualified"]:
            self.assertFalse(row[key])

    def test_attestation_subject_substitution_is_refused_after_wrapper_rehash(self):
        receipt, targets = self.openblas()
        value = self.reader.json(self.files["provenance"], "fixture")
        statement = {"subject": [{"name": "scipy_openblas64-0.3.34.106.0-py3-none-win_amd64.whl",
                                  "digest": {"sha256": "0" * 64}}]}
        value["attestation_bundles"][0]["attestations"][0]["envelope"]["statement"] = base64.b64encode(json.dumps(statement).encode()).decode()
        self.file("provenance", json.dumps(value).encode())
        receipt = self.wrapper("openblas")
        with self.assertRaises(ValueError):
            native.validate_openblas_inputs(receipt, self.reader, targets)

    def test_parent_version_substitution_is_refused_after_wrapper_rehash(self):
        receipt, targets = self.geos()
        metadata = self.reader.json(self.files["parent_pypi"], "fixture")
        metadata["info"]["version"] = "2.1.1"
        self.file("parent_pypi", json.dumps(metadata).encode())
        receipt = self.wrapper("geos")
        with self.assertRaises(ValueError):
            native.validate_geos_inputs(receipt, self.reader, targets)

    def test_wrong_input_profile_is_refused(self):
        receipt, targets = self.geos()
        receipt = self.wrapper("openblas")
        with self.assertRaises(ValueError):
            native.validate_geos_inputs(receipt, self.reader, targets)

    def test_unexpected_notice_origin_role_is_refused(self):
        receipt, targets = self.geos()
        self.notice_rows[0]["archive_role"] = "parent_pypi"
        receipt = self.wrapper("geos")
        with self.assertRaises(ValueError):
            native.validate_geos_inputs(receipt, self.reader, targets)

    def test_omitted_source_blob_row_with_rehashed_wrapper_is_refused(self):
        receipt, targets = self.openblas()
        tree = self.reader.json(self.files["source_tree"], "fixture")
        tree["tree"].pop()
        self.file("source_tree", json.dumps(tree).encode())
        receipt = self.wrapper("openblas")
        with self.assertRaises(ValueError):
            native.validate_openblas_inputs(receipt, self.reader, targets)

    def test_substituted_recipe_blob_hash_with_rehashed_wrapper_is_refused(self):
        receipt, targets = self.openblas()
        tree = self.reader.json(self.files["recipe_tree"], "fixture")
        tree["tree"][0]["sha"] = "0" * 40
        self.file("recipe_tree", json.dumps(tree).encode())
        receipt = self.wrapper("openblas")
        with self.assertRaises(ValueError):
            native.validate_openblas_inputs(receipt, self.reader, targets)

    def test_changed_archive_member_bytes_are_independently_refused(self):
        receipt, targets = self.openblas()
        original = self.reader.archive_members

        def corrupted_archive(receipt_value, names, role):
            result = original(receipt_value, names, role)
            source_name = "OpenBLAS-446c436e10450c348169808f1a6b3fae0925c9f7/LICENSE"
            if source_name in result:
                result[source_name] = b"changed source license\n"
            return result

        with mock.patch.object(self.reader, "archive_members", side_effect=corrupted_archive):
            with self.assertRaisesRegex(ValueError, "Git blob"):
                native.validate_openblas_inputs(receipt, self.reader, targets)

    def test_full_blob_counts_are_reported_without_derivation_qualification(self):
        receipt, targets = self.openblas()
        result = native.validate_openblas_inputs(receipt, self.reader, targets)
        disposition = result["dependency_dispositions"][0]
        self.assertEqual(disposition["source_git_blobs_verified"], 2)
        self.assertEqual(disposition["recipe_git_blobs_verified"], 8)
        self.assertFalse(disposition["source_derivation_verified"])

    def test_blob_mode_and_type_must_agree_even_with_a_fixture_tree_pin(self):
        receipt, targets = self.openblas()
        tree = self.reader.json(self.files["source_tree"], "fixture")
        tree["tree"][0]["type"] = "tree"
        self.file("source_tree", json.dumps(tree).encode())
        self.patch_pins({"openblas_source_tree": self.files["source_tree"]["sha256"]})
        receipt = self.wrapper("openblas")
        with self.assertRaisesRegex(ValueError, "mode/type"):
            native.validate_openblas_inputs(receipt, self.reader, targets)

    def test_symlink_blobs_are_explicitly_refused_for_selected_profiles(self):
        receipt, targets = self.openblas()
        tree = self.reader.json(self.files["source_tree"], "fixture")
        tree["tree"][0]["mode"] = "120000"
        self.file("source_tree", json.dumps(tree).encode())
        self.patch_pins({"openblas_source_tree": self.files["source_tree"]["sha256"]})
        receipt = self.wrapper("openblas")
        with self.assertRaisesRegex(ValueError, "symlink"):
            native.validate_openblas_inputs(receipt, self.reader, targets)

    def test_reformatted_shapely_metadata_with_same_fields_is_refused(self):
        receipt, targets = self.geos()
        original = self.reader.json(self.files["parent_pypi"], "fixture")
        self.file("parent_pypi", json.dumps(original, indent=2, sort_keys=True).encode())
        receipt = self.wrapper("geos")
        with self.assertRaises(ValueError):
            native.validate_geos_inputs(receipt, self.reader, targets)

    def test_reformatted_numpy_metadata_with_same_fields_is_refused(self):
        receipt, targets = self.openblas()
        original = self.reader.json(self.files["parent_pypi"], "fixture")
        self.file("parent_pypi", json.dumps(original, indent=2, sort_keys=True).encode())
        receipt = self.wrapper("openblas")
        with self.assertRaises(ValueError):
            native.validate_openblas_inputs(receipt, self.reader, targets)

    def test_upstream_metadata_url_path_substitution_is_refused(self):
        receipt, targets = self.openblas()
        original = self.reader.json(self.files["upstream_pypi"], "fixture")
        original["urls"][0]["url"] = "https://files.pythonhosted.org/packages/fabricated/scipy_openblas64-0.3.34.106.0-py3-none-win_amd64.whl"
        self.file("upstream_pypi", json.dumps(original).encode())
        receipt = self.wrapper("openblas")
        with self.assertRaises(ValueError):
            native.validate_openblas_inputs(receipt, self.reader, targets)

    def test_reformatted_attestation_with_same_subject_is_refused(self):
        receipt, targets = self.openblas()
        original = self.reader.json(self.files["provenance"], "fixture")
        self.file("provenance", json.dumps(original, indent=2, sort_keys=True).encode())
        receipt = self.wrapper("openblas")
        with self.assertRaises(ValueError):
            native.validate_openblas_inputs(receipt, self.reader, targets)


if __name__ == "__main__":
    unittest.main()
