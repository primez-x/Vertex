from __future__ import annotations

import hashlib
import importlib.util
import json
import pathlib
import shutil
import subprocess
import tempfile
import unittest


ROOT = pathlib.Path(__file__).resolve().parents[1]
SCRIPTS = ROOT / "scripts"
SPEC = importlib.util.spec_from_file_location(
    "stage_offline_bundle", SCRIPTS / "stage_offline_bundle.py"
)
assert SPEC is not None and SPEC.loader is not None
stage = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(stage)


def digest(path: pathlib.Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def write_json(path: pathlib.Path, value: object) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(value, indent=2, sort_keys=True) + "\n", encoding="utf-8")


class StageOfflineBundleTests(unittest.TestCase):
    def fixture(self):
        directory = tempfile.TemporaryDirectory()
        root = pathlib.Path(directory.name)
        app = root / "build" / "property-studio.exe"
        dependency = root / "build" / "dependency.dll"
        font = root / "assets" / "fonts" / "Inter.ttf"
        notice = root / "third_party" / "NOTICE.txt"
        font_notice = root / "assets" / "fonts" / "OFL.txt"
        source = root / "src" / "main.cpp"
        source_notice = root / "LICENSE"
        sqlite_source = root / "third_party" / "sqlite3.c"
        for path, contents in (
            (app, b"application"),
            (dependency, b"dependency"),
            (font, b"font"),
            (notice, b"notice"),
            (font_notice, b"font notice"),
            (source, b"int main() {}\n"),
            (source_notice, b"project license\n"),
            (sqlite_source, b"/* sqlite source */\n"),
        ):
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_bytes(contents)

        inventory = root / "artifacts" / "runtime" / "distribution-inventory.json"
        write_json(
            inventory,
            {
                "schema_version": 1,
                "inventory_version": 1,
                "audit_status": "incomplete",
                "distribution_qualified": False,
                "evidence": {
                    "component_manifest": {
                        "path": "third_party/distribution-components.json",
                        "sha256": digest(notice),
                    }
                },
                "components": [
                    {
                        "id": "property-studio",
                        "kind": "application",
                        "distribution_status": "included",
                        "package": {
                            "name": "Property Studio",
                            "version": "workspace",
                            "license": "Proprietary",
                        },
                        "notices": [{"path": "LICENSE", "sha256": digest(source_notice)}],
                    },
                    {
                        "id": "inter-font",
                        "kind": "asset",
                        "distribution_status": "included",
                        "package": {"name": "Inter", "version": "4.1", "license": "OFL-1.1"},
                        "source_inputs": [{"path": "assets/fonts/Inter.ttf", "sha256": digest(font)}],
                        "notices": [{"path": "assets/fonts/OFL.txt", "sha256": digest(font_notice)}],
                    },
                    {
                        "id": "sqlite",
                        "kind": "static-source",
                        "distribution_status": "included",
                        "package": {"name": "SQLite", "version": "3.53.4", "license": "Public-Domain"},
                        "notices": [{"path": "LICENSE", "sha256": digest(source_notice)}],
                    },
                ],
                "binaries": [
                    {
                        "name": "property-studio.exe",
                        "path": "build/property-studio.exe",
                        "destination": "bin/property-studio.exe",
                        "sha256": digest(app),
                        "component_id": "property-studio",
                    },
                    {
                        "name": "dependency.dll",
                        "path": "build/dependency.dll",
                        "destination": "bin/dependency.dll",
                        "sha256": digest(dependency),
                        "component_id": "property-studio",
                    },
                ],
                "static_inputs": [
                    {
                        "component_id": "sqlite",
                        "kind": "static-source",
                        "distribution_status": "included",
                        "package": {"name": "SQLite", "version": "3.53.4", "license": "Public-Domain"},
                        "source_inputs": [{"path": "third_party/sqlite3.c", "sha256": digest(sqlite_source)}],
                    }
                ],
                "runtime_imports": [
                    {
                        "from": "property-studio.exe",
                        "to": "dependency.dll",
                        "kind": "local-component",
                    }
                ],
                "system_runtime_imports": ["KERNEL32.dll"],
                "windows_api_contracts": ["api-ms-win-core.dll"],
                "summary": {"installer_qualified": False, "offline_qualified": False},
                "boundary": "Inventory only; clean-machine installation remains unqualified.",
            },
        )
        source_kit = root / "artifacts" / "source-kit-manifest.json"
        write_json(
            source_kit,
            {
                "schema_version": 1,
                "manifest_version": 1,
                "audit_status": "incomplete",
                "categories": ["source", "build", "docs", "licenses", "fixtures"],
                "files": [
                    {"category": "licenses", "path": "LICENSE", "sha256": digest(source_notice), "size": source_notice.stat().st_size},
                    {"category": "source", "path": "src/main.cpp", "sha256": digest(source), "size": source.stat().st_size},
                ],
                "category_counts": {"source": 1, "build": 0, "docs": 0, "licenses": 1, "fixtures": 0},
                "summary": {
                    "entry_count": 2,
                    "total_size": source.stat().st_size + source_notice.stat().st_size,
                    "category_counts": {"source": 1, "build": 0, "docs": 0, "licenses": 1, "fixtures": 0},
                },
                "boundary": "This records an incomplete source kit; licensing, SBOM, and rebuild remain open.",
            },
        )
        allowlist = root / "packaging" / "portable-allowlist.json"
        write_json(
            allowlist,
            {
                "schema_version": 1,
                "entries": [
                    {
                        "kind": "asset",
                        "inventory_entry": "inter-font",
                        "path": "assets/fonts/Inter.ttf",
                        "destination": "assets/fonts/Inter.ttf",
                    },
                    {
                        "kind": "notice",
                        "inventory_entry": "property-studio",
                        "path": "LICENSE",
                        "destination": "licenses/LICENSE.txt",
                    },
                    {
                        "kind": "notice",
                        "inventory_entry": "inter-font",
                        "path": "assets/fonts/OFL.txt",
                        "destination": "licenses/Inter-OFL.txt",
                    },
                ],
            },
        )
        return directory, root, inventory, source_kit, allowlist, app, dependency, source, source_notice

    def test_stages_self_contained_bundle_and_runtime_manifest(self):
        fixture = self.fixture()
        self.addCleanup(fixture[0].cleanup)
        _, root, inventory, source_kit, allowlist, app, dependency, source, source_notice = fixture
        output_root = root / "out"

        result = stage.stage_bundle(
            inventory,
            allowlist,
            source_kit,
            root,
            output_root,
            "property-studio-offline",
        )
        bundle = output_root / "property-studio-offline"

        self.assertEqual(result["audit_status"], "incomplete")
        self.assertFalse(result["qualification"]["installer_qualified"])
        self.assertFalse(result["qualification"]["offline_qualified"])
        self.assertEqual(result["manifest_kind"], "offline-bundle")
        self.assertEqual(result["source_inventory"]["sha256"], digest(inventory))
        self.assertEqual(result["source_kit"]["sha256"], digest(source_kit))
        self.assertEqual(result["sbom"]["format"], "SPDX-2.3")
        self.assertEqual(result["sbom"]["sha256"], digest(bundle / "metadata/distribution-sbom.spdx.json"))
        sbom = json.loads((bundle / "metadata/distribution-sbom.spdx.json").read_text(encoding="utf-8"))
        stage._SBOM.validate_sbom(sbom)
        licenses = {row["component_id"]: row for row in result["license_inventory"]}
        self.assertEqual(licenses["inter-font"]["license"], "OFL-1.1")
        self.assertEqual(licenses["inter-font"]["notices"][0]["path"], "assets/fonts/OFL.txt")
        self.assertEqual(licenses["sqlite"]["license"], "Public-Domain")
        self.assertEqual(
            [row["destination"] for row in result["dependency_closure"]["runtime"]],
            ["bin/dependency.dll", "bin/property-studio.exe"],
        )
        self.assertEqual(result["dependency_closure"]["static"][0]["component_id"], "sqlite")
        self.assertEqual(
            result["dependency_closure"]["static"][0]["source_inputs"][0]["path"],
            "third_party/sqlite3.c",
        )
        self.assertEqual(result["dependency_closure"]["system_runtime_imports"], ["KERNEL32.dll"])
        self.assertEqual(
            {item["path"] for item in result["files"]},
            {
                "bin/property-studio.exe",
                "bin/dependency.dll",
                "assets/fonts/Inter.ttf",
                "licenses/LICENSE.txt",
                "licenses/Inter-OFL.txt",
                "source-kit/src/main.cpp",
                "source-kit/LICENSE",
                "metadata/distribution-inventory.json",
                "metadata/source-kit-manifest.json",
                "metadata/portable-package-manifest.json",
                "metadata/distribution-sbom.spdx.json",
                "runtime-manifest.json",
                "install-offline-bundle.ps1",
                "verify-offline-bundle.ps1",
            },
        )
        self.assertEqual((bundle / "bin/property-studio.exe").read_bytes(), app.read_bytes())
        self.assertEqual((bundle / "bin/dependency.dll").read_bytes(), dependency.read_bytes())
        self.assertEqual((bundle / "source-kit/src/main.cpp").read_bytes(), source.read_bytes())
        self.assertEqual((bundle / "source-kit/LICENSE").read_bytes(), source_notice.read_bytes())
        self.assertEqual(stage.verify_bundle(bundle), {"file_count": len(result["files"]), "manifest_kind": "offline-bundle"})

        runtime_manifest = json.loads((bundle / "runtime-manifest.json").read_text(encoding="utf-8"))
        self.assertEqual(runtime_manifest["manifest_kind"], "runtime")
        self.assertEqual(
            {item["path"] for item in runtime_manifest["files"]},
            {
                "bin/property-studio.exe",
                "bin/dependency.dll",
                "assets/fonts/Inter.ttf",
                "licenses/LICENSE.txt",
                "licenses/Inter-OFL.txt",
            },
        )
        self.assertNotIn(str(root), json.dumps(result))
        self.assertNotIn("\\", json.dumps(result))
        self.assertIn("clean-machine", result["boundary"].lower())

    def test_staging_is_byte_deterministic_for_same_inputs(self):
        fixture = self.fixture()
        self.addCleanup(fixture[0].cleanup)
        _, root, inventory, source_kit, allowlist, *_ = fixture

        first = stage.stage_bundle(inventory, allowlist, source_kit, root, root / "out", "first")
        second = stage.stage_bundle(inventory, allowlist, source_kit, root, root / "out", "second")
        self.assertEqual(first, second)

        def files_at(path):
            return {
                item.relative_to(path).as_posix(): item.read_bytes()
                for item in path.rglob("*")
                if item.is_file()
            }

        self.assertEqual(files_at(root / "out" / "first"), files_at(root / "out" / "second"))
        self.assertTrue(all(
            int(item.stat().st_mtime) == stage.FIXED_MTIME
            for item in (root / "out" / "first").rglob("*")
            if item.is_file()
        ))

    def test_installed_msvc_runtime_cannot_replace_bundled_dependency(self):
        fixture = self.fixture()
        self.addCleanup(fixture[0].cleanup)
        _, root, inventory_path, source_kit, allowlist, *_ = fixture
        inventory = json.loads(inventory_path.read_text(encoding="utf-8"))
        inventory["system_runtime_imports"] = ["KERNEL32.dll", "VCRUNTIME140.dll"]
        write_json(inventory_path, inventory)
        with self.assertRaisesRegex(stage.BundleError, "Visual C\\+\\+ runtime"):
            stage.stage_bundle(inventory_path, allowlist, source_kit, root, root / "out", "missing-crt")
        self.assertFalse((root / "out" / "missing-crt").exists())

    def test_missing_source_kit_file_fails_before_publishing_bundle(self):
        fixture = self.fixture()
        self.addCleanup(fixture[0].cleanup)
        _, root, inventory, source_kit, allowlist, _, _, source, _ = fixture
        source.unlink()

        with self.assertRaises(stage.BundleError) as context:
            stage.stage_bundle(inventory, allowlist, source_kit, root, root / "out", "missing")

        self.assertIn("source-kit", str(context.exception).lower())
        self.assertFalse((root / "out" / "missing").exists())

    def test_powershell_verifier_checks_the_published_bundle(self):
        pwsh = shutil.which("pwsh")
        if pwsh is None:
            self.skipTest("PowerShell is unavailable")
        fixture = self.fixture()
        self.addCleanup(fixture[0].cleanup)
        _, root, inventory, source_kit, allowlist, *_ = fixture
        stage.stage_bundle(inventory, allowlist, source_kit, root, root / "out", "verify")
        bundle = root / "out" / "verify"

        checked = subprocess.run(
            [pwsh, "-NoLogo", "-NoProfile", "-NonInteractive", "-File",
             str(bundle / "verify-offline-bundle.ps1"), "-Root", str(bundle)],
            capture_output=True,
            text=True,
            check=False,
        )
        self.assertEqual(checked.returncode, 0, checked.stderr)
        (bundle / "bin" / "dependency.dll").write_bytes(b"tampered")
        rejected = subprocess.run(
            [pwsh, "-NoLogo", "-NoProfile", "-NonInteractive", "-File",
             str(bundle / "verify-offline-bundle.ps1"), "-Root", str(bundle)],
            capture_output=True,
            text=True,
            check=False,
        )
        self.assertNotEqual(rejected.returncode, 0)

    def test_powershell_installer_copies_only_verified_runtime_set(self):
        pwsh = shutil.which("pwsh")
        if pwsh is None:
            self.skipTest("PowerShell is unavailable")
        fixture = self.fixture()
        self.addCleanup(fixture[0].cleanup)
        _, root, inventory, source_kit, allowlist, *_ = fixture
        stage.stage_bundle(inventory, allowlist, source_kit, root, root / "out", "install")
        bundle = root / "out" / "install"
        target = root / "installed"

        checked = subprocess.run(
            [pwsh, "-NoLogo", "-NoProfile", "-NonInteractive", "-File",
             str(bundle / "install-offline-bundle.ps1"), "-InstallRoot", str(target)],
            capture_output=True,
            text=True,
            check=False,
        )
        self.assertEqual(checked.returncode, 0, checked.stderr)
        self.assertEqual((target / "bin" / "property-studio.exe").read_bytes(), b"application")
        self.assertEqual((target / "bin" / "dependency.dll").read_bytes(), b"dependency")
        self.assertEqual((target / "licenses" / "LICENSE.txt").read_bytes(), b"project license\n")
        self.assertFalse((target / "source-kit").exists())

    def test_powershell_installer_replaces_an_existing_empty_directory_atomically(self):
        pwsh = shutil.which("pwsh")
        if pwsh is None:
            self.skipTest("PowerShell is unavailable")
        fixture = self.fixture()
        self.addCleanup(fixture[0].cleanup)
        _, root, inventory, source_kit, allowlist, *_ = fixture
        stage.stage_bundle(inventory, allowlist, source_kit, root, root / "out", "replace-empty")
        bundle = root / "out" / "replace-empty"
        target = root / "installed"
        target.mkdir()

        checked = subprocess.run(
            [pwsh, "-NoLogo", "-NoProfile", "-NonInteractive", "-File",
             str(bundle / "install-offline-bundle.ps1"), "-InstallRoot", str(target)],
            capture_output=True,
            text=True,
            check=False,
        )
        self.assertEqual(checked.returncode, 0, checked.stderr)
        self.assertEqual((target / "bin" / "property-studio.exe").read_bytes(), b"application")
        self.assertFalse(any(path.name.startswith(".installed.") for path in target.parent.iterdir()))

    def test_powershell_installer_repairs_a_modified_runtime(self):
        pwsh = shutil.which("pwsh")
        if pwsh is None:
            self.skipTest("PowerShell is unavailable")
        fixture = self.fixture()
        self.addCleanup(fixture[0].cleanup)
        _, root, inventory, source_kit, allowlist, *_ = fixture
        stage.stage_bundle(inventory, allowlist, source_kit, root, root / "out", "repair")
        bundle = root / "out" / "repair"
        target = root / "installed"
        install = subprocess.run(
            [pwsh, "-NoLogo", "-NoProfile", "-NonInteractive", "-File",
             str(bundle / "install-offline-bundle.ps1"), "-InstallRoot", str(target)],
            capture_output=True, text=True, check=False,
        )
        self.assertEqual(install.returncode, 0, install.stderr)
        (target / "bin" / "property-studio.exe").write_bytes(b"tampered")
        repaired = subprocess.run(
            [pwsh, "-NoLogo", "-NoProfile", "-NonInteractive", "-File",
             str(bundle / "install-offline-bundle.ps1"), "-InstallRoot", str(target),
             "-Action", "Repair"],
            capture_output=True, text=True, check=False,
        )
        self.assertEqual(repaired.returncode, 0, repaired.stderr)
        self.assertEqual((target / "bin" / "property-studio.exe").read_bytes(), b"application")
        self.assertFalse(any(path.name.startswith(".installed.") for path in target.parent.iterdir()))

    def test_powershell_installer_uninstalls_only_a_marked_runtime(self):
        pwsh = shutil.which("pwsh")
        if pwsh is None:
            self.skipTest("PowerShell is unavailable")
        fixture = self.fixture()
        self.addCleanup(fixture[0].cleanup)
        _, root, inventory, source_kit, allowlist, *_ = fixture
        stage.stage_bundle(inventory, allowlist, source_kit, root, root / "out", "uninstall")
        bundle = root / "out" / "uninstall"
        target = root / "installed"
        installed = subprocess.run(
            [pwsh, "-NoLogo", "-NoProfile", "-NonInteractive", "-File",
             str(bundle / "install-offline-bundle.ps1"), "-InstallRoot", str(target)],
            capture_output=True, text=True, check=False,
        )
        self.assertEqual(installed.returncode, 0, installed.stderr)
        removed = subprocess.run(
            [pwsh, "-NoLogo", "-NoProfile", "-NonInteractive", "-File",
             str(bundle / "install-offline-bundle.ps1"), "-InstallRoot", str(target),
             "-Action", "Uninstall"],
            capture_output=True, text=True, check=False,
        )
        self.assertEqual(removed.returncode, 0, removed.stderr)
        self.assertFalse(target.exists())

    def test_powershell_uninstall_rejects_an_unmarked_directory(self):
        pwsh = shutil.which("pwsh")
        if pwsh is None:
            self.skipTest("PowerShell is unavailable")
        fixture = self.fixture()
        self.addCleanup(fixture[0].cleanup)
        _, root, inventory, source_kit, allowlist, *_ = fixture
        stage.stage_bundle(inventory, allowlist, source_kit, root, root / "out", "uninstall-reject")
        bundle = root / "out" / "uninstall-reject"
        target = root / "unrelated"
        target.mkdir()
        (target / "data.txt").write_text("keep", encoding="utf-8")
        rejected = subprocess.run(
            [pwsh, "-NoLogo", "-NoProfile", "-NonInteractive", "-File",
             str(bundle / "install-offline-bundle.ps1"), "-InstallRoot", str(target),
             "-Action", "Uninstall"],
            capture_output=True, text=True, check=False,
        )
        self.assertNotEqual(rejected.returncode, 0)
        self.assertEqual((target / "data.txt").read_text(encoding="utf-8"), "keep")

    def test_powershell_installer_uses_transactional_publish(self):
        fixture = self.fixture()
        self.addCleanup(fixture[0].cleanup)
        _, root, inventory, source_kit, allowlist, *_ = fixture
        stage.stage_bundle(inventory, allowlist, source_kit, root, root / "out", "transaction")
        installer = (root / "out" / "transaction" / "install-offline-bundle.ps1").read_text(encoding="utf-8")
        self.assertIn("ValidateSet('Install', 'Repair', 'Uninstall')", installer)
        self.assertIn("Uninstall", installer)
        self.assertIn("$stagingRoot", installer)
        self.assertIn("Move-Item -LiteralPath $stagingRoot", installer)
        self.assertIn("$backupRoot", installer)

    def test_strict_bundle_verifier_rejects_unlisted_files(self):
        fixture = self.fixture()
        self.addCleanup(fixture[0].cleanup)
        _, root, inventory, source_kit, allowlist, *_ = fixture
        stage.stage_bundle(inventory, allowlist, source_kit, root, root / "out", "strict")
        bundle = root / "out" / "strict"
        (bundle / "unexpected.tmp").write_bytes(b"not in the manifest")

        with self.assertRaises(stage.BundleError) as context:
            stage.verify_bundle(bundle)

        self.assertIn("unlisted", str(context.exception).lower())
        pwsh = shutil.which("pwsh")
        if pwsh is not None:
            checked = subprocess.run(
                [pwsh, "-NoLogo", "-NoProfile", "-NonInteractive", "-File",
                 str(bundle / "verify-offline-bundle.ps1"), "-Root", str(bundle)],
                capture_output=True,
                text=True,
                check=False,
            )
            self.assertNotEqual(checked.returncode, 0)

    def test_bundle_verifier_rejects_install_list_drift(self):
        fixture = self.fixture()
        self.addCleanup(fixture[0].cleanup)
        _, root, inventory, source_kit, allowlist, *_ = fixture
        stage.stage_bundle(inventory, allowlist, source_kit, root, root / "out", "drift")
        bundle = root / "out" / "drift"
        runtime_manifest_path = bundle / "runtime-manifest.json"
        runtime_manifest = json.loads(runtime_manifest_path.read_text(encoding="utf-8"))
        runtime_manifest["files"][0]["component_id"] = "drifted-component"
        write_json(runtime_manifest_path, runtime_manifest)
        bundle_manifest_path = bundle / "offline-bundle-manifest.json"
        bundle_manifest = json.loads(bundle_manifest_path.read_text(encoding="utf-8"))
        runtime_record = next(
            row for row in bundle_manifest["files"] if row["path"] == "runtime-manifest.json"
        )
        runtime_record["sha256"] = digest(runtime_manifest_path)
        runtime_record["size"] = runtime_manifest_path.stat().st_size
        bundle_manifest["runtime_manifest"]["sha256"] = digest(runtime_manifest_path)
        write_json(bundle_manifest_path, bundle_manifest)

        with self.assertRaises(stage.BundleError) as context:
            stage.verify_bundle(bundle)

        self.assertIn("install list", str(context.exception).lower())

    def test_bundle_verifier_rejects_a_structurally_invalid_sbom_even_if_hashes_are_rewritten(self):
        fixture = self.fixture()
        self.addCleanup(fixture[0].cleanup)
        _, root, inventory, source_kit, allowlist, *_ = fixture
        stage.stage_bundle(inventory, allowlist, source_kit, root, root / "out", "invalid-sbom")
        bundle = root / "out" / "invalid-sbom"
        sbom_path = bundle / "metadata" / "distribution-sbom.spdx.json"
        sbom = json.loads(sbom_path.read_text(encoding="utf-8"))
        sbom["relationships"][0]["relatedSpdxElement"] = "SPDXRef-unknown"
        write_json(sbom_path, sbom)
        sbom_hash = digest(sbom_path)
        manifest_path = bundle / "offline-bundle-manifest.json"
        manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
        manifest["sbom"]["sha256"] = sbom_hash
        next(row for row in manifest["files"] if row["path"] ==
             "metadata/distribution-sbom.spdx.json").update(
                 sha256=sbom_hash, size=sbom_path.stat().st_size)
        write_json(manifest_path, manifest)

        with self.assertRaises(stage.BundleError) as context:
            stage.verify_bundle(bundle)
        self.assertIn("sbom", str(context.exception).lower())


if __name__ == "__main__":
    unittest.main()
