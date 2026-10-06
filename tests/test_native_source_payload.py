"""Native-source report contributions must survive composition and frozen replay."""
import copy
import hashlib
import importlib.util
import json
from pathlib import Path
import tempfile
import unittest
from unittest import mock

ROOT = Path(__file__).resolve().parents[1]


def load_module(name, path):
    spec = importlib.util.spec_from_file_location(name, path)
    assert spec and spec.loader
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


composer = load_module("native_source_payload_composer", ROOT / "scripts/qualification/compose_dependency_source_kit.py")
stage = load_module("native_source_payload_stager", ROOT / "scripts/stage_offline_bundle.py")


class NativeSourcePayloadTests(unittest.TestCase):
    owner = "cad-shapely"
    evidence_kind = "geos_recipe"

    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.output = self.root / "payload"

        self.write("receipts/components.json", b"{}\n")
        self.write("receipts/runtime.json", b"{}\n")
        self.write("candidate/shapely.libs/geos.dll", b"native library bytes")
        self.binary = self.record("candidate/shapely.libs/geos.dll")
        _, _, parent_filename, parent_sha256 = composer.native.PARENT_PROFILES[self.evidence_kind]
        self.inventory = {"schema_version": 1, "binaries": [{
            "component_id": self.owner, "path": self.binary["path"],
            "destination": "bin/cad/shapely.libs/geos.dll", "sha256": self.binary["sha256"],
            "bytes": self.binary["bytes"]}],
            "components": [{"id": self.owner, "package": {
                "name": "shapely", "version": "2.1.2", "license": "BSD-3-Clause",
                "source": {"kind": "locked-archive", "archive_path": "cache/" + parent_filename,
                           "archive_filename": parent_filename, "archive_sha256": parent_sha256,
                           "licensing_clearance": False, "source_closure_qualified": False,
                           "archive_members": {self.binary["path"]: "shapely.libs/geos.dll"}}}}]}
        self.write_json("receipts/inventory.json", self.inventory)
        self.inventory_receipt = self.record("receipts/inventory.json")

        self.source = self.record(self.write("inputs/geos-source.tar.gz", b"tiny source receipt payload"))
        self.notice = self.record(self.write("inputs/geos-license.txt", b"Original license bytes\r\n"))
        self.recipe_file = self.record(self.write("inputs/geos-recipe.cmake", b"set(GEOS_VERSION 3.13.1)\n"))
        self.artifact = self.record(self.write("inputs/geos-build-metadata.json", b'{"revision":"fixture"}\n'))
        self.profile_data = {"inputs": {"source": self.source, "notice": self.notice,
                                         "recipe": self.recipe_file, "artifact": self.artifact}}
        self.input_receipt = self.record(self.write_json("native/geos-profile.json", self.profile_data))
        self.consumed = {row["path"]: row for row in
                         (self.input_receipt, self.source, self.notice, self.recipe_file, self.artifact)}

        self.contribution = self.make_contribution([self.target()])
        self.descriptor = composer.native.descriptor(
            self.evidence_kind, self.input_receipt, self.contribution, self.consumed)
        self.report = self.make_report()

    def write(self, name, data):
        path = self.root / name
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(data)
        return name

    def write_json(self, name, value):
        return self.write(name, (json.dumps(value, sort_keys=True) + "\n").encode("utf-8"))

    def record(self, name):
        path = self.root / name
        data = path.read_bytes()
        return {"path": name, "sha256": hashlib.sha256(data).hexdigest(), "bytes": len(data)}

    @staticmethod
    def source_identity(row):
        return row.get("inventory_path", row.get("source_path", row.get("path")))

    def target(self):
        return {"component_id": self.owner, "path": self.binary["path"],
                "destination": "bin/cad/shapely.libs/geos.dll", "sha256": self.binary["sha256"],
                "bytes": self.binary["bytes"]}

    def target_bindings(self, targets):
        return [{"target": dict(target), "member": "shapely.libs/geos.dll",
                 "byte_identity_verified": False} for target in targets]

    def make_contribution(self, targets):
        return {
            "sources": [{"url": "https://example.invalid/geos-source.tar.gz",
                         "status": "exact_local_source_present", "local_files": [self.source]}],
            "notices": [{**self.notice, "content_role": "notice_text_present_review_required"}],
            "artifacts": [self.artifact],
            "recipe": {"status": "exact_local_recipe_present", "files": [self.recipe_file],
                       "recipe_options": ["-DGEOS_BUILD_TESTING=OFF"],
                       "options_role": "observed recipe; build remains unqualified"},
            "remaining": ["native_source_derivation_not_qualified"],
            "dependency_dispositions": [{"component_id": self.owner,
                                          "relevance": "unresolved_candidate_exclusion",
                                          "candidate_exclusion_qualified": False}],
            "target_bindings": self.target_bindings(targets),
        }

    def make_report(self):
        component_manifest = self.record("receipts/components.json")
        runtime = self.record("receipts/runtime.json")
        return {
            "schema_version": 1, "audit_kind": "candidate_dependency_source_closure",
            "licensing_clearance": False, "corresponding_source_qualified": False,
            "boundary": "synthetic source availability fixture", "summary": {},
            "candidate_binding": {
                "inventory": self.inventory_receipt, "component_manifest": component_manifest,
                "runtime": runtime, "binaries": [{"component_id": self.owner, **self.binary}],
                "build_receipts": [], "upstream_metadata": None, "offline_bundle": None},
            "components": [{
                "id": self.owner,
                "package": {"name": "shapely", "version": "2.1.2", "license": "BSD-3-Clause"},
                "declared_source_kind": "locked-archive", "source_status": "exact_local_source_present",
                "sources": copy.deepcopy(self.contribution["sources"]),
                "notices": copy.deepcopy(self.contribution["notices"]),
                "assets": [], "artifacts": copy.deepcopy(self.contribution["artifacts"]),
                "recipe": copy.deepcopy(self.contribution["recipe"]),
                "remaining": list(self.contribution["remaining"]),
                "licensing_clearance": False, "corresponding_source_qualified": False,
                "native_source_inputs": copy.deepcopy(self.descriptor)}]}

    def normalize(self, kind, owner, input_receipt, reader, selected_targets):
        if kind != self.evidence_kind or owner != self.owner:
            raise ValueError("unexpected synthetic native profile")
        profile = reader.json(input_receipt, "native_input")
        for name, role in (("source", "source"), ("notice", "notice"),
                           ("recipe", "recipe"), ("artifact", "metadata")):
            reader.record(profile["inputs"][name], role)
        return self.make_contribution(selected_targets)

    def run_compose(self, report=None, output="payload"):
        self.write_json("report.json", self.report if report is None else report)
        with mock.patch.object(composer.native, "normalize", side_effect=self.normalize):
            return composer.compose(self.root, "report.json", output)

    def run_replay(self):
        with mock.patch.object(composer.native, "normalize", side_effect=self.normalize):
            return stage._validate_dependency_payload(
                self.output, {"files": []}, lambda identity: self.root / identity, helper=composer)

    def test_composed_native_inputs_are_copied_and_replay_without_workspace_sources(self):
        manifest = self.run_compose()
        component = manifest["components"][0]
        descriptor = component["native_source_inputs"]
        self.assertFalse(manifest["licensing_clearance"])
        self.assertFalse(manifest["corresponding_source_qualified"])
        self.assertFalse(manifest["offline_rebuild_qualified"])
        self.assertEqual(component["native_source_inputs"]["target_bindings"],
                         self.target_bindings([self.target()]))

        for consumed in descriptor["consumed_inputs"]:
            original_path = self.source_identity(consumed)
            row = next(row for row in manifest["files"] if row["source_path"] == original_path)
            self.assertIn("cad-shapely", row["component_ids"])
            self.assertIn("native_consumed", row["roles"])
            self.assertEqual((self.output / row["path"]).read_bytes(),
                             (self.root / original_path).read_bytes())

        unavailable = {self.source_identity(row) for row in descriptor["consumed_inputs"]}
        unavailable.update({"receipts/inventory.json", "receipts/components.json",
                            "receipts/runtime.json", "candidate/shapely.libs/geos.dll", "report.json"})
        for name in unavailable:
            path = self.root / name
            if path.exists():
                path.unlink()
        self.assertTrue(all(not (self.root / name).exists() for name in unavailable))

        helper, replayed, _ = self.run_replay()
        self.assertIs(helper, composer)
        self.assertEqual(replayed, manifest)

    def test_composition_refuses_missing_derived_obligations_and_altered_receipts(self):
        cases = (
            ("source", lambda item: item["sources"].clear(), "native contribution missing or altered sources"),
            ("notice", lambda item: item["notices"].clear(), "native contribution missing or altered notices"),
            ("remaining", lambda item: item["remaining"].clear(), "native unresolved obligation missing"),
            ("consumed-receipt", lambda item: item["native_source_inputs"]["consumed_inputs"][1].update(
                sha256="0" * 64), "native consumed input is missing or changed"),
            ("extra-owner", lambda item: item["native_source_inputs"]["dependency_dispositions"].append(
                {"component_id": "cad-other", "relevance": "unresolved_candidate_exclusion",
                 "candidate_exclusion_qualified": False}), "native source descriptor differs"),
        )
        for name, mutate, error in cases:
            with self.subTest(case=name):
                report = copy.deepcopy(self.report)
                mutate(report["components"][0])
                output = "refused-" + name
                with self.assertRaisesRegex(ValueError, error):
                    self.run_compose(report, output)
                self.assertFalse((self.root / output).exists())

    def test_composition_binds_rehashed_inventory_destination_and_target_hash(self):
        for change in ("destination", "target-hash"):
            with self.subTest(change=change):
                report = copy.deepcopy(self.report)
                inventory = copy.deepcopy(self.inventory)
                binary = inventory["binaries"][0]
                if change == "destination":
                    binary["destination"] = "bin/cad/other-geos.dll"
                else:
                    self.write("candidate/shapely.libs/geos.dll", b"substituted native library")
                    self.binary = self.record("candidate/shapely.libs/geos.dll")
                    binary["sha256"] = self.binary["sha256"]
                    binary["bytes"] = self.binary["bytes"]
                    report["candidate_binding"]["binaries"] = [
                        {"component_id": self.owner, **self.binary}]
                self.write_json("receipts/inventory.json", inventory)
                report["candidate_binding"]["inventory"] = self.record("receipts/inventory.json")
                output = "refused-target-" + change
                expected_error = ("native parent selected archive member mapping differs"
                                  if change == "destination" else "native source descriptor differs")
                with self.assertRaisesRegex(ValueError, expected_error):
                    self.run_compose(report, output)
                self.assertFalse((self.root / output).exists())
                self.binary = self.record("candidate/shapely.libs/geos.dll")
                self.write_json("receipts/inventory.json", self.inventory)

    def test_composition_rejects_rehashed_native_parent_relabels(self):
        mutations = (
            ("package-name", lambda component: component["package"].update(name="other")),
            ("package-version", lambda component: component["package"].update(version="2.1.3")),
            ("source-kind", lambda component: component["package"]["source"].update(kind="wheel")),
            ("parent-hash", lambda component: component["package"]["source"].update(archive_sha256="0" * 64)),
            ("archive-member", lambda component: component["package"]["source"]["archive_members"].update(
                {self.binary["path"]: "other/geos.dll"})),
            ("shortened-archive-member", lambda component: component["package"]["source"]["archive_members"].update(
                {self.binary["path"]: "geos.dll"})),
        )
        for name, mutate in mutations:
            with self.subTest(change=name):
                inventory = copy.deepcopy(self.inventory)
                mutate(inventory["components"][0])
                self.write_json("receipts/inventory.json", inventory)
                report = copy.deepcopy(self.report)
                # The inventory receipt is deliberately rehashed; the binary tuple is unchanged.
                report["candidate_binding"]["inventory"] = self.record("receipts/inventory.json")
                output = "refused-parent-" + name
                with self.assertRaisesRegex(ValueError, "native parent"):
                    self.run_compose(report, output)
                self.assertFalse((self.root / output).exists())
                self.write_json("receipts/inventory.json", self.inventory)

    def test_frozen_replay_refuses_rehashed_missing_contribution_receipts_and_extra_owner(self):
        manifest = self.run_compose()
        manifest_path = self.output / composer.MANIFEST
        original = manifest_path.read_bytes()
        cases = (
            ("source", lambda document: document["components"][0]["sources"][0]["local_files"].clear()),
            ("notice", lambda document: document["components"][0]["notices"].clear()),
            ("remaining", lambda document: document["components"][0]["remaining"].clear()),
            ("consumed-receipt", lambda document: document["components"][0]["native_source_inputs"]
             ["consumed_inputs"][1].update(sha256="0" * 64)),
            ("extra-owner", lambda document: next(row for row in document["files"]
             if row["component_ids"])["component_ids"].append("cad-other")),
        )
        for name, mutate in cases:
            with self.subTest(case=name):
                manifest_path.write_bytes(original)
                document = json.loads(original)
                mutate(document)
                if name == "extra-owner":
                    next(row for row in document["files"] if "cad-other" in row["component_ids"])["component_ids"].sort()
                manifest_path.write_text(json.dumps(document, sort_keys=True, indent=2) + "\n",
                                         encoding="utf-8")
                with self.assertRaises(ValueError):
                    self.run_replay()


if __name__ == "__main__":
    unittest.main()
