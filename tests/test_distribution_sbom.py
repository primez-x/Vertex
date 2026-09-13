from __future__ import annotations

import copy
import hashlib
import importlib.util
import json
import pathlib
import tempfile
import unittest


ROOT = pathlib.Path(__file__).resolve().parents[1]
SPEC = importlib.util.spec_from_file_location(
    "distribution_sbom", ROOT / "scripts" / "distribution_sbom.py"
)
assert SPEC is not None and SPEC.loader is not None
sbom = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(sbom)


def digest(value: bytes) -> str:
    return hashlib.sha256(value).hexdigest()


class DistributionSbomTests(unittest.TestCase):
    def inventory(self) -> dict:
        app = b"application"
        dependency = b"dependency"
        notice = b"notice"
        source = b"int main() {}\n"
        return {
            "schema_version": 1,
            "inventory_version": 1,
            "audit_status": "incomplete",
            "generated_utc": "2026-09-13T12:00:00Z",
            "components": [
                {
                    "id": "property-studio",
                    "kind": "application",
                    "distribution_status": "included",
                    "package": {
                        "name": "Property Studio",
                        "version": "workspace",
                        "license": "Proprietary",
                        "source": {"kind": "workspace", "homepage": "https://property-studio.invalid"},
                    },
                    "notices": [{"path": "LICENSE", "sha256": digest(notice)}],
                },
                {
                    "id": "dependency",
                    "kind": "runtime",
                    "distribution_status": "included",
                    "package": {
                        "name": "Dependency",
                        "version": "2.0",
                        "license": "MIT",
                        "source": {"kind": "vcpkg", "url": "https://example.invalid/dependency"},
                    },
                    "notices": [{"path": "LICENSE", "sha256": digest(notice)}],
                    "source_inputs": [{"path": "third_party/dependency.h", "sha256": digest(source)}],
                },
            ],
            "binaries": [
                {"name": "property-studio.exe", "path": "build/property-studio.exe",
                 "destination": "bin/property-studio.exe", "sha256": digest(app),
                 "component_id": "property-studio"},
                {"name": "dependency.dll", "path": "build/dependency.dll",
                 "destination": "bin/dependency.dll", "sha256": digest(dependency),
                 "component_id": "dependency"},
            ],
            "static_inputs": [
                {"component_id": "dependency", "kind": "static-source",
                 "package": {"name": "Dependency", "version": "2.0", "license": "MIT"},
                 "source_inputs": [{"path": "third_party/dependency.h", "sha256": digest(source)}]},
            ],
            "runtime_imports": [
                {"from": "property-studio.exe", "to": "dependency.dll",
                 "name": "dependency.dll", "kind": "local-component",
                 "component_id": "dependency"},
            ],
            "system_runtime_imports": ["KERNEL32.dll"],
            "windows_api_contracts": ["api-ms-win-core.dll"],
        }

    def test_builds_valid_spdx_with_deduplicated_files_and_dependency(self):
        inventory = self.inventory()
        document = sbom.build_sbom(inventory, "a" * 64)
        sbom.validate_sbom(document)
        self.assertEqual(document["spdxVersion"], "SPDX-2.3")
        self.assertIn("/" + "a" * 64, document["documentNamespace"])
        self.assertEqual(len(document["packages"]), 2)
        # LICENSE is shared by both packages and appears once in the file set.
        self.assertEqual(len(document["files"]), 4)
        package_ids = {row["name"]: row["SPDXID"] for row in document["packages"]}
        self.assertTrue(any(
            relation["relationshipType"] == "DEPENDS_ON" and
            relation["spdxElementId"] == package_ids["Property Studio"] and
            relation["relatedSpdxElement"] == package_ids["Dependency"]
            for relation in document["relationships"]
        ))
        license_file = next(row for row in document["files"] if row["fileName"] == "LICENSE")
        self.assertEqual(license_file["checksums"][0]["algorithm"], "SHA256")

    def test_output_is_deterministic_when_inventory_component_order_changes(self):
        first = sbom.build_sbom(self.inventory(), "b" * 64)
        altered = self.inventory()
        altered["components"] = list(reversed(altered["components"]))
        altered["binaries"] = list(reversed(altered["binaries"]))
        self.assertEqual(first, sbom.build_sbom(altered, "b" * 64))

    def test_invalid_paths_and_hashes_fail_closed(self):
        for mutate in (
            lambda value: value["binaries"][0].update({"destination": "../escape.exe"}),
            lambda value: value["binaries"][0].update({"sha256": "bad"}),
            lambda value: value["components"].append(copy.deepcopy(value["components"][0])),
        ):
            inventory = self.inventory()
            mutate(inventory)
            with self.assertRaises(sbom.SbomError):
                sbom.build_sbom(inventory, "c" * 64)

    def test_cli_writes_structural_spdx_document(self):
        with tempfile.TemporaryDirectory() as directory:
            root = pathlib.Path(directory)
            inventory_path = root / "inventory.json"
            output_path = root / "nested" / "distribution-sbom.spdx.json"
            inventory_path.write_text(json.dumps(self.inventory(), sort_keys=True), encoding="utf-8")
            self.assertEqual(sbom.main(["--inventory", str(inventory_path), "--output", str(output_path)]), 0)
            document = json.loads(output_path.read_text(encoding="utf-8"))
            sbom.validate_sbom(document)
            self.assertEqual(document["creationInfo"]["created"], "2026-09-13T12:00:00Z")


if __name__ == "__main__":
    unittest.main()
