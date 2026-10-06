"""Normalizer tests; the fixture reader models authenticated opaque archives.

The real reader owns file/archive authentication. These fixtures provide its
verified records and original-member bytes without storing 38 MB in the repo.
"""
import copy
import hashlib
import importlib.util
import json
from pathlib import Path
import unittest

MODULE = Path(__file__).resolve().parents[1] / "scripts/qualification/native_cpython_source_inputs.py"

def sha(data):
    return hashlib.sha256(data).hexdigest()


class FixtureReader:
    def __init__(self, frozen=False):
        self.data, self.objects, self.members, self.records = {}, {}, {}, {}
        self.frozen = frozen
        self.calls = []

    def add(self, name, data=b"", digest=None, obj=None, members=None):
        rec = {"path": name, "sha256": digest or sha(data), "bytes": len(data)}
        self.records[name] = rec
        self.data[name] = data
        if obj is not None:
            self.objects[name] = obj
        if members is not None:
            self.members[name] = members
        return copy.deepcopy(rec)

    def record(self, receipt, role):
        self.calls.append((role, receipt["path"]))
        expected = self.records[receipt["path"]]
        if receipt != expected:
            raise ValueError("reader authentication differs")
        return {**receipt, "path": "frozen/" + receipt["path"] if self.frozen else receipt["path"]}

    def bytes(self, receipt, role, limit=32 * 1024 * 1024):
        self.record(receipt, role)
        data = self.data[receipt["path"]]
        if len(data) > limit:
            raise ValueError("reader byte bound")
        return data

    def json(self, receipt, role):
        self.record(receipt, role)
        return copy.deepcopy(self.objects.get(receipt["path"], json.loads(self.data[receipt["path"]] or b"{}")))

    def archive_members(self, receipt, names, role):
        self.record(receipt, role)
        try:
            return {name: self.members[receipt["path"]][name] for name in names}
        except KeyError as error:
            raise ValueError("Missing original archive member") from error


