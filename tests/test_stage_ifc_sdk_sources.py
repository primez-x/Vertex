"""Synthetic historical SDK source fixtures; never build or contact upstream."""
import hashlib
import importlib.util
import json
import os
import pathlib
import subprocess
import tempfile
import unittest
from unittest.mock import patch

ROOT = pathlib.Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location(
    "stage_ifc_sdk_sources", ROOT / "scripts/stage_ifc_sdk_sources.py")
sdk = importlib.util.module_from_spec(spec)
spec.loader.exec_module(sdk)


def sha(data, algorithm="sha256"):
    return hashlib.new(algorithm, data).hexdigest()


class IfcSdkSourceTests(unittest.TestCase):
    def git(self, root, *args, data=None):
        options = {"creationflags": subprocess.CREATE_NO_WINDOW} if os.name == "nt" else {}
        result = subprocess.run(["git", "-C", str(root), *args], input=data,
                                stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                                check=True, **options)
        return result.stdout.strip().decode()

    def put(self, root, name, data):
        path = root / name
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(data)
        return path

    def fixture(self, parent, fallback=False):
        workspace = parent / "workspace"
        manager = workspace / ".deps/vcpkg"
        manager.mkdir(parents=True)
        self.git(manager, "init", "--quiet")
        self.git(manager, "config", "remote.origin.url", "https://github.com/microsoft/vcpkg.git")
        recipe = {"portfile.cmake": b"# historical recipe\n",
                  "vcpkg.json": (b'{"name":"boost-uninstall","license":"MIT"}\n' if fallback
                                 else b'{"name":"fixture"}\n')}
        tree_lines = []
        for name, data in sorted(recipe.items()):
            blob = self.git(manager, "hash-object", "-w", "--stdin", data=data)
            tree_lines.append(f"100644 blob {blob}\t{name}\n")
        tree = self.git(manager, "mktree", data="".join(tree_lines).encode())
        license_blob = self.git(manager, "hash-object", "-w", "--stdin",
                                data=b"MIT License\nCopyright Microsoft Corporation\nfixture full notice\n")
        manager_tree = self.git(manager, "mktree", data=("".join(tree_lines) +
                                f"100644 blob {license_blob}\tLICENSE.txt\n").encode())
        revision = self.git(manager, "-c", "user.name=Fixture", "-c", "user.email=fixture@example.invalid",
                            "commit-tree", manager_tree, data=b"fixture manager\n")
        self.git(manager, "update-ref", "HEAD", revision)
        self.put(manager, "ports/opencascade/portfile.cmake", b"# unrelated current recipe\n")
        archive = b"opaque archive bytes; no extraction or source execution\n"
        self.put(manager, "downloads/arbitrary-cache-name.bin", archive)
        # Identical cache bytes must produce one canonical staged resource.
        self.put(manager, "downloads/duplicate-cache-name.bin", archive)
        resource = {"SPDXID": "SPDXRef-resource-0", "name": "fixture/source",
                    "downloadLocation": "git+https://example.invalid/source@v1",
                    "checksums": [{"algorithm": "SHA512", "checksumValue": sha(archive, "sha512")}]}
        for label, name, version, port_version in (("kernel", "opencascade", "7.8.1", 1),
                                                  ("support", "boost-uninstall" if fallback else "boost-locale", "1.86.0", 0)):
            sdkroot = workspace / f".deps/ifc-{label}"
            paragraphs = []
            for package, architecture, package_version, dependencies in (
                    ("fixture-tool", "x64-windows", "2024-01-01", ""),
                    (name, "x64-windows-ifc-static", version, "fixture-tool (x64-windows)")):
                abi = sha(f"{label}/{package}".encode())
                pv = port_version if package == name else 0
                status = (f"Package: {package}\nVersion: {package_version}\nPort-Version: {pv}\n"
                          f"Architecture: {architecture}\nMulti-Arch: same\nAbi: {abi}\n"
                          "Status: install ok installed\n")
                if dependencies:
                    status += f"Depends: {dependencies}\n"
                paragraphs.append(status)
                share = sdkroot / architecture / "share" / package
                share.mkdir(parents=True)
                binary_id = f"{package}:{architecture}"
                receipt = {"spdxVersion": "SPDX-2.3", "SPDXID": "SPDXRef-DOCUMENT",
                           "packages": [
                               {"SPDXID": "SPDXRef-port", "name": package,
                                "versionInfo": package_version + (f"#{pv}" if pv else ""),
                                "downloadLocation": f"git+https://github.com/Microsoft/vcpkg@{tree}"},
                               {"SPDXID": "SPDXRef-binary", "name": binary_id,
                                "versionInfo": abi, "downloadLocation": "NONE"}],
                           "files": [{"SPDXID": f"SPDXRef-port-file-{i}", "fileName": "./" + p,
                                      "checksums": [{"algorithm": "SHA256", "checksumValue": sha(d)}]}
                                     for i, (p, d) in enumerate(sorted(recipe.items()))]}
                if fallback and package == "boost-uninstall":
                    for item in receipt["packages"]:
                        item["licenseConcluded"] = "MIT"
                    receipt["files"].extend([
                        {"SPDXID": "SPDXRef-binary-file-0", "fileName": "./BUILD_INFO",
                         "checksums": [{"algorithm": "SHA256", "checksumValue": sha(b"fixture build info")}]},
                        {"SPDXID": "SPDXRef-binary-file-1", "fileName": "./share/boost/vcpkg-cmake-wrapper.cmake",
                         "checksums": [{"algorithm": "SHA256", "checksumValue": sha(b"fixture wrapper")}]},
                    ])
                if package == name and not (fallback and package == "boost-uninstall"):
                    receipt["packages"].append(resource)
                    self.put(share, "vcpkg-spdx-resources.json", json.dumps({"packages": [resource]}).encode())
                self.put(share, "vcpkg.spdx.json", json.dumps(receipt).encode())
                dependency_abi = (f"fixture-tool {sha(f'{label}/fixture-tool'.encode())}\n".encode()
                                  if dependencies else b"")
                self.put(share, "vcpkg_abi_info.txt", b"features core\n" + dependency_abi +
                         b"".join(f"{p} {sha(d)}\n".encode() for p, d in sorted(recipe.items())))
                if not (fallback and package == "boost-uninstall"):
                    self.put(share, "copyright", b"fixture upstream and port notices\n")
            self.put(sdkroot, "vcpkg/status", "\n".join(paragraphs).encode())
            manifest = {"name": f"fixture-{label}", "version-string": "0",
                        "builtin-baseline": revision, "dependencies": [name],
                        "overrides": [{"name": name, "version": version, "port-version": port_version}]}
            rel = "third_party/ifc-source/vcpkg.json" if label == "kernel" else "third_party/ifc-source/support/vcpkg.json"
            self.put(workspace, rel, json.dumps(manifest).encode())
        self.put(workspace, "third_party/ifc-source/triplets/x64-windows-ifc-static.cmake",
                 b"set(VCPKG_TARGET_ARCHITECTURE x64)\nset(VCPKG_CRT_LINKAGE dynamic)\n"
                 b"set(VCPKG_LIBRARY_LINKAGE static)\nset(VCPKG_BUILD_TYPE release)\n")
        self.put(workspace, "third_party/ifc-source-lock.json", json.dumps({
            "schema_version": 1, "build_qualified": False, "source_closure_qualified": False,
            "swig": {"hash_provenance": {"repository": "https://github.com/microsoft/vcpkg.git",
                                         "revision": revision}}}).encode())
        external = parent / "handoff"
        external.mkdir()
        return workspace, external / "sdk-sources", tree, recipe, archive

    def receipt(self, workspace):
        return workspace / ".deps/ifc-kernel/x64-windows-ifc-static/share/opencascade/vcpkg.spdx.json"

    def alter_receipt(self, workspace, transform):
        path = self.receipt(workspace)
        value = json.loads(path.read_bytes())
        transform(value)
        path.write_bytes(json.dumps(value).encode())

    def test_historical_sources_receipts_dedup_and_relative_manifest(self):
        with tempfile.TemporaryDirectory() as directory:
            workspace, output, tree, recipe, archive = self.fixture(pathlib.Path(directory))
            result = sdk.stage(workspace, output)
            self.assertEqual(json.loads((output / "ifc-sdk-source-manifest.json").read_bytes()), result)
            self.assertEqual(len(result["packages"]), 4)
            self.assertEqual(len(result["resources"]), 1)
            self.assertFalse(result["source_closure_qualified"])
            self.assertFalse(result["build_qualified"])
            self.assertFalse(result["product_runtime_replaced"])
            encoded = json.dumps(result)
            self.assertNotIn(str(workspace), encoded)
            self.assertNotIn(str(output), encoded)
            self.assertEqual((output / result["resources"][0]["path"]).read_bytes(), archive)
            for package in result["packages"]:
                self.assertEqual(package["recipe"]["git_tree"], tree)
                for name, data in recipe.items():
                    self.assertEqual((output / package["recipe"]["path"] / name).read_bytes(), data)
                self.assertTrue(package["notices"])
                self.assertTrue(package["receipts"])
            self.assertEqual(result, sdk.verify(output))

    def test_invalid_inputs_fail_before_reservation(self):
        for fault in ("archive", "porthash", "abi", "identity", "missingdep", "missingtree", "unsafepath"):
            with self.subTest(fault=fault), tempfile.TemporaryDirectory() as directory:
                workspace, output, tree, recipe, archive = self.fixture(pathlib.Path(directory))
                if fault == "archive":
                    for path in (workspace / ".deps/vcpkg/downloads").iterdir():
                        path.write_bytes(b"altered bytes")
                elif fault == "porthash":
                    self.alter_receipt(workspace, lambda j: j["files"][0]["checksums"][0].update(checksumValue="0" * 64))
                elif fault == "abi":
                    self.alter_receipt(workspace, lambda j: j["packages"][1].update(versionInfo="0" * 64))
                elif fault == "identity":
                    self.alter_receipt(workspace, lambda j: j["packages"][0].update(name="foreign"))
                elif fault == "missingdep":
                    status = workspace / ".deps/ifc-kernel/vcpkg/status"
                    status.write_bytes(status.read_bytes().replace(b"fixture-tool (x64-windows)", b"absent (x64-windows)"))
                elif fault == "missingtree":
                    self.alter_receipt(workspace, lambda j: j["packages"][0].update(downloadLocation="git+https://github.com/Microsoft/vcpkg@" + "0" * 40))
                else:
                    self.alter_receipt(workspace, lambda j: j["files"][0].update(fileName="./../escape"))
                with self.assertRaises(ValueError):
                    sdk.stage(workspace, output)
                self.assertFalse(output.exists())

    def test_existing_output_and_workspace_overlap(self):
        for fault in ("existing", "overlap"):
            with self.subTest(fault=fault), tempfile.TemporaryDirectory() as directory:
                workspace, output, *_ = self.fixture(pathlib.Path(directory))
                if fault == "existing":
                    output.mkdir()
                    (output / "foreign").write_bytes(b"preserve")
                else:
                    output = workspace / "new-source-output"
                with self.assertRaises(ValueError):
                    sdk.stage(workspace, output)
                if fault == "existing":
                    self.assertEqual((output / "foreign").read_bytes(), b"preserve")

    def test_linked_output_parent_rejected(self):
        with tempfile.TemporaryDirectory() as directory:
            parent = pathlib.Path(directory)
            workspace, output, *_ = self.fixture(parent)
            link = parent / "linked"
            try:
                link.symlink_to(output.parent, target_is_directory=True)
            except OSError:
                self.skipTest("Symlink creation unavailable")
            with self.assertRaises(ValueError):
                sdk.stage(workspace, link / "sdk-sources")
            self.assertFalse(output.exists())

    def test_copy_failure_retains_partial_without_manifest(self):
        with tempfile.TemporaryDirectory() as directory:
            workspace, output, *_ = self.fixture(pathlib.Path(directory))
            original = sdk.write_exclusive
            calls = 0
            def fail_later(path, data):
                nonlocal calls
                calls += 1
                if calls == 3:
                    raise OSError("fixture copy failure")
                return original(path, data)
            with patch.object(sdk, "write_exclusive", side_effect=fail_later):
                with self.assertRaises((OSError, ValueError)):
                    sdk.stage(workspace, output)
            self.assertTrue(output.is_dir())
            self.assertTrue(any(output.rglob("*")))
            self.assertFalse((output / "ifc-sdk-source-manifest.json").exists())

    def test_output_and_input_rechecks(self):
        with tempfile.TemporaryDirectory() as directory:
            workspace, output, *_ = self.fixture(pathlib.Path(directory))
            original = sdk.write_exclusive
            changed = False
            def change_input(path, data):
                nonlocal changed
                result = original(path, data)
                if not changed:
                    changed = True
                    (workspace / ".deps/vcpkg/downloads/arbitrary-cache-name.bin").write_bytes(b"changed during copy")
                return result
            with patch.object(sdk, "write_exclusive", side_effect=change_input):
                with self.assertRaises(ValueError):
                    sdk.stage(workspace, output)
            self.assertTrue(output.exists())
            self.assertFalse((output / "ifc-sdk-source-manifest.json").exists())
        with tempfile.TemporaryDirectory() as directory:
            workspace, output, *_ = self.fixture(pathlib.Path(directory))
            sdk.stage(workspace, output)
            (output / "foreign.txt").write_bytes(b"unexpected")
            with self.assertRaises(ValueError):
                sdk.verify(output)

    def test_narrow_manager_notice_fallback(self):
        with tempfile.TemporaryDirectory() as directory:
            workspace, output, *_ = self.fixture(pathlib.Path(directory), fallback=True)
            result = sdk.stage(workspace, output)
            package = next(p for p in result["packages"] if p["name"] == "boost-uninstall")
            self.assertEqual(package["notice_origin"]["kind"], "locked-manager-license")
            self.assertEqual(package["notices"], [result["manager"]["license"]["path"]])
            self.assertFalse(package["resources"])
            self.assertEqual(result, sdk.verify(output))
        with tempfile.TemporaryDirectory() as directory:
            workspace, output, *_ = self.fixture(pathlib.Path(directory))
            (self.receipt(workspace).parent / "copyright").unlink()
            with self.assertRaises(ValueError):
                sdk.stage(workspace, output)
            self.assertFalse(output.exists())

    def test_known_uninstall_payload_requires_exact_valid_records(self):
        records = [
            {"SPDXID": "SPDXRef-binary-file-0", "fileName": "./BUILD_INFO",
             "checksums": [{"algorithm": "SHA256", "checksumValue": sha(b"build info")}]},
            {"SPDXID": "SPDXRef-binary-file-1", "fileName": "./share/boost/vcpkg-cmake-wrapper.cmake",
             "checksums": [{"algorithm": "SHA256", "checksumValue": sha(b"wrapper")}]},
        ]
        self.assertTrue(sdk.known_uninstall_payload({"files": records}))
        invalid = [[], records[:1], [records[0], records[0]],
                   records + [dict(records[0], SPDXID="SPDXRef-binary-file-2", fileName="./unreviewed.dll")],
                   [records[0], dict(records[1], SPDXID=records[0]["SPDXID"])],
                   [records[0], dict(records[1], checksums=[{"algorithm": "SHA256", "checksumValue": "invalid"}])]]
        for entries in invalid:
            with self.subTest(entries=entries):
                self.assertFalse(sdk.known_uninstall_payload({"files": entries}))

    def test_workspace_git_executable_is_never_launched(self):
        with tempfile.TemporaryDirectory() as directory:
            workspace, output, *_ = self.fixture(pathlib.Path(directory))
            executable = self.put(workspace, "untrusted/git.exe", b"never execute")
            with patch.object(sdk.shutil, "which", return_value=str(executable)), \
                    patch.object(sdk.subprocess, "Popen", side_effect=AssertionError("workspace executable launched")):
                with self.assertRaises(ValueError):
                    sdk.stage(workspace, output)
            self.assertFalse(output.exists())

    def test_manifest_limit_before_reservation(self):
        with tempfile.TemporaryDirectory() as directory:
            workspace, output, *_ = self.fixture(pathlib.Path(directory))
            with patch.object(sdk, "MAX_MANIFEST_BYTES", 128):
                with self.assertRaisesRegex(ValueError, "Manifest byte limit"):
                    sdk.stage(workspace, output)
            self.assertFalse(output.exists())

    def test_portable_windows_reserved_git_paths(self):
        for name in ("COM¹.txt", "LPT²", "CONIN$", "CONOUT$"):
            with self.subTest(name=name), tempfile.TemporaryDirectory() as directory:
                workspace, output, *_ = self.fixture(pathlib.Path(directory))
                manager = workspace / ".deps/vcpkg"
                blob = self.git(manager, "hash-object", "-w", "--stdin", data=b"unsafe")
                tree = self.git(manager, "mktree", data=f"100644 blob {blob}\t{name}\n".encode())
                def change(j):
                    j["packages"][0]["downloadLocation"] = "git+https://github.com/Microsoft/vcpkg@" + tree
                    j["files"] = [{"SPDXID": "SPDXRef-port-file-0", "fileName": "./" + name,
                                   "checksums": [{"algorithm": "SHA256", "checksumValue": sha(b"unsafe")}]}]
                self.alter_receipt(workspace, change)
                with self.assertRaises(ValueError):
                    sdk.stage(workspace, output)
                self.assertFalse(output.exists())

    def test_metadata_only_manifest_tampering(self):
        with tempfile.TemporaryDirectory() as directory:
            workspace, output, *_ = self.fixture(pathlib.Path(directory))
            result = sdk.stage(workspace, output)
            manifest = output / "ifc-sdk-source-manifest.json"
            for fault in ("manager_source", "manager_rebuild", "managerrevision", "managerblob", "scope", "inputs",
                          "packages", "resourcepath", "resourcehash",
                          "recipepath", "noticepath", "receiptpath", "packageabi"):
                with self.subTest(fault=fault):
                    value = json.loads(json.dumps(result))
                    if fault == "manager_source":
                        value["manager"]["manager_source_included"] = True
                    elif fault == "manager_rebuild":
                        value["manager"]["offline_sdk_rebuild_qualified"] = True
                    elif fault == "managerrevision":
                        value["manager"]["revision"] = "0" * 40
                    elif fault == "managerblob":
                        value["manager"]["license"]["git_blob"] = "0" * 40
                    elif fault == "scope":
                        value["scope"] = "complete-product-source"
                    elif fault == "inputs":
                        value["inputs"] = []
                    elif fault == "packages":
                        value["packages"] = []
                    elif fault == "resourcepath":
                        value["resources"][0]["path"] = "foreign.archive"
                    elif fault == "resourcehash":
                        value["resources"][0]["sha512"] = "0" * 128
                    elif fault == "recipepath":
                        value["packages"][0]["recipe"]["path"] = "foreign"
                    elif fault == "noticepath":
                        value["packages"][0]["notices"] = ["foreign"]
                    elif fault == "receiptpath":
                        value["packages"][0]["receipts"] = ["foreign"]
                    else:
                        value["packages"][0]["abi"] = "0" * 64
                    manifest.write_bytes(json.dumps(value).encode())
                    with self.assertRaises(ValueError):
                        sdk.verify(output)


if __name__ == "__main__":
    unittest.main()
