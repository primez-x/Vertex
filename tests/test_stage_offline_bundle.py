from __future__ import annotations

import hashlib
import importlib.util
import json
import os
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
    NATIVE_ACL_SNAPSHOT = r"""
Add-Type -TypeDefinition @'
using System;
using System.Collections.Generic;
using System.ComponentModel;
using System.Runtime.InteropServices;
using System.Security.AccessControl;
public static class FixtureNativeAcl {
    [DllImport("kernel32.dll", CharSet=CharSet.Unicode, SetLastError=true)]
    static extern IntPtr CreateFileW(string path, uint access, uint share, IntPtr attributes,
        uint disposition, uint flags, IntPtr template);
    [DllImport("kernel32.dll")] static extern bool CloseHandle(IntPtr handle);
    [DllImport("kernel32.dll")] static extern IntPtr LocalFree(IntPtr pointer);
    [DllImport("advapi32.dll")]
    static extern uint GetSecurityInfo(IntPtr handle, uint type, uint information,
        out IntPtr owner, out IntPtr group, out IntPtr dacl, out IntPtr sacl, out IntPtr descriptor);
    [DllImport("advapi32.dll", SetLastError=true)]
    static extern bool GetSecurityDescriptorControl(IntPtr descriptor, out ushort control, out uint revision);
    [DllImport("advapi32.dll")]
    static extern uint GetSecurityDescriptorLength(IntPtr descriptor);
    public static string Read(string path) {
        IntPtr handle=CreateFileW(path,0x20000,7,IntPtr.Zero,3,0x02200000,IntPtr.Zero);
        if(handle==new IntPtr(-1)) throw new Win32Exception(Marshal.GetLastWin32Error());
        IntPtr descriptor=IntPtr.Zero;
        try {
            IntPtr owner, group, dacl, sacl;
            uint result=GetSecurityInfo(handle,1,7,out owner,out group,out dacl,out sacl,out descriptor);
            if(result!=0) throw new Win32Exception((int)result);
            ushort control; uint revision;
            if(!GetSecurityDescriptorControl(descriptor,out control,out revision))
                throw new Win32Exception(Marshal.GetLastWin32Error());
            byte[] raw=new byte[GetSecurityDescriptorLength(descriptor)];
            Marshal.Copy(descriptor,raw,0,raw.Length);
            var parsed=new RawSecurityDescriptor(raw,0);
            var entries=new List<string>();
            if(parsed.DiscretionaryAcl!=null) foreach(GenericAce ace in parsed.DiscretionaryAcl) {
                byte[] binary=new byte[ace.BinaryLength];
                ace.GetBinaryForm(binary,0);
                entries.Add(Convert.ToBase64String(binary));
            }
            // Binary ACEs retain SID/type/mask/flags and their original order.
            return parsed.Owner.Value+"|"+parsed.Group.Value+"|"+control+"|"+revision+"|"+
                (parsed.DiscretionaryAcl==null ? "NULL" : String.Join(",",entries));
        } finally { if(descriptor!=IntPtr.Zero) LocalFree(descriptor); CloseHandle(handle); }
    }
}
'@
"""

    def run_powershell(self, script: str):
        return subprocess.run(
            [shutil.which("pwsh"), "-NoLogo", "-NoProfile", "-NonInteractive", "-Command", script],
            capture_output=True, text=True, check=False,
            creationflags=getattr(subprocess, "CREATE_NO_WINDOW", 0),
        )

    @staticmethod
    def ps_path(path):
        return "'" + str(path).replace("'", "''") + "'"

    def make_fixture_path_writable(self, path):
        if os.name != "nt":
            return
        checked = self.run_powershell(
            "$ErrorActionPreference='Stop'; $sid=[Security.Principal.WindowsIdentity]::GetCurrent().User; "
            f"$path={self.ps_path(path)}; $old=Get-Acl -LiteralPath $path; "
            "$acl=if((Get-Item -LiteralPath $path).PSIsContainer){[Security.AccessControl.DirectorySecurity]::new()}"
            "else{[Security.AccessControl.FileSecurity]::new()}; "
            "$acl.SetSecurityDescriptorSddlForm($old.Sddl,[Security.AccessControl.AccessControlSections]::Access); "
            "$acl.AddAccessRule([Security.AccessControl.FileSystemAccessRule]::new("
            "$sid,'FullControl','Allow')); $item=Get-Item -LiteralPath $path; "
            "[IO.FileSystemAclExtensions]::SetAccessControl($item,$acl)")
        self.assertEqual(checked.returncode, 0, checked.stderr)

    def runtime_acls(self, target):
        if os.name != "nt":
            return None
        checked = self.run_powershell(
            "$ErrorActionPreference='Stop'; $result=@{}; "
            f"Get-ChildItem -LiteralPath {self.ps_path(target)} -Recurse -Force | "
            "ForEach-Object { $result[$_.FullName]=(Get-Acl -LiteralPath $_.FullName).Sddl }; "
            "$result | ConvertTo-Json -Compress")
        self.assertEqual(checked.returncode, 0, checked.stderr)
        return json.loads(checked.stdout)

    def cleanup_fixture(self, directory):
        # Tests own this temporary tree. Unlock only manifest-listed module
        # paths, including a preserved transaction backup, before deleting it.
        root = pathlib.Path(directory.name)
        if os.name == "nt" and shutil.which("pwsh"):
            paths = set()
            markers = list(root.rglob("runtime-manifest.json"))
            # A rejection test may deliberately damage the installed marker.
            # The original source bundle still supplies all owned paths.
            module_relatives = set()
            for marker in markers:
                manifest = json.loads(marker.read_text(encoding="utf-8"))
                for entry in manifest["files"]:
                    relative = pathlib.PurePosixPath(entry["path"])
                    if relative.parts[0] not in ("bin", "plugins"):
                        continue
                    module_relatives.add(relative)
            for marker in markers:
                for relative in module_relatives:
                    path = marker.parent.joinpath(*relative.parts)
                    paths.add(path)
                    while path.parent != marker.parent:
                        path = path.parent
                        paths.add(path)
            existing = [p for p in paths if p.exists()]
            if existing:
                literals = ",".join(self.ps_path(p) for p in existing)
                checked = self.run_powershell(
                    "$ErrorActionPreference='Stop'; $sid=[Security.Principal.WindowsIdentity]::GetCurrent().User; "
                    f"foreach ($path in @({literals})) {{ $old=Get-Acl -LiteralPath $path; "
                    "$acl=if((Get-Item -LiteralPath $path).PSIsContainer){[Security.AccessControl.DirectorySecurity]::new()}"
                    "else{[Security.AccessControl.FileSecurity]::new()}; "
                    "$acl.SetSecurityDescriptorSddlForm($old.Sddl,[Security.AccessControl.AccessControlSections]::Access); "
                    "$acl.AddAccessRule([Security.AccessControl.FileSystemAccessRule]::new("
                    "$sid,'FullControl','Allow')); $item=Get-Item -LiteralPath $path; "
                    "[IO.FileSystemAclExtensions]::SetAccessControl($item,$acl) }")
                self.assertEqual(checked.returncode, 0, checked.stderr)
        directory.cleanup()

    def fixture(self):
        directory = tempfile.TemporaryDirectory()
        root = pathlib.Path(directory.name)
        app = root / "build" / "vertex.exe"
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
                        "id": "vertex",
                        "kind": "application",
                        "distribution_status": "included",
                        "package": {
                            "name": "Vertex",
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
                        "name": "vertex.exe",
                        "path": "build/vertex.exe",
                        "destination": "bin/vertex.exe",
                        "sha256": digest(app),
                        "component_id": "vertex",
                    },
                    {
                        "name": "dependency.dll",
                        "path": "build/dependency.dll",
                        "destination": "bin/dependency.dll",
                        "sha256": digest(dependency),
                        "component_id": "vertex",
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
                        "from": "vertex.exe",
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
                        "inventory_entry": "vertex",
                        "path": "LICENSE",
                        "destination": "licenses/LICENSE.txt",
                    },
                    {
                        "kind": "notice",
                        "inventory_entry": "inter-font",
                        "path": "assets/fonts/OFL.txt",
                        "destination": "licenses/Inter-OFL.txt",
                    },
                    {
                        "kind": "notice",
                        "inventory_entry": "sqlite",
                        "path": "LICENSE",
                        "destination": "licenses/sqlite-LICENSE.txt",
                    },
                ],
            },
        )
        return directory, root, inventory, source_kit, allowlist, app, dependency, source, source_notice

    def test_stages_self_contained_bundle_and_runtime_manifest(self):
        fixture = self.fixture()
        self.addCleanup(self.cleanup_fixture, fixture[0])
        _, root, inventory, source_kit, allowlist, app, dependency, source, source_notice = fixture
        output_root = root / "out"

        result = stage.stage_bundle(
            inventory,
            allowlist,
            source_kit,
            root,
            output_root,
            "vertex-offline",
        )
        bundle = output_root / "vertex-offline"

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
            ["bin/dependency.dll", "bin/vertex.exe"],
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
                "bin/vertex.exe",
                "bin/dependency.dll",
                "assets/fonts/Inter.ttf",
                "licenses/LICENSE.txt",
                "licenses/Inter-OFL.txt",
                "licenses/sqlite-LICENSE.txt",
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
        self.assertEqual((bundle / "bin/vertex.exe").read_bytes(), app.read_bytes())
        self.assertEqual((bundle / "bin/dependency.dll").read_bytes(), dependency.read_bytes())
        self.assertEqual((bundle / "source-kit/src/main.cpp").read_bytes(), source.read_bytes())
        self.assertEqual((bundle / "source-kit/LICENSE").read_bytes(), source_notice.read_bytes())
        self.assertEqual(stage.verify_bundle(bundle), {"file_count": len(result["files"]), "manifest_kind": "offline-bundle"})

        runtime_manifest = json.loads((bundle / "runtime-manifest.json").read_text(encoding="utf-8"))
        self.assertEqual(runtime_manifest["manifest_kind"], "runtime")
        self.assertEqual(
            {item["path"] for item in runtime_manifest["files"]},
            {
                "bin/vertex.exe",
                "bin/dependency.dll",
                "assets/fonts/Inter.ttf",
                "licenses/LICENSE.txt",
                "licenses/Inter-OFL.txt",
                "licenses/sqlite-LICENSE.txt",
            },
        )
        self.assertNotIn(str(root), json.dumps(result))
        self.assertNotIn("\\", json.dumps(result))
        self.assertIn("clean-machine", result["boundary"].lower())

    def test_staging_is_byte_deterministic_for_same_inputs(self):
        fixture = self.fixture()
        self.addCleanup(self.cleanup_fixture, fixture[0])
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
        self.addCleanup(self.cleanup_fixture, fixture[0])
        _, root, inventory_path, source_kit, allowlist, *_ = fixture
        inventory = json.loads(inventory_path.read_text(encoding="utf-8"))
        inventory["system_runtime_imports"] = ["KERNEL32.dll", "VCRUNTIME140.dll"]
        write_json(inventory_path, inventory)
        with self.assertRaisesRegex(stage.BundleError, "Visual C\\+\\+ runtime"):
            stage.stage_bundle(inventory_path, allowlist, source_kit, root, root / "out", "missing-crt")
        self.assertFalse((root / "out" / "missing-crt").exists())

    def test_missing_source_kit_file_fails_before_publishing_bundle(self):
        fixture = self.fixture()
        self.addCleanup(self.cleanup_fixture, fixture[0])
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
        self.addCleanup(self.cleanup_fixture, fixture[0])
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
        self.addCleanup(self.cleanup_fixture, fixture[0])
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
        self.assertEqual((target / "bin" / "vertex.exe").read_bytes(), b"application")
        self.assertEqual((target / "bin" / "dependency.dll").read_bytes(), b"dependency")
        self.assertEqual((target / "licenses" / "LICENSE.txt").read_bytes(), b"project license\n")
        self.assertFalse((target / "source-kit").exists())

    def test_powershell_installer_replaces_an_existing_empty_directory_atomically(self):
        pwsh = shutil.which("pwsh")
        if pwsh is None:
            self.skipTest("PowerShell is unavailable")
        fixture = self.fixture()
        self.addCleanup(self.cleanup_fixture, fixture[0])
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
        self.assertEqual((target / "bin" / "vertex.exe").read_bytes(), b"application")
        self.assertFalse(any(path.name.startswith(".installed.") for path in target.parent.iterdir()))

    def test_powershell_installer_repairs_a_modified_runtime(self):
        pwsh = shutil.which("pwsh")
        if pwsh is None:
            self.skipTest("PowerShell is unavailable")
        fixture = self.fixture()
        self.addCleanup(self.cleanup_fixture, fixture[0])
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
        self.make_fixture_path_writable(target / "bin" / "vertex.exe")
        (target / "bin" / "vertex.exe").write_bytes(b"tampered")
        repaired = subprocess.run(
            [pwsh, "-NoLogo", "-NoProfile", "-NonInteractive", "-File",
             str(bundle / "install-offline-bundle.ps1"), "-InstallRoot", str(target),
             "-Action", "Repair"],
            capture_output=True, text=True, check=False,
        )
        self.assertEqual(repaired.returncode, 0, repaired.stderr)
        self.assertEqual((target / "bin" / "vertex.exe").read_bytes(), b"application")
        self.assertFalse(any(path.name.startswith(".installed.") for path in target.parent.iterdir()))

    def test_powershell_installer_uninstalls_only_a_marked_runtime(self):
        pwsh = shutil.which("pwsh")
        if pwsh is None:
            self.skipTest("PowerShell is unavailable")
        fixture = self.fixture()
        self.addCleanup(self.cleanup_fixture, fixture[0])
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
        self.addCleanup(self.cleanup_fixture, fixture[0])
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
        self.addCleanup(self.cleanup_fixture, fixture[0])
        _, root, inventory, source_kit, allowlist, *_ = fixture
        stage.stage_bundle(inventory, allowlist, source_kit, root, root / "out", "transaction")
        installer = (root / "out" / "transaction" / "install-offline-bundle.ps1").read_text(encoding="utf-8")
        self.assertIn("ValidateSet('Install', 'Repair', 'Uninstall')", installer)
        self.assertIn("Uninstall", installer)
        self.assertIn("$stagingRoot", installer)
        self.assertIn("Move-Item -LiteralPath $stagingRoot", installer)
        self.assertIn("$backupRoot", installer)

    @unittest.skipUnless(os.name == "nt", "Windows module path inspection")
    def test_owned_module_paths_checks_each_file_and_each_distinct_parent_per_call(self):
        if shutil.which("pwsh") is None:
            self.skipTest("PowerShell is unavailable")
        with tempfile.TemporaryDirectory() as temporary:
            root = pathlib.Path(temporary).resolve()
            module_relatives = [
                f"bin/cad-runtime/Lib/site-packages/sample/shared/layer/part-{index}.dll"
                for index in range(64)
            ] + ["plugins/platforms/qwindows.dll", "plugins/imageformats/deep/qjpeg.dll"]
            expected = {}
            for relative in module_relatives:
                path = root.joinpath(*pathlib.PurePosixPath(relative).parts)
                path.parent.mkdir(parents=True, exist_ok=True)
                path.write_bytes(b"module fixture")
                expected[str(path).casefold()] = False
                parent = path.parent
                while parent != root:
                    expected[str(parent).casefold()] = True
                    parent = parent.parent
            # Case variants must share their ancestors on Windows. Every file
            # still passes through the actual resolver and complete chain check.
            manifest_relatives = list(module_relatives)
            manifest_relatives[1] = manifest_relatives[1].replace("bin/", "BIN/", 1)
            ignored = root / "assets" / "ignored.txt"
            ignored.parent.mkdir()
            ignored.write_bytes(b"not a loadable module")
            manifest_path = root / "fixture-manifest.json"
            write_json(manifest_path, {"files": [
                {"path": relative} for relative in manifest_relatives + ["assets/ignored.txt"]
            ]})
            script = r"""
$ErrorActionPreference='Stop'
$tokens=$null; $parseErrors=$null
$ast=[Management.Automation.Language.Parser]::ParseFile(INSTALLER_PATH,[ref]$tokens,[ref]$parseErrors)
if ($parseErrors.Count) { throw 'Installer parse failed' }
foreach ($name in @('Fail','Assert-NoReparseChain','Resolve-SafeChildPath','Get-OwnedModulePaths')) {
    $definitions=@($ast.FindAll({ param($node)
        $node -is [Management.Automation.Language.FunctionDefinitionAst] -and $node.Name -eq $name
    },$true))
    if ($definitions.Count -ne 1) { throw "Missing or duplicate helper: $name" }
    . ([ScriptBlock]::Create($definitions[0].Extent.Text))
}
$script:actualResolve=(Get-Command Resolve-SafeChildPath).ScriptBlock
$script:actualAssert=(Get-Command Assert-NoReparseChain).ScriptBlock
$script:resolved=[Collections.Generic.List[string]]::new()
$script:directoryAssertions=[Collections.Generic.List[string]]::new()
$script:containerChecks=[Collections.Hashtable]::new([StringComparer]::OrdinalIgnoreCase)
function Resolve-SafeChildPath([string]$RootPath,[string]$RelativePath,[string]$Field) {
    $script:resolved.Add($RelativePath)
    & $script:actualResolve $RootPath $RelativePath $Field
}
function Assert-NoReparseChain([string]$RootPath,[string]$Candidate,[string]$Field) {
    if ($Field -eq 'owned module directory') { $script:directoryAssertions.Add($Candidate) }
    & $script:actualAssert $RootPath $Candidate $Field
}
function Test-Path {
    [CmdletBinding()]
    param([string]$LiteralPath,[string]$PathType)
    $parameters=@{LiteralPath=$LiteralPath}
    if ($PathType) { $parameters.PathType=$PathType }
    if ($PathType -eq 'Container') {
        if (-not $script:containerChecks.ContainsKey($LiteralPath)) { $script:containerChecks[$LiteralPath]=0 }
        $script:containerChecks[$LiteralPath]=1+$script:containerChecks[$LiteralPath]
    }
    Microsoft.PowerShell.Management\Test-Path @parameters
}
$manifest=Get-Content -LiteralPath MANIFEST_PATH -Raw | ConvertFrom-Json
$first=Get-OwnedModulePaths FIXTURE_ROOT $manifest
$firstResolutions=$script:resolved.ToArray()
$firstAssertions=$script:directoryAssertions.ToArray()
$firstChecks=$script:containerChecks.Clone()
$second=Get-OwnedModulePaths FIXTURE_ROOT $manifest
$allChecks=$script:containerChecks.Clone()
$script:resolved.Clear()
$bad=[pscustomobject]@{files=@($manifest.files)+@([pscustomobject]@{path='bin/../outside.dll'})}
$rejected=$false; $diagnostic=''
try { $null=Get-OwnedModulePaths FIXTURE_ROOT $bad } catch {
    $rejected=$true; $diagnostic=$_.Exception.Message
}
@{paths=$first; second_paths=$second; resolutions=$firstResolutions;
  directory_assertions=$firstAssertions; directory_checks=$firstChecks; two_call_checks=$allChecks;
  unsafe_rejected=$rejected; unsafe_diagnostic=$diagnostic; unsafe_resolutions=$script:resolved.ToArray()} |
    ConvertTo-Json -Depth 5 -Compress
"""
            for token, path in (("INSTALLER_PATH", SCRIPTS / "install-offline-bundle.ps1"),
                                ("MANIFEST_PATH", manifest_path), ("FIXTURE_ROOT", root)):
                script = script.replace(token, self.ps_path(path))
            checked = self.run_powershell(script)
            self.assertEqual(checked.returncode, 0, checked.stderr)
            evidence = json.loads(checked.stdout)
            canonical = lambda paths: {path.casefold(): value for path, value in paths.items()}
            self.assertEqual(canonical(evidence["paths"]), expected)
            self.assertEqual(canonical(evidence["second_paths"]), expected)
            self.assertEqual(evidence["resolutions"], manifest_relatives)
            parents = {path for path, is_directory in expected.items() if is_directory}
            self.assertEqual({path.casefold() for path in evidence["directory_assertions"]}, parents)
            self.assertEqual(len(evidence["directory_assertions"]), len(parents))
            self.assertEqual(canonical(evidence["directory_checks"]), dict.fromkeys(parents, 1))
            self.assertEqual(canonical(evidence["two_call_checks"]), dict.fromkeys(parents, 2))
            self.assertTrue(evidence["unsafe_rejected"])
            self.assertIn("unsafe path", evidence["unsafe_diagnostic"])
            self.assertEqual(evidence["unsafe_resolutions"], manifest_relatives + ["bin/../outside.dll"])

    def lifecycle_fixture(self, installer_template=None):
        pwsh = shutil.which("pwsh")
        if pwsh is None:
            self.skipTest("PowerShell is unavailable")
        fixture = self.fixture()
        self.addCleanup(self.cleanup_fixture, fixture[0])
        _, root, inventory, source_kit, allowlist, *_ = fixture
        stage.stage_bundle(inventory, allowlist, source_kit, root, root / "out", "lifecycle",
                           installer_template=installer_template)
        bundle = root / "out" / "lifecycle"
        target = root / "installed"

        def run(action):
            return subprocess.run(
                [pwsh, "-NoLogo", "-NoProfile", "-NonInteractive", "-File",
                 str(bundle / "install-offline-bundle.ps1"), "-InstallRoot", str(target),
                 "-Action", action], capture_output=True, text=True, check=False,
                creationflags=getattr(subprocess, "CREATE_NO_WINDOW", 0),
            )

        installed = run("Install")
        self.assertEqual(installed.returncode, 0, installed.stderr)
        return bundle, target, run

    def test_lifecycle_rejects_unowned_files_without_losing_user_data(self):
        for action, relative in ((a, p) for a in ("Repair", "Uninstall")
                                 for p in ("projects/valuable.bldproj", "bin/valuable.bldproj")):
            with self.subTest(action=action, path=relative):
                _, target, run = self.lifecycle_fixture()
                project = target / relative
                project.parent.mkdir(exist_ok=True)
                self.make_fixture_path_writable(project.parent)
                project.write_bytes(b"user project")
                before = {p.relative_to(target): p.read_bytes()
                          for p in target.rglob("*") if p.is_file()}
                before_acls = self.runtime_acls(target)
                rejected = run(action)
                self.assertNotEqual(rejected.returncode, 0, rejected.stdout)
                self.assertIn("unowned", rejected.stderr)
                self.assertEqual(before, {p.relative_to(target): p.read_bytes()
                                         for p in target.rglob("*") if p.is_file()})
                self.assertEqual(before_acls, self.runtime_acls(target))

    def test_lifecycle_rejects_a_modified_ownership_manifest(self):
        for action in ("Repair", "Uninstall"):
            with self.subTest(action=action):
                _, target, run = self.lifecycle_fixture()
                marker = target / "runtime-manifest.json"
                manifest = json.loads(marker.read_text(encoding="utf-8"))
                manifest["files"] = manifest["files"][:1]
                write_json(marker, manifest)
                before = marker.read_bytes()
                rejected = run(action)
                self.assertNotEqual(rejected.returncode, 0, rejected.stdout)
                self.assertIn("manifest does not match", rejected.stderr)
                self.assertEqual(marker.read_bytes(), before)

    def test_uninstall_never_executes_the_installed_verifier(self):
        _, target, run = self.lifecycle_fixture()
        sentinel = target.parent / "executed.txt"
        (target / "verify-offline-bundle.ps1").write_text(
            "[IO.File]::WriteAllText((Join-Path (Split-Path $PSScriptRoot -Parent) "
            "'executed.txt'), 'executed')\nexit 0\n", encoding="utf-8")
        self.make_fixture_path_writable(target / "bin" / "vertex.exe")
        (target / "bin" / "vertex.exe").write_bytes(b"damaged")
        rejected = run("Uninstall")
        self.assertNotEqual(rejected.returncode, 0, rejected.stdout)
        self.assertFalse(sentinel.exists())
        self.assertTrue(target.exists())
        repaired = run("Repair")
        self.assertEqual(repaired.returncode, 0, repaired.stderr)
        self.assertFalse(sentinel.exists())
        removed = run("Uninstall")
        self.assertEqual(removed.returncode, 0, removed.stderr)
        self.assertFalse(target.exists())

    def test_lifecycle_preserves_unowned_empty_directories(self):
        _, target, run = self.lifecycle_fixture()
        empty = target / "user-created-empty-directory"
        empty.mkdir()
        for action in ("Repair", "Uninstall"):
            with self.subTest(action=action):
                rejected = run(action)
                self.assertNotEqual(rejected.returncode, 0, rejected.stdout)
                self.assertIn("unowned", rejected.stderr)
                self.assertTrue(empty.is_dir())

    @unittest.skipUnless(os.name == "nt", "Windows runtime ACL contract")
    def test_installed_module_tree_is_readonly_and_lifecycle_remains_usable(self):
        # This catches inherited user write access, omitted nested module ACLs,
        # an overly broad AppContainer grant, and cleanup of a frozen runtime.
        import ctypes
        from ctypes import wintypes

        fixture = self.fixture()
        self.addCleanup(self.cleanup_fixture, fixture[0])
        _, root, inventory_path, source_kit, allowlist, *_ = fixture
        inventory = json.loads(inventory_path.read_text(encoding="utf-8"))
        for relative in ("bin/helpers/deep.dll", "plugins/platforms/qwindows.dll"):
            source = root / "build" / pathlib.PurePosixPath(relative)
            source.parent.mkdir(parents=True, exist_ok=True)
            source.write_bytes(b"module")
            inventory["binaries"].append({
                "name": source.name, "path": source.relative_to(root).as_posix(),
                "destination": relative, "sha256": digest(source), "component_id": "vertex",
            })
        write_json(inventory_path, inventory)
        stage.stage_bundle(inventory_path, allowlist, source_kit, root, root / "out", "acl")
        bundle = root / "out" / "acl"
        target = root / "installed"

        def run(action):
            return subprocess.run(
                [shutil.which("pwsh"), "-NoLogo", "-NoProfile", "-NonInteractive", "-File",
                 str(bundle / "install-offline-bundle.ps1"), "-InstallRoot", str(target), "-Action", action],
                capture_output=True, text=True, check=False,
                creationflags=getattr(subprocess, "CREATE_NO_WINDOW", 0))

        installed = run("Install")
        self.assertEqual(installed.returncode, 0, installed.stderr)
        kernel = ctypes.WinDLL("kernel32", use_last_error=True)
        kernel.CreateFileW.argtypes = [wintypes.LPCWSTR, wintypes.DWORD, wintypes.DWORD,
                                      wintypes.LPVOID, wintypes.DWORD, wintypes.DWORD, wintypes.HANDLE]
        kernel.CreateFileW.restype = wintypes.HANDLE
        kernel.CloseHandle.argtypes = [wintypes.HANDLE]
        userenv = ctypes.WinDLL("userenv")
        sid = wintypes.LPVOID()
        derive = userenv.DeriveAppContainerSidFromAppContainerName
        derive.argtypes = [wintypes.LPCWSTR, ctypes.POINTER(wintypes.LPVOID)]
        self.assertEqual(derive("Vertex.ImportWorker", ctypes.byref(sid)), 0)
        advapi = ctypes.WinDLL("advapi32")
        advapi.ConvertSidToStringSidW.argtypes = [wintypes.LPVOID, ctypes.POINTER(wintypes.LPWSTR)]
        advapi.FreeSid.argtypes = [wintypes.LPVOID]
        kernel.LocalFree.argtypes = [wintypes.LPVOID]
        sid_string = wintypes.LPWSTR()
        try:
            self.assertTrue(advapi.ConvertSidToStringSidW(sid, ctypes.byref(sid_string)))
            worker_sid = sid_string.value
        finally:
            if sid_string:
                kernel.LocalFree(sid_string)
            advapi.FreeSid(sid)

        def assert_readonly():
            for path in (target / "bin", target / "bin/helpers", target / "plugins",
                         target / "plugins/platforms", target / "bin/vertex.exe",
                         target / "bin/helpers/deep.dll", target / "plugins/platforms/qwindows.dll"):
                accesses = (0x2, 0x4, 0x10000, 0x10006) if path.is_dir() else (0x40000000,)
                for access in accesses:
                    handle = kernel.CreateFileW(str(path), access, 7, None, 3, 0x02200000, None)
                    error = ctypes.get_last_error()
                    if handle != ctypes.c_void_p(-1).value:
                        kernel.CloseHandle(handle)
                    self.assertEqual(handle, ctypes.c_void_p(-1).value,
                                     f"runtime permits access 0x{access:x}: {path}")
                    self.assertEqual(error, 5, str(path))
            checked = self.run_powershell(
                "$ErrorActionPreference='Stop'; "
                f"$paths=@({self.ps_path(target / 'bin')},{self.ps_path(target / 'plugins/platforms/qwindows.dll')}); "
                "@($paths | ForEach-Object { $acl=Get-Acl -LiteralPath $_; "
                "@{protected=$acl.AreAccessRulesProtected; rules=@($acl.Access | ForEach-Object { "
                "@{sid=$_.IdentityReference.Translate([Security.Principal.SecurityIdentifier]).Value; "
                "rights=[int]$_.FileSystemRights; type=$_.AccessControlType.ToString(); inherited=$_.IsInherited} })} }) | ConvertTo-Json -Depth 5")
            self.assertEqual(checked.returncode, 0, checked.stderr)
            for acl in json.loads(checked.stdout):
                self.assertTrue(acl["protected"])
                self.assertFalse(any(rule["inherited"] for rule in acl["rules"]))
                containers = [rule for rule in acl["rules"] if rule["sid"].startswith("S-1-15-")]
                self.assertEqual(len(containers), 1)
                self.assertEqual(containers[0]["sid"], worker_sid)
                # Specific AppContainer SID, never ALL APPLICATION PACKAGES.
                self.assertTrue(containers[0]["sid"].startswith("S-1-15-2-"))
                self.assertNotEqual(containers[0]["sid"], "S-1-15-2-1")
                self.assertEqual(containers[0]["rights"], 0x1200a9)

        assert_readonly()
        damaged = target / "bin/helpers/deep.dll"
        self.make_fixture_path_writable(damaged)
        damaged.write_bytes(b"damaged")
        repaired = run("Repair")
        self.assertEqual(repaired.returncode, 0, repaired.stderr)
        self.assertEqual(damaged.read_bytes(), b"module")
        assert_readonly()
        removed = run("Uninstall")
        self.assertEqual(removed.returncode, 0, removed.stderr)
        self.assertFalse(target.exists())
        self.assertFalse(any(p.name.startswith(".installed.") for p in root.iterdir()))

    @unittest.skipUnless(os.name == "nt", "Windows runtime ACL rollback")
    def test_module_acl_failure_rolls_back_repair(self):
        # Inject a failure after ACL finalization to exercise the real rollback
        # with an already frozen publication and frozen original backup.
        with tempfile.TemporaryDirectory() as scratch:
            template = pathlib.Path(scratch) / "installer.ps1"
            source = (SCRIPTS / "install-offline-bundle.ps1").read_text(encoding="utf-8")
            anchor = "Protect-OwnedModuleTree $targetRoot $publishedInstall.Manifest"
            self.assertEqual(source.count(anchor), 1)
            template.write_text(source.replace(
                anchor, anchor + "\n        if ($Action -eq 'Repair') { throw 'injected ACL finalization failure' }"),
                encoding="utf-8")
            _, target, run = self.lifecycle_fixture(template)
            before = {p.relative_to(target): p.read_bytes() for p in target.rglob("*") if p.is_file()}
            before_acls = self.runtime_acls(target)
            rejected = run("Repair")
            self.assertNotEqual(rejected.returncode, 0, rejected.stdout)
            self.assertIn("injected ACL finalization failure", rejected.stderr)
            self.assertEqual(before, {p.relative_to(target): p.read_bytes()
                                      for p in target.rglob("*") if p.is_file()})
            self.assertEqual(before_acls, self.runtime_acls(target))
            self.assertFalse(any(p.name.startswith(".installed.") for p in target.parent.iterdir()))
            removed = run("Uninstall")
            self.assertEqual(removed.returncode, 0, removed.stderr)

    @unittest.skipUnless(os.name == "nt", "Windows runtime ACL rollback")
    def test_module_acl_failure_restores_existing_empty_destination(self):
        fixture = self.fixture()
        self.addCleanup(self.cleanup_fixture, fixture[0])
        _, root, inventory, source_kit, allowlist, *_ = fixture
        template = root / "installer.ps1"
        source = (SCRIPTS / "install-offline-bundle.ps1").read_text(encoding="utf-8")
        anchor = "Protect-OwnedModuleTree $targetRoot $publishedInstall.Manifest"
        self.assertEqual(source.count(anchor), 1)
        template.write_text(source.replace(anchor, anchor + "\n        throw 'injected ACL finalization failure'"),
                            encoding="utf-8")
        stage.stage_bundle(inventory, allowlist, source_kit, root, root / "out", "rollback",
                           installer_template=template)
        target = root / "installed"
        target.mkdir()
        checked = subprocess.run(
            [shutil.which("pwsh"), "-NoLogo", "-NoProfile", "-NonInteractive", "-File",
             str(root / "out/rollback/install-offline-bundle.ps1"), "-InstallRoot", str(target)],
            capture_output=True, text=True, check=False,
            creationflags=getattr(subprocess, "CREATE_NO_WINDOW", 0))
        self.assertNotEqual(checked.returncode, 0, checked.stdout)
        self.assertIn("injected ACL finalization failure", checked.stderr)
        self.assertTrue(target.is_dir())
        self.assertEqual(list(target.iterdir()), [])
        self.assertFalse(any(p.name.startswith(".installed.") for p in root.iterdir()))

    @unittest.skipUnless(os.name == "nt", "Windows runtime ACL publication race")
    def test_module_freeze_preserves_a_late_unowned_file_and_its_acl(self):
        with tempfile.TemporaryDirectory() as scratch:
            template = pathlib.Path(scratch) / "installer.ps1"
            source = (SCRIPTS / "install-offline-bundle.ps1").read_text(encoding="utf-8")
            anchor = "Protect-OwnedModuleTree $targetRoot $publishedInstall.Manifest"
            injected = """if ($Action -eq 'Repair') {
            $latePath = Join-Path $targetRoot 'bin/late-user.txt'
            [IO.File]::WriteAllText($latePath, 'user content')
            Write-Output ('LATE_ACL:' + (Get-Acl -LiteralPath $latePath).Sddl)
        }
        """
            self.assertEqual(source.count(anchor), 1)
            template.write_text(source.replace(anchor, injected + anchor), encoding="utf-8")
            _, target, run = self.lifecycle_fixture(template)
            original = {p.relative_to(target): p.read_bytes()
                        for p in target.rglob("*") if p.is_file()}
            rejected = run("Repair")
            self.assertNotEqual(rejected.returncode, 0, rejected.stdout)
            self.assertIn("unowned", rejected.stderr)
            self.assertIn("rollback preserved both", rejected.stderr)
            late = target / "bin/late-user.txt"
            self.assertEqual(late.read_text(encoding="utf-8"), "user content")
            before_acl = next(line.removeprefix("LATE_ACL:") for line in rejected.stdout.splitlines()
                              if line.startswith("LATE_ACL:"))
            self.assertEqual(self.runtime_acls(target)[str(late)], before_acl)
            backups = list(target.parent.glob(".installed.backup-*"))
            self.assertEqual(len(backups), 1)
            self.assertEqual(original, {p.relative_to(backups[0]): p.read_bytes()
                                        for p in backups[0].rglob("*") if p.is_file()})

    @unittest.skipUnless(os.name == "nt", "Windows hard-link ownership")
    def test_lifecycle_rejects_module_hard_links_before_changing_acls(self):
        for action in ("Repair", "Uninstall"):
            with self.subTest(action=action):
                _, target, run = self.lifecycle_fixture()
                module = target / "bin/dependency.dll"
                outside = target.parent / "outside.dll"
                self.make_fixture_path_writable(module)
                os.link(module, outside)
                before = self.runtime_acls(target)
                rejected = run(action)
                self.assertNotEqual(rejected.returncode, 0, rejected.stdout)
                self.assertIn("hard link", rejected.stderr)
                self.assertEqual(before, self.runtime_acls(target))
                self.assertEqual(outside.read_bytes(), b"dependency")
                self.assertEqual(module.stat().st_nlink, 2)

    @unittest.skipUnless(os.name == "nt", "Windows partial-cleanup rollback")
    def test_partial_backup_cleanup_restores_readonly_modules_and_preserves_user_file(self):
        with tempfile.TemporaryDirectory() as scratch:
            template = pathlib.Path(scratch) / "installer.ps1"
            source = (SCRIPTS / "install-offline-bundle.ps1").read_text(encoding="utf-8")
            anchor = "Enable-OwnedModuleRemoval $RootPath $Manifest"
            injected = """
    if ($RootPath -eq $backupRoot) {
        [IO.File]::WriteAllText((Join-Path $RootPath 'bin/late-user.txt'), 'user content')
    }
"""
            self.assertEqual(source.count(anchor), 1)
            template.write_text(source.replace(anchor, anchor + injected), encoding="utf-8")
            _, target, run = self.lifecycle_fixture(template)
            rejected = run("Repair")
            self.assertNotEqual(rejected.returncode, 0, rejected.stdout)
            self.assertIn("repair preserved content", rejected.stderr)
            self.assertEqual((target / "bin/late-user.txt").read_text(encoding="utf-8"), "user content")
            self.assertEqual((target / "bin/vertex.exe").read_bytes(), b"application")
            self.assertEqual((target / "bin/dependency.dll").read_bytes(), b"dependency")
            with self.assertRaises(PermissionError):
                (target / "bin/vertex.exe").write_bytes(b"forbidden")
            with self.assertRaises(PermissionError):
                (target / "bin/forbidden.dll").write_bytes(b"forbidden")
            self.assertFalse(any(p.name.startswith(".installed.") for p in target.parent.iterdir()))

    @unittest.skipUnless(os.name == "nt", "Windows native ACL propagation")
    def test_native_module_acl_update_preserves_late_child_security_descriptor(self):
        with tempfile.TemporaryDirectory() as scratch:
            template = pathlib.Path(scratch) / "installer.ps1"
            source = (SCRIPTS / "install-offline-bundle.ps1").read_text(encoding="utf-8")
            # Insert after the rescan and link preflight, immediately before
            # module ACL writes. This models the remaining publication window.
            anchor = "    $systemSid = [Security.Principal.SecurityIdentifier]::new('S-1-5-18')"
            injected = """    if ($Action -eq 'Repair') {
        $latePath = Join-Path $RootPath 'bin/late-user.txt'
        [IO.File]::WriteAllText($latePath, 'user content')
""" + self.NATIVE_ACL_SNAPSHOT + """
        Write-Output ('NATIVE_ACL:' + [FixtureNativeAcl]::Read($latePath))
    }
"""
            self.assertEqual(source.count(anchor), 1)
            template.write_text(source.replace(anchor, injected + anchor), encoding="utf-8")
            _, target, run = self.lifecycle_fixture(template)
            rejected = run("Repair")
            self.assertNotEqual(rejected.returncode, 0, rejected.stdout)
            self.assertIn("unowned", rejected.stderr)
            late = target / "bin/late-user.txt"
            self.assertEqual(late.read_text(encoding="utf-8"), "user content")
            before = next(line.removeprefix("NATIVE_ACL:") for line in rejected.stdout.splitlines()
                          if line.startswith("NATIVE_ACL:"))
            after = self.run_powershell(self.NATIVE_ACL_SNAPSHOT +
                                       f"\n[FixtureNativeAcl]::Read({self.ps_path(late)})")
            self.assertEqual(after.returncode, 0, after.stderr)
            self.assertEqual(before, after.stdout.strip())
            # Preservation includes usable access, not merely descriptor text.
            late.write_bytes(b"user content still writable")
            self.assertEqual(late.read_bytes(), b"user content still writable")
            self.assertEqual(len(list(target.parent.glob(".installed.backup-*"))), 1)

    @unittest.skipUnless(os.name == "nt", "Windows install-root ACL preservation")
    def test_install_root_delete_child_deny_preserves_unknown_native_descriptor(self):
        with tempfile.TemporaryDirectory() as scratch:
            template = pathlib.Path(scratch) / "installer.ps1"
            source = (SCRIPTS / "install-offline-bundle.ps1").read_text(encoding="utf-8")
            anchor = "    Protect-ModuleParentDeletion $RootPath"
            injected = """    if ($Action -eq 'Repair') {
        $latePath = Join-Path $RootPath 'late-user.txt'
        [IO.File]::WriteAllText($latePath, 'user content')
""" + self.NATIVE_ACL_SNAPSHOT + """
        Write-Output ('NATIVE_ACL:' + [FixtureNativeAcl]::Read($latePath))
    }
"""
            self.assertEqual(source.count(anchor), 1)
            template.write_text(source.replace(anchor, injected + anchor), encoding="utf-8")
            _, target, run = self.lifecycle_fixture(template)
            parent_before = self.run_powershell(self.NATIVE_ACL_SNAPSHOT +
                                               f"\n[FixtureNativeAcl]::Read({self.ps_path(target.parent)})")
            self.assertEqual(parent_before.returncode, 0, parent_before.stderr)
            rejected = run("Repair")
            self.assertNotEqual(rejected.returncode, 0, rejected.stdout)
            self.assertIn("unowned", rejected.stderr)
            late = target / "late-user.txt"
            before = next(line.removeprefix("NATIVE_ACL:") for line in rejected.stdout.splitlines()
                          if line.startswith("NATIVE_ACL:"))
            after = self.run_powershell(self.NATIVE_ACL_SNAPSHOT +
                                       f"\n[FixtureNativeAcl]::Read({self.ps_path(late)})")
            self.assertEqual(after.returncode, 0, after.stderr)
            self.assertEqual(before, after.stdout.strip())
            late.write_bytes(b"user content still writable")
            self.assertEqual(late.read_bytes(), b"user content still writable")
            parent_after = self.run_powershell(self.NATIVE_ACL_SNAPSHOT +
                                              f"\n[FixtureNativeAcl]::Read({self.ps_path(target.parent)})")
            self.assertEqual(parent_after.returncode, 0, parent_after.stderr)
            self.assertEqual(parent_before.stdout, parent_after.stdout)

    @unittest.skipUnless(os.name == "nt", "Windows thrown-cleanup rollback")
    def test_thrown_backup_cleanup_restores_complete_readonly_runtime(self):
        with tempfile.TemporaryDirectory() as scratch:
            template = pathlib.Path(scratch) / "installer.ps1"
            source = (SCRIPTS / "install-offline-bundle.ps1").read_text(encoding="utf-8")
            anchor = "Enable-OwnedModuleRemoval $RootPath $Manifest"
            self.assertEqual(source.count(anchor), 1)
            template.write_text(source.replace(anchor, anchor + """
    if ($RootPath -eq $backupRoot) {
        [IO.File]::Delete((Join-Path $RootPath 'bin/dependency.dll'))
        throw 'injected backup cleanup exception'
    }
"""), encoding="utf-8")
            _, target, run = self.lifecycle_fixture(template)
            before = {p.relative_to(target): p.read_bytes() for p in target.rglob("*") if p.is_file()}
            before_acls = self.runtime_acls(target)
            rejected = run("Repair")
            self.assertNotEqual(rejected.returncode, 0, rejected.stdout)
            self.assertIn("injected backup", rejected.stderr)
            self.assertIn("cleanup exception", rejected.stderr)
            self.assertEqual(before, {p.relative_to(target): p.read_bytes()
                                      for p in target.rglob("*") if p.is_file()})
            self.assertEqual(before_acls, self.runtime_acls(target))
            self.assertFalse(any(p.name.startswith(".installed.") for p in target.parent.iterdir()))

    @unittest.skipUnless(os.name == "nt", "Windows preserved inherited ACL recovery")
    def test_unsafe_backup_refreeze_retains_protected_publication_and_unknown_native_acl(self):
        with tempfile.TemporaryDirectory() as scratch:
            template = pathlib.Path(scratch) / "installer.ps1"
            source = (SCRIPTS / "install-offline-bundle.ps1").read_text(encoding="utf-8")
            anchor = "Enable-OwnedModuleRemoval $RootPath $Manifest"
            injected = """
    if ($RootPath -eq $backupRoot) {
        # Model a legacy or concurrently recreated parent with inheritable
        # grants. The new user file must retain those inherited ACE flags.
        $moduleRoot = Join-Path $RootPath 'bin'
        $acl = [Security.AccessControl.DirectorySecurity]::new()
        $acl.SetSecurityDescriptorSddlForm((Get-Acl -LiteralPath $moduleRoot).Sddl,
            [Security.AccessControl.AccessControlSections]::Access)
        $sid = [Security.Principal.WindowsIdentity]::GetCurrent().User
        $acl.AddAccessRule([Security.AccessControl.FileSystemAccessRule]::new(
            $sid, 'FullControl', 'ContainerInherit,ObjectInherit', 'None', 'Allow'))
        Set-ModuleAccessAcl $moduleRoot $acl $true
        $latePath = Join-Path $moduleRoot 'late-user.txt'
        [IO.File]::WriteAllText($latePath, 'user content')
        if (-not [VertexOfflineRuntimeSecurity]::HasInheritedDaclAce($latePath)) {
            throw 'fixture did not produce inherited ACEs'
        }
""" + self.NATIVE_ACL_SNAPSHOT + """
        Write-Host ('NATIVE_ACL:' + [FixtureNativeAcl]::Read($latePath))
    }
"""
            self.assertEqual(source.count(anchor), 1)
            template.write_text(source.replace(anchor, anchor + injected), encoding="utf-8")
            _, target, run = self.lifecycle_fixture(template)
            rejected = run("Repair")
            self.assertNotEqual(rejected.returncode, 0, rejected.stdout)
            self.assertIn("cannot safely change module permissions", rejected.stderr)
            self.assertIn("verified protected publication retained", rejected.stderr)
            backups = list(target.parent.glob(".installed.backup-*"))
            self.assertEqual(len(backups), 1)
            late = backups[0] / "bin/late-user.txt"
            self.assertEqual(late.read_text(encoding="utf-8"), "user content")
            before = next(line.removeprefix("NATIVE_ACL:") for line in rejected.stdout.splitlines()
                          if line.startswith("NATIVE_ACL:"))
            after = self.run_powershell(self.NATIVE_ACL_SNAPSHOT +
                                       f"\n[FixtureNativeAcl]::Read({self.ps_path(late)})")
            self.assertEqual(after.returncode, 0, after.stderr)
            self.assertEqual(before, after.stdout.strip())
            late.write_bytes(b"still writable")
            self.assertEqual((target / "bin/vertex.exe").read_bytes(), b"application")
            with self.assertRaises(PermissionError):
                (target / "bin/vertex.exe").write_bytes(b"forbidden")

    def test_strict_bundle_verifier_rejects_unlisted_files(self):
        fixture = self.fixture()
        self.addCleanup(self.cleanup_fixture, fixture[0])
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
        self.addCleanup(self.cleanup_fixture, fixture[0])
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
        self.addCleanup(self.cleanup_fixture, fixture[0])
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
