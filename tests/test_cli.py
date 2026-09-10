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
    with tempfile.TemporaryDirectory(prefix="property-cli-") as directory:
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
        invoke("extract", project, extracted, success=False)
        corrupt = root / "broken.bldproj"
        corrupt.write_bytes(b"not a database")
        invoke("validate", corrupt, success=False)

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
