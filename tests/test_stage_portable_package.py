import hashlib
import importlib.util
import json
import pathlib
import tempfile
import unittest


ROOT = pathlib.Path(__file__).resolve().parents[1]
SPEC = importlib.util.spec_from_file_location(
    "stage_portable_package", ROOT / "scripts" / "stage_portable_package.py"
)
stager = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(stager)


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def write_json(path, value):
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(value, indent=2) + "\n", encoding="utf-8")


class StagePortablePackageTests(unittest.TestCase):
    def fixture(self):
        directory = tempfile.TemporaryDirectory()
        root = pathlib.Path(directory.name)
        app = root / "build" / "property-studio.exe"
        dependency = root / "build" / "dependency.dll"
        font = root / "assets" / "fonts" / "Inter.ttf"
        notice = root / "third_party" / "NOTICE.txt"
        font_notice = root / "assets" / "fonts" / "OFL.txt"
        for path, contents in (
            (app, b"application"),
            (dependency, b"dependency"),
            (font, b"font"),
            (notice, b"notice"),
            (font_notice, b"font notice"),
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
                "components": [
                    {
                        "id": "property-studio",
                        "kind": "application",
                        "distribution_status": "included",
                        "package": {"name": "Property Studio", "version": "workspace", "license": "Proprietary"},
                        "notices": [{"path": "third_party/NOTICE.txt", "sha256": digest(notice)}],
                    },
                    {
                        "id": "inter-font",
                        "kind": "asset",
                        "distribution_status": "included",
                        "package": {"name": "Inter", "version": "4.1", "license": "OFL-1.1"},
                        "source_inputs": [{"path": "assets/fonts/Inter.ttf", "kind": "file", "sha256": digest(font)}],
                        "notices": [{"path": "assets/fonts/OFL.txt", "sha256": digest(font_notice)}],
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
                "summary": {"installer_qualified": False, "offline_qualified": False},
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
                        "path": "third_party/NOTICE.txt",
                        "destination": "licenses/NOTICE.txt",
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
        output_root = root / "out"
        return directory, root, inventory, allowlist, output_root, app, dependency, font, notice, font_notice

    def test_stages_inventory_binaries_and_allowlisted_assets_and_notices(self):
        fixture = self.fixture()
        self.addCleanup(fixture[0].cleanup)
        _, root, inventory, allowlist, output_root, app, dependency, font, notice, font_notice = fixture

        result = stager.stage_package(inventory, allowlist, root, output_root, "portable")
        package = output_root / "portable"
        manifest_path = package / "portable-package-manifest.json"
        manifest = json.loads(manifest_path.read_text(encoding="utf-8"))

        self.assertEqual(result, manifest)
        self.assertEqual(manifest["audit_status"], "incomplete")
        self.assertFalse(manifest["installer_qualified"])
        self.assertFalse(manifest["offline_qualified"])
        self.assertEqual(manifest["source_inventory"]["sha256"], digest(inventory))
        self.assertEqual(manifest["sbom"]["format"], "SPDX-2.3")
        self.assertEqual(manifest["sbom"]["sha256"], digest(package / "metadata/distribution-sbom.spdx.json"))
        sbom = json.loads((package / "metadata/distribution-sbom.spdx.json").read_text(encoding="utf-8"))
        stager._SBOM.validate_sbom(sbom)
        self.assertEqual(
            {row["path"] for row in manifest["files"]},
            {
                "bin/property-studio.exe",
                "bin/dependency.dll",
                "assets/fonts/Inter.ttf",
                "licenses/NOTICE.txt",
                "licenses/Inter-OFL.txt",
                "metadata/distribution-sbom.spdx.json",
            },
        )
        self.assertEqual((package / "bin/property-studio.exe").read_bytes(), app.read_bytes())
        self.assertEqual((package / "bin/dependency.dll").read_bytes(), dependency.read_bytes())
        self.assertEqual((package / "assets/fonts/Inter.ttf").read_bytes(), font.read_bytes())
        self.assertEqual((package / "licenses/NOTICE.txt").read_bytes(), notice.read_bytes())
        self.assertEqual((package / "licenses/Inter-OFL.txt").read_bytes(), font_notice.read_bytes())
        self.assertNotIn(str(root), json.dumps(manifest))
        self.assertNotIn("\\", json.dumps(manifest))

    def test_traversal_is_rejected_before_writing_output(self):
        fixture = self.fixture()
        self.addCleanup(fixture[0].cleanup)
        _, root, inventory, allowlist, output_root, *_ = fixture
        data = json.loads(allowlist.read_text(encoding="utf-8"))
        data["entries"][0]["destination"] = "../escaped/Inter.ttf"
        write_json(allowlist, data)

        with self.assertRaises(stager.StagingError) as context:
            stager.stage_package(inventory, allowlist, root, output_root, "portable")

        self.assertIn("unsafe", str(context.exception).lower())
        self.assertFalse(output_root.exists())

    def test_duplicate_destinations_are_rejected_case_insensitively(self):
        fixture = self.fixture()
        self.addCleanup(fixture[0].cleanup)
        _, root, inventory, allowlist, output_root, *_ = fixture
        data = json.loads(allowlist.read_text(encoding="utf-8"))
        data["entries"][1]["destination"] = "BIN/PROPERTY-STUDIO.EXE"
        write_json(allowlist, data)

        with self.assertRaises(stager.StagingError) as context:
            stager.stage_package(inventory, allowlist, root, output_root, "portable")

        self.assertIn("duplicate destination", str(context.exception).lower())
        self.assertFalse(output_root.exists())

    def test_missing_input_is_rejected_without_developer_machine_fallback(self):
        fixture = self.fixture()
        self.addCleanup(fixture[0].cleanup)
        _, root, inventory, allowlist, output_root, app, *_ = fixture
        app.unlink()

        with self.assertRaises(stager.StagingError) as context:
            stager.stage_package(inventory, allowlist, root, output_root, "portable")

        self.assertIn("missing", str(context.exception).lower())
        self.assertFalse(output_root.exists())

    def test_unknown_inventory_entry_is_rejected(self):
        fixture = self.fixture()
        self.addCleanup(fixture[0].cleanup)
        _, root, inventory, allowlist, output_root, *_ = fixture
        data = json.loads(allowlist.read_text(encoding="utf-8"))
        data["entries"][0]["inventory_entry"] = "not-in-inventory"
        write_json(allowlist, data)

        with self.assertRaises(stager.StagingError) as context:
            stager.stage_package(inventory, allowlist, root, output_root, "portable")

        self.assertIn("unknown inventory", str(context.exception).lower())
        self.assertFalse(output_root.exists())

    def test_allowlist_requires_an_explicit_source_path(self):
        fixture = self.fixture()
        self.addCleanup(fixture[0].cleanup)
        _, root, inventory, allowlist, output_root, *_ = fixture
        data = json.loads(allowlist.read_text(encoding="utf-8"))
        del data["entries"][0]["path"]
        write_json(allowlist, data)

        with self.assertRaises(stager.StagingError) as context:
            stager.stage_package(inventory, allowlist, root, output_root, "portable")

        self.assertIn("path", str(context.exception).lower())
        self.assertFalse(output_root.exists())

    def test_nonempty_destination_is_rejected_on_restaging(self):
        fixture = self.fixture()
        self.addCleanup(fixture[0].cleanup)
        _, root, inventory, allowlist, output_root, *_ = fixture
        stager.stage_package(inventory, allowlist, root, output_root, "portable")
        with self.assertRaises(stager.StagingError) as context:
            stager.stage_package(inventory, allowlist, root, output_root, "portable")
        self.assertIn("must be empty", str(context.exception).lower())

    def test_copied_bytes_are_verified_against_preflight_hashes(self):
        fixture = self.fixture()
        self.addCleanup(fixture[0].cleanup)
        _, root, inventory, allowlist, output_root, *_ = fixture
        original_copy = stager._copy_file

        def mutate_before_copy(source, destination, relative):
            source.write_bytes(source.read_bytes() + b" changed after preflight")
            original_copy(source, destination, relative)

        stager._copy_file = mutate_before_copy
        self.addCleanup(lambda: setattr(stager, "_copy_file", original_copy))
        with self.assertRaises(stager.StagingError) as context:
            stager.stage_package(inventory, allowlist, root, output_root, "portable")
        self.assertIn("staged hash mismatch", str(context.exception).lower())
        self.assertFalse((output_root / "portable" / "portable-package-manifest.json").exists())


if __name__ == "__main__":
    unittest.main()
