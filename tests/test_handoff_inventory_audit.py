from __future__ import annotations

import importlib.util
import json
import os
import pathlib
import shutil
import subprocess
import sys
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[1]


def load(name, path):
    spec = importlib.util.spec_from_file_location(name, path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


fixture_module = load("bundle_fixture", ROOT / "tests/test_stage_offline_bundle.py")
audit = load("handoff_inventory_audit", ROOT / "scripts/handoff_inventory_audit.py")


class HandoffInventoryTests(unittest.TestCase):
    def fixture(self, omit=None, include_shipped_audit=False, dependency_source_kit=False,
                frozen_binding=False):
        dependency = None
        if dependency_source_kit:
            dependency = fixture_module.DependencySourceKitTests()
            dependency.setUp()
            self.addCleanup(dependency.doCleanups)
            root, inventory, source_manifest, allowlist = (
                dependency.root, dependency.inventory, dependency.source_kit, dependency.allowlist)
        else:
            fixture = fixture_module.StageOfflineBundleTests().fixture()
            self.addCleanup(fixture[0].cleanup)
            _, root, inventory, source_manifest, allowlist, *_ = fixture
        entries = [
            ("source", "src/main.cpp"), ("licenses", "LICENSE"),
            ("source", "third_party/sqlite3.c"),
            ("build", "CMakeLists.txt"), ("docs", "docs/user-guide.html"),
            ("docs", "docs/desktop-workflow.md"), ("docs", "docs/project-format.md"),
            ("docs", "docs/integration-adapters.md"),
            ("docs", "docs/requirements/acceptance-evidence.json"),
            ("fixtures", "tests/fixtures/example.json"),
        ]
        if include_shipped_audit:
            for relative in (
                "packaging/handoff-inventory-contract.json",
                "scripts/distribution_sbom.py",
                "scripts/handoff_inventory_audit.py",
                "scripts/source_kit_manifest.py",
                "scripts/stage_offline_bundle.py",
                "scripts/stage_portable_package.py",
                "scripts/stage_ifc_sdk_sources.py",
                "scripts/qualification/dependency_source_closure.py",
                "scripts/qualification/compose_dependency_source_kit.py",
            ):
                path = root / relative
                path.parent.mkdir(parents=True, exist_ok=True)
                shutil.copy2(ROOT / relative, path)
                entries.append(("source", relative))
        selected = []
        for category, relative in entries:
            if category == omit or relative == omit:
                continue
            path = root / relative
            if not path.exists():
                path.parent.mkdir(parents=True, exist_ok=True)
                path.write_text("fixture evidence\n", encoding="utf-8")
            selected.append({"category": category, "path": relative})
        fixture_module.stage._SOURCE_KIT.build_manifest(root, {"entries": selected}, source_manifest)
        payload = None
        if dependency is not None:
            binding = dependency.report["candidate_binding"]
            inventory_document = json.loads(inventory.read_text())
            for component in dependency.report["components"]:
                original = next(row for row in inventory_document["components"] if row["id"] == component["id"])
                component["assets"] = [dependency.receipt(row["path"])
                                       for row in original.get("source_inputs", [])]
            if frozen_binding:
                base = fixture_module.stage.stage_bundle(
                    inventory, allowlist, source_manifest, root, root / "out", "complete-base")
                binding["offline_bundle"] = {
                    key: dependency.receipt("out/complete-base/" + relative) for key, relative in (
                        ("manifest", "offline-bundle-manifest.json"),
                        ("portable_manifest", "metadata/portable-package-manifest.json"),
                        ("runtime_manifest", "runtime-manifest.json"),
                        ("source_inventory", "metadata/distribution-inventory.json"),
                        ("source_kit", "metadata/source-kit-manifest.json"))}
                binding["offline_bundle"].update(
                    payload=[dependency.receipt("out/complete-base/" + row["path"]) for row in base["files"]],
                    source_location="out/complete-base/source-kit")
                project_path = "out/complete-base/source-kit/src/main.cpp"
            else:
                binding["offline_bundle"] = None
                project_path = "src/main.cpp"
            vertex = next(row for row in dependency.report["components"] if row["id"] == "vertex")
            vertex["sources"][0]["local_files"] = [dependency.receipt(project_path)]
            fixture_module.write_json(root / "artifacts/complete-report.json", dependency.report)
            payload = "artifacts/complete-dependency-kit"
            dependency.composer.compose(
                root, "artifacts/complete-report.json", payload,
                source_kit_manifest=None if frozen_binding else source_manifest.relative_to(root).as_posix())
        fixture_module.stage.stage_bundle(inventory, allowlist, source_manifest, root, root / "out", "bundle",
                                          dependency_source_kit=payload)
        return root / "out/bundle"

    def rewrite(self, bundle, relative, value):
        fixture_module.write_json(bundle / relative, value)
        manifest_path = bundle / "offline-bundle-manifest.json"
        manifest = json.loads(manifest_path.read_text())
        for record in manifest["files"]:
            if record["path"] == relative:
                record.update(sha256=fixture_module.digest(bundle / relative), size=(bundle / relative).stat().st_size)
        for key in ("source_kit", "runtime_manifest", "dependency_source_kit"):
            if key not in manifest:
                continue
            if manifest[key]["path"] == relative:
                manifest[key]["sha256"] = fixture_module.digest(bundle / relative)
                if "size" in manifest[key]:
                    manifest[key]["size"] = (bundle / relative).stat().st_size
        fixture_module.write_json(manifest_path, manifest)

    def test_complete_dependency_attachment_is_accepted_and_unqualified(self):
        for frozen in (False, True):
            with self.subTest(frozen=frozen):
                bundle = self.fixture(dependency_source_kit=True, frozen_binding=frozen)
                report = audit.audit_bundle(bundle)
                self.assertTrue(report["passed"], report)
                self.assertFalse(any(report["qualification"].values()))
                self.assertEqual(report["audit_status"], "incomplete")

    def change_bundle_manifest(self, bundle, mutate):
        path = bundle / "offline-bundle-manifest.json"
        document = json.loads(path.read_text())
        mutate(document)
        fixture_module.write_json(path, document)

    def test_dependency_attachment_requires_exact_reference(self):
        for mode in ("missing", "hash", "path"):
            with self.subTest(mode=mode):
                bundle = self.fixture(dependency_source_kit=True)
                def mutate(document):
                    if mode == "missing":
                        del document["dependency_source_kit"]
                    elif mode == "hash":
                        document["dependency_source_kit"]["sha256"] = "0" * 64
                    else:
                        document["dependency_source_kit"]["path"] = "metadata/source-kit-manifest.json"
                self.change_bundle_manifest(bundle, mutate)
                self.assertFalse(audit.audit_bundle(bundle)["passed"])

    def test_dependency_members_require_source_roles_and_no_installation(self):
        for field, replacement in (("role", "metadata"), ("category", "source"),
                                   ("kind", "source-kit"), ("install", True)):
            with self.subTest(field=field):
                bundle = self.fixture(dependency_source_kit=True)
                def mutate(document):
                    member = next(row for row in document["files"]
                                  if row["path"].endswith("inputs/assets/fonts/Inter.ttf"))
                    member[field] = replacement
                self.change_bundle_manifest(bundle, mutate)
                self.assertFalse(audit.audit_bundle(bundle)["passed"])

    def test_declared_rogue_project_or_dependency_file_is_refused(self):
        for relative in ("source-kit/rogue.txt", "source-kit/third_party/dependency-inputs/rogue.txt",
                         "source-kit/third_party/forged-dependency-inputs/rogue.txt"):
            with self.subTest(relative=relative):
                bundle = self.fixture(dependency_source_kit=True)
                target = bundle / relative
                target.parent.mkdir(parents=True, exist_ok=True)
                target.write_bytes(b"rogue")
                def mutate(document):
                    document["files"].append(fixture_module.stage._file_record(
                        target, relative, kind="dependency-source" if "dependency-inputs" in relative else "source-kit",
                        role="source-kit", category="dependency-source" if "dependency-inputs" in relative else "source",
                        install=False))
                    document["files"] = fixture_module.stage._sorted_file_records(document["files"])
                self.change_bundle_manifest(bundle, mutate)
                self.assertFalse(audit.audit_bundle(bundle)["passed"])

    def test_rehashed_dependency_table_cannot_forge_members_or_ownership(self):
        for mode in ("hash", "missing", "identity", "owner", "role"):
            with self.subTest(mode=mode):
                bundle = self.fixture(dependency_source_kit=True)
                relative = fixture_module.stage.DEPENDENCY_PREFIX + "/dependency-source-kit-manifest.json"
                kit = json.loads((bundle / relative).read_text())
                row = next(row for row in kit["files"] if row["path"] == "inputs/assets/fonts/Inter.ttf")
                if mode == "hash":
                    row["sha256"] = "0" * 64
                elif mode == "missing":
                    kit["files"].remove(row)
                elif mode == "identity":
                    row["source_path"] = "forged/Inter.ttf"
                elif mode == "owner":
                    row["component_ids"] = []
                else:
                    row["roles"] = ["recipe"]
                self.rewrite(bundle, relative, kit)
                self.assertFalse(audit.audit_bundle(bundle)["passed"])

    def test_rehashed_dependency_binding_must_match_candidate(self):
        for mode in ("direct-project", "frozen-project", "inventory-bytes", "binary"):
            with self.subTest(mode=mode):
                bundle = self.fixture(dependency_source_kit=True, frozen_binding=mode == "frozen-project")
                relative = fixture_module.stage.DEPENDENCY_PREFIX + "/dependency-source-kit-manifest.json"
                kit = json.loads((bundle / relative).read_text())
                binding = kit["candidate_binding"]
                if mode == "direct-project":
                    binding["source_kit"]["sha256"] = "0" * 64
                elif mode == "frozen-project":
                    binding["offline_bundle"]["source_kit"]["sha256"] = "0" * 64
                elif mode == "inventory-bytes":
                    binding["inventory"]["bytes"] += 1
                else:
                    binding["binaries"][0]["sha256"] = "0" * 64
                self.rewrite(bundle, relative, kit)
                self.assertFalse(audit.audit_bundle(bundle)["passed"])

    def test_frozen_attachment_replays_every_receipt_and_exact_use(self):
        for mode in ("orphan-recipe", "recipe-owner", "recipe-role", "recipe-hash", "archive-bytes"):
            with self.subTest(mode=mode):
                bundle = self.fixture(dependency_source_kit=True)
                relative = fixture_module.stage.DEPENDENCY_PREFIX + "/dependency-source-kit-manifest.json"
                kit = json.loads((bundle / relative).read_text())
                sqlite = next(row for row in kit["components"] if row["id"] == "sqlite")
                recipe = next(row for row in kit["files"] if row["path"] == "inputs/recipe/portfile.cmake")
                if mode == "orphan-recipe":
                    sqlite["recipe"] = None
                    recipe["component_ids"] = []
                elif mode == "recipe-owner":
                    recipe["component_ids"] = ["vertex"]
                elif mode == "recipe-role":
                    recipe["roles"] = ["assets"]
                elif mode == "recipe-hash":
                    sqlite["recipe"]["files"][0]["sha256"] = "0" * 64
                else:
                    sqlite["sources"][0]["local_files"][0]["bytes"] += 1
                self.rewrite(bundle, relative, kit)
                self.assertFalse(audit.audit_bundle(bundle)["passed"], mode)
                manifest = json.loads((bundle / "offline-bundle-manifest.json").read_text())
                closure = fixture_module.stage._load_sibling(
                    "closure_receipt_regression", "qualification/dependency_source_closure.py")
                selected, selected_receipt = closure.json_input(bundle, manifest["source_inventory"]["path"])
                source_receipt = closure.file_record(bundle, manifest["source_kit"]["path"])
                with self.assertRaises(ValueError):
                    closure.dependency_bundle_sources(bundle.parent, bundle.name, manifest,
                        {entry["path"]: entry for entry in manifest["files"]}, selected,
                        selected_receipt, source_receipt, {})

    def test_frozen_attachment_recomputes_inventory_tree_hash(self):
        # A rehashed child and file table cannot replace the inventory-bound tree.
        fixture = fixture_module.DependencySourceKitTests()
        fixture.setUp()
        self.addCleanup(fixture.doCleanups)
        tree = fixture.root / "sdk/include"
        tree.mkdir(parents=True)
        (tree / "unit.hpp").write_bytes(b"original header")
        inventory = json.loads(fixture.inventory.read_text())
        receipt = {"path": "sdk/include", "kind": "directory",
                   "sha256": fixture.composer.closure.source_tree_hash(fixture.root, "sdk/include")}
        sqlite_inventory = next(row for row in inventory["components"] if row["id"] == "sqlite")
        sqlite_inventory["source_inputs"] = [receipt]
        fixture_module.write_json(fixture.inventory, inventory)
        binding = fixture.report["candidate_binding"]
        binding["inventory"] = fixture.receipt(fixture.inventory.relative_to(fixture.root).as_posix())
        binding["offline_bundle"] = None
        vertex = next(row for row in fixture.report["components"] if row["id"] == "vertex")
        vertex["sources"][0]["local_files"] = [fixture.receipt("src/main.cpp")]
        next(row for row in fixture.report["components"] if row["id"] == "sqlite")["assets"] = [receipt]
        for component in fixture.report["components"]:
            original = next(row for row in inventory["components"] if row["id"] == component["id"])
            if component["id"] != "sqlite":
                component["assets"] = [fixture.receipt(row["path"])
                                       for row in original.get("source_inputs", [])]
        fixture_module.write_json(fixture.root / "artifacts/tree-report.json", fixture.report)
        fixture.composer.compose(fixture.root, "artifacts/tree-report.json", "artifacts/tree-kit",
                                 source_kit_manifest=fixture.source_kit.relative_to(fixture.root).as_posix())
        fixture_module.stage.stage_bundle(fixture.inventory, fixture.allowlist, fixture.source_kit,
                                         fixture.root, fixture.root / "out", "tree-bundle",
                                         dependency_source_kit="artifacts/tree-kit")
        bundle = fixture.root / "out/tree-bundle"
        relative = fixture_module.stage.DEPENDENCY_PREFIX + "/dependency-source-kit-manifest.json"
        kit = json.loads((bundle / relative).read_text())
        row = next(row for row in kit["files"] if row["source_path"] == "sdk/include/unit.hpp")
        child = fixture_module.stage.DEPENDENCY_PREFIX + "/" + row["path"]
        (bundle / child).write_bytes(b"substituted header")
        row.update(sha256=fixture_module.digest(bundle / child), bytes=(bundle / child).stat().st_size)
        self.change_bundle_manifest(bundle, lambda document: next(
            entry for entry in document["files"] if entry["path"] == child).update(
                sha256=row["sha256"], size=row["bytes"]))
        self.rewrite(bundle, relative, kit)
        manifest = json.loads((bundle / "offline-bundle-manifest.json").read_text())
        closure = fixture.composer.closure
        selected, selected_receipt = closure.json_input(bundle, manifest["source_inventory"]["path"])
        source_receipt = closure.file_record(bundle, manifest["source_kit"]["path"])
        with self.assertRaisesRegex(ValueError, "tree hash"):
            closure.dependency_bundle_sources(bundle.parent, bundle.name, manifest,
                {entry["path"]: entry for entry in manifest["files"]}, selected,
                selected_receipt, source_receipt, {})
        with self.assertRaisesRegex(ValueError, "tree hash"):
            audit._dependency_members(bundle, manifest,
                                      {entry["path"]: entry for entry in manifest["files"]})

    def test_complete_bundle_is_deterministic_and_unqualified(self):
        bundle = self.fixture()
        report = audit.audit_bundle(bundle)
        self.assertTrue(report["passed"], report)
        self.assertEqual(report, audit.audit_bundle(bundle))
        self.assertEqual(report["audit_status"], "incomplete")
        self.assertFalse(any(report["qualification"].values()))
        self.assertEqual(len(report["artifacts"]), 10)

    def test_shipped_audit_does_not_mutate_bundle_with_bytecode(self):
        bundle = self.fixture(include_shipped_audit=True, dependency_source_kit=True)

        def inventory():
            return sorted(
                path.relative_to(bundle).as_posix()
                for path in bundle.rglob("*")
                if path.is_file()
            )

        before = inventory()
        environment = os.environ.copy()
        environment.pop("PYTHONDONTWRITEBYTECODE", None)
        result = subprocess.run(
            [
                sys.executable,
                str(bundle / "source-kit/scripts/handoff_inventory_audit.py"),
                "--bundle-root",
                str(bundle),
            ],
            capture_output=True,
            text=True,
            env=environment,
        )

        self.assertEqual(result.returncode, 0, result.stderr or result.stdout)
        self.assertTrue(json.loads(result.stdout)["passed"])
        self.assertEqual(inventory(), before)
        self.assertFalse(any("__pycache__" in path for path in inventory()))

    def test_missing_required_source_category(self):
        report = audit.audit_bundle(self.fixture(omit="build"))
        self.assertFalse(report["passed"])
        self.assertIn("build", str(report["errors"]))

    def test_missing_artifact_with_docs_category_present(self):
        report = audit.audit_bundle(self.fixture(omit="docs/project-format.md"))
        self.assertFalse(report["passed"])
        self.assertIn("project_format", str(report["errors"]))

    def test_tampered_and_missing_file(self):
        for remove in (False, True):
            with self.subTest(remove=remove):
                bundle = self.fixture()
                path = bundle / "source-kit/src/main.cpp"
                if remove:
                    path.unlink()
                else:
                    path.write_bytes(b"tampered")
                self.assertFalse(audit.audit_bundle(bundle)["passed"])

    def test_rehashed_source_manifest_still_must_match_bundle(self):
        bundle = self.fixture()
        relative = "metadata/source-kit-manifest.json"
        value = json.loads((bundle / relative).read_text())
        value["files"][0]["sha256"] = "0" * 64
        self.rewrite(bundle, relative, value)
        self.assertFalse(audit.audit_bundle(bundle)["passed"])

    def test_rehashed_runtime_manifest_still_must_match_bundle(self):
        bundle = self.fixture()
        relative = "runtime-manifest.json"
        value = json.loads((bundle / relative).read_text())
        value["files"][0]["size"] += 1
        self.rewrite(bundle, relative, value)
        self.assertFalse(audit.audit_bundle(bundle)["passed"])

    def test_unsafe_source_path_and_unknown_category(self):
        for field, replacement in (("path", "../escape"), ("category", "unknown")):
            with self.subTest(field=field):
                bundle = self.fixture()
                relative = "metadata/source-kit-manifest.json"
                value = json.loads((bundle / relative).read_text())
                value["files"][0][field] = replacement
                self.rewrite(bundle, relative, value)
                self.assertFalse(audit.audit_bundle(bundle)["passed"])

    def test_cli_json_and_failure_exit_status(self):
        bundle = self.fixture(omit="build")
        result = subprocess.run([sys.executable, str(ROOT / "scripts/handoff_inventory_audit.py"),
                                 "--bundle-root", str(bundle)], capture_output=True, text=True)
        self.assertEqual(result.returncode, 1, result.stderr)
        self.assertFalse(json.loads(result.stdout)["passed"])

    def test_bundle_stale_size_and_unlisted_file(self):
        for mode in ("size", "unlisted"):
            with self.subTest(mode=mode):
                bundle = self.fixture()
                if mode == "unlisted":
                    (bundle / "extra.txt").write_text("unlisted")
                else:
                    path = bundle / "offline-bundle-manifest.json"
                    value = json.loads(path.read_text())
                    value["files"][0]["size"] += 1
                    fixture_module.write_json(path, value)
                self.assertFalse(audit.audit_bundle(bundle)["passed"])

    def test_missing_reference_and_duplicate_json_key(self):
        for mode in ("reference", "duplicate"):
            with self.subTest(mode=mode):
                bundle = self.fixture()
                path = bundle / "offline-bundle-manifest.json"
                if mode == "duplicate":
                    path.write_text('{"schema_version":1,"schema_version":1}')
                else:
                    value = json.loads(path.read_text())
                    del value["source_kit"]
                    fixture_module.write_json(path, value)
                self.assertFalse(audit.audit_bundle(bundle)["passed"])

    def test_source_category_disagrees_with_bundle(self):
        bundle = self.fixture()
        relative = "metadata/source-kit-manifest.json"
        value = json.loads((bundle / relative).read_text())
        entry = next(row for row in value["files"] if row["path"] == "src/main.cpp")
        entry["category"] = "docs"
        value["category_counts"]["source"] -= 1
        value["category_counts"]["docs"] += 1
        value["summary"]["category_counts"] = value["category_counts"]
        self.rewrite(bundle, relative, value)
        report = audit.audit_bundle(bundle)
        self.assertFalse(report["passed"])
        self.assertIn("category mismatch", str(report["errors"]))

    def test_runtime_manifest_cannot_replace_bundle_manifest(self):
        self.assertFalse(audit.audit_bundle(self.fixture(), "runtime-manifest.json")["passed"])


if __name__ == "__main__":
    unittest.main()
