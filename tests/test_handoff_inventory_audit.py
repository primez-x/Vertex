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
    def fixture(self, omit=None, include_shipped_audit=False):
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
        fixture_module.stage.stage_bundle(inventory, allowlist, source_manifest, root, root / "out", "bundle")
        return root / "out/bundle"

    def rewrite(self, bundle, relative, value):
        fixture_module.write_json(bundle / relative, value)
        manifest_path = bundle / "offline-bundle-manifest.json"
        manifest = json.loads(manifest_path.read_text())
        for record in manifest["files"]:
            if record["path"] == relative:
                record.update(sha256=fixture_module.digest(bundle / relative), size=(bundle / relative).stat().st_size)
        for key in ("source_kit", "runtime_manifest"):
            if manifest[key]["path"] == relative:
                manifest[key]["sha256"] = fixture_module.digest(bundle / relative)
        fixture_module.write_json(manifest_path, manifest)

    def test_complete_bundle_is_deterministic_and_unqualified(self):
        bundle = self.fixture()
        report = audit.audit_bundle(bundle)
        self.assertTrue(report["passed"], report)
        self.assertEqual(report, audit.audit_bundle(bundle))
        self.assertEqual(report["audit_status"], "incomplete")
        self.assertFalse(any(report["qualification"].values()))
        self.assertEqual(len(report["artifacts"]), 10)

    def test_shipped_audit_does_not_mutate_bundle_with_bytecode(self):
        bundle = self.fixture(include_shipped_audit=True)

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
