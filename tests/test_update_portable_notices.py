import importlib.util
from pathlib import Path
import unittest

SPEC = importlib.util.spec_from_file_location(
    "update_portable_notices", Path(__file__).resolve().parents[1] / "scripts/update_portable_notices.py")
MODULE = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(MODULE)


class PortableNoticesTests(unittest.TestCase):
    def catalog(self):
        return {"components": [{"id": name, "notice_paths": [".deps/qt-notices/6.11.2/NOTICE.txt"]}
                               for name in ("qtbase", "qtpdf", "qtsvg")]}

    def test_each_owner_is_covered_without_destination_collisions(self):
        retained = {"kind": "notice", "inventory_entry": "vertex",
                    "path": "LICENSE", "destination": "licenses/Vertex-LICENSE"}
        result = MODULE.refresh(self.catalog(), {"schema_version": 1, "entries": [retained]})
        self.assertNotIn(retained, result["entries"])
        self.assertEqual({row["inventory_entry"] for row in result["entries"]},
                         {"qtbase", "qtpdf", "qtsvg"})
        self.assertEqual(len({row["destination"].casefold() for row in result["entries"]}), 3)
        self.assertEqual(MODULE.refresh(self.catalog(), result), result)

    def test_native_notice_coverage_preserves_valid_destinations_and_assets(self):
        catalog = self.catalog()
        catalog["components"].append({"id": "native", "notice_paths": [
            "third_party/notices/native/lib/archive.txt", ".deps/native/share/native/copyright"]})
        retained = {"kind": "notice", "inventory_entry": "native",
                    "path": ".deps/native/share/native/copyright", "destination": "licenses/custom-copyright"}
        asset = {"kind": "asset", "inventory_entry": "vertex", "path": "asset.ttf", "destination": "assets/asset.ttf"}
        result = MODULE.refresh(catalog, {"entries": [retained, asset]})
        self.assertEqual(result["entries"][:2], [retained, asset])
        self.assertIn(("native", "third_party/notices/native/lib/archive.txt"),
                      {(row["inventory_entry"], row["path"]) for row in result["entries"]})

    def test_missing_owner_is_rejected(self):
        catalog = self.catalog()
        catalog["components"].pop()
        with self.assertRaises(ValueError):
            MODULE.refresh(catalog, {"entries": []})

    def test_unreviewed_mapping_is_rejected(self):
        catalog = self.catalog()
        catalog["components"][0]["notice_paths"] = [".deps/qt/../private.txt"]
        with self.assertRaises(ValueError):
            MODULE.refresh(catalog, {"entries": []})


if __name__ == "__main__":
    unittest.main()
