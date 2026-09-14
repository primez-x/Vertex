import hashlib
import importlib.util
import json
import pathlib
import sqlite3
import tempfile
import unittest
from unittest import mock


ROOT = pathlib.Path(__file__).resolve().parents[1]
SPEC = importlib.util.spec_from_file_location(
    "stage_project_package", ROOT / "scripts" / "stage_project_package.py"
)
stage = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(stage)


def digest(value):
    return hashlib.sha256(value).hexdigest()


def write_json(path, value):
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(value, indent=2) + "\n", encoding="utf-8")


def create_project(path):
    payload = b"reference-image"
    payload_hash = digest(payload)
    path.parent.mkdir(parents=True, exist_ok=True)
    database = sqlite3.connect(path)
    database.executescript(
        """
        CREATE TABLE metadata(key TEXT PRIMARY KEY, value TEXT NOT NULL);
        CREATE TABLE revisions(
            revision INTEGER PRIMARY KEY, parent_revision INTEGER, source_revision INTEGER,
            action TEXT NOT NULL, name TEXT, undo_stack_json TEXT NOT NULL,
            redo_stack_json TEXT NOT NULL);
        CREATE TABLE revision_entities(
            revision INTEGER NOT NULL, id TEXT NOT NULL, type TEXT NOT NULL,
            required INTEGER NOT NULL, properties_json TEXT NOT NULL,
            extensions_json TEXT NOT NULL, PRIMARY KEY(revision,id));
        CREATE TABLE revision_assets(
            revision INTEGER NOT NULL, asset_id TEXT NOT NULL, media_type TEXT NOT NULL,
            sha256 TEXT NOT NULL, metadata_json TEXT NOT NULL, data BLOB NOT NULL,
            PRIMARY KEY(revision,asset_id));
        """
    )
    database.executemany(
        "INSERT INTO metadata VALUES(?,?)",
        [
            ("format_version", "1"),
            ("document_id", "document-1"),
            ("head_revision", "1"),
            ("saved_revision", "1"),
            ("logical_digest", "0" * 64),
        ],
    )
    database.execute(
        "INSERT INTO revisions VALUES(?,?,?,?,?,?,?)",
        (0, None, None, "create", None, "[]", "[]"),
    )
    database.execute(
        "INSERT INTO revisions VALUES(?,?,?,?,?,?,?)",
        (1, 0, 0, "reference", None, "[]", "[]"),
    )
    database.execute(
        "INSERT INTO revision_assets VALUES(?,?,?,?,?,?)",
        (1, "asset-a", "image/png", payload_hash, json.dumps({"role": "underlay"}), payload),
    )
    database.commit()
    database.close()
    return payload, payload_hash