def fixture(frozen=False):
    reader = FixtureReader(frozen)
    embed_hash = "d1f04d990aee1253d8569e8e5104e30fa9f5fa830899f14843448872d936a2cf"
    source_hash = "1e66a7945a48390ee4c2a4268a0e4185884059a13c4aab6d148aa208deea4a76"
    sbom_hash = "ba428f93acb06764f0005246f8ca48bb1c04feef64ce7db0b177fb2fd371c423"
    parent = {"SPDXID": "SPDXRef-PACKAGE-cpython", "name": "CPython", "versionInfo": "3.13.15",
              "downloadLocation": "https://www.python.org/ftp/python/3.13.15/python-3.13.15-embed-amd64.zip",
              "packageFileName": "python-3.13.15-embed-amd64.zip", "checksums": [{"algorithm": "SHA256", "checksumValue": embed_hash}], "licenseConcluded": "PSF-2.0"}
    specs = [("bzip2", "bzip2", "1.0.8"), ("expat-2.8.2", "expat", "2.8.2"),
             ("hacl-star-bb3d0dc8d9d15a5cd51094d5b69e70aa09005ff0", "hacl-star", "bb3d0dc8d9d15a5cd51094d5b69e70aa09005ff0"),
             ("libb2-0.98.1", "libb2", "0.98.1"), ("libffi", "libffi", "3.4.4"),
             ("macholib-1.0", "macholib", "1.0"), ("mpdecimal", "mpdecimal", "4.0.0"),
             ("mpdecimal-2.5.1", "mpdecimal", "2.5.1"), ("openssl", "openssl", "3.0.21"),
             ("sqlite", "sqlite", "3.50.4.0"), ("tcl-core", "tcl-core", "8.6.15.0"),
             ("tk", "tk", "8.6.15.0"), ("xz", "xz", "5.2.5"), ("zlib", "zlib", "1.3.1")]
    # Tests do not assert inferred license branches: only original-member equality.
    licenses = {
        "bzip2": ["cpython-source-deps-bzip2-1.0.8/LICENSE"], "expat": ["expat-2.8.2/COPYING"],
        "hacl-star": ["hacl-star-bb3d0dc8d9d15a5cd51094d5b69e70aa09005ff0/" + n for n in ["LICENSE", "dist/LICENSE.txt", "vale/LICENSE"]],
        "libb2": ["libb2-0.98.1/COPYING"],
        "libffi": ["cpython-source-deps-libffi-3.4.4/" + n for n in ["LICENSE", "LICENSE-BUILDTOOLS"]],
        "mpdecimal": ["cpython-source-deps-mpdecimal-4.0.0/" + n for n in ["COPYRIGHT.txt", "doc/COPYRIGHT.txt"]],
        "openssl": ["cpython-source-deps-openssl-3.0.21/LICENSE.txt"],
        "xz": ["cpython-source-deps-xz-5.2.5/" + n for n in ["COPYING", "COPYING.GPLv2", "COPYING.GPLv3", "COPYING.LGPLv2.1"]],
        "zlib": ["cpython-source-deps-zlib-1.3.1/LICENSE"], "macholib": [], "sqlite": []}
    packages, edges, declared = [parent], [], []
    for suffix, name, version in specs:
        pid = "SPDXRef-PACKAGE-" + suffix
        edge = {"spdxElementId": parent["SPDXID"], "relationshipType": "DEPENDS_ON", "relatedSpdxElement": pid}
        data = (suffix + " source archive").encode()
        package = {"SPDXID": pid, "name": name, "versionInfo": version, "downloadLocation": "https://publisher.example/" + suffix + ".tar.gz", "checksums": [{"algorithm": "SHA256", "checksumValue": sha(data)}], "licenseConcluded": "NOASSERTION"}
        packages.append(package); edges.append(edge)
        availability = None
        if suffix not in {"mpdecimal-2.5.1", "tcl-core", "tk"}:
            member_data = {m: ("notice:" + m).encode() for m in licenses[name]}
            source = reader.add("sources/" + suffix + ".tar.gz", data, members=member_data)
            evidence = [{"source_archive_member": m, "local_file": reader.add("notices/" + suffix + "-" + str(i), d)} for i, (m, d) in enumerate(member_data.items())]
            availability = {"url": package["downloadLocation"], "local_file": source, "license_evidence": evidence}
        declared.append({"publisher_package": package, "publisher_relationship": edge, "source_availability": availability,
                         "license_qualified": False, "source_rebuild_qualified": False, "binary_derivation_qualified": False,
                         "candidate_relevance": {"status": "untrusted wrapper text"}})
    sbom = {"SPDXID": "SPDXRef-DOCUMENT", "documentNamespace": parent["downloadLocation"] + ".spdx.json", "files": [], "packages": packages,
            "relationships": edges + [{"spdxElementId": "SPDXRef-DOCUMENT", "relationshipType": "DESCRIBES", "relatedSpdxElement": parent["SPDXID"]}]}
    sbom_rec = reader.add("metadata/python-3.13.15-embed-amd64.zip.spdx.json", digest=sbom_hash, obj=sbom)
    controls = ["Modules/_blake2/blake2b_impl.c", "PCbuild/_bz2.vcxproj", "PCbuild/_ctypes.vcxproj", "PCbuild/_decimal.vcxproj", "PCbuild/_elementtree.vcxproj", "PCbuild/_hashlib.vcxproj", "PCbuild/_lzma.vcxproj", "PCbuild/_sqlite3.vcxproj", "PCbuild/_ssl.vcxproj", "PCbuild/get_externals.bat", "PCbuild/libffi.props", "PCbuild/liblzma.vcxproj", "PCbuild/openssl.props", "PCbuild/pyexpat.vcxproj", "PCbuild/python.props", "PCbuild/pythoncore.vcxproj", "PCbuild/sqlite3.vcxproj"]
    src_members, control_rows = {}, []
    for member in controls:
        canonical = "Python-3.13.15/" + member
        data = ("original control " + member).encode()
        src_members[canonical] = data
        control_rows.append({"source_archive_member": canonical, "local_file": reader.add("controls/" + member.replace("/", "--"), data)})
    vendor_packages, vendor_files, vendor_edges, vendor_rows = [], [], [], []
    for name, version, count in [("expat", "2.8.2", 23), ("hacl-star", "bb3d0dc8d9d15a5cd51094d5b69e70aa09005ff0", 20), ("libb2", "0.98.1", 14), ("macholib", "1.0", 4), ("mpdecimal", "2.5.1", 52)]:
        package = {"SPDXID": "SPDXRef-PACKAGE-" + name, "name": name, "versionInfo": version}
        vendor_packages.append(package); rows = []
        for index in range(count):
            member = "vendored/" + name + "/" + str(index) + ".c"
            data = member.encode(); src_members["Python-3.13.15/" + member] = data
            fid = "SPDXRef-FILE-" + name + str(index)
            vendor_files.append({"SPDXID": fid, "fileName": member, "checksums": [{"algorithm": "SHA256", "checksumValue": sha(data)}]})
            vendor_edges.append({"spdxElementId": package["SPDXID"], "relationshipType": "CONTAINS", "relatedSpdxElement": fid})
            rows.append({"member": "Python-3.13.15/" + member, "sha256": sha(data), "bytes": len(data)})
        vendor_rows.append({"package": package, "verified_source_members": sorted(rows, key=lambda r:r['member'])})
    vendor = {"packages": vendor_packages, "files": vendor_files, "relationships": vendor_edges}
    vendor_data = json.dumps(vendor).encode(); src_members["Python-3.13.15/Misc/sbom.spdx.json"] = vendor_data
    vendor_rec = reader.add("metadata/vendor.json", vendor_data, obj=vendor)
    src_members["Python-3.13.15/LICENSE"] = b"CPython source license"
    source_rec = reader.add("archives/Python-3.13.15.tar.xz", digest=source_hash, members=src_members)
    nested = {"ctypes/macholib/" + n: n.encode() for n in ["dyld.pyc", "dylib.pyc", "fetch_macholib", "fetch_macholib.bat", "framework.pyc", "README.ctypes", "__init__.pyc"]}
    stdlib = reader.add("runtime/python313.zip", b"authenticated stdlib", members=nested)
    embed_members = {"python313.dll": b"actual Python native bytes", "python313.zip": reader.data[stdlib['path']], "LICENSE.txt": b"embed license", "python313._pth": b"original configuration"}
    embed_rec = reader.add("archives/python-3.13.15-embed-amd64.zip", digest=embed_hash, members=embed_members)
    native = reader.add("runtime/python313.dll", embed_members["python313.dll"])
    lock_asset = {"kind": "interpreter", "filename": parent["packageFileName"], "version": "3.13.15", "url": parent["downloadLocation"], "sha256": embed_hash}
    lock = {"python_version": "3.13.15", "assets": [lock_asset, {"name": "CPython source", "version": "3.13.15", "sha256": source_hash, "url": "https://www.python.org/ftp/python/3.13.15/Python-3.13.15.tar.xz"}]}
    lock_rec = reader.add("third_party/cad-runtime-lock.json", json.dumps(lock).encode(), obj=lock)
    wrapper = {"schema": "vertex-cpython-publisher-source-input-receipt-v1", "qualification": {"licensing_clearance": False, "source_rebuild_qualified": False},
               "publisher_metadata": {"original_sbom": sbom_rec, "raw_metadata_files": [sbom_rec]},
               "parent_archive_binding": {"archive": embed_rec, "publisher_package": parent, "lock_asset": lock_asset, "lock_file": lock_rec},
               "cpython_source_archive": source_rec, "source_build_controls": control_rows,
               "vendored_source_metadata": {"receipt": vendor_rec, "packages": vendor_rows},
               "notice_evidence": {"embed_license": reader.add("notices/embed", embed_members["LICENSE.txt"]), "cpython_source_license": reader.add("notices/source", src_members["Python-3.13.15/LICENSE"])},
               "selected_runtime_binding": {"archive_members": [{"member": "python313.zip", "bytes": stdlib["bytes"], "sha256": stdlib["sha256"], "selected_runtime_file": stdlib}]},
               "stdlib_evidence": {"macholib_members": [{"member": n, "sha256": sha(d), "bytes": len(d)} for n,d in sorted(nested.items())], "tkinter_members": []},
               "public_signature_metadata": [], "declared_source_records": declared}
    receipt = reader.add("inputs/cpython.json", json.dumps(wrapper).encode(), obj=wrapper)
    targets = [{"component_id": "cad-cpython", "path": native["path"], "destination": "cad-runtime/python313.dll", "sha256": native["sha256"], "bytes": native["bytes"]}]
    return reader, receipt, targets


class NativeCPythonInputsTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        spec = importlib.util.spec_from_file_location("native_cpython_source_inputs_test", MODULE)
        cls.module = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(cls.module)

    def validate(self, reader, receipt, targets):
        return self.module.validate_inputs(receipt, reader, targets)

    def test_complete_synthetic_reader_and_frozen_resolution(self):
        reader, receipt, targets = fixture(frozen=True)
        result = self.validate(reader, receipt, targets)
        self.assertEqual(len(result["sources"]), 12)
        self.assertEqual(len(result["dependency_dispositions"]), 14)
        self.assertTrue(all(x["local_files"][0]["path"].startswith("frozen/") for x in result["sources"]))
        self.assertIn("incomplete", result["recipe"]["options_role"])
        self.assertEqual(result["target_bindings"][0]["destination"], targets[0]["destination"])
        self.assertTrue(any(x["name"] == "macholib" for x in result["dependency_dispositions"]))

    def test_wrong_embed_refuses(self):
        reader, receipt, targets = fixture(); reader.objects[receipt['path']]["parent_archive_binding"]["archive"]["sha256"] = "0" * 64
        with self.assertRaises(ValueError): self.validate(reader, receipt, targets)

    def test_swapped_original_spdx_version_refuses(self):
        reader, receipt, targets = fixture()
        key = reader.objects[receipt['path']]["publisher_metadata"]["original_sbom"]["path"]
        reader.objects[key]["packages"][0]["versionInfo"] = "3.13.14"
        with self.assertRaises(ValueError): self.validate(reader, receipt, targets)

    def test_replaced_native_member_refuses_against_inventory(self):
        reader, receipt, targets = fixture(); reader.members["archives/python-3.13.15-embed-amd64.zip"]["python313.dll"] = b"substituted"
        with self.assertRaises(ValueError): self.validate(reader, receipt, targets)

    def test_self_rehashed_source_wrapper_refuses_original_graph(self):
        reader, receipt, targets = fixture(); wrapper = reader.objects[receipt['path']]
        source = wrapper['declared_source_records'][0]['source_availability']
        source['local_file'] = reader.add('sources/replacement.tar.gz', b'rehashed replacement')
        with self.assertRaises(ValueError): self.validate(reader, receipt, targets)

    def test_self_rehashed_notice_refuses_original_archive_member(self):
        reader, receipt, targets = fixture(); wrapper = reader.objects[receipt['path']]
        notice = wrapper['declared_source_records'][0]['source_availability']['license_evidence'][0]
        notice['local_file'] = reader.add('notices/replacement', b'rehashed replacement notice')
        with self.assertRaises(ValueError): self.validate(reader, receipt, targets)

    def test_self_rehashed_control_refuses_original_source_member(self):
        reader, receipt, targets = fixture(); wrapper = reader.objects[receipt['path']]
        wrapper['source_build_controls'][0]['local_file'] = reader.add('controls/replacement', b'rehashed replacement control')
        with self.assertRaises(ValueError): self.validate(reader, receipt, targets)

    def test_missing_declared_dependency_refuses(self):
        reader, receipt, targets = fixture(); reader.objects[receipt['path']]['declared_source_records'].pop()
        with self.assertRaises(ValueError): self.validate(reader, receipt, targets)

    def test_false_qualification_boundary_required(self):
        reader, receipt, targets = fixture(); reader.objects[receipt['path']]['qualification']['source_rebuild_qualified'] = True
        with self.assertRaises(ValueError): self.validate(reader, receipt, targets)

    def test_wrong_owner_and_ambiguous_destination_refuse(self):
        for change in [{'component_id':'other'}, {'destination':'other/native.dll'}]:
            reader, receipt, targets = fixture(); targets[0].update(change)
            with self.assertRaises(ValueError): self.validate(reader, receipt, targets)

    def test_frozen_targets_do_not_require_runtime_binary_reads(self):
        reader, receipt, targets = fixture(frozen=True)
        del reader.records[targets[0]['path']]
        result = self.validate(reader, receipt, targets)
        self.assertEqual(result['target_bindings'][0]['path'], targets[0]['path'])
        self.assertTrue(any(r['path'] == 'frozen/runtime/python313.zip' for r in result['artifacts']))
        self.assertFalse(any(path == targets[0]['path'] for role, path in reader.calls))

    def test_replaced_vendored_metadata_or_file_refuses(self):
        for kind in ['metadata', 'file']:
            reader, receipt, targets = fixture(); wrapper = reader.objects[receipt['path']]
            if kind == 'metadata':
                wrapper['vendored_source_metadata']['receipt'] = reader.add('metadata/replacement.json', b'{}', obj={})
            else:
                reader.members['archives/Python-3.13.15.tar.xz']['Python-3.13.15/vendored/expat/0.c'] = b'replaced vendored source'
            with self.assertRaises(ValueError): self.validate(reader, receipt, targets)

    def test_missing_notice_and_duplicate_dependency_refuse(self):
        for kind in ['notice', 'duplicate']:
            reader, receipt, targets = fixture(); wrapper = reader.objects[receipt['path']]
            if kind == 'notice':
                wrapper['declared_source_records'][0]['source_availability']['license_evidence'].clear()
            else:
                wrapper['declared_source_records'][-1] = copy.deepcopy(wrapper['declared_source_records'][0])
            with self.assertRaises(ValueError): self.validate(reader, receipt, targets)

    def test_replaced_stdlib_refuses_embed_binding(self):
        reader, receipt, targets = fixture(); wrapper = reader.objects[receipt['path']]
        wrapper['selected_runtime_binding']['archive_members'][0]['selected_runtime_file'] = reader.add('runtime/replaced.zip', b'changed stdlib')
        with self.assertRaises(ValueError): self.validate(reader, receipt, targets)

    def test_self_rehashed_publisher_metadata_refuses_fixed_original_pin(self):
        reader, receipt, targets = fixture(); wrapper = reader.objects[receipt['path']]
        original = reader.objects[wrapper['publisher_metadata']['original_sbom']['path']]
        wrapper['publisher_metadata']['original_sbom'] = reader.add('metadata/rehashed.json', json.dumps(original).encode(), obj=original)
        with self.assertRaisesRegex(ValueError, 'publisher SPDX pin'): self.validate(reader, receipt, targets)

    def test_replaced_embed_notice_refuses_original_member(self):
        reader, receipt, targets = fixture(); wrapper = reader.objects[receipt['path']]
        wrapper['notice_evidence']['embed_license'] = reader.add('notices/rehashed-embed', b'changed notice')
        with self.assertRaisesRegex(ValueError, 'original archive member'): self.validate(reader, receipt, targets)

    def test_all_original_unresolved_dispositions_remain_unqualified(self):
        reader, receipt, targets = fixture()
        result = self.validate(reader, receipt, targets)
        unresolved = [r for r in result['dependency_dispositions'] if r['relevance'] == 'unresolved_candidate_exclusion']
        self.assertEqual({r['package_id'] for r in unresolved}, {
            'SPDXRef-PACKAGE-tcl-core', 'SPDXRef-PACKAGE-tk', 'SPDXRef-PACKAGE-mpdecimal-2.5.1'})
        self.assertTrue(all(r['candidate_exclusion_qualified'] is False for r in result['dependency_dispositions']))
        decimal = [r['version'] for r in result['dependency_dispositions'] if r['name'] == 'mpdecimal']
        self.assertEqual(set(decimal), {'2.5.1', '4.0.0'})

    def test_provided_signature_trust_claim_refuses(self):
        reader, receipt, targets = fixture(); reader.objects[receipt['path']]['public_signature_metadata'] = [{'cryptographic_signature_verified': True}]
        with self.assertRaisesRegex(ValueError, 'trust flag'): self.validate(reader, receipt, targets)

    def test_duplicate_native_inventory_target_refuses(self):
        reader, receipt, targets = fixture(); targets.append(copy.deepcopy(targets[0]))
        with self.assertRaisesRegex(ValueError, 'Duplicate'): self.validate(reader, receipt, targets)

    def test_malformed_wrapper_refuses_with_value_error(self):
        reader, receipt, targets = fixture(); del reader.objects[receipt['path']]['cpython_source_archive']
        with self.assertRaisesRegex(ValueError, 'Malformed'): self.validate(reader, receipt, targets)


if __name__ == "__main__":
    unittest.main()
