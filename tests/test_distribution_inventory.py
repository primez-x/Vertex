import hashlib
import importlib.util
import json
import pathlib
import subprocess
import tempfile
import unittest
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
    def fixture(self):
        directory = tempfile.TemporaryDirectory()
        root = pathlib.Path(directory.name)
        app = root / "build" / "property-studio.exe"
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
                    "id": "property-studio",
                    "kind": "application",
                    "package": {
                        "name": "Property Studio",
                        "version": "workspace",
                        "license": "Proprietary",
                    },
                    "source": {"kind": "workspace", "paths": ["src"]},
                    "notice_paths": ["third_party/NOTICE.txt"],
                    "runtime_names": ["property-studio.exe"],
                    "destinations": {"property-studio.exe": "bin/property-studio.exe"},
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
        self.assertEqual(binaries["property-studio.exe"]["sha256"], digest(app))
        self.assertEqual(binaries["dependency.dll"]["sha256"], digest(dependency))
        self.assertEqual(binaries["dependency.dll"]["destination"], "bin/dependency.dll")
        self.assertEqual(result["system_runtime_imports"], ["KERNEL32.dll"])
        self.assertEqual(result["windows_api_contracts"], ["api-ms-win-core.dll"])
        self.assertEqual(result["evidence"]["runtime"]["path"], "evidence/runtime.json")
        self.assertNotIn("\\", json.dumps(result))
        self.assertNotIn(str(root), json.dumps(result))

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
        manifest["components"][1]["destinations"]["dependency.dll"] = "bin/property-studio.exe"

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


if __name__ == "__main__":
    unittest.main()
