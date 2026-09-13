"""Contract tests for the offline symbol-catalog release manifest."""

import copy
import importlib.util
import pathlib
import unittest


SCRIPT = pathlib.Path(__file__).resolve().parents[1] / "scripts/validate_symbol_catalog.py"
SPEC = importlib.util.spec_from_file_location("symbol_catalog_validation", SCRIPT)
validation = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(validation)


class SymbolCatalogManifestTests(unittest.TestCase):
    def make_manifest(self):
        categories = list(validation.REQUIRED_CATEGORIES)
        families = [
            ("toilet", "fixtures"),
            ("double-bed", "furniture"),
            ("sofa", "furniture"),
            ("checkout-counter", "commercial"),
        ] + [(f"family-{index:02d}", categories[index % len(categories)])
             for index in range(21)]
        entries = []
        for family, category in families:
            for variant in range(8):
                identifier = f"{family}-w{variant // 4 + 1}-d{variant % 4 + 1}"
                entries.append({
                    "id": identifier,
                    "family": family,
                    "category": category,
                    "width_metres": 1.0,
                    "depth_metres": 1.0,
                    "anchor": {"x": 0.0, "y": 0.0},
                    "minimum_scale": 0.01,
                    "maximum_scale": 100.0,
                    "preview": [
                        {"start": {"x": -0.5, "y": -0.5},
                         "end": {"x": 0.5, "y": -0.5}},
                        {"start": {"x": 0.5, "y": -0.5},
                         "end": {"x": 0.5, "y": 0.5}},
                        {"start": {"x": 0.5, "y": 0.5},
                         "end": {"x": -0.5, "y": 0.5}},
                        {"start": {"x": -0.5, "y": 0.5},
                         "end": {"x": -0.5, "y": -0.5}},
                    ],
                })
        entries.sort(key=lambda item: item["id"])
        category_counts = {}
        family_counts = {}
        for entry in entries:
            category_counts[entry["category"]] = category_counts.get(entry["category"], 0) + 1
            family_counts.setdefault(entry["family"], {"id": entry["family"],
                                                          "category": entry["category"],
                                                          "variant_count": 0})
            family_counts[entry["family"]]["variant_count"] += 1
        return {
            "schema_version": 1,
            "catalog_id": "vertex.symbol-catalog",
            "entry_count": len(entries),
            "family_count": len(family_counts),
            "category_counts": category_counts,
            "families": sorted(family_counts.values(), key=lambda item: item["id"]),
            "entries": entries,
            "query": "",
            "category": "",
            "catalog_entry_count": len(entries),
            "filtered_entry_count": len(entries),
        }

    def test_valid_manifest_covers_threshold_categories_and_representatives(self):
        report = validation.validate_manifest(self.make_manifest())
        self.assertTrue(report["valid"], report["errors"])
        self.assertEqual(report["entry_count"], 200)

    def test_missing_category_and_representative_are_rejected(self):
        manifest = self.make_manifest()
        manifest["category_counts"].pop("commercial")
        report = validation.validate_manifest(manifest)
        self.assertFalse(report["valid"])
        self.assertTrue(any("category_counts" in error for error in report["errors"]))

        manifest = self.make_manifest()
        manifest["entries"] = [entry for entry in manifest["entries"]
                               if entry["family"] != "toilet"]
        manifest["entry_count"] = len(manifest["entries"])
        manifest["catalog_entry_count"] = len(manifest["entries"])
        manifest["filtered_entry_count"] = len(manifest["entries"])
        report = validation.validate_manifest(manifest)
        self.assertFalse(report["valid"])
        self.assertTrue(any("representative family" in error for error in report["errors"]))

    def test_duplicate_unsorted_and_out_of_bounds_entries_are_rejected(self):
        manifest = self.make_manifest()
        manifest["entries"][1] = copy.deepcopy(manifest["entries"][0])
        report = validation.validate_manifest(manifest)
        self.assertFalse(report["valid"])
        self.assertTrue(any("duplicate" in error for error in report["errors"]))

        manifest = self.make_manifest()
        manifest["entries"][0], manifest["entries"][1] = manifest["entries"][1], manifest["entries"][0]
        report = validation.validate_manifest(manifest)
        self.assertFalse(report["valid"])
        self.assertTrue(any("sorted" in error for error in report["errors"]))

        manifest = self.make_manifest()
        manifest["entries"][0]["preview"][0]["start"]["x"] = 0.51
        report = validation.validate_manifest(manifest)
        self.assertFalse(report["valid"])
        self.assertTrue(any("footprint" in error for error in report["errors"]))


if __name__ == "__main__":
    unittest.main()
