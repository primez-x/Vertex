import importlib.util
from pathlib import Path
import unittest

SPEC = importlib.util.spec_from_file_location(
    "update_portable_qt_notices", Path(__file__).resolve().parents[1] / "scripts/update_portable_qt_notices.py")
MODULE = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(MODULE)


class PortableQtNoticesTests(unittest.TestCase):
    def catalog(self):
        return {"components": [{"id": name, "notice_paths": [".deps/qt-notices/6.11.2/NOTICE.txt"]}
                               for name in ("qtbase", "qtpdf", "qtsvg")]}

    def test_each_owner_is_covered_without_destination_collisions(self):
        retained = {"kind": "notice", "inventory_entry": "vertex",
                    "path": "LICENSE", "destination": "licenses/Vertex-LICENSE"}
        result = MODULE.refresh(self.catalog(), {"schema_version": 1, "entries": [retained]})
        self.assertEqual(result["entries"][0], retained)
        self.assertEqual({row["inventory_entry"] for row in result["entries"][1:]},
                         {"qtbase", "qtpdf", "qtsvg"})
        self.assertEqual(len({row["destination"].casefold() for row in result["entries"]}), 4)
        self.assertEqual(MODULE.refresh(self.catalog(), result), result)

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
