import hashlib
import importlib.util
import json
import pathlib
import stat
import subprocess
import tempfile
import unittest
import zipfile
from types import SimpleNamespace
from unittest import mock


ROOT = pathlib.Path(__file__).resolve().parents[1]
SPEC = importlib.util.spec_from_file_location(
    "distribution_inventory", ROOT / "scripts" / "distribution_inventory.py"
)
inventory = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(inventory)


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def write_json(path, value):
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(value), encoding="utf-8")


class DistributionInventoryTests(unittest.TestCase):
    def bundled_asset_fixture(self):
        directory, root, manifest, runtime, app, dependency = self.fixture()
        self.addCleanup(directory.cleanup)
        model = root / "assets/model.traineddata"
        model.parent.mkdir()
        model.write_bytes(b"official model bytes")
        asset = {"name": "OCR model", "version": "4.1.0", "license": "Apache-2.0",
                 "path": "assets/model.traineddata", "sha256": digest(model),
                 "source": "https://example.invalid/models/pinned-commit"}
        write_json(root / "third_party/dependencies.json", {"bundled_assets": [asset]})
        component = {"id": "ocr-model", "kind": "asset",
                     "package": {"name": asset["name"], "version": asset["version"], "license": asset["license"]},
                     "source": {"kind": "bootstrap-dependency", "dependencies_path": "third_party/dependencies.json",
                                "asset_name": asset["name"], "paths": [asset["path"]]},
                     "notice_paths": ["third_party/NOTICE.txt"]}
        manifest["components"].append(component)
        return root, manifest, model, asset

    def test_bundled_asset_binds_declared_path_and_bytes(self):
        root, manifest, model, asset = self.bundled_asset_fixture()
        result = inventory.build_inventory(root, manifest)
        row = next(item for item in result["components"] if item["id"] == "ocr-model")
        self.assertEqual(row["package"]["source"]["asset_path"], asset["path"])
        self.assertEqual(row["package"]["source"]["declared_sha256"], digest(model))
        self.assertEqual(row["source_inputs"], [{"path": asset["path"], "kind": "file", "sha256": digest(model)}])

    def test_bundled_asset_rejects_tamper_or_substituted_path(self):
        for mutation in ("bytes", "path"):
            with self.subTest(mutation=mutation):
                root, manifest, model, asset = self.bundled_asset_fixture()
                if mutation == "bytes":
                    model.write_bytes(b"substituted model")
                else:
                    manifest["components"][-1]["source"]["paths"] = ["third_party/NOTICE.txt"]
                with self.assertRaises(inventory.InventoryError):
                    inventory.build_inventory(root, manifest)

    def test_tree_fingerprint_ignores_generated_python_caches(self):
        with tempfile.TemporaryDirectory() as directory:
            source = pathlib.Path(directory)
            module = source / "adapter.py"
            module.write_bytes(b"value = 1\n")
            original = inventory._tree_sha256(source)

            cache = source / "desktop" / "__pycache__"
            cache.mkdir(parents=True)
            generated = [cache / "adapter.cpython-313.pyc", cache / "cache-metadata",
                         source / "legacy.pyc", source / "optimized.pyo"]
            for path in generated:
                path.write_bytes(b"generated cache")
            self.assertEqual(inventory._tree_sha256(source), original)
            for path in generated:
                path.write_bytes(b"changed generated cache")
            self.assertEqual(inventory._tree_sha256(source), original)
            for path in generated:
                path.unlink()
            cache.rmdir()
            self.assertEqual(inventory._tree_sha256(source), original)

            module.write_bytes(b"value = 2\n")
            self.assertNotEqual(inventory._tree_sha256(source), original)
            module.write_bytes(b"value = 1\n")
            module.rename(source / "renamed.py")
            self.assertNotEqual(inventory._tree_sha256(source), original)

    def test_tree_fingerprint_keeps_other_artifacts_and_deterministic_order(self):
        with tempfile.TemporaryDirectory() as directory:
            source = pathlib.Path(directory)
            for name in ("z.py", "a.py", "adapter.pyd", "cache.pyc.txt"):
                (source / name).write_bytes(name.encode("utf-8"))
            original = inventory._tree_sha256(source)
            children = list(source.rglob("*"))
            with mock.patch.object(pathlib.Path, "rglob", return_value=iter(reversed(children))):
                self.assertEqual(inventory._tree_sha256(source), original)
            for name in ("adapter.pyd", "cache.pyc.txt"):
                with self.subTest(artifact=name):
                    path = source / name
                    path.write_bytes(b"changed non-cache artifact")
                    self.assertNotEqual(inventory._tree_sha256(source), original)
                    path.write_bytes(name.encode("utf-8"))

    def test_tree_fingerprint_rejects_symlinks_even_at_excluded_cache_paths(self):
        with tempfile.TemporaryDirectory() as directory:
            source = pathlib.Path(directory)
            cache = source / "__pycache__"
            cache.mkdir()
            bytecode = cache / "adapter.cpython-313.pyc"
            bytecode.write_bytes(b"generated cache")
            original = pathlib.Path.is_symlink
            for target in (cache, bytecode):
                with self.subTest(target=target.name):
                    def is_symlink(path):
                        return path == target or original(path)
                    with mock.patch.object(pathlib.Path, "is_symlink", is_symlink):
                        with self.assertRaisesRegex(inventory.InventoryError, "Symlink"):
                            inventory._tree_sha256(source)

    def fixture(self):
        directory = tempfile.TemporaryDirectory()
        root = pathlib.Path(directory.name)
        app = root / "build" / "vertex.exe"
        dependency = root / "build" / "dependency.dll"
        source = root / "src" / "dependency.h"
        notice = root / "third_party" / "NOTICE.txt"
        for path, contents in (
            (app, b"application"),
            (dependency, b"dependency"),
            (source, b"header"),
            (notice, b"notice"),
        ):
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_bytes(contents)
        runtime = root / "evidence" / "runtime.json"
        write_json(
            runtime,
            {
                "recorded_utc": "2026-09-11T00:00:00Z",
                "entry_points": [str(app),],
                "unresolved": [],
                "modules": [
                    {
                        "path": str(app),
                        "sha256": digest(app),
                        "imports": [
                            {
                                "name": "dependency.dll",
                                "kind": "local-component",
                                "resolved": str(dependency),
                                "candidates": [str(dependency)],
                            },
                            {"name": "KERNEL32.dll", "kind": "installed-system-runtime"},
                            {"name": "api-ms-win-core.dll", "kind": "windows-api-contract"},
                        ],
                    },
                    {"path": str(dependency), "sha256": digest(dependency), "imports": []},
                ],
            },
        )
        manifest = {
            "schema_version": 1,
            "audit_status": "incomplete",
            "runtime_evidence": {"path": "evidence/runtime.json"},
            "components": [
                {
                    "id": "vertex",
                    "kind": "application",
                    "package": {
                        "name": "Vertex",
                        "version": "workspace",
                        "license": "Proprietary",
                    },
                    "source": {"kind": "workspace", "paths": ["src"]},
                    "notice_paths": ["third_party/NOTICE.txt"],
                    "runtime_names": ["vertex.exe"],
                    "destinations": {"vertex.exe": "bin/vertex.exe"},
                },
                {
                    "id": "dependency",
                    "kind": "runtime",
                    "package": {"name": "Dependency", "version": "1.0", "license": "MIT"},
                    "source": {"kind": "workspace", "paths": ["src/dependency.h"]},
                    "notice_paths": ["third_party/NOTICE.txt"],
                    "runtime_names": ["dependency.dll"],
                    "destinations": {"dependency.dll": "bin/dependency.dll"},
                },
            ],
        }
        return directory, root, manifest, runtime, app, dependency

    def test_build_inventory_binds_runtime_hashes_and_system_boundary(self):
        directory, root, manifest, runtime, app, dependency = self.fixture()
        self.addCleanup(directory.cleanup)

        result = inventory.build_inventory(root, manifest)

        self.assertEqual(result["schema_version"], 1)
        self.assertEqual(result["audit_status"], "incomplete")
        binaries = {row["name"]: row for row in result["binaries"]}
        self.assertEqual(binaries["vertex.exe"]["sha256"], digest(app))
        self.assertEqual(binaries["dependency.dll"]["sha256"], digest(dependency))
        self.assertEqual(binaries["dependency.dll"]["destination"], "bin/dependency.dll")
        self.assertEqual(result["system_runtime_imports"], ["KERNEL32.dll"])
        self.assertEqual(result["windows_api_contracts"], ["api-ms-win-core.dll"])
        self.assertEqual(result["evidence"]["runtime"]["path"], "evidence/runtime.json")
        self.assertNotIn("\\", json.dumps(result))
        self.assertNotIn(str(root), json.dumps(result))

    def test_redistributable_requires_reviewed_hashes(self):
        directory, root, manifest, runtime, app, dependency = self.fixture()
        self.addCleanup(directory.cleanup)
        source = manifest["components"][1]["source"]
        source.update(kind="redistributable", sha256={"dependency.dll": digest(dependency)})
        result = inventory.build_inventory(root, manifest)
        component = next(row for row in result["components"] if row["id"] == "dependency")
        self.assertEqual(component["package"]["source"]["kind"], "redistributable")
        self.assertFalse(component["package"]["source"]["licensing_clearance"])
        source["sha256"]["dependency.dll"] = "0" * 64
        with self.assertRaises(inventory.InventoryError):
            inventory.build_inventory(root, manifest)
        source["sha256"] = {}
        with self.assertRaises(inventory.InventoryError):
            inventory.build_inventory(root, manifest)

    def test_stale_runtime_hash_fails_closed(self):
        directory, root, manifest, runtime, app, dependency = self.fixture()
        self.addCleanup(directory.cleanup)
        evidence = json.loads(runtime.read_text(encoding="utf-8"))
        evidence["modules"][0]["sha256"] = "0" * 64
        write_json(runtime, evidence)

        with self.assertRaises(inventory.InventoryError) as context:
            inventory.build_inventory(root, manifest)
        self.assertIn("hash", str(context.exception).lower())

    def test_unknown_local_import_fails_closed(self):
        directory, root, manifest, runtime, app, dependency = self.fixture()
        self.addCleanup(directory.cleanup)
        evidence = json.loads(runtime.read_text(encoding="utf-8"))
        evidence["modules"][0]["imports"][0]["name"] = "unowned.dll"
        write_json(runtime, evidence)

        with self.assertRaises(inventory.InventoryError) as context:
            inventory.build_inventory(root, manifest)
        self.assertIn("ownership", str(context.exception).lower())

    def test_duplicate_destinations_are_rejected(self):
        directory, root, manifest, runtime, app, dependency = self.fixture()
        self.addCleanup(directory.cleanup)
        manifest["components"][1]["destinations"]["dependency.dll"] = "bin/vertex.exe"

        with self.assertRaises(inventory.InventoryError) as context:
            inventory.build_inventory(root, manifest)
        self.assertIn("destination", str(context.exception).lower())

    def test_missing_runtime_evidence_is_rejected(self):
        directory, root, manifest, runtime, app, dependency = self.fixture()
        self.addCleanup(directory.cleanup)
        manifest["runtime_evidence"]["path"] = "evidence/missing.json"

        with self.assertRaises(inventory.InventoryError) as context:
            inventory.build_inventory(root, manifest)
        self.assertIn("evidence", str(context.exception).lower())

    def test_malformed_manifest_fails_closed(self):
        directory, root, manifest, runtime, app, dependency = self.fixture()
        self.addCleanup(directory.cleanup)
        manifest["components"][0]["package"]["license"] = None

        with self.assertRaises(inventory.InventoryError) as context:
            inventory.build_inventory(root, manifest)
        self.assertIn("license", str(context.exception).lower())

    def test_nested_declared_source_hash_is_checked(self):
        directory, root, manifest, runtime, app, dependency = self.fixture()
        self.addCleanup(directory.cleanup)
        manifest["components"][1]["source"] = {"kind": "workspace", "paths": ["src"]}
        manifest["components"][1]["source_files"] = [
            {"path": "src/dependency.h", "sha256": "0" * 64}
        ]

        with self.assertRaises(inventory.InventoryError) as context:
            inventory.build_inventory(root, manifest)
        self.assertIn("stale source hash", str(context.exception).lower())

    def test_spdx_hash_override_requires_pinned_provenance(self):
        with self.assertRaises(inventory.InventoryError) as context:
            inventory._validate_spdx_hash_overrides(
                {"plugins/qwindows.dll": {
                    "algorithm": "SHA1",
                    "value": "0" * 40,
                }},
                "component qtbase.source.hash_overrides",
            )
        self.assertIn("unsupported fields", str(context.exception).lower())

    def test_spdx_hash_override_hashes_the_pinned_archive_member(self):
        directory = tempfile.TemporaryDirectory()
        self.addCleanup(directory.cleanup)
        root = pathlib.Path(directory.name)
        archive = root / "qt.7z"
        tool = root / "7z.exe"
        archive.write_bytes(b"pinned archive")
        tool.write_bytes(b"tool")
        override = {
            "algorithm": "SHA1",
            "value": hashlib.sha1(b"member bytes").hexdigest(),
            "archive_path": "qt.7z",
            "archive_sha256": digest(archive),
            "archive_member": "plugins/qwindows.dll",
            "tool_path": "7z.exe",
            "reason": "fixture",
        }
        completed = subprocess.CompletedProcess(
            [str(tool)], 0, stdout=b"member bytes", stderr=b"")
        with mock.patch.object(inventory.subprocess, "run", return_value=completed) as run:
            actual = inventory._archive_member_digest(root, override, "qtbase")
        self.assertEqual(actual, override["value"])
        run.assert_called_once()
        self.assertIn("plugins/qwindows.dll", run.call_args.args[0])

    def test_vcpkg_spdx_ownership_and_notice_are_recorded(self):
        directory, root, manifest, runtime, app, dependency = self.fixture()
        self.addCleanup(directory.cleanup)
        spdx = root / "deps" / "share" / "thing" / "vcpkg.spdx.json"
        write_json(
            spdx,
            {
                "spdxVersion": "SPDX-2.3",
                "SPDXID": "SPDXRef-DOCUMENT",
                "documentNamespace": "https://example.invalid/spdx/thing",
                "packages": [
                    {
                        "SPDXID": "SPDXRef-port",
                        "name": "thing",
                        "versionInfo": "2.4.1",
                        "downloadLocation": "git+https://example.invalid/thing@abc123",
                        "licenseConcluded": "MIT",
                    }
                ],
                "files": [
                    {
                        "SPDXID": "SPDXRef-binary",
                        "fileName": "./bin/dependency.dll",
                        "checksums": [
                            {"algorithm": "SHA256", "checksumValue": digest(dependency)}
                        ],
                    }
                ],
                "relationships": [
                    {
                        "spdxElementId": "SPDXRef-port",
                        "relatedSpdxElement": "SPDXRef-binary",
                        "relationshipType": "CONTAINS",
                    }
                ],
            },
        )
        manifest["components"][1]["package"].update(name="thing", version="2.4.1")
        manifest["components"][1]["source"] = {
            "kind": "vcpkg-spdx",
            "spdx_path": "deps/share/thing/vcpkg.spdx.json",
            "package_id": "SPDXRef-port",
        }

        result = inventory.build_inventory(root, manifest)

        row = next(item for item in result["binaries"] if item["name"] == "dependency.dll")
        self.assertEqual(row["package"]["name"], "thing")
        self.assertEqual(row["package"]["version"], "2.4.1")
        self.assertEqual(row["package"]["source"]["url"], "git+https://example.invalid/thing@abc123")
        self.assertEqual(row["evidence"]["spdx_path"], "deps/share/thing/vcpkg.spdx.json")
        self.assertEqual(row["package"]["source"]["license_concluded"], "MIT")

        # An unknown upstream conclusion is retained, never converted into clearance.
        doc = json.loads(spdx.read_text(encoding="utf-8"))
        doc["packages"][0]["licenseConcluded"] = "LicenseRef-vcpkg-null"
        write_json(spdx, doc)
        manifest["components"][1]["package"]["license"] = "LicenseRef-vcpkg-null"
        result = inventory.build_inventory(root, manifest)
        row = next(item for item in result["components"] if item["id"] == "dependency")
        self.assertEqual(row["package"]["source"]["license_concluded"], "LicenseRef-vcpkg-null")
        self.assertFalse(result["distribution_qualified"])
        component = manifest["components"][1]
        notice = root / "third_party/NOTICE.txt"
        component["artifacts"] = [{"path": "deps/share/thing/vcpkg.spdx.json", "sha256": digest(spdx)},
                                  {"path": "third_party/NOTICE.txt", "sha256": digest(notice)}]
        inventory.build_inventory(root, manifest)
        original_notice = notice.read_bytes()
        notice.write_bytes(b"substituted copyright evidence")
        with self.assertRaisesRegex(inventory.InventoryError, "hash"):
            inventory.build_inventory(root, manifest)
        notice.write_bytes(original_notice)
        original_spdx = spdx.read_bytes()
        doc["documentNamespace"] = "https://example.invalid/substituted-evidence"
        write_json(spdx, doc)
        with self.assertRaisesRegex(inventory.InventoryError, "hash"):
            inventory.build_inventory(root, manifest)
        spdx.write_bytes(original_spdx)
        manifest["components"][1]["package"]["license"] = "BSD-2-Clause"
        with self.assertRaisesRegex(inventory.InventoryError, "license mismatch"):
            inventory.build_inventory(root, manifest)

    def test_spdx_prefix_is_resolved_from_repository_root(self):
        directory, root, manifest, runtime, app, dependency = self.fixture()
        self.addCleanup(directory.cleanup)
        spdx = root / "deps" / "share" / "thing" / "vcpkg.spdx.json"
        write_json(
            spdx,
            {
                "spdxVersion": "SPDX-2.3",
                "SPDXID": "SPDXRef-DOCUMENT",
                "documentNamespace": "https://example.invalid/spdx/thing",
                "packages": [
                    {
                        "SPDXID": "SPDXRef-port",
                        "name": "thing",
                        "versionInfo": "2.4.1",
                        "downloadLocation": "git+https://example.invalid/thing@abc123",
                        "licenseConcluded": "MIT",
                    }
                ],
                "files": [
                    {
                        "SPDXID": "SPDXRef-binary",
                        "fileName": "./dependency.dll",
                        "checksums": [
                            {"algorithm": "SHA256", "checksumValue": digest(dependency)}
                        ],
                    }
                ],
                "relationships": [
                    {
                        "spdxElementId": "SPDXRef-port",
                        "relatedSpdxElement": "SPDXRef-binary",
                        "relationshipType": "CONTAINS",
                    }
                ],
            },
        )
        manifest["components"][1]["package"].update(name="thing", version="2.4.1")
        manifest["components"][1]["source"] = {
            "kind": "vcpkg-spdx",
            "spdx_path": "deps/share/thing/vcpkg.spdx.json",
            "prefix_path": "build",
            "package_id": "SPDXRef-port",
        }

        result = inventory.build_inventory(root, manifest)

        row = next(item for item in result["binaries"] if item["name"] == "dependency.dll")
        self.assertEqual(row["evidence"]["spdx_file"]["file_name"], "./dependency.dll")

    def locked_archive_fixture(self, *, interpreter=False):
        directory, root, manifest, runtime, app, dependency = self.fixture()
        self.addCleanup(directory.cleanup)
        binary = root / "stage" / "cad-runtime" / "dependency.dll"
        payload = root / "stage" / "cad-runtime" / ("python313._pth" if interpreter else "Lib/example.py")
        binary.parent.mkdir(parents=True, exist_ok=True)
        payload.parent.mkdir(parents=True, exist_ok=True)
        binary.write_bytes(dependency.read_bytes())
        payload.write_bytes(b"python313.zip\n.\nLib/site-packages\n" if interpreter else b"example = 1\n")
        filename = "python-3.13.15-embed-amd64.zip" if interpreter else "example-1.0-py3-none-any.whl"
        archive = root / "cache" / filename
        archive.parent.mkdir()
        member = "python313._pth" if interpreter else "example/__init__.py"
        with zipfile.ZipFile(archive, "w") as source:
            source.writestr("dependency.dll", binary.read_bytes())
            source.writestr(member, b"python313.zip\n.\n# import site\n" if interpreter else payload.read_bytes())
        lock = root / "third_party" / "cad-runtime-lock.json"
        entry = {"filename": filename, "name": "Dependency", "version": "1.0", "license": "MIT",
                 "kind": "interpreter" if interpreter else "wheel", "sha256": digest(archive),
                 "url": "https://example.invalid/" + filename}
        write_json(lock, {"assets": [entry]})
        component = manifest["components"][1]
        binary_relative, payload_relative = binary.relative_to(root).as_posix(), payload.relative_to(root).as_posix()
        component["source"] = {
            "kind": "locked-archive", "lock_path": "third_party/cad-runtime-lock.json",
            "archive_filename": filename, "archive_path": archive.relative_to(root).as_posix(),
            "paths": [payload_relative],
            "archive_members": {binary_relative: "dependency.dll", payload_relative: member},
        }
        component["source_files"] = [{"path": binary_relative, "sha256": digest(binary)},
                                     {"path": payload_relative, "sha256": digest(payload)}]
        evidence = json.loads(runtime.read_text(encoding="utf-8"))
        evidence["modules"][1]["path"] = str(binary)
        evidence["modules"][0]["imports"][0].update(resolved=str(binary), candidates=[str(binary)])
        write_json(runtime, evidence)
        return root, manifest, runtime, archive, lock, binary, payload, entry

    def test_locked_archive_binds_members_and_exposes_runtime_assets(self):
        root, manifest, runtime, archive, lock, binary, payload, entry = self.locked_archive_fixture()
        result = inventory.build_inventory(root, manifest)
        row = next(item for item in result["components"] if item["id"] == "dependency")
        source = row["package"]["source"]
        self.assertEqual(source["kind"], "locked-archive")
        self.assertEqual(source["archive_sha256"], entry["sha256"])
        self.assertEqual(source["url"], entry["url"])
        self.assertFalse(source["licensing_clearance"])
        self.assertFalse(source["source_closure_qualified"])
        self.assertEqual(row["source_inputs"], [{"path": payload.relative_to(root).as_posix(),
                                               "kind": "file", "sha256": digest(payload)}])
        self.assertEqual(len(source["source_paths"]), 2)
        self.assertNotIn(str(root), json.dumps(result))

    def test_locked_archive_rejects_staged_tamper_even_with_updated_manifest_hash(self):
        root, manifest, runtime, archive, lock, binary, payload, entry = self.locked_archive_fixture()
        payload.write_bytes(b"substituted")
        manifest["components"][1]["source_files"][1]["sha256"] = digest(payload)
        with self.assertRaisesRegex(inventory.InventoryError, "archive member"):
            inventory.build_inventory(root, manifest)

    def test_locked_archive_rejects_archive_substitution(self):
        root, manifest, runtime, archive, lock, binary, payload, entry = self.locked_archive_fixture()
        with zipfile.ZipFile(archive, "a") as source:
            source.writestr("substitute", b"unlocked")
        with self.assertRaisesRegex(inventory.InventoryError, "archive hash"):
            inventory.build_inventory(root, manifest)

    def test_locked_archive_uses_same_immutable_bytes_for_hash_and_members(self):
        root, manifest, runtime, archive, lock, binary, payload, entry = self.locked_archive_fixture()
        original = inventory.zipfile.ZipFile
        def replace_cache_then_open(*args, **kwargs):
            archive.write_bytes(b"changed after read")
            return original(*args, **kwargs)
        with mock.patch.object(inventory.zipfile, "ZipFile", side_effect=replace_cache_then_open):
            result = inventory.build_inventory(root, manifest)
        self.assertEqual(len(result["binaries"]), 2)

    def test_locked_archive_uses_same_lock_bytes_for_parsing_and_digest(self):
        root, manifest, runtime, archive, lock, binary, payload, entry = self.locked_archive_fixture()
        original = lock.read_bytes()
        open_zip = inventory.zipfile.ZipFile
        def replace_lock_then_open(value):
            lock.write_bytes(b'{"assets": []}')
            return open_zip(value)
        with mock.patch.object(inventory.zipfile, "ZipFile", side_effect=replace_lock_then_open):
            result = inventory.build_inventory(root, manifest)
        component = next(row for row in result["components"] if row["id"] == "dependency")
        self.assertEqual(component["package"]["source"]["lock_sha256"], hashlib.sha256(original).hexdigest())
        self.assertNotEqual(component["package"]["source"]["lock_sha256"], digest(lock))

    def test_locked_archive_rejects_version_license_name_and_kind_mismatch(self):
        for field, value in (("version", "2.0"), ("license", "BSD-3-Clause"),
                             ("name", "Another package"), ("kind", "development")):
            with self.subTest(field=field):
                root, manifest, runtime, archive, lock, binary, payload, entry = self.locked_archive_fixture()
                entry[field] = value
                write_json(lock, {"assets": [entry]})
                with self.assertRaises(inventory.InventoryError):
                    inventory.build_inventory(root, manifest)

    def test_locked_archive_rejects_extra_fields_and_mapping_gaps(self):
        for mutation in ("source", "source_file", "extra_mapping", "missing_mapping", "extra_path", "alias_member"):
            with self.subTest(mutation=mutation):
                root, manifest, runtime, archive, lock, binary, payload, entry = self.locked_archive_fixture()
                component = manifest["components"][1]
                source = component["source"]
                if mutation == "source":
                    source["trust_override"] = True
                elif mutation == "source_file":
                    component["source_files"][0]["trust_override"] = True
                elif mutation == "extra_mapping":
                    source["archive_members"]["stage/extra.py"] = "extra.py"
                elif mutation == "missing_mapping":
                    source["archive_members"].pop(payload.relative_to(root).as_posix())
                elif mutation == "extra_path":
                    source["paths"].append("stage/extra.py")
                else:
                    source["archive_members"][payload.relative_to(root).as_posix()] = "example/./__init__.py"
                with self.assertRaises(inventory.InventoryError):
                    inventory.build_inventory(root, manifest)

    def test_locked_archive_rejects_runtime_basename_substitution(self):
        root, manifest, runtime, archive, lock, binary, payload, entry = self.locked_archive_fixture()
        substitute = root / "other-stage" / binary.name
        substitute.parent.mkdir()
        substitute.write_bytes(binary.read_bytes())
        evidence = json.loads(runtime.read_text(encoding="utf-8"))
        evidence["modules"][1]["path"] = str(substitute)
        evidence["modules"][0]["imports"][0].update(resolved=str(substitute), candidates=[str(substitute)])
        write_json(runtime, evidence)
        with self.assertRaisesRegex(inventory.InventoryError, "locked.*(path|source)"):
            inventory.build_inventory(root, manifest)

    def test_locked_interpreter_allows_only_fixed_pth_transform(self):
        root, manifest, runtime, archive, lock, binary, payload, entry = self.locked_archive_fixture(interpreter=True)
        result = inventory.build_inventory(root, manifest)
        self.assertEqual(len(result["binaries"]), 2)
        payload.write_bytes(b"python313.zip\n.\nimport site\n")
        manifest["components"][1]["source_files"][1]["sha256"] = digest(payload)
        with self.assertRaisesRegex(inventory.InventoryError, "archive member"):
            inventory.build_inventory(root, manifest)

    def test_locked_wheel_does_not_allow_pth_transform(self):
        root, manifest, runtime, archive, lock, binary, payload, entry = self.locked_archive_fixture(interpreter=True)
        entry["kind"] = "wheel"
        write_json(lock, {"assets": [entry]})
        with self.assertRaises(inventory.InventoryError):
            inventory.build_inventory(root, manifest)

    def test_locked_archive_rejects_corrupt_duplicate_and_link_members(self):
        for mutation in ("corrupt", "duplicate", "link", "missing"):
            with self.subTest(mutation=mutation):
                root, manifest, runtime, archive, lock, binary, payload, entry = self.locked_archive_fixture()
                if mutation == "corrupt":
                    archive.write_bytes(b"not a ZIP")
                else:
                    with zipfile.ZipFile(archive, "w") as source:
                        source.writestr("dependency.dll", binary.read_bytes())
                        if mutation != "missing":
                            info = zipfile.ZipInfo("example/__init__.py")
                            if mutation == "link":
                                info.create_system = 3
                                info.external_attr = (stat.S_IFLNK | 0o777) << 16
                            source.writestr(info, payload.read_bytes())
                        if mutation == "duplicate":
                            with self.assertWarns(UserWarning):
                                source.writestr("dependency.dll", binary.read_bytes())
                entry["sha256"] = digest(archive)
                write_json(lock, {"assets": [entry]})
                with self.assertRaisesRegex(inventory.InventoryError, "(archive|member)"):
                    inventory.build_inventory(root, manifest)

    def test_locked_archive_rejects_reparse_cache_or_staged_path(self):
        for target_kind in ("archive", "cache_directory", "payload"):
            with self.subTest(target=target_kind):
                root, manifest, runtime, archive, lock, binary, payload, entry = self.locked_archive_fixture()
                target = {"archive": archive, "cache_directory": archive.parent, "payload": payload}[target_kind]
                original = pathlib.Path.lstat
                def reparse_metadata(path, *args, **kwargs):
                    metadata = original(path, *args, **kwargs)
                    if path == target:
                        return SimpleNamespace(st_mode=metadata.st_mode, st_file_attributes=0x400)
                    return metadata
                with mock.patch.object(pathlib.Path, "lstat", reparse_metadata):
                    with self.assertRaisesRegex(inventory.InventoryError, "reparse"):
                        inventory.build_inventory(root, manifest)

    def test_locked_archive_rejects_pe_payload_and_allows_empty_asset_list(self):
        root, manifest, runtime, archive, lock, binary, payload, entry = self.locked_archive_fixture()
        component = manifest["components"][1]
        component["source"]["paths"].append(binary.relative_to(root).as_posix())
        with self.assertRaises(inventory.InventoryError):
            inventory.build_inventory(root, manifest)
        component["source"]["paths"] = []
        component["source_files"].pop()
        component["source"]["archive_members"].pop(payload.relative_to(root).as_posix())
        result = inventory.build_inventory(root, manifest)
        row = next(item for item in result["components"] if item["id"] == "dependency")
        self.assertEqual(row["source_inputs"], [])

    def test_locked_archive_asset_only_component_preserves_verified_inputs(self):
        root, manifest, runtime, archive, lock, binary, payload, entry = self.locked_archive_fixture()
        component = manifest["components"][1]
        component.update(kind="asset", runtime_names=[], destinations={})
        component["source_files"].pop(0)
        component["source"]["archive_members"].pop(binary.relative_to(root).as_posix())
        evidence = json.loads(runtime.read_text(encoding="utf-8"))
        evidence["modules"].pop()
        evidence["modules"][0]["imports"] = []
        write_json(runtime, evidence)
        result = inventory.build_inventory(root, manifest)
        row = result["static_inputs"][0]
        self.assertEqual(row["source_inputs"], [{"path": payload.relative_to(root).as_posix(),
                                               "kind": "file", "sha256": digest(payload)}])


if __name__ == "__main__":
    unittest.main()