class StageProjectPackageTests(unittest.TestCase):
    def fixture(self):
        temporary = tempfile.TemporaryDirectory()
        root = pathlib.Path(temporary.name)
        project = root / "projects" / "house.bldproj"
        payload, payload_hash = create_project(project)
        resources = root / "packaging" / "project-resources.json"
        template = root / "templates" / "residential.json"
        profile = root / "profiles" / "imperial.json"
        documentation = root / "docs" / "project-format.md"
        template.parent.mkdir(parents=True)
        profile.parent.mkdir(parents=True)
        documentation.parent.mkdir(parents=True)
        template.write_text('{"kind":"template","name":"Residential"}\n', encoding="utf-8")
        profile.write_text('{"units":"imperial"}\n', encoding="utf-8")
        documentation.write_text("Project format reference\n", encoding="utf-8")
        write_json(
            resources,
            {
                "schema_version": 1,
                "entries": [
                    {"kind": "template", "path": "templates/residential.json",
                     "destination": "templates/residential.json", "name": "Residential"},
                    {"kind": "profile", "path": "profiles/imperial.json",
                     "destination": "profiles/imperial.json", "name": "Imperial"},
                    {"kind": "documentation", "path": "docs/project-format.md",
                     "destination": "documentation/project-format.md"},
                ],
            },
        )
        return temporary, root, project, resources, payload, payload_hash

    def test_stages_project_assets_and_explicit_resources(self):
        fixture = self.fixture()
        self.addCleanup(fixture[0].cleanup)
        _, root, project, resources, payload, payload_hash = fixture
        output = root / "out"

        result = stage.stage_project_package(project, root, output, "house",
                                             resources)
        package = output / "house"
        manifest = json.loads((package / "project-package-manifest.json").read_text(encoding="utf-8"))
        self.assertEqual(result, manifest)
        self.assertEqual(manifest["manifest_kind"], "project-package")
        self.assertFalse(manifest["offline_qualified"])
        self.assertEqual(manifest["project"]["source"], "projects/house.bldproj")
        self.assertEqual(manifest["project"]["document_id"], "document-1")
        self.assertEqual(manifest["project"]["revision_count"], 2)
        self.assertEqual(manifest["assets"][0]["sha256"], payload_hash)
        self.assertEqual((package / f"assets/{payload_hash}.bin").read_bytes(), payload)
        self.assertEqual(manifest["summary"]["template_count"], 1)
        self.assertEqual(manifest["summary"]["profile_count"], 1)
        self.assertEqual(manifest["summary"]["documentation_count"], 1)
        self.assertEqual(stage.verify_package(package)["file_count"], len(manifest["files"]))
        self.assertNotIn(str(root), json.dumps(manifest))
        self.assertNotIn("\\", json.dumps(manifest))

    def test_assets_are_deduplicated_across_revisions(self):
        fixture = self.fixture()
        self.addCleanup(fixture[0].cleanup)
        _, root, project, _, _, payload_hash = fixture
        database = sqlite3.connect(project)
        database.execute(
            "INSERT INTO revision_assets VALUES(?,?,?,?,?,?)",
            (0, "old-asset", "image/png", payload_hash, json.dumps({"role": "old"}), b"reference-image"),
        )
        database.commit()
        database.close()
        result = stage.stage_project_package(project, root, root / "out")
        self.assertEqual(result["summary"]["asset_count"], 1)
        self.assertEqual(len(list((root / "out/project-package/assets").glob("*.bin"))), 1)
        self.assertEqual(len(result["assets"][0]["references"]), 2)

    def test_source_change_after_copy_keeps_staged_database_and_assets_consistent(self):
        fixture = self.fixture()
        self.addCleanup(fixture[0].cleanup)
        _, root, project, _, payload, payload_hash = fixture
        original_project = project.read_bytes()
        copy_file = stage._copy_file

        def copy_then_change_source(source, destination, relative):
            result = copy_file(source, destination, relative)
            if source == project:
                database = sqlite3.connect(project)
                try:
                    database.execute("UPDATE metadata SET value=? WHERE key='document_id'",
                                     ("document-replaced",))
                    database.execute("UPDATE revision_assets SET data=?,sha256=?",
                                     (b"updated-asset", digest(b"updated-asset")))
                    database.commit()
                finally:
                    database.close()
            return result

        with mock.patch.object(stage, "_copy_file", side_effect=copy_then_change_source):
            manifest = stage.stage_project_package(project, root, root / "out")
        package = root / "out/project-package"
        self.assertEqual((package / manifest["project"]["path"]).read_bytes(), original_project)
        self.assertNotEqual(project.read_bytes(), original_project)
        self.assertEqual(manifest["project"]["document_id"], "document-1")
        self.assertEqual(manifest["assets"][0]["sha256"], payload_hash)
        self.assertEqual((package / manifest["assets"][0]["path"]).read_bytes(), payload)
        self.assertEqual(stage.verify_package(package)["asset_count"], 1)

    def test_verify_rejects_manifest_metadata_not_matching_packaged_database(self):
        fixture = self.fixture()
        self.addCleanup(fixture[0].cleanup)
        _, root, project, _, _, _ = fixture
        manifest = stage.stage_project_package(project, root, root / "out")
        package = root / "out/project-package"
        for field, changed in (("document_id", "document-replaced"),
                               ("format_version", 2), ("head_revision", 0),
                               ("saved_revision", None), ("logical_digest", "1" * 64),
                               ("revision_count", 3)):
            with self.subTest(field=field):
                original = manifest["project"][field]
                manifest["project"][field] = changed
                write_json(package / stage.DEFAULT_MANIFEST_NAME, manifest)
                with self.assertRaisesRegex(stage.ProjectPackageError, "metadata"):
                    stage.verify_package(package)
                manifest["project"][field] = original

    def test_verify_rejects_asset_records_not_matching_packaged_database(self):
        fixture = self.fixture()
        self.addCleanup(fixture[0].cleanup)
        _, root, project, _, _, _ = fixture
        original = stage.stage_project_package(project, root, root / "out")
        package = root / "out/project-package"
        for change in ("reference", "omitted"):
            with self.subTest(change=change):
                manifest = json.loads(json.dumps(original))
                if change == "reference":
                    manifest["assets"][0]["references"][0]["asset_id"] = "replaced-asset"
                else:
                    manifest["assets"] = []
                    manifest["summary"]["asset_count"] = 0
                write_json(package / stage.DEFAULT_MANIFEST_NAME, manifest)
                with self.assertRaisesRegex(stage.ProjectPackageError, "assets"):
                    stage.verify_package(package)

    def test_verify_rejects_rehashed_asset_payload_not_matching_database(self):
        fixture = self.fixture()
        self.addCleanup(fixture[0].cleanup)
        _, root, project, _, payload, _ = fixture
        manifest = stage.stage_project_package(project, root, root / "out")
        package = root / "out/project-package"
        asset_path = manifest["assets"][0]["path"]
        replacement = b"x" * len(payload)
        (package / asset_path).write_bytes(replacement)
        for record in manifest["files"]:
            if record["path"] == asset_path:
                record["sha256"] = digest(replacement)
        write_json(package / stage.DEFAULT_MANIFEST_NAME, manifest)
        with self.assertRaisesRegex(stage.ProjectPackageError, "asset.*hash"):
            stage.verify_package(package)

    def test_stale_project_asset_is_rejected_without_publishing(self):
        fixture = self.fixture()
        self.addCleanup(fixture[0].cleanup)
        _, root, project, resources, _, _ = fixture
        database = sqlite3.connect(project)
        database.execute("UPDATE revision_assets SET sha256=?", ("0" * 64,))
        database.commit()
        database.close()
        with self.assertRaises(stage.ProjectPackageError) as context:
            stage.stage_project_package(project, root, root / "out", "broken", resources)
        self.assertIn("sha-256", str(context.exception).lower())
        self.assertFalse((root / "out/broken").exists())

    def test_resource_traversal_and_duplicate_destinations_are_rejected(self):
        fixture = self.fixture()
        self.addCleanup(fixture[0].cleanup)
        _, root, project, resources, _, _ = fixture
        data = json.loads(resources.read_text(encoding="utf-8"))
        data["entries"][0]["destination"] = "../escape.json"
        write_json(resources, data)
        with self.assertRaises(stage.ProjectPackageError):
            stage.stage_project_package(project, root, root / "out", "unsafe", resources)
        data["entries"][0]["destination"] = "profiles/imperial.json"
        write_json(resources, data)
        with self.assertRaises(stage.ProjectPackageError) as context:
            stage.stage_project_package(project, root, root / "out", "duplicate", resources)
        self.assertIn("duplicate", str(context.exception).lower())

    def test_verify_rejects_unlisted_or_modified_payload(self):
        fixture = self.fixture()
        self.addCleanup(fixture[0].cleanup)
        _, root, project, _, _, _ = fixture
        package = root / "out" / "project-package"
        stage.stage_project_package(project, root, root / "out")
        (package / "unexpected.txt").write_text("foreign\n", encoding="utf-8")
        with self.assertRaises(stage.ProjectPackageError) as context:
            stage.verify_package(package)
        self.assertIn("unlisted", str(context.exception).lower())

    def test_restores_verified_package_with_project_and_resources(self):
        fixture = self.fixture()
        self.addCleanup(fixture[0].cleanup)
        _, root, project, resources, payload, payload_hash = fixture
        package = root / "out" / "house"
        stage.stage_project_package(project, root, root / "out", "house", resources)

        result = stage.restore_project_package(package, root / "restored", "copy")
        restored = root / "restored" / "copy"
        self.assertEqual(result["manifest_kind"], "project-package")
        self.assertEqual(result["project_path"], "project/house.bldproj")
        self.assertEqual(result["resource_count"], 3)
        self.assertEqual((restored / "project/house.bldproj").read_bytes(), project.read_bytes())
        self.assertEqual((restored / f"assets/{payload_hash}.bin").read_bytes(), payload)
        self.assertEqual((restored / "templates/residential.json").read_text(encoding="utf-8"),
                         '{"kind":"template","name":"Residential"}\n')
        self.assertEqual(stage.verify_package(restored)["file_count"],
                         stage.verify_package(package)["file_count"])

    def test_restore_rejects_existing_or_unsafe_destination(self):
        fixture = self.fixture()
        self.addCleanup(fixture[0].cleanup)
        _, root, project, _, _, _ = fixture
        package = root / "out" / "project-package"
        stage.stage_project_package(project, root, root / "out")
        destination_root = root / "restored"
        destination_root.mkdir()
        with self.assertRaises(stage.ProjectPackageError):
            stage.restore_project_package(package, destination_root, "../escape")
        stage.restore_project_package(package, destination_root, "copy")
        with self.assertRaises(stage.ProjectPackageError) as context:
            stage.restore_project_package(package, destination_root, "copy")
        self.assertIn("already exists", str(context.exception).lower())

    def test_stages_windows_project_names_that_need_uri_encoding(self):
        fixture = self.fixture()
        self.addCleanup(fixture[0].cleanup)
        _, root, project, _, _, _ = fixture

        for index, filename in enumerate((
            "house#1.bldproj",
            "house%231.bldproj",
            "house with spaces é.bldproj",
        )):
            with self.subTest(filename=filename):
                special_project = project.with_name(filename)
                create_project(special_project)
                project_bytes = special_project.read_bytes()
                output = root / f"out-special-{index}"
                manifest = stage.stage_project_package(
                    special_project, root, output, "project-package"
                )
                package = output / "project-package"
                self.assertEqual(
                    manifest["project"]["source"], f"projects/{filename}"
                )
                self.assertEqual(stage.verify_package(package)["file_count"],
                                 len(manifest["files"]))
                payload_files = {
                    path.relative_to(package).as_posix()
                    for path in package.rglob("*")
                    if path.is_file()
                }
                self.assertEqual(
                    payload_files,
                    {record["path"] for record in manifest["files"]}
                    | {stage.DEFAULT_MANIFEST_NAME},
                )
                restored = root / f"restored-{index}"
                stage.restore_project_package(package, root, restored.name)
                self.assertEqual(
                    (restored / "project" / filename).read_bytes(), project_bytes
                )

    def test_inspecting_missing_project_never_creates_a_database(self):
        fixture = self.fixture()
        self.addCleanup(fixture[0].cleanup)
        _, root, _, _, _, _ = fixture
        missing = root / "projects" / "missing#project.bldproj"
        with self.assertRaises(stage.ProjectPackageError):
            stage._inspect_project(missing, lambda digest, data: None)
        self.assertFalse(missing.exists())


if __name__ == "__main__":
    unittest.main()
