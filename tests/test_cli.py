"""Process-level project format smoke test, including Unicode filenames."""
import json
import hashlib
import pathlib
import sqlite3
import subprocess
import sys
import tempfile
from contextlib import closing


def main():
    executable = pathlib.Path(sys.argv[1]).resolve()
    with tempfile.TemporaryDirectory(prefix="vertex-cli-") as directory:
        root = pathlib.Path(directory)
        project = root / "maison-é-住宅.bldproj"
        def invoke(*arguments, success=True):
            result = subprocess.run(
                [str(executable), *map(str, arguments)], capture_output=True,
                text=True, encoding="utf-8", timeout=15,
                creationflags=subprocess.CREATE_NO_WINDOW if sys.platform == "win32" else 0)
            if (result.returncode == 0) != success:
                raise AssertionError(f"Unexpected exit {result.returncode}: {result.stdout} {result.stderr}")
            return json.loads(result.stdout) if success else result
        catalog = invoke("symbols")
        assert catalog["schema_version"] == 1
        assert catalog["catalog_id"] == "vertex.symbol-catalog"
        assert catalog["catalog_revision"] == 1
        assert catalog["entry_count"] >= 600
        assert catalog["family_count"] >= 300
        assert {"fixtures", "furniture", "plumbing", "commercial"}.issubset(
            catalog["category_counts"]
        )
        assert any(entry["family"] == "toilet" and entry["width_metres"] > 0
                   for entry in catalog["entries"])
        toilets = invoke("symbols", "toilet")
        assert toilets["filtered_entry_count"] >= 18
        assert all("toilet" in entry["family"]
                   for entry in toilets["entries"])
        commercial = invoke("symbols", "", "commercial")
        assert commercial["filtered_entry_count"] >= 36
        assert all(entry["category"] == "commercial" for entry in commercial["entries"])
        upper_commercial = invoke("symbols", "", "COMMERCIAL")
        assert upper_commercial["filtered_entry_count"] == commercial["filtered_entry_count"]
        created = invoke("new", project)
        inspected = invoke("inspect", project)
        assert inspected["document_id"] == created["document_id"]
        assert inspected["revision"] == created["revision"]
        valid = invoke("validate", project)
        assert valid["storage_integrity"] == "valid"
        assert valid["production_or_geometry_certification"] is False
        invoke("new", project, success=False)
        extracted = root / "extracted"
        invoke("extract", project, extracted)
        exported = json.loads((extracted / "project.json").read_text(encoding="utf-8"))
        assert exported["document"]["document_id"] == created["document_id"]
        assert exported["revisions"]
        assert "archive_role" not in exported and "recovery_records" not in exported
        migrated = root / "migrated.bldproj"
        migration = invoke("migrate", project, migrated)
        assert migration["migrated_from"].endswith(project.name)
        assert migration["migrated_to"].endswith(migrated.name)
        assert migration["source_preserved"] is True
        assert migration["source_file_sha256"] != "0" * 64
        assert migration["destination_file_sha256"] == hashlib.sha256(migrated.read_bytes()).hexdigest()
        migrated_info = invoke("inspect", migrated)
        assert migrated_info["document_id"] == created["document_id"]
        assert migrated_info["revision"] == created["revision"]
        invoke("migrate", project, migrated, success=False)
        invoke("extract", project, extracted, success=False)
        corrupt = root / "broken.bldproj"
        corrupt.write_bytes(b"not a database")
        invoke("validate", corrupt, success=False)

        # The archive executable emits independent, valid v4/v6 fixtures plus
        # opaque and corrupt cases for end-to-end CLI coverage.
        if len(sys.argv) > 2:
            archive_emitter = pathlib.Path(sys.argv[2]).resolve()
            archive_fixtures = root / "archive-fixtures"
            archive_fixtures.mkdir()
            emitted = subprocess.run(
                [str(archive_emitter), "--emit-fixtures", str(archive_fixtures)],
                capture_output=True, text=True, encoding="utf-8", timeout=30,
                creationflags=subprocess.CREATE_NO_WINDOW if sys.platform == "win32" else 0)
            if emitted.returncode != 0:
                raise AssertionError(f"Archive fixture emission failed: {emitted.stdout} {emitted.stderr}")

            def archive_manifest(directory):
                return json.loads((directory / "project.json").read_text(encoding="utf-8"))

            def archive_payload_bytes(directory):
                return {str(path.relative_to(directory)): path.read_bytes()
                        for path in sorted(directory.rglob("*")) if path.is_file()}

            for fixture_name, expected_role in (
                    ("ordinary-v4.bldproj", "ordinary"),
                    ("recovery-v4.bldproj", "recovery_copy"),
                    ("ordinary-v6.bldproj", "ordinary")):
                source = archive_fixtures / fixture_name
                source_bytes = source.read_bytes()
                source_hash = hashlib.sha256(source_bytes).hexdigest()
                inspected_archive = invoke("inspect", source)
                assert inspected_archive["archive_role"] == expected_role
                assert inspected_archive["editable"] is True
                assert inspected_archive["validation_complete"] is True
                assert inspected_archive["recovery_summary"]["status"] == "supported"
                assert inspected_archive["recovery_summary"]["record_count"] >= 2
                assert "workspace_history" in inspected_archive["recovery_summary"]["record_kinds"]
                validated_archive = invoke("validate", source)
                assert validated_archive["archive_role"] == expected_role
                assert validated_archive["storage_integrity"] == "valid"
                assert validated_archive["validation_complete"] is True

                extracted_source = root / (fixture_name + "-extracted")
                invoke("extract", source, extracted_source)
                source_manifest = archive_manifest(extracted_source)
                assert source_manifest["archive_role"] == expected_role
                assert source_manifest["exchange_version"] == 12
                assert source_manifest["recovery_records"]

                destination = root / (fixture_name + "-migrated.bldproj")
                migration = invoke("migrate", source, destination)
                with closing(sqlite3.connect(destination)) as database:
                    stored_version = database.execute("PRAGMA user_version").fetchone()[0]
                assert migration["format_version"] == stored_version
                assert migration["archive_role"] == expected_role
                assert migration["source_file_sha256"] == source_hash
                assert migration["source_preserved"] is True
                assert source.read_bytes() == source_bytes
                migrated_info = invoke("inspect", destination)
                assert migrated_info["archive_role"] == expected_role
                assert migrated_info["recovery_summary"] == inspected_archive["recovery_summary"]

                extracted_migrated = root / (fixture_name + "-migrated-extracted")
                invoke("extract", destination, extracted_migrated)
                migrated_manifest = archive_manifest(extracted_migrated)
                for key in ("document", "revisions", "archive_role", "recovery_records"):
                    assert migrated_manifest[key] == source_manifest[key], key
                assert archive_payload_bytes(extracted_migrated) == archive_payload_bytes(extracted_source)

                destination_bytes = destination.read_bytes()
                invoke("migrate", source, destination, success=False)
                assert destination.read_bytes() == destination_bytes
                extracted_metadata = (extracted_source / "project.json").read_bytes()
                invoke("extract", source, extracted_source, success=False)
                assert (extracted_source / "project.json").read_bytes() == extracted_metadata
                assert source.read_bytes() == source_bytes

            opaque_archive = archive_fixtures / "opaque-v4.bldproj"
            opaque_info = invoke("inspect", opaque_archive)
            assert opaque_info["file_sha256"] == hashlib.sha256(opaque_archive.read_bytes()).hexdigest()
            assert opaque_info["archive_role"] == "unknown"
            assert opaque_info["editable"] is False
            assert opaque_info["validation_complete"] is False
            assert opaque_info["recovery_summary"]["status"] == "opaque"
            assert opaque_info["recovery_summary"]["record_count"] == 3
            assert opaque_info["recovery_summary"]["diagnostic"]
            opaque_migration = root / "opaque-migrated.bldproj"
            opaque_extraction = root / "opaque-extracted"
            opaque_validation = invoke("validate", opaque_archive, success=False)
            assert opaque_validation.stdout == ""
            opaque_migration_result = invoke("migrate", opaque_archive, opaque_migration, success=False)
            opaque_extraction_result = invoke("extract", opaque_archive, opaque_extraction, success=False)
            assert opaque_migration_result.stdout == "" and opaque_extraction_result.stdout == ""
            assert not opaque_migration.exists() and not opaque_extraction.exists()

            corrupt_archive = archive_fixtures / "corrupt-v4.bldproj"
            corrupt_migration = root / "corrupt-migrated.bldproj"
            corrupt_extraction = root / "corrupt-extracted"
            for command, arguments in (
                    ("inspect", (corrupt_archive,)),
                    ("validate", (corrupt_archive,)),
                    ("migrate", (corrupt_archive, corrupt_migration)),
                    ("extract", (corrupt_archive, corrupt_extraction))):
                assert invoke(command, *arguments, success=False).stdout == ""
            assert not corrupt_migration.exists() and not corrupt_extraction.exists()

        # The resource command is the application-level registration seam for
        # templates/profiles/documentation restored from a local project
        # package. It must consume only the package manifest and verified bytes.
        resource_package = root / "resource-package"
        template_bytes = b'{"kind":"template","name":"Residential"}\n'
        profile_bytes = b'{"units":"imperial"}\n'
        documentation_bytes = b"Project format reference\n"
        (resource_package / "templates").mkdir(parents=True)
        (resource_package / "profiles").mkdir(parents=True)
        (resource_package / "documentation").mkdir(parents=True)
        (resource_package / "templates" / "residential.json").write_bytes(template_bytes)
        (resource_package / "profiles" / "imperial.json").write_bytes(profile_bytes)
        (resource_package / "documentation" / "project-format.md").write_bytes(documentation_bytes)
        def resource_record(kind, name, relative, payload):
            return {"kind": kind, "name": name, "path": relative,
                    "sha256": hashlib.sha256(payload).hexdigest(), "size": len(payload)}
        resources = [
            resource_record("template", "Residential", "templates/residential.json", template_bytes),
            resource_record("profile", "Imperial", "profiles/imperial.json", profile_bytes),
            resource_record("documentation", "Project format", "documentation/project-format.md", documentation_bytes),
        ]
        files = [{"kind": entry["kind"], "path": entry["path"],
                  "sha256": entry["sha256"], "size": entry["size"]}
                 for entry in resources]
        (resource_package / "project-package-manifest.json").write_text(
            json.dumps({"schema_version": 1, "manifest_version": 1,
                        "manifest_kind": "project-package", "audit_status": "incomplete",
                        "offline_qualified": False, "resources": resources, "files": files,
                        "summary": {"file_count": len(files) + 1, "asset_count": 0,
                                    "template_count": 1, "profile_count": 1,
                                    "documentation_count": 1}},
                       indent=2) + "\n", encoding="utf-8")
        registered = invoke("resources", resource_package)
        assert registered["resource_count"] == 3
        assert registered["network_required"] is False
        assert [entry["name"] for entry in registered["resources"]] == [
            "Project format", "Imperial", "Residential"]

        def constraint_fixture(path, *, version=1, slope=0):
            # Independently construct a format-v1 fixture with a valid logical
            # digest. A semantic rejection must not merely be a checksum error.
            info = invoke("new", path)
            entities = [
                {"id": "lock-a", "type": "constraint", "required": False,
                 "properties": {
                     "version": version, "relation": "horizontal", "wall_ids": ["wall-a"],
                     "bindings": [
                         {"owner_id": "wall-a", "feature": "baseline", "role": "start"},
                         {"owner_id": "wall-a", "feature": "baseline", "role": "end"}],
                     "future_metadata": {"preserve": [1, "opaque", True]}},
                 "extensions": {}},
                {"id": "wall-a", "type": "wall", "required": False,
                 "properties": {"baseline": {"start": [0, 0], "end": [4, slope],
                                               "sweep_radians": 0},
                                "thickness_m": 0.2, "height_m": 3, "elevation_m": 0},
                 "extensions": {}}]
            record = {"revision": 0, "parent_revision": None, "source_revision": None,
                      "action": "create", "name": None, "undo_stack": [], "redo_stack": [],
                      "entities": entities, "assets": []}
            manifest = {"format_version": 1, "document_id": info["document_id"],
                        "head_revision": 0, "saved_revision": 0,
                        "history": [record], "named_revisions": {}}
            canonical = json.dumps(manifest, sort_keys=True, ensure_ascii=False,
                                   separators=(",", ":")).encode("utf-8")
            with closing(sqlite3.connect(path)) as database:
                for entity in entities:
                    database.execute(
                        "INSERT INTO revision_entities VALUES(?,?,?,?,?,?)",
                        (0, entity["id"], entity["type"], int(entity["required"]),
                         json.dumps(entity["properties"]), json.dumps(entity["extensions"])))
                database.execute("UPDATE metadata SET value=? WHERE key='logical_digest'",
                                 (hashlib.sha256(canonical).hexdigest(),))
                database.commit()
            return entities

        constrained = root / "constrained.bldproj"
        constraint_fixture(constrained)
        assert invoke("inspect", constrained)["editable"] is True
        assert invoke("validate", constrained)["storage_integrity"] == "valid"
        invalid_constraint = root / "violated-lock.bldproj"
        constraint_fixture(invalid_constraint, slope=1)
        failure = invoke("validate", invalid_constraint, success=False)
        assert "constraint" in failure.stderr.lower(), failure.stderr
        assert "relation" in failure.stderr.lower(), failure.stderr
        future_constraint = root / "future-lock.bldproj"
        future_entities = constraint_fixture(future_constraint, version=99)
        future_info = invoke("inspect", future_constraint)
        assert future_info["editable"] is False
        assert "constraint" in future_info["read_only_reason"].lower()
        future_extract = root / "future-extracted"
        invoke("extract", future_constraint, future_extract)
        future_json = json.loads((future_extract / "project.json").read_text(encoding="utf-8"))
        assert future_json["revisions"][0]["entities"] == future_entities
    print("CLI Unicode, roundtrip, extraction, corruption and constraint integrity tests passed")


if __name__ == "__main__":
    main()
